/* SPDX-License-Identifier: MIT */
#include "mtl_fragment_shading_rate.h"
#include "mtl_compiler.h"
#include "mtl_bridge.h"
#include "mtl_format.h"
#include "mtl_vrs_mesh_types.h"
#include "mtl_vrs_mesh_prepare.h"
#include "kosmickrisp/libkk/kk_argument_table.h"
#import <Metal/Metal.h>
#include <stdlib.h>

struct mtl_vrs_pipelines {
   id<MTLRenderPipelineState> coverage, coarse, replay, order, prepare;
   uint32_t result_stride, raster_samples;
   bool uses_per_draw, early_fragment_tests, statistics;
};

static const char support_msl[] =
   KK_ARGUMENT_TABLE_MSL_PROLOG
   "#include <metal_stdlib>\nusing namespace metal;\n"
   "typedef uint uint32_t; typedef ulong uint64_t; typedef int int32_t;\n"
   KK_VRS_MESH_TYPES_MSL
   KK_VRS_MSL_LAYOUT
   "kernel void kk_vrs_order(){threadgroup_barrier(mem_flags::mem_device); }\n"
   "struct V { float4 position [[position]]; };\n"
   "struct O { float4 color [[color(0)]]; uint mask [[sample_mask]]; };\n"
   KK_VRS_MESH_PREPARE_MSL
   "inline void kk_vrs_capture_pixel(float4 position, uint mask,\n"
   " constant kk_vrs_context &ctx) {\n"
   " uint2 xy=uint2(position.xy);\n"
   " if(simd_is_helper_thread() || !mask || xy.x>=ctx.width || xy.y>=ctx.height)return;\n"
   " device kk_vrs_coverage *coverage=(device kk_vrs_coverage*)ctx.coverage;\n"
   " uint p=xy.y*ctx.width+xy.x;\n"
   " atomic_fetch_or_explicit((device atomic_uint*)&coverage[p].mask,mask,memory_order_relaxed);\n"
   " coverage[p].z=position.z; coverage[p].reciprocal_w=position.w;\n"
   "}\n"
   "fragment void kk_vrs_capture(V in [[stage_in]], uint mask [[sample_mask]],\n"
   " constant kk_vrs_context &ctx [[buffer(KK_ARGUMENT_PER_DRAW)]]) { kk_vrs_capture_pixel(in.position,mask,ctx); }\n"
   "[[early_fragment_tests]] fragment void kk_vrs_capture_early(V in [[stage_in]],\n"
   " uint mask [[sample_mask, post_depth_coverage]],\n"
   " constant kk_vrs_context &ctx [[buffer(KK_ARGUMENT_PER_DRAW)]]) { kk_vrs_capture_pixel(in.position,mask,ctx); }\n"
   "fragment O kk_vrs_replay(V in [[stage_in]],\n"
   " constant kk_vrs_context &ctx [[buffer(KK_ARGUMENT_PER_DRAW)]]) {\n"
   " uint2 xy=uint2(in.position.xy);\n"
   " if(simd_is_helper_thread() || xy.x>=ctx.width || xy.y>=ctx.height)return {float4(0),0};\n"
   " device const kk_vrs_result *results=(device const kk_vrs_result*)ctx.results;\n"
   " device const uint *map=(device const uint*)ctx.pixel_results;\n"
   " kk_vrs_result r=results[map[xy.y*ctx.width+xy.x]];\n"
   " uint w=1u<<((r.rate>>2)&3u), h=1u<<(r.rate&3u);\n"
   " uint p=(xy.y%h)*w+xy.x%w;\n"
   " uint bit=w*h-1-p;\n"
   " return {float4(r.color[0],r.color[1],r.color[2],r.color[3]),(r.mask>>bit)&1u};\n"
   "}\n";

static MTL4LibraryFunctionDescriptor *
function(id<MTLLibrary> library, const char *name)
{
   MTL4LibraryFunctionDescriptor *f = [[[MTL4LibraryFunctionDescriptor alloc] init] autorelease];
   f.library = library;
   f.name = [NSString stringWithUTF8String:name];
   return f;
}

mtl_library *
mtl_vrs_new_support_library(mtl_compiler *compiler,
   enum mtl_language_version version, enum mtl_math_mode math,
   enum mtl_math_floating_point_functions functions)
{
   return mtl_new_library(compiler, support_msl, version, math, functions);
}

static struct mtl_vrs_pipelines *
new_function_pipelines(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *native_pipeline,
   MTL4FunctionDescriptor *application, MTL4FunctionDescriptor *replay,
   bool native_application, bool replay_framebuffer_reads,
   enum mtl_language_version version, enum mtl_math_mode math,
   enum mtl_math_floating_point_functions functions, uint32_t result_stride, bool uses_per_draw, bool early_fragment_tests,
   mtl_library *support_library, bool statistics)
{
   @autoreleasepool {
      if (statistics) {
         mtl_function_descriptor *specialized = mtl_specialize_pipeline_statistics(
            (mtl_function_descriptor *)application);
         if (!specialized) return NULL;
         application = [(MTL4FunctionDescriptor *)specialized autorelease];
      }
      NSError *error = nil;
      id<MTLLibrary> support = support_library
         ? [(id<MTLLibrary>)support_library retain]
         : (id<MTLLibrary>)mtl_vrs_new_support_library(compiler, version, math, functions);
      if (!support) return NULL;
      struct mtl_vrs_pipelines *p = calloc(1, sizeof(*p));
      if (!p) { [support release]; return NULL; }
      p->result_stride = result_stride;
      p->uses_per_draw = uses_per_draw;
      p->early_fragment_tests = early_fragment_tests;
      p->statistics = statistics;
      MTL4RenderPipelineDescriptor *original = (MTL4RenderPipelineDescriptor *)native_pipeline;
      p->raster_samples = original.rasterSampleCount;
      MTL4RenderPipelineDescriptor *r = [[original copy] autorelease];
      r.fragmentFunctionDescriptor = function(support,
         early_fragment_tests ? "kk_vrs_capture_early" : "kk_vrs_capture");
      for (unsigned i = 0; i < 8; i++)
         r.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
      p->coverage = (id<MTLRenderPipelineState>)mtl_new_render_pipeline(compiler, (void *)r);
      if (p->coverage) {
         MTL4RenderPipelineDescriptor *fine = replay ? [original copy] : [r copy];
         fine.fragmentFunctionDescriptor = replay ? replay : function(support, "kk_vrs_replay");
         if (!replay) fine.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
         enum mtl_pixel_format formats[8];
         for (unsigned i = 0; i < 8; i++)
            formats[i] = (enum mtl_pixel_format)fine.colorAttachments[i].pixelFormat;
         p->replay = (id<MTLRenderPipelineState>)mtl_new_render_pipeline_for_attachments(
            compiler, (void *)fine, formats, 8, 0, fine.rasterSampleCount,
            replay_framebuffer_reads);
         [fine release];
      }
      if (p->replay && native_application) {
         MTL4RenderPipelineDescriptor *body = [original copy];
         body.fragmentFunctionDescriptor = application;
         for (unsigned i = 0; i < 8; i++)
            body.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
         p->coarse = (id<MTLRenderPipelineState>)mtl_new_render_pipeline(compiler, (void *)body);
         [body release];
         MTL4TileRenderPipelineDescriptor *order = [[[MTL4TileRenderPipelineDescriptor alloc] init] autorelease];
         order.tileFunctionDescriptor = function(support, "kk_vrs_order");
         for (unsigned i = 0; i < 8; i++)
            order.colorAttachments[i].pixelFormat = original.colorAttachments[i].pixelFormat;
         order.rasterSampleCount = original.rasterSampleCount;
         p->order = (id<MTLRenderPipelineState>)mtl_new_render_pipeline(compiler, (void *)order);
         MTL4RenderPipelineDescriptor *prepare = [[[MTL4RenderPipelineDescriptor alloc] init] autorelease];
         prepare.vertexFunctionDescriptor = function(support, "kk_vrs_prepare_mesh_job");
         prepare.rasterizationEnabled = NO;
         prepare.rasterSampleCount = original.rasterSampleCount;
         for (unsigned i = 0; i < 8; i++) {
            prepare.colorAttachments[i].pixelFormat = original.colorAttachments[i].pixelFormat;
            prepare.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
         }
         p->prepare = (id<MTLRenderPipelineState>)mtl_new_render_pipeline(compiler, (void *)prepare);
      }
      if (p->replay && !native_application) {
         MTL4TileRenderPipelineDescriptor *t = [[[MTL4TileRenderPipelineDescriptor alloc] init] autorelease];
         t.tileFunctionDescriptor = application;
         t.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
         t.rasterSampleCount = 1;
         t.threadgroupSizeMatchesTileSize = YES;
         p->coarse = (id<MTLRenderPipelineState>)mtl_new_render_pipeline(compiler, (void *)t);
      }
      [support release];
      if (!p->coverage || !p->replay || !p->coarse || (native_application && (!p->order || !p->prepare))) {
         fprintf(stderr,"VRS pipeline: %s\n",error.localizedDescription.UTF8String);
         mtl_vrs_destroy_pipelines(p);
         return NULL;
      }
      return p;
   }
}

static struct mtl_vrs_pipelines *
new_pipelines(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *native_pipeline, const char *coarse_msl,
   const char *coarse_entrypoint, bool native_application,
   const char *replay_msl, const char *replay_entry, bool replay_framebuffer_reads,
   enum mtl_language_version version, enum mtl_math_mode math,
   enum mtl_math_floating_point_functions functions)
{
   mtl_library *application = mtl_new_library(compiler, coarse_msl, version, math, functions);
   if (!application) return NULL;
   mtl_library *replay = replay_msl
      ? mtl_new_library(compiler, replay_msl, version, math, functions) : NULL;
   if (replay_msl && !replay) { mtl_release(application); return NULL; }
   struct mtl_vrs_pipelines *result;
   @autoreleasepool {
      result = new_function_pipelines(compiler, native_pipeline,
         function((id<MTLLibrary>)application, coarse_entrypoint),
         replay ? function((id<MTLLibrary>)replay, replay_entry) : nil,
         native_application, replay_framebuffer_reads, version, math, functions, sizeof(struct kk_vrs_result), false, false, NULL, false);
   }
   mtl_release(application);
   mtl_release(replay);
   return result;
}

struct mtl_vrs_pipelines *
mtl_vrs_new_cached_native_pipelines_for_statistics(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *native_pipeline,
   mtl_function_descriptor *body, mtl_function_descriptor *replay,
   bool framebuffer_reads, enum mtl_language_version version,
   enum mtl_math_mode math, enum mtl_math_floating_point_functions functions,
   uint32_t result_stride, bool uses_per_draw, bool early_fragment_tests,
   bool statistics)
{
   if (!native_pipeline || !body || !replay) return NULL;
   return new_function_pipelines(compiler, native_pipeline,
      (MTL4FunctionDescriptor *)body, (MTL4FunctionDescriptor *)replay, true,
      framebuffer_reads, version, math, functions, result_stride, uses_per_draw,
      early_fragment_tests, NULL, statistics);
}

struct mtl_vrs_pipelines *
mtl_vrs_new_cached_native_pipelines(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *native_pipeline,
   mtl_function_descriptor *body, mtl_function_descriptor *replay,
   bool framebuffer_reads, enum mtl_language_version version,
   enum mtl_math_mode math, enum mtl_math_floating_point_functions functions,
   uint32_t result_stride, bool uses_per_draw, bool early_fragment_tests)
{
   return mtl_vrs_new_cached_native_pipelines_for_statistics(compiler,
      native_pipeline, body, replay, framebuffer_reads, version, math, functions,
      result_stride, uses_per_draw, early_fragment_tests, false);
}

struct mtl_vrs_pipelines *
mtl_vrs_new_shared_native_pipelines_for_statistics(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *native_pipeline,
   mtl_function_descriptor *body, mtl_function_descriptor *replay,
   bool framebuffer_reads, enum mtl_language_version version,
   enum mtl_math_mode math, enum mtl_math_floating_point_functions functions,
   uint32_t result_stride, bool uses_per_draw, bool early_fragment_tests,
   mtl_library *support_library, bool statistics)
{
   if (!native_pipeline || !body || !replay || !support_library) return NULL;
   return new_function_pipelines(compiler, native_pipeline,
      (MTL4FunctionDescriptor *)body, (MTL4FunctionDescriptor *)replay, true,
      framebuffer_reads, version, math, functions, result_stride, uses_per_draw,
      early_fragment_tests, support_library, statistics);
}

struct mtl_vrs_pipelines *
mtl_vrs_new_shared_native_pipelines(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *native_pipeline,
   mtl_function_descriptor *body, mtl_function_descriptor *replay,
   bool framebuffer_reads, enum mtl_language_version version,
   enum mtl_math_mode math, enum mtl_math_floating_point_functions functions,
   uint32_t result_stride, bool uses_per_draw, bool early_fragment_tests,
   mtl_library *support_library)
{
   return mtl_vrs_new_shared_native_pipelines_for_statistics(compiler,
      native_pipeline, body, replay, framebuffer_reads, version, math, functions,
      result_stride, uses_per_draw, early_fragment_tests, support_library, false);
}

bool
mtl_vrs_pipelines_statistics(const struct mtl_vrs_pipelines *pipelines)
{
   return pipelines && pipelines->statistics;
}

void
mtl_vrs_destroy_pipelines(struct mtl_vrs_pipelines *p)
{
   if (!p) return;
   [p->coverage release]; [p->coarse release]; [p->replay release]; [p->order release];
   [p->prepare release]; free(p);
}

bool
mtl_vrs_encode_2x2(mtl_render_encoder *encoder,
   const struct mtl_vrs_pipelines *p,
   mtl_argument_table *original_table, mtl_argument_table *coarse_table,
   const struct kk_vrs_context *ctx)
{
   if (!p || !encoder || !original_table || !coarse_table || !ctx ||
       !ctx->coverage || !ctx->results || !ctx->pixel_results ||
       (ctx->result_stride ? ctx->result_stride : sizeof(struct kk_vrs_result)) != p->result_stride ||
       !ctx->texel_width || !ctx->texel_height ||
       ctx->combiner0 > 4 || ctx->combiner1 > 4 ||
       ctx->width != 32 || ctx->height != 32 ||
       (ctx->fragment_width != 1 && ctx->fragment_width != 2 && ctx->fragment_width != 4) ||
       (ctx->fragment_height != 1 && ctx->fragment_height != 2 && ctx->fragment_height != 4) ||
       ctx->tile_width != 16 || ctx->tile_height != 16)
      return false;
   @autoreleasepool {
      id<MTL4RenderCommandEncoder> e = (id<MTL4RenderCommandEncoder>)encoder;
      [e setArgumentTable:(id<MTL4ArgumentTable>)original_table atStages:MTLRenderStageVertex];
      [e setArgumentTable:(id<MTL4ArgumentTable>)coarse_table atStages:MTLRenderStageFragment|MTLRenderStageTile];
      [e setRenderPipelineState:p->coverage];
      [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [e setRenderPipelineState:p->coarse];
      [e dispatchThreadsPerTile:MTLSizeMake(16,16,1)];
      [e setRenderPipelineState:p->replay];
      [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [e setArgumentTable:(id<MTL4ArgumentTable>)original_table atStages:MTLRenderStageVertex|MTLRenderStageFragment|MTLRenderStageTile];
      return true;
   }
}


struct mtl_vrs_pipelines *
mtl_vrs_new_pipelines(mtl_compiler *compiler, mtl_render_pipeline_descriptor *native,
   const char *msl, const char *entry)
{
   return new_pipelines(compiler, native, msl, entry, false, NULL, NULL, false,
      MTL_LANGUAGE_VERSION_4_0, MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
}

struct mtl_vrs_pipelines *
mtl_vrs_new_native_pipelines(mtl_compiler *compiler, mtl_render_pipeline_descriptor *native,
   const char *msl, const char *entry,
   const char *replay_msl, const char *replay_entry, bool replay_framebuffer_reads, enum mtl_language_version version,
   enum mtl_math_mode math, enum mtl_math_floating_point_functions functions)
{
   return new_pipelines(compiler, native, msl, entry, true,
                        replay_msl, replay_entry, replay_framebuffer_reads, version, math, functions);
}

bool
mtl_vrs_pipelines_early_tests(const struct mtl_vrs_pipelines *pipelines)
{
   return pipelines && pipelines->early_fragment_tests;
}

uint32_t
mtl_vrs_pipelines_samples(const struct mtl_vrs_pipelines *pipelines)
{
   return pipelines ? pipelines->raster_samples : 0;
}

mtl_render_pipeline_state *
mtl_vrs_phase_pipeline(const struct mtl_vrs_pipelines *p, enum mtl_vrs_phase phase)
{
   if (!p || (unsigned)phase > MTL_VRS_REPLAY) return NULL;
   return (void *)(phase == MTL_VRS_COVERAGE ? p->coverage :
      phase == MTL_VRS_BODY ? p->coarse : p->replay);
}

bool
mtl_vrs_order_native_phase(mtl_render_encoder *encoder, const struct mtl_vrs_pipelines *p,
   enum mtl_vrs_phase phase)
{
   if (!encoder || !p || (unsigned)phase > MTL_VRS_REPLAY) return false;
   if (phase != MTL_VRS_COVERAGE) {
      if (!p->order) return false;
      id<MTL4RenderCommandEncoder> e = (id<MTL4RenderCommandEncoder>)encoder;
      [e setRenderPipelineState:p->order];
      [e dispatchThreadsPerTile:MTLSizeMake(1,1,1)];
   }
   return true;
}

bool
mtl_vrs_encode_native_phase(mtl_render_encoder *encoder, const struct mtl_vrs_pipelines *p,
   mtl_argument_table *original_table, mtl_argument_table *coarse_table,
   const struct kk_vrs_context *ctx, enum mtl_vrs_phase phase)
{
   if (!encoder || !p || !original_table || !coarse_table || !ctx ||
       (unsigned)phase > MTL_VRS_REPLAY ||
       (p->raster_samples != 1 && p->raster_samples != 2 && p->raster_samples != 4) ||
       (ctx->reserved ? ctx->reserved : 1) != p->raster_samples ||
       ctx->width != 32 || ctx->height != 32 || !ctx->coverage ||
       !ctx->results || !ctx->pixel_results ||
       (p->uses_per_draw && !ctx->original_per_draw) ||
       (ctx->result_stride ? ctx->result_stride : sizeof(struct kk_vrs_result)) !=
          p->result_stride) return false;
   id<MTL4RenderCommandEncoder> e = (id<MTL4RenderCommandEncoder>)encoder;
   [e setArgumentTable:(id<MTL4ArgumentTable>)original_table atStages:MTLRenderStageVertex];
   [e setArgumentTable:(id<MTL4ArgumentTable>)coarse_table atStages:MTLRenderStageFragment];
   if (!mtl_vrs_order_native_phase(encoder, p, phase)) return false;
   [e setRenderPipelineState:(id<MTLRenderPipelineState>)mtl_vrs_phase_pipeline(p, phase)];
   [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
   if (phase == MTL_VRS_REPLAY)
      [e setArgumentTable:(id<MTL4ArgumentTable>)original_table atStages:MTLRenderStageVertex|MTLRenderStageFragment];
   return true;
}

bool
mtl_vrs_encode_native(mtl_render_encoder *encoder, const struct mtl_vrs_pipelines *p,
   mtl_argument_table *original_table, mtl_argument_table *coarse_table,
   const struct kk_vrs_context *ctx)
{
   return mtl_vrs_encode_native_phase(encoder, p, original_table, coarse_table, ctx, MTL_VRS_COVERAGE) &&
          mtl_vrs_encode_native_phase(encoder, p, original_table, coarse_table, ctx, MTL_VRS_BODY) &&
          mtl_vrs_encode_native_phase(encoder, p, original_table, coarse_table, ctx, MTL_VRS_REPLAY);
}

bool
mtl_vrs_prepare_mesh_job(mtl_render_encoder *encoder, const struct mtl_vrs_pipelines *p,
   mtl_argument_table *table, uint64_t parameters, uint64_t context, uint32_t pixels)
{
   if (!encoder || !p || !p->prepare || !table || !parameters || !context || !pixels || pixels > 1024)
      return false;
   /* This owned explicit intra-pass dependency orders device storage before
    * reuse. It does not replace or weaken ordinary driver barriers. */
   mtl_barrier_after_encoder_stages(encoder, MTL_STAGE_MESH | MTL_STAGE_FRAGMENT, MTL_STAGE_VERTEX);
   mtl_set_address(table, parameters, KK_ARGUMENT_PER_DRAW);
   id<MTL4RenderCommandEncoder> e = (id<MTL4RenderCommandEncoder>)encoder;
   [e setArgumentTable:(id<MTL4ArgumentTable>)table atStages:MTLRenderStageVertex];
   [e setRenderPipelineState:p->prepare];
   [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:pixels];
   mtl_barrier_after_encoder_stages(encoder, MTL_STAGE_VERTEX, MTL_STAGE_MESH | MTL_STAGE_FRAGMENT);
   mtl_set_address(table, context, KK_ARGUMENT_PER_DRAW);
   return true;
}

/* SPDX-License-Identifier: MIT */
#include "kk_fragment_shading_rate_command.h"
#include "kk_fragment_shading_rate_resources.h"
#include "kk_shader.h"
#include "kk_mesh_pipeline_api.h"
#include "kk_poly_retained_raster_types.h"
#include "kosmickrisp/libkk/kk_argument_table.h"
#include "kosmickrisp/bridge/mtl_bridge.h"

static bool
kk_vrs_snapshot_is_live(const struct kk_depth_stencil_snapshot *snapshot)
{
   return snapshot->command->metal.render == snapshot->encoder &&
          snapshot->command->state.gfx.shader == snapshot->producer;
}

typedef VkResult (*kk_vrs_raster_phase)(struct kk_cmd_buffer *,
   const struct mtl_vrs_pipelines *, const struct kk_vrs_context *,
   enum mtl_vrs_phase, void *);
typedef void (*kk_vrs_raster_restore)(mtl_render_encoder *, void *);

static VkResult
kk_vrs_cmd_encode_phases(struct kk_cmd_buffer *cmd,
   struct kk_shader *actual_producer, const struct mtl_vrs_pipelines *pipelines,
   const struct kk_vrs_context *context, kk_vrs_raster_phase draw,
   kk_vrs_raster_restore restore_raster, void *data)
{
   if (!cmd || !actual_producer || actual_producer != cmd->state.gfx.shader ||
       !cmd->metal.render || !pipelines || !context || !draw || !restore_raster)
      return VK_ERROR_UNKNOWN;
   const struct kk_rendering_state *render = &cmd->state.gfx.render;
   /* The general window/indirect scheduler supplies larger or layered draws.
    * Never truncate them into this bounded leaf or silently shade at 1x1. */
   uint32_t samples = cmd->vk.dynamic_graphics_state.ms.rasterization_samples;
   if ((samples != 1 && samples != 2 && samples != 4) ||
       (context->reserved ? context->reserved : 1) != samples ||
       mtl_vrs_pipelines_samples(pipelines) != samples ||
       context->width != 32 || context->height != 32 ||
       render->area.offset.x || render->area.offset.y ||
       render->area.extent.width != 32 || render->area.extent.height != 32 ||
       render->layer_count != 1 || render->view_mask)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   const struct kk_depth_stencil_snapshot *snapshot;
   VkResult result = kk_cmd_capture_depth_stencil(cmd, actual_producer, &snapshot);
   if (result != VK_SUCCESS)
      return result;
   /* Resolve the sole derived state before the first potentially committing
    * draw. The snapshot owns it through command retirement. */
   if (!kk_cmd_depth_stencil_variant(snapshot, KK_DEPTH_STENCIL_BYPASS))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   const enum mtl_visibility_result_mode visibility = cmd->state.gfx.occlusion.mode;
   const size_t visibility_offset = cmd->state.gfx.occlusion.index * sizeof(uint64_t);
   bool early = mtl_vrs_pipelines_early_tests(pipelines);
   for (enum mtl_vrs_phase phase = MTL_VRS_COVERAGE; phase <= MTL_VRS_REPLAY; phase++) {
      if (!kk_vrs_snapshot_is_live(snapshot)) {
         result = VK_ERROR_UNKNOWN;
         goto restore;
      }
      /* Early DS commits once during post-depth coverage, even if the body
       * later discards. Late DS commits once after application exports and
       * fine output lowering; a rejected sample cannot erase side effects. */
      bool commit = early ? phase == MTL_VRS_COVERAGE : phase == MTL_VRS_REPLAY;
      result = kk_cmd_bind_depth_stencil(snapshot,
         commit ? KK_DEPTH_STENCIL_ORIGINAL : KK_DEPTH_STENCIL_BYPASS);
      if (result != VK_SUCCESS)
         goto restore;
      /* Count passing samples at the same phase that commits DS. Early-test
       * queries precede application kill/A2C; late queries include the final
       * fine output mask. Internal body/other raster draws never contribute.
       * Leave the logical query state and query-free native path unchanged. */
      if (visibility != MTL_VISIBILITY_RESULT_MODE_DISABLED)
         mtl_set_visibility_result_mode(snapshot->encoder,
            commit ? visibility : MTL_VISIBILITY_RESULT_MODE_DISABLED, visibility_offset);
      result = draw(cmd, pipelines, context, phase, data);
      if (result != VK_SUCCESS)
         goto restore;
   }
restore:
   /* A retained but ended encoder is not valid storage for memoryless data.
    * Never bind to another encoder to conceal an invalid phase transition. */
   if (!kk_vrs_snapshot_is_live(snapshot)) {
      result = VK_ERROR_UNKNOWN;
   } else {
      VkResult restored = kk_cmd_bind_depth_stencil(snapshot, KK_DEPTH_STENCIL_ORIGINAL);
      restore_raster(snapshot->encoder, data);
      if (visibility != MTL_VISIBILITY_RESULT_MODE_DISABLED)
         mtl_set_visibility_result_mode(snapshot->encoder, visibility, visibility_offset);
      if (result == VK_SUCCESS)
         result = restored;
   }
   if (result != VK_SUCCESS)
      vk_command_buffer_set_error(&cmd->vk, result);
   return result;
}

struct kk_vrs_native_raster {
   mtl_render_pipeline_state *original_pipeline;
   mtl_argument_table *original, *coarse;
};

static VkResult
kk_vrs_native_phase(struct kk_cmd_buffer *cmd, const struct mtl_vrs_pipelines *p,
   const struct kk_vrs_context *context, enum mtl_vrs_phase phase, void *data)
{
   struct kk_vrs_native_raster *raster = data;
   return mtl_vrs_encode_native_phase(cmd->metal.render, p, raster->original,
      raster->coarse, context, phase) ? VK_SUCCESS : VK_ERROR_UNKNOWN;
}

static void
kk_vrs_restore_native(mtl_render_encoder *encoder, void *data)
{
   struct kk_vrs_native_raster *raster = data;
   mtl_render_set_pipeline_state(encoder, raster->original_pipeline);
   mtl_render_set_argument_table(encoder, raster->original,
      MTL_RENDER_STAGE_VERTEX | MTL_RENDER_STAGE_FRAGMENT |
      MTL_RENDER_STAGE_OBJECT | MTL_RENDER_STAGE_MESH);
}

VkResult
kk_vrs_cmd_encode_native(struct kk_cmd_buffer *cmd,
   struct kk_shader *actual_producer, const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_table, mtl_argument_table *coarse_table,
   const struct kk_vrs_context *context)
{
   if (!cmd || !actual_producer || actual_producer != cmd->state.gfx.shader ||
       !cmd->metal.render || !pipelines || !context || !original_table || !coarse_table ||
       original_table == coarse_table)
      return VK_ERROR_UNKNOWN;
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_vrs_native_raster raster = {
      .original = original_table, .coarse = coarse_table,
      .original_pipeline = kk_attachment_pipeline(cmd, actual_producer,
         gfx->conservative ? 2 : (gfx->polygon_points ||
            (gfx->layer_replay.bind && actual_producer->info.poly_layer_replay))),
   };
   if (!raster.original_pipeline)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   return kk_vrs_cmd_encode_phases(cmd, actual_producer, pipelines, context,
      kk_vrs_native_phase, kk_vrs_restore_native, &raster);
}

struct kk_vrs_mesh_raster {
   const struct kk_mesh_retained_batch *batch;
   uint64_t parameters;
   mtl_argument_table *original_fragment, *coarse;
};

static VkResult
kk_vrs_mesh_phase(struct kk_cmd_buffer *cmd, const struct mtl_vrs_pipelines *p,
   const struct kk_vrs_context *context, enum mtl_vrs_phase phase, void *data)
{
   struct kk_vrs_mesh_raster *raster = data;
   if (!mtl_vrs_order_native_phase(raster->batch->encoder, p, phase))
      return VK_ERROR_UNKNOWN;
   return kk_mesh_retained_raster_replay_job(cmd, raster->batch,
      raster->parameters, mtl_vrs_phase_pipeline(p, phase), raster->coarse);
}

static void
kk_vrs_restore_mesh(mtl_render_encoder *encoder, void *data)
{
   struct kk_vrs_mesh_raster *raster = data;
   mtl_render_set_pipeline_state(encoder, raster->batch->raster_pipeline);
   mtl_render_set_argument_table(encoder, raster->batch->mesh_table, MTL_RENDER_STAGE_MESH);
   mtl_render_set_argument_table(encoder, raster->original_fragment, MTL_RENDER_STAGE_FRAGMENT);
}

static VkResult
kk_vrs_cmd_encode_mesh_parameters(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, uint64_t parameters,
   const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_fragment, mtl_argument_table *coarse_fragment,
   const struct kk_vrs_context *context)
{
   struct kk_vrs_mesh_raster raster = {
      .batch = batch, .parameters = parameters,
      .original_fragment = original_fragment, .coarse = coarse_fragment,
   };
   /* Only the separate Fragment table changes; Mesh owns slot5 and its exact
    * original reader grid/guard. Alias filtering uses its existing named slot. */
   mtl_set_address(coarse_fragment, batch->layer_selector, KK_ARGUMENT_POLY_LAYER_REPLAY);
   return kk_vrs_cmd_encode_phases(cmd, (struct kk_shader *)batch->producer,
      pipelines, context, kk_vrs_mesh_phase, kk_vrs_restore_mesh, &raster);
}

static bool
kk_vrs_mesh_job_arguments_valid(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch,
   mtl_argument_table *original_fragment, mtl_argument_table *coarse_fragment,
   const struct kk_vrs_context *context)
{
   return cmd && batch && context &&
      batch->producer == cmd->state.gfx.shader && batch->encoder == cmd->metal.render &&
      original_fragment && coarse_fragment && original_fragment != coarse_fragment &&
      coarse_fragment != batch->mesh_table &&
      context->original_per_draw == batch->per_draw_address;
}

VkResult
kk_vrs_cmd_encode_mesh_job(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, uint64_t job,
   const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_fragment, mtl_argument_table *coarse_fragment,
   const struct kk_vrs_context *context)
{
   if (!job || job % 4 || !pipelines ||
       !kk_vrs_mesh_job_arguments_valid(cmd, batch, original_fragment, coarse_fragment, context))
      return VK_ERROR_UNKNOWN;
   uint64_t parameters;
   VkResult result = kk_mesh_retained_raster_job_parameters(cmd, batch, job, &parameters);
   if (result != VK_SUCCESS)
      return result;
   return kk_vrs_cmd_encode_mesh_parameters(cmd, batch, parameters, pipelines,
      original_fragment, coarse_fragment, context);
}

VkResult
kk_vrs_cmd_encode_next_mesh_job(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, const struct kk_vrs_mesh_jobs *jobs,
   const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_fragment, mtl_argument_table *coarse_fragment,
   const struct kk_vrs_prepared *resources)
{
   if (!jobs || jobs->batch != batch || !jobs->gpu || !jobs->reader_parameters ||
       !resources || !pipelines ||
       !kk_vrs_mesh_job_arguments_valid(cmd, batch, original_fragment,
          coarse_fragment, resources->context))
      return VK_ERROR_UNKNOWN;
   VkResult result = kk_vrs_cmd_prepare_next_mesh_job(cmd, batch, jobs,
      pipelines, coarse_fragment, resources);
   if (result != VK_SUCCESS)
      return result;
   return kk_vrs_cmd_encode_mesh_parameters(cmd, batch, jobs->reader_parameters,
      pipelines, original_fragment, coarse_fragment, resources->context);
}

struct kk_vrs_poly_raster {
   const struct kk_poly_retained_raster *raster;
   uint64_t indirect_draw;
   mtl_render_pipeline_state *original_pipeline;
   mtl_argument_table *original_fragment, *coarse;
};

static VkResult
kk_vrs_poly_phase(struct kk_cmd_buffer *cmd, const struct mtl_vrs_pipelines *p,
   const struct kk_vrs_context *context, enum mtl_vrs_phase phase, void *data)
{
   struct kk_vrs_poly_raster *draw = data;
   if (!mtl_vrs_order_native_phase(cmd->metal.render, p, phase))
      return VK_ERROR_UNKNOWN;
   mtl_render_set_pipeline_state(cmd->metal.render, mtl_vrs_phase_pipeline(p, phase));
   return kk_poly_retained_draw(draw->raster, cmd->metal.render,
      draw->coarse, draw->indirect_draw);
}

static void
kk_vrs_restore_poly(mtl_render_encoder *encoder, void *data)
{
   struct kk_vrs_poly_raster *draw = data;
   mtl_render_set_pipeline_state(encoder, draw->original_pipeline);
   mtl_render_set_argument_table(encoder, draw->raster->vertex_table, MTL_RENDER_STAGE_VERTEX);
   mtl_render_set_argument_table(encoder, draw->original_fragment, MTL_RENDER_STAGE_FRAGMENT);
}

VkResult
kk_vrs_cmd_encode_poly_job(struct kk_cmd_buffer *cmd,
   const struct kk_poly_retained_raster *raster, uint64_t indirect_draw,
   const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_fragment, mtl_argument_table *coarse_fragment,
   const struct kk_vrs_context *context)
{
   if (!cmd || !raster || raster->command != cmd || !indirect_draw ||
       !cmd->metal.render || !context || !pipelines ||
       raster->producer != cmd->state.gfx.shader ||
       !original_fragment || !coarse_fragment || original_fragment == coarse_fragment ||
       coarse_fragment == raster->vertex_table ||
       context->original_per_draw != raster->per_draw_address)
      return VK_ERROR_UNKNOWN;
   struct kk_vrs_poly_raster draw = {
      .raster = raster, .indirect_draw = indirect_draw,
      .original_fragment = original_fragment, .coarse = coarse_fragment,
      .original_pipeline = kk_attachment_pipeline(cmd, raster->producer,
         raster->conservative ? 2 : 1),
   };
   if (!draw.original_pipeline)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   mtl_set_address(coarse_fragment, raster->layer_selector_address, KK_ARGUMENT_POLY_LAYER_REPLAY);
   return kk_vrs_cmd_encode_phases(cmd, raster->producer, pipelines, context,
      kk_vrs_poly_phase, kk_vrs_restore_poly, &draw);
}

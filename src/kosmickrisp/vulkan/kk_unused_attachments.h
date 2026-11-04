/* SPDX-License-Identifier: MIT */
#ifndef KK_UNUSED_ATTACHMENTS_H
#define KK_UNUSED_ATTACHMENTS_H

#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "kk_perf.h"

/* The retained descriptor stays unspecialized for later format variants. */
static mtl_render_pipeline_state *
kk_attachment_compile_pipeline(struct kk_device *device,
   struct kk_shader *shader, mtl_render_pipeline_descriptor *descriptor)
{
   if (!shader->info.framebuffer_read_mask)
      return KK_PERF_CALL(device, KK_PERF_METAL_PIPELINE, mtl_new_render_pipeline(device->mtl_compiler_handle, descriptor));
   return KK_PERF_CALL(device, KK_PERF_METAL_PIPELINE, mtl_new_render_pipeline_for_attachments(device->mtl_compiler_handle,
      descriptor, shader->info.vs.rt_formats, MAX_DRAW_BUFFERS, 0,
      shader->info.vs.sample_count, true));
}

struct kk_attachment_variant {
   struct kk_attachment_variant *next;
   enum mtl_pixel_format formats[MAX_DRAW_BUFFERS];
   unsigned kind;
   enum mtl_primitive_topology_class topology;
   uint32_t samples;
   bool indirect;
   bool statistics;
   mtl_render_pipeline_state *pipeline;
};

struct kk_attachment_pipelines {
   simple_mtx_t mutex;
   mtl_render_pipeline_descriptor *descriptor[KK_PIPELINE_KIND_COUNT];
   bool statistics[KK_PIPELINE_KIND_COUNT];
   bool dynamic_variants;
   enum mtl_primitive_topology_class topology[KK_PIPELINE_KIND_COUNT];
   mtl_depth_stencil_state *depth_stencil[4];
   struct kk_attachment_variant *variants;
};

/* Present only after a compiled GS actually consumes statistics addresses.
 * The ordinary shader and attachment-cache layouts gain no storage word. */
struct kk_statistics_pre_gs {
   mtl_function_descriptor *function;
   mtl_compute_pipeline_state *pipeline;
};

static struct kk_statistics_pre_gs *
kk_statistics_pre_gs(struct kk_shader *shader)
{
   assert(shader->info.geometry_statistics && shader->attachment_pipelines);
   return (void *)(shader->attachment_pipelines + 1);
}

static bool
kk_attachment_dynamic_variants(struct kk_device *device, struct kk_shader *shader)
{
   return device->vk.enabled_features.dynamicRenderingUnusedAttachments ||
      shader->info.vs.dynamic_rasterization_samples ||
      device->vk.enabled_features.deviceGeneratedCommands ||
      device->vk.enabled_features.drawIndirectCount ||
      shader->info.polygon_point || shader->info.poly_layer_replay;
}

static VkResult
kk_attachment_pipelines_create(struct kk_device *device, struct kk_shader *shader)
{
   if (shader->attachment_pipelines)
      return VK_SUCCESS;
   size_t bytes = sizeof(*shader->attachment_pipelines);
   if (shader->info.geometry_statistics)
      bytes += sizeof(struct kk_statistics_pre_gs);
   struct kk_attachment_pipelines *cache = calloc(1, bytes);
   if (!cache)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   cache->dynamic_variants = kk_attachment_dynamic_variants(device, shader);
   simple_mtx_init(&cache->mutex, mtx_plain);
   shader->attachment_pipelines = cache;
   return VK_SUCCESS;
}

static mtl_depth_stencil_state *
kk_compile_ds_state(struct kk_device *device, struct kk_shader_info *info);

mtl_depth_stencil_state *
kk_attachment_depth_stencil(struct kk_cmd_buffer *cmd, struct kk_shader *shader)
{
   mtl_depth_stencil_state *original = shader->pipeline.gfx.ds_handle;
   struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
   if (!cache || !original)
      return original;
   unsigned pipeline_mask = (shader->info.vs.d_format != MTL_PIXEL_FORMAT_INVALID) |
      ((shader->info.vs.s_format != MTL_PIXEL_FORMAT_INVALID) << 1);
   const struct kk_rendering_state *render = &cmd->state.gfx.render;
   unsigned mask = pipeline_mask &
      ((render->depth_att.iview != NULL) | ((render->stencil_att.iview != NULL) << 1));
   if (mask == pipeline_mask)
      return original;
   mtl_depth_stencil_state *state = p_atomic_read(&cache->depth_stencil[mask]);
   if (state)
      return state;
   simple_mtx_lock(&cache->mutex);
   state = p_atomic_read(&cache->depth_stencil[mask]);
   if (!state) {
      struct kk_shader_info info = shader->info;
      if (!(mask & 1)) {
         info.vs.has_depth_write = false;
         info.vs.depth_compare_op = VK_COMPARE_OP_ALWAYS;
      }
      if (!(mask & 2))
         info.vs.has_stencil_test = false;
      state = kk_compile_ds_state(kk_cmd_buffer_device(cmd), &info);
      p_atomic_set(&cache->depth_stencil[mask], state);
   }
   simple_mtx_unlock(&cache->mutex);
   return state;
}

static bool
kk_discard_unused_color(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   const nir_lower_blend_options *opts = data;
   unsigned location = nir_intrinsic_io_semantics(intr).location;
   if (location < FRAG_RESULT_DATA0 ||
       location >= FRAG_RESULT_DATA0 + ARRAY_SIZE(opts->rt) ||
       opts->rt[location - FRAG_RESULT_DATA0].format != PIPE_FORMAT_NONE)
      return false;
   nir_instr_remove(&intr->instr);
   return true;
}

static void
kk_attachment_pipelines_finish(struct kk_shader *shader)
{
   struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
   if (!cache)
      return;
   while (cache->variants) {
      struct kk_attachment_variant *variant = cache->variants;
      cache->variants = variant->next;
      mtl_release(variant->pipeline);
      free(variant);
   }
   for (unsigned i = 0; i < ARRAY_SIZE(cache->descriptor); ++i)
      mtl_release(cache->descriptor[i]);
   for (unsigned i = 0; i < ARRAY_SIZE(cache->depth_stencil); ++i)
      mtl_release(cache->depth_stencil[i]);
   if (shader->info.geometry_statistics) {
      struct kk_statistics_pre_gs *statistics = kk_statistics_pre_gs(shader);
      mtl_release(statistics->function);
      mtl_release(statistics->pipeline);
   }
   simple_mtx_destroy(&cache->mutex);
   free(cache);
}

/* Variants are immutable and remain alive until their shader is destroyed. */
static mtl_render_pipeline_state *
kk_attachment_find_variant(struct kk_attachment_pipelines *cache,
   unsigned kind, const enum mtl_pixel_format *formats, bool indirect,
   uint32_t samples, enum mtl_primitive_topology_class topology, bool statistics)
{
   for (struct kk_attachment_variant *v = p_atomic_read(&cache->variants);
        v; v = v->next) {
      if (v->kind == kind && v->indirect == indirect &&
          v->statistics == statistics && v->samples == samples &&
          v->topology == topology &&
          !memcmp(v->formats, formats, sizeof(v->formats)))
         return v->pipeline;
   }
   return NULL;
}

static mtl_render_pipeline_state *
kk_attachment_pipeline_for_formats(struct kk_device *device,
   struct kk_shader *shader, unsigned kind, const enum mtl_pixel_format *formats,
   bool indirect, VkResult *error, uint32_t samples,
   enum mtl_primitive_topology_class topology, bool statistics)
{
   if (shader->lazy_polygon &&
       (kind == KK_PIPELINE_POLYGON || kind == KK_PIPELINE_CONSERVATIVE)) {
      VkResult result = kk_shader_ensure_polygon(device, shader);
      if (result != VK_SUCCESS) {
         *error = result;
         return NULL;
      }
   }
   mtl_render_pipeline_state *original = kind == KK_PIPELINE_MESH_VIRTUAL
      ? shader->mesh->runtime->virtual->gfx.render : kind == KK_PIPELINE_MESH_CAPTURE
      ? shader->mesh->runtime->retained[0]->gfx.render : kind == KK_PIPELINE_MESH_READER
      ? shader->mesh->runtime->retained[1]->gfx.render : kind == KK_PIPELINE_MESH_SLICED
      ? shader->mesh->runtime->sliced[0]->gfx.render : kind == KK_PIPELINE_MESH_VIRTUAL_SLICED
      ? shader->mesh->runtime->sliced[1]->gfx.render : kind == KK_PIPELINE_CONSERVATIVE
      ? shader->pipeline.gfx.conservative_render : kind == KK_PIPELINE_POLYGON
      ? shader->pipeline.gfx.polygon_render : shader->pipeline.gfx.render;
   if (!original) {
      *error = VK_ERROR_INVALID_SHADER_NV;
      return NULL;
   }
   struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
   if (!cache) {
      if (indirect || statistics) *error = VK_ERROR_OUT_OF_HOST_MEMORY;
      return indirect || statistics ? NULL : original;
   }
   statistics &= cache->statistics[kind];

   if (!indirect && !statistics && topology == cache->topology[kind] &&
       samples == shader->info.vs.sample_count &&
       !memcmp(formats, shader->info.vs.rt_formats, sizeof(shader->info.vs.rt_formats)))
      return original;

   mtl_render_pipeline_state *cached = kk_attachment_find_variant(
      cache, kind, formats, indirect, samples, topology, statistics);
   if (cached)
      return cached;
   simple_mtx_lock(&cache->mutex);
   cached = kk_attachment_find_variant(cache, kind, formats, indirect,
                                       samples, topology, statistics);
   if (cached) {
      simple_mtx_unlock(&cache->mutex);
      return cached;
   }
   struct kk_attachment_variant *variant = calloc(1, sizeof(*variant));
   VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
   if (variant) {
      uint32_t disabled = 0;
      for (unsigned i = 0; i < MAX_DRAW_BUFFERS; ++i) {
         if (shader->info.vs.rt_formats[i] == MTL_PIXEL_FORMAT_INVALID ||
             formats[i] == MTL_PIXEL_FORMAT_INVALID)
            disabled |= BITFIELD_BIT(i);
      }
      mtl_render_pipeline_descriptor *descriptor = indirect
         ? kk_dgc_render_descriptor(cache->descriptor[kind])
         : mtl_retain(cache->descriptor[kind]);
      if (descriptor && statistics) {
         mtl_render_pipeline_descriptor *counting =
            mtl_statistics_render_descriptor(descriptor);
         mtl_release(descriptor);
         descriptor = counting;
      }
      if (!descriptor) {
         free(variant);
         simple_mtx_unlock(&cache->mutex);
         *error = VK_ERROR_OUT_OF_HOST_MEMORY;
         return NULL;
      }
      variant->pipeline = KK_PERF_CALL(device, KK_PERF_METAL_PIPELINE, mtl_new_render_pipeline_for_raster_attachments(
         device->mtl_compiler_handle,
         descriptor, formats, MAX_DRAW_BUFFERS, disabled, samples,
         shader->info.framebuffer_read_mask != 0, topology));
      mtl_release(descriptor);
      result = VK_ERROR_INVALID_SHADER_NV;
      if (variant->pipeline) {
         memcpy(variant->formats, formats, sizeof(shader->info.vs.rt_formats));
         variant->kind = kind;
         variant->topology = topology;
         variant->indirect = indirect;
         variant->statistics = statistics;
         variant->samples = samples;
         variant->next = p_atomic_read(&cache->variants);
         p_atomic_set(&cache->variants, variant);
         simple_mtx_unlock(&cache->mutex);
         return variant->pipeline;
      }
      free(variant);
   }
   simple_mtx_unlock(&cache->mutex);
   *error = result;
   return NULL;
}
mtl_render_pipeline_state *
kk_attachment_pipeline(struct kk_cmd_buffer *cmd, struct kk_shader *shader,
                       unsigned kind)
{
   const struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
   const bool statistics = cache && cache->statistics[kind] &&
      cmd->state.gfx.descriptors.root.draw.fragment_statistic != 0;
   if (kind == KK_PIPELINE_MAIN && !cmd->state.gfx.dgc_icb &&
       (!cache || !cache->dynamic_variants)) {
      if (!statistics)
         return shader->pipeline.gfx.render;
      /* Query-only caches have the pipeline's immutable attachment state. */
      VkResult error = VK_SUCCESS;
      mtl_render_pipeline_state *pipeline = kk_attachment_pipeline_for_formats(
         kk_cmd_buffer_device(cmd), shader, kind, shader->info.vs.rt_formats,
         false, &error, shader->info.vs.sample_count, cache->topology[kind], true);
      if (!pipeline)
         vk_command_buffer_set_error(&cmd->vk, error);
      return pipeline;
   }
   const struct kk_rendering_state *render = &cmd->state.gfx.render;
   enum mtl_pixel_format formats[MAX_DRAW_BUFFERS] = {0};
   for (unsigned i = 0; i < render->color_att_count; ++i) {
      unsigned location = render->color_map[i];
      if (location < MAX_DRAW_BUFFERS && render->color_att[i].iview)
         formats[location] = vk_format_to_mtl_pixel_format(render->color_att[i].vk_format);
   }
   uint32_t samples = MAX2(cmd->vk.dynamic_graphics_state.ms.rasterization_samples, 1);
   enum mtl_primitive_topology_class topology = kind == KK_PIPELINE_CONSERVATIVE ?
      MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE : kind == KK_PIPELINE_POLYGON && shader->info.polygon_point ?
      kk_polygon_raster_topology(shader->info.polygon.primitive_vertices,
         cmd->vk.dynamic_graphics_state.rs.polygon_mode,
         cmd->vk.dynamic_graphics_state.rs.line.mode) : shader->info.vs.topology;
   VkResult error = VK_SUCCESS;
   mtl_render_pipeline_state *pipeline = kk_attachment_pipeline_for_formats(
      kk_cmd_buffer_device(cmd), shader, kind, formats,
      cmd->state.gfx.dgc_icb, &error, samples, topology, statistics);
   if (!pipeline)
      vk_command_buffer_set_error(&cmd->vk, error);
   return pipeline;
}

VkResult
kk_dgc_eager_graphics_pipeline(struct kk_device *device, struct kk_shader *shader)
{
   if (!shader->info.indirect_bindable || shader->info.dgc_replay ||
       shader->info.vs.dynamic_vi)
      return VK_SUCCESS;
   for (unsigned kind = 0; kind < 2; ++kind) {
      struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
      if (!cache || !cache->descriptor[kind]) continue;
      VkResult error = VK_SUCCESS;
      if (!kk_attachment_pipeline_for_formats(device, shader, kind,
             shader->info.vs.rt_formats, true, &error,
             shader->info.vs.sample_count, cache->topology[kind], false))
         return error;
   }
   return VK_SUCCESS;
}

#endif

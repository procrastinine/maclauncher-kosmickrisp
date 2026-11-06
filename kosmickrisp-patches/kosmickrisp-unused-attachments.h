/* SPDX-License-Identifier: MIT */
#ifndef KK_UNUSED_ATTACHMENTS_H
#define KK_UNUSED_ATTACHMENTS_H

#include "util/simple_mtx.h"

struct kk_attachment_variant {
   struct kk_attachment_variant *next;
   enum mtl_pixel_format formats[MAX_DRAW_BUFFERS];
   bool polygon;
   mtl_render_pipeline_state *pipeline;
};

struct kk_attachment_pipelines {
   simple_mtx_t mutex;
   mtl_render_pipeline_descriptor *descriptor[2];
   mtl_depth_stencil_state *depth_stencil[4];
   struct kk_attachment_variant *variants;
};

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
   simple_mtx_lock(&cache->mutex);
   if (!cache->depth_stencil[mask]) {
      struct kk_shader_info info = shader->info;
      if (!(mask & 1)) {
         info.vs.has_depth_write = false;
         info.vs.depth_compare_op = VK_COMPARE_OP_ALWAYS;
      }
      if (!(mask & 2))
         info.vs.has_stencil_test = false;
      cache->depth_stencil[mask] = kk_compile_ds_state(kk_cmd_buffer_device(cmd), &info);
   }
   mtl_depth_stencil_state *state = cache->depth_stencil[mask];
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
   simple_mtx_destroy(&cache->mutex);
   free(cache);
}

mtl_render_pipeline_state *
kk_attachment_pipeline(struct kk_cmd_buffer *cmd, struct kk_shader *shader,
                       bool polygon)
{
   mtl_render_pipeline_state *original = polygon ? shader->pipeline.gfx.polygon_render
                                                : shader->pipeline.gfx.render;
   struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
   if (!cache)
      return original;

   const struct kk_rendering_state *render = &cmd->state.gfx.render;
   enum mtl_pixel_format formats[MAX_DRAW_BUFFERS] = {0};
   for (unsigned i = 0; i < render->color_att_count; ++i) {
      unsigned location = render->color_map[i];
      if (location < MAX_DRAW_BUFFERS && render->color_att[i].iview)
         formats[location] = vk_format_to_mtl_pixel_format(render->color_att[i].vk_format);
   }
   if (!memcmp(formats, shader->info.vs.rt_formats, sizeof(formats)))
      return original;

   simple_mtx_lock(&cache->mutex);
   for (struct kk_attachment_variant *v = cache->variants; v; v = v->next) {
      if (v->polygon == polygon && !memcmp(v->formats, formats, sizeof(formats))) {
         simple_mtx_unlock(&cache->mutex);
         return v->pipeline;
      }
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
      variant->pipeline = mtl_new_render_pipeline_for_attachments(
         kk_cmd_buffer_device(cmd)->mtl_compiler_handle,
         cache->descriptor[polygon], formats, MAX_DRAW_BUFFERS, disabled);
      result = VK_ERROR_INVALID_SHADER_NV;
      if (variant->pipeline) {
         memcpy(variant->formats, formats, sizeof(formats));
         variant->polygon = polygon;
         variant->next = cache->variants;
         cache->variants = variant;
         simple_mtx_unlock(&cache->mutex);
         return variant->pipeline;
      }
      free(variant);
   }
   simple_mtx_unlock(&cache->mutex);
   vk_command_buffer_set_error(&cmd->vk, result);
   return NULL;
}
#endif

/* SPDX-License-Identifier: MIT */
#ifndef KK_DEPTH_BOUNDS_MESH_DRAW_H
#define KK_DEPTH_BOUNDS_MESH_DRAW_H

/* Only this callback reads the borrowed Mesh batch. Each recorded prefix owns
 * its captured inputs; existing continuation gates its tail before the next
 * prefix clears/reuses the same finite workspace or Mesh resets its arena. */
VkResult
kk_depth_bounds_record_mesh_batch(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *source, void *data)
{
   (void)data;
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *producer = gfx->shader;
   uint64_t maximum = (uint64_t)source->group_capacity * source->layout.max_primitives;
   if (!maximum || cmd->vk.dynamic_graphics_state.rs.rasterizer_discard_enable)
      return VK_SUCCESS;
   if (maximum > UINT32_MAX)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct kk_ptr storage = {0};
   const VkRect2D area = gfx->render.area;
   uint32_t layer_begin = gfx->render.view_mask ? ffs(gfx->render.view_mask) - 1u : 0;
   uint32_t layer_end = gfx->render.view_mask ? layer_begin + 1 : gfx->render.layer_count;
   if (source->layer_filtered) {
      layer_begin = 0;
      layer_end = 1;
   }
   for (uint32_t layer = layer_begin; layer < layer_end; ++layer) {
      for (uint32_t y = area.offset.y & ~1u; y < area.offset.y + area.extent.height;) {
         uint32_t next_y = y;
         for (uint32_t x = area.offset.x & ~1u; x < area.offset.x + area.extent.width;) {
            uint32_t next_x = x;
            for (uint32_t base = 0; base < maximum;) {
               struct kk_ptr params = kk_pool_alloc(cmd, sizeof(struct kk_depth_bounds_envelope), 8);
               if (!params.cpu) return VK_ERROR_OUT_OF_HOST_MEMORY;
               memset(params.cpu, 0, sizeof(struct kk_depth_bounds_envelope));
               struct kk_interlock_draw *p = NULL;
               VkResult result = kk_interlock_prepare_draw(cmd, params, storage, true, &p);
               if (result != VK_SUCCESS) return result;
               storage = p->storage;
               struct kk_interlock_envelope *env = p->env;
               uint32_t count = MIN2(p->batch_size, maximum - base);
               env->tile_x = x; env->tile_y = y;
               env->tile_width = ALIGN_POT(MIN2(p->tile_size, env->width - x), 2);
               env->tile_height = ALIGN_POT(MIN2(p->tile_size, env->height - y), 2);
               env->layer = layer; env->primitive_base = base; env->primitive_count = count;
               p->layer = layer;
               p->first_capture = base == 0;
               p->last_capture = base + count == maximum;
               /* A clear prefix must preserve live attachment contents before
                * changing encoders, including originally transient images. */
               if (cmd->metal.render) kk_apply_attachment_store_ops(cmd, true);
               mtl_compute_encoder *compute = cs_get_compute(cmd);
               const struct kk_depth_bounds_capture_context clear = {
                  .env = params.gpu, .original_per_draw = source->per_draw_address,
               };
               struct kk_ptr context = kk_pool_upload(cmd, &clear, sizeof(clear), 8);
               if (!context.gpu) { kk_interlock_draw_finish(p->device, p); return VK_ERROR_OUT_OF_HOST_MEMORY; }
               mtl_set_address(cmd->argument_table, context.gpu, KK_ARGUMENT_PER_DRAW);
               mtl_compute_set_pipeline_state(compute, producer->interlock_pipeline->bounds->clear);
               mtl_compute_set_argument_table(compute, cmd->argument_table);
               mtl_dispatch_threads(compute,
                  (struct mtl_size){env->tile_width * env->tile_height * count, 1, 1},
                  (struct mtl_size){64, 1, 1});
               struct kk_mesh_retained_batch batch;
               result = kk_mesh_retained_transaction_resume(cmd, source, &batch);
               const struct kk_depth_stencil_snapshot *snapshot = NULL;
               if (result == VK_SUCCESS)
                  result = kk_cmd_capture_depth_stencil(cmd, producer, &snapshot);
               if (result != VK_SUCCESS) { kk_interlock_draw_finish(p->device, p); return result; }
               kk_depth_bounds_set_transaction_state(env, &snapshot->ds, snapshot->tested_aspects);
               mtl_render_pipeline_state *capture;
               result = kk_depth_bounds_mesh_capture_pipeline(cmd, &batch, &capture);
               if (result != VK_SUCCESS) { kk_interlock_draw_finish(p->device, p); return result; }
               /* The prefix executable stays retained through its packet drain. */
               p->pipeline[KK_INTERLOCK_CAPTURE] = mtl_retain(capture);
               struct mtl_scissor_rect scissors[KK_MAX_VIEWPORTS];
               for (unsigned i = 0; i < p->scissor_count; ++i) {
                  const struct mtl_scissor_rect *app = &p->scissors[i];
                  uint32_t right = MIN2(x + env->tile_width, app->x + app->width);
                  uint32_t bottom = MIN2(y + env->tile_height, app->y + app->height);
                  uint32_t left = MIN2(MAX2(x, app->x), right);
                  uint32_t top = MIN2(MAX2(y, app->y), bottom);
                  scissors[i] = (struct mtl_scissor_rect){left, top, right - left, bottom - top};
               }
               mtl_set_scissor_rects(batch.encoder, scissors, p->scissor_count);
               /* Only the shared software transaction counts passing samples. */
               mtl_set_visibility_result_mode(batch.encoder,
                  MTL_VISIBILITY_RESULT_MODE_DISABLED, 0);
               for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i) {
                  const struct kk_mesh_retained_job job = {
                     (base + i) / source->layout.max_primitives,
                     (base + i) % source->layout.max_primitives};
                  struct kk_ptr selected = kk_pool_upload(cmd, &job, sizeof(job), 4);
                  if (!selected.gpu) { result = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
                  result = kk_depth_bounds_mesh_capture_job(cmd, &batch, snapshot,
                     params.gpu, selected.gpu, cmd->argument_table);
               }
               mtl_set_visibility_result_mode(batch.encoder, gfx->occlusion.mode,
                  gfx->occlusion.index * 8);
               if (result == VK_SUCCESS)
                  result = kk_interlock_append_draw(cmd, p);
               if (result != VK_SUCCESS) { kk_interlock_draw_finish(p->device, p); return result; }
               base += count;
               next_x = x + p->tile_size;
               next_y = y + p->tile_size;
            }
            x = next_x;
         }
         y = next_y;
      }
   }
   return VK_SUCCESS;
}
#endif

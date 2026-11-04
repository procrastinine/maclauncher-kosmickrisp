/* SPDX-License-Identifier: MIT */
#include "kk_interlock_draw.h"
#include "kk_interlock_envelope.h"
#include "kk_interlock_shader_data.h"
#include "kk_interlock_submit.h"
#include "kk_interlock_heap.h"
#include "kk_cmd_buffer.h"
#include "kk_device.h"
#include "kk_perf.h"
#include "kk_debug.h"
#include <stdio.h>
#include "kk_format.h"
#include "kk_image_layout.h"
#include "kk_image_view.h"
#include "vk_alloc.h"
#include "vk_format.h"
#include "kosmickrisp/bridge/mtl_bridge.h"

#define KK_INTERLOCK_SMALL_TILE_SIZE 8u
#define KK_INTERLOCK_MAX_TILE_SIZE 64u
#define KK_INTERLOCK_BATCH_SIZE 32u
#define KK_INTERLOCK_INPUT_BUDGET (16u * 1024u * 1024u)

struct kk_interlock_attachment {
   mtl_texture *read;
   mtl_texture *render;
   unsigned level;
   unsigned layer;
};

/* Everything read by the continuation belongs to this packet or is an
 * immutable GPU upload from the recorded command buffer. No callback reads
 * the command buffer's current dynamic state or bound shader pointers. */
struct kk_interlock_draw {
   struct kk_device *device;
   struct kk_interlock_envelope *env;
   struct kk_interlock_shader_info info;
   mtl_argument_table *arguments;
   mtl_render_pipeline_state *pipeline[3];
   mtl_compute_pipeline_state *load;
   mtl_depth_stencil_state *replace;
   mtl_command_allocator *allocator;
   mtl_command_buffer *command;
   struct kk_bo *scratch;
   struct kk_interlock_attachment color[8], depth, stencil;
   struct mtl_scissor_rect scissors[KK_MAX_VIEWPORTS];
   unsigned scissor_count;
   VkRect2D area;
   unsigned layers;
   unsigned first_layer;
   unsigned capture_layers;
   unsigned raster_arity;
   enum mtl_primitive_type capture_type;
   unsigned batch_size;
   unsigned tile_size;
   unsigned primitives;
   unsigned primitive;
   unsigned x, y, layer;
   bool discard;
   mtl_command_queue *queue;
   mtl_feedback_handler_callback completed;
   void *callback_data;
};

static void
kk_interlock_attachment_finish(struct kk_interlock_attachment *a)
{
   mtl_release(a->read);
   mtl_release(a->render);
}

static void
kk_interlock_draw_finish(struct kk_device *dev, void *data)
{
   struct kk_interlock_draw *p = data;
   for (unsigned i = 0; i < ARRAY_SIZE(p->color); ++i)
      kk_interlock_attachment_finish(&p->color[i]);
   kk_interlock_attachment_finish(&p->depth);
   kk_interlock_attachment_finish(&p->stencil);
   for (unsigned i = 0; i < ARRAY_SIZE(p->pipeline); ++i)
      mtl_release(p->pipeline[i]);
   mtl_release(p->load);
   mtl_release(p->replace);
   mtl_release(p->arguments);
   mtl_release(p->command);
   mtl_release(p->allocator);
   if (p->scratch)
      kk_destroy_bo(dev, p->scratch);
   vk_free(&dev->vk.alloc, p);
}

static bool
kk_interlock_attachment_init(const struct kk_attachment *attachment,
   unsigned samples, bool stencil, struct kk_interlock_attachment *out)
{
   struct kk_image_view *view = attachment->iview;
   if (!view)
      return true;
   enum pipe_format format = view->planes[0].format;
   if (stencil)
      format = util_format_stencil_only(format);
   const struct kk_va_format *native = kk_get_va_format(format);
   struct kk_view_layout layout = {
      .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .sample_count_sa = samples,
      .format = {.pipe = format, .mtl = native->mtl_pixel_format},
      .base_level = view->planes[0].render_is_view ? 0 : view->vk.base_mip_level,
      .num_levels = 1,
      .base_array_layer = view->planes[0].render_is_view ? 0 : view->vk.base_array_layer,
      .array_len = view->vk.layer_count,
   };
   memcpy(layout.swizzle.channels, native->unswizzle.channels, 4);
   out->render = mtl_retain(view->planes[0].mtl_handle_render);
   out->level = layout.base_level;
   out->layer = layout.base_array_layer;
   out->read = mtl_new_texture_view_with(out->render, &layout);
   return out->render && out->read;
}

static void
kk_interlock_set_attachment(mtl_render_pass_attachment_descriptor *desc,
   const struct kk_interlock_attachment *a, unsigned layer)
{
   if (!a->render)
      return;
   mtl_render_pass_attachment_descriptor_set_texture(desc, a->render);
   mtl_render_pass_attachment_descriptor_set_level(desc, a->level);
   mtl_render_pass_attachment_descriptor_set_slice(desc, a->layer + layer);
   mtl_render_pass_attachment_descriptor_set_load_action(desc, MTL_LOAD_ACTION_LOAD);
   mtl_render_pass_attachment_descriptor_set_store_action(desc, MTL_STORE_ACTION_STORE);
}

static mtl_render_pass_descriptor *
kk_interlock_pass(struct kk_interlock_draw *p, unsigned program)
{
   mtl_render_pass_descriptor *pass = mtl_new_render_pass_descriptor();
   mtl_render_pass_descriptor_set_render_target_width(pass, p->env->width);
   mtl_render_pass_descriptor_set_render_target_height(pass, p->env->height);
   mtl_render_pass_descriptor_set_default_raster_sample_count(pass,
      program == KK_INTERLOCK_TRANSACTION ? 1 : p->env->samples);
   mtl_render_pass_descriptor_set_render_target_array_length(pass,
      program == KK_INTERLOCK_CAPTURE ? p->capture_layers : 1);
   if (program != KK_INTERLOCK_TRANSACTION)
      mtl_render_pass_descriptor_set_sample_positions(pass,
         (const struct mtl_sample_position *)p->env->sample_positions, p->env->samples);
   if (program == KK_INTERLOCK_REPLAY) {
      for (unsigned i = 0; i < ARRAY_SIZE(p->color); ++i)
         kk_interlock_set_attachment(
            mtl_render_pass_descriptor_get_color_attachment(pass, i),
            &p->color[i], p->layer);
      kk_interlock_set_attachment(
         mtl_render_pass_descriptor_get_depth_attachment(pass), &p->depth, p->layer);
      kk_interlock_set_attachment(
         mtl_render_pass_descriptor_get_stencil_attachment(pass), &p->stencil, p->layer);
   }
   return pass;
}

static void
kk_interlock_render_batch(struct kk_interlock_draw *p, unsigned program,
   unsigned count)
{
   struct kk_interlock_envelope *env = p->env;
   mtl_render_pass_descriptor *pass = kk_interlock_pass(p, program);
   mtl_render_encoder *enc =
      mtl_new_render_command_encoder_with_descriptor(p->command, pass);
   if (unlikely(p->device->perf != NULL))
      kk_perf_render_begin(p->device->perf, enc, pass);
   mtl_release(pass);
   mtl_barrier_after_queue_stages(enc, MTL_STAGE_ALL,
      MTL_STAGE_VERTEX | MTL_STAGE_FRAGMENT);
   mtl_render_set_pipeline_state(enc, p->pipeline[program]);
   mtl_render_set_argument_table(enc, p->arguments,
      MTL_RENDER_STAGE_VERTEX | MTL_RENDER_STAGE_FRAGMENT);
   struct mtl_viewport viewport = program == KK_INTERLOCK_CAPTURE ?
      (struct mtl_viewport){0, env->height, env->width, -(double)env->height, 0, 1} :
      (struct mtl_viewport){env->tile_x, env->tile_y,
         env->tile_width, env->tile_height, 0, 1};
   struct mtl_scissor_rect tile = {
      env->tile_x, env->tile_y,
      MIN2(env->tile_width, env->width - env->tile_x),
      MIN2(env->tile_height, env->height - env->tile_y)};
   unsigned right = tile.x + tile.width, bottom = tile.y + tile.height;
   tile.x = MAX2(tile.x, p->area.offset.x);
   tile.y = MAX2(tile.y, p->area.offset.y);
   tile.width = right > tile.x ? right - tile.x : 0;
   tile.height = bottom > tile.y ? bottom - tile.y : 0;
   if (program == KK_INTERLOCK_CAPTURE) {
      struct mtl_viewport viewports[KK_MAX_VIEWPORTS];
      struct mtl_scissor_rect scissors[KK_MAX_VIEWPORTS];
      for (unsigned i = 0; i < p->scissor_count; ++i) {
         viewports[i] = viewport;
         if (env->geometry.depth_clamp && env->geometry.polygon_mode != VK_POLYGON_MODE_POINT) {
            viewports[i].znear = env->geometry.viewport[i][4];
            viewports[i].zfar = env->geometry.viewport[i][5];
         }
         const struct mtl_scissor_rect *app = &p->scissors[i];
         unsigned x1 = MIN2(tile.x + tile.width, app->x + app->width);
         unsigned y1 = MIN2(tile.y + tile.height, app->y + app->height);
         unsigned x0 = MIN2(MAX2(tile.x, app->x), tile.x + tile.width);
         unsigned y0 = MIN2(MAX2(tile.y, app->y), tile.y + tile.height);
         scissors[i] = (struct mtl_scissor_rect){x0, y0,
            x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
      }
      mtl_set_viewports(enc, viewports, p->scissor_count);
      mtl_set_scissor_rects(enc, scissors, p->scissor_count);
      mtl_set_cull_mode(enc, MTL_CULL_MODE_NONE);
      mtl_set_depth_clip_mode(enc, env->geometry.depth_clamp || !env->geometry.depth_clip ?
         MTL_DEPTH_CLIP_MODE_CLAMP : MTL_DEPTH_CLIP_MODE_CLIP);
   } else {
      mtl_set_viewports(enc, &viewport, 1);
      mtl_set_scissor_rects(enc, &tile, 1);
   }
   if (program == KK_INTERLOCK_REPLAY)
      mtl_set_depth_stencil_state(enc, p->replace);
   unsigned arity = program == KK_INTERLOCK_CAPTURE ? p->raster_arity : 3;
   mtl_draw_primitives(enc, program == KK_INTERLOCK_CAPTURE ? p->capture_type : MTL_PRIMITIVE_TYPE_TRIANGLE,
      program == KK_INTERLOCK_CAPTURE ? p->primitive * arity : 0,
      program == KK_INTERLOCK_REPLAY ? 3 : count * arity, 1, 0);
   mtl_end_encoding(enc);
   if (unlikely(p->device->perf != NULL))
      kk_perf_render_end(p->device->perf, enc);
   mtl_release(enc);
}

static void kk_interlock_draw_step(struct kk_interlock_draw *p);

static void
kk_interlock_draw_completed(struct mtl_feedback_data *feedback)
{
   struct kk_interlock_draw *p = feedback->user_data;
   if (KK_DEBUG(MSL))
      fprintf(stderr, "interlock completed tile=%u,%u primitive=%u/%u layer=%u/%u error=%u\n",
         p->x, p->y, p->primitive, p->primitives, p->layer, p->layers, feedback->error);
   if (feedback->error == MTL_COMMAND_QUEUE_ERROR_NONE &&
       !vk_device_is_lost(&p->device->vk)) {
      p->primitive += MIN2(p->batch_size, p->primitives - p->primitive);
      if (p->primitive == p->primitives) {
         p->primitive = 0;
         p->x += p->tile_size;
         if (p->x >= (unsigned)(p->area.offset.x + p->area.extent.width)) {
            p->x = p->area.offset.x & ~1u;
            p->y += p->tile_size;
         }
         if (p->y >= (unsigned)(p->area.offset.y + p->area.extent.height)) {
            p->y = p->area.offset.y & ~1u;
            ++p->layer;
         }
      }
      if (p->primitives && p->layer < p->layers) {
         kk_interlock_draw_step(p);
         return;
      }
   }
   struct mtl_feedback_data final = *feedback;
   final.user_data = p->callback_data;
   if (vk_device_is_lost(&p->device->vk) && final.error == MTL_COMMAND_QUEUE_ERROR_NONE)
      final.error = MTL_COMMAND_QUEUE_ERROR_INTERNAL;
   p->completed(&final);
}

static void
kk_interlock_draw_step(struct kk_interlock_draw *p)
{
   struct kk_interlock_envelope *env = p->env;
   unsigned count = MIN2(p->batch_size, p->primitives - p->primitive);
   env->tile_x = p->x;
   env->tile_y = p->y;
   env->tile_width = ALIGN_POT(MIN2(p->tile_size, env->width - p->x), 2);
   env->tile_height = ALIGN_POT(MIN2(p->tile_size, env->height - p->y), 2);
   env->layer = p->layer;
   env->primitive_base = p->primitive;
   env->primitive_count = count;
   if (KK_DEBUG(MSL))
      fprintf(stderr, "interlock encode tile=%u,%u size=%u,%u primitive=%u+%u layer=%u/%u\n",
         p->x, p->y, env->tile_width, env->tile_height, p->primitive, count,
         p->layer, p->layers);
   unsigned cells = p->tile_size * p->tile_size * p->batch_size;
   uint64_t inputs = (uint64_t)p->tile_size * p->tile_size *
      p->batch_size * env->samples * env->input_stride;
   memset((char *)p->scratch->cpu + inputs, 0, cells * (4 + 4 + 16));
   mtl_command_allocator_reset(p->allocator);
   mtl_begin_command_buffer(p->command, p->allocator);
   if (p->primitives) {
      if (!p->primitive) {
         mtl_compute_encoder *compute = mtl_new_compute_command_encoder(p->command);
         mtl_barrier_after_queue_stages(compute, MTL_STAGE_ALL, MTL_STAGE_DISPATCH);
         mtl_compute_set_pipeline_state(compute, p->load);
         mtl_compute_set_argument_table(compute, p->arguments);
         mtl_dispatch_threads(compute,
            (struct mtl_size){env->tile_width, env->tile_height, env->samples},
            (struct mtl_size){8, 8, 1});
         mtl_end_encoding(compute);
         mtl_release(compute);
      }
      kk_interlock_render_batch(p, KK_INTERLOCK_CAPTURE, count);
      kk_interlock_render_batch(p, KK_INTERLOCK_TRANSACTION, count);
      if (p->primitive + count == p->primitives)
         kk_interlock_render_batch(p, KK_INTERLOCK_REPLAY, count);
   }
   mtl_end_command_buffer(p->command);
   simple_mtx_lock(&p->device->residency_set.mutex);
   mtl_residency_set_commit(p->device->residency_set.handle);
   simple_mtx_unlock(&p->device->residency_set.mutex);
   mtl_commit_options *options = mtl_new_commit_options();
   if (!options) {
      struct mtl_feedback_data failure = {
         .user_data = p->callback_data,
         .error = MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY,
         .error_message = "Interlock batch feedback allocation failed",
      };
      p->completed(&failure);
      return;
   }
   mtl_commit_options_add_feedback_handler(options, kk_interlock_draw_completed, p);
   mtl_command_queue_commit(p->queue, &p->command, 1, options);
   mtl_release(options);
}

static bool
kk_interlock_draw_encode(void *data, mtl_command_queue *queue,
   mtl_feedback_handler_callback completed, void *callback_data)
{
   struct kk_interlock_draw *p = data;
   if (vk_device_is_lost(&p->device->vk))
      return false;
   p->queue = queue;
   p->completed = completed;
   p->callback_data = callback_data;
   p->primitives = p->env->geometry.draw[0] / p->raster_arity;
   if (KK_DEBUG(MSL))
      fprintf(stderr, "interlock source draw=%u clip_grid=%u invocations=%u vertices_out=%u arity=%u clip=%u cull=%u\n",
         p->env->geometry.draw[0], p->env->geometry.clip_grid[0],
         p->env->geometry.invocations, p->env->geometry.vertices_out,
         p->raster_arity, p->env->geometry.clip_count, p->env->geometry.cull_count);
   if (p->discard || !p->area.extent.width || !p->area.extent.height || !p->layers)
      p->primitives = 0;
   p->primitive = 0;
   p->layer = p->first_layer;
   p->x = p->area.offset.x & ~1u;
   p->y = p->area.offset.y & ~1u;
   kk_interlock_draw_step(p);
   return true;
}

static struct kk_interlock_stencil_state
kk_interlock_stencil(const struct vk_stencil_test_face_state *state)
{
   return (struct kk_interlock_stencil_state){
      state->reference, state->compare_mask, state->write_mask, state->op.compare,
      state->op.fail, state->op.depth_fail, state->op.pass};
}

VkResult
kk_interlock_record_draw(struct kk_cmd_buffer *cmd)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *shader = cmd->state.shaders[MESA_SHADER_VERTEX];
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_interlock_draw *p = vk_zalloc(&dev->vk.alloc, sizeof(*p), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!p)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   p->device = dev;
   p->env = gfx->interlock_capture.cpu;
   p->info = shader->interlock->info;
   p->area = gfx->render.area;
   p->layers = gfx->render.layer_count;
   p->capture_layers = gfx->render.view_mask ? util_last_bit(gfx->render.view_mask) : p->layers;
   p->discard = dyn->rs.rasterizer_discard_enable;
   if (!p->env) {
      kk_interlock_draw_finish(dev, p);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   struct kk_interlock_envelope *env = p->env;
   env->width = p->area.offset.x + p->area.extent.width;
   env->height = p->area.offset.y + p->area.extent.height;
   env->samples = dyn->ms.rasterization_samples;
   unsigned source_arity = p->info.source_vertices_per_primitive;
   p->raster_arity = source_arity < 3 ? source_arity :
      env->geometry.polygon_mode == VK_POLYGON_MODE_LINE ? 2 : 3;
   p->capture_type = p->raster_arity == 1 ? MTL_PRIMITIVE_TYPE_POINT :
      p->raster_arity == 2 ? MTL_PRIMITIVE_TYPE_LINE : MTL_PRIMITIVE_TYPE_TRIANGLE;
   env->source_vertices_per_primitive = p->raster_arity;
   env->source_points_expanded = source_arity == 3 &&
      env->geometry.polygon_mode == VK_POLYGON_MODE_POINT;
   if (env->view_mask) {
      p->first_layer = env->source_view;
      p->layers = env->source_view + 1;
   }
   env->input_stride = p->info.inputs.stride + 32;
   env->sample_mask = dyn->ms.sample_mask;
   env->depth_enable = dyn->ds.depth.test_enable && gfx->render.depth_att.iview;
   env->depth_write = dyn->ds.depth.write_enable && gfx->render.depth_att.iview;
   env->depth_compare = dyn->ds.depth.compare_op;
   env->bounds_enable = dyn->ds.depth.bounds_test.enable;
   env->bounds_min = dyn->ds.depth.bounds_test.min;
   env->bounds_max = dyn->ds.depth.bounds_test.max;
   env->stencil_enable = dyn->ds.stencil.test_enable && gfx->render.stencil_att.iview;
   env->stencil_state[0] = kk_interlock_stencil(&dyn->ds.stencil.front);
   env->stencil_state[1] = kk_interlock_stencil(&dyn->ds.stencil.back);
   if (gfx->occlusion.mode != MTL_VISIBILITY_RESULT_MODE_DISABLED)
      env->occlusion = dev->occlusion_queries.bo->gpu + gfx->occlusion.index * 8;
   if (!mtl_interlock_default_sample_positions(dev->mtl_handle,
          (struct mtl_sample_position *)env->sample_positions, env->samples)) {
      kk_interlock_draw_finish(dev, p);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (gfx->render.sample_locations_enable)
      memcpy(env->sample_positions, gfx->render.sample_locations,
         MIN2(gfx->render.sample_locations_count, env->samples) * 8);
   else if (gfx->render.ms_bresenham_lines)
      for (unsigned i = 0; i < env->samples; ++i)
         env->sample_positions[i][0] = env->sample_positions[i][1] = 0.5f;
   /* Larger surfaces amortize feedback and encoder setup over larger tiles.
    * Bound each input allocation by the same budget, reducing tile size when
    * one primitive's captured inputs would exceed it. */
   p->tile_size = MAX2(env->width, env->height) > KK_INTERLOCK_MAX_TILE_SIZE ?
      KK_INTERLOCK_MAX_TILE_SIZE : KK_INTERLOCK_SMALL_TILE_SIZE;
   while ((uint64_t)p->tile_size * p->tile_size * env->samples * env->input_stride >
          KK_INTERLOCK_INPUT_BUDGET && p->tile_size > 2)
      p->tile_size /= 2;
   uint64_t tile_cells = p->tile_size * p->tile_size;
   p->batch_size = MAX2(1, MIN2(KK_INTERLOCK_BATCH_SIZE,
      KK_INTERLOCK_INPUT_BUDGET / (tile_cells * env->samples * env->input_stride)));
   uint64_t cells = tile_cells * p->batch_size;
   uint64_t input_bytes = cells * env->samples * env->input_stride;
   uint64_t header_bytes = cells * (4 + 4 + 16);
   uint64_t shadow_bytes = tile_cells * env->samples * (4 + 4 + 8 * 16);
   VkResult result = kk_alloc_bo(dev, &cmd->vk.base,
      input_bytes + header_bytes + shadow_bytes, 16, &p->scratch);
   if (result != VK_SUCCESS)
      goto fail;
   env->inputs = p->scratch->gpu;
   env->claims = env->inputs + input_bytes;
   env->coverage = env->claims + cells * 4;
   env->metadata = env->coverage + cells * 4;
   env->depth = env->metadata + cells * 16;
   env->stencil = env->depth + tile_cells * env->samples * 4;
   for (unsigned i = 0; i < ARRAY_SIZE(env->color); ++i)
      env->color[i] = env->stencil + tile_cells * env->samples * (4 + i * 16);
   enum mtl_pixel_format empty[8], formats[8];
   for (unsigned i = 0; i < ARRAY_SIZE(formats); ++i)
      empty[i] = formats[i] = MTL_PIXEL_FORMAT_INVALID;
   for (unsigned i = 0; i < gfx->render.color_att_count; ++i) {
      unsigned logical = gfx->render.color_map[i];
      if (logical == MESA_VK_ATTACHMENT_UNUSED || !gfx->render.color_att[i].iview)
         continue;
      if (!kk_interlock_attachment_init(&gfx->render.color_att[i], env->samples,
             false, &p->color[logical])) {
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         goto fail;
      }
      env->color_texture[logical] = mtl_texture_get_gpu_resource_id(p->color[logical].read);
      formats[logical] = vk_format_to_mtl_pixel_format(gfx->render.color_att[i].vk_format);
   }
   if (!kk_interlock_attachment_init(&gfx->render.depth_att, env->samples,
          false, &p->depth) ||
       !kk_interlock_attachment_init(&gfx->render.stencil_att, env->samples,
          true, &p->stencil)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto fail;
   }
   if (p->depth.read)
      env->depth_texture = mtl_texture_get_gpu_resource_id(p->depth.read);
   if (p->stencil.read)
      env->stencil_texture = mtl_texture_get_gpu_resource_id(p->stencil.read);
   for (unsigned i = 0; i < ARRAY_SIZE(p->pipeline); ++i) {
      mtl_render_pipeline_state *handle;
      result = kk_interlock_pipeline_get(dev, shader->interlock_pipeline, i,
         i == KK_INTERLOCK_REPLAY ? formats : empty,
         i == KK_INTERLOCK_TRANSACTION ? 1 : env->samples,
         i != KK_INTERLOCK_CAPTURE ? MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE :
            p->raster_arity == 1 ? MTL_PRIMITIVE_TOPOLOGY_CLASS_POINT :
            p->raster_arity == 2 ? MTL_PRIMITIVE_TOPOLOGY_CLASS_LINE :
            MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE,
         gfx->descriptors.root.draw.fragment_statistic != 0, &handle);
      if (result != VK_SUCCESS)
         goto fail;
      p->pipeline[i] = mtl_retain(handle);
   }
   p->load = mtl_retain(shader->interlock_pipeline->load);
   p->replace = mtl_retain(shader->interlock_pipeline->replace);
   p->scissor_count = MAX2(dyn->vp.scissor_count, 1);
   for (unsigned i = 0; i < p->scissor_count; ++i) {
      const VkRect2D *r = &dyn->vp.scissors[i];
      int64_t x0 = MAX2(r->offset.x, p->area.offset.x);
      int64_t y0 = MAX2(r->offset.y, p->area.offset.y);
      int64_t x1 = MIN2((int64_t)r->offset.x + r->extent.width,
         (int64_t)p->area.offset.x + p->area.extent.width);
      int64_t y1 = MIN2((int64_t)r->offset.y + r->extent.height,
         (int64_t)p->area.offset.y + p->area.extent.height);
      p->scissors[i] = (struct mtl_scissor_rect){MAX2(x0, 0), MAX2(y0, 0),
         x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
   }
   p->allocator = mtl_new_command_allocator(dev->mtl_handle);
   p->command = mtl_new_command_buffer(dev->mtl_handle);
   if (!p->replace || !p->allocator || !p->command) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto fail;
   }
   /* Existing argument bindings already describe this exact uploaded draw.
    * Detaching them also prevents later command recording from changing them. */
   mtl_argument_table *tail_arguments = kk_new_command_argument_table(dev);
   if (!tail_arguments) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto fail;
   }
   p->arguments = cmd->argument_table;
   result = kk_interlock_split(cmd, p, kk_interlock_draw_encode, kk_interlock_draw_finish);
   if (result != VK_SUCCESS) {
      p->arguments = NULL;
      mtl_release(tail_arguments);
      goto fail;
   }
   cmd->argument_table = tail_arguments;
   /* An event wait gates later GPU work. Give an otherwise empty final tail
    * real work so its completion and subsequent application event signals
    * cannot escape the selected transaction. This private input word is dead
    * after the continuation has completed. */
   libkk_write_u32(cmd, kk_grid_1d(1), true, p->scratch->gpu, 0);
   return VK_SUCCESS;
fail:
   kk_interlock_draw_finish(dev, p);
   return result;
}

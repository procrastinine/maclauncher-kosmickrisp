/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_COMMAND_H
#define KK_MESH_COMMAND_H
#include "kosmickrisp/libkk/kk_mesh_dispatch.h"
#include "kosmickrisp/libkk/kk_argument_table.h"
#include "kosmickrisp/libkk/kk_canonical_layout.h"
#include "kk_debug.h"
#include "kk_interlock_heap.h"
#include <stdio.h>

static bool
kk_mesh_retained_active(const struct kk_cmd_buffer *cmd)
{
   return cmd->state.gfx.layer_replay.bind ||
          cmd->state.gfx.mesh_retained_consumer.consume;
}

bool
kk_mesh_retained_raster_begin(struct kk_cmd_buffer *cmd,
   kk_mesh_retained_consume consume, void *data)
{
   if (!consume || cmd->state.gfx.mesh_retained_consumer.consume ||
       !cmd->state.gfx.shader || cmd->state.gfx.shader->info.stage != MESA_SHADER_MESH)
      return false;
   cmd->state.gfx.mesh_retained_consumer =
      (struct kk_mesh_retained_consumer){consume, data};
   return true;
}

void
kk_mesh_retained_raster_end(struct kk_cmd_buffer *cmd)
{
   const struct kk_mesh_retained_consumer state = cmd->state.gfx.mesh_retained_consumer;
   if (state.transaction) {
      cmd->interlock_heap_active = state.saved_heap_active;
      cmd->interlock_heap_used = state.saved_heap_used;
   }
   cmd->state.gfx.mesh_retained_consumer = (struct kk_mesh_retained_consumer){0};
}

VkResult
kk_mesh_retained_raster_replay(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, mtl_render_pipeline_state *pipeline,
   mtl_argument_table *fragment_table)
{
   if (!batch || !pipeline || !fragment_table || !batch->encoder ||
       batch->producer != cmd->state.gfx.shader)
      return VK_ERROR_INITIALIZATION_FAILED;
   mtl_render_set_pipeline_state(batch->encoder, pipeline);
   mtl_render_set_argument_table(batch->encoder, batch->mesh_table, MTL_RENDER_STAGE_MESH);
   mtl_render_set_argument_table(batch->encoder, fragment_table, MTL_RENDER_STAGE_FRAGMENT);
   mtl_draw_mesh_threadgroups_indirect(batch->encoder, batch->grid,
      (struct mtl_size){1,1,1}, (struct mtl_size){1,1,1});
   return VK_SUCCESS;
}

VkResult
kk_mesh_retained_raster_job_parameters(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, uint64_t job, uint64_t *parameters)
{
   if (!batch || !job || (job & 3u) || !parameters || !batch->grid ||
       batch->producer != cmd->state.gfx.shader)
      return VK_ERROR_INITIALIZATION_FAILED;
   const struct kk_mesh_params p = {
      .records = job, .indirect_grid = batch->grid,
      .indirect_count = batch->outputs, .max_draw_count = batch->group_capacity,
      .predicate_inverted = batch->logical_layer,
   };
   struct kk_ptr data = kk_pool_upload(cmd, &p, sizeof(p), 8);
   if (!data.gpu) return VK_ERROR_OUT_OF_HOST_MEMORY;
   *parameters = data.gpu;
   return VK_SUCCESS;
}

VkResult
kk_mesh_retained_raster_replay_job(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, uint64_t parameters,
   mtl_render_pipeline_state *pipeline, mtl_argument_table *fragment_table)
{
   if (!batch || !parameters || !pipeline || !fragment_table || !batch->encoder ||
       batch->producer != cmd->state.gfx.shader)
      return VK_ERROR_INITIALIZATION_FAILED;
   /* Mesh owns this reader-only upload/binding. records selects the GPU job,
    * indirect_grid validates original group coverage; existing cache address
    * and logical-Layer meanings are unchanged. Snapshot before restoring. */
   mtl_set_address(batch->mesh_table, parameters, KK_ARGUMENT_MESH_PARAMETERS);
   mtl_render_set_pipeline_state(batch->encoder, pipeline);
   mtl_render_set_argument_table(batch->encoder, batch->mesh_table, MTL_RENDER_STAGE_MESH);
   mtl_render_set_argument_table(batch->encoder, fragment_table, MTL_RENDER_STAGE_FRAGMENT);
   mtl_draw_mesh_threadgroups(batch->encoder, (struct mtl_size){1,1,1},
      (struct mtl_size){1,1,1}, (struct mtl_size){1,1,1});
   mtl_set_address(batch->mesh_table, batch->reader_parameters, KK_ARGUMENT_MESH_PARAMETERS);
   return VK_SUCCESS;
}

bool
kk_mesh_retained_transaction_begin(struct kk_cmd_buffer *cmd,
   kk_mesh_retained_consume consume, void *data)
{
   if (!kk_mesh_retained_raster_begin(cmd, consume, data)) return false;
   struct kk_mesh_retained_consumer *state = &cmd->state.gfx.mesh_retained_consumer;
   state->saved_heap_active = cmd->interlock_heap_active;
   state->saved_heap_used = cmd->interlock_heap_used;
   /* Native output records live in this command's bounded pool arena. Any
    * canonical packet pointers also need the existing private Poly heap so
    * another submission cannot recycle their contents while we are deferred. */
   if (cmd->state.gfx.shader->mesh->info.flat_heap) {
      VkResult result = kk_interlock_heap_select(cmd, true);
      if (result != VK_SUCCESS) {
         cmd->interlock_heap_active = state->saved_heap_active;
         cmd->interlock_heap_used = state->saved_heap_used;
         *state = (struct kk_mesh_retained_consumer){0};
         vk_command_buffer_set_error(&cmd->vk, result);
         return false;
      }
   }
   state->transaction = true;
   return true;
}

void
kk_mesh_retained_transaction_end(struct kk_cmd_buffer *cmd)
{
   kk_mesh_retained_raster_end(cmd);
}

VkResult
kk_mesh_retained_transaction_resume(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *source, struct kk_mesh_retained_batch *current)
{
   const struct kk_mesh_retained_consumer *state = &cmd->state.gfx.mesh_retained_consumer;
   if (!source || !current || !state->transaction || !state->callback_active ||
       source->producer != cmd->state.gfx.shader ||
       source->outputs != cmd->state.gfx.mesh_retained_outputs)
      return VK_ERROR_INITIALIZATION_FAILED;
   /* No original producer, heap reset or input preparation occurs here.
    * Restore fixed graphics/attachment state after the owner's split, then
    * return the current encoder; the owner binds DS BYPASS for capture. */
   kk_flush_gfx_state(cmd);
   if (cmd->vk.record_result != VK_SUCCESS) return cmd->vk.record_result;
   assert(source->producer == cmd->state.gfx.shader);
   *current = *source;
   current->encoder = cs_get_render(cmd);
   mtl_set_address(current->mesh_table, current->reader_parameters, KK_ARGUMENT_MESH_PARAMETERS);
   mtl_set_address(current->mesh_table, current->layer_selector, KK_ARGUMENT_POLY_LAYER_REPLAY);
   return VK_SUCCESS;
}

/* Optional retained raster cache. Counts and all output slots belong to the
 * physical group; no compaction can change PrimitiveID or index ordering. */
static uint32_t
kk_mesh_retained_stride(const struct kk_mesh_shader_info *info)
{
   uint32_t v = MAX2(info->max_vertices, 1u);
   uint32_t p = MAX2(info->max_primitives, 1u);
   uint32_t vpp = mesa_vertices_per_prim(info->primitive);
   uint32_t primitives = ALIGN_POT(32u + v * info->retained_strides[0], 32u);
   uint32_t indices = ALIGN_POT(primitives + p * info->retained_strides[1], 4u);
   return ALIGN_POT(indices + p * (vpp + 1u) * 4u, 32u);
}

bool
kk_mesh_layer_replay_begin(struct kk_cmd_buffer *cmd, uint32_t layers,
   VkResult (*bind)(struct kk_cmd_buffer *, void *, uint32_t), void *data)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   if (!layers || !bind || gfx->layer_replay.bind || !gfx->shader ||
       gfx->shader->info.stage != MESA_SHADER_MESH)
      return false;
   gfx->layer_replay = (struct kk_poly_layer_replay_state) {
      .bind = bind, .data = data, .layer_count = layers,
   };
   return true;
}

void
kk_mesh_layer_replay_end(struct kk_cmd_buffer *cmd)
{
   if (cmd->mesh_table)
      mtl_set_address(cmd->mesh_table, 0, KK_ARGUMENT_POLY_LAYER_REPLAY);
   cmd->state.gfx.layer_replay = (struct kk_poly_layer_replay_state){0};
}

/* Both flat and projected packets use the existing shared Poly heap. Bound each physical
 * draw by declared outputs, then reclaim only after its fragments complete.
 * This never allocates output storage proportional to the logical grid. */
static uint32_t
kk_mesh_canonical_span(struct kk_cmd_buffer *cmd)
{
   const struct kk_mesh_shader_info *info = &cmd->state.gfx.shader->mesh->info;
   uint32_t retained_span = kk_mesh_retained_active(cmd)
      ? MAX2(KK_MESH_RETAINED_BYTES, kk_mesh_retained_stride(info)) / kk_mesh_retained_stride(info)
      : KK_MESH_MAX_PHYSICAL_GROUPS;
   if (!info->flat_heap)
      return MIN2(retained_span, KK_MESH_MAX_PHYSICAL_GROUPS);
   kk_heap(cmd);
   const struct poly_heap *heap = cmd->interlock_heap_active
      ? kk_interlock_heap_bo(cmd->interlock_heap)->cpu
      : kk_cmd_buffer_device(cmd)->heap->cpu;
   uint64_t available = heap->size;
   unsigned packet_size = info->projected_basis ?
      kk_canonical_projected_buffer_size() : kk_canonical_buffer_size();
   uint64_t bytes = (uint64_t)MAX2(info->max_primitives, 1u) *
      (packet_size + 16u);
   uint32_t span = MAX2(1u, MIN2((uint64_t)KK_MESH_MAX_PHYSICAL_GROUPS, available / bytes));
   span = MIN2(span, retained_span);
   /* Every full virtual slice must be an exact native rectangle. Otherwise
    * rounding its grid height would execute the next slice's logical groups
    * twice. The final partial rectangle is guarded by mesh_count. */
   if (span > KK_MESH_MAX_DIMENSION)
      span -= span % KK_MESH_MAX_DIMENSION;
   if (KK_DEBUG(MSL))
      fprintf(stderr, "mesh canonical: heap=%llu packet=%u max_primitives=%u span=%u\n",
         (unsigned long long)available, packet_size,
         info->max_primitives, span);
   return span;
}

static uint32_t
kk_mesh_canonical_task_limit(const struct kk_mesh_shader_info *info, bool retained)
{
   return (info->flat_heap || retained) ? MIN2(KK_MESH_BATCH_SIZE,
      MAX2(1u, KK_MESH_MAX_GROUPS / MAX2(info->task_grid_bound, 1u))) :
      KK_MESH_BATCH_SIZE;
}

static void
kk_mesh_reset_canonical_heap(struct kk_cmd_buffer *cmd)
{
   if (!cmd->state.gfx.shader->mesh->info.flat_heap)
      return;
   libkk_mesh_reset_heap(cmd, kk_grid_1d(1), true,
      cmd->state.gfx.per_draw_data.per_vertex_heap,
      cmd->state.gfx.generated_heap_floor);
}

static mtl_argument_table *
kk_mesh_table(struct kk_cmd_buffer *cmd)
{
   if (!cmd->mesh_table) {
      struct kk_device *dev = kk_cmd_buffer_device(cmd);
      mtl_argument_table_descriptor *desc = mtl_new_argument_table_descriptor();
      mtl_set_max_buffer_binding_count(desc, kk_argument_table_buffer_count(
         (struct kk_argument_table_usage){.mesh = true, .poly_layer_replay = true,
            .interlock_guard = dev->interlock_guard != NULL}));
      cmd->mesh_table = mtl_new_argument_table(dev->mtl_handle, desc);
      mtl_release(desc);
      if (!cmd->mesh_table) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
      mtl_set_address(cmd->mesh_table, dev->samplers.table.bo->gpu, KK_ARGUMENT_SAMPLERS);
      mtl_set_address(cmd->mesh_table, 0, KK_ARGUMENT_POLY_LAYER_REPLAY);
      if (dev->interlock_guard)
         mtl_set_address(cmd->mesh_table, dev->interlock_guard->gpu,
                         KK_ARGUMENT_INTERLOCK_GUARD);
   }
   mtl_set_address(cmd->mesh_table, cmd->state.gfx.descriptors.root.addr, KK_ARGUMENT_ROOT);
   return cmd->mesh_table;
}

static struct kk_mesh_params
kk_mesh_parameters(struct kk_cmd_buffer *cmd, uint64_t grid, uint64_t count,
                     uint32_t id, uint32_t maximum)
{
   struct kk_mesh_params params = {
      .indirect_grid = grid,
      .indirect_count = count,
      .draw_id = id,
      .max_draw_count = maximum,
      .predicate = cmd->state.cond_render.enabled ? cmd->state.cond_render.address : 0,
      .predicate_inverted = cmd->state.cond_render.inverted,
   };
   if (cmd->state.gfx.shader->mesh->info.queries) {
      params.task_statistic = kk_pipeline_stat_addr(cmd,
         VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT);
      params.mesh_statistic = kk_pipeline_stat_addr(cmd,
         VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT);
      params.primitive_statistic = cmd->state.gfx.mesh_primitives_query;
   }
   return params;
}

static bool
kk_mesh_bind_parameters(struct kk_cmd_buffer *cmd, mtl_argument_table *table,
                           const struct kk_mesh_params *params)
{
   struct kk_ptr gpu = kk_pool_upload(cmd, params, sizeof(*params), 8);
   if (!gpu.gpu)
      return false;
   mtl_set_address(table, gpu.gpu, KK_ARGUMENT_MESH_PARAMETERS);
   return true;
}

static bool
kk_mesh_bind_draw(struct kk_cmd_buffer *cmd, mtl_argument_table *table,
                    uint32_t draw_id)
{
   struct kk_shader *s = cmd->state.gfx.shader;
   if (s->mesh->info.flat_heap) {
      cmd->state.gfx.per_draw_data.per_vertex_heap = kk_heap(cmd);
      cmd->state.gfx.per_draw_data.per_vertex_topology = s->mesh->info.primitive;
      cmd->state.gfx.per_draw_data.provoking_last =
         cmd->vk.dynamic_graphics_state.rs.provoking_vertex ==
         VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   }
   cmd->state.gfx.mesh_per_draw_address = 0;
   bool uses = s->info.uses_per_draw_data || cmd->state.gfx.mesh_retained_consumer.consume;
   if (!uses)
      return true;
   cmd->state.gfx.per_draw_data.draw_id = draw_id;
   struct kk_ptr data = kk_pool_upload(cmd, &cmd->state.gfx.per_draw_data,
      /* Logical ViewIndex is the last Mesh input. GS report addresses are not
       * consumed by Task/Mesh or their retained raster readers. */
      offsetof(struct kk_per_draw_data, multiview_index) +
         sizeof(cmd->state.gfx.per_draw_data.multiview_index), 8);
   if (!data.gpu)
      return false;
   cmd->state.gfx.mesh_per_draw_address = data.gpu;
   mtl_set_address(table, data.gpu, KK_ARGUMENT_PER_DRAW);
   return true;
}

static void
kk_mesh_render(struct kk_cmd_buffer *cmd, struct kk_pipeline_handles *pipe,
                  mtl_argument_table *table, uint64_t indirect,
                  struct mtl_size direct, bool virtual)
{
   kk_mesh_reset_canonical_heap(cmd);
   kk_flush_gfx_state(cmd);
   struct kk_shader *s = cmd->state.gfx.shader;
   unsigned kind = pipe == &s->pipeline ? KK_PIPELINE_MAIN : KK_PIPELINE_MESH_VIRTUAL;
   mtl_render_pipeline_state *render = kk_attachment_pipeline(cmd, s, kind);
   if (!render)
      return;
   mtl_render_encoder *enc = cs_get_render(cmd);
   mtl_render_set_pipeline_state(enc, render);
   if (table != cmd->argument_table)
      mtl_set_address(table, cmd->state.gfx.descriptors.root.addr, KK_ARGUMENT_ROOT);
   const struct kk_mesh_fragment_program *fragment = kk_mesh_fragment(s);
   if (fragment && fragment->bind_arguments)
      fragment->bind_arguments(fragment, s, pipe, kind, render, table);
   mtl_render_set_argument_table(enc, table,
      MTL_RENDER_STAGE_OBJECT | MTL_RENDER_STAGE_MESH | MTL_RENDER_STAGE_FRAGMENT);
   struct mtl_size object = virtual || !s->mesh->info.has_task
      ? (struct mtl_size){1, 1, 1} : s->mesh->info.task_local_size;
   if (indirect)
      mtl_draw_mesh_threadgroups_indirect(enc, indirect, object, s->mesh->info.local_size);
   else
      mtl_draw_mesh_threadgroups(enc, direct, object, s->mesh->info.local_size);
   /* Classic pipeline encoding always finds its original table bound. */
   mtl_render_set_argument_table(enc, cmd->argument_table,
      MTL_RENDER_STAGE_VERTEX | MTL_RENDER_STAGE_FRAGMENT);
}

/* One arena is reused in submission order. Its size is independent of the
 * task draw's workgroup count, including indirect and generated draws. */
static bool
kk_mesh_scratch(struct kk_cmd_buffer *cmd)
{
   if (cmd->state.gfx.mesh_batch)
      return true;
   struct kk_ptr p = kk_pool_alloc(cmd,
      KK_MESH_BATCH_SIZE * 16384u + KK_MESH_BATCH_SIZE * sizeof(struct kk_mesh_task_record) +
      sizeof(struct kk_mesh_batch), 16);
   if (!p.gpu)
      return false;
   cmd->state.gfx.mesh_payloads = p.gpu;
   cmd->state.gfx.mesh_records = p.gpu + KK_MESH_BATCH_SIZE * 16384u;
   cmd->state.gfx.mesh_batch = cmd->state.gfx.mesh_records +
      KK_MESH_BATCH_SIZE * sizeof(struct kk_mesh_task_record);
   return true;
}

/* Bound both GPU work per native command buffer and Metal's encoded resource
 * indices. Counters span logical draws, including inactive counted records. */
static bool
kk_mesh_begin_batch(struct kk_cmd_buffer *cmd, uint32_t draws, bool task)
{
   if ((cmd->mesh_batches_in_part >= 64u ||
        cmd->mesh_draws_in_part + draws > 32768u) &&
       !kk_mesh_split_command_buffer(cmd))
      return false;
   cmd->mesh_batches_in_part += task;
   cmd->mesh_draws_in_part += draws;
   return true;
}

static void
kk_draw_mesh_unsliced(struct kk_cmd_buffer *cmd, struct mtl_size direct,
                uint64_t indirect, uint64_t count, uint32_t draw_count,
                uint32_t stride, bool force_virtual)
{
   struct kk_shader *shader = cmd->state.gfx.shader;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_pipeline_handles *pipe;
   VkResult result = kk_mesh_get_pipeline(dev, shader, force_virtual, &pipe);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }
   bool virtual = pipe->gfx.pre_render_count != 0;
   kk_flush_gfx_state(cmd);
   bool params_needed = virtual || shader->mesh->info.queries;
   /* Mesh fragments may read the existing nullable logical-Layer selector,
    * even when their producer needs no query parameters. */
   mtl_argument_table *table = kk_mesh_table(cmd);
   if (!table)
      return;
   if (!indirect && (virtual || count || cmd->state.cond_render.enabled)) {
      uint32_t grid[] = {direct.x, direct.y, direct.z};
      indirect = kk_pool_upload(cmd, grid, sizeof(grid), 4).gpu;
      if (!indirect)
         return;
   }
   if (virtual && !kk_mesh_scratch(cmd))
      return;
   for (uint32_t draw = 0; draw < draw_count; ++draw) {
      if (!kk_mesh_bind_draw(cmd, table, draw))
         return;
      uint64_t grid = indirect ? indirect + (uint64_t)draw * stride : 0;
      struct kk_mesh_params p = kk_mesh_parameters(cmd, grid, count, draw, draw_count);
      uint32_t task_limit = kk_mesh_canonical_task_limit(&shader->mesh->info, kk_mesh_retained_active(cmd));
      if (virtual && shader->mesh->info.flat_heap) {
         p.pad = kk_mesh_canonical_span(cmd);
         p.max_draw_count = task_limit;
      }
      if (!virtual) {
         if (params_needed && !kk_mesh_bind_parameters(cmd, table, &p))
            return;
         if (count || cmd->state.cond_render.enabled) {
            struct kk_ptr b = kk_pool_alloc(cmd, sizeof(struct kk_mesh_batch), 8);
            if (!b.gpu) return;
            p.batch = b.gpu;
            uint64_t args = kk_pool_upload(cmd, &p, sizeof(p), 8).gpu;
            if (!args) return;
            libkk_mesh_filter(cmd, kk_grid_1d(1), true, args);
            grid = b.gpu + offsetof(struct kk_mesh_batch, mesh_dispatch);
         }
         kk_mesh_render(cmd, pipe, table, grid, direct, false);
         continue;
      }
      p.batch = cmd->state.gfx.mesh_batch;
      p.records = cmd->state.gfx.mesh_records;
      p.payloads = cmd->state.gfx.mesh_payloads;
      p.payload_stride = 16384;
      uint64_t groups = direct.x ? (uint64_t)direct.x * direct.y * direct.z : KK_MESH_MAX_GROUPS;
      uint32_t batches = DIV_ROUND_UP(groups, task_limit);
      for (uint32_t batch = 0; batch < batches; ++batch) {
         if (!kk_mesh_begin_batch(cmd, 1, true))
            return;
         p.task_base = batch * task_limit;
         struct kk_ptr args = kk_pool_upload(cmd, &p, sizeof(p), 8);
         if (!args.gpu) return;
         mtl_compute_encoder *enc = cs_get_compute(cmd);
         kk_cmd_bind_compute_arguments(cmd, enc, cmd->argument_table);
         libkk_mesh_prepare(cmd, kk_grid_1d(1), true, args.gpu);
         mtl_set_address(table, cmd->state.gfx.descriptors.root.addr, KK_ARGUMENT_ROOT);
         mtl_set_address(table, args.gpu, KK_ARGUMENT_MESH_PARAMETERS);
         kk_cmd_bind_compute_arguments(cmd, enc, table);
         mtl_compute_pipeline_state *task_pipeline = kk_cmd_sampler_compute_pipeline(
            cmd, shader, pipe->gfx.pre_render[0]);
         if (!task_pipeline) return;
         mtl_compute_set_pipeline_state(enc, task_pipeline);
         mtl_dispatch_threadgroups_with_indirect_buffer(enc,
            p.batch + offsetof(struct kk_mesh_batch, task_dispatch), shader->mesh->info.task_local_size);
         kk_cmd_bind_compute_arguments(cmd, enc, cmd->argument_table);
         libkk_mesh_prefix(cmd, kk_grid_1d(1), true, args.gpu);
         kk_mesh_render(cmd, pipe, table,
            p.batch + offsetof(struct kk_mesh_batch, mesh_dispatch), direct, true);
      }
   }
}

/* Every physical slice remains in this encoder and consumes the same bounded
 * arena before a later task batch overwrites it. Empty indirect slices execute
 * no workgroups. Only their parameter address and grid differ. */
static void
kk_mesh_render_slices_native(struct kk_cmd_buffer *cmd, struct kk_pipeline_handles *pipe,
                         mtl_argument_table *table, uint64_t grids,
                         uint64_t parameters, uint32_t slice_count, bool virtual)
{
   kk_flush_gfx_state(cmd);
   struct kk_shader *s = cmd->state.gfx.shader;
   unsigned kind = virtual ? KK_PIPELINE_MESH_VIRTUAL_SLICED : KK_PIPELINE_MESH_SLICED;
   mtl_render_pipeline_state *render = kk_attachment_pipeline(cmd, s, kind);
   if (!render)
      return;
   mtl_render_encoder *enc = cs_get_render(cmd);
   mtl_render_set_pipeline_state(enc, render);
   mtl_set_address(table, cmd->state.gfx.descriptors.root.addr, KK_ARGUMENT_ROOT);
   const struct kk_mesh_fragment_program *fragment = kk_mesh_fragment(s);
   if (fragment && fragment->bind_arguments)
      fragment->bind_arguments(fragment, s, pipe, kind, render, table);
   struct mtl_size object = virtual || !s->mesh->info.has_task
      ? (struct mtl_size){1, 1, 1} : s->mesh->info.task_local_size;
   /* Metal snapshots the current table at each draw. Keep the same table
    * bound across slices so a slot update does not rebind every stage. */
   mtl_render_set_argument_table(enc, table,
      MTL_RENDER_STAGE_OBJECT | MTL_RENDER_STAGE_MESH | MTL_RENDER_STAGE_FRAGMENT);
   for (uint32_t i = 0; i < slice_count; ++i) {
      mtl_set_address(table, parameters + (uint64_t)i * sizeof(struct kk_mesh_params),
                      KK_ARGUMENT_MESH_PARAMETERS);
      mtl_draw_mesh_threadgroups_indirect(enc, grids + 3u * sizeof(uint32_t) * i,
                                          object, s->mesh->info.local_size);
   }
   mtl_render_set_argument_table(enc, cmd->argument_table,
      MTL_RENDER_STAGE_VERTEX | MTL_RENDER_STAGE_FRAGMENT);
}

/* One physical slice is captured once, then read for every logical layer.
 * The same grid/count, cache contents, canonical packets, per-draw upload and
 * fragment executable remain valid until the last reader has been encoded.
 * Only then may the next slice/task batch reset or overwrite its arenas. */
static void
kk_mesh_render_retained(struct kk_cmd_buffer *cmd, struct kk_pipeline_handles *capture,
   mtl_argument_table *table, uint64_t grid, uint64_t parameters,
   struct kk_mesh_retained_batch_id id, uint32_t group_capacity,
   uint32_t group_bound)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   const struct kk_poly_layer_replay_state replay = gfx->layer_replay;
   struct kk_shader *s = gfx->shader;
   struct kk_pipeline_handles *reader;
   VkResult result = kk_mesh_get_retained_pipeline(kk_cmd_buffer_device(cmd), s, true, &reader);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }
   kk_flush_gfx_state(cmd);
   mtl_render_pipeline_state *render = kk_attachment_pipeline(cmd, s, KK_PIPELINE_MESH_CAPTURE);
   if (!render) return;
   mtl_render_encoder *enc = cs_get_render(cmd);
   mtl_render_set_pipeline_state(enc, render);
   mtl_set_address(table, parameters, KK_ARGUMENT_MESH_PARAMETERS);
   mtl_set_address(table, gfx->descriptors.root.addr, KK_ARGUMENT_ROOT);
   mtl_set_address(table, 0, KK_ARGUMENT_POLY_LAYER_REPLAY);
   const struct kk_mesh_fragment_program *fragment = kk_mesh_fragment(s);
   if (fragment && fragment->bind_arguments)
      fragment->bind_arguments(fragment, s, capture,
         KK_PIPELINE_MESH_CAPTURE, render, table);
   mtl_render_set_argument_table(enc, table,
      MTL_RENDER_STAGE_MESH | MTL_RENDER_STAGE_FRAGMENT);
   mtl_draw_mesh_threadgroups_indirect(enc, grid,
      (struct mtl_size){1,1,1}, s->mesh->info.local_size);
   /* Submission preflight kk_whole_pass_transient_safe rejects Mesh pipeline
    * binds and Mesh commands for the WHOLE pass, before BeginRendering or
    * earlier ordinary draws. kk_transient_prepare_rendering therefore uses
    * its existing private backing, never a late promotion of live tile data.
    * This split orders capture writes; it is not a general memoryless route. */
   cs_get_compute(cmd);
   uint32_t view_mask = gfx->render.view_mask;
   assert(!(view_mask & (view_mask - 1u)));
   uint32_t first = view_mask ? ffs(view_mask) - 1u : 0u;
   uint32_t layers = view_mask ? 1u : replay.bind ? replay.layer_count : 1u;
   for (uint32_t offset = 0; offset < layers; ++offset) {
      uint32_t layer = first + offset;
      result = replay.bind ? replay.bind(cmd, replay.data, layer) : VK_SUCCESS;
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return;
      }
      assert(gfx->shader == s);
      kk_flush_gfx_state(cmd);
      render = kk_attachment_pipeline(cmd, s, KK_PIPELINE_MESH_READER);
      if (!render) return;
      /* Stage-private copies retain the original counter/predicate inputs in
       * prepare/task records. The reader's body never evaluates those fields. */
      struct kk_mesh_params p = { .indirect_count = gfx->mesh_retained_outputs,
                                  .predicate_inverted = layer };
      uint64_t args = kk_pool_upload(cmd, &p, sizeof(p), 8).gpu;
      const struct kk_poly_layer_selector selector = {layer, 1};
      uint64_t select = replay.bind
         ? kk_pool_upload(cmd, &selector, sizeof(selector), 8).gpu : 0;
      if (!args || (replay.bind && !select)) return;
      mtl_set_address(table, args, KK_ARGUMENT_MESH_PARAMETERS);
      mtl_set_address(table, select, KK_ARGUMENT_POLY_LAYER_REPLAY);
      mtl_set_address(table, gfx->descriptors.root.addr, KK_ARGUMENT_ROOT);
      const struct kk_mesh_fragment_program *fragment = kk_mesh_fragment(s);
      if (fragment && fragment->bind_arguments)
         fragment->bind_arguments(fragment, s, reader,
            KK_PIPELINE_MESH_READER, render, table);
      enc = cs_get_render(cmd);
      const struct kk_mesh_retained_consumer consumer = gfx->mesh_retained_consumer;
      if (consumer.consume) {
         const struct kk_mesh_shader_info *info = &s->mesh->info;
         uint32_t vpp = mesa_vertices_per_prim(info->primitive);
         uint32_t primitive_offset = ALIGN_POT(32u + MAX2(info->max_vertices, 1u) *
                                              info->retained_strides[0], 32u);
         uint32_t index_offset = ALIGN_POT(primitive_offset + MAX2(info->max_primitives, 1u) *
                                          info->retained_strides[1], 4u);
         struct kk_mesh_retained_batch batch = {
            .producer = s, .raster_descriptor = kk_mesh_retained_raster_descriptor(s),
            .raster_pipeline = render, .encoder = enc, .mesh_table = table,
            .outputs = gfx->mesh_retained_outputs,
            .output_bytes = gfx->mesh_retained_output_bytes, .grid = grid,
            .reader_parameters = args, .layer_selector = select,
            .per_draw_address = gfx->mesh_per_draw_address,
            .task_records = gfx->mesh_records, .id = id,
            .layout = {
               .group_stride = kk_mesh_retained_stride(info), .vertices = 32u,
               .vertex_stride = info->retained_strides[0],
               .primitives = primitive_offset, .primitive_stride = info->retained_strides[1],
               .indices = index_offset, .layers = index_offset + MAX2(info->max_primitives, 1u) * vpp * 4u,
               .vertices_per_primitive = vpp,
               .max_vertices = info->max_vertices, .max_primitives = info->max_primitives,
            },
            .group_capacity = group_capacity, .group_bound = group_bound,
            .logical_layer = layer,
            .view_index = gfx->per_draw_data.multiview_index,
            .layer_filtered = replay.bind != NULL,
         };
         memcpy(batch.layout.fields, info->retained_fields, sizeof(batch.layout.fields));
         /* All reads of this reusable source are encoded before return. A
          * transaction callback may resume across preserved-store splits;
          * only its independent input/DS captures survive in continuations.
          * Their existing completion gate orders the next arena overwrite. */
         gfx->mesh_retained_consumer.callback_active = true;
         result = consumer.consume(cmd, &batch, consumer.data);
         gfx->mesh_retained_consumer.callback_active = false;
         if (result != VK_SUCCESS) {
            vk_command_buffer_set_error(&cmd->vk, result);
            return;
         }
      } else {
         mtl_render_set_pipeline_state(enc, render);
         mtl_render_set_argument_table(enc, table,
            MTL_RENDER_STAGE_MESH | MTL_RENDER_STAGE_FRAGMENT);
         mtl_draw_mesh_threadgroups_indirect(enc, grid,
            (struct mtl_size){1,1,1}, (struct mtl_size){1,1,1});
      }
   }
   /* Complete all raster reads before the next capture's overwrite. Heap reset
    * kernels and task batches subsequently close this encoder in-order. */
   cs_get_compute(cmd);
   mtl_set_address(table, 0, KK_ARGUMENT_POLY_LAYER_REPLAY);
}

static void
kk_mesh_render_slices(struct kk_cmd_buffer *cmd, struct kk_pipeline_handles *pipe,
                        mtl_argument_table *table, uint64_t grids,
                        uint64_t parameters, uint32_t slice_count, bool virtual,
                        uint32_t draw_id, uint32_t task_base, uint32_t span,
                        uint64_t groups)
{
   if (kk_mesh_retained_active(cmd)) {
      for (uint32_t i = 0; i < slice_count; ++i) {
         if (!kk_mesh_begin_batch(cmd, 1, false)) return;
         kk_mesh_reset_canonical_heap(cmd);
         /* Slices are linear spans of the physical groups (filter/prefix). */
         uint64_t base = (uint64_t)i * span;
         uint32_t bound = groups > base ? MIN2(groups - base, (uint64_t)span) : 0;
         kk_mesh_render_retained(cmd, pipe, table,
            grids + (uint64_t)i * 3u * sizeof(uint32_t),
            parameters + (uint64_t)i * sizeof(struct kk_mesh_params),
            (struct kk_mesh_retained_batch_id){draw_id, task_base, i, i * span}, span, bound);
         if (cmd->vk.record_result != VK_SUCCESS) return;
      }
      return;
   }
   if (!cmd->state.gfx.shader->mesh->info.flat_heap) {
      kk_mesh_render_slices_native(cmd, pipe, table, grids, parameters, slice_count, virtual);
      return;
   }
   for (uint32_t i = 0; i < slice_count; ++i) {
      if (!kk_mesh_begin_batch(cmd, 1, false))
         return;
      kk_mesh_reset_canonical_heap(cmd);
      kk_mesh_render_slices_native(cmd, pipe, table,
         grids + (uint64_t)i * 3u * sizeof(uint32_t),
         parameters + (uint64_t)i * sizeof(struct kk_mesh_params), 1, virtual);
   }
}

static bool
kk_mesh_sliced_scratch(struct kk_cmd_buffer *cmd, bool virtual)
{
   if (virtual) {
      if (!kk_mesh_scratch(cmd))
         return false;
      if (!cmd->state.gfx.mesh_slices) {
         uint32_t maximum = DIV_ROUND_UP(KK_MESH_BATCH_SIZE * KK_MESH_MAX_GROUPS,
                                        KK_MESH_MAX_PHYSICAL_GROUPS);
         struct kk_ptr p = kk_pool_alloc(cmd, maximum * 3u * sizeof(uint32_t), 8);
         if (!p.gpu)
            return false;
         cmd->state.gfx.mesh_slices = p.gpu;
      }
   } else if (!cmd->state.gfx.mesh_native_batch) {
      /* A native task group emits at most KK_MESH_NATIVE_GROUPS children.
       * Reserve the worst native slice count once for command-buffer reuse. */
      uint32_t minimum_span = KK_MESH_MAX_PHYSICAL_GROUPS / KK_MESH_NATIVE_GROUPS;
      uint32_t maximum = DIV_ROUND_UP(KK_MESH_MAX_GROUPS, minimum_span);
      struct kk_ptr p = kk_pool_alloc(cmd, sizeof(struct kk_mesh_batch) +
         maximum * 3u * sizeof(uint32_t), 8);
      if (!p.gpu)
         return false;
      cmd->state.gfx.mesh_native_batch = p.gpu;
      cmd->state.gfx.mesh_native_slices = p.gpu + sizeof(struct kk_mesh_batch);
   }
   return true;
}

static void
kk_draw_mesh_sliced(struct kk_cmd_buffer *cmd, struct mtl_size direct,
                       uint64_t indirect, uint64_t count, uint32_t draw_count,
                       uint32_t stride, bool virtual, uint32_t span,
                       uint32_t slice_count)
{
   struct kk_shader *shader = cmd->state.gfx.shader;
   struct kk_pipeline_handles *pipe;
   bool retained = kk_mesh_retained_active(cmd);
   VkResult result = retained
      ? kk_mesh_get_retained_pipeline(kk_cmd_buffer_device(cmd), shader, false, &pipe)
      : kk_mesh_get_sliced_pipeline(kk_cmd_buffer_device(cmd), shader, virtual, &pipe);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }
   kk_flush_gfx_state(cmd);
   mtl_argument_table *table = kk_mesh_table(cmd);
   if (!table || !kk_mesh_sliced_scratch(cmd, virtual))
      return;
   if (retained) {
      uint64_t cache_bytes = MAX2(KK_MESH_RETAINED_BYTES, kk_mesh_retained_stride(&shader->mesh->info));
      if (cmd->state.gfx.mesh_retained_output_bytes < cache_bytes) {
         /* The target is one bounded working slice. A record larger than the
          * target gets one full record; no output/dispatch is clipped. Old pool
          * allocations retain their normal command-retirement lifetime. */
         struct kk_ptr cache = kk_pool_alloc(cmd, cache_bytes, 32);
         if (!cache.gpu) return;
         cmd->state.gfx.mesh_retained_outputs = cache.gpu;
         cmd->state.gfx.mesh_retained_output_bytes = cache_bytes;
      }
   }
   bool canonical = shader->mesh->info.flat_heap;
   uint32_t task_limit = kk_mesh_canonical_task_limit(&shader->mesh->info, kk_mesh_retained_active(cmd));
   uint64_t slice_grids = virtual ? cmd->state.gfx.mesh_slices :
                                   cmd->state.gfx.mesh_native_slices;
   if (canonical || retained) {
      struct kk_ptr p = kk_pool_alloc(cmd, (uint64_t)slice_count * 3u * sizeof(uint32_t), 8);
      if (!p.gpu)
         return;
      slice_grids = p.gpu;
   }
   uint64_t groups = indirect ? KK_MESH_MAX_GROUPS : (uint64_t)direct.x * direct.y * direct.z;
   if (!indirect) {
      uint32_t grid[] = {direct.x, direct.y, direct.z};
      indirect = kk_pool_upload(cmd, grid, sizeof(grid), 4).gpu;
      if (!indirect)
         return;
   }
   /* These immutable slice parameters are shared by every task batch of a
    * logical draw. Only the task pre-pass receives a changing task_base. */
   struct kk_mesh_params single_parameters;
   struct kk_mesh_params *parameters = slice_count == 1 ? &single_parameters :
      malloc((size_t)slice_count * sizeof(*parameters));
   if (!parameters) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   for (uint32_t draw = 0; draw < draw_count; ++draw) {
      if (!kk_mesh_bind_draw(cmd, table, draw))
         break;
      struct kk_mesh_params p = kk_mesh_parameters(cmd,
         indirect + (uint64_t)draw * stride, count, draw, draw_count);
      p.batch = virtual ? cmd->state.gfx.mesh_batch : cmd->state.gfx.mesh_native_batch;
      p.records = cmd->state.gfx.mesh_records;
      p.payloads = cmd->state.gfx.mesh_payloads;
      p.payload_stride = virtual ? 16384u : span;
      if ((canonical || retained) && virtual) {
         p.pad = span;
         p.max_draw_count = task_limit;
      }
      for (uint32_t i = 0; i < slice_count; ++i) {
         parameters[i] = p;
         if (retained) parameters[i].indirect_count = cmd->state.gfx.mesh_retained_outputs;
         if (virtual)
            parameters[i].pad = i * ((canonical || retained) ? span : KK_MESH_MAX_PHYSICAL_GROUPS);
         else
            parameters[i].task_base = i * span;
      }
      uint64_t slice_params = kk_pool_upload(cmd, parameters,
         slice_count * sizeof(*parameters), 8).gpu;
      if (!slice_params)
         break;
      if (!virtual) {
         if (!kk_mesh_begin_batch(cmd, canonical ? 0 : slice_count, false))
            break;
         uint64_t args = kk_pool_upload(cmd, &p, sizeof(p), 8).gpu;
         if (!args)
            break;
         libkk_mesh_filter_slices(cmd, kk_grid_1d(1), true, args,
            slice_grids, slice_count);
         kk_mesh_render_slices(cmd, pipe, table, slice_grids,
                                slice_params, slice_count, false, draw, 0, span, groups);
         continue;
      }
      uint32_t batches = DIV_ROUND_UP(groups, task_limit);
      for (uint32_t batch = 0; batch < batches; ++batch) {
         if (!kk_mesh_begin_batch(cmd, canonical ? 0 : slice_count, true))
            goto finish;
         p.task_base = batch * task_limit;
         struct kk_ptr args = kk_pool_upload(cmd, &p, sizeof(p), 8);
         if (!args.gpu)
            goto finish;
         mtl_compute_encoder *enc = cs_get_compute(cmd);
         kk_cmd_bind_compute_arguments(cmd, enc, cmd->argument_table);
         libkk_mesh_prepare(cmd, kk_grid_1d(1), true, args.gpu);
         mtl_set_address(table, cmd->state.gfx.descriptors.root.addr, KK_ARGUMENT_ROOT);
         mtl_set_address(table, args.gpu, KK_ARGUMENT_MESH_PARAMETERS);
         kk_cmd_bind_compute_arguments(cmd, enc, table);
         mtl_compute_pipeline_state *task_pipeline = kk_cmd_sampler_compute_pipeline(
            cmd, shader, pipe->gfx.pre_render[0]);
         if (!task_pipeline) goto finish;
         mtl_compute_set_pipeline_state(enc, task_pipeline);
         mtl_dispatch_threadgroups_with_indirect_buffer(enc,
            p.batch + offsetof(struct kk_mesh_batch, task_dispatch), shader->mesh->info.task_local_size);
         kk_cmd_bind_compute_arguments(cmd, enc, cmd->argument_table);
         libkk_mesh_prefix_slices(cmd, kk_grid_1d(1), true, args.gpu,
            slice_grids, slice_count);
         uint64_t tasks = MIN2(groups - p.task_base, (uint64_t)task_limit);
         kk_mesh_render_slices(cmd, pipe, table, slice_grids,
                                slice_params, slice_count, true, draw, p.task_base, span,
                                tasks * MAX2(shader->mesh->info.task_grid_bound, 1u));
      }
   }
finish:
   if (parameters != &single_parameters)
      free(parameters);
}

static void
kk_draw_mesh_one_view(struct kk_cmd_buffer *cmd, struct mtl_size direct,
                uint64_t indirect, uint64_t count, uint32_t draw_count,
                uint32_t stride, bool force_virtual)
{
   const struct kk_mesh_shader_info *info = &cmd->state.gfx.shader->mesh->info;
   bool retained = kk_mesh_retained_active(cmd);
   if (retained && info->has_task) force_virtual = true;
   uint32_t physical_span = kk_mesh_canonical_span(cmd);
   if (info->flat_heap && info->has_task &&
       info->task_grid_bound > physical_span)
      force_virtual = true;
   bool virtual = info->has_task &&
      (force_virtual || info->task_grid_bound > KK_MESH_NATIVE_GROUPS);
   uint64_t groups = indirect ? KK_MESH_MAX_GROUPS : (uint64_t)direct.x * direct.y * direct.z;
   uint32_t span = physical_span;
   uint32_t slices;
   if (virtual) {
      uint32_t tasks = MIN2(groups, kk_mesh_canonical_task_limit(info, retained));
      uint32_t maximum = tasks * info->task_grid_bound;
      uint32_t width = MIN2(maximum, KK_MESH_MAX_DIMENSION);
      uint64_t physical = width ? (uint64_t)width * DIV_ROUND_UP(maximum, width) : 0;
      if (!retained && physical <= physical_span) {
         kk_draw_mesh_unsliced(cmd, direct, indirect, count, draw_count, stride, force_virtual);
         return;
      }
      /* An all-zero child grid still executes the original Task body and
       * its side effects/queries. Keep one GPU-empty raster slice. */
      slices = MAX2(1u, DIV_ROUND_UP(maximum, physical_span));
   } else {
      if (info->has_task)
         span /= MAX2(info->task_grid_bound, 1u);
      if (!retained && !indirect && groups <= span) {
         kk_draw_mesh_unsliced(cmd, direct, indirect, count, draw_count, stride, force_virtual);
         return;
      }
      slices = DIV_ROUND_UP(groups, span);
   }
   kk_draw_mesh_sliced(cmd, direct, indirect, count, draw_count, stride,
                         virtual, span, slices);
}

/* Original-view execution belongs here, outside attachment alias repetition.
 * Once per selected Vulkan view, never once per alias layer. Effective Layer
 * is that actual ViewIndex; application Layer is not added to it. */
static void
kk_draw_mesh(struct kk_cmd_buffer *cmd, struct mtl_size direct,
   uint64_t indirect, uint64_t count, uint32_t draw_count,
   uint32_t stride, bool force_virtual)
{
   if (cmd->state.gfx.render.layered_bc) {
      if (!kk_layered_bc_prepare_draw(cmd)) return;
      kk_flush_gfx_state(cmd);
      if (!kk_mesh_layer_replay_begin(cmd, cmd->state.gfx.render.layer_count,
            kk_layered_bc_bind_layer, &cmd->state.gfx.render))
         goto layered_bc_mesh_error;
   }

   const uint32_t mask = cmd->state.gfx.render.view_mask;
   if (!mask) {
      kk_draw_mesh_one_view(cmd, direct, indirect, count, draw_count, stride, force_virtual);
      goto layered_bc_mesh_done;
   }
   uint64_t saved_view = cmd->state.gfx.per_draw_data.multiview_index;
   u_foreach_bit(view, mask) {
      cmd->state.gfx.render.view_mask = 1u << view;
      cmd->state.gfx.per_draw_data.multiview_index = view;
      kk_draw_mesh_one_view(cmd, direct, indirect, count, draw_count, stride, force_virtual);
      if (cmd->vk.record_result != VK_SUCCESS) break;
   }
   cmd->state.gfx.render.view_mask = mask;
   cmd->state.gfx.per_draw_data.multiview_index = saved_view;

   goto layered_bc_mesh_done;
layered_bc_mesh_error:
   vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
layered_bc_mesh_done:
   if (cmd->state.gfx.render.layered_bc && cmd->state.gfx.layer_replay.bind)
      kk_mesh_layer_replay_end(cmd);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawMeshTasksEXT(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y, uint32_t z)
{
   if (!(x && y && z)) return;
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   kk_draw_mesh(cmd, (struct mtl_size){x, y, z}, 0, 0, 1, 0, false);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawMeshTasksIndirectEXT(VkCommandBuffer commandBuffer, VkBuffer buffer,
   VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
   if (!drawCount) return;
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(kk_buffer, b, buffer);
   kk_draw_mesh(cmd, (struct mtl_size){0}, vk_buffer_address(&b->vk, offset), 0,
      drawCount, stride, false);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer commandBuffer, VkBuffer buffer,
   VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset,
   uint32_t maxDrawCount, uint32_t stride)
{
   if (!maxDrawCount) return;
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(kk_buffer, b, buffer);
   VK_FROM_HANDLE(kk_buffer, c, countBuffer);
   kk_draw_mesh(cmd, (struct mtl_size){0}, vk_buffer_address(&b->vk, offset),
      vk_buffer_address(&c->vk, countOffset), maxDrawCount, stride, false);
}

/* Installed only on a device that enabled software bounds. All devices that
 * do not use the feature retain the original Mesh entry points above. */
static int
kk_depth_bounds_mesh_begin(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   kk_depth_bounds_route(cmd);
   if (!cmd->state.gfx.shader->info.depth_bounds_emulation)
      return 0;
   if (!kk_mesh_retained_transaction_begin(cmd, kk_depth_bounds_record_mesh_batch, NULL)) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INITIALIZATION_FAILED);
      return -1;
   }
   return 1;
}

static void VKAPI_CALL
kk_depth_bounds_draw_mesh(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y, uint32_t z)
{
   if (!(x && y && z)) return;
   int selected = kk_depth_bounds_mesh_begin(commandBuffer);
   if (selected < 0) return;
   kk_CmdDrawMeshTasksEXT(commandBuffer, x, y, z);
   if (selected) kk_mesh_retained_transaction_end(kk_cmd_buffer_from_handle(commandBuffer));
}

static void VKAPI_CALL
kk_depth_bounds_draw_mesh_indirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
   VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
   if (!drawCount) return;
   int selected = kk_depth_bounds_mesh_begin(commandBuffer);
   if (selected < 0) return;
   kk_CmdDrawMeshTasksIndirectEXT(commandBuffer, buffer, offset, drawCount, stride);
   if (selected) kk_mesh_retained_transaction_end(kk_cmd_buffer_from_handle(commandBuffer));
}

static void VKAPI_CALL
kk_depth_bounds_draw_mesh_indirect_count(VkCommandBuffer commandBuffer, VkBuffer buffer,
   VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset,
   uint32_t maxDrawCount, uint32_t stride)
{
   if (!maxDrawCount) return;
   int selected = kk_depth_bounds_mesh_begin(commandBuffer);
   if (selected < 0) return;
   kk_CmdDrawMeshTasksIndirectCountEXT(commandBuffer, buffer, offset,
      countBuffer, countOffset, maxDrawCount, stride);
   if (selected) kk_mesh_retained_transaction_end(kk_cmd_buffer_from_handle(commandBuffer));
}

/* Vertex-pipeline draws select the ordinary or transaction shaders of a
 * dynamic-enable pipeline before the draw reads any bound shader. */
#define KK_DEPTH_BOUNDS_DRAW(name, params, args) \
static void VKAPI_CALL \
kk_depth_bounds_##name params \
{ \
   kk_depth_bounds_route(kk_cmd_buffer_from_handle(commandBuffer)); \
   kk_##name args; \
}
KK_DEPTH_BOUNDS_DRAW(CmdDraw, (VkCommandBuffer commandBuffer, uint32_t vertexCount,
   uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance),
   (commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance))
KK_DEPTH_BOUNDS_DRAW(CmdDrawMultiEXT, (VkCommandBuffer commandBuffer, uint32_t drawCount,
   const VkMultiDrawInfoEXT *pVertexInfo, uint32_t instanceCount, uint32_t firstInstance,
   uint32_t stride),
   (commandBuffer, drawCount, pVertexInfo, instanceCount, firstInstance, stride))
KK_DEPTH_BOUNDS_DRAW(CmdDrawIndexed, (VkCommandBuffer commandBuffer, uint32_t indexCount,
   uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance),
   (commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance))
KK_DEPTH_BOUNDS_DRAW(CmdDrawMultiIndexedEXT, (VkCommandBuffer commandBuffer, uint32_t drawCount,
   const VkMultiDrawIndexedInfoEXT *pIndexInfo, uint32_t instanceCount, uint32_t firstInstance,
   uint32_t stride, const int32_t *pVertexOffset),
   (commandBuffer, drawCount, pIndexInfo, instanceCount, firstInstance, stride, pVertexOffset))
KK_DEPTH_BOUNDS_DRAW(CmdDrawIndirect2KHR, (VkCommandBuffer commandBuffer,
   const VkDrawIndirect2InfoKHR *pInfo), (commandBuffer, pInfo))
KK_DEPTH_BOUNDS_DRAW(CmdDrawIndexedIndirect2KHR, (VkCommandBuffer commandBuffer,
   const VkDrawIndirect2InfoKHR *pInfo), (commandBuffer, pInfo))
KK_DEPTH_BOUNDS_DRAW(CmdDrawIndirectCount2KHR, (VkCommandBuffer commandBuffer,
   const VkDrawIndirectCount2InfoKHR *pInfo), (commandBuffer, pInfo))
KK_DEPTH_BOUNDS_DRAW(CmdDrawIndexedIndirectCount2KHR, (VkCommandBuffer commandBuffer,
   const VkDrawIndirectCount2InfoKHR *pInfo), (commandBuffer, pInfo))
KK_DEPTH_BOUNDS_DRAW(CmdDrawIndirectByteCount2EXT, (VkCommandBuffer commandBuffer,
   uint32_t instanceCount, uint32_t firstInstance,
   const VkBindTransformFeedbackBuffer2InfoEXT *counter, uint32_t counterOffset,
   uint32_t vertexStride),
   (commandBuffer, instanceCount, firstInstance, counter, counterOffset, vertexStride))
#undef KK_DEPTH_BOUNDS_DRAW

/* Generated commands draw with the shaders bound when they are preprocessed
 * and executed, so both select from the command buffer's enable first. */
static void VKAPI_CALL
kk_depth_bounds_CmdPreprocessGeneratedCommandsEXT(VkCommandBuffer commandBuffer,
   const VkGeneratedCommandsInfoEXT *info, VkCommandBuffer state)
{
   kk_depth_bounds_route(kk_cmd_buffer_from_handle(state));
   kk_CmdPreprocessGeneratedCommandsEXT(commandBuffer, info, state);
}

static void VKAPI_CALL
kk_depth_bounds_CmdExecuteGeneratedCommandsEXT(VkCommandBuffer commandBuffer,
   VkBool32 preprocessed, const VkGeneratedCommandsInfoEXT *info)
{
   kk_depth_bounds_route(kk_cmd_buffer_from_handle(commandBuffer));
   kk_CmdExecuteGeneratedCommandsEXT(commandBuffer, preprocessed, info);
}

void
kk_depth_bounds_init_dispatch(struct kk_device *dev)
{
#define KK_DEPTH_BOUNDS_ENTRY(name) dev->vk.dispatch_table.name = kk_depth_bounds_##name;
   KK_DEPTH_BOUNDS_ENTRY(CmdDraw) KK_DEPTH_BOUNDS_ENTRY(CmdDrawMultiEXT)
   KK_DEPTH_BOUNDS_ENTRY(CmdDrawIndexed) KK_DEPTH_BOUNDS_ENTRY(CmdDrawMultiIndexedEXT)
   KK_DEPTH_BOUNDS_ENTRY(CmdDrawIndirect2KHR) KK_DEPTH_BOUNDS_ENTRY(CmdDrawIndexedIndirect2KHR)
   KK_DEPTH_BOUNDS_ENTRY(CmdDrawIndirectCount2KHR)
   KK_DEPTH_BOUNDS_ENTRY(CmdDrawIndexedIndirectCount2KHR)
   KK_DEPTH_BOUNDS_ENTRY(CmdDrawIndirectByteCount2EXT)
   KK_DEPTH_BOUNDS_ENTRY(CmdPreprocessGeneratedCommandsEXT)
   KK_DEPTH_BOUNDS_ENTRY(CmdExecuteGeneratedCommandsEXT)
#undef KK_DEPTH_BOUNDS_ENTRY
   dev->vk.dispatch_table.CmdDrawMeshTasksEXT = kk_depth_bounds_draw_mesh;
   dev->vk.dispatch_table.CmdDrawMeshTasksIndirectEXT = kk_depth_bounds_draw_mesh_indirect;
   dev->vk.dispatch_table.CmdDrawMeshTasksIndirectCountEXT = kk_depth_bounds_draw_mesh_indirect_count;
}

#endif

/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_PASS_PREPASS_H
#define KK_MESH_PASS_PREPASS_H
#include "kk_debug.h"
#include "vk_cmd_queue.h"
#include "vk_pipeline.h"
#include "vk_render_pass.h"

/* The budget bounds a single GPU input packet, independent of child grids.
 * Larger input schedules retain the existing live continuation. */
#define KK_MESH_PASS_INPUTS 65536u
struct kk_mesh_pass_call {
   struct mtl_size direct;
   uint64_t grid, count, predicate, source_grid;
   uint32_t maximum, stride, actual, captured, inverted, first;
   bool known;
   uint32_t *grids;
};
struct kk_mesh_pass_span { uint32_t call, base, count, first; };
struct kk_mesh_pass {
   struct kk_mesh_continuation saved;
   struct vk_cmd_queue commands;
   struct vk_cmd_queue_entry *entries;
   struct kk_mesh_pass_call *calls;
   struct kk_ptr inputs, parameters, gpu_batches, gpu_grids;
   struct kk_mesh_pass_span *spans, *initial_spans;
   uint32_t span_count, initial_span_count;
   uint32_t entry_count, call_count, cursor, input_count;
   bool uses_occlusion_queries, gpu_only;
};

/* Only CPU state setters may be evaluated ahead of the captured draw span.
 * GPU writes, barriers, nested render scopes and unknown operations remain
 * in their original execution order through the ordinary path. */
static bool
kk_mesh_pass_state_command(enum vk_cmd_type type)
{
   switch (type) {
   case VK_CMD_BIND_PIPELINE: case VK_CMD_BIND_SHADERS_EXT:
   case VK_CMD_BIND_DESCRIPTOR_SETS: case VK_CMD_BIND_DESCRIPTOR_SETS2:
   case VK_CMD_PUSH_CONSTANTS: case VK_CMD_PUSH_CONSTANTS2:
   case VK_CMD_PUSH_DESCRIPTOR_SET: case VK_CMD_PUSH_DESCRIPTOR_SET2:
   case VK_CMD_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE:
   case VK_CMD_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE2:
   case VK_CMD_PUSH_DATA_EXT:
   case VK_CMD_BIND_INDEX_BUFFER: case VK_CMD_BIND_INDEX_BUFFER2:
   case VK_CMD_BIND_INDEX_BUFFER3_KHR:
   case VK_CMD_BIND_VERTEX_BUFFERS: case VK_CMD_BIND_VERTEX_BUFFERS2:
   case VK_CMD_BIND_VERTEX_BUFFERS3_KHR:
   case VK_CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS_EXT:
   case VK_CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS2_EXT:
   case VK_CMD_SET_PRIMITIVE_TOPOLOGY:
   case VK_CMD_SET_PRIMITIVE_RESTART_INDEX_EXT:
   case VK_CMD_SET_VERTEX_INPUT_EXT: case VK_CMD_SET_DEVICE_MASK:
   case VK_CMD_SET_SAMPLE_LOCATIONS_EXT: case VK_CMD_SET_SAMPLE_LOCATIONS_ENABLE_EXT:
   case VK_CMD_SET_ALPHA_TO_ONE_ENABLE_EXT:
   case VK_CMD_SET_PATCH_CONTROL_POINTS_EXT: case VK_CMD_SET_TESSELLATION_DOMAIN_ORIGIN_EXT:
   case VK_CMD_SET_RASTERIZATION_STREAM_EXT:
   case VK_CMD_SET_CONSERVATIVE_RASTERIZATION_MODE_EXT:
   case VK_CMD_SET_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT:
   case VK_CMD_SET_LINE_STIPPLE: case VK_CMD_SET_LINE_STIPPLE_ENABLE_EXT:
   case VK_CMD_SET_LINE_RASTERIZATION_MODE_EXT:
   case VK_CMD_SET_DEPTH_CLAMP_RANGE_EXT:
   case VK_CMD_SET_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT:
   case VK_CMD_SET_RENDERING_ATTACHMENT_LOCATIONS:
   case VK_CMD_SET_RENDERING_INPUT_ATTACHMENT_INDICES:
   case VK_CMD_SET_FRAGMENT_SHADING_RATE_KHR:
   case VK_CMD_BIND_RESOURCE_HEAP_EXT: case VK_CMD_BIND_SAMPLER_HEAP_EXT:
   case VK_CMD_BIND_DESCRIPTOR_BUFFERS_EXT: case VK_CMD_SET_DESCRIPTOR_BUFFER_OFFSETS_EXT:
   case VK_CMD_SET_DESCRIPTOR_BUFFER_OFFSETS2_EXT:
   case VK_CMD_BIND_DESCRIPTOR_BUFFER_EMBEDDED_SAMPLERS_EXT:
   case VK_CMD_BIND_DESCRIPTOR_BUFFER_EMBEDDED_SAMPLERS2_EXT:
   case VK_CMD_SET_VIEWPORT: case VK_CMD_SET_VIEWPORT_WITH_COUNT:
   case VK_CMD_SET_SCISSOR: case VK_CMD_SET_SCISSOR_WITH_COUNT:
   case VK_CMD_SET_LINE_WIDTH: case VK_CMD_SET_DEPTH_BIAS:
   case VK_CMD_SET_BLEND_CONSTANTS: case VK_CMD_SET_DEPTH_BOUNDS:
   case VK_CMD_SET_STENCIL_COMPARE_MASK: case VK_CMD_SET_STENCIL_WRITE_MASK:
   case VK_CMD_SET_STENCIL_REFERENCE: case VK_CMD_SET_CULL_MODE:
   case VK_CMD_SET_FRONT_FACE: case VK_CMD_SET_DEPTH_TEST_ENABLE:
   case VK_CMD_SET_DEPTH_WRITE_ENABLE: case VK_CMD_SET_DEPTH_COMPARE_OP:
   case VK_CMD_SET_DEPTH_BOUNDS_TEST_ENABLE: case VK_CMD_SET_STENCIL_TEST_ENABLE:
   case VK_CMD_SET_STENCIL_OP: case VK_CMD_SET_RASTERIZER_DISCARD_ENABLE:
   case VK_CMD_SET_DEPTH_BIAS_ENABLE: case VK_CMD_SET_PRIMITIVE_RESTART_ENABLE:
   case VK_CMD_SET_RASTERIZATION_SAMPLES_EXT: case VK_CMD_SET_SAMPLE_MASK_EXT:
   case VK_CMD_SET_ALPHA_TO_COVERAGE_ENABLE_EXT:
   case VK_CMD_SET_COLOR_BLEND_ENABLE_EXT: case VK_CMD_SET_COLOR_BLEND_EQUATION_EXT:
   case VK_CMD_SET_COLOR_WRITE_MASK_EXT: case VK_CMD_SET_COLOR_WRITE_ENABLE_EXT:
   case VK_CMD_SET_LOGIC_OP_EXT: case VK_CMD_SET_LOGIC_OP_ENABLE_EXT:
   case VK_CMD_SET_POLYGON_MODE_EXT: case VK_CMD_SET_DEPTH_CLAMP_ENABLE_EXT:
   case VK_CMD_SET_PROVOKING_VERTEX_MODE_EXT: case VK_CMD_SET_DEPTH_CLIP_ENABLE_EXT:
   case VK_CMD_SET_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT:
   case VK_CMD_SET_DEPTH_BIAS2_EXT:
   case VK_CMD_BEGIN_CONDITIONAL_RENDERING_EXT:
   case VK_CMD_BEGIN_CONDITIONAL_RENDERING2_EXT:
   case VK_CMD_END_CONDITIONAL_RENDERING_EXT:
      return true;
   default: return false;
   }
}

static bool
kk_mesh_pass_call_info(const struct vk_cmd_queue_entry *entry,
   struct kk_mesh_pass_call *call)
{
   memset(call, 0, sizeof(*call));
   switch (entry->type) {
   case VK_CMD_DRAW_MESH_TASKS_EXT:
      call->direct = (struct mtl_size){entry->u.draw_mesh_tasks_ext.group_count_x,
         entry->u.draw_mesh_tasks_ext.group_count_y, entry->u.draw_mesh_tasks_ext.group_count_z};
      call->maximum = call->direct.x && call->direct.y && call->direct.z;
      return true;
   case VK_CMD_DRAW_MESH_TASKS_INDIRECT_EXT: {
      const struct vk_cmd_draw_mesh_tasks_indirect_ext *d = &entry->u.draw_mesh_tasks_indirect_ext;
      call->maximum = d->draw_count; call->stride = d->stride;
      if (call->maximum) call->grid = vk_buffer_address(&kk_buffer_from_handle(d->buffer)->vk, d->offset);
      return true;
   }
   case VK_CMD_DRAW_MESH_TASKS_INDIRECT_COUNT_EXT: {
      const struct vk_cmd_draw_mesh_tasks_indirect_count_ext *d = &entry->u.draw_mesh_tasks_indirect_count_ext;
      call->maximum = d->max_draw_count; call->stride = d->stride;
      if (call->maximum) {
         call->grid = vk_buffer_address(&kk_buffer_from_handle(d->buffer)->vk, d->offset);
         call->count = vk_buffer_address(&kk_buffer_from_handle(d->count_buffer)->vk, d->count_buffer_offset);
      }
      return true;
   }
   case VK_CMD_DRAW_MESH_TASKS_INDIRECT2_EXT: {
      const VkDrawIndirect2InfoKHR *d = entry->u.draw_mesh_tasks_indirect2_ext.info;
      call->maximum = d->drawCount; call->stride = d->addressRange.stride;
      call->grid = d->addressRange.address; return true;
   }
   case VK_CMD_DRAW_MESH_TASKS_INDIRECT_COUNT2_EXT: {
      const VkDrawIndirectCount2InfoKHR *d = entry->u.draw_mesh_tasks_indirect_count2_ext.info;
      call->maximum = d->maxDrawCount; call->stride = d->addressRange.stride;
      call->grid = d->addressRange.address; call->count = d->countAddressRange.address; return true;
   }
   default: return false;
   }
}

static bool
kk_mesh_pass_native_shader(const struct kk_shader *shader)
{
   return shader && shader->info.stage == MESA_SHADER_MESH &&
      !shader->info.interlock_envelope && !shader->info.depth_bounds_emulation &&
      !shader->mesh->info.flat_heap &&
      (!shader->mesh->info.has_task || shader->mesh->info.task_grid_bound <= KK_MESH_NATIVE_GROUPS);
}

static void
kk_mesh_pass_queue(struct vk_cmd_queue *queue, struct vk_cmd_queue_entry *entries, uint32_t count)
{
   memset(queue, 0, sizeof(*queue)); list_inithead(&queue->cmds);
   for (uint32_t i = 0; i < count; ++i) list_addtail(&entries[i].cmd_link, &queue->cmds);
}

/* Classify without invoking native methods or changing application state. */
static bool
kk_mesh_pass_eligible(struct kk_cmd_buffer *cmd, struct kk_mesh_pass *p)
{
   const struct kk_shader *mesh = cmd->state.shaders[MESA_SHADER_MESH];
   uint64_t inputs = 0; bool indirect = false, only_mesh = true, direct_conditional = false;
   bool conditional = cmd->state.cond_render.enabled; unsigned queries = 0;
   if (cmd->metal.render) return false;
   if (p->entries[0].type == VK_CMD_BEGIN_RENDERING) {
      if (p->entries[0].u.begin_rendering.rendering_info->flags &
          (VK_RENDERING_RESUMING_BIT | VK_RENDERING_SUSPENDING_BIT)) return false;
   } else {
      const VkRenderPassBeginInfo *begin = p->entries[0].type == VK_CMD_BEGIN_RENDER_PASS2 ?
         p->entries[0].u.begin_render_pass2.render_pass_begin : p->entries[0].u.begin_render_pass.render_pass_begin;
      VK_FROM_HANDLE(vk_render_pass,pass,begin->renderPass);
      if (pass->subpass_count != 1) return false;
   }
   for (uint32_t i = 1; i + 1 < p->entry_count; ++i) {
      const struct vk_cmd_queue_entry *e = &p->entries[i];
      if (e->type == VK_CMD_BIND_SHADERS_EXT) {
         const struct vk_cmd_bind_shaders_ext *b = &e->u.bind_shaders_ext;
         for (uint32_t s = 0; s < b->stage_count; ++s)
            if (b->stages[s] == VK_SHADER_STAGE_MESH_BIT_EXT) mesh = kk_shader_from_handle(b->shaders[s]);
      } else if (e->type == VK_CMD_BIND_PIPELINE) {
         VK_FROM_HANDLE(vk_pipeline, pipeline, e->u.bind_pipeline.pipeline);
         const struct vk_shader *shader = vk_pipeline_get_shader(pipeline, MESA_SHADER_MESH);
         mesh = shader ? container_of(shader, struct kk_shader, vk) : NULL;
      }
      if (e->type == VK_CMD_BEGIN_CONDITIONAL_RENDERING_EXT ||
          e->type == VK_CMD_BEGIN_CONDITIONAL_RENDERING2_EXT) conditional = true;
      else if (e->type == VK_CMD_END_CONDITIONAL_RENDERING_EXT) conditional = false;
      struct kk_mesh_pass_call call;
      if (kk_mesh_pass_call_info(e, &call)) {
         if (!call.maximum) continue;
         if (!kk_mesh_pass_native_shader(mesh)) return false;
         inputs += call.maximum; indirect |= call.grid != 0;
         only_mesh &= !mesh->mesh->info.has_task;
         direct_conditional |= !call.grid && conditional;

      } else if (e->type == VK_CMD_CLEAR_ATTACHMENTS) {
         /* Attachment writes do not publish indirect buffers. */
      } else if (e->type == VK_CMD_BEGIN_QUERY || e->type == VK_CMD_BEGIN_QUERY_INDEXED_EXT) {
         ++queries;
      } else if (e->type == VK_CMD_END_QUERY || e->type == VK_CMD_END_QUERY_INDEXED_EXT) {
         if (!queries--) return false;
      } else if (e->type == VK_CMD_BEGIN_DEBUG_UTILS_LABEL_EXT ||
                 e->type == VK_CMD_END_DEBUG_UTILS_LABEL_EXT ||
                 e->type == VK_CMD_INSERT_DEBUG_UTILS_LABEL_EXT) {
      } else if (!kk_mesh_pass_state_command(e->type)) return false;
   }
   p->gpu_only = only_mesh && !direct_conditional && inputs <= 32;
   p->input_count = MIN2(inputs, (uint64_t)KK_MESH_PASS_INPUTS);
   return indirect && inputs && !queries;
}

static void
kk_mesh_pass_free(struct kk_mesh_pass *p)
{
   free(p->entries); free(p->calls); free(p->spans); free(p->initial_spans); free(p);
}

static void
kk_mesh_pass_finish(struct kk_device *dev, void *data)
{
   struct kk_mesh_pass *p = data;
   for (uint32_t i=0;i<p->call_count;++i) free(p->calls[i].grids);
   free(p->entries); free(p->calls); free(p->spans); free(p->initial_spans);
   /* saved is the first member and owns the same pool/state lifetime. */
   kk_mesh_continuation_finish(dev, &p->saved);
}

/* Fill one bounded packet. Large max-count calls first probe one record,
 * then capture only their published real count; no worst-case input schedule. */
static uint32_t
kk_mesh_pass_fill(struct kk_mesh_pass *p)
{
   uint32_t count=0; p->span_count=0;
   for (uint32_t i=0;i<p->call_count && count<p->input_count;++i) {
      const struct kk_mesh_pass_call *call=&p->calls[i];
      uint32_t limit=call->known?call->actual:call->maximum;
      if (call->captured>=limit) continue;
      uint32_t n=!call->known && limit>p->input_count?1:
         MIN2(limit-call->captured,p->input_count-count);
      p->spans[p->span_count++]=(struct kk_mesh_pass_span){i,call->captured,n,count};
      for (uint32_t d=0;d<n;++d) {
         uint32_t id=call->captured+d;
         ((struct kk_mesh_params *)p->parameters.cpu)[count++]=(struct kk_mesh_params){
            .indirect_grid=call->source_grid+(uint64_t)id*call->stride,
            .indirect_count=call->count,.draw_id=id,.max_draw_count=call->maximum,
            .predicate=call->predicate,.predicate_inverted=call->inverted,
         };
      }
   }
   return count;
}

static bool
kk_mesh_pass_accept_inputs(struct kk_mesh_pass *p)
{
   for (uint32_t i=0;i<p->span_count;++i) {
      const struct kk_mesh_pass_span *span=&p->spans[i];
      struct kk_mesh_pass_call *call=&p->calls[span->call];
      const uint32_t *data=(const uint32_t *)p->inputs.cpu+span->first*4u;
      if (!call->known) {
         call->actual=MIN2(call->maximum,data[3]); call->known=true;
         if (call->actual) {
            call->grids=malloc((uint64_t)call->actual*4u*sizeof(uint32_t));
            if (!call->grids) return false;
         }
      }
      uint32_t n=call->actual>span->base?MIN2(span->count,call->actual-span->base):0;
      if (n) memcpy(call->grids+(uint64_t)span->base*4u,data,(uint64_t)n*4u*sizeof(uint32_t));
      call->captured+=n;
   }
   return true;
}

static void kk_mesh_pass_inputs_complete(struct mtl_feedback_data *);
static bool kk_mesh_pass_advance(struct kk_mesh_pass *);

static void
kk_mesh_pass_reset(struct kk_mesh_pass *p)
{
   /* A reusable Vulkan recording captures new GPU inputs on every submit.
    * Its initial parameter packet is immutable; only streaming state resets. */
   for (uint32_t i=0;i<p->call_count;++i) {
      free(p->calls[i].grids); p->calls[i].grids=NULL;
      p->calls[i].actual=p->calls[i].captured=0; p->calls[i].known=false;
   }
   p->span_count=p->initial_span_count;
   memcpy(p->spans,p->initial_spans,(size_t)p->span_count*sizeof(*p->spans));
}

static bool
kk_mesh_pass_encode(void *data, mtl_command_queue *queue,
   mtl_feedback_handler_callback completed, void *completed_data)
{
   struct kk_mesh_pass *p=data;
   p->saved.queue=queue; p->saved.completed=completed; p->saved.completed_data=completed_data;
   kk_mesh_pass_reset(p);
   return kk_mesh_pass_advance(p);
}

static void
kk_mesh_pass_inputs_complete(struct mtl_feedback_data *feedback)
{
   struct kk_mesh_pass *p=feedback->user_data;
   if (!feedback->error && !vk_device_is_lost(&p->saved.device->vk) && kk_mesh_pass_advance(p)) return;
   struct mtl_feedback_data final=*feedback; final.user_data=p->saved.completed_data;
   if (!final.error) final.error=MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY;
   p->saved.completed(&final);
}

static bool
kk_mesh_pass_advance(struct kk_mesh_pass *p)
{
   struct kk_cmd_buffer *cmd = p->saved.cmd;
   struct kk_device *dev = p->saved.device;
   if (vk_device_is_lost(&dev->vk) || !kk_mesh_pass_accept_inputs(p)) return false;
   uint32_t next=kk_mesh_pass_fill(p);
   VkResult result = kk_BeginCommandBuffer(kk_cmd_buffer_to_handle(cmd),
      &(VkCommandBufferBeginInfo){.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT});
   if (result != VK_SUCCESS) return false;
   if (next) {
      libkk_mesh_snapshot_batch(cmd,kk_grid_1d(next),true,p->parameters.gpu,p->inputs.gpu,next);
      if (kk_EndCommandBuffer(kk_cmd_buffer_to_handle(cmd))!=VK_SUCCESS) return false;
      kk_device_make_resources_resident(dev);
      mtl_commit_options *options=mtl_new_commit_options(); if (!options) return false;
      mtl_commit_options_add_feedback_handler(options,kk_mesh_pass_inputs_complete,p);
      mtl_command_queue_commit(p->saved.queue,cmd->native_submit.buffers,cmd->native_submit.count,options);
      mtl_release(options); return true;
   }
   cmd->uses_occlusion_queries = p->uses_occlusion_queries;
   cmd->state = p->saved.state;
   memset(&cmd->state.cs, 0, sizeof(cmd->state.cs)); cmd->state.rt = NULL;
   cmd->state.gfx.render_pass_descriptor = NULL;
   cmd->state.gfx.depth_stencil_state = NULL; cmd->state.gfx.is_depth_stencil_dynamic = false;
   memset(cmd->state.gfx.descriptors.push, 0, sizeof(cmd->state.gfx.descriptors.push));
   for (unsigned i=0;i<KK_MAX_SETS;++i) {
      const struct kk_push_descriptor_set *push = p->saved.state.gfx.descriptors.push[i];
      if (!push) continue;
      size_t size=sizeof(*push)+push->capacity;
      cmd->state.gfx.descriptors.push[i]=vk_alloc(&kk_cmd_buffer_pool(cmd)->vk.alloc,size,8,VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!cmd->state.gfx.descriptors.push[i]) return false;
      memcpy(cmd->state.gfx.descriptors.push[i],push,size);
   }
   cmd->vk.dynamic_graphics_state.vi=&cmd->state.gfx._dynamic_vi;
   cmd->vk.dynamic_graphics_state.ms.sample_locations=&cmd->state.gfx._dynamic_sl;
   vk_dynamic_graphics_state_copy(&cmd->vk.dynamic_graphics_state,&p->saved.dynamic);
   kk_cmd_buffer_dirty_all_gfx(cmd);
   p->cursor=0; cmd->mesh_pass=p;
   mtl_execute_command_queue(&p->commands,kk_cmd_buffer_to_handle(cmd),&dev->vk.dispatch_table);
   cmd->mesh_pass=NULL; cmd->mesh_prepared_grid=NULL;
   if (cmd->interlock_segments || p->cursor!=p->call_count || cmd->vk.record_result!=VK_SUCCESS)
      return false;
   if (kk_EndCommandBuffer(kk_cmd_buffer_to_handle(cmd))!=VK_SUCCESS) return false;
   kk_device_make_resources_resident(dev);
   mtl_commit_options *options=mtl_new_commit_options(); if (!options) return false;
   mtl_commit_options_add_feedback_handler(options,p->saved.completed,p->saved.completed_data);
   mtl_command_queue_commit(p->saved.queue,cmd->native_submit.buffers,cmd->native_submit.count,options);
   mtl_release(options);
   return true;
}

static void kk_mesh_pass_gpu_draw(struct kk_cmd_buffer *, struct kk_mesh_pass *,
   const struct kk_mesh_pass_call *);

static void
kk_mesh_pass_draw(struct kk_cmd_buffer *cmd, struct mtl_size direct,
   uint64_t grid, uint64_t count, uint32_t maximum, uint32_t stride, bool force_virtual)
{
   struct kk_mesh_pass *p=cmd->mesh_pass;
   if (p->cursor>=p->call_count) { vk_command_buffer_set_error(&cmd->vk,VK_ERROR_INITIALIZATION_FAILED); return; }
   const struct kk_mesh_pass_call *call=&p->calls[p->cursor++];
   if (call->grid!=grid || call->count!=count || call->maximum!=maximum ||
       call->stride!=stride || (grid==0 && (direct.x!=call->direct.x ||
       direct.y!=call->direct.y || direct.z!=call->direct.z))) {
      vk_command_buffer_set_error(&cmd->vk,VK_ERROR_INITIALIZATION_FAILED); return;
   }
   if (p->gpu_only) { kk_mesh_pass_gpu_draw(cmd,p,call); return; }
   const uint32_t *inputs=call->grids;
   uint32_t actual=call->actual;
   __typeof__(cmd->state.cond_render) conditional=cmd->state.cond_render;
   cmd->state.cond_render.enabled=false; /* Captured predicate already filtered. */
   for (uint32_t i=0;i<actual;++i) {
      const uint32_t *g=inputs+i*4u; if (!(g[0]&&g[1]&&g[2])) continue;
      cmd->mesh_prepared_grid=g; cmd->mesh_prepared_draw=i;
      kk_draw_mesh(cmd,(struct mtl_size){g[0],g[1],g[2]},0,0,1,0,force_virtual);
      if (cmd->vk.record_result!=VK_SUCCESS) break;
   }
   cmd->mesh_prepared_grid=NULL; cmd->state.cond_render=conditional;
}

static void
kk_mesh_pass_condition(struct kk_conditional_rendering_state *conditional,
   const struct vk_cmd_queue_entry *e)
{
   if (e->type == VK_CMD_BEGIN_CONDITIONAL_RENDERING2_EXT) {
      const VkConditionalRenderingBeginInfo2EXT *info=e->u.begin_conditional_rendering2_ext.conditional_rendering_begin;
      conditional->address=info->addressRange.address;
      conditional->inverted=!!(info->flags&VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT);
      conditional->enabled=true;
   } else if (e->type == VK_CMD_BEGIN_CONDITIONAL_RENDERING_EXT) {
      const VkConditionalRenderingBeginInfoEXT *info=e->u.begin_conditional_rendering_ext.conditional_rendering_begin;
      conditional->address=vk_buffer_address(&kk_buffer_from_handle(info->buffer)->vk,info->offset);
      conditional->inverted=!!(info->flags&VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT);
      conditional->enabled=true;
   } else if (e->type == VK_CMD_END_CONDITIONAL_RENDERING_EXT) conditional->enabled=false;
}

static bool
kk_mesh_pass_build_calls(struct kk_cmd_buffer *cmd, struct kk_mesh_pass *p, bool advance)
{
   struct kk_conditional_rendering_state conditional=cmd->state.cond_render;
   for (uint32_t i=1;i+1<p->entry_count;++i) {
      struct vk_cmd_queue_entry *e=&p->entries[i]; struct kk_mesh_pass_call call;
      if (kk_mesh_pass_call_info(e,&call)) {
         /* An ordinary direct draw has no GPU input work at all. */
         if (!call.maximum || (!call.grid && !conditional.enabled)) continue;
         call.source_grid=call.grid;
         if (!call.source_grid) {
            uint32_t g[]={call.direct.x,call.direct.y,call.direct.z};
            call.source_grid=kk_pool_upload(cmd,g,sizeof(g),4).gpu;
            if (!call.source_grid) return false;
         }
         call.predicate=conditional.enabled?conditional.address:0;
         call.inverted=conditional.inverted;
         p->calls[p->call_count++]=call;
      } else {
         kk_mesh_pass_condition(&conditional,e);
         if (advance && kk_mesh_pass_state_command(e->type)) {
            struct vk_cmd_queue_entry copy=*e; struct vk_cmd_queue state;
            kk_mesh_pass_queue(&state,&copy,1);
            vk_cmd_queue_execute(&state,kk_cmd_buffer_to_handle(cmd),&kk_cmd_buffer_device(cmd)->vk.dispatch_table);
         }
      }
   }
   return true;
}

static void
kk_mesh_pass_gpu_draw(struct kk_cmd_buffer *cmd, struct kk_mesh_pass *p,
   const struct kk_mesh_pass_call *call)
{
   struct kk_shader *shader=cmd->state.gfx.shader; struct kk_pipeline_handles *pipe;
   VkResult result=kk_mesh_get_sliced_pipeline(kk_cmd_buffer_device(cmd),shader,false,&pipe);
   if (result!=VK_SUCCESS) { vk_command_buffer_set_error(&cmd->vk,result); return; }
   uint32_t mask=cmd->state.gfx.render.view_mask, remaining=mask;
   uint64_t saved_view=cmd->state.gfx.per_draw_data.multiview_index;
   do {
      if (mask) {
         unsigned view=u_bit_scan(&remaining); cmd->state.gfx.render.view_mask=1u<<view;
         cmd->state.gfx.per_draw_data.multiview_index=view;
      }
      kk_flush_gfx_state(cmd); mtl_argument_table *table=kk_mesh_table(cmd); if (!table) break;
      for (uint32_t d=0;d<call->maximum;++d) {
         if (!kk_mesh_bind_draw(cmd,table,d)) break;
         struct kk_mesh_params params[2];
         params[0]=kk_mesh_parameters(cmd,0,0,d,call->maximum);
         params[0].predicate=0;
         params[0].batch=p->gpu_batches.gpu+(uint64_t)(call->first+d)*sizeof(struct kk_mesh_batch);
         params[0].payload_stride=KK_MESH_MAX_PHYSICAL_GROUPS;
         params[1]=params[0]; params[1].task_base=KK_MESH_MAX_PHYSICAL_GROUPS;
         uint64_t address=kk_pool_upload(cmd,params,sizeof(params),8).gpu;
         if (!address || !kk_mesh_begin_batch(cmd,2,false)) break;
         kk_mesh_render_slices_native(cmd,pipe,table,
            p->gpu_grids.gpu+(uint64_t)(call->first+d)*6u*sizeof(uint32_t),address,2,false);
      }
      if (cmd->vk.record_result!=VK_SUCCESS) break;
   } while (remaining);
   cmd->state.gfx.render.view_mask=mask;
   cmd->state.gfx.per_draw_data.multiview_index=saved_view;
}

static void
kk_mesh_pass_gpu_execute(struct kk_cmd_buffer *cmd, struct kk_mesh_pass *p)
{
   p->calls=calloc(p->entry_count,sizeof(*p->calls));
   if (!p->calls || !kk_mesh_pass_build_calls(cmd,p,false)) goto fail;
   p->parameters=kk_pool_alloc(cmd,(uint64_t)p->input_count*sizeof(struct kk_mesh_params),8);
   p->gpu_batches=kk_pool_alloc(cmd,(uint64_t)p->input_count*sizeof(struct kk_mesh_batch),8);
   p->gpu_grids=kk_pool_alloc(cmd,(uint64_t)p->input_count*6u*sizeof(uint32_t),4);
   if (!p->parameters.gpu || !p->gpu_batches.gpu || !p->gpu_grids.gpu) goto fail;
   uint32_t n=0;
   for (uint32_t i=0;i<p->call_count;++i) {
      struct kk_mesh_pass_call *call=&p->calls[i];call->first=n;
      for (uint32_t d=0;d<call->maximum;++d) {
         ((struct kk_mesh_params *)p->parameters.cpu)[n]=(struct kk_mesh_params){
            .indirect_grid=call->source_grid+(uint64_t)d*call->stride,
            .indirect_count=call->count,.draw_id=d,.max_draw_count=call->maximum,
            .predicate=call->predicate,.predicate_inverted=call->inverted,
            .batch=p->gpu_batches.gpu+(uint64_t)n*sizeof(struct kk_mesh_batch),
            .payload_stride=KK_MESH_MAX_PHYSICAL_GROUPS,
         };++n;
      }
   }
   if (KK_DEBUG(MSL))
      fprintf(stderr, "mesh pass prelude: route=gpu inputs=%u calls=%u\n", n, p->call_count);
   KK_CMD_BARRIER(cmd,mtl_barrier_after_encoder_stages_with_options,cs_get_compute(cmd),
      MTL_STAGE_ALL,MTL_STAGE_DISPATCH,MTL_VISIBILITY_RESOURCE_ALIAS);
   libkk_mesh_native_pass(cmd,kk_grid_1d(n),true,p->parameters.gpu,p->gpu_grids.gpu,n);
   p->cursor=0; cmd->mesh_pass=p;
   mtl_execute_command_queue(&p->commands,kk_cmd_buffer_to_handle(cmd),&kk_cmd_buffer_device(cmd)->vk.dispatch_table);
   cmd->mesh_pass=NULL;
   if (p->cursor!=p->call_count) vk_command_buffer_set_error(&cmd->vk,VK_ERROR_INITIALIZATION_FAILED);
   kk_mesh_pass_free(p); return;
fail:
   vk_command_buffer_set_error(&cmd->vk,VK_ERROR_OUT_OF_HOST_MEMORY);kk_mesh_pass_free(p);
}

static void
kk_mesh_pass_execute(VkCommandBuffer handle, void *data)
{
   struct kk_mesh_pass *p=data;
   VK_FROM_HANDLE(kk_cmd_buffer,cmd,handle);
   struct kk_device *dev=kk_cmd_buffer_device(cmd);
   if (!kk_mesh_pass_eligible(cmd,p)) {
      mtl_execute_command_queue(&p->commands,handle,&dev->vk.dispatch_table);
      kk_mesh_pass_free(p); return;
   }
   if (p->gpu_only) { kk_mesh_pass_gpu_execute(cmd,p); return; }
   p->saved.device=dev; p->saved.state=cmd->state;
   p->uses_occlusion_queries=cmd->uses_occlusion_queries;
   memset(p->saved.state.gfx.descriptors.push,0,sizeof(p->saved.state.gfx.descriptors.push));
   for (unsigned i=0;i<KK_MAX_SETS;++i) {
      const struct kk_push_descriptor_set *push=cmd->state.gfx.descriptors.push[i]; if (!push) continue;
      size_t size=sizeof(*push)+push->capacity;
      p->saved.state.gfx.descriptors.push[i]=malloc(size);
      if (!p->saved.state.gfx.descriptors.push[i]) goto fail;
      memcpy(p->saved.state.gfx.descriptors.push[i],push,size);
   }
   p->saved.dynamic=cmd->vk.dynamic_graphics_state;
   p->saved.dynamic.vi=&p->saved.state.gfx._dynamic_vi;
   p->saved.dynamic.ms.sample_locations=&p->saved.state.gfx._dynamic_sl;
   p->calls=calloc(p->entry_count,sizeof(*p->calls));
   p->spans=calloc(p->entry_count,sizeof(*p->spans));
   if (!p->calls || !p->spans) goto fail;
   p->parameters=kk_pool_alloc(cmd,(uint64_t)p->input_count*sizeof(struct kk_mesh_params),8);
   p->inputs=kk_pool_alloc(cmd,(uint64_t)p->input_count*4u*sizeof(uint32_t),8);
   if (!p->parameters.gpu || !p->inputs.gpu) goto fail;
   if (!kk_mesh_pass_build_calls(cmd,p,true)) goto fail;
   VkResult result=kk_CreateCommandPool(kk_device_to_handle(dev),
      &(VkCommandPoolCreateInfo){.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
       .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
       .queueFamilyIndex=kk_cmd_buffer_pool(cmd)->vk.queue_family_index},NULL,&p->saved.pool);
   if (result!=VK_SUCCESS) goto fail;
   struct vk_command_buffer *vk_cmd;
   result=kk_cmd_buffer_ops.create(&kk_cmd_pool_from_handle(p->saved.pool)->vk,VK_COMMAND_BUFFER_LEVEL_PRIMARY,&vk_cmd);
   if (result!=VK_SUCCESS) goto fail;
   p->saved.cmd=container_of(vk_cmd,struct kk_cmd_buffer,vk);
   uint32_t first=kk_mesh_pass_fill(p);
   p->initial_span_count=p->span_count;
   p->initial_spans=malloc((size_t)p->span_count*sizeof(*p->spans));
   if (!p->initial_spans) goto fail;
   memcpy(p->initial_spans,p->spans,(size_t)p->span_count*sizeof(*p->spans));
   uint64_t initial_parameters=kk_pool_upload(cmd,p->parameters.cpu,
      (uint64_t)first*sizeof(struct kk_mesh_params),8).gpu;
   if (!initial_parameters) goto fail;
   /* This also covers explicit legacy external dependencies: the prelude
    * reads after all preceding producers, before the first render encoder. */
   KK_CMD_BARRIER(cmd,mtl_barrier_after_encoder_stages_with_options,cs_get_compute(cmd),
      MTL_STAGE_ALL,MTL_STAGE_DISPATCH,MTL_VISIBILITY_RESOURCE_ALIAS);
   if (KK_DEBUG(MSL))
      fprintf(stderr, "mesh pass prelude: route=feedback inputs=%u calls=%u\n", first, p->call_count);
   libkk_mesh_snapshot_batch(cmd,kk_grid_1d(first),true,initial_parameters,p->inputs.gpu,first);
   result=kk_interlock_split(cmd,p,kk_mesh_pass_encode,kk_mesh_pass_finish);
   if (result==VK_SUCCESS) return;
fail:
   vk_command_buffer_set_error(&cmd->vk,VK_ERROR_OUT_OF_HOST_MEMORY);
   kk_mesh_pass_finish(dev,p);
}

/* Secondary queues retain their Vulkan lifetime. Copy only immutable IR nodes,
 * preserving every order and input pointer while leaving source lists intact. */
static bool
kk_mesh_pass_flatten(struct vk_cmd_queue *queue, struct util_dynarray *flat)
{
   list_for_each_entry(struct vk_cmd_queue_entry,e,&queue->cmds,cmd_link) {
      if (e->type==VK_CMD_EXECUTE_COMMANDS) {
         for (uint32_t i=0;i<e->u.execute_commands.command_buffer_count;++i) {
            VK_FROM_HANDLE(vk_command_buffer,secondary,e->u.execute_commands.command_buffers[i]);
            if (!kk_mesh_pass_flatten(&secondary->cmd_queue,flat)) return false;
         }
      } else {
         struct vk_cmd_queue_entry *out=util_dynarray_grow(flat,struct vk_cmd_queue_entry,1);
         if (!out) return false; *out=*e;
      }
   }
   return true;
}

void
kk_mesh_execute_command_queue(struct vk_cmd_queue *source,VkCommandBuffer handle,
   const struct vk_device_dispatch_table *dispatch)
{
   VK_FROM_HANDLE(kk_cmd_buffer,cmd,handle);
   struct kk_device *dev=kk_cmd_buffer_device(cmd);
   if (kk_device_physical(dev)->info.gpu_apple_family>=9 ||
       !dev->vk.enabled_features.meshShader || !cmd->uses_indirect_mesh || cmd->mesh_pass) {
      mtl_execute_command_queue(source,handle,dispatch); return;
   }
   struct util_dynarray flat=UTIL_DYNARRAY_INIT;
   if (!kk_mesh_pass_flatten(source,&flat)) {
      util_dynarray_fini(&flat); vk_command_buffer_set_error(&cmd->vk,VK_ERROR_OUT_OF_HOST_MEMORY); return;
   }
   uint32_t count=util_dynarray_num_elements(&flat,struct vk_cmd_queue_entry);
   struct vk_cmd_queue_entry *entries=util_dynarray_begin(&flat);
   for (uint32_t i=0;i<count;++i) {
      if (entries[i].type!=VK_CMD_BEGIN_RENDERING && entries[i].type!=VK_CMD_BEGIN_RENDER_PASS &&
          entries[i].type!=VK_CMD_BEGIN_RENDER_PASS2) continue;
      uint32_t end=i+1; bool indirect=false;
      for (;end<count;++end) {
         enum vk_cmd_type type=entries[end].type;
         if (type==VK_CMD_BEGIN_RENDERING || type==VK_CMD_BEGIN_RENDER_PASS || type==VK_CMD_BEGIN_RENDER_PASS2) break;
         if (type==VK_CMD_END_RENDERING || type==VK_CMD_END_RENDERING2_KHR ||
             type==VK_CMD_END_RENDER_PASS || type==VK_CMD_END_RENDER_PASS2) break;
         struct kk_mesh_pass_call call;
         indirect|=kk_mesh_pass_call_info(&entries[end],&call) && call.grid;
      }
      if (!indirect || end==count || entries[end].type==VK_CMD_BEGIN_RENDERING ||
          entries[end].type==VK_CMD_BEGIN_RENDER_PASS || entries[end].type==VK_CMD_BEGIN_RENDER_PASS2) continue;
      struct kk_mesh_pass *p=calloc(1,sizeof(*p));
      if (!p) { vk_command_buffer_set_error(&cmd->vk,VK_ERROR_OUT_OF_HOST_MEMORY); break; }
      p->entry_count=end-i+1; p->entries=malloc((size_t)p->entry_count*sizeof(*p->entries));
      if (!p->entries) { free(p); vk_command_buffer_set_error(&cmd->vk,VK_ERROR_OUT_OF_HOST_MEMORY); break; }
      memcpy(p->entries,entries+i,(size_t)p->entry_count*sizeof(*p->entries));
      kk_mesh_pass_queue(&p->commands,p->entries,p->entry_count);
      entries[i].type=VK_CMD_DRIVER; entries[i].u.driver.data=p; entries[i].u.driver.execute=kk_mesh_pass_execute;
      /* Compact replaced nodes; following scans observe unchanged command order. */
      memmove(entries+i+1,entries+end+1,(size_t)(count-end-1)*sizeof(*entries));
      count-=end-i;
   }
   struct vk_cmd_queue prepared; kk_mesh_pass_queue(&prepared,entries,count);
   mtl_execute_command_queue(&prepared,handle,dispatch);
   util_dynarray_fini(&flat);
}
#endif

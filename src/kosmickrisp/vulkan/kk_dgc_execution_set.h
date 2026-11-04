/* SPDX-License-Identifier: MIT */
#include "vk_pipeline.h"
#include "vk_pipeline_cache.h"
#include "vk_shader.h"
#include "util/simple_mtx.h"

static const mesa_shader_stage kk_dgc_program_stages[] = {
   MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, MESA_SHADER_COMPUTE,
};
static const mesa_shader_stage kk_dgc_graphics_stages[] = {
   MESA_SHADER_VERTEX, MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL,
   MESA_SHADER_GEOMETRY, MESA_SHADER_TASK, MESA_SHADER_MESH, MESA_SHADER_FRAGMENT,
};
struct kk_dgc_program { struct vk_shader *shader[3]; };
struct kk_dgc_execution_set {
   struct vk_object_base base;
   simple_mtx_t mutex;
   uint32_t count;
   VkShaderStageFlags stages;
   struct kk_dgc_program initial;
   struct vk_dynamic_graphics_state dynamic;
   struct vk_vertex_input_state vi;
   struct vk_sample_locations_state sample_locations;
   struct kk_dgc_program programs[];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(kk_dgc_execution_set, base, VkIndirectExecutionSetEXT,
   VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT)

static void
kk_dgc_program_ref(struct kk_dgc_program *out, const struct kk_dgc_program *in)
{
   *out = *in;
   for (unsigned i = 0; i < ARRAY_SIZE(out->shader); ++i)
      if (out->shader[i]) vk_pipeline_cache_object_ref(&out->shader[i]->pipeline.cache_obj);
}
static void
kk_dgc_program_finish(struct kk_device *dev, struct kk_dgc_program *program)
{
   for (unsigned i = 0; i < ARRAY_SIZE(program->shader); ++i)
      if (program->shader[i])
         vk_pipeline_cache_object_unref(&dev->vk, &program->shader[i]->pipeline.cache_obj);
}
static struct kk_dgc_program
kk_dgc_program_from_pipeline(struct vk_pipeline *pipeline)
{
   struct kk_dgc_program program = {0};
   for (unsigned i = 0; i < ARRAY_SIZE(program.shader); ++i)
      if (pipeline->stages & mesa_to_vk_shader_stage(kk_dgc_program_stages[i]))
         program.shader[i] = vk_pipeline_get_shader(pipeline, kk_dgc_program_stages[i]);
   return program;
}
static void
kk_dgc_program_bind(struct kk_cmd_buffer *cmd, const struct kk_dgc_program *program,
                    bool compute)
{
   const struct vk_device_shader_ops *ops = kk_cmd_buffer_device(cmd)->vk.shader_ops;
   if (compute) {
      struct vk_shader *shader = program->shader[2];
      if (shader)
         ops->cmd_bind_shaders(&cmd->vk, 1, &kk_dgc_program_stages[2], &shader);
      else cmd->state.shaders[MESA_SHADER_COMPUTE] = NULL;
   } else {
      struct vk_shader *shaders[ARRAY_SIZE(kk_dgc_graphics_stages)] = {
         program->shader[0], NULL, NULL, NULL, NULL, NULL, program->shader[1],
      };
      ops->cmd_bind_shaders(&cmd->vk, ARRAY_SIZE(kk_dgc_graphics_stages),
         kk_dgc_graphics_stages, shaders);
   }
}
VKAPI_ATTR VkResult VKAPI_CALL
kk_CreateIndirectExecutionSetEXT(VkDevice device,
   const VkIndirectExecutionSetCreateInfoEXT *info,
   const VkAllocationCallbacks *alloc, VkIndirectExecutionSetEXT *out)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   *out = VK_NULL_HANDLE;
   if (kk_device_physical(dev)->info.gpu_apple_family < 10 ||
       info->type != VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT)
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   const VkIndirectExecutionSetPipelineInfoEXT *pi = info->info.pPipelineInfo;
   VK_FROM_HANDLE(vk_pipeline, pipeline, pi->initialPipeline);
   if (!pi->maxPipelineCount || pi->maxPipelineCount > KK_DGC_PIPELINE_COUNT ||
       (pipeline->stages & ~KK_DGC_PIPELINE_STAGES))
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   struct kk_dgc_execution_set *set = vk_object_zalloc(&dev->vk, alloc,
      sizeof(*set) + pi->maxPipelineCount * sizeof(set->programs[0]),
      VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT);
   if (!set) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   set->count = pi->maxPipelineCount;
   set->stages = pipeline->stages;
   simple_mtx_init(&set->mutex, mtx_plain);
   struct kk_dgc_program initial = kk_dgc_program_from_pipeline(pipeline);
   kk_dgc_program_ref(&set->initial, &initial);
   kk_dgc_program_ref(&set->programs[0], &initial);
   if (pipeline->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      set->dynamic = *vk_pipeline_get_dynamic_graphics_state(pipeline);
      if (set->dynamic.vi) { set->vi = *set->dynamic.vi; set->dynamic.vi = &set->vi; }
      if (set->dynamic.ms.sample_locations) {
         set->sample_locations = *set->dynamic.ms.sample_locations;
         set->dynamic.ms.sample_locations = &set->sample_locations;
      }
   }
   *out = kk_dgc_execution_set_to_handle(set);
   return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
kk_DestroyIndirectExecutionSetEXT(VkDevice device, VkIndirectExecutionSetEXT handle,
   const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(kk_dgc_execution_set, set, handle);
   if (!set) return;
   kk_dgc_program_finish(dev, &set->initial);
   for (uint32_t i = 0; i < set->count; ++i)
      kk_dgc_program_finish(dev, &set->programs[i]);
   simple_mtx_destroy(&set->mutex);
   vk_object_free(&dev->vk, alloc, set);
}
VKAPI_ATTR void VKAPI_CALL
kk_UpdateIndirectExecutionSetPipelineEXT(VkDevice device, VkIndirectExecutionSetEXT handle,
   uint32_t count, const VkWriteIndirectExecutionSetPipelineEXT *writes)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(kk_dgc_execution_set, set, handle);
   simple_mtx_lock(&set->mutex);
   for (uint32_t i = 0; i < count; ++i) {
      assert(writes[i].index < set->count);
      VK_FROM_HANDLE(vk_pipeline, pipeline, writes[i].pipeline);
      struct kk_dgc_program next = kk_dgc_program_from_pipeline(pipeline);
      struct kk_dgc_program retained;
      kk_dgc_program_ref(&retained, &next);
      kk_dgc_program_finish(dev, &set->programs[writes[i].index]);
      set->programs[writes[i].index] = retained;
   }
   simple_mtx_unlock(&set->mutex);
}

/* A submitted command keeps the exact selected providers through retirement,
 * including an update to a different unused set entry on another host thread.
 * The low bit distinguishes this CPU block from aligned native ICB resources
 * in the existing actual-use retirement array. No ordinary command storage. */
struct kk_dgc_program_snapshot {
   uint32_t count;
   struct kk_dgc_program initial;
   struct kk_dgc_program programs[];
};
static struct kk_dgc_program_snapshot *
kk_dgc_snapshot_programs(struct kk_cmd_buffer *cmd, struct kk_dgc_execution_set *set)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_dgc_program_snapshot *snapshot = vk_zalloc(&dev->vk.alloc,
      sizeof(*snapshot) + set->count * (sizeof(snapshot->programs[0]) + sizeof(mtl_depth_stencil_state *)),
      8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!snapshot) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return NULL;
   }
   void **retired = util_dynarray_grow(&cmd->dgc_commands, void *, 1);
   if (!retired) {
      vk_free(&dev->vk.alloc, snapshot);
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return NULL;
   }
   simple_mtx_lock(&set->mutex);
   snapshot->count = set->count;
   kk_dgc_program_ref(&snapshot->initial, &set->initial);
   for (uint32_t i = 0; i < set->count; ++i)
      kk_dgc_program_ref(&snapshot->programs[i], &set->programs[i]);
   simple_mtx_unlock(&set->mutex);
   *retired = (void *)((uintptr_t)snapshot | 1u);
   return snapshot;
}
static mtl_depth_stencil_state **
kk_dgc_snapshot_depth(struct kk_dgc_program_snapshot *snapshot)
{
   return (mtl_depth_stencil_state **)(snapshot->programs + snapshot->count);
}
static void
kk_dgc_snapshot_finish(struct kk_device *dev, struct kk_dgc_program_snapshot *snapshot)
{
   kk_dgc_program_finish(dev, &snapshot->initial);
   for (uint32_t i = 0; i < snapshot->count; ++i) {
      mtl_release(kk_dgc_snapshot_depth(snapshot)[i]);
      kk_dgc_program_finish(dev, &snapshot->programs[i]);
   }
   vk_free(&dev->vk.alloc, snapshot);
}

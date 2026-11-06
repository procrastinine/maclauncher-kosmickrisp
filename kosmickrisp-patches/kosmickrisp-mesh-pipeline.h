/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_PIPELINE_H
#define KK_MESH_PIPELINE_H
#include "kk_mesh_sliced.h"

static VkResult
kk_mesh_function(struct kk_device *dev, const struct msl_compile_data *data,
                  bool virtual, const char *suffix, mtl_library **library,
                  mtl_function_descriptor **function)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);
   char *source = virtual ? ralloc_asprintf(NULL,
      "#define KK_MESH_VIRTUAL 1\n#define KK_MESH_VIRTUAL_ONLY 1\n%s", data->code) : NULL;
   *library = mtl_new_library(dev->mtl_compiler_handle,
      source ? source : data->code, pdev->info.msl_version,
      MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
   ralloc_free(source);
   if (!*library)
      return VK_ERROR_INVALID_SHADER_NV;
   char *entry = suffix ? ralloc_asprintf(NULL, "%s_%s", data->entrypoint_name, suffix) : NULL;
   *function = mtl_new_library_function_descriptor(*library,
      entry ? entry : data->entrypoint_name);
   ralloc_free(entry);
   return *function ? VK_SUCCESS : VK_ERROR_INVALID_SHADER_NV;
}

VkResult
kk_mesh_build_pipeline(struct kk_device *dev, struct kk_shader *shader,
   const struct msl_compile_data *task_data,
   const struct msl_compile_data *mesh_data,
   const struct kk_mesh_fragment_program *fragment_program,
   bool force_virtual, struct kk_pipeline_handles *pipe)
{
   const struct kk_shader_info *info = &shader->info;
   if (!fragment_program || !mesh_data ||
       (!!task_data != info->mesh.has_task) ||
       (!!fragment_program->monolithic == !!fragment_program->attach))
      return VK_ERROR_INVALID_SHADER_NV;
   if (fragment_program->attach && !shader->mesh_fragment) {
      shader->mesh_fragment = malloc(sizeof(*shader->mesh_fragment));
      if (!shader->mesh_fragment)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      *shader->mesh_fragment = *fragment_program;
   }
   bool virtual = info->mesh.has_task &&
      (force_virtual || info->mesh.task_grid_bound > KK_MESH_NATIVE_GROUPS);
   void *temporary = ralloc_context(NULL);
   struct msl_compile_data retained_mesh;
   if (info->mesh.retained_capture || info->mesh.retained_reader) {
      retained_mesh = *mesh_data;
      retained_mesh.code = ralloc_asprintf(temporary,
         "#define KK_MESH_RETAINED 1\n#define %s 1\n%s",
         info->mesh.retained_reader ? "KK_MESH_RETAINED_READER" : "KK_MESH_RETAINED_CAPTURE",
         mesh_data->code);
      mesh_data = &retained_mesh;
   }
   struct msl_compile_data sliced_mesh;
   if (virtual && info->mesh.sliced) {
      sliced_mesh = *mesh_data;
      sliced_mesh.code = kk_mesh_virtual_sliced_source(temporary, mesh_data->code);
      if (!sliced_mesh.code) {
         ralloc_free(temporary);
         return VK_ERROR_INVALID_SHADER_NV;
      }
      mesh_data = &sliced_mesh;
   }
   mtl_library *mesh_lib = NULL, *task_lib = NULL, *fragment_lib = NULL;
   mtl_function_descriptor *mesh = NULL, *task = NULL, *fragment = NULL;
   mtl_render_pipeline_descriptor *descriptor = NULL;
   VkResult result = kk_mesh_function(dev, mesh_data,
      virtual, info->mesh.retained_reader ? "retained" : virtual ? "virtual" : NULL, &mesh_lib, &mesh);
   if (result != VK_SUCCESS)
      goto out;
   if (info->mesh.has_task) {
      result = kk_mesh_function(dev, task_data,
         virtual, virtual ? "compute" : NULL, &task_lib, &task);
      if (result != VK_SUCCESS)
         goto out;
      if (virtual) {
         struct mtl_size local = info->mesh.task_local_size;
         pipe->gfx.pre_render[0] = mtl_new_compute_pipeline_state(
            dev->mtl_compiler_handle, task, local.x * local.y * local.z);
         if (!pipe->gfx.pre_render[0]) {
            result = VK_ERROR_INVALID_SHADER_NV;
            goto out;
         }
         pipe->gfx.pre_render_count = 1;
      }
   }
   if (fragment_program->monolithic) {
      result = kk_mesh_function(dev, fragment_program->monolithic,
         false, NULL, &fragment_lib, &fragment);
      if (result != VK_SUCCESS)
         goto out;
   }
   struct mtl_size ml = info->mesh.retained_reader
      ? (struct mtl_size){1, 1, 1} : info->mesh.local_size;
   struct mtl_size tl = info->mesh.task_local_size;
   descriptor = mtl_new_mesh_pipeline_descriptor(mesh, virtual ? NULL : task,
      ml.x * ml.y * ml.z, tl.x * tl.y * tl.z,
      MAX2(info->mesh.payload_size, info->mesh.task_payload_size),
      info->mesh.task_grid_bound);
   if (!descriptor) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto out;
   }
   if (fragment_program->attach) {
      result = fragment_program->attach(dev, fragment_program, descriptor);
      if (result != VK_SUCCESS)
         goto out;
   } else {
      mtl_render_pipeline_descriptor_set_fragment_shader(descriptor, fragment);
   }
   kk_configure_render_pipeline(dev, shader, pipe, descriptor, false);
   pipe->gfx.render = mtl_new_render_pipeline(dev->mtl_compiler_handle, descriptor);
   if (!pipe->gfx.render) {
      result = VK_ERROR_INVALID_SHADER_NV;
   } else {
      unsigned kind = info->mesh.retained_reader ? KK_PIPELINE_MESH_READER :
         info->mesh.retained_capture ? KK_PIPELINE_MESH_CAPTURE : info->mesh.sliced ? (virtual ? KK_PIPELINE_MESH_VIRTUAL_SLICED : KK_PIPELINE_MESH_SLICED) :
         force_virtual ? KK_PIPELINE_MESH_VIRTUAL : KK_PIPELINE_MAIN;
      if (fragment_program->finish)
         result = fragment_program->finish(dev, fragment_program, descriptor, pipe, kind);
      if (result == VK_SUCCESS)
         result = kk_record_render_descriptor(dev, shader, descriptor,
                                             kind);
   }
out:
   mtl_release(descriptor);
   mtl_release(mesh);
   mtl_release(task);
   mtl_release(fragment);
   mtl_release(mesh_lib);
   mtl_release(task_lib);
   mtl_release(fragment_lib);
   ralloc_free(temporary);
   return result;
}

static VkResult
kk_compile_mesh_pipeline(struct kk_device *dev, struct kk_shader *shader,
                          bool force_virtual, struct kk_pipeline_handles *pipe)
{
   const struct kk_mesh_fragment_program monolithic = {
      .monolithic = &shader->msl_data[MESA_SHADER_FRAGMENT],
   };
   return kk_mesh_build_pipeline(dev, shader,
      shader->info.mesh.has_task ? &shader->msl_data[MESA_SHADER_TASK] : NULL,
      &shader->msl_data[MESA_SHADER_MESH],
      shader->mesh_fragment ? shader->mesh_fragment : &monolithic,
      force_virtual, pipe);
}

void
kk_mesh_release_pipeline(struct kk_device *dev, struct kk_shader *shader,
                           struct kk_pipeline_handles *pipe, unsigned kind)
{
   if (shader->mesh_fragment && shader->mesh_fragment->release_auxiliary)
      shader->mesh_fragment->release_auxiliary(dev, shader->mesh_fragment, pipe, kind);
   mtl_release(pipe->gfx.render);
   mtl_release(pipe->gfx.ds_handle);
   for (unsigned i = 0; i < pipe->gfx.pre_render_count; ++i)
      mtl_release(pipe->gfx.pre_render[i]);
   free(pipe);
}

VkResult
kk_mesh_get_sliced_pipeline(struct kk_device *dev, struct kk_shader *shader,
                              bool virtual, struct kk_pipeline_handles **out)
{
   unsigned kind = virtual ? KK_PIPELINE_MESH_VIRTUAL_SLICED : KK_PIPELINE_MESH_SLICED;
   struct kk_pipeline_handles *cached = p_atomic_read(&shader->mesh_sliced[virtual]);
   if (!cached) {
      struct kk_pipeline_handles *created = calloc(1, sizeof(*created));
      if (!created)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      void *mem = ralloc_context(NULL);
      struct msl_compile_data task_sliced, mesh_sliced;
      const struct msl_compile_data *task = shader->info.mesh.has_task
         ? &shader->msl_data[MESA_SHADER_TASK] : NULL;
      const struct msl_compile_data *mesh = &shader->msl_data[MESA_SHADER_MESH];
      VkResult result = VK_SUCCESS;
      if (!virtual) {
         bool valid = task
            ? kk_mesh_native_sliced_data(mem, task, true, &task_sliced)
            : kk_mesh_native_sliced_data(mem, mesh, false, &mesh_sliced);
         if (!valid) {
            result = VK_ERROR_INVALID_SHADER_NV;
            goto finish;
         }
         if (task) task = &task_sliced;
         else mesh = &mesh_sliced;
      }
      struct kk_shader executable = *shader;
      executable.info.mesh.sliced = true;
      const struct kk_mesh_fragment_program monolithic = {
         .monolithic = &shader->msl_data[MESA_SHADER_FRAGMENT],
      };
      result = kk_mesh_build_pipeline(dev, &executable, task, mesh,
         shader->mesh_fragment ? shader->mesh_fragment : &monolithic,
         virtual, created);
finish:
      ralloc_free(mem);
      if (result != VK_SUCCESS) {
         kk_mesh_release_pipeline(dev, shader, created, kind);
         return result;
      }
      cached = p_atomic_cmpxchg(&shader->mesh_sliced[virtual], NULL, created);
      if (cached)
         kk_mesh_release_pipeline(dev, shader, created, kind);
      else
         cached = created;
   }
   *out = cached;
   return VK_SUCCESS;
}

/* Both variants share the final fragment executable and its ownership. Capture
 * retains the original task/mesh code; reader has no user stage or task entry. */
VkResult
kk_mesh_get_retained_pipeline(struct kk_device *dev, struct kk_shader *shader,
                             bool reader, struct kk_pipeline_handles **out)
{
   struct kk_pipeline_handles *cached = p_atomic_read(&shader->mesh_retained[reader]);
   if (!cached) {
      unsigned kind = reader ? KK_PIPELINE_MESH_READER : KK_PIPELINE_MESH_CAPTURE;
      struct kk_pipeline_handles *created = calloc(1, sizeof(*created));
      if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
      void *mem = ralloc_context(NULL);
      struct kk_shader executable = *shader;
      executable.info.mesh.retained_capture = !reader;
      executable.info.mesh.retained_reader = reader;
      executable.info.mesh.sliced = !reader;
      bool virtual = !reader && shader->info.mesh.has_task;
      executable.info.mesh.has_task = virtual;
      const struct msl_compile_data *mesh = &shader->msl_data[MESA_SHADER_MESH];
      struct msl_compile_data sliced;
      VkResult result = VK_SUCCESS;
      if (!reader && !virtual) {
         if (!kk_mesh_native_sliced_data(mem, mesh, false, &sliced)) {
            result = VK_ERROR_INVALID_SHADER_NV;
            goto finish;
         }
         mesh = &sliced;
      }
      const struct kk_mesh_fragment_program monolithic = {
         .monolithic = &shader->msl_data[MESA_SHADER_FRAGMENT],
      };
      result = kk_mesh_build_pipeline(dev, &executable,
         virtual ? &shader->msl_data[MESA_SHADER_TASK] : NULL, mesh,
         shader->mesh_fragment ? shader->mesh_fragment : &monolithic,
         virtual, created);
finish:
      ralloc_free(mem);
      if (result != VK_SUCCESS) {
         kk_mesh_release_pipeline(dev, shader, created, kind);
         return result;
      }
      cached = p_atomic_cmpxchg(&shader->mesh_retained[reader], NULL, created);
      if (cached) kk_mesh_release_pipeline(dev, shader, created, kind);
      else cached = created;
   }
   *out = cached;
   return VK_SUCCESS;
}

mtl_render_pipeline_descriptor *
kk_mesh_retained_raster_descriptor(const struct kk_shader *shader)
{
   return shader->attachment_pipelines
      ? shader->attachment_pipelines->descriptor[KK_PIPELINE_MESH_READER] : NULL;
}

/* DGC can request the compute/mesh-only form even for a bounded native task
 * shader. The same stored MSL body supplies it; normal native draws create no
 * second pipeline. Atomic publication permits parallel command recording. */
VkResult
kk_mesh_get_pipeline(struct kk_device *dev, struct kk_shader *shader,
                      bool force_virtual, struct kk_pipeline_handles **out)
{
   if (!force_virtual || !shader->info.mesh.has_task ||
       shader->info.mesh.task_grid_bound > KK_MESH_NATIVE_GROUPS) {
      *out = &shader->pipeline;
      return VK_SUCCESS;
   }
   struct kk_pipeline_handles *cached = p_atomic_read(&shader->mesh_virtual);
   if (!cached) {
      struct kk_pipeline_handles *created = calloc(1, sizeof(*created));
      if (!created)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      VkResult result = kk_compile_mesh_pipeline(dev, shader, true, created);
      if (result != VK_SUCCESS) {
         kk_mesh_release_pipeline(dev, shader, created, KK_PIPELINE_MESH_VIRTUAL);
         return result;
      }
      cached = p_atomic_cmpxchg(&shader->mesh_virtual, NULL, created);
      if (cached) {
         kk_mesh_release_pipeline(dev, shader, created, KK_PIPELINE_MESH_VIRTUAL);
      } else {
         cached = created;
      }
   }
   *out = cached;
   return VK_SUCCESS;
}

#endif

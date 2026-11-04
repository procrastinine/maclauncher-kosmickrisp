/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_device.h"
#include "kk_physical_device.h"
#include "kk_shader.h"

#include "kosmickrisp/clc/kk_precompiled_shader.h"

#include "libkk_shaders.h"

#include "kosmickrisp/bridge/mtl_bridge.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"

static const enum libkk_program conservative_programs[] = {
   LIBKK_CONSERVATIVE_CLIP,
   LIBKK_CONSERVATIVE_PREPARE,
   LIBKK_CONSERVATIVE_OUTPUT_SETUP,
   LIBKK_CONSERVATIVE_OUTPUT,
};

static const enum libkk_program primitives_generated_programs[] = {
   LIBKK_PRIMITIVES_GENERATED,
   LIBKK_PRIMITIVES_GENERATED_RESTART,
};

static bool
is_optional_program(enum libkk_program program)
{
   for (unsigned i = 0; i < ARRAY_SIZE(conservative_programs); ++i)
      if (program == conservative_programs[i])
         return true;
   for (unsigned i = 0; i < ARRAY_SIZE(primitives_generated_programs); ++i)
      if (program == primitives_generated_programs[i])
         return true;
   return false;
}

static VkResult
build_precompiled_shader(struct kk_device *dev, enum libkk_program program)
{
   const uint32_t *bin = libkk_AppleSilicon[program];
   const struct kk_precompiled_info *info = (void *)bin;
   const char *msl = (const char *)bin + sizeof(*info);

   mtl_library *library = mtl_new_library(
      dev->mtl_compiler_handle, msl, MTL_LANGUAGE_VERSION_4_0,
      MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
   if (library == NULL)
      return vk_error(dev, VK_ERROR_INVALID_SHADER_NV);

   struct kk_precompiled_shader *shader = &dev->precompiled_cache.shaders[program];

   /* TODO_KOSMICKRISP Do not hardcode the entrypoint */
   mtl_function_descriptor *function =
      mtl_new_library_function_descriptor(library, "main_entrypoint");
   uint32_t local_size_threads = info->workgroup_size[0] *
                                 info->workgroup_size[1] *
                                 info->workgroup_size[2];
   shader->pipeline = mtl_new_compute_pipeline_state(
      dev->mtl_compiler_handle, function,
      /* Preserve private temporaries in conservative preparation by
       * leaving this compiler limit at its default. Dispatch dimensions
       * still come from the precompiled kernel metadata. */
      program == LIBKK_CONSERVATIVE_PREPARE ? 0 : local_size_threads);
   mtl_release(function);
   mtl_release(library);

   if (!shader->pipeline)
      return vk_error(dev, VK_ERROR_INVALID_SHADER_NV);

   memcpy(&shader->info, info, sizeof(*info));
   return VK_SUCCESS;
}

static VkResult
build_precompiled_shaders(struct kk_device *dev)
{
   uint32_t i = 0;
   for (; i < LIBKK_NUM_PROGRAMS; ++i) {
      if (is_optional_program(i))
         continue;
      VkResult result = build_precompiled_shader(dev, i);
      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; ++j)
            mtl_release(dev->precompiled_cache.shaders[j].pipeline);
         return result;
      }
   }

   return VK_SUCCESS;
}

static VkResult
build_program_group(struct kk_device *dev, const enum libkk_program *programs,
                    unsigned count)
{
   for (unsigned i = 0; i < count; ++i) {
      VkResult result = build_precompiled_shader(dev, programs[i]);
      if (result != VK_SUCCESS) {
         /* Device teardown also visits these slots. Leave no stale handles
          * after a partial initialization failure. */
         for (unsigned j = 0; j < i; ++j) {
            struct kk_precompiled_shader *shader =
               &dev->precompiled_cache.shaders[programs[j]];
            mtl_release(shader->pipeline);
            shader->pipeline = NULL;
         }
         return result;
      }
   }
   return VK_SUCCESS;
}

static void
build_conservative_shaders(const void *data)
{
   struct kk_device *dev = (struct kk_device *)data;
   dev->precompiled_cache.conservative_result = build_program_group(
      dev, conservative_programs, ARRAY_SIZE(conservative_programs));
}

static void
build_primitives_generated_shaders(const void *data)
{
   struct kk_device *dev = (struct kk_device *)data;
   dev->precompiled_cache.primitives_generated_result = build_program_group(
      dev, primitives_generated_programs, ARRAY_SIZE(primitives_generated_programs));
}

VkResult
kk_device_init_conservative_lib(struct kk_device *dev)
{
   util_call_once_data(&dev->precompiled_cache.conservative_once,
                      build_conservative_shaders, dev);
   return dev->precompiled_cache.conservative_result;
}

VkResult
kk_device_init_primitives_generated_lib(struct kk_device *dev)
{
   util_call_once_data(&dev->precompiled_cache.primitives_generated_once,
                      build_primitives_generated_shaders, dev);
   return dev->precompiled_cache.primitives_generated_result;
}

VkResult
kk_device_init_lib(struct kk_device *dev)
{
   return build_precompiled_shaders(dev);
}

void
kk_device_finish_lib(struct kk_device *dev)
{
   for (uint32_t i = 0; i < LIBKK_NUM_PROGRAMS; ++i)
      mtl_release(dev->precompiled_cache.shaders[i].pipeline);
}

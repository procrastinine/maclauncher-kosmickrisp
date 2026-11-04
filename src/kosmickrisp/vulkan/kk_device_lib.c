/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_device.h"
#include "kk_physical_device.h"
#include "kk_shader.h"
#include "kk_perf.h"
#include "util/u_call_once.h"

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

static VkResult
build_precompiled_shader(struct kk_device *dev, enum libkk_program program)
{
   const uint32_t *bin = libkk_AppleSilicon[program];
   const struct kk_precompiled_info *info = (void *)bin;
   const char *msl = (const char *)bin + sizeof(*info);

   mtl_library *library = KK_PERF_CALL(dev, KK_PERF_METAL_LIBRARY, mtl_new_library(
      dev->mtl_compiler_handle, msl, MTL_LANGUAGE_VERSION_4_0,
      MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST));
   if (library == NULL)
      return vk_error(dev, VK_ERROR_INVALID_SHADER_NV);

   struct kk_precompiled_shader *shader = &dev->precompiled_cache.shaders[program];

   /* TODO_KOSMICKRISP Do not hardcode the entrypoint */
   mtl_function_descriptor *function =
      mtl_new_library_function_descriptor(library, "main_entrypoint");
   uint32_t local_size_threads = info->workgroup_size[0] *
                                 info->workgroup_size[1] *
                                 info->workgroup_size[2];
   shader->pipeline = KK_PERF_CALL(dev, KK_PERF_METAL_PIPELINE, mtl_new_compute_pipeline_state(
      dev->mtl_compiler_handle, function,
      /* Preserve private temporaries in conservative preparation by
       * leaving this compiler limit at its default. Dispatch dimensions
       * still come from the precompiled kernel metadata. */
      program == LIBKK_CONSERVATIVE_PREPARE ? 0 : local_size_threads));
   mtl_release(function);
   mtl_release(library);

   if (!shader->pipeline)
      return vk_error(dev, VK_ERROR_INVALID_SHADER_NV);

   memcpy(&shader->info, info, sizeof(*info));
   return VK_SUCCESS;
}

VkResult
kk_device_init_lib(struct kk_device *dev)
{
   for (unsigned i = 0; i < LIBKK_NUM_PROGRAMS; i++) {
      dev->precompiled_cache.once[i] = (util_once_flag)UTIL_ONCE_FLAG_INIT;
      dev->precompiled_cache.shaders[i].pipeline = NULL;
   }
   return VK_SUCCESS;
}

struct kk_precompiled_build {
   struct kk_device *dev;
   enum libkk_program program;
};

static void
build_precompiled_once(const void *data)
{
   const struct kk_precompiled_build *build = data;
   build->dev->precompiled_cache.result[build->program] =
      build_precompiled_shader(build->dev, build->program);
}

/* Shared slot initialization for dispatch and feature-owned group preflight.
 * A slot remains device-owned after publication, including partial group
 * failures. The per-slot once flag publishes metadata, pipeline and result. */
VkResult
kk_device_ensure_precompiled_shader(struct kk_device *dev, enum libkk_program idx)
{
   assert(idx < LIBKK_NUM_PROGRAMS);
   const struct kk_precompiled_build build = {dev, idx};
   util_call_once_data(&dev->precompiled_cache.once[idx],
                      build_precompiled_once, &build);
   return dev->precompiled_cache.result[idx];
}

struct kk_precompiled_shader *
kk_device_get_precompiled(struct kk_device *dev, enum libkk_program idx)
{
   if (kk_device_ensure_precompiled_shader(dev, idx) != VK_SUCCESS)
      return NULL;
   return &dev->precompiled_cache.shaders[idx];
}

/* Feature-owned preflight uses the same published slots as dispatch. A later
 * failure leaves earlier successful slots owned by the device. */
static VkResult
ensure_program_group(struct kk_device *dev, const enum libkk_program *programs,
                     unsigned count)
{
   for (unsigned i = 0; i < count; i++) {
      VkResult result = kk_device_ensure_precompiled_shader(dev, programs[i]);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

VkResult
kk_device_init_conservative_lib(struct kk_device *dev)
{
   return ensure_program_group(dev, conservative_programs, ARRAY_SIZE(conservative_programs));
}

VkResult
kk_device_init_primitives_generated_lib(struct kk_device *dev)
{
   return ensure_program_group(dev, primitives_generated_programs, ARRAY_SIZE(primitives_generated_programs));
}

void
kk_device_finish_lib(struct kk_device *dev)
{
   for (uint32_t i = 0; i < LIBKK_NUM_PROGRAMS; ++i) {
      mtl_release(dev->precompiled_cache.shaders[i].pipeline);
   }
}

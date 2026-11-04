/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_SUPPORT_CACHE_H
#define KK_FRAGMENT_SHADING_RATE_SUPPORT_CACHE_H

#include "kk_device.h"
#include "kk_instance.h"
#include "kk_physical_device.h"
#include "kk_fragment_shading_rate_cache.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_fragment_shading_rate.h"
#include "util/blob.h"
#include "vk_pipeline_cache.h"

#define KK_VRS_SUPPORT_SCHEMA 2u

struct kk_vrs_support {
   struct vk_pipeline_cache_object base;
   mtl_library *library;
   enum mtl_language_version version;
   enum mtl_math_mode math;
   enum mtl_math_floating_point_functions functions;
   uint8_t key[48];
};

static void
kk_vrs_support_key(struct kk_device *dev, enum mtl_language_version version,
   enum mtl_math_mode math, enum mtl_math_floating_point_functions functions,
   uint8_t key[48])
{
   struct kk_instance *instance =
      kk_physical_device_instance(kk_device_physical(dev));
   memset(key, 0, 48);
   memcpy(key, "KK-VRS-SUPPORT", 14);
   memcpy(key + 16, instance->driver_build_sha, VK_UUID_SIZE);
   const uint32_t fields[] = {version, math, functions, KK_VRS_SUPPORT_SCHEMA};
   for (unsigned i = 0; i < ARRAY_SIZE(fields); i++)
      for (unsigned b = 0; b < 4; b++)
         key[32 + 4 * i + b] = fields[i] >> (8 * b);
}

static bool
kk_vrs_support_serialize(struct vk_pipeline_cache_object *base, struct blob *blob)
{
   struct kk_vrs_support *support = container_of(base, struct kk_vrs_support, base);
   blob_write_uint32(blob, KK_VRS_SUPPORT_SCHEMA);
   blob_write_uint32(blob, support->version);
   blob_write_uint32(blob, support->math);
   blob_write_uint32(blob, support->functions);
   return !blob->out_of_memory;
}

static void
kk_vrs_support_destroy(struct vk_device *device,
                       struct vk_pipeline_cache_object *base)
{
   struct kk_vrs_support *support = container_of(base, struct kk_vrs_support, base);
   mtl_release(support->library);
   vk_pipeline_cache_object_finish(base);
   free(support);
}

static const struct vk_pipeline_cache_object_ops kk_vrs_support_ops;

static struct kk_vrs_support *
kk_vrs_support_create(struct kk_device *dev, const uint8_t key[48],
   enum mtl_language_version version, enum mtl_math_mode math,
   enum mtl_math_floating_point_functions functions, VkResult *result)
{
   *result = VK_ERROR_OUT_OF_HOST_MEMORY;
   struct kk_vrs_support *support = calloc(1, sizeof(*support));
   if (!support)
      return NULL;
   memcpy(support->key, key, sizeof(support->key));
   support->version = version;
   support->math = math;
   support->functions = functions;
   vk_pipeline_cache_object_init(&dev->vk, &support->base, &kk_vrs_support_ops,
                                support->key, sizeof(support->key));
   support->library = mtl_vrs_new_support_library(dev->mtl_compiler_handle,
                                                 version, math, functions);
   if (!support->library) {
      kk_vrs_support_destroy(&dev->vk, &support->base);
      *result = VK_ERROR_INVALID_SHADER_NV;
      return NULL;
   }
   *result = VK_SUCCESS;
   return support;
}

static struct vk_pipeline_cache_object *
kk_vrs_support_deserialize(struct vk_pipeline_cache *cache,
   const void *key, size_t key_size, struct blob_reader *blob)
{
   if (key_size != 48 || blob_read_uint32(blob) != KK_VRS_SUPPORT_SCHEMA)
      return NULL;
   enum mtl_language_version version = blob_read_uint32(blob);
   enum mtl_math_mode math = blob_read_uint32(blob);
   enum mtl_math_floating_point_functions functions = blob_read_uint32(blob);
   struct kk_device *dev = container_of(cache->base.device, struct kk_device, vk);
   if (blob->overrun || blob->current != blob->end ||
       (version != MTL_LANGUAGE_VERSION_4_0 && version != MTL_LANGUAGE_VERSION_4_1) ||
       version > kk_device_physical(dev)->info.msl_version ||
       math > MTL_MATH_MODE_FAST ||
       functions > MTL_MATH_FLOATING_POINT_FUNCTIONS_PRECISE)
      return NULL;
   uint8_t expected[48];
   kk_vrs_support_key(dev, version, math, functions, expected);
   if (memcmp(key, expected, sizeof(expected)))
      return NULL;
   VkResult result;
   struct kk_vrs_support *support =
      kk_vrs_support_create(dev, expected, version, math, functions, &result);
   return support ? &support->base : NULL;
}

static const struct vk_pipeline_cache_object_ops kk_vrs_support_ops = {
   .serialize = kk_vrs_support_serialize,
   .deserialize = kk_vrs_support_deserialize,
   .destroy = kk_vrs_support_destroy,
};

static VkResult
kk_vrs_support_get(struct kk_device *dev, enum mtl_language_version version,
   enum mtl_math_mode math, enum mtl_math_floating_point_functions functions,
   struct kk_vrs_support **out)
{
   *out = NULL;
   if (!dev->vrs)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   uint8_t key[48];
   kk_vrs_support_key(dev, version, math, functions, key);
   struct vk_pipeline_cache_object *base = NULL;
   VkResult result = VK_SUCCESS;
   /* lookup may deserialize/compile. Use the same VRS-only single-flight
    * boundary as application artifacts; never hold the common cache lock. */
   simple_mtx_lock(&dev->vrs->compile_mutex);
   if (dev->vk.mem_cache)
      base = vk_pipeline_cache_lookup_object(dev->vk.mem_cache, key, sizeof(key),
                                             &kk_vrs_support_ops, NULL);
   if (!base) {
      struct kk_vrs_support *support =
         kk_vrs_support_create(dev, key, version, math, functions, &result);
      if (!support)
         goto unlock;
      base = &support->base;
      if (dev->vk.mem_cache)
         base = vk_pipeline_cache_add_object(dev->vk.mem_cache, base);
   }
   *out = container_of(base, struct kk_vrs_support, base);
unlock:
   simple_mtx_unlock(&dev->vrs->compile_mutex);
   return result;
}

#endif

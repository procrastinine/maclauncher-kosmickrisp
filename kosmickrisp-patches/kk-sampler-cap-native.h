/* SPDX-License-Identifier: MIT
 * Native descriptors share one materialization cache across logical devices.
 * Cache references retain stable IDs until the final device using it retires.
 */
#ifndef KK_SAMPLER_CAPACITY_NATIVE_H
#define KK_SAMPLER_CAPACITY_NATIVE_H
#include "kk-sampler-cap-keys.h"
#include "kosmickrisp/bridge/mtl_device.h"
#include "util/hash_table.h"
#include <stdlib.h>

struct kk_sampler_native_state {
   struct mtl_sampler_packed key;
   mtl_sampler *handle;
};
struct kk_sampler_native_cache {
   struct kk_sampler_native_cache *next;
   uint64_t registry_id;
   uint32_t users, limit, canonical_count;
   struct hash_table *states;
   struct mtl_sampler_id_check *id_check;
   bool acceleration_closed;
   struct mtl_sampler_packed canonical[KK_SAMPLER_CAP_CANONICAL_MAX];
};
static struct kk_sampler_native_cache *kk_sampler_native_caches;
static simple_mtx_t kk_sampler_native_mutex;
static once_flag kk_sampler_native_once = ONCE_FLAG_INIT;
static uint32_t kk_sampler_native_count;
static uint32_t kk_sampler_native_limit = UINT32_MAX;
DERIVE_HASH_TABLE(mtl_sampler_packed);

static void
kk_sampler_native_mutex_init(void)
{
   simple_mtx_init(&kk_sampler_native_mutex, mtx_plain);
}

/* The process-wide mutex protects cache membership and the actual native
 * descriptor budget, including materialization requested by sparse queries. */
static mtl_sampler *
kk_sampler_native_materialize(struct kk_device *dev,
                              struct kk_sampler_native_cache *cache,
                              const struct mtl_sampler_packed *packed)
{
   struct mtl_sampler_packed key;
   kk_sampler_cap_key_normalize(&key, packed);
   struct hash_entry *entry = _mesa_hash_table_search(cache->states, &key);
   if (entry)
      return ((struct kk_sampler_native_state *)entry->data)->handle;
   if (cache->acceleration_closed ||
       kk_sampler_native_count >= kk_sampler_native_limit)
      return NULL;
   struct kk_sampler_native_state *state = ralloc(cache->states, struct kk_sampler_native_state);
   if (!state)
      return NULL;
   memcpy(&state->key, &key, sizeof(key));
   mtl_sampler_descriptor *descriptor = create_sampler_descriptor(dev, &key);
   state->handle = mtl_new_sampler(dev->mtl_handle, descriptor);
   mtl_release(descriptor);
   if (!state->handle) {
      ralloc_free(state);
      return NULL;
   }
   if (!mtl_sampler_id_check_matches(cache->id_check, state->handle)) {
      /* A non-nil state may have a raw ID that differs from ordinary Metal
       * argument encoding. Never publish it or retry new acceleration while
       * this cache lives; reserved canonical states continue serving requests. */
      cache->acceleration_closed = true;
      mtl_release(state->handle);
      ralloc_free(state);
      return NULL;
   }
   if (!_mesa_hash_table_insert(cache->states, &state->key, state)) {
      mtl_release(state->handle);
      ralloc_free(state);
      return NULL;
   }
   ++kk_sampler_native_count;
   return state->handle;
}

static void
kk_sampler_native_cache_destroy(struct kk_sampler_native_cache *cache)
{
   hash_table_foreach(cache->states, entry) {
      struct kk_sampler_native_state *state = entry->data;
      mtl_release(state->handle);
      --kk_sampler_native_count;
   }
   mtl_sampler_id_check_destroy(cache->id_check);
   ralloc_free(cache->states);
   free(cache);
}

static VkResult
kk_sampler_native_canonical_add(struct kk_device *dev,
                                struct kk_sampler_native_cache *cache,
                                const struct mtl_sampler_packed *input)
{
   struct mtl_sampler_packed key;
   kk_sampler_cap_key(&key, input);
   if (_mesa_hash_table_search(cache->states, &key))
      return VK_SUCCESS;
   if (cache->canonical_count >= KK_SAMPLER_CAP_CANONICAL_MAX ||
       !kk_sampler_native_materialize(dev, cache, &key))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   memcpy(&cache->canonical[cache->canonical_count++], &key, sizeof(key));
   return VK_SUCCESS;
}

VkResult
kk_sampler_cap_native_init(struct kk_device *dev)
{
   call_once(&kk_sampler_native_once, kk_sampler_native_mutex_init);
   simple_mtx_lock(&kk_sampler_native_mutex);
   const uint64_t registry_id = mtl_device_get_registry_id(dev->mtl_handle);
   struct kk_sampler_native_cache *cache = kk_sampler_native_caches;
   for (; cache; cache = cache->next) {
      if (cache->registry_id == registry_id) {
         ++cache->users;
         dev->samplers.native_cache = cache;
         simple_mtx_unlock(&kk_sampler_native_mutex);
         return VK_SUCCESS;
      }
   }
   cache = calloc(1, sizeof(*cache));
   if (!cache) {
      simple_mtx_unlock(&kk_sampler_native_mutex);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   cache->states = mtl_sampler_packed_table_create(NULL);
   if (!cache->states) {
      free(cache);
      simple_mtx_unlock(&kk_sampler_native_mutex);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   cache->id_check = mtl_sampler_id_check_create(dev->mtl_handle);
   if (!cache->id_check) {
      ralloc_free(cache->states);
      free(cache);
      simple_mtx_unlock(&kk_sampler_native_mutex);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   cache->limit = kk_device_physical(dev)->info.max_sampler_count;
   const uint32_t prior_limit = kk_sampler_native_limit;
   kk_sampler_native_limit = MIN2(kk_sampler_native_limit, cache->limit);
   const enum mtl_sampler_address_mode modes[] = {
      MTL_SAMPLER_ADDRESS_MODE_REPEAT, MTL_SAMPLER_ADDRESS_MODE_MIRROR_REPEAT,
      MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER_COLOR,
      MTL_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
   };
   VkResult result = VK_SUCCESS;
   for (unsigned normalized = 0; normalized < 2 && result == VK_SUCCESS; ++normalized)
   for (unsigned compare = 0; compare < 8 && result == VK_SUCCESS; ++compare)
   for (unsigned u = 0; u < 5 && result == VK_SUCCESS; ++u)
   for (unsigned v = 0; v < 5 && result == VK_SUCCESS; ++v)
   for (unsigned w = 0; w < 5 && result == VK_SUCCESS; ++w)
   for (unsigned border = 0; border < 3 && result == VK_SUCCESS; ++border) {
      if (!normalized && (compare != MTL_COMPARE_FUNCTION_ALWAYS ||
          (u != 2 && u != 3) || (v != 2 && v != 3) || w != 2))
         continue;
      const struct mtl_sampler_packed input = {
         .mode_u = modes[u], .mode_v = modes[v], .mode_w = modes[w],
         .border_color = border, .compare_func = compare,
         .normalized_coordinates = normalized,
      };
      result = kk_sampler_native_canonical_add(dev, cache, &input);
   }
   if (result != VK_SUCCESS) {
      kk_sampler_native_cache_destroy(cache);
      kk_sampler_native_limit = prior_limit;
   } else {
      cache->registry_id = registry_id;
      cache->users = 1;
      cache->next = kk_sampler_native_caches;
      kk_sampler_native_caches = cache;
      dev->samplers.native_cache = cache;
   }
   simple_mtx_unlock(&kk_sampler_native_mutex);
   return result;
}

void
kk_sampler_cap_native_finish(struct kk_device *dev)
{
   struct kk_sampler_native_cache *cache = dev->samplers.native_cache;
   if (!cache)
      return;
   simple_mtx_lock(&kk_sampler_native_mutex);
   dev->samplers.native_cache = NULL;
   if (--cache->users == 0) {
      struct kk_sampler_native_cache **link = &kk_sampler_native_caches;
      while (*link != cache)
         link = &(*link)->next;
      *link = cache->next;
      kk_sampler_native_cache_destroy(cache);
      kk_sampler_native_limit = UINT32_MAX;
      for (cache = kk_sampler_native_caches; cache; cache = cache->next)
         kk_sampler_native_limit = MIN2(kk_sampler_native_limit, cache->limit);
   }
   simple_mtx_unlock(&kk_sampler_native_mutex);
}

uint32_t
kk_sampler_cap_native_keys(struct kk_device *dev,
                           const struct mtl_sampler_packed **keys)
{
   const struct kk_sampler_native_cache *cache = dev->samplers.native_cache;
   *keys = cache->canonical;
   return cache->canonical_count;
}

static mtl_sampler *
kk_sampler_cap_native_create(struct kk_device *dev,
                              const struct mtl_sampler_packed *packed)
{
   struct kk_sampler_native_cache *cache = dev->samplers.native_cache;
   if (!cache)
      return NULL;
   simple_mtx_lock(&kk_sampler_native_mutex);
   mtl_sampler *handle = kk_sampler_native_materialize(dev, cache, packed);
   if (handle)
      mtl_retain(handle);
   simple_mtx_unlock(&kk_sampler_native_mutex);
   return handle;
}
#endif

/* SPDX-License-Identifier: MIT */
#ifndef KK_COMPILER_CACHE_H
#define KK_COMPILER_CACHE_H

/* Disk store of the shared native compiler's Metal pipeline archive. */
#include "util/disk_cache.h"
#include "util/mesa-blake3.h"
#include "kosmickrisp/bridge/mtl_compiler.h"
#include <stdio.h>
#include <sys/utsname.h>

static inline struct disk_cache *
kk_open_disk_cache(const blake3_hash driver_digest,
                   uint32_t apple_family, uint64_t disabled_workarounds)
{
#ifdef ENABLE_SHADER_CACHE
   char renderer[32], timestamp[BLAKE3_HEX_LEN];
   snprintf(renderer, sizeof(renderer), "kosmickrisp_apple_%u", apple_family);
   _mesa_blake3_format(timestamp, driver_digest);
   return disk_cache_create(renderer, timestamp, disabled_workarounds);
#else
   return NULL;
#endif
}

/* The archive namespace. The archive holds Metal's binaries for the MSL and
 * pipeline descriptors KosmicKrisp generates, and every hint in its index is a
 * digest of that MSL, its compile options and the descriptor, so it is keyed on
 * content rather than the driver build: a shader whose generated code is
 * unchanged keeps its binary across driver updates, and any change to the MSL
 * or descriptor is a different hint. The index identity adds the OS and Metal
 * compiler versions. Change KK_ARCHIVE_CACHE_FORMAT when the index, hint or
 * piece encoding changes. compiler_domain covers device settings that select
 * different generated code, as the pipeline cache UUID does. */
#define KK_ARCHIVE_CACHE_FORMAT "kosmickrisp Metal archive 1"
static inline void
kk_archive_cache_digest(const blake3_hash compiler_domain, blake3_hash digest)
{
   blake3_hasher hasher;
   _mesa_blake3_init(&hasher);
   _mesa_blake3_update(&hasher, KK_ARCHIVE_CACHE_FORMAT, sizeof(KK_ARCHIVE_CACHE_FORMAT));
   _mesa_blake3_update(&hasher, compiler_domain, sizeof(blake3_hash));
   _mesa_blake3_final(&hasher, digest);
}

#ifdef ENABLE_SHADER_CACHE
#include "kk_compiler_cache_index.h"
/* Own factory inputs, not a physical-device/cache pointer: this compiler is
 * shared across Vulkan devices and can outlive the first physical device. */
struct kk_compiler_cache {
   uint64_t index_sequence;
   struct disk_cache *disk;
   cache_key key;
   blake3_hash driver_digest;
   uint32_t apple_family;
   uint64_t workarounds;
   bool initialized;
   struct {
      char tag[sizeof("Metal4 pipelines")];
      uint64_t registry;
      char os_version[sizeof(((struct utsname *)0)->version)];
      blake3_hash msl_salt;
      /* Metal framework version and OS build: the compiler producing the binaries. */
      char metal[256];
   } identity;
};

static bool
kk_compiler_cache_init(struct kk_compiler_cache *cache)
{
   if (!cache->initialized) {
      cache->initialized = true;
      cache->disk = kk_open_disk_cache(cache->driver_digest, cache->apple_family,
                                       cache->workarounds);
      if (cache->disk)
         disk_cache_compute_key(cache->disk, &cache->identity,
                                sizeof(cache->identity), cache->key);
   }
   return cache->disk != NULL;
}

static void *
kk_compiler_cache_load(void *data, size_t *size)
{
   struct kk_compiler_cache *cache = data;
   *size = 0;
   return kk_compiler_cache_init(cache)
      ? kk_compiler_index_load(cache->disk, cache->key, &cache->index_sequence, size)
      : NULL;
}

static bool
kk_compiler_cache_store(void *data, const void *bytes, size_t size)
{
   struct kk_compiler_cache *cache = data;
   return kk_compiler_cache_init(cache) &&
      kk_compiler_index_store(cache->disk, cache->key, &cache->index_sequence,
                             bytes, size);
}

static bool
kk_compiler_cache_available(void *data)
{
   return kk_compiler_cache_init(data);
}

static void
kk_compiler_piece_key(struct kk_compiler_cache *cache, const uint8_t id[32],
                      cache_key key)
{
   struct {
      cache_key parent;
      uint8_t content[32];
   } input = {0};
   memcpy(input.parent, cache->key, sizeof(input.parent));
   memcpy(input.content, id, sizeof(input.content));
   disk_cache_compute_key(cache->disk, &input, sizeof(input), key);
}

static void *
kk_compiler_cache_load_piece(void *data, const uint8_t id[32], size_t *size)
{
   struct kk_compiler_cache *cache = data;
   *size = 0;
   if (!kk_compiler_cache_init(cache)) return NULL;
   cache_key key;
   kk_compiler_piece_key(cache, id, key);
   return disk_cache_get(cache->disk, key, size);
}

static bool
kk_compiler_cache_store_piece(void *data, const void *bytes, size_t size,
                              uint8_t id[32])
{
   struct kk_compiler_cache *cache = data;
   if (!size || !kk_compiler_cache_init(cache)) return false;
   _mesa_blake3_compute(bytes, size, id);
   cache_key key;
   kk_compiler_piece_key(cache, id, key);
   disk_cache_put(cache->disk, key, bytes, size, NULL);
   disk_cache_wait_for_idle(cache->disk);
   size_t stored_size = 0;
   void *stored = disk_cache_get(cache->disk, key, &stored_size);
   bool complete = stored && stored_size == size && !memcmp(stored, bytes, size);
   free(stored);
   return complete;
}

static void
kk_compiler_cache_destroy(void *data)
{
   struct kk_compiler_cache *cache = data;
   if (cache->disk)
      disk_cache_destroy(cache->disk);
   free(cache);
}


static const struct mtl_compiler_cache_ops kk_compiler_cache_ops = {
   .available = kk_compiler_cache_available,
   .load = kk_compiler_cache_load,
   .store = kk_compiler_cache_store,
   .load_piece = kk_compiler_cache_load_piece,
   .store_piece = kk_compiler_cache_store_piece,
   .destroy = kk_compiler_cache_destroy,
};

/* archive_digest from kk_archive_cache_digest; metal from
 * mtl_device_get_compiler_identity. Owned by the cached compiler. */
static inline struct kk_compiler_cache *
kk_compiler_cache_new(const blake3_hash archive_digest, uint32_t apple_family,
                      uint64_t workarounds, uint64_t registry,
                      const blake3_hash msl_salt, const char metal[256])
{
   struct kk_compiler_cache *cache = calloc(1, sizeof(*cache));
   if (!cache)
      return NULL;
   memcpy(cache->driver_digest, archive_digest, sizeof(cache->driver_digest));
   cache->apple_family = apple_family;
   cache->workarounds = workarounds;
   memcpy(cache->identity.tag, "Metal4 pipelines", sizeof("Metal4 pipelines"));
   cache->identity.registry = registry;
   memcpy(cache->identity.msl_salt, msl_salt, sizeof(cache->identity.msl_salt));
   snprintf(cache->identity.metal, sizeof(cache->identity.metal), "%s", metal);
   struct utsname os = {0};
   if (!uname(&os))
      memcpy(cache->identity.os_version, os.version, sizeof(os.version));
   return cache;
}
#endif

#endif

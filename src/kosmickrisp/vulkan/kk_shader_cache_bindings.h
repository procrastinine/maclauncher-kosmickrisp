/* SPDX-License-Identifier: MIT */
#ifndef KK_SHADER_CACHE_BINDINGS_H
#define KK_SHADER_CACHE_BINDINGS_H

#include "util/mesa-blake3.h"
#include "util/blob.h"
#include "util/u_debug.h"
#include "kk_physical_device.h"
#include "vk_alloc.h"
#include "vk_sampler.h"

/* Native code and its canonical inputs share the build UUID and this format. */
#define KK_SHADER_CACHE_BINDINGS_MAGIC 0x4b4b5343u
#define KK_SHADER_CACHE_BINDINGS_VERSION 1u
#define KK_SHADER_CACHE_USES_EMBEDDED 0x1u
static_assert(KK_EMBEDDED_SAMPLER_BINDING_SCHEMA == 1u,
              "cached embedded bindings use runtime root schema1");

struct kk_shader_cache_bindings {
   uint32_t count;
   bool uses_embedded;
   const void *states;
};

static inline void
kk_shader_cache_binding_stats(const struct kk_shader *shader, bool portable)
{
   if (!debug_get_bool_option("MESA_SHADER_CACHE_SHOW_STATS", false))
      return;
   const char *code = shader->msl_data[shader->info.stage].code;
   blake3_hash digest;
   char hex[BLAKE3_HEX_LEN];
   _mesa_blake3_compute(code, strlen(code), digest);
   _mesa_blake3_format(hex, digest);
   fprintf(stderr, "KK_SHADER_CACHE_BINDINGS source=%s embedded=%u count=%u schema=%u portable=%u\n",
           hex, shader->cache_uses_embedded_samplers,
           shader->embedded_sampler_count, KK_EMBEDDED_SAMPLER_BINDING_SCHEMA,
           portable);
}

static inline bool
kk_shader_cache_write_bindings(const struct kk_shader *shader, struct blob *blob)
{
   uint32_t count = shader->embedded_sampler_count;
   /* Compiler provenance is authoritative. Missing runtime reconstruction is
    * a serialization miss, while the live in-process shader remains usable. */
   bool portable = (!shader->cache_uses_embedded_samplers || count) &&
                   (!count || (shader->embedded_sampler_states &&
                               shader->embedded_sampler_table));
   kk_shader_cache_binding_stats(shader, portable);
   if (!portable)
      return false;

   blob_write_uint32(blob, KK_SHADER_CACHE_BINDINGS_MAGIC);
   blob_write_uint32(blob, KK_SHADER_CACHE_BINDINGS_VERSION);
   blob_write_uint32(blob, KK_EMBEDDED_SAMPLER_BINDING_SCHEMA);
   blob_write_uint32(blob, sizeof(struct vk_sampler_state));
   blob_write_uint32(blob, count);
   blob_write_uint32(blob, shader->cache_uses_embedded_samplers ?
                           KK_SHADER_CACHE_USES_EMBEDDED : 0);
   size_t bytes = (size_t)count * sizeof(struct vk_sampler_state);
   blake3_hash digest;
   _mesa_blake3_compute(shader->embedded_sampler_states, bytes, digest);
   blob_write_bytes(blob, digest, sizeof(digest));
   blob_write_bytes(blob, shader->embedded_sampler_states, bytes);
   return !blob->out_of_memory;
}

static inline VkResult
kk_shader_cache_read_bindings(struct kk_device *dev, struct blob_reader *blob,
                              struct kk_shader_cache_bindings *bindings)
{
   uint32_t magic = blob_read_uint32(blob);
   uint32_t version = blob_read_uint32(blob);
   uint32_t schema = blob_read_uint32(blob);
   uint32_t state_size = blob_read_uint32(blob);
   uint32_t count = blob_read_uint32(blob);
   uint32_t flags = blob_read_uint32(blob);
   uint32_t limit = kk_device_physical(dev)->vk.properties.maxDescriptorHeapEmbeddedSamplers;
   if (blob->overrun || magic != KK_SHADER_CACHE_BINDINGS_MAGIC ||
       version != KK_SHADER_CACHE_BINDINGS_VERSION ||
       schema != KK_EMBEDDED_SAMPLER_BINDING_SCHEMA ||
       state_size != sizeof(struct vk_sampler_state) || count > limit ||
       (flags & ~KK_SHADER_CACHE_USES_EMBEDDED) ||
       ((flags & KK_SHADER_CACHE_USES_EMBEDDED) && !count))
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;

   size_t bytes;
   if (__builtin_mul_overflow((size_t)count, sizeof(struct vk_sampler_state), &bytes))
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   blake3_hash expected, actual;
   blob_copy_bytes(blob, expected, sizeof(expected));
   if (blob->overrun || bytes > (size_t)(blob->end - blob->current))
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   const void *states = blob_read_bytes(blob, bytes);
   _mesa_blake3_compute(states, bytes, actual);
   if (blob->overrun || memcmp(expected, actual, sizeof(expected)))
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   *bindings = (struct kk_shader_cache_bindings) {
      .count = count,
      .uses_embedded = flags & KK_SHADER_CACHE_USES_EMBEDDED,
      .states = states,
   };
   return VK_SUCCESS;
}

static inline VkResult
kk_shader_cache_restore_bindings(struct kk_device *dev, struct kk_shader *shader,
                                 const struct kk_shader_cache_bindings *bindings)
{
   shader->cache_uses_embedded_samplers = bindings->uses_embedded;
   if (!bindings->count)
      return VK_SUCCESS;
   /* Blob bytes can be unaligned; the public-internal initializer receives
    * an aligned canonical state array and retains its own CPU copy. */
   size_t bytes = (size_t)bindings->count * sizeof(struct vk_sampler_state);
   struct vk_sampler_state *states = vk_alloc(&dev->vk.alloc, bytes, 8,
                                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!states)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   memcpy(states, bindings->states, bytes);
   VkResult result = kk_shader_embedded_samplers_init(dev, shader,
                                                     bindings->count, states);
   vk_free(&dev->vk.alloc, states);
   return result;
}
#endif

/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_CACHE_IMPL_H
#define KK_FRAGMENT_SHADING_RATE_CACHE_IMPL_H
/* Included after common MSL [de]serialization in kk_shader.c. */
#include "kk_fragment_shading_rate_cache.h"
#include "util/simple_mtx.h"
#include "nir_serialize.h"
#include "kosmickrisp/bridge/mtl_fragment_shading_rate.h"
#include "kk_fragment_shading_rate_cells_nir.h"

#define KK_VRS_SOURCE_SCHEMA 17u
#define KK_VRS_ARTIFACT_SCHEMA 3u
#define KK_VRS_KEY_SIZE 48u
struct kk_vrs_source {
   struct kk_vrs_source_data data;
   struct kk_vrs_source *captured;
   struct kk_vrs_artifact *ready[KK_VRS_FRAGMENT_KIND_COUNT];
   uint8_t key[KK_VRS_KEY_SIZE];
   blake3_hash fine_key;
   size_t size;
   uint8_t payload[];
};
struct kk_vrs_artifact {
   struct vk_pipeline_cache_object base;
   struct msl_compile_data data;
   mtl_library *library;
   mtl_function_descriptor *function;
   enum mtl_language_version version;
   enum mtl_math_mode math;
   enum mtl_math_floating_point_functions functions;
   bool framebuffer_reads;
   uint8_t key[KK_VRS_KEY_SIZE];
};

VkResult
kk_vrs_device_init(struct kk_device *dev)
{
   if (!dev->vk.enabled_features.pipelineFragmentShadingRate &&
       !dev->vk.enabled_features.primitiveFragmentShadingRate &&
       !dev->vk.enabled_features.attachmentFragmentShadingRate)
      return VK_SUCCESS;
   dev->vrs = calloc(1, sizeof(*dev->vrs));
   if (!dev->vrs) return VK_ERROR_OUT_OF_HOST_MEMORY;
   simple_mtx_init(&dev->vrs->compile_mutex, mtx_plain);
   return VK_SUCCESS;
}

void
kk_vrs_device_finish(struct kk_device *dev)
{
   if (!dev->vrs) return;
   simple_mtx_destroy(&dev->vrs->compile_mutex);
   free(dev->vrs);
   dev->vrs = NULL;
}

static void
kk_vrs_hash_u32(struct mesa_blake3 *hash, uint32_t value)
{
   const uint8_t bytes[4] = {value, value >> 8, value >> 16, value >> 24};
   _mesa_blake3_update(hash, bytes, sizeof(bytes));
}

struct kk_vrs_source *
kk_vrs_source_create(struct kk_device *dev, const blake3_hash fine_key,
                     const struct kk_vrs_source_data *data)
{
   if (!dev->vrs || !data || !data->nir_size || !data->interface_size ||
       !data->options_size || !data->abi_size || data->uses_per_draw > 1 || data->early_fragment_tests > 1)
      return NULL;
   const void *parts[] = {data->nir, data->interface, data->epilogue,
                          data->options, data->abi};
   const uint32_t sizes[] = {data->nir_size, data->interface_size,
                             data->epilogue_size, data->options_size, data->abi_size};
   size_t size = sizeof(struct kk_vrs_source);
   for (unsigned i = 0; i < ARRAY_SIZE(parts); i++) {
      if ((sizes[i] && !parts[i]) || SIZE_MAX - size < sizes[i]) return NULL;
      size += sizes[i];
   }
   struct kk_vrs_source *source = calloc(1, size);
   if (!source) return NULL;
   source->size = size;
   source->data = *data;
   source->data.result_stride = data->result_stride ? data->result_stride : sizeof(struct kk_vrs_result);
   memcpy(source->fine_key, fine_key, sizeof(source->fine_key));
   void const **dest[] = {&source->data.nir, &source->data.interface,
      &source->data.epilogue, &source->data.options, &source->data.abi};
   uint8_t *next = source->payload;
   struct mesa_blake3 hash;
   _mesa_blake3_init(&hash);
   kk_vrs_hash_u32(&hash, KK_VRS_SOURCE_SCHEMA);
   kk_vrs_hash_u32(&hash, source->data.result_stride);
   kk_vrs_hash_u32(&hash, source->data.uses_per_draw);
   kk_vrs_hash_u32(&hash, source->data.early_fragment_tests);
   _mesa_blake3_update(&hash, fine_key, sizeof(source->fine_key));
   for (unsigned i = 0; i < ARRAY_SIZE(parts); i++) {
      *dest[i] = next;
      if (sizes[i]) memcpy(next, parts[i], sizes[i]);
      kk_vrs_hash_u32(&hash, sizes[i]);
      _mesa_blake3_update(&hash, next, sizes[i]);
      next += sizes[i];
   }
   /* Distinct fixed-size type prefix and explicit little-endian schema. */
   memcpy(source->key, "KK-VRS-FRAG", 11);
   source->key[12] = KK_VRS_ARTIFACT_SCHEMA;
   _mesa_blake3_final(&hash, source->key + 16);
   return source;
}

size_t
kk_vrs_source_size(const struct kk_vrs_source *source)
{
   return source ? source->size + kk_vrs_source_size(source->captured) : 0;
}

void
kk_vrs_source_attach_captured(struct kk_vrs_source *source, struct kk_vrs_source *captured)
{
   assert(source && !source->captured && source != captured && (!captured || !captured->captured));
   source->captured = captured;
}

struct kk_vrs_source *
kk_vrs_source_captured(const struct kk_vrs_source *source)
{
   return source ? source->captured : NULL;
}

uint32_t
kk_vrs_source_result_stride(const struct kk_vrs_source *source)
{
   return source ? source->data.result_stride : 0;
}

const struct kk_vrs_source_data *
kk_vrs_source_data(const struct kk_vrs_source *source)
{
   return source ? &source->data : NULL;
}

void
kk_vrs_artifact_unref(struct kk_device *dev, struct kk_vrs_artifact *artifact)
{
   if (artifact) vk_pipeline_cache_object_unref(&dev->vk, &artifact->base);
}

void
kk_vrs_source_destroy(struct kk_device *dev, struct kk_vrs_source *source)
{
   if (!source) return;
   kk_vrs_source_destroy(dev, source->captured);
   for (unsigned i = 0; i < KK_VRS_FRAGMENT_KIND_COUNT; i++)
      kk_vrs_artifact_unref(dev, source->ready[i]);
   free(source);
}

void
kk_vrs_source_serialize(const struct kk_vrs_source *source, struct blob *blob)
{
   blob_write_uint32(blob, source ? KK_VRS_SOURCE_SCHEMA : 0);
   if (!source) return;
   blob_write_bytes(blob, source->fine_key, sizeof(source->fine_key));
   blob_write_uint32(blob, source->data.result_stride);
   blob_write_uint32(blob, source->data.uses_per_draw);
   blob_write_uint32(blob, source->data.early_fragment_tests);
   const void *parts[] = {source->data.nir, source->data.interface,
      source->data.epilogue, source->data.options, source->data.abi};
   const uint32_t sizes[] = {source->data.nir_size, source->data.interface_size,
      source->data.epilogue_size, source->data.options_size, source->data.abi_size};
   for (unsigned i = 0; i < ARRAY_SIZE(parts); i++) {
      blob_write_uint32(blob, sizes[i]);
      blob_write_bytes(blob, parts[i], sizes[i]);
   }
   kk_vrs_source_serialize(source->captured, blob);
}

static VkResult
kk_vrs_source_deserialize_depth(struct kk_device *dev, struct blob_reader *blob,
                          struct kk_vrs_source **out, unsigned depth)
{
   *out = NULL;
   uint32_t schema = blob_read_uint32(blob);
   if (blob->overrun) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   if (!schema) return VK_SUCCESS;
   if (depth > 1 || schema != KK_VRS_SOURCE_SCHEMA || !dev->vrs)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   blake3_hash fine;
   blob_copy_bytes(blob, fine, sizeof(fine));
   struct kk_vrs_source_data data = {
      .result_stride = blob_read_uint32(blob), .uses_per_draw = blob_read_uint32(blob),
      .early_fragment_tests = blob_read_uint32(blob),
   };
   if (data.early_fragment_tests > 1 || data.uses_per_draw > 1 || data.result_stride < sizeof(struct kk_vrs_result) ||
       data.result_stride % 16 ||
       data.result_stride > sizeof(struct kk_vrs_result) + FRAG_RESULT_MAX * 8 * 4 * 8)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   void const **parts[] = {&data.nir, &data.interface, &data.epilogue,
                           &data.options, &data.abi};
   uint32_t *sizes[] = {&data.nir_size, &data.interface_size,
                        &data.epilogue_size, &data.options_size, &data.abi_size};
   for (unsigned i = 0; i < ARRAY_SIZE(parts); i++) {
      *sizes[i] = blob_read_uint32(blob);
      *parts[i] = blob_read_bytes(blob, *sizes[i]);
   }
   if (blob->overrun || !data.nir_size || !data.interface_size ||
       !data.options_size || !data.abi_size)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   struct kk_vrs_source *source = kk_vrs_source_create(dev, fine, &data);
   if (!source) return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult result = kk_vrs_source_deserialize_depth(dev, blob, &source->captured, depth + 1);
   if (result != VK_SUCCESS) { kk_vrs_source_destroy(dev, source); return result; }
   *out = source;
   return VK_SUCCESS;
}

VkResult
kk_vrs_source_deserialize(struct kk_device *dev, struct blob_reader *blob,
                          struct kk_vrs_source **out)
{
   return kk_vrs_source_deserialize_depth(dev, blob, out, 0);
}

static bool
kk_vrs_artifact_serialize(struct vk_pipeline_cache_object *base, struct blob *blob)
{
   struct kk_vrs_artifact *artifact = container_of(base, struct kk_vrs_artifact, base);
   blob_write_uint32(blob, KK_VRS_ARTIFACT_SCHEMA);
   blob_write_uint32(blob, artifact->version);
   blob_write_uint32(blob, artifact->math);
   blob_write_uint32(blob, artifact->functions);
   blob_write_uint32(blob, artifact->framebuffer_reads);
   /* The common helper includes requires_msl41 when that dependency lands. */
   kk_msl_serialize(&artifact->data, blob);
   return !blob->out_of_memory;
}

static void
kk_vrs_artifact_destroy(struct vk_device *device,
                        struct vk_pipeline_cache_object *base)
{
   struct kk_vrs_artifact *artifact = container_of(base, struct kk_vrs_artifact, base);
   mtl_release(artifact->function);
   mtl_release(artifact->library);
   ralloc_free(artifact->data.code);
   ralloc_free(artifact->data.entrypoint_name);
   vk_pipeline_cache_object_finish(base);
   free(artifact);
}

static VkResult
kk_vrs_artifact_native(struct kk_device *dev, struct kk_vrs_artifact *artifact)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);
   if ((artifact->version != MTL_LANGUAGE_VERSION_4_0 &&
        artifact->version != MTL_LANGUAGE_VERSION_4_1) ||
       artifact->version > pdev->info.msl_version ||
       artifact->math > MTL_MATH_MODE_FAST ||
       artifact->functions > MTL_MATH_FLOATING_POINT_FUNCTIONS_PRECISE ||
       !artifact->data.code || !artifact->data.entrypoint_name)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   /* Version is the result of the common selector at compilation, retained in
    * this canonical artifact. There is no independent language selection. */
   artifact->library = mtl_new_library(dev->mtl_compiler_handle,
      artifact->data.code, artifact->version, artifact->math, artifact->functions);
   if (!artifact->library) return VK_ERROR_INVALID_SHADER_NV;
   return kk_msl_function(dev, &artifact->data, artifact->library,
      artifact->data.entrypoint_name, &artifact->function);
}

static const struct vk_pipeline_cache_object_ops kk_vrs_artifact_ops;
static struct vk_pipeline_cache_object *
kk_vrs_artifact_deserialize(struct vk_pipeline_cache *cache,
   const void *key, size_t key_size, struct blob_reader *blob)
{
   if (key_size != KK_VRS_KEY_SIZE || memcmp(key, "KK-VRS-FRAG", 11) ||
       ((const uint8_t *)key)[12] != KK_VRS_ARTIFACT_SCHEMA ||
       ((const uint8_t *)key)[13] >= KK_VRS_FRAGMENT_KIND_COUNT ||
       blob_read_uint32(blob) != KK_VRS_ARTIFACT_SCHEMA)
      return NULL;
   struct kk_vrs_artifact *artifact = calloc(1, sizeof(*artifact));
   if (!artifact) return NULL;
   memcpy(artifact->key, key, key_size);
   vk_pipeline_cache_object_init(cache->base.device, &artifact->base,
      &kk_vrs_artifact_ops, artifact->key, key_size);
   artifact->version = blob_read_uint32(blob);
   artifact->math = blob_read_uint32(blob);
   artifact->functions = blob_read_uint32(blob);
   uint32_t reads = blob_read_uint32(blob);
   if (reads > 1) goto fail;
   artifact->framebuffer_reads = reads;
   /* Validate lengths before common allocation, without depending on the
    * common format's optional requirement byte or aliasing aligned structs. */
   struct blob_reader lengths = *blob;
   uint32_t entry = blob_read_uint32(&lengths), code = blob_read_uint32(&lengths);
   if (lengths.overrun || !entry || !code ||
       (uint64_t)entry + code > (uint64_t)(lengths.end - lengths.current))
      goto fail;
   if (kk_msl_deserialize(blob, &artifact->data) != VK_SUCCESS ||
       blob->current != blob->end ||
       artifact->data.code[code - 1] || artifact->data.entrypoint_name[entry - 1])
      goto fail;
   struct kk_device *dev = container_of(cache->base.device, struct kk_device, vk);
   /* lookup_object invokes this while the VRS-only compile mutex is held. */
   if (kk_vrs_artifact_native(dev, artifact) != VK_SUCCESS) goto fail;
   return &artifact->base;
fail:
   kk_vrs_artifact_destroy(cache->base.device, &artifact->base);
   return NULL;
}

static const struct vk_pipeline_cache_object_ops kk_vrs_artifact_ops = {
   .serialize = kk_vrs_artifact_serialize,
   .deserialize = kk_vrs_artifact_deserialize,
   .destroy = kk_vrs_artifact_destroy,
};

VkResult
kk_vrs_artifact_get(struct kk_device *dev, struct kk_vrs_source *source,
                    enum kk_vrs_fragment_kind kind, kk_vrs_build_fragment build,
                    struct kk_vrs_artifact **out)
{
   *out = NULL;
   if (!dev->vrs || !source || !build || (unsigned)kind >= KK_VRS_FRAGMENT_KIND_COUNT)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   uint8_t key[KK_VRS_KEY_SIZE];
   memcpy(key, source->key, sizeof(key));
   key[13] = kind;
   simple_mtx_lock(&dev->vrs->compile_mutex);
   struct kk_vrs_artifact *artifact = source->ready[kind];
   VkResult result = VK_SUCCESS;
   if (!artifact && dev->vk.mem_cache) {
      struct vk_pipeline_cache_object *base = vk_pipeline_cache_lookup_object(
         dev->vk.mem_cache, key, sizeof(key), &kk_vrs_artifact_ops, NULL);
      if (base) artifact = container_of(base, struct kk_vrs_artifact, base);
   }
   if (!artifact) {
      artifact = calloc(1, sizeof(*artifact));
      if (!artifact) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto unlock; }
      memcpy(artifact->key, key, sizeof(artifact->key));
      vk_pipeline_cache_object_init(&dev->vk, &artifact->base,
         &kk_vrs_artifact_ops, artifact->key, sizeof(artifact->key));
      result = build(dev, &source->data, kind, &artifact->data, &artifact->version,
                      &artifact->math, &artifact->functions, &artifact->framebuffer_reads);
      if (result == VK_SUCCESS) result = kk_vrs_artifact_native(dev, artifact);
      if (result != VK_SUCCESS) {
         kk_vrs_artifact_destroy(&dev->vk, &artifact->base);
         artifact = NULL;
         goto unlock;
      }
      if (dev->vk.mem_cache) {
         struct vk_pipeline_cache_object *canonical = vk_pipeline_cache_add_object(
            dev->vk.mem_cache, &artifact->base);
         artifact = container_of(canonical, struct kk_vrs_artifact, base);
      }
      /* A NULL/disabled common cache is respected; no replacement store. */
   }
   if (!source->ready[kind])
      source->ready[kind] = artifact; /* Own the returned canonical ref. */
   *out = container_of(vk_pipeline_cache_object_ref(&artifact->base),
                       struct kk_vrs_artifact, base);
unlock:
   simple_mtx_unlock(&dev->vrs->compile_mutex);
   /* No attachment-cache or encoder lock is entered while this lock is held. */
   return result;
}

mtl_function_descriptor *
kk_vrs_artifact_function(const struct kk_vrs_artifact *artifact)
{
   return artifact ? artifact->function : NULL;
}

bool
kk_vrs_artifact_uses_pipeline_statistics(const struct kk_vrs_artifact *artifact)
{
   return artifact && artifact->data.uses_pipeline_statistics;
}

bool
kk_vrs_artifact_framebuffer_reads(const struct kk_vrs_artifact *artifact)
{
   return artifact && artifact->framebuffer_reads;
}

/* Source is the actual linked, once-lowered callable FS. Runtime embedded
 * bindings retain their original stage and ordinal through the shared root. */
struct kk_vrs_source *
kk_vrs_source_capture(struct kk_device *dev, nir_shader *nir,
   const struct kk_per_vertex_map *map,
   const struct vk_graphics_pipeline_state *state, const struct msl_compile_data *fine)
{
   if (!fine || !fine->code || !fine->entrypoint_name) return NULL;
   struct blob code, interface, epilogue, options, abi;
   blob_init(&code); blob_init(&interface); blob_init(&epilogue);
   blob_init(&options); blob_init(&abi);
   nir_serialize(&code, nir, false);
   blob_write_uint32(&interface, map && map->active);
   blob_write_uint32(&interface, map && map->canonical_heap);
   blob_write_uint32(&interface, map && map->projected_basis);
   blob_write_uint64(&interface, map ? map->locations : 0);
   blob_write_uint64(&interface, map ? map->buffered_locations : 0);
   blob_write_uint64(&interface, map ? map->unused_locations : 0);
   blob_write_uint64(&interface, map ? map->flat_locations : 0);
   for (unsigned i = 0; i < VARYING_SLOT_MAX; i++)
      for (unsigned v = 0; v < 3; v++)
         blob_write_uint32(&interface, map ? map->slot[i][v] : 0);
   blob_write_uint32(&interface, map ? map->barycentric_slot[0] : 0);
   blob_write_uint32(&interface, map ? map->barycentric_slot[1] : 0);
   blob_write_uint32(&interface, map && map->buffer);
   blob_write_uint32(&interface, map ? map->buffer_slot : 0);
   blob_write_uint32(&interface, map && map->out_of_slots);
   unsigned count = state && state->rp ? state->rp->color_attachment_count : 0;
   blob_write_uint32(&epilogue, count);
   for (unsigned i = 0; i < count; i++) {
      blob_write_uint32(&epilogue, state->rp->color_attachment_formats[i]);
      blob_write_uint32(&epilogue, kk_get_logical_color_att_index(state, i));
   }
   blob_write_uint32(&epilogue, state && state->ms ? state->ms->rasterization_samples : 1);
   struct kk_physical_device *pdev = kk_device_physical(dev);
   blob_write_uint64(&options, pdev->settings.disabled_workarounds);
   blob_write_uint32(&options, pdev->info.msl_version);
   blob_write_uint32(&options, MTL_MATH_MODE_FAST);
   blob_write_uint32(&options, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
   blob_write_bytes(&abi, pdev->vk.properties.shaderBinaryUUID, VK_UUID_SIZE);
   blob_write_uint32(&abi, nir_num_intrinsics);
   blob_write_uint32(&abi, nir_num_opcodes);
   blob_write_uint32(&abi, KK_VRS_SOURCE_SCHEMA);
   blob_write_uint32(&abi, KK_EMBEDDED_SAMPLER_BINDING_SCHEMA);
   struct mesa_blake3 hash;
   _mesa_blake3_init(&hash);
   kk_vrs_hash_u32(&hash, strlen(fine->entrypoint_name) + 1);
   _mesa_blake3_update(&hash, fine->entrypoint_name, strlen(fine->entrypoint_name) + 1);
   kk_vrs_hash_u32(&hash, strlen(fine->code) + 1);
   _mesa_blake3_update(&hash, fine->code, strlen(fine->code) + 1);
   blake3_hash fine_code_key;
   _mesa_blake3_final(&hash, fine_code_key);
   struct kk_vrs_source_data data = {
      .nir = code.data, .nir_size = code.size,
      .result_stride = kk_nir_vrs_result_stride(nir),
      .uses_per_draw = kk_nir_vrs_uses_per_draw(nir),
      .early_fragment_tests = nir->info.fs.early_fragment_tests,
      .interface = interface.data, .interface_size = interface.size,
      .epilogue = epilogue.data, .epilogue_size = epilogue.size,
      .options = options.data, .options_size = options.size,
      .abi = abi.data, .abi_size = abi.size,
   };
   struct kk_vrs_source *source = NULL;
   if (data.result_stride && !code.out_of_memory && !interface.out_of_memory && !epilogue.out_of_memory &&
       !options.out_of_memory && !abi.out_of_memory)
      source = kk_vrs_source_create(dev, fine_code_key, &data);
   blob_finish(&code); blob_finish(&interface); blob_finish(&epilogue);
   blob_finish(&options); blob_finish(&abi);
   return source;
}
/* Reconstruct only the immutable emission inputs. The application, fine
 * epilogue, descriptors and actual interface have already been lowered once. */
static VkResult
kk_vrs_build_linked_fragment(struct kk_device *dev,
   const struct kk_vrs_source_data *source, enum kk_vrs_fragment_kind kind,
   struct msl_compile_data *out, enum mtl_language_version *version,
   enum mtl_math_mode *math,
   enum mtl_math_floating_point_functions *functions, bool *framebuffer_reads)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);
   struct blob_reader abi, options, epilogue;
   blob_reader_init(&abi, source->abi, source->abi_size);
   uint8_t uuid[VK_UUID_SIZE];
   blob_copy_bytes(&abi, uuid, sizeof(uuid));
   uint32_t intrinsics = blob_read_uint32(&abi);
   uint32_t opcodes = blob_read_uint32(&abi);
   uint32_t schema = blob_read_uint32(&abi);
   uint32_t embedded_schema = blob_read_uint32(&abi);
   if (abi.overrun || abi.current != abi.end ||
       memcmp(uuid, pdev->vk.properties.shaderBinaryUUID, sizeof(uuid)) ||
       intrinsics != nir_num_intrinsics || opcodes != nir_num_opcodes ||
       schema != KK_VRS_SOURCE_SCHEMA ||
       embedded_schema != KK_EMBEDDED_SAMPLER_BINDING_SCHEMA)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;

   blob_reader_init(&options, source->options, source->options_size);
   uint64_t workarounds = blob_read_uint64(&options);
   uint32_t supported = blob_read_uint32(&options);
   *math = blob_read_uint32(&options);
   *functions = blob_read_uint32(&options);
   if (options.overrun || options.current != options.end ||
       workarounds != pdev->settings.disabled_workarounds ||
       supported != pdev->info.msl_version)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;

   struct vk_render_pass_state rp = {0};
   struct vk_color_attachment_location_state cal;
   memset(cal.color_map, MESA_VK_ATTACHMENT_UNUSED, sizeof(cal.color_map));
   blob_reader_init(&epilogue, source->epilogue, source->epilogue_size);
   uint32_t count = blob_read_uint32(&epilogue);
   if (epilogue.overrun || count > ARRAY_SIZE(rp.color_attachment_formats))
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   rp.color_attachment_count = count;
   for (unsigned i = 0; i < count; i++) {
      rp.color_attachment_formats[i] = blob_read_uint32(&epilogue);
      uint32_t location = blob_read_uint32(&epilogue);
      if (location >= MAX_DRAW_BUFFERS && location != MESA_VK_ATTACHMENT_UNUSED)
         return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
      cal.color_map[i] = location;
   }
   uint32_t samples = blob_read_uint32(&epilogue);
   if (epilogue.overrun || epilogue.current != epilogue.end ||
       !samples || (samples & (samples - 1)) || samples > 64)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   struct vk_graphics_pipeline_state state = {.rp = &rp, .cal = &cal};

   glsl_type_singleton_init_or_ref();
   struct blob_reader code;
   blob_reader_init(&code, source->nir, source->nir_size);
   nir_shader *nir = nir_deserialize(NULL, &kk_nir_options, &code);
   VkResult result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   if (!nir || code.overrun || code.current != code.end ||
       kk_nir_vrs_result_stride(nir) != source->result_stride ||
       kk_nir_vrs_uses_per_draw(nir) != source->uses_per_draw ||
       nir->info.fs.early_fragment_tests != source->early_fragment_tests) goto finish;
   bool lowered = kind == KK_VRS_COARSE_BODY
      ? kk_nir_lower_linked_native_coarse_fragment(nir)
      : kk_nir_lower_linked_replay_fragment(nir, samples);
   if (!lowered) {
      result = VK_ERROR_FEATURE_NOT_PRESENT;
      goto finish;
   }
   kk_compile_msl_options(dev, nir, &state, out, NULL, framebuffer_reads, NULL);
   nir = NULL; /* The common compiler consumes its compilation clone. */
   if (out->requires_msl41 && pdev->info.msl_version < MTL_LANGUAGE_VERSION_4_1) {
      result = VK_ERROR_FEATURE_NOT_PRESENT;
      goto finish;
   }
   *version = kk_msl_language_version(out, pdev->info.msl_version);
   result = VK_SUCCESS;
finish:
   ralloc_free(nir);
   glsl_type_singleton_decref();
   return result;
}

VkResult
kk_vrs_fragment_get(struct kk_device *dev, struct kk_vrs_source *source,
   enum kk_vrs_fragment_kind kind, struct kk_vrs_artifact **out)
{
   return kk_vrs_artifact_get(dev, source, kind, kk_vrs_build_linked_fragment, out);
}

#include "kk_fragment_shading_rate_support_cache.h"

static VkResult
kk_vrs_pipelines_for_source(struct kk_device *dev,
   mtl_render_pipeline_descriptor *raster_descriptor, struct kk_vrs_source *source,
   bool statistics, struct mtl_vrs_pipelines **out)
{
   *out = NULL;
   if (!raster_descriptor || !source)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   struct kk_vrs_artifact *body = NULL, *replay = NULL;
   struct kk_vrs_support *support = NULL;
   VkResult result = kk_vrs_fragment_get(dev, source,
                                        KK_VRS_COARSE_BODY, &body);
   if (result != VK_SUCCESS) goto finish;
   result = kk_vrs_fragment_get(dev, source, KK_VRS_FINE_REPLAY, &replay);
   if (result != VK_SUCCESS) goto finish;
   result = kk_vrs_support_get(dev, MAX2(body->version, replay->version),
                               body->math, body->functions, &support);
   if (result != VK_SUCCESS) goto finish;
   /* Each get has released the compilation mutex. Enter the native attachment
    * factory only with the caller's actual producer descriptor and interface. */
   /* Only the actual application artifact owns statistics. A query cannot
    * specialize replay/support work or manufacture another source artifact. */
   statistics &= kk_vrs_artifact_uses_pipeline_statistics(body);
   *out = mtl_vrs_new_shared_native_pipelines_for_statistics(dev->mtl_compiler_handle,
      raster_descriptor, body->function, replay->function,
      replay->framebuffer_reads, MAX2(body->version, replay->version),
      body->math, body->functions, source->data.result_stride,
      source->data.uses_per_draw,
      source->data.early_fragment_tests, support->library, statistics);
   if (!*out) result = VK_ERROR_INVALID_SHADER_NV;
finish:
   if (support) vk_pipeline_cache_object_unref(&dev->vk, &support->base);
   kk_vrs_artifact_unref(dev, replay);
   kk_vrs_artifact_unref(dev, body);
   return result;
}
VkResult
kk_vrs_native_pipelines_create_for_raster_statistics(struct kk_device *dev,
   mtl_render_pipeline_descriptor *raster_descriptor, const struct kk_shader *fragment,
   bool statistics, struct mtl_vrs_pipelines **out)
{
   return kk_vrs_pipelines_for_source(dev, raster_descriptor,
      fragment ? fragment->vrs_source : NULL, statistics, out);
}

VkResult
kk_vrs_native_pipelines_create_for_raster(struct kk_device *dev,
   mtl_render_pipeline_descriptor *raster_descriptor, const struct kk_shader *fragment,
   struct mtl_vrs_pipelines **out)
{
   return kk_vrs_native_pipelines_create_for_raster_statistics(dev,
      raster_descriptor, fragment, false, out);
}

VkResult
kk_vrs_captured_pipelines_create_statistics(struct kk_device *dev,
   mtl_render_pipeline_descriptor *raster_descriptor, const struct kk_shader *fragment,
   bool statistics, struct mtl_vrs_pipelines **out)
{
   return kk_vrs_pipelines_for_source(dev, raster_descriptor,
      fragment ? kk_vrs_source_captured(fragment->vrs_source) : NULL, statistics, out);
}

VkResult
kk_vrs_captured_pipelines_create(struct kk_device *dev,
   mtl_render_pipeline_descriptor *raster_descriptor, const struct kk_shader *fragment,
   struct mtl_vrs_pipelines **out)
{
   return kk_vrs_captured_pipelines_create_statistics(dev,
      raster_descriptor, fragment, false, out);
}

VkResult
kk_vrs_native_pipelines_create_statistics(struct kk_device *dev,
   const struct kk_shader *producer, const struct kk_shader *fragment,
   bool statistics, struct mtl_vrs_pipelines **out)
{
   return kk_vrs_native_pipelines_create_for_raster_statistics(dev,
      kk_vrs_native_pipeline_descriptor(producer), fragment, statistics, out);
}

VkResult
kk_vrs_native_pipelines_create(struct kk_device *dev,
   const struct kk_shader *producer, const struct kk_shader *fragment,
   struct mtl_vrs_pipelines **out)
{
   return kk_vrs_native_pipelines_create_statistics(dev,
      producer, fragment, false, out);
}
#endif

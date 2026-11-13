/* SPDX-License-Identifier: MIT
 * Lazily translated compute shaders.
 *
 * A compute pipeline created for a deferred build (kk_async_compute.c) keeps
 * its compiler input as a recipe (kk_recipe_write) and returns without
 * translating. Its deferred build translates the recipe (NIR lowering and
 * MSL), then compiles the native library and pipeline, under the same quiet
 * gate, prediction and promotion as any deferred build; the first bind joins
 * it. Translation runs on a compiler worker with the default allocator, so
 * the device allocator and the application's allocation callbacks are used
 * only on API threads.
 *
 * Shaders translate at creation instead when they have descriptor-heap
 * embedded samplers or are indirect bindable (both allocate device objects
 * during translation), and whenever the creation cannot defer (creation
 * feedback, compile-required failure, capture flags, more than one pipeline).
 *
 * The serialized form of a lazily created shader is always its recipe, so
 * vkGetPipelineCacheData reports the same size before and after the build.
 * The translated MSL and metadata are stored in the Mesa disk cache under a
 * digest of the recipe; a later build of the same recipe restores them and
 * compiles only the native pipeline, as a restored shader does.
 */
#ifndef KK_LAZY_COMPUTE_H
#define KK_LAZY_COMPUTE_H
#include "kk_async_compute.h"
#include "kk_atomic64.h"
#include "util/disk_cache.h"
#include "vk_alloc.h"

#define KK_LAZY_COMPUTE_MAGIC 0x4b4b4c43u /* "KKLC" */

static bool
kk_lazy_compute_binary(const struct blob_reader *blob)
{
   uint32_t magic;
   if ((size_t)(blob->end - blob->current) < sizeof(magic))
      return false;
   memcpy(&magic, blob->current, sizeof(magic));
   return magic == KK_LAZY_COMPUTE_MAGIC;
}

/* The disk-cache key of the translated shader: a digest of the complete
 * compiler input and the residency classes of its native code. */
static void
kk_lazy_compute_key(struct disk_cache *disk, const void *recipe, size_t size, uint32_t modes,
                    cache_key key)
{
   struct {
      char tag[sizeof("KosmicKrisp translated compute 1")];
      blake3_hash recipe;
      uint32_t modes;
   } input;
   memset(&input, 0, sizeof(input));
   memcpy(input.tag, "KosmicKrisp translated compute 1", sizeof(input.tag));
   _mesa_blake3_compute(recipe, size, input.recipe);
   input.modes = modes;
   disk_cache_compute_key(disk, &input, sizeof(input), key);
}

/* The translated shader stored for a recipe; VK_INCOMPLETE when there is
 * none. Under kk_translate_only the native pipeline is not compiled. */
static VkResult
kk_lazy_compute_restore(struct kk_device *dev, struct disk_cache *disk, const cache_key key,
                        const VkAllocationCallbacks *alloc, struct kk_shader **out)
{
   size_t size = 0;
   void *data = disk_cache_get(disk, key, &size);
   if (!data)
      return VK_INCOMPLETE;
   struct blob_reader reader;
   blob_reader_init(&reader, data, size);
   struct vk_shader *restored = NULL;
   VkResult result = kk_deserialize_shader_native(&dev->vk, &reader,
      kk_device_physical(dev)->vk.properties.shaderBinaryVersion, alloc, &restored);
   free(data);
   if (result != VK_SUCCESS)
      return VK_INCOMPLETE;
   struct kk_shader *shader = container_of(restored, struct kk_shader, vk);
   if (shader->info.stage != MESA_SHADER_COMPUTE || !shader->msl_data[MESA_SHADER_COMPUTE].code ||
       shader->info.indirect_bindable || shader->embedded_sampler_count) {
      kk_shader_destroy(&dev->vk, shader, alloc);
      return VK_INCOMPLETE;
   }
   *out = shader;
   return VK_SUCCESS;
}

/* Moves the translation into the published shader. Before the join, other
 * threads read only its stage, residency classes and sparse state, so those
 * bytes keep the values creation wrote. */
static void
kk_lazy_compute_adopt(struct kk_shader *shader, struct kk_shader *built)
{
   char *dst = (char *)&shader->info;
   const char *src = (const char *)&built->info;
   const size_t stage = offsetof(struct kk_shader_info, stage);
   const size_t stage_end = stage + sizeof(shader->info.stage);
   const size_t sparse = offsetof(struct kk_shader_info, sparse_modes);
   const size_t sparse_end = sparse + sizeof(shader->info.sparse_modes);
   static_assert(offsetof(struct kk_shader_info, stage) + sizeof(mesa_shader_stage) <=
                 offsetof(struct kk_shader_info, sparse_modes),
                 "the preserved fields are ordered");
   memcpy(dst, src, stage);
   memcpy(dst + stage_end, src + stage_end, sparse - stage_end);
   memcpy(dst + sparse_end, src + sparse_end, sizeof(shader->info) - sparse_end);
   shader->msl_data[MESA_SHADER_COMPUTE] = built->msl_data[MESA_SHADER_COMPUTE];
   memset(&built->msl_data[MESA_SHADER_COMPUTE], 0, sizeof(built->msl_data[0]));
   shader->cache_uses_embedded_samplers = built->cache_uses_embedded_samplers;
}

/* The deferred build of a lazily created shader, on a compiler worker. */
static VkResult
kk_compile_lazy_compute(struct kk_device *dev, struct kk_shader *shader)
{
   const struct kk_lazy_compute *lazy = shader->lazy;
   const VkAllocationCallbacks *alloc = vk_default_allocator();
   struct disk_cache *disk = dev->vk.physical->disk_cache;
   cache_key key;
   struct kk_shader *built = NULL;
   VkResult result = VK_INCOMPLETE;
   kk_translate_only = true;
   if (disk) {
      kk_lazy_compute_key(disk, lazy->recipe, lazy->recipe_size, lazy->modes, key);
      result = kk_lazy_compute_restore(dev, disk, key, alloc, &built);
   }
   const bool restored = result == VK_SUCCESS;
   if (!restored)
      result = kk_recipe_compile(dev, lazy->recipe, lazy->recipe_size, lazy->required_modes,
                                 lazy->modes, lazy->modes, alloc, &built);
   kk_translate_only = false;
   if (result != VK_SUCCESS)
      return result;
   if (!restored && (built->info.stage != MESA_SHADER_COMPUTE ||
                     !built->msl_data[MESA_SHADER_COMPUTE].code ||
                     built->info.indirect_bindable || built->embedded_sampler_count)) {
      kk_shader_destroy(&dev->vk, built, alloc);
      return VK_ERROR_INVALID_SHADER_NV;
   }
   if (!restored && disk) {
      struct blob blob;
      blob_init(&blob);
      if (kk_shader_serialize_native(&dev->vk, &built->vk, &blob) && !blob.out_of_memory)
         disk_cache_put(disk, key, blob.data, blob.size, NULL);
      blob_finish(&blob);
   }
   kk_lazy_compute_adopt(shader, built);
   kk_shader_destroy(&dev->vk, built, alloc);
   return kk_compile_async_compute(dev, shader);
}

/* Creates the published shader and starts its deferred build. The recipe is
 * borrowed when shared is set, otherwise owned (own is freed with it).
 * Returns VK_INCOMPLETE, owning nothing, when the shader must translate now. */
static VkResult
kk_lazy_compute_new(struct kk_device *dev, const void *recipe, size_t size, void *own,
                    uint32_t required_modes, uint32_t modes,
                    const VkAllocationCallbacks *alloc, struct kk_shader **out)
{
   if (kk_atomic64_prepare(dev) != VK_SUCCESS)
      return VK_INCOMPLETE;
   struct kk_lazy_compute *lazy = malloc(sizeof(*lazy));
   if (!lazy)
      return VK_INCOMPLETE;
   *lazy = (struct kk_lazy_compute) {
      .recipe = recipe, .recipe_size = size, .own = own,
      .required_modes = required_modes, .modes = modes,
   };
   struct kk_shader *shader = vk_shader_zalloc(&dev->vk, &kk_shader_ops, MESA_SHADER_COMPUTE,
                                               alloc, kk_shader_size(MESA_SHADER_COMPUTE));
   if (!shader) {
      free(lazy);
      return VK_INCOMPLETE;
   }
   kk_shader_init_mesh_state(shader, MESA_SHADER_COMPUTE);
   shader->info.stage = MESA_SHADER_COMPUTE;
   shader->info.sparse_modes = modes;
   shader->lazy = lazy;
   if (!kk_async_compute_start(dev, shader, kk_compile_lazy_compute)) {
      shader->lazy = NULL;
      free(lazy);
      vk_shader_free(&dev->vk, alloc, &shader->vk);
      return VK_INCOMPLETE;
   }
   *out = shader;
   return VK_SUCCESS;
}

static VkResult
kk_lazy_compute_create(struct kk_device *dev, uint32_t count,
   const struct vk_shader_compile_info *infos, const struct vk_graphics_pipeline_state *graphics,
   const struct vk_features *features, uint32_t modes, uint32_t always_modes,
   const struct kk_sparse_shader_state *state, const VkAllocationCallbacks *alloc,
   struct vk_shader **out)
{
   if (!kk_defer_compute || dev->eager_compute_translation || count != 1 || graphics ||
       infos[0].stage != MESA_SHADER_COMPUTE ||
       (infos[0].flags & VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT) ||
       infos[0].embedded_sampler_count)
      return VK_INCOMPLETE;
   const void *recipe;
   size_t size;
   void *own = NULL;
   uint32_t required_modes = always_modes;
   if (state) {
      /* The sparse state already holds this input; its header records the
       * guarded classes. */
      recipe = state->recipe.data;
      size = state->recipe.size;
      required_modes = modes;
   } else {
      struct blob blob;
      blob_init(&blob);
      kk_recipe_write(&blob, 1, infos, NULL, features, always_modes, always_modes);
      if (blob.out_of_memory) {
         blob_finish(&blob);
         return VK_INCOMPLETE;
      }
      blob_finish_get_buffer(&blob, &own, &size);
      recipe = own;
   }
   struct kk_shader *shader;
   VkResult result = kk_lazy_compute_new(dev, recipe, size, own, required_modes,
                                         always_modes, alloc, &shader);
   if (result != VK_SUCCESS) {
      free(own);
      return result;
   }
   ralloc_free(infos[0].nir);
   out[0] = &shader->vk;
   return VK_SUCCESS;
}

/* The same bytes whether or not the build has translated the shader. A
 * failed build is not cached. */
static bool
kk_lazy_compute_serialize(const struct kk_shader *shader, struct blob *blob)
{
   if (!kk_async_compute_serializable((struct kk_shader *)shader))
      return false;
   const struct kk_lazy_compute *lazy = shader->lazy;
   if (lazy->recipe_size > UINT32_MAX)
      return false;
   blob_write_uint32(blob, KK_LAZY_COMPUTE_MAGIC);
   blob_write_uint32(blob, lazy->required_modes);
   blob_write_uint32(blob, lazy->modes);
   blob_write_uint32(blob, lazy->recipe_size);
   blob_write_bytes(blob, lazy->recipe, lazy->recipe_size);
   struct kk_sparse_shader_state *state = shader->sparse;
   blob_write_uint8(blob, state != NULL);
   if (!state)
      return !blob->out_of_memory;
   simple_mtx_lock(&state->mutex);
   blob_write_bytes(blob, state->guarded_key, sizeof(state->guarded_key));
   struct blob binary;
   blob_init(&binary);
   if (state->guarded[state->required_modes])
      kk_shader_serialize_native(shader->vk.base.device,
                                 &state->guarded[state->required_modes]->vk, &binary);
   else
      blob_write_bytes(&binary, state->binary.data, state->binary.size);
   blob_write_uint32(blob, binary.size);
   blob_write_bytes(blob, binary.data, binary.size);
   bool ok = !binary.out_of_memory && !blob->out_of_memory;
   blob_finish(&binary);
   simple_mtx_unlock(&state->mutex);
   return ok;
}

/* A restored recipe defers its translation again when the creation defers;
 * otherwise it compiles now from the same input. */
static VkResult
kk_lazy_compute_deserialize(struct kk_device *dev, struct blob_reader *blob,
                            const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   blob_read_uint32(blob);
   const uint32_t required_modes = blob_read_uint32(blob);
   const uint32_t modes = blob_read_uint32(blob);
   const uint32_t size = blob_read_uint32(blob);
   const void *recipe = blob_read_bytes(blob, size);
   const uint8_t sparse = blob_read_uint8(blob);
   if (blob->overrun || !size || sparse > 1 || (required_modes & ~KK_SPARSE_MODES) ||
       (modes & ~required_modes) || (sparse && required_modes == modes) ||
       (!sparse && required_modes != modes))
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   struct kk_sparse_shader_state *state = NULL;
   if (sparse) {
      state = calloc(1, sizeof(*state));
      if (!state)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      simple_mtx_init(&state->mutex, mtx_plain);
      blob_init(&state->recipe);
      blob_init(&state->binary);
      state->required_modes = required_modes;
      state->always_modes = modes;
      blob_write_bytes(&state->recipe, recipe, size);
      blob_copy_bytes(blob, state->guarded_key, sizeof(state->guarded_key));
      const uint32_t binary_size = blob_read_uint32(blob);
      const void *binary = blob_read_bytes(blob, binary_size);
      if (!blob->overrun)
         blob_write_bytes(&state->binary, binary, binary_size);
   }
   VkResult result = VK_SUCCESS;
   if (blob->overrun)
      result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   else if (state && (state->recipe.out_of_memory || state->binary.out_of_memory))
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
   if (result == VK_SUCCESS)
      result = kk_sparse_prepare_pipeline(dev, modes);
   struct kk_shader *shader = NULL;
   if (result == VK_SUCCESS && kk_defer_compute && !dev->eager_compute_translation) {
      void *own = NULL;
      const void *source = state ? state->recipe.data : NULL;
      if (!state) {
         own = malloc(size);
         if (own)
            memcpy(own, recipe, size);
         source = own;
      }
      if (source) {
         result = kk_lazy_compute_new(dev, source, size, own, required_modes, modes, alloc, &shader);
         if (result != VK_SUCCESS) {
            free(own);
            shader = NULL;
            result = VK_SUCCESS;
         }
      }
   }
   if (result == VK_SUCCESS && !shader) {
      /* Restores the stored translation when there is one; otherwise
       * translates now. Both compile the native pipeline now. */
      struct disk_cache *disk = dev->vk.physical->disk_cache;
      if (disk) {
         cache_key key;
         kk_lazy_compute_key(disk, recipe, size, modes, key);
         if (kk_lazy_compute_restore(dev, disk, key, alloc, &shader) != VK_SUCCESS)
            shader = NULL;
      }
      if (!shader)
         result = kk_recipe_compile(dev, recipe, size, required_modes, modes, modes, alloc, &shader);
   }
   if (result != VK_SUCCESS) {
      if (state) {
         blob_finish(&state->recipe);
         blob_finish(&state->binary);
         simple_mtx_destroy(&state->mutex);
         free(state);
      }
      return result;
   }
   shader->info.sparse_modes = modes;
   shader->sparse = state;
   *out = &shader->vk;
   return VK_SUCCESS;
}
#endif

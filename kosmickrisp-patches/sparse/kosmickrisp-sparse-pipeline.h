/* SPDX-License-Identifier: MIT
 * Sparse buffer variants share the normal compiler and pipeline serializer.
 * Only the compact compiler input is retained; Metal compilation is deferred
 * until the device actually creates a residency resource.
 */
#ifndef KK_SPARSE_PIPELINE_H
#define KK_SPARSE_PIPELINE_H
#include "kk_sparse.h"
#include "nir_serialize.h"
#include "util/blob.h"

struct kk_sparse_shader_state {
   simple_mtx_t mutex;
   struct blob recipe;
   struct blob binary;
   blake3_hash guarded_key;
   struct kk_shader *guarded;
};

static VkResult kk_compile_shaders_native(struct vk_device *, uint32_t,
   struct vk_shader_compile_info *, const struct vk_graphics_pipeline_state *,
   const struct vk_features *, const VkAllocationCallbacks *, struct vk_shader **, bool);
static bool kk_shader_serialize_native(struct vk_device *, const struct vk_shader *, struct blob *);
static VkResult kk_deserialize_shader_native(struct vk_device *, struct blob_reader *, uint32_t,
   const VkAllocationCallbacks *, struct vk_shader **);

/* These are the existing shared graphics-state structures, with their one
 * nested pointer (sample locations) encoded separately. No Vulkan object or
 * native address is part of the replay input. */
#define KK_SPARSE_GRAPHICS_STATES(X) \
   X(vi) X(ia) X(ts) X(vp) X(dr) X(rs) X(fsr) X(ms) X(ds) X(cb) X(ial) X(cal) X(mv) X(rp)

static void
kk_sparse_write_graphics(struct blob *blob, const struct vk_graphics_pipeline_state *state)
{
   blob_write_uint8(blob, state != NULL);
   if (!state) return;
   blob_write_bytes(blob, state, offsetof(struct vk_graphics_pipeline_state, vi));
#define WRITE(member) do { \
   blob_write_uint8(blob, state->member != NULL); \
   if (state->member) { \
      __typeof__(*state->member) value = *state->member; \
      if (offsetof(struct vk_graphics_pipeline_state, member) == offsetof(struct vk_graphics_pipeline_state, ms)) \
         ((struct vk_multisample_state *)&value)->sample_locations = NULL; \
      blob_write_bytes(blob, &value, sizeof(value)); \
   } \
} while (0);
   KK_SPARSE_GRAPHICS_STATES(WRITE)
#undef WRITE
   bool locations = state->ms && state->ms->sample_locations;
   blob_write_uint8(blob, locations);
   if (locations) blob_write_bytes(blob, state->ms->sample_locations, sizeof(*state->ms->sample_locations));
}

static struct vk_graphics_pipeline_state *
kk_sparse_read_graphics(void *ctx, struct blob_reader *blob)
{
   if (!blob_read_uint8(blob)) return NULL;
   struct vk_graphics_pipeline_state *state = rzalloc(ctx, struct vk_graphics_pipeline_state);
   if (!state) return NULL;
   blob_copy_bytes(blob, state, offsetof(struct vk_graphics_pipeline_state, vi));
#define READ(member) do { \
   if (blob_read_uint8(blob)) { \
      __typeof__(state->member) value = ralloc_size(ctx, sizeof(*state->member)); \
      if (!value) { blob->overrun = true; return NULL; } \
      blob_copy_bytes(blob, (void *)value, sizeof(*value)); \
      state->member = value; \
   } \
} while (0);
   KK_SPARSE_GRAPHICS_STATES(READ)
#undef READ
   if (blob_read_uint8(blob)) {
      struct vk_sample_locations_state *locations = ralloc(ctx, struct vk_sample_locations_state);
      if (!locations || !state->ms) { blob->overrun = true; return NULL; }
      blob_copy_bytes(blob, locations, sizeof(*locations));
      ((struct vk_multisample_state *)state->ms)->sample_locations = locations;
   }
   return state;
}

static void
kk_sparse_write_layout(struct blob *blob, const struct vk_descriptor_set_layout *vk_layout)
{
   blob_write_uint8(blob, vk_layout != NULL);
   if (!vk_layout) return;
   const struct kk_descriptor_set_layout *layout =
      vk_to_kk_descriptor_set_layout((struct vk_descriptor_set_layout *)vk_layout);
   struct kk_descriptor_set_layout header = *layout;
   memset(&header.vk, 0, sizeof(header.vk));
   header.embedded_samplers_addr = 0;
   blob_write_bytes(blob, &header, sizeof(header));
   for (unsigned i = 0; i < layout->binding_count; ++i) {
      const struct kk_descriptor_set_binding_layout *binding = &layout->binding[i];
      struct kk_descriptor_set_binding_layout value = *binding;
      value.immutable_samplers = NULL;
      blob_write_bytes(blob, &value, sizeof(value));
      blob_write_uint8(blob, binding->immutable_samplers != NULL);
      if (!binding->immutable_samplers) continue;
      for (unsigned j = 0; j < binding->array_size; ++j) {
         const struct kk_sampler *sampler = binding->immutable_samplers[j];
         blob_write_uint8(blob, sampler != NULL);
         if (!sampler) continue;
         struct kk_sampler copy = *sampler;
         memset(&copy.vk.base, 0, sizeof(copy.vk.base));
         memset(copy.planes, 0, sizeof(copy.planes));
         copy.vk.ycbcr_conversion = NULL;
         blob_write_bytes(blob, &copy, sizeof(copy));
         blob_write_uint8(blob, sampler->vk.ycbcr_conversion != NULL);
         if (sampler->vk.ycbcr_conversion)
            blob_write_bytes(blob, &sampler->vk.ycbcr_conversion->state,
                             sizeof(sampler->vk.ycbcr_conversion->state));
      }
   }
}

static struct vk_descriptor_set_layout *
kk_sparse_read_layout(void *ctx, struct blob_reader *blob)
{
   if (!blob_read_uint8(blob)) return NULL;
   struct kk_descriptor_set_layout header;
   blob_copy_bytes(blob, &header, sizeof(header));
   if (blob->overrun || header.binding_count > (1u << 20)) { blob->overrun = true; return NULL; }
   struct kk_descriptor_set_layout *layout = rzalloc_size(ctx,
      sizeof(header) + header.binding_count * sizeof(layout->binding[0]));
   if (!layout) { blob->overrun = true; return NULL; }
   *layout = header;
   for (unsigned i = 0; i < layout->binding_count; ++i) {
      struct kk_descriptor_set_binding_layout *binding = &layout->binding[i];
      blob_copy_bytes(blob, binding, sizeof(*binding));
      binding->immutable_samplers = NULL;
      if (!blob_read_uint8(blob)) continue;
      if (binding->array_size > (1u << 20)) { blob->overrun = true; return NULL; }
      binding->immutable_samplers = rzalloc_array(ctx, struct kk_sampler *, binding->array_size);
      if (!binding->immutable_samplers) { blob->overrun = true; return NULL; }
      for (unsigned j = 0; j < binding->array_size; ++j) {
         if (!blob_read_uint8(blob)) continue;
         struct kk_sampler *sampler = rzalloc(ctx, struct kk_sampler);
         if (!sampler) { blob->overrun = true; return NULL; }
         blob_copy_bytes(blob, sampler, sizeof(*sampler));
         sampler->vk.ycbcr_conversion = NULL;
         if (blob_read_uint8(blob)) {
            sampler->vk.ycbcr_conversion = rzalloc(ctx, struct vk_ycbcr_conversion);
            if (!sampler->vk.ycbcr_conversion) { blob->overrun = true; return NULL; }
            blob_copy_bytes(blob, &sampler->vk.ycbcr_conversion->state,
                            sizeof(sampler->vk.ycbcr_conversion->state));
         }
         binding->immutable_samplers[j] = sampler;
      }
   }
   return &layout->vk;
}

static struct kk_sparse_shader_state *
kk_sparse_capture_pipeline(uint32_t count, const struct vk_shader_compile_info *infos,
   const struct vk_graphics_pipeline_state *graphics, const struct vk_features *features)
{
   struct kk_sparse_shader_state *state = calloc(1, sizeof(*state));
   if (!state) return NULL;
   simple_mtx_init(&state->mutex, mtx_plain);
   blob_init(&state->recipe); blob_init(&state->binary);
   struct blob *blob = &state->recipe;
   blob_write_uint32(blob, count);
   blob_write_bytes(blob, features, sizeof(*features));
   kk_sparse_write_graphics(blob, graphics);
   for (unsigned i = 0; i < count; ++i) {
      const struct vk_shader_compile_info *info = &infos[i];
      blob_write_uint32(blob, info->stage);
      blob_write_uint32(blob, info->flags);
      blob_write_uint64(blob, info->rt_flags);
      blob_write_uint32(blob, info->next_stage_mask);
      blob_write_bytes(blob, info->robustness, sizeof(*info->robustness));
      blob_write_uint32(blob, info->set_layout_count);
      for (unsigned j = 0; j < info->set_layout_count; ++j)
         kk_sparse_write_layout(blob, info->set_layouts[j]);
      blob_write_uint32(blob, info->embedded_sampler_count);
      blob_write_bytes(blob, info->embedded_samplers,
                       info->embedded_sampler_count * sizeof(*info->embedded_samplers));
      blob_write_uint32(blob, info->push_constant_range_count);
      blob_write_bytes(blob, info->push_constant_ranges,
                       info->push_constant_range_count * sizeof(*info->push_constant_ranges));
      nir_serialize(blob, info->nir, false);
   }
   if (blob->out_of_memory) {
      blob_finish(blob); blob_finish(&state->binary);
      simple_mtx_destroy(&state->mutex); free(state); return NULL;
   }
   struct mesa_blake3 hash;
   _mesa_blake3_init(&hash);
   const uint32_t variant[] = {0x53505231, BITFIELD_BIT(KK_FEAT_SPARSE_BUFFER)};
   _mesa_blake3_update(&hash, variant, sizeof(variant));
   _mesa_blake3_update(&hash, blob->data, blob->size);
   _mesa_blake3_final(&hash, state->guarded_key);
   return state;
}

void
kk_sparse_shader_finish(struct kk_shader *shader)
{
   struct kk_sparse_shader_state *state = shader->sparse;
   if (!state) return;
   if (state->guarded) kk_shader_destroy(shader->vk.base.device, state->guarded, NULL);
   blob_finish(&state->recipe); blob_finish(&state->binary);
   simple_mtx_destroy(&state->mutex); free(state); shader->sparse = NULL;
}

static VkResult
kk_sparse_compile_variant(struct kk_device *dev, struct kk_sparse_shader_state *state,
                          struct kk_shader **out)
{
   if (!state->binary.size && dev->vk.mem_cache) {
      struct vk_pipeline_cache_object *cached = vk_pipeline_cache_lookup_object(
         dev->vk.mem_cache, state->guarded_key, sizeof(state->guarded_key),
         &vk_raw_data_cache_object_ops, NULL);
      if (cached) {
         struct vk_raw_data_cache_object *raw = container_of(cached, struct vk_raw_data_cache_object, base);
         blob_write_bytes(&state->binary, raw->data, raw->data_size);
         vk_pipeline_cache_object_unref(&dev->vk, cached);
         if (state->binary.out_of_memory) return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }
   if (state->binary.size) {
      struct blob_reader binary;
      blob_reader_init(&binary, state->binary.data, state->binary.size);
      struct vk_shader *vk_shader;
      VkResult result = kk_deserialize_shader_native(&dev->vk, &binary,
         kk_device_physical(dev)->vk.properties.shaderBinaryVersion, NULL, &vk_shader);
      if (result == VK_SUCCESS) *out = container_of(vk_shader, struct kk_shader, vk);
      return result;
   }
   void *ctx = ralloc_context(NULL);
   if (!ctx) return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct blob_reader blob;
   blob_reader_init(&blob, state->recipe.data, state->recipe.size);
   unsigned count = blob_read_uint32(&blob);
   struct vk_features features;
   blob_copy_bytes(&blob, &features, sizeof(features));
   struct vk_graphics_pipeline_state *graphics = kk_sparse_read_graphics(ctx, &blob);
   if (count > MESA_SHADER_STAGES) { ralloc_free(ctx); return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT; }
   struct vk_shader_compile_info infos[MESA_SHADER_STAGES] = {0};
   struct vk_shader *compiled[MESA_SHADER_STAGES] = {0};
   for (unsigned i = 0; i < count; ++i) {
      struct vk_shader_compile_info *info = &infos[i];
      info->stage = blob_read_uint32(&blob);
      info->flags = blob_read_uint32(&blob);
      info->rt_flags = blob_read_uint64(&blob);
      info->next_stage_mask = blob_read_uint32(&blob);
      struct vk_pipeline_robustness_state *rs = rzalloc(ctx, struct vk_pipeline_robustness_state);
      if (!rs) { ralloc_free(ctx); return VK_ERROR_OUT_OF_HOST_MEMORY; }
      blob_copy_bytes(&blob, rs, sizeof(*rs)); info->robustness = rs;
      info->set_layout_count = blob_read_uint32(&blob);
      if (info->set_layout_count > MESA_VK_MAX_DESCRIPTOR_SETS) { blob.overrun = true; break; }
      struct vk_descriptor_set_layout **layouts = rzalloc_array(ctx,
         struct vk_descriptor_set_layout *, info->set_layout_count);
      if (info->set_layout_count && !layouts) { ralloc_free(ctx); return VK_ERROR_OUT_OF_HOST_MEMORY; }
      for (unsigned j = 0; j < info->set_layout_count; ++j) layouts[j] = kk_sparse_read_layout(ctx, &blob);
      info->set_layouts = layouts;
      info->embedded_sampler_count = blob_read_uint32(&blob);
      if (info->embedded_sampler_count > (1u << 20)) { blob.overrun = true; break; }
      struct vk_sampler_state *samplers = ralloc_array(ctx, struct vk_sampler_state, info->embedded_sampler_count);
      if (info->embedded_sampler_count && !samplers) { ralloc_free(ctx); return VK_ERROR_OUT_OF_HOST_MEMORY; }
      blob_copy_bytes(&blob, samplers, info->embedded_sampler_count * sizeof(*samplers));
      info->embedded_samplers = samplers;
      info->push_constant_range_count = blob_read_uint32(&blob);
      if (info->push_constant_range_count > (1u << 16)) { blob.overrun = true; break; }
      VkPushConstantRange *ranges = ralloc_array(ctx, VkPushConstantRange, info->push_constant_range_count);
      if (info->push_constant_range_count && !ranges) { ralloc_free(ctx); return VK_ERROR_OUT_OF_HOST_MEMORY; }
      blob_copy_bytes(&blob, ranges, info->push_constant_range_count * sizeof(*ranges));
      info->push_constant_ranges = ranges;
      if (blob.overrun || info->stage >= MESA_SHADER_STAGES) break;
      info->nir = nir_deserialize(ctx, kk_get_nir_options(&kk_device_physical(dev)->vk, info->stage, rs), &blob);
      if (!info->nir) { blob.overrun = true; break; }
   }
   if (blob.overrun) { ralloc_free(ctx); return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT; }
   VkResult result = kk_compile_shaders_native(&dev->vk, count, infos, graphics, &features,
                                               NULL, compiled, true);
   if (result == VK_SUCCESS) {
      *out = container_of(compiled[0], struct kk_shader, vk);
      for (unsigned i = 1; i < count; ++i)
         kk_shader_destroy(&dev->vk, container_of(compiled[i], struct kk_shader, vk), NULL);
      if (dev->vk.mem_cache) {
         struct blob binary;
         blob_init(&binary);
         if (kk_shader_serialize_native(&dev->vk, compiled[0], &binary)) {
            struct vk_raw_data_cache_object *raw = vk_raw_data_cache_object_create(
               &dev->vk, state->guarded_key, sizeof(state->guarded_key), binary.data, binary.size);
            if (raw) {
               struct vk_pipeline_cache_object *cached = vk_pipeline_cache_add_object(dev->vk.mem_cache, &raw->base);
               vk_pipeline_cache_object_unref(&dev->vk, cached);
            }
         }
         blob_finish(&binary);
      }
   }
   ralloc_free(ctx);
   return result;
}

struct kk_shader *
kk_sparse_select_shader(struct kk_cmd_buffer *cmd, struct kk_shader *shader)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   if (!shader || !shader->sparse || !kk_sparse_residency_used(dev)) return shader;
   struct kk_sparse_shader_state *state = shader->sparse;
   simple_mtx_lock(&state->mutex);
   if (!state->guarded) {
      VkResult result = kk_sparse_compile_variant(dev, state, &state->guarded);
      if (result != VK_SUCCESS) vk_command_buffer_set_error(&cmd->vk, result);
   }
   struct kk_shader *selected = state->guarded ? state->guarded : shader;
   simple_mtx_unlock(&state->mutex);
   return selected;
}

static VkResult
kk_compile_shaders(struct vk_device *device, uint32_t count,
   struct vk_shader_compile_info *infos, const struct vk_graphics_pipeline_state *graphics,
   const struct vk_features *features, const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   struct kk_sparse_shader_state *state = NULL;
   if (features->sparseResidencyBuffer) {
      state = kk_sparse_capture_pipeline(count, infos, graphics, features);
      if (!state) {
         for (unsigned i = 0; i < count; ++i) ralloc_free(infos[i].nir);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }
   VkResult result = kk_compile_shaders_native(device, count, infos, graphics,
                                               features, alloc, out, false);
   if (state) {
      if (result == VK_SUCCESS) container_of(out[0], struct kk_shader, vk)->sparse = state;
      else {
         blob_finish(&state->recipe); blob_finish(&state->binary);
         simple_mtx_destroy(&state->mutex); free(state);
      }
   }
   return result;
}

static bool
kk_shader_serialize(struct vk_device *dev, const struct vk_shader *vk_shader, struct blob *blob)
{
   if (!kk_shader_serialize_native(dev, vk_shader, blob)) return false;
   const struct kk_shader *shader = container_of(vk_shader, struct kk_shader, vk);
   struct kk_sparse_shader_state *state = shader->sparse;
   blob_write_uint32(blob, state ? state->recipe.size : 0);
   if (!state) return !blob->out_of_memory;
   simple_mtx_lock(&state->mutex);
   blob_write_bytes(blob, state->recipe.data, state->recipe.size);
   blob_write_bytes(blob, state->guarded_key, sizeof(state->guarded_key));
   struct blob binary;
   blob_init(&binary);
   if (state->guarded) kk_shader_serialize_native(dev, &state->guarded->vk, &binary);
   else blob_write_bytes(&binary, state->binary.data, state->binary.size);
   blob_write_uint32(blob, binary.size);
   blob_write_bytes(blob, binary.data, binary.size);
   bool ok = !binary.out_of_memory && !blob->out_of_memory;
   blob_finish(&binary);
   simple_mtx_unlock(&state->mutex);
   return ok;
}

static VkResult
kk_deserialize_shader(struct vk_device *dev, struct blob_reader *blob, uint32_t version,
   const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   VkResult result = kk_deserialize_shader_native(dev, blob, version, alloc, out);
   if (result != VK_SUCCESS) return result;
   unsigned size = blob_read_uint32(blob);
   if (size) {
      struct kk_sparse_shader_state *state = calloc(1, sizeof(*state));
      if (!state) { kk_shader_destroy(dev, container_of(*out, struct kk_shader, vk), alloc); return VK_ERROR_OUT_OF_HOST_MEMORY; }
      simple_mtx_init(&state->mutex, mtx_plain);
      blob_init(&state->recipe); blob_init(&state->binary);
      container_of(*out, struct kk_shader, vk)->sparse = state;
      const void *data = blob_read_bytes(blob, size);
      if (!blob->overrun) blob_write_bytes(&state->recipe, data, size);
      blob_copy_bytes(blob, state->guarded_key, sizeof(state->guarded_key));
      unsigned binary_size = blob_read_uint32(blob);
      data = blob_read_bytes(blob, binary_size);
      if (!blob->overrun) blob_write_bytes(&state->binary, data, binary_size);
      if (state->recipe.out_of_memory || state->binary.out_of_memory) result = VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   if (blob->overrun) result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   if (result != VK_SUCCESS) kk_shader_destroy(dev, container_of(*out, struct kk_shader, vk), alloc);
   return result;
}
#undef KK_SPARSE_GRAPHICS_STATES
#endif

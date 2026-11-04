/* SPDX-License-Identifier: MIT
 * Sparse buffer variants share the normal compiler and pipeline serializer.
 * Only the compact compiler input is retained; Metal compilation is deferred
 * until the device creates a residency resource. GPU-updated heaps and physical
 * pointers require guards before submission, including before first resource use.
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
   uint32_t required_modes, always_modes;
   struct kk_shader *guarded[KK_SPARSE_MODES + 1];
};

static VkResult kk_compile_shaders_native(struct vk_device *, uint32_t,
   struct vk_shader_compile_info *, const struct vk_graphics_pipeline_state *,
   const struct vk_features *, const VkAllocationCallbacks *, struct vk_shader **, uint32_t);
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

/* A compact recipe is needed only for operations that could later access
 * these sparse classes. Descriptor contents may change after compilation. */
static uint32_t
kk_sparse_pipeline_modes(uint32_t count, const struct vk_shader_compile_info *infos,
                         const struct vk_features *features, uint32_t *always_modes)
{
   uint32_t enabled = (features->sparseResidencyBuffer ? KK_SPARSE_BUFFER : 0) |
      (features->sparseResidencyImage3D ? KK_SPARSE_STANDARD_3D : 0);
   uint32_t modes = 0;
   *always_modes = 0;
   if (!enabled) return 0;
   for (unsigned i = 0; i < count; ++i) {
      uint32_t stage_modes = 0, physical_modes = 0;
      nir_foreach_function_impl(impl, infos[i].nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type == nir_instr_type_tex) {
                  nir_tex_instr *tex = nir_instr_as_tex(instr);
                  bool sampled = !tex->is_shadow &&
                     (tex->op == nir_texop_tex || tex->op == nir_texop_txb ||
                      tex->op == nir_texop_txl || tex->op == nir_texop_txd);
                  if (tex->sampler_dim == GLSL_SAMPLER_DIM_3D &&
                      (sampled || tex->op == nir_texop_txf))
                     stage_modes |= KK_SPARSE_STANDARD_3D;
               } else if (instr->type == nir_instr_type_intrinsic) {
                  nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
                  switch (intr->intrinsic) {
                  case nir_intrinsic_image_load:
                  case nir_intrinsic_image_sparse_load:
                  case nir_intrinsic_image_store:
                  case nir_intrinsic_image_atomic:
                  case nir_intrinsic_image_atomic_swap:
                  case nir_intrinsic_image_deref_load:
                  case nir_intrinsic_image_deref_sparse_load:
                  case nir_intrinsic_image_deref_store:
                  case nir_intrinsic_image_deref_atomic:
                  case nir_intrinsic_image_deref_atomic_swap:
                  case nir_intrinsic_bindless_image_load:
                  case nir_intrinsic_bindless_image_sparse_load:
                  case nir_intrinsic_bindless_image_store:
                  case nir_intrinsic_bindless_image_atomic:
                  case nir_intrinsic_bindless_image_atomic_swap:
                     if (nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_3D)
                        stage_modes |= KK_SPARSE_STANDARD_3D;
                     break;
                  /* Explicit IO may already have been applied by a caller. */
                  case nir_intrinsic_load_ubo:
                  case nir_intrinsic_load_ssbo:
                  case nir_intrinsic_store_ssbo:
                  case nir_intrinsic_ssbo_atomic:
                  case nir_intrinsic_ssbo_atomic_swap:
                     stage_modes |= KK_SPARSE_BUFFER;
                     break;
                  case nir_intrinsic_load_global:
                  case nir_intrinsic_store_global:
                  case nir_intrinsic_global_atomic:
                  case nir_intrinsic_global_atomic_swap:
                     stage_modes |= KK_SPARSE_BUFFER;
                     physical_modes |= KK_SPARSE_BUFFER;
                     break;
                  default: break;
                  }
                  for (unsigned j = 0; j < nir_intrinsic_infos[intr->intrinsic].num_srcs; ++j) {
                     nir_instr *parent = nir_def_instr(intr->src[j].ssa);
                     if (parent->type == nir_instr_type_deref &&
                         nir_deref_mode_is_one_of(nir_instr_as_deref(parent),
                            nir_var_mem_ubo | nir_var_mem_ssbo | nir_var_mem_global)) {
                        stage_modes |= KK_SPARSE_BUFFER;
                        if (nir_deref_mode_is(nir_instr_as_deref(parent), nir_var_mem_global))
                           physical_modes |= KK_SPARSE_BUFFER;
                     }
                  }

               }
            }
         }
      }
      modes |= stage_modes;
      *always_modes |= physical_modes;
      if (infos[i].flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT)
         *always_modes |= stage_modes;
   }
   /* The lazy native object represents a linked VS/CS pipeline. Standalone
    * stages and task/mesh groups retain their own native ownership. */
   if (count && infos[0].stage != MESA_SHADER_VERTEX &&
       infos[0].stage != MESA_SHADER_COMPUTE)
      *always_modes |= modes;
   *always_modes &= enabled;
   return modes & enabled;
}

static struct kk_sparse_shader_state *
kk_sparse_capture_pipeline(uint32_t count, const struct vk_shader_compile_info *infos,
   const struct vk_graphics_pipeline_state *graphics, const struct vk_features *features,
   uint32_t modes, uint32_t always_modes)
{
   struct kk_sparse_shader_state *state = calloc(1, sizeof(*state));
   if (!state) return NULL;
   simple_mtx_init(&state->mutex, mtx_plain);
   blob_init(&state->recipe); blob_init(&state->binary);
   struct blob *blob = &state->recipe;
   state->required_modes = modes;
   state->always_modes = always_modes;
   blob_write_uint32(blob, count);
   blob_write_uint32(blob, modes);
   blob_write_uint32(blob, always_modes);
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
   const uint32_t variant[] = {0x53505233, modes, always_modes, KK_FEAT_COUNT};
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
   for (unsigned modes = 1; modes <= KK_SPARSE_MODES; ++modes)
      if (state->guarded[modes])
         kk_shader_destroy(shader->vk.base.device, state->guarded[modes], NULL);
   blob_finish(&state->recipe); blob_finish(&state->binary);
   simple_mtx_destroy(&state->mutex); free(state); shader->sparse = NULL;
}

static VkResult
kk_sparse_compile_variant(struct kk_device *dev, struct kk_sparse_shader_state *state,
                          uint32_t modes, struct kk_shader **out)
{
   assert(modes && !(modes & ~state->required_modes));
   blake3_hash key;
   struct mesa_blake3 hash;
   _mesa_blake3_init(&hash);
   _mesa_blake3_update(&hash, state->guarded_key, sizeof(state->guarded_key));
   _mesa_blake3_update(&hash, &modes, sizeof(modes));
   _mesa_blake3_final(&hash, key);
   if (modes == state->required_modes && state->binary.size) {
      struct blob_reader binary;
      blob_reader_init(&binary, state->binary.data, state->binary.size);
      struct vk_shader *shader;
      VkResult result = kk_deserialize_shader_native(&dev->vk, &binary,
         kk_device_physical(dev)->vk.properties.shaderBinaryVersion, NULL, &shader);
      if (result == VK_SUCCESS) *out = container_of(shader, struct kk_shader, vk);
      return result;
   }
   if (dev->vk.mem_cache) {
      struct vk_pipeline_cache_object *cached = vk_pipeline_cache_lookup_object(
         dev->vk.mem_cache, key, sizeof(key), &vk_raw_data_cache_object_ops, NULL);
      if (cached) {
         struct vk_raw_data_cache_object *raw = container_of(cached, struct vk_raw_data_cache_object, base);
         struct blob_reader binary;
         blob_reader_init(&binary, raw->data, raw->data_size);
         struct vk_shader *shader;
         VkResult result = kk_deserialize_shader_native(&dev->vk, &binary,
            kk_device_physical(dev)->vk.properties.shaderBinaryVersion, NULL, &shader);
         vk_pipeline_cache_object_unref(&dev->vk, cached);
         if (result == VK_SUCCESS) *out = container_of(shader, struct kk_shader, vk);
         return result;
      }
   }
   void *ctx = ralloc_context(NULL);
   if (!ctx) return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct blob_reader blob;
   blob_reader_init(&blob, state->recipe.data, state->recipe.size);
   unsigned count = blob_read_uint32(&blob);
   if (blob_read_uint32(&blob) != state->required_modes ||
       blob_read_uint32(&blob) != state->always_modes) {
      ralloc_free(ctx); return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   }
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
                                               NULL, compiled, modes);
   if (result == VK_SUCCESS) {
      *out = container_of(compiled[0], struct kk_shader, vk);
      (*out)->info.sparse_modes = modes;
      for (unsigned i = 1; i < count; ++i)
         kk_shader_destroy(&dev->vk, container_of(compiled[i], struct kk_shader, vk), NULL);
      if (dev->vk.mem_cache) {
         struct blob binary;
         blob_init(&binary);
         if (kk_shader_serialize_native(&dev->vk, compiled[0], &binary)) {
            struct vk_raw_data_cache_object *raw = vk_raw_data_cache_object_create(
               &dev->vk, key, sizeof(key), binary.data, binary.size);
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
   /* Begin snapshots whether this recording needs sparse guards. If the
    * device first creates a sparse resource later, submit re-records this
    * command buffer before execution; draws need no activation polling. */
   if (shader && shader->sparse_base) shader = shader->sparse_base;
   if (!shader || !shader->sparse) return shader;
   struct kk_sparse_shader_state *state = shader->sparse;
   uint32_t modes = state->always_modes |
      (state->required_modes & cmd->sparse_guarded);
   if (modes == state->always_modes) return shader;
   /* A native variant is immutable and published exactly once. Warm binds
    * do not need the compilation/serialization mutex. */
   struct kk_shader *selected = p_atomic_read(&state->guarded[modes]);
   if (selected) return selected;
   simple_mtx_lock(&state->mutex);
   selected = state->guarded[modes];
   if (!selected) {
      VkResult result = kk_sparse_compile_variant(dev, state, modes, &selected);
      if (result != VK_SUCCESS) vk_command_buffer_set_error(&cmd->vk, result);
      else {
         selected->sparse_base = shader;
         p_atomic_set(&state->guarded[modes], selected);
      }
   }
   simple_mtx_unlock(&state->mutex);
   return selected ? selected : shader;
}

static VkResult
kk_compile_shaders_untracked(struct vk_device *device, uint32_t count,
   struct vk_shader_compile_info *infos, const struct vk_graphics_pipeline_state *graphics,
   const struct vk_features *features, const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   if (count && mesa_shader_stage_is_rt(infos[0].stage))
      return kk_compile_shaders_native(device,count,infos,graphics,features,alloc,out,features->sparseResidencyBuffer || features->sparseResidencyImage3D);
   struct kk_sparse_shader_state *state = NULL;
   uint32_t always_modes;
   uint32_t modes = kk_sparse_pipeline_modes(count, infos, features, &always_modes);
   struct kk_device *dev = container_of(device, struct kk_device, vk);
   VkResult result = kk_sparse_prepare_pipeline(dev, always_modes);
   if (result != VK_SUCCESS) {
      for (unsigned i = 0; i < count; ++i) ralloc_free(infos[i].nir);
      return result;
   }
   if (modes & ~always_modes) {
      state = kk_sparse_capture_pipeline(count, infos, graphics, features, modes, always_modes);
      if (!state) {
         for (unsigned i = 0; i < count; ++i) ralloc_free(infos[i].nir);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }
   result = kk_compile_shaders_native(device, count, infos, graphics,
                                      features, alloc, out, always_modes);
   if (result == VK_SUCCESS)
      for (unsigned i = 0; i < count; ++i)
         container_of(out[i], struct kk_shader, vk)->info.sparse_modes = always_modes;
   if (state) {
      if (result == VK_SUCCESS) container_of(out[0], struct kk_shader, vk)->sparse = state;
      else {
         blob_finish(&state->recipe); blob_finish(&state->binary);
         simple_mtx_destroy(&state->mutex); free(state);
      }
   }
   return result;
}

static ATTRIBUTE_NOINLINE VkResult
kk_compile_shaders_tracked(struct vk_device *device, uint32_t count,
   struct vk_shader_compile_info *infos, const struct vk_graphics_pipeline_state *graphics,
   const struct vk_features *features, const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   struct kk_device *tracking_dev = container_of(device, struct kk_device, vk);
   struct kk_perf *perf = tracking_dev->perf;
   uint64_t start = kk_perf_now();
   kk_perf_compiler(perf, 1);
   struct kk_perf_compile_scope scope;
   kk_perf_compile_begin(perf, &scope);
   if (scope.perf) {
      scope.input_count = count;
      for (uint32_t i = 0; i < count; ++i)
         scope.stage_mask |= BITFIELD_BIT(infos[i].stage);
   }
   VkResult result = kk_compile_shaders_untracked(device, count, infos, graphics, features, alloc, out);
   kk_perf_compile_end(&scope, false, result);
   kk_perf_compiler(perf, -1);
   kk_perf_add(perf, KK_PERF_SHADER_COMPILE, kk_perf_now() - start);
   return result;
}

static VkResult
kk_compile_shaders(struct vk_device *device, uint32_t count,
   struct vk_shader_compile_info *infos, const struct vk_graphics_pipeline_state *graphics,
   const struct vk_features *features, const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   struct kk_device *tracking_dev = container_of(device, struct kk_device, vk);
   if (unlikely(tracking_dev->perf))
      return kk_compile_shaders_tracked(device, count, infos, graphics, features, alloc, out);
   return kk_compile_shaders_untracked(device, count, infos, graphics, features, alloc, out);
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
   if (state->guarded[state->required_modes])
      kk_shader_serialize_native(dev, &state->guarded[state->required_modes]->vk, &binary);
   else blob_write_bytes(&binary, state->binary.data, state->binary.size);
   blob_write_uint32(blob, binary.size);
   blob_write_bytes(blob, binary.data, binary.size);
   bool ok = !binary.out_of_memory && !blob->out_of_memory;
   blob_finish(&binary);
   simple_mtx_unlock(&state->mutex);
   return ok;
}

static VkResult
kk_deserialize_shader_untracked(struct vk_device *dev, struct blob_reader *blob, uint32_t version,
   const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   if (kk_rt_shader_binary(blob)) return kk_rt_shader_deserialize(dev,blob,alloc,out);
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
      if (!blob->overrun) {
         blob_write_bytes(&state->recipe, data, size);
         struct blob_reader recipe;
         blob_reader_init(&recipe, data, size);
         blob_read_uint32(&recipe);
         state->required_modes = blob_read_uint32(&recipe);
         state->always_modes = blob_read_uint32(&recipe);
         if (recipe.overrun || !state->required_modes ||
             (state->required_modes & ~KK_SPARSE_MODES) ||
             (state->always_modes & ~state->required_modes) ||
             state->always_modes != container_of(*out, struct kk_shader, vk)->info.sparse_modes)
            blob->overrun = true;
      }
      blob_copy_bytes(blob, state->guarded_key, sizeof(state->guarded_key));
      unsigned binary_size = blob_read_uint32(blob);
      data = blob_read_bytes(blob, binary_size);
      if (!blob->overrun) blob_write_bytes(&state->binary, data, binary_size);
      if (state->recipe.out_of_memory || state->binary.out_of_memory) result = VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   if (blob->overrun) result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   if (result == VK_SUCCESS)
      result = kk_sparse_prepare_pipeline(container_of(dev, struct kk_device, vk),
         container_of(*out, struct kk_shader, vk)->info.sparse_modes);
   if (result != VK_SUCCESS) kk_shader_destroy(dev, container_of(*out, struct kk_shader, vk), alloc);
   return result;
}

static ATTRIBUTE_NOINLINE VkResult
kk_deserialize_shader_tracked(struct vk_device *dev, struct blob_reader *blob, uint32_t version,
   const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   struct kk_device *tracking_dev = container_of(dev, struct kk_device, vk);
   struct kk_perf *perf = tracking_dev->perf;
   uint64_t start = kk_perf_now();
   kk_perf_compiler(perf, 1);
   struct kk_perf_compile_scope scope;
   kk_perf_compile_begin(perf, &scope);
   VkResult result = kk_deserialize_shader_untracked(dev, blob, version, alloc, out);
   if (scope.perf) {
      scope.input_count = 1;
      if (result == VK_SUCCESS)
         scope.stage_mask = BITFIELD_BIT(container_of(*out, struct kk_shader, vk)->info.stage);
   }
   kk_perf_compile_end(&scope, true, result);
   kk_perf_compiler(perf, -1);
   kk_perf_add(perf, KK_PERF_SHADER_COMPILE, kk_perf_now() - start);
   if (result == VK_SUCCESS)
      kk_perf_add(perf, KK_PERF_CACHE_BINARY_HIT, 0);
   return result;
}

static VkResult
kk_deserialize_shader(struct vk_device *dev, struct blob_reader *blob, uint32_t version,
   const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   struct kk_device *tracking_dev = container_of(dev, struct kk_device, vk);
   if (unlikely(tracking_dev->perf))
      return kk_deserialize_shader_tracked(dev, blob, version, alloc, out);
   return kk_deserialize_shader_untracked(dev, blob, version, alloc, out);
}
#undef KK_SPARSE_GRAPHICS_STATES
#endif

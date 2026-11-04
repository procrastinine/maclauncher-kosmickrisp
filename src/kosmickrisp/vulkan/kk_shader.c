/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_shader.h"
#include "kk_async_compute.h"
#include "util/hash_table.h"
#include "kk_depth_bias_control.h"
#include "kk_atomic64.h"
#include "kosmickrisp-rt-shader.h"
#include "kosmickrisp-rt-cps.h"
#include "kosmickrisp-rt-pipeline.h"
#include "kosmickrisp-rt-commands.h"
#include "kosmickrisp-rt-groups.h"
#include "kk_ray_tracing.h"

#include "kk_cmd_buffer.h"
#include "kk_debug.h"
#include "kk_descriptor_set_layout.h"
#include "kk_device.h"
#include "kk_perf.h"
#include "kk_format.h"
#include "kk_nir_lower_per_vertex.h"
#include "kk_nir_lower_vbo.h"
#include "kk_physical_device.h"
#include "kk_sampler.h"
#include "kk_embedded_sampler_runtime.h"
#include "kk_sampler_emulation.h"

#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/ns_process_info.h"
#include "kosmickrisp/bridge/vk_to_mtl_map.h"
#include "kosmickrisp/compiler/nir_to_msl.h"

#include "nir_builder.h"
#include "kk_sliced_3d.h"
#include "nir_lower_blend.h"

#include "poly/nir/poly_nir.h"
#include "kosmickrisp/compiler/msl_compute_derivatives.h"
#include "poly/geometry.h"
#include "kk_multiview_nir.h"
#include "kk_statistics_nir.h"

#include "vk_blend.h"
#include "vk_format.h"
#include "vk_graphics_state.h"
#include "vk_nir_convert_ycbcr.h"
#include "vk_pipeline.h"
#include "kk_image_view.h"
#include "kk_dgc.h"
#include "kk_unused_attachments.h"
#include "kk_fragment_shading_rate_pipeline.h"
#include "kk_dgc_pipeline.h"

static const nir_shader_compiler_options *
kk_get_nir_options(struct vk_physical_device *vk_pdev, mesa_shader_stage stage,
                   UNUSED const struct vk_pipeline_robustness_state *rs)
{
   return &kk_nir_options;
}

static const struct vk_pipeline_robustness_state rs_all_supported = {
   .uniform_buffers =
      VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2,
   .storage_buffers =
      VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2,
   .images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2,
};

static struct spirv_to_nir_options
kk_get_spirv_options(struct vk_physical_device *vk_pdev,
                     UNUSED mesa_shader_stage stage,
                     const struct vk_pipeline_robustness_state *rs)
{
   if (KK_DEBUG(FORCE_ROBUSTNESS))
      rs = &rs_all_supported;
   return (struct spirv_to_nir_options){
      .environment = NIR_SPIRV_VULKAN,
      .separate_clip_cull_distance_arrays = true,
      .ssbo_addr_format = kk_buffer_addr_format(rs->storage_buffers),
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .ubo_addr_format = kk_buffer_addr_format(rs->uniform_buffers),
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ssbo_alignment = KK_MIN_SSBO_ALIGNMENT,
      .min_ubo_alignment = KK_MIN_UBO_ALIGNMENT,
   };
}

static void
kk_preprocess_nir(UNUSED struct vk_physical_device *vk_pdev, nir_shader *nir,
                  UNUSED const struct vk_pipeline_robustness_state *rs)
{
   /* Gather info before preprocess_nir but after some general lowering, so
    * inputs_read and system_values_read are accurately set.
    */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   msl_preprocess_nir(nir);

   /* Cannot be part of msl_preprocess_nir since clc does not expose
    * has_base_workgroup_id */
   nir_lower_compute_system_values_options csv_options = {
      .has_base_global_invocation_id = 0,
      .has_base_workgroup_id = nir->info.stage == MESA_SHADER_COMPUTE,
   };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv_options);
}

static bool
has_static_depth_stencil_state(const struct vk_graphics_pipeline_state *state)
{
   if (!state->ds)
      return false;

   return !(
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE) |
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE) |
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP) |
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE) |
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_OP) |
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK) |
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK));
}

static bool
kk_vi_is_static(const struct vk_graphics_pipeline_state *state)
{
   return state && state->vi &&
          !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VI);
}

#include "kk_barycentric_shader.h"
#include "kk_interlock_select.h"
static void kk_msl_copy(struct msl_compile_data *, const struct msl_compile_data *);
#include "kk_interlock_layers_nir.h"

static bool kk_conservative_possible(const struct vk_graphics_pipeline_state *state);
static bool kk_wide_line_possible(const struct vk_graphics_pipeline_state *state);
static bool kk_poly_layer_alias_possible(const struct vk_graphics_pipeline_state *state,
                                         const struct vk_features *features);

enum kk_vs_assembly_flags {
   KK_VS_STRIP_FAN_OR_ADJACENCY = BITFIELD_BIT(0),
   KK_VS_DYNAMIC_TOPOLOGY = BITFIELD_BIT(1),
   KK_VS_DYNAMIC_PROVOKING = BITFIELD_BIT(2),
   KK_VS_RESTART_TOPOLOGY = BITFIELD_BIT(3),
};

struct kk_vs_key {
   bool is_points;
   bool dynamic_vi;
   uint8_t assembly_flags;
   bool provoking_last;
   struct vk_vertex_input_state vi;
};

static void
kk_populate_vs_key(struct kk_vs_key *key,
                   const struct vk_graphics_pipeline_state *state,
                   bool barycentric)
{
   memset(key, 0, sizeof(*key));
   key->is_points = state->ia && state->ia->primitive_topology ==
                                    VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   key->dynamic_vi = !kk_vi_is_static(state);
   if (barycentric) {
      if (kk_state_strip_fan_or_adjacency(state))
         key->assembly_flags |= KK_VS_STRIP_FAN_OR_ADJACENCY;
      key->provoking_last = kk_state_provoking_last(state);
      /* Static assembly is folded only for the barycentric corner path.
       * Keep dynamic state distinct from an identical placeholder value. */
      if (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY))
         key->assembly_flags |= KK_VS_DYNAMIC_TOPOLOGY;
      if (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX))
         key->assembly_flags |= KK_VS_DYNAMIC_PROVOKING;
      if (state->ia) {
         VkPrimitiveTopology topology = state->ia->primitive_topology;
         if ((topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
             topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
             topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) &&
            (state->ia->primitive_restart_enable ||
             BITSET_TEST(state->dynamic,
                          MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE)))
            key->assembly_flags |= KK_VS_RESTART_TOPOLOGY;
      }
   }
   if (!key->dynamic_vi)
      key->vi = *state->vi;
}

struct kk_fs_key {
   VkFormat color_formats[MESA_VK_MAX_COLOR_ATTACHMENTS];
   struct vk_color_blend_state color_blend;
   struct vk_depth_stencil_state ds;
   uint32_t rasterization_samples;
   uint16_t static_sample_mask;
   bool sample_shading_enable;
   bool alpha_to_coverage_enable;
   bool alpha_to_one_enable;
   uint8_t dynamic_multisample;
   bool has_depth;
   bool has_stencil;
   bool dynamic_input_attachment_map;
   uint8_t depth_input_att;
   uint8_t stencil_input_att;
};

static void
kk_populate_fs_key(struct kk_fs_key *key,
                   const struct vk_graphics_pipeline_state *state)
{
   memset(key, 0, sizeof(*key));

   /* Required since we [de]serialize blend, and render target swizzle for
    * non-native formats */
   memcpy(key->color_formats, state->rp->color_attachment_formats,
          sizeof(key->color_formats));

   if (has_static_depth_stencil_state(state))
      key->ds = *(state->ds);

   /* Blend state gets [de]serialized, so we need to hash it */
   if (state->cb)
      key->color_blend = *(state->cb);

   if (state->ms) {
      key->rasterization_samples = state->ms->rasterization_samples;
      key->static_sample_mask = state->ms->sample_mask;
      key->sample_shading_enable = state->ms->sample_shading_enable;
      key->alpha_to_coverage_enable = state->ms->alpha_to_coverage_enable;
      key->alpha_to_one_enable = state->ms->alpha_to_one_enable;
   }

   key->dynamic_multisample =
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES) |
      (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK) << 1) |
      (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE) << 2);

   /* Depth writes are removed unless there's an actual attachment */
   key->has_depth = state->rp->depth_attachment_format != VK_FORMAT_UNDEFINED;
   key->has_stencil =
      state->rp->stencil_attachment_format != VK_FORMAT_UNDEFINED;

   /* Values that modify if a forced depth is present or not */
   key->depth_input_att =
      state->ial ? state->ial->depth_att : MESA_VK_ATTACHMENT_NO_INDEX;
   key->stencil_input_att =
      state->ial ? state->ial->stencil_att : MESA_VK_ATTACHMENT_NO_INDEX;
   key->dynamic_input_attachment_map =
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_INPUT_ATTACHMENT_MAP);
}

static kk_feature_key
kk_make_feature_key(const struct vk_features *feats)
{
   kk_feature_key key = 0;
   if (feats->deviceGeneratedCommands || feats->drawIndirectCount)
      key |= BITFIELD_BIT(KK_FEAT_DGC_REPLAY);
   if (feats->depthBounds)
      key |= BITFIELD_BIT(KK_FEAT_DEPTH_BOUNDS);
   if (feats->fragmentShaderPixelInterlock)
      key |= BITFIELD_BIT(KK_FEAT_PIXEL_ENVELOPE);
   if (feats->pipelineFragmentShadingRate || feats->primitiveFragmentShadingRate ||
       feats->attachmentFragmentShadingRate)
      key |= BITFIELD_BIT(KK_FEAT_FRAGMENT_SHADING_RATE);
   if (feats->customBorderColors)
      key |= BITFIELD_BIT(KK_FEAT_CUSTOM_BORDER);
   /* Cache entries with a replay input cannot alias entries without it.
    * This bit changes serialization only; the guard bit is selected on use. */
   if (feats->shaderOutputLayer || feats->multiview)
      key |= BITFIELD_BIT(KK_FEAT_POLY_LAYER_REPLAY);
   if (feats->sparseResidencyBuffer || feats->sparseResidencyImage3D) key |= BITFIELD_BIT(KK_FEAT_SPARSE_REPLAY);
   if (feats->shaderStorageImageMultisample)
      key |= BITFIELD_BIT(KK_FEAT_WRITABLE_MSAA);
   if (feats->nullDescriptor)
      key |= BITFIELD_BIT(KK_FEAT_NULL_DESCRIPTOR);
   if (feats->nonSeamlessCubeMap)
      key |= BITFIELD_BIT(KK_FEAT_NON_SEAMLESS_CUBE);
   if (feats->samplerFilterMinmax)
      key |= BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX);
   if (feats->minLod)
      key |= BITFIELD_BIT(KK_FEAT_IMAGE_VIEW_MIN_LOD);
   if (feats->imageSlicedViewOf3D)
      key |= BITFIELD_BIT(KK_FEAT_SLICED_3D);
   /* Individual queried compressed formats remain usable without enabling
    * a family-wide guarantee. Actual shader types gate the preparation walks. */
   key |= BITFIELD_BIT(KK_FEAT_LAYERED_BLOCK_VIEWS);
   if (feats->maintenance11)
      key |= BITFIELD_BIT(KK_FEAT_IMAGE_ARRAY_ALIAS);
   if (feats->sparseBinding)
      key |= BITFIELD_BIT(KK_FEAT_SPARSE_IMAGE_QUERY);
   if (feats->pipelineStatisticsQuery)
      key |= BITFIELD_BIT(KK_FEAT_PIPELINE_STATISTICS);
   if (feats->transformFeedback || feats->primitivesGeneratedQuery ||
       feats->pipelineStatisticsQuery)
      key |= BITFIELD_BIT(KK_FEAT_GEOMETRY_REPORTS);
   if (feats->meshShaderQueries || feats->pipelineStatisticsQuery)
      key |= BITFIELD_BIT(KK_FEAT_MESH_QUERIES);
   return key;
}

static void
kk_hash_graphics_state(struct vk_physical_device *device,
                       const struct vk_graphics_pipeline_state *state,
                       const struct vk_features *enabled_features,
                       VkShaderStageFlags stages, blake3_hash blake3_out)
{
   struct mesa_blake3 blake3_ctx;
   _mesa_blake3_init(&blake3_ctx);

   if (stages & VK_SHADER_STAGE_VERTEX_BIT) {
      struct kk_vs_key key;
      kk_populate_vs_key(&key, state, enabled_features->fragmentShaderBarycentric);
      _mesa_blake3_update(&blake3_ctx, &key, sizeof(key));
   }

   if (stages & VK_SHADER_STAGE_FRAGMENT_BIT) {
      struct kk_fs_key key;
      kk_populate_fs_key(&key, state);
      _mesa_blake3_update(&blake3_ctx, &key, sizeof(key));

      _mesa_blake3_update(&blake3_ctx, &state->mv->view_mask,
                          sizeof(state->mv->view_mask));
   }

   /* Pre-raster multiview also changes shaders without an FS. Keep
    * ordinary no-multiview keys unchanged. */
   if (!(stages & VK_SHADER_STAGE_FRAGMENT_BIT) && state && state->mv &&
       state->mv->view_mask)
      _mesa_blake3_update(&blake3_ctx, &state->mv->view_mask,
                          sizeof(state->mv->view_mask));

   const uint32_t feature_key[] = {
      (KK_SHADER_INFO_CACHE_VERSION << 16) | KK_FEAT_COUNT,
      sizeof(struct kk_shader_info),
      offsetof(struct kk_per_draw_data, geometry_statistics),
      KK_STATISTICS_SHADER_ABI,
      kk_make_feature_key(enabled_features),
   };
   _mesa_blake3_update(&blake3_ctx, &feature_key, sizeof(feature_key));
   const bool layer_alias = kk_poly_layer_alias_possible(state, enabled_features);
   _mesa_blake3_update(&blake3_ctx, &layer_alias, sizeof(layer_alias));
   if (kk_conservative_possible(state)) {
      uint32_t conservative = BITFIELD_BIT(KK_FEAT_CONSERVATIVE_RASTER);
      _mesa_blake3_update(&blake3_ctx, &conservative, sizeof(conservative));
   }

   /* Serialized producer topology is pipeline state, including when the
    * same shader body is reused by different raster classes. */
   if (state && state->rs && state->ia) {
      const uint32_t raster_key[] = {
         state->ia->primitive_topology, state->rs->polygon_mode, state->rs->line.mode,
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE),
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE),
      };
      _mesa_blake3_update(&blake3_ctx, raster_key, sizeof(raster_key));
      if (kk_wide_line_possible(state)) {
         bool dynamic_width = BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_WIDTH);
         bool dynamic_stipple = BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE);
         float width = dynamic_width ? 0 : kk_polygon_line_width(state->rs->line.width);
         const uint32_t line_key[] = {dynamic_width, dynamic_stipple,
            dynamic_stipple ? 0 : state->rs->line.stipple.enable};
         _mesa_blake3_update(&blake3_ctx, &width, sizeof(width));
         _mesa_blake3_update(&blake3_ctx, line_key, sizeof(line_key));
      }
   }

   /* Representation changes whether a Poly raster pipeline is required.
    * Keep ordinary pipeline cache keys while separating controlled pipelines. */
   uint8_t depth_bias_control = kk_depth_bias_control_flags(
      enabled_features->depthBiasControl, state);
   if (depth_bias_control) {
      const uint32_t representation_key[] = {0x44424332, depth_bias_control};
      _mesa_blake3_update(&blake3_ctx, representation_key, sizeof(representation_key));
   }
   _mesa_blake3_final(&blake3_ctx, blake3_out);
}

static void
shared_var_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size =
      glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length, *align = comp_size;
}

struct lower_ycbcr_state {
   uint32_t set_layout_count;
   struct vk_descriptor_set_layout *const *set_layouts;
   const struct vk_shader_compile_info *info;
};

static const struct vk_ycbcr_conversion_state *
lookup_ycbcr_conversion(const void *_state, uint32_t set, uint32_t binding,
                        uint32_t array_index)
{
   const struct lower_ycbcr_state *state = _state;
   if (set == VK_NIR_YCBCR_SET_IMMUTABLE_SAMPLERS) {
      assert(state->info && binding < state->info->embedded_sampler_count);
      const struct vk_sampler_state *sampler = &state->info->embedded_samplers[binding];
      return sampler->has_ycbcr_conversion ? &sampler->ycbcr_conversion : NULL;
   }
   assert(set < state->set_layout_count);
   assert(state->set_layouts[set] != NULL);
   const struct kk_descriptor_set_layout *set_layout =
      vk_to_kk_descriptor_set_layout(state->set_layouts[set]);
   assert(binding < set_layout->binding_count);

   const struct kk_descriptor_set_binding_layout *bind_layout =
      &set_layout->binding[binding];

   if (bind_layout->immutable_samplers == NULL)
      return NULL;

   array_index = MIN2(array_index, bind_layout->array_size - 1);

   const struct kk_sampler *sampler =
      bind_layout->immutable_samplers[array_index];

   return sampler && sampler->vk.ycbcr_conversion
             ? &sampler->vk.ycbcr_conversion->state
             : NULL;
}

static unsigned
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static bool
kk_nir_swizzle_fragment_output(nir_builder *b, nir_intrinsic_instr *intrin,
                               void *data)
{
   if (intrin->intrinsic != nir_intrinsic_store_output &&
       intrin->intrinsic != nir_intrinsic_load_output &&
       intrin->intrinsic != nir_intrinsic_load_tile_image)
      return false;

   unsigned slot = nir_intrinsic_io_semantics(intrin).location;
   if (slot < FRAG_RESULT_DATA0)
      return false;

   const struct vk_graphics_pipeline_state *state =
      (const struct vk_graphics_pipeline_state *)data;
   VkFormat vk_format =
      state->rp->color_attachment_formats[slot - FRAG_RESULT_DATA0];
   if (vk_format == VK_FORMAT_UNDEFINED)
      return false;

   enum pipe_format format = vk_format_to_pipe_format(vk_format);
   const struct kk_va_format *supported_format = kk_get_va_format(format);

   /* Check if we have to apply any swizzle */
   if (!supported_format->is_native) {
      unsigned channel_unswizzle[] = {
         supported_format->unswizzle.red, supported_format->unswizzle.green,
         supported_format->unswizzle.blue, supported_format->unswizzle.alpha};

      if (intrin->intrinsic == nir_intrinsic_store_output) {
         unsigned channel_swizzle[4] = {0u};
         for (uint32_t i = 0u; i < 4; ++i)
            channel_swizzle[channel_unswizzle[i]] = i;

         b->cursor = nir_before_instr(&intrin->instr);
         nir_def *to_replace = intrin->src[0].ssa;
         nir_def *swizzled = nir_swizzle(b, to_replace, channel_swizzle,
                                         to_replace->num_components);
         nir_src_rewrite(&intrin->src[0], swizzled);
      } else {
         b->cursor = nir_after_instr(&intrin->instr);
         nir_def *to_replace = &intrin->def;
         nir_def *swizzled = nir_swizzle(b, to_replace, channel_unswizzle,
                                         to_replace->num_components);
         nir_def_rewrite_uses_after(to_replace, swizzled);
      }
      return true;
   }

   return false;
}

static bool
kk_is_possible_both_depth_clip_clamp(
   const struct vk_graphics_pipeline_state *state, bool enabled)
{
   bool dyn_clamp =
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_CLAMP_ENABLE);
   bool dyn_clip =
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE);

   /* If rasterization state is null, clamp must be dynamic, and clip must be
    * inverted from clamp if not also dynamic. Thus, they cannot match unless
    * both are dynamic. */
   if (!state->rs)
      return dyn_clamp && dyn_clip;

   /* If clip is static and inverted from clamp, they can never be the same */
   if (!dyn_clip &&
       state->rs->depth_clip_enable == VK_MESA_DEPTH_CLIP_ENABLE_NOT_CLAMP)
      return false;

   /* Need to account for:
    * - Both are dynamic
    * - Both are static and match the desired value
    * - One is dynamic and the other is static and matches the desired value
    */
   bool static_clamp_match = state->rs->depth_clamp_enable == enabled;
   bool static_clip_match =
      vk_rasterization_state_depth_clip_enable(state->rs) == enabled;
   return (dyn_clamp || static_clamp_match) && (dyn_clip || static_clip_match);
}

static bool
kk_native_vbo_input_slot(UNUSED nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_input)
      return false;
   const uint64_t mask = *(const uint64_t *)data;
   const unsigned location = nir_intrinsic_io_semantics(intr).location - VERT_ATTRIB_GENERIC0;
   assert(location < KK_MAX_ATTRIBS && (mask & BITFIELD64_BIT(location)));
   const unsigned slot = util_bitcount64(mask & BITFIELD64_MASK(location));
   if (nir_intrinsic_base(intr) == slot)
      return false;
   nir_intrinsic_set_base(intr, slot);
   return true;
}

static void
kk_lower_vs_vbo_mask(nir_shader *nir, const struct vk_graphics_pipeline_state *state,
                     const struct vk_pipeline_robustness_state *rs,
                     uint64_t attribs_read)
{
   assert(!(nir->info.inputs_read & BITFIELD64_MASK(VERT_ATTRIB_GENERIC0)) &&
          "Fixed-function attributes not used in Vulkan");
   NIR_PASS(_, nir, nir_recompute_io_bases, nir_var_shader_in);
   /* the shader_out portion of this is load-bearing even for tess eval */
   /* Fold constant offset srcs for IO. */
   NIR_PASS(_, nir, nir_opt_constant_folding);

   bool robustness2 =
      rs->vertex_inputs ==
      VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2;
   uint64_t stage_inputs = nir->info.inputs_read >> VERT_ATTRIB_GENERIC0;
   assert((stage_inputs & ~attribs_read) == 0);
   /* Root entries are packed using the owning linked shader's mask. A native
    * alternative can use fewer inputs, but must keep those same root slots. */
   if (stage_inputs != attribs_read)
      nir_shader_intrinsics_pass(nir, kk_native_vbo_input_slot,
                                 nir_metadata_all, &attribs_read);
   if (!kk_vi_is_static(state)) {
      NIR_PASS(_, nir, kk_nir_lower_vbo, NULL, robustness2);
      return;
   }

   struct kk_attribute attributes[KK_MAX_ATTRIBS] = {};
   u_foreach_bit(i, state->vi->attributes_valid & attribs_read) {
      const struct vk_vertex_attribute_state *attr = &state->vi->attributes[i];
      assert(state->vi->bindings_valid & BITFIELD_BIT(attr->binding));
      const struct vk_vertex_binding_state *binding =
         &state->vi->bindings[attr->binding];

      /* nir_assign_io_var_locations compacts vertex inputs, eliminating
       * unused inputs. We need to do the same here to match the locations.
       */
      unsigned slot = util_bitcount64(attribs_read & BITFIELD_MASK(i));
      attributes[slot].divisor = binding->divisor;
      attributes[slot].binding = attr->binding;
      attributes[slot].format = vk_format_to_pipe_format(attr->format);
      attributes[slot].buf = attr->binding;
      attributes[slot].instanced =
         binding->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE;
   }
   NIR_PASS(_, nir, kk_nir_lower_vbo, attributes, robustness2);
}

static void
kk_lower_vs_vbo(nir_shader *nir, const struct vk_graphics_pipeline_state *state,
                const struct vk_pipeline_robustness_state *rs)
{
   kk_lower_vs_vbo_mask(nir, state, rs,
      nir->info.inputs_read >> VERT_ATTRIB_GENERIC0);
}

/* Lowering for the stage which ends up as the vertex stage on hardware */
static void
kk_lower_hw_vs_position(nir_shader *nir, const struct vk_graphics_pipeline_state *state,
               bool is_point)
{
   if (is_point)
      NIR_PASS(_, nir, msl_ensure_vertex_point_size_output);
   else
      nir_shader_intrinsics_pass(nir, msl_nir_vs_remove_point_size_write,
                                 nir_metadata_control_flow, NULL);

   NIR_PASS(_, nir, msl_ensure_vertex_position_output);
   NIR_PASS(_, nir, nir_lower_clip_halfz_dynamic);

   /* In any situation where both clip and clamp can be disabled, we need to
    * add viewport Z transform emulation to the shader */
   if (kk_is_possible_both_depth_clip_clamp(state, false))
      NIR_PASS(_, nir, msl_nir_lower_vs_disabled_depth_clamp_clip);
}

static void
kk_lower_hw_vs(nir_shader *nir, const struct vk_graphics_pipeline_state *state,
               bool is_point)
{
   kk_lower_hw_vs_position(nir, state, is_point);
   NIR_PASS(_, nir, msl_nir_vs_io_types);
}

static inline uint8_t
kk_get_logical_color_att_index(const struct vk_graphics_pipeline_state *state,
                               uint8_t i)
{
   return (state->cal) ? state->cal->color_map[i] : i;
}

static void
kk_lower_fs_blend(nir_shader *nir,
                  const struct vk_graphics_pipeline_state *state)
{
   nir_lower_blend_options opts = {
      .scalar_blend_const = false,
      .logicop_enable = state->cb->logic_op_enable,
      .logicop_func = vk_logic_op_to_pipe(state->cb->logic_op),
   };

   uint8_t unused_outputs = nir->info.outputs_written >> FRAG_RESULT_DATA0;
   static_assert(ARRAY_SIZE(opts.rt) == 8, "max RTs out of sync");
   for (unsigned i = 0; i < ARRAY_SIZE(opts.rt); ++i) {
      opts.rt[i].format = PIPE_FORMAT_NONE;
   }

   /* Only declared physical attachments have formats to map. Identity
    * entries beyond this range may alias a live remapped output and must not
    * overwrite its format before unused-output removal. */
   for (unsigned i = 0; i < state->rp->color_attachment_count; ++i) {
      uint8_t logical_index = kk_get_logical_color_att_index(state, i);
      if (logical_index == MESA_VK_ATTACHMENT_UNUSED) {
         continue;
      }

      nir_lower_blend_rt *rt = &opts.rt[logical_index];

      const struct vk_color_blend_attachment_state *att =
         &state->cb->attachments[i];
      rt->format =
         vk_format_to_pipe_format(state->rp->color_attachment_formats[i]);
      /* Blend equations operate on logical RGBA. Trimming an A8 target to
       * one component would retain red and discard alpha. */
      if (rt->format == PIPE_FORMAT_A8_UNORM)
         rt->format = PIPE_FORMAT_R8G8B8A8_UNORM;
      if (rt->format != PIPE_FORMAT_NONE)
         unused_outputs &= ~BITFIELD_BIT(logical_index);
      rt->colormask = att->write_mask;

      if (!att->blend_enable) {
         rt->rgb.src_factor = rt->alpha.src_factor = PIPE_BLENDFACTOR_ONE;
         rt->rgb.dst_factor = rt->alpha.dst_factor = PIPE_BLENDFACTOR_ZERO;
         rt->rgb.func = rt->alpha.func = PIPE_BLEND_ADD;
      } else if (att->color_blend_op >= VK_BLEND_OP_ZERO_EXT) {
         rt->advanced_blend = true;
         rt->blend_mode = vk_advanced_blend_op_to_pipe(att->color_blend_op);
         rt->src_premultiplied = att->src_premultiplied;
         rt->dst_premultiplied = att->dst_premultiplied;
         rt->overlap = vk_blend_overlap_to_pipe(att->blend_overlap);

         /* Unused by runtime and not supported */
         assert(!att->clamp_results);
      } else {
         rt->rgb.src_factor =
            vk_blend_factor_to_pipe(att->src_color_blend_factor);
         rt->rgb.dst_factor =
            vk_blend_factor_to_pipe(att->dst_color_blend_factor);
         rt->rgb.func = vk_blend_op_to_pipe(att->color_blend_op);

         rt->alpha.src_factor =
            vk_blend_factor_to_pipe(att->src_alpha_blend_factor);
         rt->alpha.dst_factor =
            vk_blend_factor_to_pipe(att->dst_alpha_blend_factor);
         rt->alpha.func = vk_blend_op_to_pipe(att->alpha_blend_op);
      }
   }
   /* Undefined pipeline formats discard stores even when a pass binds a
    * real attachment there. Remove them before Metal types the outputs. */
   if (unused_outputs)
      nir_shader_intrinsics_pass(nir, kk_discard_unused_color,
                                 nir_metadata_control_flow, &opts);
   /* Fold constant offset srcs for IO. */
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_lower_blend, &opts);
}

static bool
lower_subpass_dim(nir_builder *b, nir_tex_instr *tex, UNUSED void *_data)
{
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_SUBPASS)
      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   else if (tex->sampler_dim == GLSL_SAMPLER_DIM_SUBPASS_MS)
      tex->sampler_dim = GLSL_SAMPLER_DIM_MS;
   else
      return false;

   return true;
}

#include "kk_dynamic_multisample.h"

#include "kk_polygon_nir.h"
#include "kk_interlock_source_nir.h"
#include "kk_interlock_shader.h"

static void
kk_lower_fs(struct kk_device *dev, nir_shader *nir,
            const struct vk_graphics_pipeline_state *state)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);

   nir->info.fs.uses_sample_shading |=
      state->ms && state->ms->sample_shading_enable;

   if (kk_line_effects_possible(state) &&
       (state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
        BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE)))
      nir_shader_intrinsics_pass(nir, kk_line_alpha, nir_metadata_control_flow, NULL);

   kk_line_lower_pure_early(nir, state);

   const bool pixel_envelope = dev->vk.enabled_features.fragmentShaderPixelInterlock &&
      kk_interlock_multisample_pipeline(state) && kk_interlock_pixel_shader(nir);
   /* Envelope output processing applies these shared passes per sample,
    * after the original pixel-frequency body has executed only once. */
   if (!pixel_envelope)
      kk_lower_multisample_alpha(nir, state);

   /* msl_nir_lower_sample_shading needs to go before blending since
    * nir_lower_blend will always set uses_sample_shading to true if there's any
    * output read. I believe we do not need to lower it always, that is why it
    * goes here but need to double check. */
   if (nir->info.fs.uses_sample_shading)
      NIR_PASS(_, nir, msl_nir_lower_sample_shading);

   if (!pixel_envelope && state->cb)
      kk_lower_fs_blend(nir, state);

   enum pipe_format rts[MAX_DRAW_BUFFERS] = {PIPE_FORMAT_NONE};
   const struct vk_render_pass_state *rp = state->rp;
   for (uint32_t i = 0u; i < rp->color_attachment_count; ++i) {
      uint8_t logical_index = kk_get_logical_color_att_index(state, i);
      if (logical_index != MESA_VK_ATTACHMENT_UNUSED) {
         rts[logical_index] =
            vk_format_to_pipe_format(rp->color_attachment_formats[i]);
      }
   }

   NIR_PASS(_, nir, msl_nir_fs_force_output_signedness, rts);

   if (state->rp->depth_attachment_format == VK_FORMAT_UNDEFINED ||
       nir->info.fs.early_fragment_tests) {
      NIR_PASS(_, nir, msl_nir_fs_remove_depth_write);
   }

   /* In any situation where both clip and clamp can be enabled, we need to
    * add clamp emulation to the shader */
   if (kk_is_possible_both_depth_clip_clamp(state, true))
      NIR_PASS(_, nir, msl_nir_lower_fs_combined_depth_clamp_clip);

   /* Input attachments are treated as 2D textures. Fixes sampler dimension */
   NIR_PASS(_, nir, nir_shader_tex_pass, lower_subpass_dim, nir_metadata_all,
            NULL);

   /* Swizzle non-native formats' outputs */
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, kk_nir_swizzle_fragment_output,
            nir_metadata_control_flow, (void *)state);

   if (!pixel_envelope && kk_fs_has_sample_mask(state)) {

      /* KK_WORKAROUND_7 */
      if (!(pdev->settings.disabled_workarounds & BITFIELD64_BIT(7))) {
         if (!nir->info.fs.early_fragment_tests) {
            nir_function_impl *entrypoint = nir_shader_get_entrypoint(nir);
            nir_builder b = nir_builder_at(nir_after_impl(entrypoint));

            nir_def *sample_id = nir_load_sample_id(&b);
            nir_def *sample_bit = nir_ishl(&b, nir_imm_int(&b, 1), sample_id);
            nir_def *sample_mask_bit =
               nir_iand(&b, nir_load_sample_mask_in(&b), sample_bit);
            nir_discard_if(&b, nir_ieq_imm(&b, sample_mask_bit, 0u));
         }
      }
      kk_lower_sample_mask(nir, state);
   }
   /* Check https://github.com/KhronosGroup/Vulkan-Portability/issues/54 for
    * explanation on why we need this. */
   else if (nir->info.fs.needs_full_quad_helper_invocations ||
            nir->info.fs.needs_coarse_quad_helper_invocations ||
            (dev->vk.enabled_features.fragmentShaderBarycentric &&
             fs_needs_unmerged_triangles(nir))) {
      struct kk_physical_device *pdev = kk_device_physical(dev);

      /* Metal by default merges triangles which results in incorrect edges, see
       * mentioned issue above. The issue is that if we disable them through
       * writing to the sample mask, then derivatives are broken for multisample
       * rendering in M1 and M2 with macOS 26. This bug is fixed in macOS 27.
       *
       * This is why we are choosing the lesser evil which is incorrect edges
       * for multisampled rendering. Since CTS does not have tests for the edge
       * case with multisample, we get a clean run. This does not mean we are
       * Vulkan conformant with macOS26 for M1 and M2.
       */
      bool ms_bug_present = !ns_is_os_version_at_least(27, 0, 0) &&
                            (pdev->info.gpu_apple_family == 7 ||
                             pdev->info.gpu_apple_family == 8) &&
                            state->ms && state->ms->rasterization_samples > 1;
      /* KK_WORKAROUND_19: diagnostic-only triangle-merge comparison. */
      if (!ms_bug_present &&
          !(pdev->settings.disabled_workarounds & BITFIELD64_BIT(19)))
         NIR_PASS(_, nir, msl_disable_triangle_merge);
   }

   /* KK_WORKAROUND_5 */
   if (!(pdev->settings.disabled_workarounds & BITFIELD64_BIT(5)))
      NIR_PASS(_, nir, msl_nir_fake_guard_for_discards);
   /* KK_WORKAROUND_4 */
   if (!(pdev->settings.disabled_workarounds & BITFIELD64_BIT(4))) {
      NIR_PASS(_, nir, nir_lower_helper_writes, true);
   }
}

static bool
kk_fs_reads_ds_input_attachment(const nir_shader *nir, uint32_t depth_att,
                                uint32_t stencil_att, bool indices_known)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_image_deref_depth_input_attachment_load:
            case nir_intrinsic_image_deref_stencil_input_attachment_load:
               return true;
            case nir_intrinsic_image_deref_input_attachment_load: {
               if (!indices_known || !nir_src_is_const(intr->src[3]))
                  return true;
               const uint32_t idx =
                  nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[3]);
               if (idx == depth_att || idx == stencil_att)
                  return true;
               break;
            }
            default:
               break;
            }
         }
      }
   }

   return false;
}

static bool
kk_fs_needs_forced_depth_write(const nir_shader *nir,
                               const struct vk_graphics_pipeline_state *state)
{
   const struct vk_input_attachment_location_state *ial = state->ial;
   if (ial == NULL)
      return false;

   const bool has_depth_ia =
      state->rp->depth_attachment_format != VK_FORMAT_UNDEFINED &&
      ial->depth_att != MESA_VK_ATTACHMENT_NO_INDEX;
   const bool has_stencil_ia =
      state->rp->stencil_attachment_format != VK_FORMAT_UNDEFINED &&
      ial->stencil_att != MESA_VK_ATTACHMENT_NO_INDEX;

   if (!has_depth_ia && !has_stencil_ia)
      return false;

   if (has_static_depth_stencil_state(state)) {
      const struct vk_depth_stencil_state *ds = state->ds;

      const bool writes_depth =
         has_depth_ia && ds->depth.test_enable && ds->depth.write_enable;
      const bool writes_stencil =
         has_stencil_ia && ds->stencil.test_enable &&
         ds->stencil.write_enable &&
         (ds->stencil.front.write_mask | ds->stencil.back.write_mask);

      if (!writes_depth && !writes_stencil)
         return false;
   }

   const uint32_t depth_att =
      has_depth_ia ? ial->depth_att : NIR_VARIABLE_NO_INDEX;
   const uint32_t stencil_att =
      has_stencil_ia ? ial->stencil_att : NIR_VARIABLE_NO_INDEX;

   const bool indices_known =
      !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_INPUT_ATTACHMENT_MAP);

   return kk_fs_reads_ds_input_attachment(nir, depth_att, stencil_att,
                                          indices_known);
}

#include "kk_mesh_nir.h"
static void kk_poly_layer_lower_fs(nir_shader *nir);

static void
kk_finish_nir(struct kk_device *dev, nir_shader *nir,
              const struct vk_pipeline_robustness_state *rs,
              uint32_t set_layout_count,
              struct vk_descriptor_set_layout *const *set_layouts,
              kk_feature_key features, VkShaderCreateFlagsEXT binding_flags)
{
   const bool descriptor_heap = binding_flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
   const bool raw_texel = binding_flags & (VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT |
                                         VK_SHADER_CREATE_DESCRIPTOR_BUFFER_BIT_MESA);
   struct kk_physical_device *pdev = kk_device_physical(dev);
   if (descriptor_heap && nir->info.stage == MESA_SHADER_FRAGMENT)
      NIR_PASS(_, nir, kk_nir_finish_heap_image_loads);
   if (features & BITFIELD_BIT(KK_FEAT_IMAGE_VIEW_MIN_LOD))
      NIR_PASS(_, nir, kk_nir_lower_image_view_min_lod);

   if (features & BITFIELD_BIT(KK_FEAT_LAYERED_BLOCK_VIEWS))
      NIR_PASS(_, nir, kk_nir_prepare_layered_bc, rs, set_layout_count, set_layouts, descriptor_heap);
   if (features & BITFIELD_BIT(KK_FEAT_STANDARD_3D))
      NIR_PASS(_, nir, kk_nir_lower_standard_3d_samples, set_layout_count, set_layouts);

   NIR_PASS(_, nir, kk_nir_lower_sampler_helpers, set_layout_count, set_layouts,
      features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX),
      features & BITFIELD_BIT(KK_FEAT_CUSTOM_BORDER));
   /* Sparse image opcodes require shaderResourceResidency. Normal and
    * generated non-sparse operations need neither status-plumbing walk. */
   const bool sparse_instructions = dev->vk.enabled_features.shaderResourceResidency;
   if (sparse_instructions)
      NIR_PASS(_, nir, kk_nir_lower_sparse_sample_views, set_layout_count, set_layouts,
         (features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX)) && pdev->info.gpu_apple_family < 10);


   /* Descriptor lowering needs to happen after lowering blend since we will
    * generate a nir_intrinsic_load_blend_const_color_rgba which gets lowered by
    * the lower descriptor pass
    */
   NIR_PASS(_, nir, kk_nir_lower_descriptors_for_binding_model, features, rs,
            set_layout_count, set_layouts, raw_texel);

   if (features & BITFIELD_BIT(KK_FEAT_SLICED_3D))
      NIR_PASS(_, nir, kk_nir_lower_sliced_3d);

   /* KK_WORKAROUND_16 - must be called after kk_nir_lower_descriptors(), which
    * lowers all image intrinsics to be bindless */
   if (rs->images ==
          VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2 &&
       !(pdev->settings.disabled_workarounds & BITFIELD64_BIT(16)))
      NIR_PASS(_, nir, msl_lower_robustness2_images);

   /* KK_WORKAROUND_18 */
   if (!(pdev->settings.disabled_workarounds & BITFIELD64_BIT(18)))
      NIR_PASS(_, nir, kk_nir_add_device_barrier_workaround);

   NIR_PASS(_, nir, kk_nir_lower_textures);

   if (features & BITFIELD_BIT(KK_FEAT_NULL_DESCRIPTOR))
      NIR_PASS(_, nir, kk_nir_lower_null_images);

   if (sparse_instructions)
      NIR_PASS(_, nir, kk_nir_lower_sparse);

   kk_lower_compute_derivatives(nir);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
}

static void
kk_lower_nir_untracked(struct kk_device *dev, nir_shader *nir, bool emulated_stage,
             bool defer_hw_vs,
             const struct vk_pipeline_robustness_state *rs,
             uint32_t set_layout_count,
             struct vk_descriptor_set_layout *const *set_layouts,
             const struct vk_graphics_pipeline_state *state,
             kk_feature_key features,
             const struct vk_shader_compile_info *compile_info,
             const uint64_t *embedded)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);

   if (nir->info.io_lowered)
      return;

   if (KK_DEBUG(FORCE_ROBUSTNESS))
      rs = &rs_all_supported;

   const nir_opt_access_options access_options = {
      .is_vulkan = true,
   };
   NIR_PASS(_, nir, nir_opt_access, &access_options);

   if (nir->info.ray_queries)
      NIR_PASS(_, nir, kk_nir_lower_ray_queries);

   const struct lower_ycbcr_state ycbcr_state = {
      .set_layout_count = set_layout_count,
      .set_layouts = set_layouts,
      .info = compile_info,
   };
   NIR_PASS(_, nir, nir_vk_lower_ycbcr_tex, lookup_ycbcr_conversion,
            &ycbcr_state);

   const bool descriptor_heap = compile_info &&
      (compile_info->flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT);
   if (descriptor_heap)
      NIR_PASS(_, nir, kk_nir_prepare_descriptor_heaps,
               compile_info->embedded_sampler_count, embedded, features & BITFIELD_BIT(KK_FEAT_SPARSE_BUFFER));

   bool poly_preraster = state &&
      (state->shader_stages & (VK_SHADER_STAGE_GEOMETRY_BIT |
                              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT));
   bool poly_multiview = poly_preraster && state->mv->view_mask;
   bool last_preraster = nir->info.stage == MESA_SHADER_GEOMETRY ||
      (nir->info.stage == MESA_SHADER_TESS_EVAL &&
       !(state->shader_stages & VK_SHADER_STAGE_GEOMETRY_BIT));
   /* Only active multiview needs a separate Layer/EmitVertex pass. The
    * ordinary descriptor pass folds remaining ViewIndex loads to zero. */
   if (poly_multiview && nir->info.stage != MESA_SHADER_FRAGMENT)
      NIR_PASS(_, nir, kk_nir_lower_poly_multiview, poly_multiview,
               last_preraster);

   if ((nir->info.stage == MESA_SHADER_TASK || nir->info.stage == MESA_SHADER_MESH) &&
       state && state->mv && state->mv->view_mask)
      NIR_PASS(_, nir, kk_nir_lower_poly_multiview, true, false);

   /* Massage IO related variables to please Metal */
   if (nir->info.stage == MESA_SHADER_VERTEX) {
      if (!poly_multiview)
         NIR_PASS(_, nir, kk_nir_lower_vs_multiview, state->mv->view_mask);

      /* kk_nir_lower_vs_multiview may create a temporary array to assign the
       * correct view index. Since we don't handle derefs, we need to get rid of
       * them. */
      NIR_PASS(_, nir, nir_lower_vars_to_scratch, 0,
               glsl_get_natural_size_align_bytes,
               glsl_get_natural_size_align_bytes);
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, kk_nir_lower_fs_multiview, state->mv->view_mask);

      /* Run before nir_lower_input_attachments() below, which rewrites the
       * subpass loads it looks for. */
      if (kk_fs_needs_forced_depth_write(nir, state))
         NIR_PASS(_, nir, msl_ensure_depth_write);

      msl_nir_lower_input_attachments(nir, state->ial, state->cal,
         state->rp, dev->vk.enabled_features.fragmentShaderPixelInterlock &&
         kk_interlock_multisample_pipeline(state) && kk_interlock_pixel_shader(nir));
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT &&
       (state->shader_stages & VK_SHADER_STAGE_MESH_BIT_EXT))
      kk_poly_layer_lower_fs(nir);

   /* Common msl texture lowering needs to happen after ycbcr lowering and
    * before descriptor lowering. */
   if (features & BITFIELD_BIT(KK_FEAT_LAYERED_BLOCK_VIEWS))
      NIR_PASS(_, nir, kk_nir_prepare_layered_bc_cubes, rs, set_layout_count, set_layouts, descriptor_heap);
   if (features & BITFIELD_BIT(KK_FEAT_NON_SEAMLESS_CUBE))
      NIR_PASS(_, nir, kk_nir_lower_non_seamless_cube, set_layout_count, set_layouts);

   NIR_PASS(_, nir, msl_lower_textures, pdev->info.gpu_apple_family < 10);

   /* Lower push constants before lower_descriptors */
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);

   NIR_PASS(_, nir, nir_lower_memory_model);

   if (features & BITFIELD_BIT(KK_FEAT_SPARSE_BUFFER)) {
      NIR_PASS(_, nir, kk_nir_lower_sparse_buffer_io, rs, set_layout_count, set_layouts);
      if (descriptor_heap)
         NIR_PASS(_, nir, kk_nir_finish_sparse_heap_buffers);
   }

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,
            nir_address_format_64bit_global);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo,
            kk_buffer_addr_format(rs->storage_buffers));
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,
            kk_buffer_addr_format(rs->uniform_buffers));

   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_vec4,
            nir_lower_io_lower_64bit_to_32 |
               nir_lower_io_use_interpolated_input_intrinsics);

   NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
            shared_var_info);

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
            nir_address_format_32bit_offset);

   kk_lower_task_payload(nir);

   if (nir->info.zero_initialize_shared_memory && nir->info.shared_size > 0) {
      /* QMD::SHARED_MEMORY_SIZE requires an alignment of 256B so it's safe to
       * align everything up to 16B so we can write whole vec4s.
       */
      nir->info.shared_size = align(nir->info.shared_size, 16);
      NIR_PASS(_, nir, nir_zero_initialize_shared_memory, nir->info.shared_size,
               16);

      /* We need to call lower_compute_system_values again because
       * nir_zero_initialize_shared_memory generates load_invocation_id which
       * has to be lowered to load_invocation_index.
       */
      NIR_PASS(_, nir, nir_lower_compute_system_values, NULL);
   }

   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~nir_var_function_temp);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_shader_in | nir_var_shader_out | nir_var_system_value,
            NULL);
   nir->info.io_lowered = true;

   /* Required before kk_nir_lower_vbo so load_input intrinsics' parents are
    * load_const, otherwise the pass will complain */
   NIR_PASS(_, nir, nir_opt_constant_folding);

   /* These passes operate on lowered IO. */
   if ((nir->info.stage == MESA_SHADER_VERTEX ||
        nir->info.stage == MESA_SHADER_TESS_EVAL) &&
       !emulated_stage) {
      bool is_point =
         nir->info.stage == MESA_SHADER_TESS_EVAL
            ? nir->info.tess.point_mode
            : state->ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      if (defer_hw_vs)
         NIR_PASS(_, nir, msl_nir_vs_io_types);
      else
         kk_lower_hw_vs(nir, state, is_point);
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, msl_nir_fs_io_types);
   }

   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      kk_finish_nir(dev, nir, rs, set_layout_count, set_layouts, features,
         compile_info ? compile_info->flags : 0);
}

static void
kk_lower_nir(struct kk_device *dev, nir_shader *nir, bool emulated_stage,
             bool defer_hw_vs,
             const struct vk_pipeline_robustness_state *rs,
             uint32_t set_layout_count,
             struct vk_descriptor_set_layout *const *set_layouts,
             const struct vk_graphics_pipeline_state *state,
             kk_feature_key features,
             const struct vk_shader_compile_info *compile_info,
             const uint64_t *embedded)
{
   KK_PERF_VOID(dev, KK_PERF_NIR,
      kk_lower_nir_untracked(dev, nir, emulated_stage, defer_hw_vs, rs, set_layout_count,
                            set_layouts, state, features, compile_info, embedded));
}

static const struct vk_shader_ops kk_shader_ops;

static void kk_interlock_shader_finish(struct kk_interlock_shader *);
void kk_interlock_pipeline_finish(struct kk_interlock_pipeline *);

/* Fold already-produced metadata once before executable publication. No IR
 * walk, descriptor inspection or mutable sampler snapshot is involved. */
static void
kk_shader_sampler_info(struct kk_shader *shader)
{
   bool uses = shader->gs_main.uses_sampler_emulation ||
               shader->polygon_native.uses_sampler_emulation ||
               shader->polygon_native_fs.uses_sampler_emulation;
   for (unsigned i = 0; i < ARRAY_SIZE(shader->msl_data); ++i)
      uses |= shader->msl_data[i].uses_sampler_emulation;
   for (unsigned i = 0; i < ARRAY_SIZE(shader->polygon); ++i)
      uses |= shader->polygon[i].uses_sampler_emulation;
   for (unsigned i = 0; i < ARRAY_SIZE(shader->gs_aux); ++i)
      uses |= shader->gs_aux[i].uses_sampler_emulation;
   shader->info.uses_sampler_emulation = uses;
}

static void
kk_shader_destroy(struct vk_device *vk_dev, struct kk_shader *shader,
                  const VkAllocationCallbacks *pAllocator)
{
   struct kk_device *dev = container_of(vk_dev, struct kk_device, vk);

   if (shader->info.stage == MESA_SHADER_COMPUTE && shader->pipeline.compute.pending)
      kk_async_compute_shader_finish(shader);
   if (shader->vk.stage == MESA_SHADER_VERTEX && shader->gs_mesh)
      kk_shader_destroy(vk_dev, shader->gs_mesh, pAllocator);
   kk_shader_embedded_samplers_finish(dev, shader);
   const bool mesh_stage = kk_shader_stage_has_mesh_info(shader->vk.stage);
   kk_sparse_shader_finish(shader);
   if (mesh_stage && shader->mesh && shader->mesh->runtime) {
      struct kk_mesh_shader_runtime *runtime = shader->mesh->runtime;
      for (unsigned i = 0; i < 2; ++i)
         if (runtime->retained[i])
            kk_mesh_release_pipeline(dev, shader, runtime->retained[i],
               i ? KK_PIPELINE_MESH_READER : KK_PIPELINE_MESH_CAPTURE);
      if (runtime->virtual)
         kk_mesh_release_pipeline(dev, shader, runtime->virtual, KK_PIPELINE_MESH_VIRTUAL);
      for (unsigned i = 0; i < 2; ++i)
         if (runtime->sliced[i])
            kk_mesh_release_pipeline(dev, shader, runtime->sliced[i],
               i ? KK_PIPELINE_MESH_VIRTUAL_SLICED : KK_PIPELINE_MESH_SLICED);
      free(runtime);
   }
   free(shader->lazy_polygon);
   kk_attachment_pipelines_finish(shader);
   kk_interlock_pipeline_finish(shader->interlock_pipeline);
   kk_interlock_shader_finish(shader->interlock);
   struct kk_pipeline_handles *pipe = &shader->pipeline;

   if (shader->info.stage == MESA_SHADER_COMPUTE) {
      if (shader->dgc_compute)
         mtl_release(shader->dgc_compute);
      mtl_release(pipe->cs);
      pipe->cs = NULL;
   } else if (shader->info.stage == MESA_SHADER_FRAGMENT) {
      kk_vrs_source_destroy(dev, shader->vrs_source);
   } else if ((shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_MESH)) {
      /* Pre-render compute pipelines. */
      for (uint32_t i = 0u; i < pipe->gfx.pre_render_count; ++i) {
         mtl_release(pipe->gfx.pre_render[i]);
         pipe->gfx.pre_render[i] = NULL;
      }
      pipe->gfx.pre_render_count = 0u;

      /* Graphics pipeline. */
      mtl_release(pipe->gfx.render);
      if (pipe->gfx.ds_handle)
         mtl_release(pipe->gfx.ds_handle);
      pipe->gfx.render = NULL;
      pipe->gfx.ds_handle = NULL;

      for (uint32_t i = 0u; i < pipe->gfx.pre_render_count; ++i)
         mtl_release(pipe->gfx.pre_render[i]);

      /* Serialization data. */
      u_foreach_bit(i, shader->info.vs.additional_stages_bits) {
         struct msl_compile_data *data = &shader->msl_data[i];
         ralloc_free((void *)data->entrypoint_name);
         ralloc_free((void *)data->code);
      }
   }

   struct msl_compile_data *data = &shader->msl_data[shader->info.stage];
   ralloc_free((void *)data->entrypoint_name);
   ralloc_free((void *)data->code);

   u_foreach_bit(i, shader->info.gs_aux_mask) {
      if ((shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_MESH))
         mtl_release(pipe->gfx.gs_aux[i]);
      ralloc_free(shader->gs_aux[i].entrypoint_name);
      ralloc_free(shader->gs_aux[i].code);
   }
   if ((shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_MESH)) {
      mtl_release(pipe->gfx.polygon_render);
      mtl_release(pipe->gfx.conservative_render);
      mtl_release(pipe->gfx.polygon_capture);
      mtl_release(pipe->gfx.polygon_xfb);
   }
   for (unsigned i = 0; i < ARRAY_SIZE(shader->polygon); ++i) {
      ralloc_free(shader->polygon[i].entrypoint_name);
      ralloc_free(shader->polygon[i].code);
   }
   ralloc_free((void *)shader->polygon_native.entrypoint_name);
   ralloc_free((void *)shader->polygon_native.code);
   ralloc_free((void *)shader->polygon_native_fs.entrypoint_name);
   ralloc_free((void *)shader->polygon_native_fs.code);
   ralloc_free((void *)shader->gs_main.entrypoint_name);
   ralloc_free((void *)shader->gs_main.code);

   vk_shader_free(&dev->vk, pAllocator, &shader->vk);
}

static bool
gather_vs_inputs(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_input)
      return false;

   struct nir_io_semantics io = nir_intrinsic_io_semantics(intr);
   BITSET_WORD *attribs_read = data;
   BITSET_SET(attribs_read, (io.location - VERT_ATTRIB_GENERIC0));
   return false;
}

static uint64_t
fs_flat_output_mask(nir_shader *nir)
{
   uint64_t result = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block_safe(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               if (intr->intrinsic == nir_intrinsic_load_input)
                  result |= BITFIELD64_BIT(nir_intrinsic_io_semantics(intr).location);
            }
         }
      }
   }
   return result;
}

static void
gather_shader_info(struct kk_shader *shader, nir_shader *nir,
                   const struct vk_graphics_pipeline_state *state)
{
   shader->info.stage = nir->info.stage;
   shader->info.uses_per_draw_data = msl_gather_uses_per_draw_data(nir);
   shader->info.num_cull_distances = nir->info.cull_distance_array_size;
   if (nir->info.stage == MESA_SHADER_VERTEX) {
      nir_shader_intrinsics_pass(nir, gather_vs_inputs, nir_metadata_all,
                                 &shader->info.vs.attribs_read);
      shader->info.vs.dynamic_vi = !kk_vi_is_static(state);
      shader->info.outputs_written = nir->info.outputs_written;
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* Some meta shaders like vk-meta-resolve will have depth_layout as NONE
       * which is not a valid Metal layout */
      if (nir->info.fs.depth_layout == FRAG_DEPTH_LAYOUT_NONE)
         nir->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_ANY;
      shader->info.outputs_written = nir->info.outputs_written;
      shader->info.fs.flat_outputs = fs_flat_output_mask(nir);
      shader->info.fs.uses_flat_varyings = shader->info.fs.flat_outputs != 0;
   } else if (nir->info.stage == MESA_SHADER_COMPUTE) {
      shader->info.cs.local_size.x = nir->info.workgroup_size[0];
      shader->info.cs.local_size.y = nir->info.workgroup_size[1];
      shader->info.cs.local_size.z = nir->info.workgroup_size[2];
   }
}

static void
modify_nir_info(nir_shader *nir)
{
   /* Fragment info was gathered at the last SSA point in msl_optimize_nir. */
   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   if (nir->info.stage == MESA_SHADER_VERTEX) {
      /* Vertex attribute fetch is done in shader through argument buffers. */
      nir->info.inputs_read = 0u;
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* Some meta shaders like vk-meta-resolve will have depth_layout as NONE
       * which is not a valid Metal layout */
      if (nir->info.fs.depth_layout == FRAG_DEPTH_LAYOUT_NONE)
         nir->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_ANY;

      /* These values are part of the declaration and go with IO. We only
       * require the instrunctions to understand interpolation mode. */
      BITSET_CLEAR(nir->info.system_values_read,
                   SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL);
      BITSET_CLEAR(nir->info.system_values_read,
                   SYSTEM_VALUE_BARYCENTRIC_PERSP_SAMPLE);
      BITSET_CLEAR(nir->info.system_values_read,
                   SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTROID);
      BITSET_CLEAR(nir->info.system_values_read,
                   SYSTEM_VALUE_BARYCENTRIC_LINEAR_PIXEL);
      BITSET_CLEAR(nir->info.system_values_read,
                   SYSTEM_VALUE_BARYCENTRIC_LINEAR_CENTROID);
      BITSET_CLEAR(nir->info.system_values_read,
                   SYSTEM_VALUE_BARYCENTRIC_LINEAR_SAMPLE);

      /* Perspective [[barycentric_coord]] returns wrong values unless
       * [[position]] is also declared. */
      if (BITSET_TEST(nir->info.system_values_read,
                      SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD))
         BITSET_SET(nir->info.system_values_read, SYSTEM_VALUE_FRAG_COORD);
   }
}

static bool
lower_vertex_id_zero_base(struct nir_builder *b, nir_intrinsic_instr *intrin,
                          void *data)
{
   if (intrin->intrinsic != nir_intrinsic_load_vertex_id_zero_base)
      return false;

   b->cursor = nir_instr_remove(&intrin->instr);

   nir_def *id = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_def_rewrite_uses(&intrin->def, id);

   return true;
}

static bool
kk_nir_lower_vertex_id_zero_base(struct nir_shader *nir)
{
   bool progress = nir_shader_intrinsics_pass(nir, lower_vertex_id_zero_base,
                                              nir_metadata_control_flow, NULL);

   if (progress) {
      BITSET_SET(nir->info.system_values_read,
                 SYSTEM_VALUE_GLOBAL_INVOCATION_ID);
      BITSET_CLEAR(nir->info.system_values_read,
                   SYSTEM_VALUE_VERTEX_ID_ZERO_BASE);
   }
   return progress;
}

static void
kk_finish_msl_nir(nir_shader *nir, bool statistics)
{
   NIR_PASS(_, nir, kk_nir_lower_poly, statistics);
   msl_optimize_nir(nir);
   modify_nir_info(nir);
}

/* Consumes nir */
static void
kk_compile_msl_options(struct kk_device *dev, nir_shader *nir,
               const struct vk_graphics_pipeline_state *state,
               struct msl_compile_data *data,
               const struct kk_compile_options *opts,
               bool *has_framebuffer_reads, uint8_t *framebuffer_read_mask)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);
   KK_PERF_VOID(dev, KK_PERF_NIR,
                kk_finish_msl_nir(nir, opts && opts->pipeline_statistics));
   if (has_framebuffer_reads)
      *has_framebuffer_reads = nir->info.stage == MESA_SHADER_FRAGMENT &&
                               nir->info.outputs_read != 0;
   if (framebuffer_read_mask)
      *framebuffer_read_mask = nir->info.stage == MESA_SHADER_FRAGMENT
         ? nir->info.outputs_read >> FRAG_RESULT_DATA0 : 0;

   struct nir_to_msl_options translate_options = opts && opts->msl_custom
      ? *opts->msl_custom : (struct nir_to_msl_options){0};
   translate_options.mem_ctx = NULL;
   translate_options.disabled_workarounds = pdev->settings.disabled_workarounds;
   translate_options.native_sampler_lod_bias = pdev->info.gpu_apple_family >= 10;
   translate_options.native_query_texture_lod = pdev->info.supports_query_texture_lod;
   translate_options.per_vertex_inputs = opts ? opts->native_per_vertex : NULL;
   translate_options.native_atomic64_minmax = pdev->info.gpu_apple_family >= 8;
   struct kk_mesh_msl_options mesh_options;
   if (nir->info.stage == MESA_SHADER_TASK || nir->info.stage == MESA_SHADER_MESH) {
      mesh_options.queries = opts && opts->mesh_queries;
      mesh_options.retained_strides = opts ? opts->mesh_retained_strides : NULL;
      mesh_options.retained_fields = opts ? opts->mesh_retained_fields : NULL;
      mesh_options.view_mask = nir->info.stage == MESA_SHADER_MESH && state && state->mv ? state->mv->view_mask : 0;
      mesh_options.view_index_offset = offsetof(struct kk_per_draw_data, multiview_index);
      mesh_options.flat_heap = nir->info.stage == MESA_SHADER_MESH &&
         opts && opts->per_vertex_map && opts->per_vertex_map->active &&
         opts->per_vertex_map->canonical_heap;
      mesh_options.projected_basis = mesh_options.flat_heap &&
         opts->per_vertex_map->projected_basis;
      mesh_options.flat_heap_offset = mesh_options.flat_heap
         ? offsetof(struct kk_per_draw_data, per_vertex_heap) : 0;
      translate_options.mesh = &mesh_options;
   }
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      for (uint32_t i = 0u; i < MAX_DRAW_BUFFERS; ++i) {
         /* If the attachment is unused, default to 4 to avoid issues with alpha
          * to coverage with unused attachments. */
         translate_options.rts_component_count[i] = 4;
      }
      for (uint32_t i = 0u; i < state->rp->color_attachment_count; ++i) {
         uint8_t logical_index = kk_get_logical_color_att_index(state, i);
         if (logical_index == MESA_VK_ATTACHMENT_UNUSED) {
            continue;
         }
         enum pipe_format format =
            vk_format_to_pipe_format(state->rp->color_attachment_formats[i]);
         if (format != PIPE_FORMAT_NONE) {
            translate_options.rts_component_count[logical_index] =
               format == PIPE_FORMAT_A8_UNORM ? 4 :
               util_format_get_nr_components(format);
         }
      }
   }

   data->code = KK_PERF_CALL(dev, KK_PERF_MSL,
      nir_to_msl(nir, &translate_options));
   data->requires_msl41 = translate_options.requires_msl41;
   data->uses_atomic64_locks = translate_options.uses_atomic64_locks;
   data->uses_matrix_coordinates = translate_options.uses_matrix_coordinates;
   if (unlikely(dev->perf))
      kk_perf_msl_source(dev->perf, nir->info.stage, data->code);
   data->uses_pipeline_statistics = translate_options.uses_pipeline_statistics;
   data->uses_sampler_emulation = translate_options.uses_sampler_emulation;
   data->requires_sampler_emulation = translate_options.requires_sampler_emulation;
   const char *entrypoint_name = nir_shader_get_entrypoint(nir)->function->name;

   /* We need to steal so it doesn't get destroyed with the nir. Needs to happen
    * after nir_to_msl since that's where we rename the entrypoint.
    */
   ralloc_steal(NULL, (void *)entrypoint_name);
   data->entrypoint_name = (char *)entrypoint_name;

   if (KK_DEBUG(MSL)) {
      blake3_hash hash; char hex[BLAKE3_HEX_LEN];
      _mesa_blake3_compute(data->code, strlen(data->code), hash);
      _mesa_blake3_format(hex, hash);
      mesa_logi("// KK_MSL blake3=%s stage=%u bytes=%zu\n%s\n", hex,
                nir->info.stage, strlen(data->code), data->code);
   }

   ralloc_free(nir);
}

void
kk_compile_msl(struct kk_device *dev, nir_shader *nir,
               const struct vk_graphics_pipeline_state *state,
               struct msl_compile_data *data)
{
   kk_compile_msl_options(dev, nir, state, data, NULL, NULL, NULL);
}

#include "kk_conservative_nir.h"
#include "kk_polygon_scalar_io.h"
#include "kosmickrisp/libkk/kk_gs_mesh.h"
#include "kk_gs_mesh_nir.h"
#include "kk_poly_layer_replay_nir.h"
#include "kk_poly_retained_raster_types.h"
#include "kk_poly_layer_multiview_nir.h"

static void
kk_vrs_finish_captured_fragment(nir_shader *point_fs,
   const struct kk_shader *prev_stage, const struct vk_graphics_pipeline_state *state,
   const struct kk_compile_options *opts,
   const struct vk_pipeline_robustness_state *robustness, bool pixel_envelope)
{
      bool point_line_effects = kk_line_effects_for_primitive(state,
         prev_stage ? prev_stage->info.polygon.primitive_vertices : 0);
      if (!opts->poly_layer_replay || (prev_stage && prev_stage->info.polygon_point))
      if (!prev_stage || prev_stage->info.polygon.primitive_vertices != 1)
      nir_shader_intrinsics_pass(point_fs, kk_polygon_point_coord,
                                 nir_metadata_control_flow, point_line_effects ? (void *)1 : NULL);
      kk_line_fragment(point_fs, point_line_effects, state);
      if (opts->poly_layer_replay) kk_poly_layer_lower_fs(point_fs);
      if (pixel_envelope)
         nir_shader_intrinsics_pass(point_fs, kk_interlock_trim_native_output,
            nir_metadata_control_flow, (void *)state);
      NIR_PASS(_, point_fs, kk_nir_lower_descriptors, false, robustness, 0, NULL);
}

mtl_render_pipeline_descriptor *
kk_poly_retained_pipeline_descriptor(const struct kk_shader *shader, bool conservative)
{
   return shader && shader->attachment_pipelines ?
      shader->attachment_pipelines->descriptor[conservative ? 2 : 1] : NULL;
}

void
kk_poly_retained_lower_fragment(nir_shader *nir, const struct kk_shader_info *producer,
   const struct vk_graphics_pipeline_state *state)
{
   bool effects = kk_line_effects_for_primitive(state,
      producer->polygon.primitive_vertices);
   if (producer->polygon.primitive_vertices != 1)
      nir_shader_intrinsics_pass(nir, kk_polygon_point_coord,
         nir_metadata_control_flow, effects ? (void *)1 : NULL);
   kk_line_fragment(nir, effects, state);
   kk_poly_layer_lower_fs(nir);
}

static VkResult
kk_compile_shader(struct kk_device *dev, nir_shader *nir,
                  struct kk_shader *prev_stage,
                  const struct vk_pipeline_robustness_state *robustness,
                  const struct vk_graphics_pipeline_state *state,
                  const struct kk_compile_options *opts,
                  const VkAllocationCallbacks *pAllocator,
                  struct kk_shader **shader_out)
{
   assert(nir->info.io_lowered && "nir must have lowered io");
   static const struct kk_compile_options defaults;
   if (!opts)
      opts = &defaults;
   const bool pixel_envelope = opts->pixel_envelope;
   const bool pixel_sample_shading = opts->pixel_sample_shading;
   const struct kk_per_vertex_map *per_vertex_map = opts->per_vertex_map;
   nir_shader *conservative_fs = nir->info.stage == MESA_SHADER_FRAGMENT ? opts->conservative_fs : NULL;
   struct kk_physical_device *pdev = kk_device_physical(dev);

   struct kk_shader *shader;
   VkResult result = VK_SUCCESS;

   mesa_shader_stage stage = nir->info.stage;
   bool vrs_captured_required = false;
   shader = vk_shader_zalloc(&dev->vk, &kk_shader_ops, stage, pAllocator,
                             kk_shader_size(stage));
   if (shader == NULL) {
      ralloc_free(opts->vrs_source);
      ralloc_free(nir);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   kk_shader_init_mesh_state(shader, stage);

   shader->info.conservative = kk_conservative_possible(state);
   shader->cache_uses_embedded_samplers = nir->info.uses_embedded_samplers;
   if (pixel_envelope && stage != MESA_SHADER_FRAGMENT && state->mv && state->mv->view_mask) {
      NIR_PASS(_, nir, kk_interlock_lower_source_views);
      NIR_PASS(_, nir, kk_nir_lower_descriptors, false, robustness, 0, NULL);
   }
   gather_shader_info(shader, nir, state);
   shader->info.interlock_envelope = pixel_envelope;
   shader->info.poly_layer_replay = opts->poly_layer_replay;
   if (stage == MESA_SHADER_FRAGMENT)
      shader->info.fs.line_geometry_stipple = kk_line_geometry_stipple(nir, state);
   if (stage == MESA_SHADER_FRAGMENT) {
      shader->info.conservative_full_coverage =
         BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FULLY_COVERED);
      if (shader->info.conservative_full_coverage && !opts->independent_interface) {
         nir_shader_intrinsics_pass(nir, kk_conservative_disabled_inner,
                                    nir_metadata_control_flow, NULL);
         nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
      }
   }
   if (stage == MESA_SHADER_TASK || stage == MESA_SHADER_MESH) {
      shader->mesh->info.local_size = (struct mtl_size){
         nir->info.workgroup_size[0], nir->info.workgroup_size[1], nir->info.workgroup_size[2]};
      shader->mesh->info.payload_size = nir->info.task_payload_size;
      shader->mesh->info.queries = opts->mesh_queries;
      if (stage == MESA_SHADER_TASK) {
         shader->mesh->info.task_grid_bound = kk_task_grid_bound(nir);
      } else {
         shader->mesh->info.primitive = nir->info.mesh.primitive_type;
         shader->mesh->info.flat_heap = per_vertex_map && per_vertex_map->active &&
            per_vertex_map->canonical_heap;
         shader->mesh->info.projected_basis = shader->mesh->info.flat_heap &&
            per_vertex_map->projected_basis;
         shader->mesh->info.max_primitives = nir->info.mesh.max_primitives_out;
         shader->info.uses_per_draw_data |= shader->mesh->info.flat_heap;
         shader->mesh->info.max_vertices = nir->info.mesh.max_vertices_out;
         shader->mesh->info.view_mask = state && state->mv ? state->mv->view_mask : 0;
         shader->info.uses_per_draw_data |= shader->mesh->info.view_mask != 0;
         shader->info.num_cull_distances = 0;
         if (prev_stage && prev_stage->info.stage == MESA_SHADER_TASK) {
            shader->mesh->info.has_task = true;
            shader->mesh->info.task_local_size = prev_stage->mesh->info.local_size;
            shader->mesh->info.task_payload_size = prev_stage->mesh->info.payload_size;
            shader->mesh->info.task_grid_bound = prev_stage->mesh->info.task_grid_bound;
         }
      }
   }
   kk_barycentric_shader_info(shader, per_vertex_map);
   if (stage == MESA_SHADER_FRAGMENT &&
       (pixel_envelope || opts->polygon_capture || opts->poly_layer_replay)) {
      shader->info.fs.noperspective_outputs = kk_polygon_noperspective_outputs(nir);
      nir_shader *point_fs = nir_shader_clone(NULL, nir);
      kk_vrs_finish_captured_fragment(point_fs, prev_stage, state, opts,
         robustness, pixel_envelope);
      shader->info.polygon_point = true;
      if (pixel_envelope || opts->polygon_regular || opts->poly_layer_replay) {
         vrs_captured_required = dev->vrs && opts->vrs_source && !pixel_envelope;
         nir_shader *captured = vrs_captured_required ?
            nir_shader_clone(NULL, opts->vrs_source) : NULL;
         if (captured) {
            /* The retained named source predates ordinary DCE. Drop only its
             * dead descriptor nodes before the one captured-reader finish. */
            NIR_PASS(_, captured, nir_opt_dce);
            bool sample_frequency;
            bool known_frequency = kk_nir_vrs_frequency(opts->vrs_source,
               &sample_frequency, false);
            kk_vrs_finish_captured_fragment(captured, prev_stage, state, opts,
               robustness, pixel_envelope);
            if (known_frequency)
               kk_nir_vrs_mark_frequency(captured, sample_frequency);
         }
         kk_compile_msl(dev, point_fs, state, &shader->polygon[3]);
         if (captured) {
            shader->vrs_source = kk_vrs_source_capture(dev, captured,
               per_vertex_map, state, &shader->polygon[3]);
            ralloc_free(captured);
         }
      } else {
         ralloc_free(point_fs);
      }
      if (conservative_fs) {
         kk_conservative_lower_fs(conservative_fs, &prev_stage->info.polygon);
         if (opts->poly_layer_replay) kk_poly_layer_lower_fs(conservative_fs);
         kk_compile_msl(dev, conservative_fs, state, &shader->polygon[5]);
      }
   }

   if (stage == MESA_SHADER_FRAGMENT) kk_line_fragment(nir, false, state);

   unsigned num_cull_distances =
      prev_stage ? prev_stage->info.num_cull_distances : 0;
   /* A GS lowers clip and cull distances on its rasterization VS instead; the
    * stage feeding it has none after linking */
   if (stage == MESA_SHADER_FRAGMENT ||
       (stage == MESA_SHADER_VERTEX && nir->info.next_stage == MESA_SHADER_FRAGMENT) ||
       (stage == MESA_SHADER_TESS_EVAL && nir->info.next_stage == MESA_SHADER_FRAGMENT))
      msl_nir_lower_clip_cull_distance(nir, num_cull_distances);

   /* When using poly to emulate tessellation or GS, the VS and TCS are turned
    * to compute shaders that will be dispatched before the draw call. The TES
    * becomes a VS, or a compute shader when a GS follows. A GS becomes a
    * compute pass plus a VS that rasterizes its output. */
   if (stage == MESA_SHADER_VERTEX) {
      /* VBO lowering needs to go here otherwise, the linking step removes all
       * inputs since we read vertex attributes from UBOs. */
      kk_lower_vs_vbo(nir, state, robustness);
      if (nir->info.next_stage != MESA_SHADER_FRAGMENT) {
         /* TODO_KOSMICKRISP: Poly lowering breaks with vectorized I/O less than
          * 32-bit, which we support. Lower vertex outputs to scalar. */
         NIR_PASS(_, nir, nir_lower_io_to_scalar, nir_var_shader_out, NULL,
                  NULL);

         NIR_PASS(_, nir, poly_nir_lower_vs_before_gs);
         NIR_PASS(_, nir, kk_nir_lower_vertex_id_zero_base);
         nir->info.stage = MESA_SHADER_COMPUTE;
         memset(&nir->info.cs, 0, sizeof(nir->info.cs));
         nir->xfb_info = NULL;
         NIR_PASS(_, nir, poly_nir_lower_sw_vs);
      } else {
         /* Metal's instance_id contains base_instance. When the emulation path
          * is taken, since we launch compute, they correctly get translated.
          * For the non-emulated path we need to subtract base_instance... */
         NIR_PASS(_, nir, msl_nir_lower_instance_id);

         if (per_vertex_map &&
             (per_vertex_map->active || per_vertex_map->canonical_heap)) {
            bool progress = false;
            NIR_PASS(progress, nir, kk_nir_lower_vs_per_vertex_outputs_with_state,
                     per_vertex_map,
                     dev->vk.enabled_features.fragmentShaderBarycentric
                        ? state : NULL);
            if (progress)
               shader->info.uses_per_draw_data = true;
         }
      }
   } else if (stage == MESA_SHADER_TESS_CTRL) {
      /* TODO_KOSMICKRISP: Poly lowering breaks with vectorized I/O less than
       * 32-bit, which we support. Lower TCS inputs & outputs to scalar. */
      NIR_PASS(_, nir, nir_lower_io_to_scalar,
               nir_var_shader_in | nir_var_shader_out, NULL, NULL);

      NIR_PASS(_, nir, poly_nir_lower_tcs, false);

      shader->info.tess.info.ccw = nir->info.tess.ccw;
      shader->info.tess.info.points = nir->info.tess.point_mode;
      shader->info.tess.info.spacing = nir->info.tess.spacing;
      shader->info.tess.info.mode = nir->info.tess._primitive_mode;

      shader->info.tess.tcs_output_patch_size = nir->info.tess.tcs_vertices_out;
      shader->info.tess.tcs_per_vertex_outputs =
         poly_tcs_per_vertex_outputs(nir);
      shader->info.tess.tcs_nr_patch_outputs =
         util_last_bit(nir->info.patch_outputs_written);
      shader->info.tess.tcs_output_stride = poly_tcs_output_stride(nir);
   } else if (stage == MESA_SHADER_TESS_EVAL) {
      /* TODO_KOSMICKRISP: Poly lowering breaks with vectorized I/O less than
       * 32-bit, which we support. Lower TES inputs to scalar, and outputs too
       * when a GS reads them from memory. */
      nir_variable_mode modes = nir_var_shader_in;
      if (nir->info.next_stage == MESA_SHADER_GEOMETRY)
         modes |= nir_var_shader_out;
      NIR_PASS(_, nir, nir_lower_io_to_scalar, modes, NULL, NULL);

      shader->info.tess.info.ccw = nir->info.tess.ccw;
      shader->info.tess.info.points = nir->info.tess.point_mode;
      shader->info.tess.info.spacing = nir->info.tess.spacing;
      shader->info.tess.info.mode = nir->info.tess._primitive_mode;

      bool to_hw_vs = nir->info.next_stage != MESA_SHADER_GEOMETRY;
      /* This destroys info so it needs to happen after the gather */
      NIR_PASS(_, nir, poly_nir_lower_tes, to_hw_vs);
      if (!to_hw_vs) {
         shader->info.outputs_written = nir->info.outputs_written;
         NIR_PASS(_, nir, poly_nir_lower_vs_before_gs);
         nir->info.stage = MESA_SHADER_COMPUTE;
         memset(&nir->info.cs, 0, sizeof(nir->info.cs));
         nir->xfb_info = NULL;
      } else {
         if (per_vertex_map &&
             (per_vertex_map->active || per_vertex_map->canonical_heap)) {
            bool progress = false;
            NIR_PASS(progress, nir, kk_nir_lower_vs_per_vertex_outputs,
                     per_vertex_map);
            if (progress)
               shader->info.uses_per_draw_data = true;
         }
      }
   } else if (stage == MESA_SHADER_GEOMETRY) {
      /* TODO_KOSMICKRISP: Poly lowering breaks with vectorized I/O less than
       * 32-bit, which we support. Lower GS inputs to scalar. */
      NIR_PASS(_, nir, nir_lower_io_to_scalar, nir_var_shader_in, NULL, NULL);

      nir_shader *count = NULL, *rast = NULL, *pre_gs = NULL;
      nir_shader *capture = NULL, *capture_xfb = NULL;
      bool retained_points = opts->poly_layer_replay &&
         u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_POINTS;
      bool polygon = retained_points || ((opts->polygon_capture || opts->poly_layer_replay) &&
         (u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_TRIANGLES ||
          u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_LINES ||
          u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_POINTS));
      const bool reports = nir->xfb_info || opts->geometry_reports;
      NIR_PASS(_, nir, poly_nir_lower_gs_capture, reports ? &count : NULL, &rast,
               reports ? &pre_gs : NULL,
               &shader->info.gs, &shader->info.polygon,
               (polygon || pixel_envelope) ? &capture : NULL,
               (polygon || pixel_envelope) ? &capture_xfb : NULL);
      if (polygon || pixel_envelope) {
         shader->info.polygon_point = polygon;
         nir_shader *point_vs = retained_points ?
            kk_polygon_retained_point_shader(&shader->info.polygon, nir->options) :
            pixel_envelope && mesa_vertices_per_prim(shader->info.gs.mode) < 3 ?
            kk_interlock_source_vertex_shader(&shader->info.polygon, nir->options,
               mesa_vertices_per_prim(shader->info.gs.mode)) :
            kk_polygon_vertex_shader(&shader->info.polygon, nir->options,
               kk_line_effects_for_primitive(state,
                  shader->info.polygon.primitive_vertices), state);
         if (opts->poly_layer_replay) kk_poly_layer_lower_vs(point_vs);
         /* This reader shares the already linked fragment interface with the
          * ordinary GS raster VS, including generated barycentric slots. */
         if (per_vertex_map &&
             (per_vertex_map->active || per_vertex_map->canonical_heap)) {
            bool progress = false;
            NIR_PASS(progress, point_vs, kk_nir_lower_vs_per_vertex_outputs,
                     per_vertex_map);
            if (progress)
               shader->info.uses_per_draw_data = true;
         }
         NIR_PASS(_, point_vs, kk_nir_lower_polygon_scalar_outputs,
                  shader->info.polygon.outputs);
         if (pixel_envelope)
            kk_interlock_source_resolved_cull(point_vs, &shader->info.polygon);
         NIR_PASS(_, point_vs, msl_nir_vs_io_types);
         NIR_PASS(_, point_vs, kk_nir_lower_descriptors, false, robustness, 0, NULL);
         if (pixel_envelope || opts->polygon_regular || opts->poly_layer_replay)
            kk_compile_msl(dev, point_vs, state, &shader->polygon[2]);
         else
            ralloc_free(point_vs);
         if (shader->info.conservative) {
            nir_shader *conservative_vs = kk_conservative_vertex_shader(nir->options);
            if (opts->poly_layer_replay) kk_poly_layer_lower_vs(conservative_vs);
            NIR_PASS(_, conservative_vs, msl_nir_vs_io_types);
            NIR_PASS(_, conservative_vs, kk_nir_lower_descriptors, false, robustness, 0, NULL);
            kk_compile_msl(dev, conservative_vs, state, &shader->polygon[4]);
         }
         nir_shader *programs[] = {capture, capture_xfb};
         for (unsigned i = 0; i < ARRAY_SIZE(programs); ++i) {
            if (!programs[i]) continue;
            programs[i]->info.stage = MESA_SHADER_COMPUTE;
            memset(&programs[i]->info.cs, 0, sizeof(programs[i]->info.cs));
            programs[i]->info.workgroup_size[0] = KK_POLY_WG_SIZE;
            programs[i]->info.workgroup_size[1] = programs[i]->info.workgroup_size[2] = 1;
            msl_preprocess_nir(programs[i]);
            kk_compile_msl_options(dev, programs[i], state, &shader->polygon[i],
                                   opts, NULL, NULL);
         }
      }

      shader->info.has_geometry = true;
      shader->info.geometry = shader->info.gs;
      nir_shader *aux[] = {count, pre_gs};
      for (unsigned i = 0; i < ARRAY_SIZE(aux); ++i) {
         if (!aux[i]) continue;
         shader->info.gs_aux_mask |= BITFIELD_BIT(i);
         aux[i]->info.stage = MESA_SHADER_COMPUTE;
         memset(&aux[i]->info.cs, 0, sizeof(aux[i]->info.cs));
         aux[i]->info.workgroup_size[0] = i ? 1 : KK_POLY_WG_SIZE;
         aux[i]->info.workgroup_size[1] = aux[i]->info.workgroup_size[2] = 1;
         msl_preprocess_nir(aux[i]);
         kk_compile_msl_options(dev, aux[i], state, &shader->gs_aux[i],
                                opts, NULL, NULL);
      }
      shader->info.geometry_statistics =
         shader->gs_aux[1].uses_pipeline_statistics;

      nir->info.stage = MESA_SHADER_COMPUTE;
      memset(&nir->info.cs, 0, sizeof(nir->info.cs));
      nir->xfb_info = NULL;
      kk_compile_msl_options(dev, nir, state, &shader->gs_main, opts, NULL, NULL);

      /* From here on, nir is the rasterization VS of the GS */
      nir = rast;
      /* Layered point output uses the retained reader compiled above.
       * Never clone this ordinary raster body for physical-layer replay. */

      /* poly selects the rasterized vertex through shader_temp variables,
       * localize them so msl_optimize_nir turns them into SSA */
      NIR_PASS(_, nir, nir_lower_global_vars_to_local);

      kk_lower_hw_vs(nir, state, shader->info.gs.mode == MESA_PRIM_POINTS);
      msl_nir_lower_clip_cull_distance(nir, 0);

      if (per_vertex_map &&
          (per_vertex_map->active || per_vertex_map->canonical_heap)) {
         bool progress = false;
         NIR_PASS(progress, nir, kk_nir_lower_vs_per_vertex_outputs,
                  per_vertex_map);
         if (progress)
            shader->info.uses_per_draw_data = true;
      }
      /* Descriptors were lowered with the GS, this lowers the system values
       * the hardware vertex lowering added */
      NIR_PASS(_, nir, kk_nir_lower_descriptors, false, robustness, 0, NULL);
   }

   struct kk_compile_options mesh_options;
   if (stage == MESA_SHADER_MESH) {
      mesh_options = *opts;
      mesh_options.mesh_retained_strides = shader->mesh->info.retained_strides;
      mesh_options.mesh_retained_fields = (uint32_t *)shader->mesh->info.retained_fields;
      opts = &mesh_options;
   }
   if (pixel_envelope && stage == MESA_SHADER_FRAGMENT) {
      result = kk_interlock_compile_fragment(dev, shader, nir, prev_stage, state, robustness, pixel_sample_shading);
      if (result != VK_SUCCESS) {
         ralloc_free(nir);
         kk_shader_destroy(&dev->vk, shader, pAllocator);
         return result;
      }
   }
   if (pixel_envelope && stage == MESA_SHADER_FRAGMENT)
      nir_shader_intrinsics_pass(nir, kk_interlock_trim_native_output,
         nir_metadata_control_flow, (void *)state);
   kk_compile_msl_options(dev, nir, state, &shader->msl_data[stage], opts,
                          NULL, &shader->info.framebuffer_read_mask);

   if (opts->vrs_source) {
      struct kk_vrs_source *source = kk_vrs_source_capture_linked(dev, opts->vrs_source,
         per_vertex_map, state, &shader->msl_data[stage], opts->linked_producer);
      ralloc_free(opts->vrs_source);
      if (!source || (vrs_captured_required && !shader->vrs_source)) {
         kk_vrs_source_destroy(dev, source);
         kk_shader_destroy(&dev->vk, shader, pAllocator);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      kk_vrs_source_attach_captured(source, shader->vrs_source);
      shader->vrs_source = source;
   }
   kk_shader_sampler_info(shader);
   *shader_out = shader;

   return result;
}

static const struct vk_pipeline_robustness_state rs_none = {
   .uniform_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED,
   .storage_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED,
   .images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2,
};

VkResult
kk_compile_nir_shader(struct kk_device *dev, nir_shader *nir,
                      const VkAllocationCallbacks *alloc,
                      struct kk_shader **shader_out)
{
   const struct kk_physical_device *pdev = kk_device_physical(dev);

   assert(nir->info.stage == MESA_SHADER_COMPUTE);
   if (nir->options == NULL)
      nir->options = kk_get_nir_options((struct vk_physical_device *)&pdev->vk,
                                        nir->info.stage, &rs_none);

   struct kk_shader *shader = NULL;
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   VkResult result =
      kk_compile_shader(dev, nir, NULL, &rs_none, NULL, NULL, alloc, &shader);
   if (result != VK_SUCCESS)
      return result;

   *shader_out = shader;

   return VK_SUCCESS;
}

static void
nir_opts(nir_shader *nir, void *data)
{
   bool progress;

   do {
      progress = false;

      NIR_PASS(progress, nir, nir_opt_loop);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);

      NIR_PASS(progress, nir, nir_opt_if, 0);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_licm, NULL);

      NIR_PASS(progress, nir, nir_opt_peephole_select,
               &(nir_opt_peephole_select_options){
                  .limit = 8,
                  .expensive_alu_ok = true,
                  .discard_ok = true,
               });

      NIR_PASS(progress, nir, nir_opt_phi_precision);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);

      NIR_PASS(progress, nir, nir_opt_undef);
      NIR_PASS(progress, nir, nir_opt_loop_unroll);
   } while (progress);
}

static nir_shader *
get_empty_nir(struct kk_device *dev, mesa_shader_stage stage,
              const struct vk_graphics_pipeline_state *state,
              kk_feature_key features)
{
   nir_shader *nir = nir_shader_create(
      NULL, stage,
      kk_get_nir_options(&kk_device_physical(dev)->vk, stage, NULL));

   nir_function *function = nir_function_create(nir, "main_entrypoint");
   function->is_entrypoint = true;
   nir_function_impl_create(function);

   const struct vk_pipeline_robustness_state no_robustness = {
      .storage_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED,
      .uniform_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED,
      .vertex_inputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED,
      .images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED,
      .null_uniform_buffer_descriptor = false,
      .null_storage_buffer_descriptor = false,
   };
   kk_lower_nir(dev, nir, false, false, &no_robustness, 0u, NULL, state, features, NULL, NULL);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   return nir;
}

static VkResult
kk_compile_compute_with_library(struct kk_device *device, struct kk_shader *shader,
   const struct msl_compile_data *data, uint32_t local_size_threads,
   mtl_compute_pipeline_state **pipe, bool indirect,
   mtl_function_descriptor **retained_function, mtl_library *library)
{
   if (library == NULL)
      return VK_ERROR_INVALID_SHADER_NV;

   mtl_function_descriptor *function = NULL;
   VkResult result = kk_msl_function_sampler_base(device, data, library, data->entrypoint_name,
                                   &function);
   if (result != VK_SUCCESS) {
      mtl_release(library);
      return result;
   }

   mtl_compute_pipeline_state *pipeline = NULL;
   result = kk_sampler_create_compute(device, shader, function,
      data->uses_sampler_emulation, data->requires_sampler_emulation,
      local_size_threads, indirect, &pipeline, false);
   if (pipeline && retained_function)
      *retained_function = mtl_retain(function);
   mtl_release(function);
   mtl_release(library);

   if (result != VK_SUCCESS)
      return result;

   *pipe = pipeline;

   return VK_SUCCESS;
}

static VkResult
kk_compile_compute_pipeline_mode_body(struct kk_device *device, struct kk_shader *shader,
                            const struct msl_compile_data *data,
                            uint32_t local_size_threads,
                            mtl_compute_pipeline_state **pipe, bool indirect,
                            mtl_function_descriptor **retained_function)
{
   struct kk_physical_device *pdev = kk_device_physical(device);

   mtl_library *library = KK_PERF_CALL(device, KK_PERF_METAL_LIBRARY, mtl_new_library(
      device->mtl_compiler_handle, data->code, kk_msl_language_version(data, pdev->info.msl_version),
      MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST));
   return kk_compile_compute_with_library(device, shader, data, local_size_threads,
      pipe, indirect, retained_function, library);
}

static VkResult
kk_compile_compute_pipeline_mode(struct kk_device *device, struct kk_shader *shader,
                            const struct msl_compile_data *data,
                            uint32_t local_size_threads,
                            mtl_compute_pipeline_state **pipe, bool indirect,
                            mtl_function_descriptor **retained_function)
{
   void *foreground = mtl_compiler_foreground_begin(device->mtl_compiler_handle);
   VkResult result = kk_compile_compute_pipeline_mode_body(device, shader, data, local_size_threads, pipe, indirect, retained_function);
   mtl_compiler_foreground_end(foreground);
   return result;
}

static VkResult
kk_compile_compute_pipeline(struct kk_device *device, struct kk_shader *shader,
                            const struct msl_compile_data *data,
                            uint32_t threads, mtl_compute_pipeline_state **pipe)
{
   return kk_compile_compute_pipeline_mode(device, shader, data, threads, pipe, false, NULL);
}

static VkResult
kk_compile_async_compute(struct kk_device *device, struct kk_shader *shader)
{
   const struct msl_compile_data *data = &shader->msl_data[MESA_SHADER_COMPUTE];
   struct kk_physical_device *pdev = kk_device_physical(device);
   mtl_library *library = KK_PERF_CALL(device, KK_PERF_METAL_LIBRARY,
      mtl_new_library_async_wait(device->mtl_compiler_handle, data->code,
         kk_msl_language_version(data, pdev->info.msl_version), MTL_MATH_MODE_FAST,
         MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST));
   struct mtl_size local = shader->info.cs.local_size;
   return kk_compile_compute_with_library(device, shader, data, local.x * local.y * local.z,
                                          &shader->pipeline.cs, false, NULL, library);
}

VkResult
kk_compile_indirect_compute_pipeline(struct kk_device *device, struct kk_shader *shader,
   const struct msl_compile_data *data, uint32_t threads,
   mtl_compute_pipeline_state **pipe)
{
   return kk_compile_compute_pipeline_mode(device, shader, data, threads, pipe, true, NULL);
}

VkResult
kk_shader_get_statistics_pre_gs(struct kk_device *device, struct kk_shader *shader,
                               mtl_compute_pipeline_state **pipeline)
{
   struct kk_statistics_pre_gs *statistics = kk_statistics_pre_gs(shader);
   *pipeline = p_atomic_read(&statistics->pipeline);
   if (*pipeline)
      return kk_sampler_compute_pipeline(device, shader, *pipeline, pipeline);
   struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
   simple_mtx_lock(&cache->mutex);
   mtl_compute_pipeline_state *compiled = statistics->pipeline;
   VkResult result = VK_SUCCESS;
   if (!compiled) {
      mtl_function_descriptor *function =
         mtl_specialize_pipeline_statistics(statistics->function);
      if (!function) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      } else {
         result = kk_sampler_create_compute(device, shader, function,
            shader->gs_aux[1].uses_sampler_emulation,
            shader->gs_aux[1].requires_sampler_emulation,
            1, false, &compiled, true);
         mtl_release(function);
         if (compiled)
            p_atomic_set(&statistics->pipeline, compiled);
         else
            result = VK_ERROR_INVALID_SHADER_NV;
      }
   }
   *pipeline = compiled;
   simple_mtx_unlock(&cache->mutex);
   return result == VK_SUCCESS ?
      kk_sampler_compute_pipeline(device, shader, compiled, pipeline) : result;
}

static mtl_depth_stencil_state *
kk_create_ds_state(struct kk_device *device,
                   const struct kk_depth_stencil_key *key)
{
   mtl_stencil_descriptor *front = NULL;
   mtl_stencil_descriptor *back = NULL;
   mtl_depth_stencil_descriptor *descriptor =
      mtl_new_depth_stencil_descriptor();
   mtl_depth_stencil_descriptor_set_depth_write_enabled(
      descriptor, (key->ops >> 3) & 1);
   mtl_depth_stencil_descriptor_set_depth_compare_function(
      descriptor, key->ops & 7);

   if (key->ops & (1 << 4)) {
      back = mtl_new_stencil_descriptor();
      mtl_stencil_descriptor_set_depth_failure_operation(
         back, (key->ops >> 20) & 7);
      mtl_stencil_descriptor_set_stencil_failure_operation(
         back, (key->ops >> 17) & 7);
      mtl_stencil_descriptor_set_depth_stencil_pass_operation(
         back, (key->ops >> 23) & 7);
      mtl_stencil_descriptor_set_stencil_compare_function(
         back, (key->ops >> 26) & 7);
      mtl_stencil_descriptor_set_read_mask(back,
                                           key->back_read);
      mtl_stencil_descriptor_set_write_mask(back,
                                            key->back_write);
      mtl_depth_stencil_descriptor_set_back_face_stencil(descriptor, back);

      front = mtl_new_stencil_descriptor();
      mtl_stencil_descriptor_set_depth_failure_operation(
         front, (key->ops >> 8) & 7);
      mtl_stencil_descriptor_set_stencil_failure_operation(
         front, (key->ops >> 5) & 7);
      mtl_stencil_descriptor_set_depth_stencil_pass_operation(
         front, (key->ops >> 11) & 7);
      mtl_stencil_descriptor_set_stencil_compare_function(
         front, (key->ops >> 14) & 7);
      mtl_stencil_descriptor_set_read_mask(front,
                                           key->front_read);
      mtl_stencil_descriptor_set_write_mask(front,
                                            key->front_write);
      mtl_depth_stencil_descriptor_set_front_face_stencil(descriptor, front);
   }

   mtl_depth_stencil_state *state =
      KK_PERF_CALL(device, KK_PERF_METAL_PIPELINE,
         mtl_new_depth_stencil_state(device->mtl_handle, descriptor));

   if (front)
      mtl_release(front);
   if (back)
      mtl_release(back);
   mtl_release(descriptor);

   return state;
}

/* Native depth/stencil objects are immutable. Keep a bounded device cache,
 * shared by pipeline creation and dynamic state, with an independent reference
 * for every caller. Unused stencil fields do not distinguish native states. */
struct kk_ds_cache_entry {
   struct kk_depth_stencil_key key;
   mtl_depth_stencil_state *state;
};

static uint32_t
kk_ds_key_hash(const void *key)
{
   return _mesa_hash_data(key, sizeof(struct kk_depth_stencil_key));
}

static bool
kk_ds_key_equal(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(struct kk_depth_stencil_key)) == 0;
}

static struct kk_depth_stencil_key
kk_ds_key_from_info(const struct kk_shader_info *info)
{
   struct kk_depth_stencil_key key = {
      .ops = info->vs.depth_compare_op | (info->vs.has_depth_write << 3) |
             (info->vs.has_stencil_test << 4),
   };
   if (info->vs.has_stencil_test) {
      key.ops |= (info->vs.stencil_front.fail << 5) |
                 (info->vs.stencil_front.depth_fail << 8) |
                 (info->vs.stencil_front.pass << 11) |
                 (info->vs.stencil_front.compare << 14) |
                 (info->vs.stencil_back.fail << 17) |
                 (info->vs.stencil_back.depth_fail << 20) |
                 (info->vs.stencil_back.pass << 23) |
                 (info->vs.stencil_back.compare << 26);
      key.front_read = info->vs.stencil_front.compare_mask;
      key.front_write = info->vs.stencil_front.write_mask;
      key.back_read = info->vs.stencil_back.compare_mask;
      key.back_write = info->vs.stencil_back.write_mask;
   }

   return key;
}

static mtl_depth_stencil_state *
kk_compile_ds_key(struct kk_device *device,
                  const struct kk_depth_stencil_key *key)
{
   simple_mtx_lock(&device->depth_stencil_cache_mutex);
   if (!device->depth_stencil_cache)
      device->depth_stencil_cache =
         _mesa_hash_table_create(NULL, kk_ds_key_hash, kk_ds_key_equal);
   struct hash_table *cache = device->depth_stencil_cache;
   struct hash_entry *found = cache ? _mesa_hash_table_search(cache, key) : NULL;
   if (found) {
      struct kk_ds_cache_entry *entry = found->data;
      mtl_depth_stencil_state *state = mtl_retain(entry->state);
      simple_mtx_unlock(&device->depth_stencil_cache_mutex);
      return state;
   }

   /* Creation is cheap native state setup, not shader compilation. Holding
    * this cache's mutex prevents duplicate objects on concurrent misses. */
   mtl_depth_stencil_state *state = kk_create_ds_state(device, key);
   if (state && cache && cache->entries < 1024) {
      struct kk_ds_cache_entry *entry = ralloc(cache, struct kk_ds_cache_entry);
      if (entry) {
         entry->key = *key;
         entry->state = state;
         if (_mesa_hash_table_insert(cache, &entry->key, entry))
            mtl_retain(state); /* Cache ownership; caller keeps creation ref. */
         else
            ralloc_free(entry);
      }
   }
   /* A full cache or host allocation failure preserves ordinary creation. */
   simple_mtx_unlock(&device->depth_stencil_cache_mutex);
   return state;
}

static mtl_depth_stencil_state *
kk_compile_ds_state(struct kk_device *device, struct kk_shader_info *info)
{
   struct kk_depth_stencil_key key = kk_ds_key_from_info(info);
   return kk_compile_ds_key(device, &key);
}

void
kk_depth_stencil_cache_finish(struct kk_device *device)
{
   if (device->depth_stencil_cache) {
      hash_table_foreach(device->depth_stencil_cache, entry) {
         struct kk_ds_cache_entry *ds = entry->data;
         mtl_release(ds->state);
      }
      _mesa_hash_table_destroy(device->depth_stencil_cache, NULL);
   }
   simple_mtx_destroy(&device->depth_stencil_cache_mutex);
}

static void
kk_gather_ds_info(const struct vk_depth_stencil_state *ds, bool has_depth,
                  bool has_stencil, struct kk_shader_info *info)
{
   /* Depth */
   if (has_depth && ds->depth.test_enable) {
      info->vs.has_depth_write = ds->depth.write_enable;
      info->vs.depth_compare_op = ds->depth.compare_op;
   } else {
      info->vs.has_depth_write = false;
      info->vs.depth_compare_op = VK_COMPARE_OP_ALWAYS;
   }

   /* Stencil */
   info->vs.has_stencil_test = has_stencil && ds->stencil.test_enable;
   if (info->vs.has_stencil_test) {
      info->vs.stencil_back.depth_fail = ds->stencil.back.op.depth_fail;
      info->vs.stencil_back.fail = ds->stencil.back.op.fail;
      info->vs.stencil_back.pass = ds->stencil.back.op.pass;
      info->vs.stencil_back.compare = ds->stencil.back.op.compare;
      info->vs.stencil_back.compare_mask = ds->stencil.back.compare_mask;
      info->vs.stencil_back.write_mask = ds->stencil.back.write_mask;

      info->vs.stencil_front.depth_fail = ds->stencil.front.op.depth_fail;
      info->vs.stencil_front.fail = ds->stencil.front.op.fail;
      info->vs.stencil_front.pass = ds->stencil.front.op.pass;
      info->vs.stencil_front.compare = ds->stencil.front.op.compare;
      info->vs.stencil_front.compare_mask = ds->stencil.front.compare_mask;
      info->vs.stencil_front.write_mask = ds->stencil.front.write_mask;
   }
}

static struct kk_depth_stencil_key
kk_ds_key_from_dynamic(const struct vk_depth_stencil_state *ds,
                       bool has_depth, bool has_stencil)
{
   struct kk_depth_stencil_key key = {
      .ops = has_depth && ds->depth.test_enable
                ? ds->depth.compare_op | (ds->depth.write_enable << 3)
                : VK_COMPARE_OP_ALWAYS,
   };
   if (has_stencil && ds->stencil.test_enable) {
      key.ops |= (1 << 4) |
                 (ds->stencil.front.op.fail << 5) |
                 (ds->stencil.front.op.depth_fail << 8) |
                 (ds->stencil.front.op.pass << 11) |
                 (ds->stencil.front.op.compare << 14) |
                 (ds->stencil.back.op.fail << 17) |
                 (ds->stencil.back.op.depth_fail << 20) |
                 (ds->stencil.back.op.pass << 23) |
                 (ds->stencil.back.op.compare << 26);
      key.front_read = ds->stencil.front.compare_mask;
      key.front_write = ds->stencil.front.write_mask;
      key.back_read = ds->stencil.back.compare_mask;
      key.back_write = ds->stencil.back.write_mask;
   }
   return key;
}

mtl_depth_stencil_state *
kk_compile_depth_stencil_state(struct kk_device *device,
                               const struct vk_depth_stencil_state *ds,
                              bool has_depth, bool has_stencil)
{
   struct kk_depth_stencil_key key =
      kk_ds_key_from_dynamic(ds, has_depth, has_stencil);
   return kk_compile_ds_key(device, &key);
}

mtl_depth_stencil_state *
kk_update_dynamic_ds_state(struct kk_device *device,
                           const struct vk_depth_stencil_state *ds,
                           bool has_depth, bool has_stencil,
                           struct kk_depth_stencil_key *previous_key,
                           mtl_depth_stencil_state *previous_state)
{
   struct kk_depth_stencil_key key =
      kk_ds_key_from_dynamic(ds, has_depth, has_stencil);
   /* A new encoder needs the same native bind, not another shared-cache
    * lookup and reference transfer for the state this command already owns. */
   if (previous_state && kk_ds_key_equal(previous_key, &key))
      return previous_state;

   mtl_depth_stencil_state *state = kk_compile_ds_key(device, &key);
   mtl_release(previous_state);
   *previous_key = key;
   return state;
}

static void
kk_msl_copy(struct msl_compile_data *dst, const struct msl_compile_data *src)
{
   /* Requirements belong to the source, including stage specialization.
    * Copy them together so new compiler outputs survive graphics linking. */
   *dst = *src;
   dst->code = ralloc_strdup(NULL, src->code);
   dst->entrypoint_name = ralloc_strdup(NULL, src->entrypoint_name);
}

/* Copies all msl shader data to the vertex and gathers information for pipeline
 * compilation. */
static void
gather_graphics_pipeline_create_info(
   const struct vk_graphics_pipeline_state *state, struct kk_shader **shaders,
   uint32_t shader_count)
{
   assert(state && shader_count >= 2 &&
          "state, vertex and fragment shaders are a must");
   struct kk_shader *vs = shaders[shaders[0]->info.stage == MESA_SHADER_TASK ? 1 : 0];
   struct kk_shader_info *info = &vs->info;

   /* Render pass data */
   const struct vk_render_pass_state *rp = state->rp;
   bool has_depth = rp->depth_attachment_format != VK_FORMAT_UNDEFINED;
   bool has_stencil = rp->stencil_attachment_format != VK_FORMAT_UNDEFINED;
   {
      info->vs.color_attachment_count = rp->color_attachment_count;
      for (uint8_t i = 0u; i < MAX_DRAW_BUFFERS; ++i) {
         info->vs.rt_formats[i] = MTL_PIXEL_FORMAT_INVALID;
      }
      for (uint8_t i = 0u; i < state->rp->color_attachment_count; ++i) {
         uint8_t logical_index = kk_get_logical_color_att_index(state, i);
         if (logical_index == MESA_VK_ATTACHMENT_UNUSED) {
            continue;
         }
         VkFormat format = state->rp->color_attachment_formats[i];
         info->vs.rt_formats[logical_index] =
            format == VK_FORMAT_UNDEFINED
               ? MTL_PIXEL_FORMAT_INVALID
               : vk_format_to_mtl_pixel_format(format);
      }
      info->vs.d_format =
         has_depth ? vk_format_to_mtl_pixel_format(rp->depth_attachment_format)
                   : MTL_PIXEL_FORMAT_INVALID;
      info->vs.s_format =
         has_stencil
            ? vk_format_to_mtl_pixel_format(rp->stencil_attachment_format)
            : MTL_PIXEL_FORMAT_INVALID;

      bool poly_multiview = state->mv->view_mask &&
         (state->shader_stages & (VK_SHADER_STAGE_GEOMETRY_BIT |
                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT));
      /* Poly already replays the stages for one view at a time. */
      info->vs.view_mask = poly_multiview ? 1u : state->mv->view_mask;
   }

   info->vs.has_ds = has_static_depth_stencil_state(state);
   if (info->vs.has_ds)
      kk_gather_ds_info(state->ds, has_depth, has_stencil, info);

   info->vs.dynamic_multisample =
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK) ||
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE);
   info->vs.dynamic_rasterization_samples =
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES);
   info->vs.has_ms = state->ms != NULL;
   info->vs.sample_count = 1u;
   if (info->vs.has_ms) {
      const struct vk_multisample_state *ms = state->ms;
      info->vs.sample_count = MAX2(ms->rasterization_samples, 1);
   }

   /* We need to store all other stage sources in the vertex too otherwise we
    * won't be able to create the whole pipeline correctly. */
   struct kk_shader *gs = NULL;
   for (uint32_t i = 0; i < shader_count; ++i) {
      struct kk_shader *s = shaders[i];
      if (s == vs) continue;
      mesa_shader_stage stage = s->info.stage;
      kk_msl_copy(&vs->msl_data[stage], &s->msl_data[stage]);
      if (stage == MESA_SHADER_FRAGMENT && s->interlock) {
         kk_interlock_shader_finish(vs->interlock);
         vs->interlock = kk_interlock_shader_ref(s->interlock);
         info->interlock_envelope = true;
      }

      if (stage == MESA_SHADER_FRAGMENT)
         info->framebuffer_read_mask = s->info.framebuffer_read_mask;
      info->vs.additional_stages_bits |= BITFIELD_BIT(stage);
      if (stage == MESA_SHADER_FRAGMENT && (info->polygon_point || info->poly_layer_replay || info->interlock_envelope))
         kk_msl_copy(&vs->polygon[3], &s->polygon[3]);
      if (stage == MESA_SHADER_FRAGMENT && info->conservative)
         kk_msl_copy(&vs->polygon[5], &s->polygon[5]);
      if (stage == MESA_SHADER_FRAGMENT)
         info->conservative_full_coverage |= s->info.conservative_full_coverage;

      if (s->info.stage == MESA_SHADER_TESS_CTRL)
         info->vs.tess_local_thread_size = s->info.tess.tcs_output_patch_size;

      if (s->info.stage == MESA_SHADER_GEOMETRY) {
         kk_msl_copy(&vs->gs_main, &s->gs_main);
         info->polygon_point = s->info.polygon_point;
         info->conservative = s->info.conservative;
         if (info->conservative) kk_msl_copy(&vs->polygon[4], &s->polygon[4]);
         info->polygon = s->info.polygon;
         if (info->polygon_point || info->poly_layer_replay || info->interlock_envelope) {
            for (unsigned j = 0; j < 3; ++j)
               if (s->polygon[j].code) kk_msl_copy(&vs->polygon[j], &s->polygon[j]);
         }
         info->has_geometry = true;
         info->geometry_statistics = s->info.geometry_statistics;
         info->geometry = s->info.gs;
         info->gs_aux_mask = s->info.gs_aux_mask;
         u_foreach_bit(i, info->gs_aux_mask)
            kk_msl_copy(&vs->gs_aux[i], &s->gs_aux[i]);
         gs = s;
      }
   }

   uint8_t topology;
   if (vs->info.stage == MESA_SHADER_MESH) {
      /* One executable owns every linked stage, including cache restores. */
      for (uint32_t i = 0; i < shader_count; ++i)
         info->uses_per_draw_data |= shaders[i]->info.uses_per_draw_data;
      topology = vs->mesh->info.primitive == MESA_PRIM_POINTS
         ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
         : vs->mesh->info.primitive == MESA_PRIM_LINES
            ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   } else {
      topology = state->ia->primitive_topology;
   }
   if (info->vs.additional_stages_bits & BITFIELD_BIT(MESA_SHADER_TESS_CTRL)) {
      /* TODO_KOSMICKRISP Hackity hack */
      struct kk_shader *tesc = shaders[1];
      struct kk_shader *tese = shaders[2];

      struct kk_tess_info info =
         kk_tess_info_merge(tese->info.tess.info, tesc->info.tess.info);

      /* Determine primitive based on the merged state */
      if (info.points) {
         topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      } else if (info.mode == TESS_PRIMITIVE_ISOLINES) {
         topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      } else {
         topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      }
   }

   if (gs) {
      /* Only the topology class matters here */
      enum mesa_prim prim = u_reduced_prim(gs->info.gs.mode);
      if (prim == MESA_PRIM_POINTS)
         topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      else if (prim == MESA_PRIM_LINES)
         topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      else
         topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   }

   info->vs.topology =
      vk_primitive_topology_to_mtl_primitive_topology_class(topology);
   info->vs.polygon_topology = info->polygon_point ?
      kk_polygon_raster_topology(info->polygon.primitive_vertices,
         state->rs->polygon_mode, state->rs->line.mode) : info->vs.topology;
}

static void
kk_configure_render_pipeline(struct kk_device *device, struct kk_shader *vs,
                             struct kk_pipeline_handles *pipe,
                             mtl_render_pipeline_descriptor *pipeline_descriptor,
                             unsigned polygon)
{
   /* If color attachments are reordered there might be unused / invalid gaps
    * in the array so need to check them all, not just first
    * color_attachment_count entries.
    */
   for (uint8_t i = 0; i < MAX_DRAW_BUFFERS; ++i) {
      if (vs->info.vs.rt_formats[i] != MTL_PIXEL_FORMAT_INVALID)
         mtl_render_pipeline_descriptor_set_color_attachment_format(
            pipeline_descriptor, i, vs->info.vs.rt_formats[i]);
   }

   if (vs->info.vs.has_ds && !polygon) {
      pipe->gfx.ds_handle = kk_compile_ds_state(device, &vs->info);
   }

   if (vs->info.vs.view_mask && vs->info.stage != MESA_SHADER_MESH) {
      uint32_t max_amplification = util_bitcount(vs->info.vs.view_mask);
      mtl_render_pipeline_descriptor_set_max_vertex_amplification_count(
         pipeline_descriptor, max_amplification);
   }

   if (vs->info.vs.has_ms) {
      mtl_render_pipeline_descriptor_set_raster_sample_count(
         pipeline_descriptor, vs->info.vs.sample_count);
   }

}

static VkResult
kk_record_render_descriptor(struct kk_device *device, struct kk_shader *shader,
                             mtl_render_pipeline_descriptor *descriptor,
                             unsigned kind, bool statistics, uint32_t sampler_stages,
                             uint32_t sampler_required)
{
   if (!sampler_stages && !statistics && !shader->info.geometry_statistics &&
       !kk_attachment_dynamic_variants(device, shader))
      return VK_SUCCESS;
   VkResult result = kk_attachment_pipelines_create(device, shader);
   if (result != VK_SUCCESS)
      return result;
   struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
   simple_mtx_lock(&cache->mutex);
   if (!cache->descriptor[kind]) {
      cache->descriptor[kind] = mtl_retain(descriptor);
      cache->statistics[kind] = statistics;
      cache->sampler_stages[kind] = sampler_stages;
      cache->sampler_required[kind] = sampler_required;
      if (sampler_stages) p_atomic_set(&cache->sampler_render, true);
      cache->topology[kind] = kind == KK_PIPELINE_CONSERVATIVE ?
         MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE : kind == KK_PIPELINE_POLYGON ?
         shader->info.vs.polygon_topology : shader->info.vs.topology;
   }
   simple_mtx_unlock(&cache->mutex);
   return VK_SUCCESS;
}

#include "kk_mesh_pipeline.h"

#include "kk_geometry_reports.h"

struct kk_lazy_polygon {
   bool gs_mesh;
   util_once_flag once;
   VkResult result;
   struct kk_device *device;
   struct kk_shader *shader;
};

static VkResult
kk_compile_graphics_prerender(struct kk_device *device, struct kk_shader *vs,
                             mesa_shader_stage *raster_stage)
{
   struct kk_pipeline_handles *pipe = &vs->pipeline;
   mesa_shader_stage vs_stage = MESA_SHADER_VERTEX;
   VkResult result;
   uint32_t stages_bits = vs->info.vs.additional_stages_bits;
   pipe->gfx.pre_render_count = util_bitcount(stages_bits) - 1u;
   for (uint32_t i = 0u; i < pipe->gfx.pre_render_count; ++i) {
      uint32_t local_thread_size = vs_stage == MESA_SHADER_TESS_CTRL
                                      ? vs->info.vs.tess_local_thread_size
                                      : KK_POLY_WG_SIZE;
      result = kk_compile_compute_pipeline(device, vs, &vs->msl_data[vs_stage],
                                           local_thread_size,
                                           &pipe->gfx.pre_render[i]);

      if (result != VK_SUCCESS)
         return VK_ERROR_INVALID_SHADER_NV;
      vs_stage = u_bit_scan(&stages_bits);
   }

   /* The GS compute pass is the last pre-render pipeline, its rasterization VS
    * pairs with the FS. */
   if (vs->info.vs.additional_stages_bits &
       BITFIELD_BIT(MESA_SHADER_GEOMETRY)) {
      result = kk_compile_compute_pipeline(
         device, vs, &vs->gs_main, KK_POLY_WG_SIZE,
         &pipe->gfx.pre_render[pipe->gfx.pre_render_count++]);
      if (result != VK_SUCCESS)
         return VK_ERROR_INVALID_SHADER_NV;
   }

   *raster_stage = vs_stage;
   return VK_SUCCESS;
}

static VkResult
kk_compile_graphics_pipeline_native_body(struct kk_device *device, struct kk_shader *vs, unsigned polygon)
{
   struct kk_physical_device *pdev = kk_device_physical(device);
   VkResult result = VK_SUCCESS;
   struct kk_pipeline_handles *pipe = &vs->pipeline;
   mesa_shader_stage vs_stage = MESA_SHADER_VERTEX;
   /* A complete native pair does not consume the optional compute siblings. */
   if (!polygon && !vs->lazy_polygon) {
      result = kk_compile_graphics_prerender(device, vs, &vs_stage);
      if (result != VK_SUCCESS)
         return result;
   }
   const struct msl_compile_data *vs_data = polygon ? &vs->polygon[2 * polygon] :
      vs->info.polygon_native ? &vs->polygon_native : &vs->msl_data[vs_stage];
   const struct msl_compile_data *fs_data = polygon ? &vs->polygon[2 * polygon + 1] :
      vs->info.polygon_native ? &vs->polygon_native_fs : &vs->msl_data[MESA_SHADER_FRAGMENT];
   const struct mtl_library_source sources[2] = {
      {vs_data->code, kk_msl_language_version(vs_data, pdev->info.msl_version)},
      {fs_data->code, kk_msl_language_version(fs_data, pdev->info.msl_version)},
   };
   mtl_library *libraries[2];
   if (!KK_PERF_CALL(device, KK_PERF_METAL_LIBRARY_PAIR,
                    mtl_new_library_pair(device->mtl_compiler_handle, sources,
                       MTL_MATH_MODE_FAST,
                       MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST, libraries)))
      return VK_ERROR_INVALID_SHADER_NV;
   mtl_library *vertex_library = libraries[0];
   mtl_library *fragment_library = libraries[1];
   mtl_function_descriptor *vertex_function = NULL;
   result = kk_msl_function_sampler_base(device, vs_data, vertex_library,
                            vs_data->entrypoint_name, &vertex_function);
   if (result != VK_SUCCESS) {
      mtl_release(fragment_library);
      mtl_release(vertex_library);
      return result;
   }

   mtl_function_descriptor *fragment_function = NULL;
   result = kk_msl_function_sampler_base(device, fs_data, fragment_library,
                            fs_data->entrypoint_name, &fragment_function);
   if (result != VK_SUCCESS) {
      mtl_release(vertex_function);
      mtl_release(fragment_library);
      mtl_release(vertex_library);
      return result;
   }

   mtl_render_pipeline_descriptor *pipeline_descriptor =
      mtl_new_render_pipeline_descriptor();
   mtl_render_pipeline_descriptor_set_vertex_shader(pipeline_descriptor,
                                                    vertex_function);
   mtl_render_pipeline_descriptor_set_fragment_shader(pipeline_descriptor,
                                                      fragment_function);

   /* Layered rendering in Metal requires setting primitive topology class */
   mtl_render_pipeline_descriptor_set_input_primitive_topology(
      pipeline_descriptor, polygon == 2 ? MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE :
         polygon ? vs->info.vs.polygon_topology : vs->info.vs.topology);

   kk_configure_render_pipeline(device, vs, pipe, pipeline_descriptor, polygon);

   uint32_t sampler_stages =
      (vs_data->uses_sampler_emulation ? MTL_SAMPLER_EMULATION_VERTEX : 0) |
      (fs_data->uses_sampler_emulation ? MTL_SAMPLER_EMULATION_FRAGMENT : 0);
   uint32_t sampler_required =
      (vs_data->requires_sampler_emulation ? MTL_SAMPLER_EMULATION_VERTEX : 0) |
      (fs_data->requires_sampler_emulation ? MTL_SAMPLER_EMULATION_FRAGMENT : 0);
   mtl_render_pipeline_state *render = kk_attachment_compile_pipeline(device, vs,
      pipeline_descriptor, sampler_stages, sampler_required);
   if (polygon == 2) pipe->gfx.conservative_render = render;
   else if (polygon) pipe->gfx.polygon_render = render;
   else pipe->gfx.render = render;
   if (render == NULL)
      result = VK_ERROR_INVALID_SHADER_NV;

   if (render)
      result = kk_record_render_descriptor(device, vs, pipeline_descriptor,
                                          polygon, fs_data->uses_pipeline_statistics,
                                          sampler_stages, sampler_required);
   mtl_release(pipeline_descriptor);
   mtl_release(fragment_function);
   mtl_release(fragment_library);
   mtl_release(vertex_function);
   mtl_release(vertex_library);

   return result;
}

static VkResult
kk_compile_graphics_pipeline_native(struct kk_device *device, struct kk_shader *vs, unsigned polygon)
{
   void *foreground = mtl_compiler_foreground_begin(device->mtl_compiler_handle);
   VkResult result = kk_compile_graphics_pipeline_native_body(device, vs, polygon);
   mtl_compiler_foreground_end(foreground);
   return result;
}

static VkResult
kk_compile_polygon_siblings(struct kk_device *device, struct kk_shader *vs)
{
   VkResult result = VK_SUCCESS;
   if (vs->lazy_polygon) {
      mesa_shader_stage unused;
      result = kk_compile_graphics_prerender(device, vs, &unused);
      if (result != VK_SUCCESS)
         return result;
   }
   if (vs->info.polygon_point || vs->info.interlock_envelope) {
   result = kk_compile_compute_pipeline(device, vs, &vs->polygon[0], KK_POLY_WG_SIZE,
                                        &vs->pipeline.gfx.polygon_capture);
   if (result != VK_SUCCESS) return result;
   if (vs->polygon[1].code) {
      result = kk_compile_compute_pipeline(device, vs, &vs->polygon[1], KK_POLY_WG_SIZE,
                                           &vs->pipeline.gfx.polygon_xfb);
      if (result != VK_SUCCESS) return result;
   }
   }
   if (vs->polygon[2].code)
      result = kk_compile_graphics_pipeline_native(device, vs, true);
   if (result == VK_SUCCESS && vs->info.conservative)
      result = kk_compile_graphics_pipeline_native(device, vs, 2);
   if (result == VK_SUCCESS && vs->interlock)
      result = kk_interlock_pipeline_compile(device, vs);
   return result;
}

static void
kk_compile_polygon_once(const void *data)
{
   struct kk_lazy_polygon *lazy = (struct kk_lazy_polygon *)data;
   if (lazy->gs_mesh)
      lazy->result = kk_compile_mesh_pipeline(lazy->device, lazy->shader, false,
                                            &lazy->shader->pipeline);
   else
      lazy->result = kk_compile_polygon_siblings(lazy->device, lazy->shader);
}

VkResult
kk_shader_ensure_polygon(struct kk_device *dev, struct kk_shader *shader)
{
   struct kk_lazy_polygon *lazy = shader->lazy_polygon;
   if (!lazy)
      return VK_SUCCESS;
   assert(lazy->device == dev);
   util_call_once_data(&lazy->once, kk_compile_polygon_once, lazy);
   return lazy->result;
}

static VkResult
kk_compile_graphics_pipeline(struct kk_device *device, struct kk_shader *vs)
{
   kk_shader_sampler_info(vs);
   u_foreach_bit(i, vs->info.gs_aux_mask)
      vs->pipeline.gfx.gs_aux_once[i] = (util_once_flag)UTIL_ONCE_FLAG_INIT;
   if (vs->info.conservative) {
      VkResult result = kk_device_init_conservative_lib(device);
      if (result != VK_SUCCESS)
         return result;
   }
   if (vs->info.stage == MESA_SHADER_MESH) {
      if (!vs->info.api_geometry)
         return kk_compile_mesh_pipeline(device, vs, false, &vs->pipeline);
      vs->lazy_polygon = calloc(1, sizeof(*vs->lazy_polygon));
      if (!vs->lazy_polygon)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      vs->lazy_polygon->once = (util_once_flag)UTIL_ONCE_FLAG_INIT;
      vs->lazy_polygon->device = device;
      vs->lazy_polygon->shader = vs;
      vs->lazy_polygon->gs_mesh = true;
      return VK_SUCCESS;
   }
   const bool siblings = vs->info.polygon_point || vs->info.poly_layer_replay ||
                         vs->info.interlock_envelope;
   /* Other routes consume pre-render programs as part of their ordinary
    * pipeline. Keep their existing readiness and indirect-link contracts. */
   if (siblings && vs->info.polygon_native && !vs->interlock &&
       !vs->info.interlock_envelope && !vs->info.indirect_bindable &&
       !device->vk.enabled_features.deviceGeneratedCommands) {
      vs->lazy_polygon = calloc(1, sizeof(*vs->lazy_polygon));
      if (!vs->lazy_polygon)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      vs->lazy_polygon->once = (util_once_flag)UTIL_ONCE_FLAG_INIT;
      vs->lazy_polygon->device = device;
      vs->lazy_polygon->shader = vs;
   }
   VkResult result = kk_compile_graphics_pipeline_native(device, vs, false);
   if (result != VK_SUCCESS)
      return result;
   if (siblings && !vs->lazy_polygon) {
      result = kk_compile_polygon_siblings(device, vs);
      if (result != VK_SUCCESS)
         return result;
   }
   return kk_dgc_eager_graphics_pipeline(device, vs);
}

/* Build only from the already linked original stages. This alternative owns
 * its native handles/source; recording may still select the compute parent. */
static VkResult
kk_compile_gs_mesh(struct kk_device *dev, nir_shader *vs, nir_shader *gs,
                   nir_shader *fs,
                   const struct vk_graphics_pipeline_state *state,
                   const struct vk_pipeline_robustness_state *robustness,
                   const struct kk_per_vertex_map *map,
                   const VkAllocationCallbacks *alloc, struct kk_shader **out)
{
   *out = NULL;
   struct kk_gs_mesh_layout layout;
   enum mesa_prim input = vk_topology_to_mesa(state->ia->primitive_topology);
   if (mesa_prim_has_adjacency(input) || input == MESA_PRIM_PATCHES ||
       input == MESA_PRIM_LINE_LOOP ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY) ||
       !kk_gs_mesh_layout(vs, gs, fs,
                          kk_device_physical(dev)->info.gpu_apple_family, map,
                          &layout))
      return VK_SUCCESS;
   nir_shader *n[3] = {nir_shader_clone(NULL, vs), nir_shader_clone(NULL, gs),
                       nir_shader_clone(NULL, fs)};
   kk_lower_vs_vbo(n[0], state, robustness);
   kk_gs_mesh_lower_vs(n[0], &layout, input);
   kk_gs_mesh_lower_gs(n[1], &layout, state);
   for (unsigned i = 0; i < 2; i++) {
      NIR_PASS(_, n[i], nir_opt_dce);
      NIR_PASS(_, n[i], kk_nir_lower_descriptors, false, robustness, 0, NULL);
   }
   struct kk_shader *s[3] = {NULL, NULL, NULL};
   const struct kk_compile_options options = {0};
   VkResult result = VK_SUCCESS;
   for (unsigned i = 0; i < 3; i++) {
      result = kk_compile_shader(dev, n[i], i ? s[i - 1] : NULL, robustness,
                                 state, &options, alloc, &s[i]);
      n[i] = NULL;
      if (result != VK_SUCCESS)
         goto done;
   }
   gather_graphics_pipeline_create_info(state, s, 3);
   s[1]->info.api_geometry = true;
   s[1]->mesh->info.gs_max_objects = layout.max_objects;
   result = kk_compile_graphics_pipeline(dev, s[1]);
   if (result == VK_SUCCESS) {
      *out = s[1];
      s[1] = NULL;
   }
done:
   for (unsigned i = 0; i < 3; i++) {
      ralloc_free(n[i]);
      if (s[i])
         kk_shader_destroy(&dev->vk, s[i], alloc);
   }
   return result;
}

static bool
move_primitive_id(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const unsigned *slot = data;

   /* Metal requires the vertex output and fragment input types to match */
   if (intr->intrinsic == nir_intrinsic_load_primitive_id &&
       b->shader->info.stage == MESA_SHADER_FRAGMENT) {
      b->cursor = nir_before_instr(&intr->instr);
      nir_def *id = nir_load_input(b, 1, 32, nir_imm_int(b, 0),
                                   .dest_type = nir_type_uint32,
                                   .io_semantics.location = *slot);
      nir_def_replace(&intr->def, id);
      return true;
   }

   if (intr->intrinsic != nir_intrinsic_store_output &&
       intr->intrinsic != nir_intrinsic_store_per_primitive_output)
      return false;

   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   if (sem.location != VARYING_SLOT_PRIMITIVE_ID)
      return false;

   sem.location = *slot;
   nir_intrinsic_set_io_semantics(intr, sem);
   nir_intrinsic_set_src_type(intr, nir_type_uint32);
   return true;
}

/* Metal's primitive_id attribute numbers the primitives the GS rasterization
 * pass draws, not the value the GS writes. Pass that value as a flat varying
 * in a slot neither shader uses instead. */
static void
kk_forward_gs_primitive_id(nir_shader *gs, nir_shader *fs)
{
   assert(gs->info.io_lowered && fs->info.io_lowered);

   uint64_t used = gs->info.outputs_written | fs->info.inputs_read;
   unsigned slot = VARYING_SLOT_VAR31;
   while (slot >= VARYING_SLOT_VAR0 && (used & BITFIELD64_BIT(slot)))
      --slot;

   if (!(gs->info.outputs_written & VARYING_BIT_PRIMITIVE_ID) ||
       slot < VARYING_SLOT_VAR0)
      return;

   /* Makes progress only if the FS reads the primitive ID */
   if (!nir_shader_intrinsics_pass(fs, move_primitive_id,
                                   nir_metadata_control_flow, &slot))
      return;

   nir_shader_intrinsics_pass(gs, move_primitive_id, nir_metadata_all, &slot);

   BITSET_CLEAR(fs->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID);
   fs->info.inputs_read |= BITFIELD64_BIT(slot);
   gs->info.outputs_written &= ~VARYING_BIT_PRIMITIVE_ID;
   gs->info.outputs_written |= BITFIELD64_BIT(slot);
   if (gs->info.stage == MESA_SHADER_MESH) {
      gs->info.per_primitive_outputs &= ~VARYING_BIT_PRIMITIVE_ID;
      gs->info.per_primitive_outputs |= BITFIELD64_BIT(slot);
      fs->info.per_primitive_inputs |= BITFIELD64_BIT(slot);
   }
}

#include "kk_sparse_pipeline.h"
#include "kk_linked_producer.h"

static void
kk_finish_fragment_nir_untracked(struct kk_device *dev, nir_shader *fs,
                       const struct vk_graphics_pipeline_state *state,
                       const struct vk_shader_compile_info *info,
                       kk_feature_key features, nir_shader **retained)
{
   bool callable = retained && dev->vrs &&
      !fs->info.fs.uses_sample_shading &&
      !(state && state->ms && state->ms->sample_shading_enable) &&
      kk_vrs_pipeline_can_coarsen(state,
         dev->vk.enabled_features.primitiveFragmentShadingRate,
         dev->vk.enabled_features.attachmentFragmentShadingRate) &&
      kk_nir_vrs_split_application(fs);
   kk_lower_fs(dev, fs, state);
   kk_finish_nir(dev, fs, info ? info->robustness : &rs_none,
      info ? info->set_layout_count : 0, info ? info->set_layouts : NULL, features,
      info ? info->flags : 0);
   /* The shared embedded schema carries original-stage root loads; its
    * canonical sampler lifetime remains with the owning fine shader. */
   static_assert(KK_EMBEDDED_SAMPLER_BINDING_SCHEMA == 1u,
                 "retained embedded source uses portable runtime root bindings");
   if (retained && dev->vrs && callable) {
      *retained = nir_shader_clone(NULL, fs);
      /* callable excludes actual application sample-frequency programs. */
      kk_nir_vrs_mark_frequency(*retained, false);
   }
   if (callable)
      kk_nir_vrs_flatten(fs);
   if (dev->vrs)
      kk_nir_vrs_lower_fine_rate(fs);
   nir_opts(fs, NULL);
   nir_shader_gather_info(fs, nir_shader_get_entrypoint(fs));
}

static void
kk_finish_fragment_nir(struct kk_device *dev, nir_shader *fs,
                       const struct vk_graphics_pipeline_state *state,
                       const struct vk_shader_compile_info *info,
                       kk_feature_key features, nir_shader **retained)
{
   KK_PERF_VOID(dev, KK_PERF_NIR,
      kk_finish_fragment_nir_untracked(dev, fs, state, info, features, retained));
}

static VkResult
kk_compile_shaders_native(struct vk_device *device, uint32_t shader_count,
                   struct vk_shader_compile_info *infos,
                   const struct vk_graphics_pipeline_state *state,
                   const struct vk_features *enabled_features,
                   const VkAllocationCallbacks *pAllocator,
                   struct vk_shader **shaders_out, uint32_t sparse_modes)
{
   VkResult result = VK_SUCCESS;
   struct kk_device *dev = container_of(device, struct kk_device, vk);
   struct kk_physical_device *pdev = kk_device_physical(dev);

   kk_feature_key features = kk_make_feature_key(enabled_features);
   if (sparse_modes & KK_SPARSE_BUFFER)
      features |= BITFIELD_BIT(KK_FEAT_SPARSE_BUFFER);
   if (sparse_modes & KK_SPARSE_STANDARD_3D)
      features |= BITFIELD_BIT(KK_FEAT_STANDARD_3D);

   /* Vulkan doesn't enforce a fragment shader to build pipelines. We may need
    * to create one. */
   const bool dgc_graphics = (features & BITFIELD_BIT(KK_FEAT_DGC_REPLAY)) &&
      (infos[0].stage <= MESA_SHADER_FRAGMENT ||
       infos[0].stage == MESA_SHADER_TASK || infos[0].stage == MESA_SHADER_MESH);
   bool dgc_replay = false;
   bool dgc_interlock_guard = false;
   uint32_t push_size = 0;
   uint32_t heap_push_size = 0;
   nir_shader *nir_shaders[shader_count + 2u];
   nir_shader *retained_fs = NULL;
   nir_shader *fs_to_finish = NULL;
   struct kk_shader *shaders[shader_count + 2u];

   bool poly_layer_replay = false;
   mesa_shader_stage layer_producer = MESA_SHADER_NONE;
   mesa_shader_stage cull_producer = MESA_SHADER_NONE;
   bool cull_distance = false;
   for (uint32_t i = 0; i < shader_count; ++i) {
      mesa_shader_stage stage = infos[i].stage;
      if ((stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_TESS_EVAL ||
           stage == MESA_SHADER_GEOMETRY || stage == MESA_SHADER_MESH) &&
          (cull_producer == MESA_SHADER_NONE || stage > cull_producer)) {
         cull_producer = stage;
         cull_distance = stage != MESA_SHADER_MESH &&
            infos[i].nir->info.cull_distance_array_size != 0;
      }
      if ((stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_TESS_EVAL ||
           stage == MESA_SHADER_GEOMETRY) &&
          (layer_producer == MESA_SHADER_NONE || stage > layer_producer)) {
         layer_producer = stage;
         poly_layer_replay = infos[i].nir->info.outputs_written & VARYING_BIT_LAYER;
      }
   }
   /* Multiview supplies the last producer's effective Layer even when
    * the application has no Layer output. Mesh owns its canonical reader. */
   poly_layer_replay |= layer_producer != MESA_SHADER_NONE && state &&
      state->mv && state->mv->view_mask;
   poly_layer_replay = poly_layer_replay &&
      kk_poly_layer_alias_possible(state, enabled_features);
   /* VRS retains linked source below. Device enablement does not select
    * Layer replay or eagerly compile a captured application sibling. */
   /* Device enablement alone must not create a captured pipeline. The final
    * producer owns primitive rejection; earlier-stage distances are inputs
    * for that producer. Native mesh output performs its own cull test. */
   const uint8_t depth_bias_control = kk_depth_bias_control_flags(
      enabled_features->depthBiasControl, state);
   const bool polygon_regular = cull_distance ||
      kk_polygon_regular_possible(state, depth_bias_control);
   const bool polygon_capture = polygon_regular || kk_conservative_possible(state);
   unsigned barycentric_inputs = 0;
   if (enabled_features->fragmentShaderBarycentric && shader_count &&
       infos[shader_count - 1u].stage == MESA_SHADER_FRAGMENT)
      barycentric_inputs = kk_barycentric_input_flags(infos[shader_count - 1u].nir);

   /* Lower shaders, notably lowering IO. This is a prerequisite for intershader
    * optimization. */
   const struct vk_pipeline_robustness_state *vertex_robustness = &rs_none;
   bool pixel_envelope = false;
   bool pixel_sample_shading = false;
   const bool pixel_candidate = shader_count &&
      infos[shader_count - 1u].stage == MESA_SHADER_FRAGMENT &&
      enabled_features->fragmentShaderPixelInterlock &&
      kk_interlock_multisample_pipeline(state) &&
      kk_interlock_pixel_shader(infos[shader_count - 1u].nir);
   nir_shader *deferred_native = NULL;
   const struct vk_shader_compile_info *deferred_info = NULL;
   const bool retain_native = dev->vrs && !pixel_candidate && !polygon_capture &&
      !poly_layer_replay &&
      kk_vrs_pipeline_can_coarsen(state, enabled_features->primitiveFragmentShadingRate,
                                 enabled_features->attachmentFragmentShadingRate);
   for (uint32_t step = 0u; step < shader_count; ++step) {
      uint32_t i = pixel_candidate ? (step ? step - 1u : shader_count - 1u) : step;
      const struct vk_shader_compile_info *info = &infos[i];
      nir_shader *nir = info->nir;
      /* Collect the ordinary layout extent in the existing stage walk. */
      for (uint32_t r = 0; r < info->push_constant_range_count; ++r) {
         const VkPushConstantRange *range = &info->push_constant_ranges[r];
         if (range->stageFlags & mesa_to_vk_shader_stage(info->stage))
            push_size = MAX2(push_size, range->offset + range->size);
      }
      if (mesa_shader_stage_is_rt(nir->info.stage) && !kk_rt_prepare_context(nir)) {
         for (uint32_t j=0;j<shader_count;++j) ralloc_free(infos[j].nir);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      if (info->flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT) {
         nir_foreach_variable_with_modes(var, nir, nir_var_mem_push_const)
            heap_push_size = MAX2(heap_push_size, MIN2(glsl_get_explicit_size(var->type, false), KK_MAX_PUSH_SIZE));
         nir_foreach_function_impl(impl, nir) {
            nir_foreach_block(block, impl) {
               nir_foreach_instr(instr, block) {
                  if (instr->type != nir_instr_type_intrinsic) continue;
                  nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
                  if (load->intrinsic != nir_intrinsic_load_push_constant) continue;
                  uint32_t end = nir_intrinsic_base(load) + nir_intrinsic_range(load);
                  if (nir_src_is_const(load->src[0]))
                     end = MAX2(end, nir_intrinsic_base(load) + nir_src_as_uint(load->src[0]) +
                                load->def.num_components * load->def.bit_size / 8);
                  heap_push_size = MAX2(heap_push_size, MIN2(end, KK_MAX_PUSH_SIZE));
               }
            }
         }
      }
      if (dgc_graphics && !dgc_replay)
         dgc_replay = kk_dgc_shader_needs_replay(nir);
      if (dgc_graphics && nir->info.stage == MESA_SHADER_FRAGMENT)
         dgc_interlock_guard = nir->info.fs.pixel_interlock_ordered ||
            nir->info.fs.pixel_interlock_unordered ||
            nir->info.fs.sample_interlock_ordered ||
            nir->info.fs.sample_interlock_unordered;

      /* Stages before the tessellator or a GS run as compute */
      bool emulated_stage =
         info->next_stage_mask & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                  VK_SHADER_STAGE_GEOMETRY_BIT);

      emulated_stage |= pixel_envelope &&
         (nir->info.stage == MESA_SHADER_VERTEX || nir->info.stage == MESA_SHADER_TESS_EVAL);
      /* XFB without an explicit GS gets a Poly passthrough after linking.
       * Keep its producer in compute form before hardware-VS lowering. */
      emulated_stage |= (nir->xfb_info || polygon_capture || poly_layer_replay) && (nir->info.stage == MESA_SHADER_VERTEX ||
                                         nir->info.stage == MESA_SHADER_TESS_EVAL);
      if (pixel_candidate && nir->info.stage == MESA_SHADER_FRAGMENT) {
         pixel_envelope = true;
         pixel_sample_shading = kk_interlock_sample_frequency(nir) ||
            (state->ms && state->ms->sample_shading_enable);
      }
      msl_preprocess_nir_workarounds(nir, pdev->settings.disabled_workarounds);
      if (nir->info.stage == MESA_SHADER_FRAGMENT && !nir->info.io_lowered)
         fs_to_finish = nir;
      const bool defer_hw_vs = retain_native && !emulated_stage &&
         info->stage == layer_producer &&
         (info->stage == MESA_SHADER_VERTEX || info->stage == MESA_SHADER_TESS_EVAL);
      if (defer_hw_vs) { deferred_native = nir; deferred_info = info; }
      kk_lower_nir(dev, nir, emulated_stage, defer_hw_vs, info->robustness,
                   info->set_layout_count, info->set_layouts, state, features, info, NULL);
      if (nir->info.stage == MESA_SHADER_FRAGMENT &&
          (features & BITFIELD_BIT(KK_FEAT_PIPELINE_STATISTICS)))
         NIR_PASS(_, nir, kk_nir_statistics_fs);

      if (nir->info.stage == MESA_SHADER_VERTEX)
         vertex_robustness = info->robustness;

      nir_shaders[i] = nir;
   }

   if (shader_count && mesa_shader_stage_is_rt(nir_shaders[0]->info.stage))
      return kk_rt_compile_lowered(device, shader_count, nir_shaders, infos, heap_push_size, pAllocator,
                                    shaders_out, kk_compile_msl);

   /* Since we don't support GPL nor shader objects and Metal render pipelines
    * require both vertex and fragment, we may need to provide a pass-through
    * fragment. */
   bool explicit_geometry = false, xfb = false, tess = false;
   for (unsigned i = 0; i < shader_count; ++i) {
      explicit_geometry |= infos[i].stage == MESA_SHADER_GEOMETRY;
      tess |= infos[i].stage == MESA_SHADER_TESS_EVAL;
      xfb |= nir_shaders[i]->xfb_info != NULL;
   }
   uint32_t total_shaders = shader_count;
   nir_shader *native_pair[2] = {NULL, NULL};
   bool native_fragment_needs_finish = false;
   bool native_passthrough = !explicit_geometry && !xfb && !tess &&
      (kk_polygon_native_possible(state) || poly_layer_replay ||
       (!pixel_envelope && kk_depth_bias_native_possible(depth_bias_control, state)));
   nir_shader *native_vs = NULL, *native_fs = NULL;
   for (unsigned i = 0; i < shader_count; ++i) {
      nir_shader *n = nir_shaders[i];
      if (n->info.stage == MESA_SHADER_VERTEX) native_vs = n;
      if (n->info.stage == MESA_SHADER_FRAGMENT) {
         native_fs = n;
         if (!poly_layer_replay && BITSET_TEST(n->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID))
            native_passthrough = false;
      }
   }
   if (native_passthrough && native_vs && !native_vs->info.cull_distance_array_size) {
      native_pair[0] = nir_shader_clone(NULL, native_vs);
      native_pair[1] = native_fs ? nir_shader_clone(NULL, native_fs) :
         get_empty_nir(dev, MESA_SHADER_FRAGMENT, state, features);
      native_fragment_needs_finish = !native_fs || native_fs == fs_to_finish;
      native_pair[0]->info.next_stage = MESA_SHADER_FRAGMENT;
      kk_lower_hw_vs(native_pair[0], state, false);
      msl_nir_lower_clip_cull_distance(native_pair[0], 0);
      /* VBO lowering waits until linking establishes the owning root mask. */
      /* The first descriptor pass rewrites uses but keeps dead resource
       * descriptors until DCE. Remove those before lowering only new sysvals. */
      NIR_PASS(_, native_pair[0], nir_opt_dce);
      NIR_PASS(_, native_pair[0], kk_nir_lower_descriptors, false, vertex_robustness, 0, NULL);
      nir_opt_varyings_bulk(native_pair, 2, true, UINT32_MAX, UINT32_MAX, nir_opts, NULL);
      nir_opt_varyings_bulk(native_pair, 2, true, UINT32_MAX, UINT32_MAX, nir_opts, NULL);
      /* This clone predates deferred fragment finishing. Use its original
       * descriptor layouts, and leave already-finished incoming IR alone. */
      if (native_fragment_needs_finish) {
         const struct vk_shader_compile_info *info = NULL;
         for (unsigned i = 0; i < shader_count; ++i)
            if (infos[i].stage == MESA_SHADER_FRAGMENT) info = &infos[i];
         kk_finish_fragment_nir(dev, native_pair[1], state, info, features, NULL);
      }
   }
   if (!explicit_geometry && (xfb || pixel_envelope || polygon_capture || poly_layer_replay)) {
      nir_shader *producer = NULL;
      unsigned insert = shader_count;
      for (unsigned i = 0; i < shader_count; ++i) {
         if (nir_shaders[i]->info.stage != MESA_SHADER_FRAGMENT) producer = nir_shaders[i];
         if (nir_shaders[i]->info.stage == MESA_SHADER_FRAGMENT) insert = i;
      }
      assert(producer && state);
      /* The native alternative was cloned above with ordinary amplification.
       * Only its captured sibling consumes the existing per-view uniform. */
      if (state->mv->view_mask && producer->info.stage == MESA_SHADER_VERTEX)
         kk_poly_layer_capture_view(producer, state->mv->view_mask);
      producer->info.next_stage = MESA_SHADER_GEOMETRY;
      size_t key_size = poly_passthrough_gs_key_size(producer->xfb_info ? producer->xfb_info->output_count : 0);
      struct poly_passthrough_gs_key *key = rzalloc_size(NULL, key_size);
      key->prim = u_reduced_prim(vk_topology_to_mesa(state->ia->primitive_topology));
      if (tess) {
         key->prim = producer->info.tess.point_mode ? MESA_PRIM_POINTS :
            (producer->info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES ? MESA_PRIM_LINES : MESA_PRIM_TRIANGLES);
      }
      key->outputs = producer->info.outputs_written;
      key->clip_distance_array_size = producer->info.clip_distance_array_size;
      key->cull_distance_array_size = producer->info.cull_distance_array_size;
      memcpy(key->xfb_stride, producer->info.xfb_stride, sizeof(key->xfb_stride));
      if (producer->xfb_info)
         memcpy(&key->xfb_info, producer->xfb_info,
                nir_xfb_info_size(producer->xfb_info->output_count));
      nir_foreach_function_impl(impl, producer) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic) continue;
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               if (intr->intrinsic != nir_intrinsic_store_output) continue;
               unsigned slot = nir_intrinsic_io_semantics(intr).location;
               key->output_components[slot] = MAX2(key->output_components[slot],
                  nir_intrinsic_component(intr) + intr->num_components);
               key->output_types[slot] = nir_intrinsic_src_type(intr);
            }
         }
      }
      nir_builder builder = nir_builder_init_simple_shader(MESA_SHADER_GEOMETRY,
         producer->options, "Poly transform feedback passthrough");
      poly_nir_passthrough_gs(&builder, key);
      if (pixel_envelope || polygon_capture || poly_layer_replay)
         nir_shader_intrinsics_pass(builder.shader, kk_polygon_passthrough_primitive_id,
                                    nir_metadata_control_flow, NULL);
      ralloc_free(key);
      builder.shader->info.io_lowered = true;
      msl_preprocess_nir(builder.shader);
      for (unsigned i = total_shaders; i > insert; --i)
         nir_shaders[i] = nir_shaders[i - 1];
      nir_shaders[insert] = builder.shader;
      ++total_shaders;
   }
   if (state && nir_shaders[total_shaders - 1]->info.stage != MESA_SHADER_FRAGMENT) {
      nir_shaders[total_shaders] = get_empty_nir(dev, MESA_SHADER_FRAGMENT, state, features);
      fs_to_finish = nir_shaders[total_shaders];
      ++total_shaders;
   }

   /* nir_opt_varyings_bulk removes a GS primitive ID output that the FS does
    * not read as an input. The FS is last and a GS right before it. */
   if (total_shaders >= 2u &&
       (nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_GEOMETRY ||
        nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_MESH)) {
      kk_forward_gs_primitive_id(nir_shaders[total_shaders - 2u],
                                 nir_shaders[total_shaders - 1u]);
   }

   /* Preserve upstream XFB side effects through the original varying linker,
    * before Poly lowers those IO stores to Metal device writes. */
   for (unsigned i = 0; i < total_shaders; ++i) {
      if (nir_shaders[i]->xfb_info)
         NIR_PASS(_, nir_shaders[i], nir_io_add_intrinsic_xfb_info);
   }
   nir_opt_varyings_bulk(nir_shaders, total_shaders, true, UINT32_MAX,
                         UINT32_MAX, nir_opts, NULL);
   /* Second pass is required because some dEQP-VK.glsl.matrix.sub.dynamic.*
    * would fail otherwise due to vertex outputting vec4 while fragments reading
    * vec3 when in reality only vec3 is needed. */
   nir_opt_varyings_bulk(nir_shaders, total_shaders, true, UINT32_MAX,
                         UINT32_MAX, nir_opts, NULL);

   /* Link original inputs before the fine epilogue; its complete native
    * inputs must exist before the one mesh/per-vertex packing decision. */
   for (unsigned i = 0; i < total_shaders; i++) {
      nir_shader *fs = nir_shaders[i];
      if (fs->info.stage != MESA_SHADER_FRAGMENT || fs != fs_to_finish) continue;
      const struct vk_shader_compile_info *info = NULL;
      for (unsigned j = 0; j < shader_count; j++)
         if (infos[j].stage == MESA_SHADER_FRAGMENT) info = &infos[j];
      /* Main and native alternatives share the same fine epilogue and
       * descriptor finishing; only the main program retains VRS source. */
      kk_finish_fragment_nir(dev, fs, state, info, features, &retained_fs);
   }

   nir_shader *conservative_fs = kk_conservative_possible(state) &&
      nir_shaders[total_shaders - 1u]->info.stage == MESA_SHADER_FRAGMENT
      ? nir_shader_clone(NULL, nir_shaders[total_shaders - 1u]) : NULL;
   /* The normal fragment finish gathered current system-value metadata.
    * Classify only now: early preprocessing metadata predates sysval lowering. */
   struct msl_per_vertex_inputs native_inputs;
   bool native_per_vertex =
      (barycentric_inputs & KK_BARYCENTRIC_INPUT_PER_VERTEX) &&
      pdev->info.gpu_apple_family >= 10 && !pixel_envelope && !polygon_capture &&
      !poly_layer_replay && !retain_native && !native_pair[0] &&
      kk_barycentric_native_per_vertex(total_shaders, nir_shaders, state,
                                       &native_inputs);
   bool force_per_vertex = kk_barycentric_force_per_vertex(
      shader_count, infos, state, enabled_features,
      native_per_vertex ? barycentric_inputs & ~KK_BARYCENTRIC_INPUT_PER_VERTEX
                        : barycentric_inputs);
   struct kk_per_vertex_map per_vertex_map = {0};
   if (total_shaders >= 2u &&
       nir_shaders[total_shaders - 1u]->info.stage == MESA_SHADER_FRAGMENT &&
       nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_MESH)
      NIR_PASS(_, nir_shaders[total_shaders - 1u], kk_nir_lower_mesh_flat_inputs,
               nir_shaders[total_shaders - 2u], &per_vertex_map);
   if (state && total_shaders >= 2u &&
       nir_shaders[total_shaders - 1u]->info.stage == MESA_SHADER_FRAGMENT &&
       (nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_VERTEX ||
        nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_TESS_EVAL ||
        nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_GEOMETRY))
      NIR_PASS(
         _, nir_shaders[total_shaders - 1u], kk_nir_lower_fs_per_vertex_inputs,
         nir_shaders[total_shaders - 2u], force_per_vertex,
         &per_vertex_map);

   if (per_vertex_map.out_of_slots) {
      for (uint32_t i = 0; i < total_shaders; i++) {
         ralloc_free(nir_shaders[i]);
      }
      ralloc_free(retained_fs);
      memset(shaders_out, 0, shader_count * sizeof(*shaders_out));
      return vk_errorf(dev, VK_ERROR_UNKNOWN,
                       "Out of varying slots for barycentric emulation");
   }

   if (retained_fs && per_vertex_map.active) {
      struct kk_per_vertex_map retained_map = per_vertex_map;
      kk_nir_apply_fs_per_vertex_map(retained_fs, &retained_map);
   }
   struct blob linked_producer;
   const struct blob *linked = NULL;
   if (deferred_native) {
      /* This is the original, twice-linked producer with the canonical map,
       * not a pre-link reconstruction or a post-MSL copy. Descriptor and
       * robustness lowering already belongs to this exact stage. */
      if (retained_fs) {
         blob_init(&linked_producer);
         kk_linked_producer_encode(&linked_producer, deferred_native, state,
            KK_DEBUG(FORCE_ROBUSTNESS) ? &rs_all_supported : vertex_robustness, features,
            features & BITFIELD_BIT(KK_FEAT_PIPELINE_STATISTICS),
            features & BITFIELD_BIT(KK_FEAT_GEOMETRY_REPORTS));
         linked = &linked_producer;
      }
      bool point = deferred_native->info.stage == MESA_SHADER_TESS_EVAL
         ? deferred_native->info.tess.point_mode
         : state->ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      kk_lower_hw_vs_position(deferred_native, state, point);
      /* Only the native tail's new sysvals remain. Dead resource descriptors
       * must be removed before the same root-only pass used by native_pair. */
      NIR_PASS(_, deferred_native, nir_opt_dce);
      NIR_PASS(_, deferred_native, kk_nir_lower_descriptors, false,
               deferred_info->robustness, 0, NULL);
   }
   const struct kk_compile_options compile_options = {
      .linked_producer = linked,
      .pixel_envelope = pixel_envelope,
      .pixel_sample_shading = pixel_sample_shading,
      .poly_layer_replay = poly_layer_replay,
      .polygon_capture = polygon_capture,
      .polygon_regular = polygon_regular,
      .per_vertex_map = &per_vertex_map,
      .native_per_vertex = native_per_vertex && !per_vertex_map.active
         ? &native_inputs : NULL,
      .mesh_queries = features & BITFIELD_BIT(KK_FEAT_MESH_QUERIES),
      .pipeline_statistics = features & BITFIELD_BIT(KK_FEAT_PIPELINE_STATISTICS),
      .geometry_reports = features & BITFIELD_BIT(KK_FEAT_GEOMETRY_REPORTS),
      .conservative_fs = conservative_fs,
   };
   struct kk_shader *gs_mesh = NULL;
   /* Ordinary VS/FS never enters this branch or allocates alternate state. */
   if (explicit_geometry && !tess && total_shaders == 3 &&
       nir_shaders[0]->info.stage == MESA_SHADER_VERTEX &&
       nir_shaders[1]->info.stage == MESA_SHADER_GEOMETRY && !pixel_envelope &&
       !poly_layer_replay && !state->mv->view_mask &&
       !(infos[0].flags & VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT) &&
       !sparse_modes) {
      /* Embedded sampler ownership and indirect executable binding remain
       * with their existing compute executable in this bounded route. */
      bool supported_bindings = true;
      for (unsigned i = 0; i < shader_count; ++i)
         supported_bindings &= !infos[i].embedded_sampler_count &&
            !(infos[i].flags & (VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT |
                               VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT));
      if (supported_bindings)
         result = kk_compile_gs_mesh(dev, nir_shaders[0], nir_shaders[1],
                                  nir_shaders[2], state, vertex_robustness,
                                  &per_vertex_map, pAllocator, &gs_mesh);
      if (result != VK_SUCCESS) {
         for (unsigned i = 0; i < total_shaders; i++)
            ralloc_free(nir_shaders[i]);
         ralloc_free(conservative_fs);
         ralloc_free(retained_fs);
         ralloc_free(native_pair[0]);
         ralloc_free(native_pair[1]);
         if (linked)
            blob_finish(&linked_producer);
         memset(shaders_out, 0, shader_count * sizeof(*shaders_out));
         return result;
      }
   }

   for (uint32_t i = 0; i < total_shaders; i++) {
      struct kk_shader *prev_stage = i > 0 ? shaders[i - 1] : NULL;
      struct kk_compile_options stage_options = compile_options;
      if (nir_shaders[i]->info.stage == MESA_SHADER_FRAGMENT) {
         stage_options.vrs_source = retained_fs;
         retained_fs = NULL;
      }
      result =
         kk_compile_shader(dev, nir_shaders[i], prev_stage, vertex_robustness,
                           state, &stage_options, pAllocator, &shaders[i]);
      if (i == 0 && result == VK_SUCCESS && gs_mesh) {
         shaders[0]->gs_mesh = gs_mesh;
         gs_mesh = NULL;
      }
      if (result == VK_SUCCESS) {
         for (uint32_t input = 0; input < shader_count; ++input) {
            if (infos[input].stage != shaders[i]->vk.stage) continue;
            result = kk_shader_embedded_samplers_init(dev, shaders[i],
               infos[input].embedded_sampler_count, infos[input].embedded_samplers);
            break;
         }
         if (result != VK_SUCCESS) {
            kk_shader_destroy(&dev->vk, shaders[i], pAllocator);
            shaders[i] = NULL;
         }
      }

      if (result != VK_SUCCESS) {
         if (gs_mesh) kk_shader_destroy(&dev->vk, gs_mesh, pAllocator);
         /* Clean up all the shaders before this point */
         for (uint32_t j = 0; j < i; j++)
            kk_shader_destroy(&dev->vk, shaders[j], pAllocator);

         /* Clean up all the NIR after this point */
         for (uint32_t j = i + 1; j < total_shaders; j++) {
            ralloc_free(nir_shaders[j]);
         }
         ralloc_free(retained_fs);
         if (linked) blob_finish(&linked_producer);

         ralloc_free(native_pair[0]);
      ralloc_free(native_pair[1]);
      /* Memset the output array */
         memset(shaders_out, 0, shader_count * sizeof(*shaders_out));

         return result;
      }
   }

   if (linked) blob_finish(&linked_producer);

   /* Retain the stage-filtered layout extent in serialized shader info.
    * Graphics stages share one root, including stages lowered through Poly. */
   if (dgc_graphics && !dgc_replay) {
      for (uint32_t i = 0; i < total_shaders; ++i)
         dgc_replay |= shaders[i]->info.stage == MESA_SHADER_FRAGMENT &&
                       shaders[i]->info.fs.emulates_per_vertex_inputs;
   }
   const bool indirect_bindable =
      infos[0].flags & VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT;
   /* Shader objects are zero-allocated. Leave unused DGC metadata cold rather
    * than writing it for every stage of an ordinary pipeline. */
   if (dgc_replay || dgc_interlock_guard || indirect_bindable) {
      for (uint32_t i = 0; i < total_shaders; ++i) {
         shaders[i]->info.dgc_replay = dgc_replay;
         shaders[i]->info.dgc_interlock_guard = dgc_interlock_guard;
         shaders[i]->info.indirect_bindable = indirect_bindable;
      }
   }
   push_size = MAX2(push_size, heap_push_size);
   for (uint32_t i = 0; i < total_shaders; ++i) {
      shaders[i]->info.push_size = push_size;
      shaders[i]->info.descriptor_heap = infos[0].flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
   }

   /* Compile pipeline:
    * 1. Compute pipeline
    * 2. Graphics with all stages (since we don't support GPL nor shader
    * objects for now). This will be addressed later.
    */
   if (shaders[0]->vk.stage == MESA_SHADER_COMPUTE) {
      struct kk_shader *s = shaders[0];
      uint32_t local_size_threads = s->info.cs.local_size.x *
                                    s->info.cs.local_size.y *
                                    s->info.cs.local_size.z;
      bool deferred = kk_defer_compute && !s->info.indirect_bindable &&
         !s->msl_data[MESA_SHADER_COMPUTE].uses_atomic64_locks && !sparse_modes &&
         kk_async_compute_start(dev, s, kk_compile_async_compute);
      if (!deferred)
         result = kk_compile_compute_pipeline(dev, s, &s->msl_data[MESA_SHADER_COMPUTE],
                                              local_size_threads, &s->pipeline.cs);
      if (result == VK_SUCCESS && s->info.indirect_bindable &&
          !kk_dgc_get_compute_pipeline(dev, s))
         result = VK_ERROR_INVALID_SHADER_NV;
   } else {
      gather_graphics_pipeline_create_info(state, shaders, total_shaders);
      if (depth_bias_control) {
         struct kk_shader *producer = shaders[
            shaders[0]->info.stage == MESA_SHADER_TASK ? 1 : 0];
         producer->info.depth_bias_control = depth_bias_control;
      }
      if (native_pair[0]) {
         NIR_PASS(_, native_pair[0], nir_opt_dce);
         nir_shader_gather_info(native_pair[0], nir_shader_get_entrypoint(native_pair[0]));
         kk_lower_vs_vbo_mask(native_pair[0], state, vertex_robustness,
                              shaders[0]->info.vs.attribs_read);
         NIR_PASS(_, native_pair[0], msl_nir_lower_instance_id);
         NIR_PASS(_, native_pair[0], nir_opt_dce);
         NIR_PASS(_, native_pair[0], kk_nir_lower_descriptors, false,
                  vertex_robustness, 0, NULL);
         NIR_PASS(_, native_pair[0], kk_nir_lower_polygon_scalar_outputs,
                  native_pair[0]->info.outputs_written);
         kk_compile_msl(dev, native_pair[0], state, &shaders[0]->polygon_native);
         kk_line_fragment(native_pair[1], false, state);
         kk_compile_msl(dev, native_pair[1], state, &shaders[0]->polygon_native_fs);
         native_pair[0] = native_pair[1] = NULL;
         shaders[0]->info.polygon_native = true;
      }
      shaders[0]->info.api_geometry = explicit_geometry;
      result = kk_compile_graphics_pipeline(dev, shaders[shaders[0]->info.stage == MESA_SHADER_TASK ? 1 : 0]);

      /* Generated Poly and empty-fragment leaves are already copied into the
       * original vertex pipeline and share its normal lifetime. */
   }

   if (result != VK_SUCCESS) {
      for (uint32_t i = 0; i < total_shaders; i++) {
         kk_shader_destroy(&dev->vk, shaders[i], pAllocator);
      }

      ralloc_free(native_pair[0]);
      ralloc_free(native_pair[1]);
      /* Memset the output array */
      memset(shaders_out, 0, shader_count * sizeof(*shaders_out));
   } else {
      for (unsigned i = 0; i < shader_count; ++i) {
         for (unsigned j = 0; j < total_shaders; ++j)
            if (shaders[j]->vk.stage == infos[i].stage) shaders_out[i] = &shaders[j]->vk;
      }
      for (unsigned j = 0; j < total_shaders; ++j) {
         bool original = false;
         for (unsigned i = 0; i < shader_count; ++i) original |= shaders[j]->vk.stage == infos[i].stage;
         if (!original) kk_shader_destroy(&dev->vk, shaders[j], pAllocator);
      }
   }

   return result;
}

/* A GS and the VS holding its pipeline also carry the GS compute pass. */
static bool
kk_has_gs_main(const struct kk_shader_info *info)
{
   return info->stage == MESA_SHADER_GEOMETRY ||
          (info->stage == MESA_SHADER_VERTEX &&
           (info->vs.additional_stages_bits &
            BITFIELD_BIT(MESA_SHADER_GEOMETRY)));
}

static void
kk_msl_serialize(const struct msl_compile_data *data, struct blob *blob)
{
   uint32_t entrypoint_length = strlen(data->entrypoint_name) + 1;
   uint32_t code_length = strlen(data->code) + 1;
   blob_write_uint32(blob, entrypoint_length);
   blob_write_uint32(blob, code_length);
   blob_write_uint8(blob, 0x80u | (data->requires_msl41 ? 1u : 0u) |
                         (data->uses_matrix_coordinates ? 2u : 0u) |
                         (data->uses_atomic64_locks ? 4u : 0u) |
                         (data->uses_pipeline_statistics ? 8u : 0u) |
                         (data->uses_sampler_emulation ? 16u : 0u) |
                         (data->requires_sampler_emulation ? 32u : 0u));
   blob_write_bytes(blob, data->entrypoint_name, entrypoint_length);
   blob_write_bytes(blob, data->code, code_length);
}

#include "kk_shader_cache_bindings.h"

static bool
kk_shader_serialize_native(struct vk_device *vk_dev, const struct vk_shader *vk_shader,
                    struct blob *blob)
{
   struct kk_shader *shader = container_of(vk_shader, struct kk_shader, vk);

   if (shader->info.stage == MESA_SHADER_COMPUTE && shader->pipeline.compute.pending &&
       !kk_async_compute_serializable(shader))
      return false;
   if (!kk_shader_cache_write_bindings(shader, blob))
      return false;
   blob_write_bytes(blob, &shader->info, sizeof(shader->info));
   if (kk_shader_stage_has_mesh_info(shader->info.stage))
      blob_write_bytes(blob, &shader->mesh->info, sizeof(shader->mesh->info));
   kk_msl_serialize(&shader->msl_data[shader->info.stage], blob);
   if ((shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_MESH)) {
      u_foreach_bit(stage, shader->info.vs.additional_stages_bits) {
         kk_msl_serialize(&shader->msl_data[stage], blob);
      }
   }
   if (kk_has_gs_main(&shader->info))
      kk_msl_serialize(&shader->gs_main, blob);

   u_foreach_bit(i, shader->info.gs_aux_mask)
      kk_msl_serialize(&shader->gs_aux[i], blob);
   if (shader->info.polygon_point || shader->info.poly_layer_replay || shader->info.interlock_envelope) {
      for (unsigned i = 0; i < ARRAY_SIZE(shader->polygon); ++i) {
         blob_write_uint8(blob, shader->polygon[i].code != NULL);
         if (shader->polygon[i].code) kk_msl_serialize(&shader->polygon[i], blob);
      }
   }
   if (shader->info.polygon_native) {
      kk_msl_serialize(&shader->polygon_native, blob);
      kk_msl_serialize(&shader->polygon_native_fs, blob);
   }
   if (vk_dev->enabled_features.pipelineFragmentShadingRate ||
       vk_dev->enabled_features.primitiveFragmentShadingRate ||
       vk_dev->enabled_features.attachmentFragmentShadingRate)
      kk_vrs_source_serialize(shader->info.stage == MESA_SHADER_FRAGMENT
                                ? shader->vrs_source : NULL, blob);
   kk_interlock_shader_serialize(shader, blob);
   if (shader->info.stage == MESA_SHADER_VERTEX && shader->info.api_geometry) {
      blob_write_uint8(blob, shader->gs_mesh != NULL);
      if (shader->gs_mesh && !kk_shader_serialize_native(vk_dev, &shader->gs_mesh->vk, blob))
         return false;
   }
   return !blob->out_of_memory;
}

static VkResult
kk_msl_deserialize(struct blob_reader *blob, struct msl_compile_data *data)
{
   const uint32_t entrypoint_length = blob_read_uint32(blob);
   const uint32_t code_length = blob_read_uint32(blob);
   const uint8_t flags = blob_read_uint8(blob);
   data->requires_msl41 = (flags & 1u) != 0;
   data->uses_matrix_coordinates = (flags & 2u) != 0;
   data->uses_atomic64_locks = (flags & 4u) != 0;
   data->uses_pipeline_statistics = (flags & 8u) != 0;
   data->uses_sampler_emulation = (flags & 16u) != 0;
   data->requires_sampler_emulation = (flags & 32u) != 0;
   data->entrypoint_name = ralloc_array(NULL, char, entrypoint_length);
   if (data->entrypoint_name == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   data->code = ralloc_array(NULL, char, code_length);
   if (data->code == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   blob_copy_bytes(blob, (void *)data->entrypoint_name, entrypoint_length);
   blob_copy_bytes(blob, (void *)data->code, code_length);

   if (blob->overrun || !entrypoint_length || !code_length ||
       data->entrypoint_name[entrypoint_length - 1] || data->code[code_length - 1] ||
       (flags & ~0xbfu) || (!(flags & 0x80u) && (flags & 60u)) ||
       ((flags & 32u) && !(flags & 16u)))
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   /* Old ordinary source remains reusable. Old atomic source loaded its
    * address from root word0 and must be recompiled for specialization. */
   if (!(flags & 0x80u) && strstr(data->code, "kk_atomic64_lock("))
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;

   return VK_SUCCESS;
}

#include "kk_fragment_shading_rate_cache_impl.h"

static VkResult
kk_deserialize_shader_native_inner(struct vk_device *vk_dev, struct blob_reader *blob,
                      uint32_t binary_version,
                      const VkAllocationCallbacks *pAllocator,
                      struct vk_shader **shader_out, bool internal_mesh)
{
   struct kk_device *dev = container_of(vk_dev, struct kk_device, vk);

   struct kk_shader_cache_bindings bindings;
   VkResult result = kk_shader_cache_read_bindings(dev, blob, &bindings);
   if (result != VK_SUCCESS)
      return result;
   struct kk_shader_info info;
   blob_copy_bytes(blob, &info, sizeof(info));
   struct kk_mesh_shader_info mesh_info;
   if (!blob->overrun && kk_shader_stage_has_mesh_info(info.stage))
      blob_copy_bytes(blob, &mesh_info, sizeof(mesh_info));
   if (blob->overrun ||
       (internal_mesh &&
        (info.stage != MESA_SHADER_MESH || !info.api_geometry ||
         !mesh_info.gs_max_objects || !mesh_info.has_task ||
         mesh_info.local_size.x != 1 || mesh_info.local_size.y != 1 ||
         mesh_info.local_size.z != 1 || mesh_info.task_local_size.x != 1 ||
         mesh_info.task_local_size.y != 1 || mesh_info.task_local_size.z != 1 ||
         mesh_info.max_vertices > 256 || mesh_info.max_primitives > 512 ||
         mesh_info.task_grid_bound > KK_MESH_NATIVE_GROUPS)))
      return vk_error(dev, VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);

   struct kk_shader *shader = vk_shader_zalloc(
      &dev->vk, &kk_shader_ops, info.stage, pAllocator, kk_shader_size(info.stage));
   if (shader == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Cache incompatibility can fail before any source is accepted. Cleanup
    * must already know the actual stage owning that partial source. */
   shader->info = info;
   kk_shader_init_mesh_state(shader, info.stage);
   if (shader->mesh)
      shader->mesh->info = mesh_info;

   result = kk_shader_cache_restore_bindings(dev, shader, &bindings);
   if (result != VK_SUCCESS)
      goto fail;
   result = kk_msl_deserialize(blob, &shader->msl_data[info.stage]);
   if (result != VK_SUCCESS)
      goto fail;

   if ((shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_MESH)) {
      u_foreach_bit(stage, shader->info.vs.additional_stages_bits) {
         result = kk_msl_deserialize(blob, &shader->msl_data[stage]);
         if (result != VK_SUCCESS)
            goto fail;
      }
   }

   if (kk_has_gs_main(&info)) {
      result = kk_msl_deserialize(blob, &shader->gs_main);
      if (result != VK_SUCCESS)
         goto fail;
   }

   u_foreach_bit(i, info.gs_aux_mask) {
      result = kk_msl_deserialize(blob, &shader->gs_aux[i]);
      if (result != VK_SUCCESS) goto fail;
   }
   if (info.polygon_point || info.poly_layer_replay || info.interlock_envelope) {
      for (unsigned i = 0; i < ARRAY_SIZE(shader->polygon); ++i) {
         if (!blob_read_uint8(blob)) continue;
         result = kk_msl_deserialize(blob, &shader->polygon[i]);
         if (result != VK_SUCCESS) goto fail;
      }
   }
   if (info.polygon_native) {
      result = kk_msl_deserialize(blob, &shader->polygon_native);
      if (result != VK_SUCCESS) goto fail;
      result = kk_msl_deserialize(blob, &shader->polygon_native_fs);
      if (result != VK_SUCCESS) goto fail;
   }
   if (dev->vrs) {
      if (info.stage == MESA_SHADER_FRAGMENT)
         result = kk_vrs_source_deserialize(dev, blob, &shader->vrs_source);
      else if (blob_read_uint32(blob) != 0 || blob->overrun)
         result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
      if (result != VK_SUCCESS) goto fail;
   }
   result = kk_interlock_shader_deserialize(shader, blob);
   if (result != VK_SUCCESS) goto fail;
   if (info.stage == MESA_SHADER_VERTEX && info.api_geometry) {
      uint8_t present = blob_read_uint8(blob);
      if (present > 1 || blob->overrun) {
         result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
         goto fail;
      }
      if (present) {
         struct vk_shader *child = NULL;
         result = kk_deserialize_shader_native_inner(vk_dev, blob, binary_version,
                                                     pAllocator, &child, true);
         if (result != VK_SUCCESS)
            goto fail;
         shader->gs_mesh = container_of(child, struct kk_shader, vk);
      }
   }

   kk_shader_sampler_info(shader);
   if (info.stage == MESA_SHADER_COMPUTE) {
      uint32_t local_size_threads = shader->info.cs.local_size.x *
                                    shader->info.cs.local_size.y *
                                    shader->info.cs.local_size.z;
      result = kk_compile_compute_pipeline(
         dev, shader, &shader->msl_data[MESA_SHADER_COMPUTE], local_size_threads,
         &shader->pipeline.cs);
      if (result == VK_SUCCESS && shader->info.indirect_bindable &&
          !kk_dgc_get_compute_pipeline(dev, shader))
         result = VK_ERROR_INVALID_SHADER_NV;
   } else if (info.stage == MESA_SHADER_VERTEX || info.stage == MESA_SHADER_MESH) {
      result = kk_compile_graphics_pipeline(dev, shader);
   }

   if (result != VK_SUCCESS)
      goto fail;

   *shader_out = &shader->vk;

   return VK_SUCCESS;

fail:
   kk_shader_destroy(&dev->vk, shader, pAllocator);
   return vk_error(dev, result);
}

static VkResult
kk_deserialize_shader_native(struct vk_device *vk_dev, struct blob_reader *blob,
   uint32_t binary_version, const VkAllocationCallbacks *alloc,
   struct vk_shader **out)
{
   return kk_deserialize_shader_native_inner(vk_dev, blob, binary_version, alloc, out, false);
}

static void
kk_cmd_init_matrix_coordinates(struct kk_cmd_buffer *cmd)
{
   /* Exact binary16 encodings of the immutable coordinate ramp 0..63. */
   static const uint16_t matrix_coordinates[64] = {
      0x0000, 0x3c00, 0x4000, 0x4200, 0x4400, 0x4500, 0x4600, 0x4700,
      0x4800, 0x4880, 0x4900, 0x4980, 0x4a00, 0x4a80, 0x4b00, 0x4b80,
      0x4c00, 0x4c40, 0x4c80, 0x4cc0, 0x4d00, 0x4d40, 0x4d80, 0x4dc0,
      0x4e00, 0x4e40, 0x4e80, 0x4ec0, 0x4f00, 0x4f40, 0x4f80, 0x4fc0,
      0x5000, 0x5020, 0x5040, 0x5060, 0x5080, 0x50a0, 0x50c0, 0x50e0,
      0x5100, 0x5120, 0x5140, 0x5160, 0x5180, 0x51a0, 0x51c0, 0x51e0,
      0x5200, 0x5220, 0x5240, 0x5260, 0x5280, 0x52a0, 0x52c0, 0x52e0,
      0x5300, 0x5320, 0x5340, 0x5360, 0x5380, 0x53a0, 0x53c0, 0x53e0,
   };
   STATIC_ASSERT(sizeof(matrix_coordinates) ==
                 sizeof(cmd->state.cs.descriptors.root.cs.matrix_coordinates));
   memcpy(cmd->state.cs.descriptors.root.cs.matrix_coordinates,
          matrix_coordinates, sizeof(matrix_coordinates));
   cmd->state.cs.matrix_coordinates_initialized = true;
   cmd->state.cs.descriptors.root_dirty = true;
}

static void
kk_sparse_shader_bound(struct kk_cmd_buffer *cmd, const struct kk_shader *shader)
{
   if (!shader || !(shader->info.sparse_modes & KK_SPARSE_BUFFER)) return;
   cmd->uses_sparse_buffers = true;
   struct kk_descriptor_state *gfx = &cmd->state.gfx.descriptors;
   struct kk_descriptor_state *cs = &cmd->state.cs.descriptors;
   /* Meta state restore may clear one root while the command-buffer usage
    * bit remains set. Repair either root at bind, never at draw/dispatch. */
   if (!gfx->root.sparse_state || !cs->root.sparse_state) {
      uint64_t address = kk_sparse_device_address(kk_cmd_buffer_device(cmd));
      assert(address);
      gfx->root_dirty |= gfx->root.sparse_state != address;
      cs->root_dirty |= cs->root.sparse_state != address;
      gfx->root.sparse_state = cs->root.sparse_state = address;
   }
}

static void
kk_cmd_bind_compute_shader_selected(struct kk_cmd_buffer *cmd, struct kk_shader *shader)
{
   VkResult ready = shader && shader->info.stage == MESA_SHADER_COMPUTE &&
      shader->pipeline.compute.pending ? kk_async_compute_join(shader) : VK_SUCCESS;
   if (unlikely(ready != VK_SUCCESS)) {
      cmd->state.shaders[MESA_SHADER_COMPUTE] = shader;
      vk_command_buffer_set_error(&cmd->vk, ready);
      return;
   }
   kk_sparse_shader_bound(cmd, shader);
   if (shader && shader->msl_data[MESA_SHADER_COMPUTE].uses_matrix_coordinates &&
       !cmd->state.cs.matrix_coordinates_initialized)
      kk_cmd_init_matrix_coordinates(cmd);
   struct kk_descriptor_state *desc = &cmd->state.cs.descriptors;
   /* Meta operations can restore an unbound compute stage. */
   uint32_t push_size = shader ? shader->info.push_size : 0;
   bool heap = shader && shader->info.descriptor_heap;
   desc->root_dirty |= heap != desc->descriptor_heap || push_size > desc->push_size;
   desc->descriptor_heap = heap;
   if (heap) {
      desc->root_dirty |= desc->heap_embedded_samplers[MESA_SHADER_COMPUTE] !=
                           shader->embedded_sampler_table;
      desc->heap_embedded_samplers[MESA_SHADER_COMPUTE] = shader->embedded_sampler_table;
   }
   desc->push_size = push_size;
   cmd->state.shaders[MESA_SHADER_COMPUTE] = shader;
}

static void
kk_cmd_bind_graphics_shader_selected(struct kk_cmd_buffer *cmd,
                            const mesa_shader_stage stage,
                            struct kk_shader *shader)
{
   kk_sparse_shader_bound(cmd, shader);
   if (shader && shader->info.descriptor_heap) {
      cmd->state.gfx.descriptors.root_dirty |=
         cmd->state.gfx.descriptors.heap_embedded_samplers[stage] != shader->embedded_sampler_table;
      cmd->state.gfx.descriptors.heap_embedded_samplers[stage] = shader->embedded_sampler_table;
   }
   cmd->state.shaders[stage] = shader;
   cmd->state.dirty_shaders |= BITFIELD_BIT(stage);

   /* Relevant pipeline data is only stored in vertex shaders */
   if (stage != MESA_SHADER_VERTEX && stage != MESA_SHADER_MESH)
      return;
   if (!shader) return;
   cmd->state.gfx.shader = shader;
   kk_cmd_buffer_update_conservative(cmd, shader);

   struct kk_descriptor_state *desc = &cmd->state.gfx.descriptors;
   desc->root_dirty |= shader->info.descriptor_heap != desc->descriptor_heap || shader->info.push_size > desc->push_size;
   desc->descriptor_heap = shader->info.descriptor_heap;
   desc->push_size = shader->info.push_size;

   bool requires_dynamic_depth_stencil = shader->pipeline.gfx.ds_handle == NULL;
   if (cmd->state.gfx.is_depth_stencil_dynamic) {
      /* If we are switching from dynamic to static, we need to clean up
       * temporary state. Otherwise, leave the existing dynamic state
       * untouched.
       */
      if (!requires_dynamic_depth_stencil) {
         mtl_release(cmd->state.gfx.depth_stencil_state);
         cmd->state.gfx.depth_stencil_state = shader->pipeline.gfx.ds_handle;
      }
   } else
      cmd->state.gfx.depth_stencil_state = shader->pipeline.gfx.ds_handle;
   cmd->state.gfx.is_depth_stencil_dynamic = requires_dynamic_depth_stencil;
   cmd->state.gfx.dirty |= KK_DIRTY_VB;

   cmd->state.gfx.pipeline_sample_count = shader->info.vs.sample_count;
}

void
kk_cmd_bind_compute_shader(struct kk_cmd_buffer *cmd, struct kk_shader *shader)
{
   kk_cmd_bind_compute_shader_selected(cmd, kk_sparse_select_shader(cmd, shader));
}

static void
kk_cmd_bind_graphics_shader(struct kk_cmd_buffer *cmd, mesa_shader_stage stage,
                            struct kk_shader *shader)
{
   kk_cmd_bind_graphics_shader_selected(cmd, stage, kk_sparse_select_shader(cmd, shader));
}

static void
kk_cmd_bind_shaders(struct vk_command_buffer *cmd_buffer, uint32_t stage_count,
                    const mesa_shader_stage *stages,
                    struct vk_shader **const shaders)
{
   struct kk_cmd_buffer *cmd =
      container_of(cmd_buffer, struct kk_cmd_buffer, vk);

   for (uint32_t i = 0; i < stage_count; i++) {
      struct kk_shader *shader = container_of(shaders[i], struct kk_shader, vk);

      if (stages[i] == MESA_SHADER_COMPUTE || stages[i] == MESA_SHADER_KERNEL)
         kk_cmd_bind_compute_shader(cmd, shader);
      else
         kk_cmd_bind_graphics_shader(cmd, stages[i], shader);
   }
}

/* DGC constructs its existing pipeline/parameter snapshot once per command.
 * Ordinary draws and dispatches select at bind and never call this helper. */
void
kk_sparse_refresh_shader(struct kk_cmd_buffer *cmd, struct kk_cmd_buffer *state,
                         mesa_shader_stage stage)
{
   struct kk_shader *shader = state->state.shaders[stage];
   struct kk_shader *selected = kk_sparse_select_shader(cmd, shader);
   kk_sparse_shader_bound(cmd, selected);
   if (selected == shader) return;
   if (stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_MESH)
      kk_cmd_bind_graphics_shader_selected(state, stage, selected);
   else kk_cmd_bind_compute_shader_selected(state, selected);
}

static VkResult
kk_shader_get_executable_properties(
   UNUSED struct vk_device *device, const struct vk_shader *vk_shader,
   uint32_t *executable_count, VkPipelineExecutablePropertiesKHR *properties)
{
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutablePropertiesKHR, out, properties,
                          executable_count);

   return vk_outarray_status(&out);
}

static VkResult
kk_shader_get_executable_statistics(
   UNUSED struct vk_device *device, const struct vk_shader *vk_shader,
   uint32_t executable_index, uint32_t *statistic_count,
   VkPipelineExecutableStatisticKHR *statistics)
{
   /* TODO_KOSMICKRISP */
   return VK_SUCCESS;
}

static VkResult
kk_shader_get_executable_internal_representations(
   UNUSED struct vk_device *device, const struct vk_shader *vk_shader,
   uint32_t executable_index, uint32_t *internal_representation_count,
   VkPipelineExecutableInternalRepresentationKHR *internal_representations)
{
   /* TODO_KOSMICKRISP */
   return VK_SUCCESS;
}

static void
kk_vk_shader_destroy(struct vk_device *vk_dev, struct vk_shader *vk_shader,
                     const VkAllocationCallbacks *pAllocator)
{
   kk_shader_destroy(vk_dev, container_of(vk_shader, struct kk_shader, vk),
                     pAllocator);
}

static const struct vk_shader_ops kk_shader_ops = {
   .destroy = kk_vk_shader_destroy,
   .serialize = kk_shader_serialize,
   .get_executable_properties = kk_shader_get_executable_properties,
   .get_executable_statistics = kk_shader_get_executable_statistics,
   .get_executable_internal_representations =
      kk_shader_get_executable_internal_representations,
};

const struct vk_device_shader_ops kk_device_shader_ops = {
   .link_rt_pipeline = kk_rt_pipeline_realize,
   .write_rt_shader_group = kk_rt_write_group,
   .cmd_set_rt_state = kk_rt_set_state,
   .cmd_set_stack_size = kk_rt_set_stack_size,
   .destroy_rt_pipeline = kk_rt_pipeline_destroy,
   .cmd_bind_rt_pipeline = kk_rt_pipeline_bind,
   .get_nir_options = kk_get_nir_options,
   .require_graphics_stage_linking = kk_interlock_link_graphics,
   .get_spirv_options = kk_get_spirv_options,
   .preprocess_nir = kk_preprocess_nir,
   .hash_state = kk_hash_graphics_state,
   .compile =
      kk_compile_shaders, /* This will only generate the MSL string we need
                             to use for actual library generation */
   .deserialize = kk_deserialize_shader,
   .cmd_set_dynamic_graphics_state = vk_cmd_set_dynamic_graphics_state,
   .cmd_bind_shaders = kk_cmd_bind_shaders,
};

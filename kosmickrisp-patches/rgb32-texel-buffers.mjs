// SPDX-License-Identifier: MIT
// RGB32 SRVs use three consecutive R32 elements. The existing descriptor's
// prefix word carries the representation tag, so its size and upload stay fixed.
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_buffer_view.c', s => {
    s = s.replace('#include "kk_buffer_view.h"', '#include "kk_buffer_view.h"\n#include "kk_descriptor_types.h"');
    s = s.replace('VkFormatFeatureFlags2\nkk_get_buffer_format_features',
      `static enum pipe_format
kk_texel_buffer_native_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R32G32B32_FLOAT: return PIPE_FORMAT_R32_FLOAT;
   case PIPE_FORMAT_R32G32B32_UINT: return PIPE_FORMAT_R32_UINT;
   case PIPE_FORMAT_R32G32B32_SINT: return PIPE_FORMAT_R32_SINT;
   default: return format;
   }
}

VkFormatFeatureFlags2
kk_get_buffer_format_features`);
    s = s.replace('   const struct kk_va_format *format = kk_get_va_format(p_format);',
      `   if (kk_texel_buffer_native_format(p_format) != p_format)
      features |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;

   const struct kk_va_format *format = kk_get_va_format(p_format);`);
    s = s.replace('   const struct kk_va_format *supported_format = kk_get_va_format(p_format);',
      `   enum pipe_format native_format = kk_texel_buffer_native_format(p_format);
   bool rgb32 = native_format != p_format;
   p_format = native_format;
   const struct kk_va_format *supported_format = kk_get_va_format(p_format);`);
    s = s.replace('      .width_px = view->vk.elements + view->texel_offset,',
      '      .width_px = view->vk.elements * (rgb32 ? 3u : 1u) + view->texel_offset,');
    s = s.replace('   *pBufferView = kk_buffer_view_to_handle(view);\n\n   return VK_SUCCESS;',
      `   if (rgb32)
      view->texel_offset |= KK_TEXEL_BUFFER_RGB32;

   *pBufferView = kk_buffer_view_to_handle(view);

   return VK_SUCCESS;`);
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s.replace(
    '   if (sampler != NULL) {',
    `   if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF && tex->op == nir_texop_txf) {
      nir_def *metadata = load_resource_deref_desc(
         b, 2, 32, nir_src_as_deref(nir_src_for_ssa(texture)),
         offsetof(struct kk_storage_image_descriptor, texel_offset), ctx);
      nir_def *rgb32 = nir_ine_imm(b, nir_iand_imm(b, nir_channel(b, metadata, 0),
                                                 KK_TEXEL_BUFFER_RGB32), 0);
      b->cursor = nir_after_instr(&tex->instr);
      /* Keep the original definition out of the fixup's uses: replacement
       * spans new control-flow blocks, not one instruction list. */
      nir_tex_instr *first = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
      nir_builder_instr_insert(b, &first->instr);
      nir_def *base = tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa;
      nir_def *valid = nir_ine_imm(b, base, -1);
      nir_push_if(b, rgb32);
      nir_def *rgb[4] = {nir_channel(b, &first->def, 0)};
      for (unsigned c = 1; c < 3; ++c) {
         nir_tex_instr *component = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
         int coord_src = nir_tex_instr_src_index(component, nir_tex_src_coord);
         nir_def *component_coord = nir_bcsel(b, valid, nir_iadd_imm(b, base, c), base);
         nir_builder_instr_insert(b, &component->instr);
         nir_src_rewrite(&component->src[coord_src].src, component_coord);
         rgb[c] = nir_channel(b, &component->def, 0);
      }
      nir_def *one = nir_alu_type_get_base_type(tex->dest_type) == nir_type_float
                       ? nir_imm_floatN_t(b, 1.0, tex->def.bit_size)
                       : nir_imm_intN_t(b, 1, tex->def.bit_size);
      rgb[3] = nir_bcsel(b, valid, one, nir_imm_intN_t(b, 0, tex->def.bit_size));
      nir_def *assembled = nir_vec(b, rgb, tex->def.num_components);
      nir_pop_if(b, NULL);
      nir_def *result = nir_if_phi(b, assembled, &first->def);
      nir_def_replace(&tex->def, result);
   }

   if (sampler != NULL) {`).replace(
    'nir, try_lower_descriptors_instr, nir_metadata_control_flow, &ctx);',
    'nir, try_lower_descriptors_instr, nir_metadata_none, &ctx);'));
}

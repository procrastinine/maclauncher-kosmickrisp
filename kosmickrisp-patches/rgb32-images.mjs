// SPDX-License-Identifier: MIT
import fs from 'node:fs';
const rgb32Storage = `   if (nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_BUF &&
       (intr->intrinsic == nir_intrinsic_bindless_image_load ||
        intr->intrinsic == nir_intrinsic_bindless_image_store)) {
      nir_def *metadata = load_resource_deref_desc(
         b, 2, 32, deref,
         offsetof(struct kk_storage_image_descriptor, texel_offset), ctx);
      nir_def *rgb32 = nir_ine_imm(b, nir_iand_imm(b, nir_channel(b, metadata, 0),
                                                 KK_TEXEL_BUFFER_RGB32), 0);
      nir_def *coord = intr->src[1].ssa;
      nir_def *base = nir_channel(b, coord, 0);
      nir_def *valid = nir_ine_imm(b, base, -1);
      if (intr->intrinsic == nir_intrinsic_bindless_image_load) {
         b->cursor = nir_after_instr(&intr->instr);
         nir_intrinsic_instr *first = nir_instr_as_intrinsic(
            nir_instr_clone(b->shader, &intr->instr));
         nir_builder_instr_insert(b, &first->instr);
         nir_push_if(b, rgb32);
         nir_def *rgb[4] = {nir_channel(b, &first->def, 0)};
         for (unsigned c = 1; c < 3; ++c) {
            nir_def *at = nir_vector_insert_imm(b, coord,
               nir_bcsel(b, valid, nir_iadd_imm(b, base, c), base), 0);
            nir_intrinsic_instr *component = nir_instr_as_intrinsic(
               nir_instr_clone(b->shader, &intr->instr));
            nir_builder_instr_insert(b, &component->instr);
            nir_src_rewrite(&component->src[1], at);
            rgb[c] = nir_channel(b, &component->def, 0);
         }
         nir_def *one = nir_alu_type_get_base_type(type) == nir_type_float ?
            nir_imm_floatN_t(b, 1, intr->def.bit_size) :
            nir_imm_intN_t(b, 1, intr->def.bit_size);
         rgb[3] = nir_bcsel(b, valid, one, nir_imm_intN_t(b, 0, intr->def.bit_size));
         nir_def *assembled = nir_vec(b, rgb, intr->def.num_components);
         nir_pop_if(b, NULL);
         nir_def *result = nir_if_phi(b, assembled, &first->def);
         nir_def_replace(&intr->def, result);
      } else {
         nir_def *value = intr->src[3].ssa;
         nir_push_if(b, rgb32);
         for (unsigned c = 0; c < 3; ++c) {
            nir_def *at = nir_vector_insert_imm(b, coord,
               nir_bcsel(b, valid, nir_iadd_imm(b, base, c), base), 0);
            nir_def *scalar = nir_replicate(b, nir_channel(b, value, c),
                                            value->num_components);
            nir_intrinsic_instr *component = nir_instr_as_intrinsic(
               nir_instr_clone(b->shader, &intr->instr));
            nir_builder_instr_insert(b, &component->instr);
            nir_src_rewrite(&component->src[1], at);
            nir_src_rewrite(&component->src[3], scalar);
         }
         nir_push_else(b, NULL);
         nir_builder_instr_insert(b, nir_instr_clone(b->shader, &intr->instr));
         nir_pop_if(b, NULL);
         nir_instr_remove(&intr->instr);
      }
   }
`;
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-rgb32-copy.h'];
export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_format.h', s => s.replace(
    'const struct kk_va_format *kk_get_va_format(enum pipe_format format);',
    `static inline bool
kk_format_is_rgb32(enum pipe_format format)
{
   return format == PIPE_FORMAT_R32G32B32_FLOAT ||
          format == PIPE_FORMAT_R32G32B32_UINT ||
          format == PIPE_FORMAT_R32G32B32_SINT;
}

static inline enum pipe_format
kk_format_backing(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R32G32B32_FLOAT: return PIPE_FORMAT_R32G32B32A32_FLOAT;
   case PIPE_FORMAT_R32G32B32_UINT: return PIPE_FORMAT_R32G32B32A32_UINT;
   case PIPE_FORMAT_R32G32B32_SINT: return PIPE_FORMAT_R32G32B32A32_SINT;
   default: return format;
   }
}

const struct kk_va_format *kk_get_va_format(enum pipe_format format);`));
  edit('src/kosmickrisp/vulkan/kk_format.c', s => s.replace(
    '   // 128-bit formats',
    `   // RGB32 images retain Vulkan's 12-byte transfer layout. Native images
   // have a fourth, hidden component; sampled and storage reads supply one.
   MTL_FMT(R32G32B32_FLOAT, R32G32B32A32_FLOAT, MTL_SWIZZLE_RGB1,
           .bit_widths = 96, .filter = 1, .write = 1,
           MTL_FMT_TB_NONE, true),
   MTL_FMT(R32G32B32_UINT, R32G32B32A32_UINT, MTL_SWIZZLE_RGB1,
           .bit_widths = 96, .write = 1, MTL_FMT_TB_NONE, true),
   MTL_FMT(R32G32B32_SINT, R32G32B32A32_SINT, MTL_SWIZZLE_RGB1,
           .bit_widths = 96, .write = 1, MTL_FMT_TB_NONE, true),

   // 128-bit formats`)
    // Commas in capability initializers must remain one macro argument.
    .replace('.bit_widths = 96, .filter = 1, .write = 1,\n           MTL_FMT_TB_NONE',
             'MTL_FMT_RGB32_FLOAT, MTL_FMT_TB_NONE')
    .replaceAll('.bit_widths = 96, .write = 1, MTL_FMT_TB_NONE',
                'MTL_FMT_RGB32_INT, MTL_FMT_TB_NONE')
    .replace('// Write, Color, Sparse, Atomic',
      `#define MTL_FMT_RGB32_FLOAT .bit_widths = 96, .filter = 1, .write = 1
#define MTL_FMT_RGB32_INT .bit_widths = 96, .write = 1

// Write, Color, Sparse, Atomic`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
    `   struct mtl_clear_color swizzled_color;
   for (uint32_t i = 0u; i < 4; ++i)
      swizzled_color.channel[supported_format->unswizzle.channels[i]] =
         value.channel[i];`,
    `   struct mtl_clear_color swizzled_color = value;
   for (uint32_t i = 0u; i < 4; ++i) {
      unsigned channel = supported_format->unswizzle.channels[i];
      /* Constant view channels have no physical destination. */
      if (channel < 4)
         swizzled_color.channel[channel] = value.channel[i];
   }`));
  edit('src/kosmickrisp/vulkan/kk_image.c', s => s
    .replace('   /* You can\'t tile a non-power-of-two */',
      `   /* Optimal RGB32 images use RGBA32 backing. Linear host memory must
    * retain Vulkan's packed RGB layout and cannot alias that representation. */
   if (kk_format_is_rgb32(p_format) && tiling == VK_IMAGE_TILING_LINEAR)
      return 0;

   /* You can't tile a non-power-of-two */`)
    .replace('if (!util_is_power_of_two_nonzero(util_format_get_blocksize(p_format)))',
      'if (!kk_format_is_rgb32(p_format) &&\n       !util_is_power_of_two_nonzero(util_format_get_blocksize(p_format)))')
    .replace('if (!util_format_is_depth_and_stencil(p_format))',
      'if (!util_format_is_depth_and_stencil(p_format) && !kk_format_is_rgb32(p_format))'));
  edit('src/kosmickrisp/vulkan/kk_image_layout.c', s => s.replace(
    '   layout->format.pipe = format;',
    '   layout->format.pipe = kk_format_backing(format);'));
  edit('src/kosmickrisp/vulkan/kk_image_view.c', s => s.replace(
    '         .format = {.pipe = p_format,',
    '         .format = {.pipe = kk_format_backing(p_format),'));
  edits.set('src/kosmickrisp/vulkan/kk_rgb32_copy.h',
    fs.readFileSync(new URL('./kosmickrisp-rgb32-copy.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_cmd_copy.c', s => s
    .replace('#include "kk_entrypoints.h"',
      '#include "kk_entrypoints.h"\n#include "kk_format.h"\n#include "vk_format.h"')
    .replace('VKAPI_ATTR void VKAPI_CALL\nkk_CmdCopyBufferToImage2',
      '#include "kk_rgb32_copy.h"\n\nVKAPI_ATTR void VKAPI_CALL\nkk_CmdCopyBufferToImage2')
    .replace('   mtl_compute_encoder *encoder = cs_get_compute(cmd);\n   for (int r = 0; r < pCopyBufferToImageInfo->regionCount; r++) {',
      `   if (kk_format_is_rgb32(vk_format_to_pipe_format(image->vk.format))) {
      for (uint32_t r = 0; r < pCopyBufferToImageInfo->regionCount; ++r)
         kk_copy_rgb32_buffer_image(cmd, buffer, image,
                                   &pCopyBufferToImageInfo->pRegions[r], true);
      return;
   }
   mtl_compute_encoder *encoder = cs_get_compute(cmd);
   for (int r = 0; r < pCopyBufferToImageInfo->regionCount; r++) {`)
    .replace('   mtl_compute_encoder *encoder = cs_get_compute(cmd);\n   for (unsigned r = 0; r < pCopyImageToBufferInfo->regionCount; r++) {',
      `   if (kk_format_is_rgb32(vk_format_to_pipe_format(image->vk.format))) {
      for (uint32_t r = 0; r < pCopyImageToBufferInfo->regionCount; ++r)
         kk_copy_rgb32_buffer_image(cmd, buffer, image,
                                   &pCopyImageToBufferInfo->pRegions[r], false);
      return;
   }
   mtl_compute_encoder *encoder = cs_get_compute(cmd);
   for (unsigned r = 0; r < pCopyImageToBufferInfo->regionCount; r++) {`));
  // The common raw-copy fallback handles volume extents and 2D-array slices.
  edit('src/kosmickrisp/vulkan/kk_cmd_copy.c', s => s
    .replace('         buffer_size += buffer_size_2d_B * layer_count;',
      '         buffer_size += (size_t)buffer_size_2d_B *\n' +
      '                        MAX2(layer_count, region->extent.depth);')
    .replace('   /* Texture->Buffer->Texture */',
      `   bool src_3d = src->vk.image_type == VK_IMAGE_TYPE_3D;
   bool dst_3d = dst->vk.image_type == VK_IMAGE_TYPE_3D;
   uint32_t layers = MAX2(
      vk_image_subresource_layer_count(&src->vk, &region->srcSubresource),
      vk_image_subresource_layer_count(&dst->vk, &region->dstSubresource));
   if (src_3d != dst_3d)
      src_size.z = dst_size.z = 1;

   /* Texture->Buffer->Texture */`)
    .replace(/   \/\* TODO_KOSMICKRISP:[\s\S]*?   for \(uint32_t slice_idx = 0;[\s\S]*?        \+\+slice_idx\) \{/,
      '   for (uint32_t slice_idx = 0; slice_idx < layers; ++slice_idx) {')
    .replace('         region->srcSubresource.baseArrayLayer + slice_idx;',
      `         src_3d ? 0 : region->srcSubresource.baseArrayLayer + slice_idx;
      info.mtl_data.buffer_2d_image_size_B = src_3d ? buffer_size_2d_B : 0;`)
    .replace('         region->dstSubresource.baseArrayLayer + slice_idx;',
      `         dst_3d ? 0 : region->dstSubresource.baseArrayLayer + slice_idx;
      info.mtl_data.buffer_2d_image_size_B = dst_3d ? buffer_size_2d_B : 0;`)
    .replace('      buffer_offset += info.buffer_slice_size_B;\n   }\n\n   return buffer_offset;',
      `      buffer_offset += (size_t)buffer_size_2d_B * src_size.z;
      if (src_3d != dst_3d) {
         if (src_3d) src_origin.z++;
         if (dst_3d) dst_origin.z++;
      }
   }

   return buffer_offset;`));
  edit('src/kosmickrisp/libkk/kk_query.cl', s => s + `
/* Bit-preserving packed RGB32 <-> native RGBA32 transfer conversion. */
KERNEL2D(8, 8)
libkk_rgb32_copy(global uint8_t *packed, global uint32_t *rgba,
                 uint64_t row_B, uint32_t width, uint32_t height,
                 uint32_t expand, uint32_t alpha)
{
   uint x = cl_global_id.x, y = cl_global_id.y;
   if (x >= width || y >= height)
      return;
   global uint32_t *rgb = (global uint32_t *)(packed + y * row_B + x * 12u);
   global uint32_t *dst = rgba + ((uint64_t)y * width + x) * 4u;
   if (expand) {
      dst[0] = rgb[0]; dst[1] = rgb[1]; dst[2] = rgb[2]; dst[3] = alpha;
   } else {
      rgb[0] = dst[0]; rgb[1] = dst[1]; rgb[2] = dst[2];
   }
}
`);
  edit('src/kosmickrisp/vulkan/kk_buffer_view.c', s => s.replace(
    '      features |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;\n\n   const struct kk_va_format *format',
    `      features |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;

   const struct kk_va_format *format`));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s.replace(
    '   nir_rewrite_image_intrinsic(intr, handle, nir_image_intrinsic_type_bindless);',
    '   nir_rewrite_image_intrinsic(intr, handle, nir_image_intrinsic_type_bindless);\n\n' + rgb32Storage));
}

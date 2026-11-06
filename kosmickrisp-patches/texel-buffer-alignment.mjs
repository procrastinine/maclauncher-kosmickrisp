// SPDX-License-Identifier: MIT
export function apply({edit}) {
edit('src/kosmickrisp/vulkan/kk_descriptor_types.h', s => s.replace(
 'struct kk_storage_image_descriptor {',
 '#define KK_TEXEL_BUFFER_RGB32 (1u << 31)\n\nstruct kk_storage_image_descriptor {'));
// A texel-buffer view can begin at one texel even when Metal requires a
// 16-byte texture-buffer offset. Keep the native view aligned and carry its
// prefix and logical length in the descriptor, including null descriptors.
edit('src/kosmickrisp/vulkan/kk_descriptor_types.h', s => s
 .replace('struct kk_storage_image_descriptor {\n   uint64_t image_gpu_resource_id;\n};',
 `struct kk_storage_image_descriptor {
   uint64_t image_gpu_resource_id;
   uint32_t texel_offset;
   uint32_t texel_count;
};`)
 .replace('sizeof(struct kk_storage_image_descriptor) == 8',
          'sizeof(struct kk_storage_image_descriptor) == 16'));
edit('src/kosmickrisp/vulkan/kk_buffer_view.h', (s, replace) => replace(
 '   uint64_t texel_buffer_gpu_id;',
 '   uint64_t texel_buffer_gpu_id;\n   uint32_t texel_offset;'));
edit('src/kosmickrisp/vulkan/kk_buffer_view.c', s => s
 .replace('   struct kk_image_layout layout = {', `   /* Align relative to the buffer, whose memory binding is already aligned.
    * A common multiple also handles three-component texels without rounding
    * a byte prefix to a fractional texel. */
   uint32_t texel_size = util_format_get_blocksize(p_format);
   uint32_t alignment = KK_MIN_TEXEL_BUFFER_ALIGNMENT;
   while (alignment % texel_size)
      alignment += KK_MIN_TEXEL_BUFFER_ALIGNMENT;
   VkDeviceSize prefix = view->vk.offset % alignment;
   view->texel_offset = prefix / texel_size;

   struct kk_image_layout layout = {`)
 .replace('      .width_px = view->vk.elements,',
          '      .width_px = view->vk.elements + view->texel_offset,')
 .replace('      .linear_stride_B = view->vk.range,',
          '      .linear_stride_B = view->vk.range + prefix,')
 .replace('      kk_buffer_mtl_offset(buffer, view->vk.offset));',
          '      kk_buffer_mtl_offset(buffer, view->vk.offset - prefix));'));
edit('src/kosmickrisp/vulkan/kk_descriptor_set.c', (s, replace) => replace(
 '      desc.image_gpu_resource_id = view->texel_buffer_gpu_id;',
 `      desc.image_gpu_resource_id = view->texel_buffer_gpu_id;
      desc.texel_offset = view->texel_offset;
      desc.texel_count = view->vk.elements;`));
edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => {
 const helper = `/* Preserve logical view bounds before adding the Metal-alignment prefix.
 * Negative and overflowing coordinates must not access that prefix. */
static nir_def *
kk_texel_buffer_coord(nir_builder *b, nir_def *coord, nir_def *metadata)
{
   nir_def *word = nir_channel(b, metadata, 0);
   nir_def *offset = nir_iand_imm(b, word, ~KK_TEXEL_BUFFER_RGB32);
   nir_def *stride = nir_bcsel(b, nir_ine_imm(b, nir_iand_imm(b, word, KK_TEXEL_BUFFER_RGB32), 0),
                              nir_imm_int(b, 3), nir_imm_int(b, 1));
   nir_def *count = nir_channel(b, metadata, 1);
   nir_def *x = nir_channel(b, coord, 0);
   nir_def *in_bounds = nir_ult(b, x, count);
   nir_def *shifted = nir_bcsel(b, in_bounds, nir_iadd(b, nir_imul(b, x, stride), offset),
                               nir_imm_int(b, -1));
   return coord->num_components == 1 ? shifted
                                    : nir_vector_insert_imm(b, coord, shifted, 0);
}

`;
 s = s.replace('static bool\nlower_image_intrin(', helper + 'static bool\nlower_image_intrin(');
 s = s.replace('   /* Reads and queries use the texture descriptor; writes and atomics PBE. */',
 `   if (nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_BUF) {
      nir_def *metadata = load_resource_deref_desc(
         b, 2, 32, deref,
         offsetof(struct kk_storage_image_descriptor, texel_offset), ctx);
      if (intr->intrinsic == nir_intrinsic_image_deref_size) {
         nir_def_replace(&intr->def, nir_channel(b, metadata, 1));
         return true;
      }
      if (intr->intrinsic == nir_intrinsic_image_deref_load ||
          intr->intrinsic == nir_intrinsic_image_deref_sparse_load ||
          intr->intrinsic == nir_intrinsic_image_deref_store ||
          intr->intrinsic == nir_intrinsic_image_deref_atomic ||
          intr->intrinsic == nir_intrinsic_image_deref_atomic_swap)
         nir_src_rewrite(&intr->src[1],
                        kk_texel_buffer_coord(b, intr->src[1].ssa, metadata));
   }

   /* Reads and queries use the texture descriptor; writes and atomics PBE. */`);
 s = s.replace('   /* LOD bias is passed in the descriptor set, rather than embedded into',
 `   if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF) {
      nir_def *metadata = load_resource_deref_desc(
         b, 2, 32, nir_src_as_deref(nir_src_for_ssa(texture)),
         offsetof(struct kk_storage_image_descriptor, texel_offset), ctx);
      if (tex->op == nir_texop_txs) {
         nir_def_replace(&tex->def, nir_channel(b, metadata, 1));
         return true;
      }
      int coord_src = nir_tex_instr_src_index(tex, nir_tex_src_coord);
      if (coord_src >= 0)
         nir_src_rewrite(&tex->src[coord_src].src,
                        kk_texel_buffer_coord(b, tex->src[coord_src].src.ssa,
                                              metadata));
   }

   /* LOD bias is passed in the descriptor set, rather than embedded into`);
 return s;
});
edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
 .replace('.storageTexelBufferOffsetSingleTexelAlignment = false',
          '.storageTexelBufferOffsetSingleTexelAlignment = true')
 .replace('.uniformTexelBufferOffsetSingleTexelAlignment = false',
          '.uniformTexelBufferOffsetSingleTexelAlignment = true')
 .replace('.maxTexelBufferElements = 16384 * 16384,',
          '.maxTexelBufferElements = 16384 * 16384 - KK_MIN_TEXEL_BUFFER_ALIGNMENT,'));
}

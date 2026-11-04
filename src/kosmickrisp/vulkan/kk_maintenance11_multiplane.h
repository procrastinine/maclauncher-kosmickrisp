/* SPDX-License-Identifier: MIT */
#ifndef KK_MAINTENANCE11_MULTIPLANE_H
#define KK_MAINTENANCE11_MULTIPLANE_H

#ifdef KK_MAINTENANCE11_DESCRIPTOR
static void
kk_alias_sampled_tag(struct kk_sampled_image_descriptor *desc, bool native_alias)
{
   /* Called after ordinary LOD assignment; the sparse query tag is independent. */
   if (native_alias)
      desc->image_min_lod_uint16 |= KK_IMAGE_NATIVE_ARRAY_ALIAS_TAG;
}

#else
static nir_def *
kk_alias_sampled_address(nir_builder *b, const nir_tex_instr *tex,
                         nir_deref_instr *deref, nir_def *primary_slot_address,
                         unsigned image_plane_offset_B, unsigned relative_offs,
                         const struct lower_descriptors_ctx *ctx)
{
   if (!ctx->image_array_alias || !tex->is_array ||
       !kk_alias_array_dimension(tex->sampler_dim) ||
       relative_offs != offsetof(struct kk_sampled_image_descriptor,
                                 image_gpu_resource_id))
      return primary_slot_address;

   /* Heap plane1/2 primary slots are48/56, but their image flags remain in
    * the root descriptor. Set planeN flags live in descriptor64*N. */
   nir_def *flags = load_resource_deref_desc(b, 1, 16, deref,
      image_plane_offset_B +
         offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16), ctx);
   nir_def *array_slot = nir_iadd_imm(b, primary_slot_address,
      offsetof(struct kk_sampled_image_descriptor, aux_resource_id));
   return nir_bcsel(b, nir_ine_imm(b,
      nir_iand_imm(b, flags, KK_IMAGE_NATIVE_ARRAY_ALIAS_TAG), 0),
      array_slot, primary_slot_address);
}
#endif
#endif

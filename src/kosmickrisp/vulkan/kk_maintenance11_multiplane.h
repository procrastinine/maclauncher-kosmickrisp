/* SPDX-License-Identifier: MIT */
#ifndef KK_MAINTENANCE11_MULTIPLANE_H
#define KK_MAINTENANCE11_MULTIPLANE_H

#ifdef KK_MAINTENANCE11_DESCRIPTOR
static void
kk_alias_sampled_tag(struct kk_sampled_image_descriptor *desc,
                      const struct kk_image_view *view, bool input_attachment)
{
   /* The LOD assignment precedes this publication. Preserve an independent
    * sparse query-pointer tag and all numerical LOD bits. */
   desc->image_min_lod_uint16 &= ~KK_IMAGE_NATIVE_ARRAY_ALIAS_TAG;
   if (view->plane_count == 1 && kk_alias_view_is_1d_or_2d(view) &&
       !kk_alias_has_writable_msaa(view) && desc->aux_resource_id &&
       (input_attachment || kk_alias_view_is_array(view) ||
        view->planes[0].sampled_alias_gpu_resource_id))
      desc->image_min_lod_uint16 |= KK_IMAGE_NATIVE_ARRAY_ALIAS_TAG;
}
#else
static nir_def *
kk_alias_sampled_address(nir_builder *b, const nir_tex_instr *tex,
                         nir_deref_instr *deref, nir_def *primary_slot_address,
                         unsigned image_plane_offset_B, unsigned relative_offs,
                         const struct lower_descriptors_ctx *ctx)
{
   if (!tex->is_array || !kk_alias_array_dimension(tex->sampler_dim) ||
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

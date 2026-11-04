/* SPDX-License-Identifier: MIT */
/* Mesa's vk_image_view_init normalizes the storage-only slice range, also
 * consumed by NVK and panvk. Metal retains the full native 3D texture. */
#include <stddef.h>
#include "kk_sparse.h"
static_assert(offsetof(struct kk_storage_image_descriptor, texel_offset) ==
              offsetof(struct kk_storage_image_descriptor, image_gpu_resource_id) +
              sizeof(uint64_t), "slice offset follows native texture ID");
static_assert(offsetof(struct kk_storage_image_descriptor, texel_count) ==
              offsetof(struct kk_storage_image_descriptor, texel_offset) +
              sizeof(uint32_t), "slice range occupies adjacent metadata words");
#ifdef KK_SLICED_3D_DESCRIPTOR
static inline void
kk_sliced_3d_descriptor(struct kk_storage_image_descriptor *desc,
                       const struct vk_image_view *view)
{
   if (view->view_type != VK_IMAGE_VIEW_TYPE_3D)
      return;

   /* The existing texel-buffer words are unused by 3D storage images. */
   desc->texel_offset = view->storage.z_slice_offset;
   desc->texel_count = view->storage.z_slice_count;
}
#else
static bool
kk_lower_sliced_3d_intrinsic(nir_builder *b, nir_intrinsic_instr *intr,
                            UNUSED void *data)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_bindless_image_load:
   case nir_intrinsic_bindless_image_sparse_load:
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap:
   case nir_intrinsic_bindless_image_size:
      break;
   default:
      return false;
   }
   if (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_3D)
      return false;

   /* Descriptor lowering retains the exact descriptor address as the handle
    * load's source, including dynamic indexing and descriptor robustness. */
   nir_intrinsic_instr *handle = nir_src_as_intrinsic(intr->src[0]);
   assert(handle && handle->intrinsic == nir_intrinsic_load_texture_handle_kk);
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *address = nir_iadd_imm(b, handle->src[0].ssa,
      offsetof(struct kk_storage_image_descriptor, texel_offset));
   nir_def *range = nir_build_load_global_constant(b, 2, 32, address,
      .align_mul = 8, .access = ACCESS_CAN_SPECULATE);
   range = kk_nir_storage_sparse_slice_range(b, range);
   nir_def *count = nir_channel(b, range, 1);
   if (intr->intrinsic == nir_intrinsic_bindless_image_size) {
      b->cursor = nir_after_instr(&intr->instr);
      nir_def *size = nir_vector_insert_imm(b, &intr->def, count, 2);
      nir_def_rewrite_uses_after(&intr->def, size);
   } else {
      nir_def *coord = intr->src[1].ssa;
      nir_def *z = nir_channel(b, coord, 2);
      nir_def *shifted = nir_bcsel(b, nir_ult(b, z, count),
         nir_iadd(b, z, nir_channel(b, range, 0)), nir_imm_int(b, -1));
      nir_src_rewrite(&intr->src[1],
                     nir_vector_insert_imm(b, coord, shifted, 2));
   }
   return true;
}

static bool
kk_nir_lower_sliced_3d(nir_shader *nir)
{
   return nir_shader_intrinsics_pass(nir, kk_lower_sliced_3d_intrinsic,
                                     nir_metadata_none, NULL);
}
#endif

/* SPDX-License-Identifier: MIT */
#ifndef KK_MAINTENANCE11_IMAGE_ALIAS_HELPERS_H
#define KK_MAINTENANCE11_IMAGE_ALIAS_HELPERS_H

#if defined(KK_MAINTENANCE11_VIEW) || defined(KK_MAINTENANCE11_DESCRIPTOR)
/* The actual image, including narrowed sampled-only views, owns writable MSAA
 * representation. This is the kk_writable_msaa_requested image predicate. */
static bool
kk_alias_has_writable_msaa(const struct kk_image_view *view)
{
   return view->vk.image->samples > 1 &&
          (view->vk.image->usage & VK_IMAGE_USAGE_STORAGE_BIT);
}
#endif

#ifdef KK_MAINTENANCE11_VIEW
static bool
kk_image_view_alias_eligible(const struct kk_image_view *view)
{
   return (view->vk.image->create_flags &
           VK_IMAGE_CREATE_ALIAS_SINGLE_LAYER_DESCRIPTOR_BIT_KHR) &&
          view->vk.layer_count == 1 && !kk_alias_has_writable_msaa(view) &&
          (view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D ||
           view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
           view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D ||
           view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
}

static VkResult
kk_image_view_init_alias(struct kk_device *dev, struct kk_image_view *view, uint8_t plane,
                         mtl_texture *image, const struct kk_view_layout *layout)
{
   if (!kk_image_view_alias_eligible(view))
      return VK_SUCCESS;
   struct kk_view_layout alias = *layout;
   bool array = view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
                view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   alias.view_type = array ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   if (view->vk.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
      view->planes[plane].mtl_handle_sampled_alias =
         mtl_new_texture_view_with(image, &alias);
      if (!view->planes[plane].mtl_handle_sampled_alias)
         return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      view->planes[plane].sampled_alias_gpu_resource_id =
         mtl_texture_get_gpu_resource_id(view->planes[plane].mtl_handle_sampled_alias);
      VkResult result = kk_sparse_image_view_query_aliases(dev, view, image,
                                                         layout, false, array);
      if (result != VK_SUCCESS)
         return result;
   }
   if (view->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT) {
      view->planes[plane].mtl_handle_storage_alias =
         mtl_new_texture_view_with(image, &alias);
      if (!view->planes[plane].mtl_handle_storage_alias)
         return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      view->planes[plane].storage_alias_gpu_resource_id =
         mtl_texture_get_gpu_resource_id(view->planes[plane].mtl_handle_storage_alias);
      VkResult result = kk_sparse_image_view_query_aliases(dev, view, image,
                                                         layout, true, array);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

static void
kk_image_view_finish_alias(struct kk_image_view *view, uint8_t plane)
{
   if (view->planes[plane].mtl_handle_sampled_alias)
      mtl_release(view->planes[plane].mtl_handle_sampled_alias);
   if (view->planes[plane].mtl_handle_storage_alias)
      mtl_release(view->planes[plane].mtl_handle_storage_alias);
}
#elif defined(KK_MAINTENANCE11_DESCRIPTOR)
static bool
kk_alias_view_is_array(const struct kk_image_view *view)
{
   return view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
          view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY;
}

static bool
kk_alias_view_is_1d_or_2d(const struct kk_image_view *view)
{
   return kk_alias_view_is_array(view) ||
          view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D ||
          view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D;
}

static void
kk_alias_sampled_descriptor(struct kk_sampled_image_descriptor *desc,
                            const struct kk_image_view *view, uint8_t plane,
                            bool input_attachment)
{
   /* Whole multi-plane views retain the primary/plane1/plane2 chain.
    * An individually selected plane is still an ordinary single-plane view. */
   if (view->plane_count != 1)
      return;
   /* Writable MSAA owns aux48/table and aux56/sample-layer metadata. */
   if (kk_alias_has_writable_msaa(view))
      return;
   if (view->vk.image->samples > 1)
      desc->aux_word = 0;
   /* Cube face emulation owns the same auxiliary slot for cube descriptors. */
   if (!kk_alias_view_is_1d_or_2d(view))
      return;
   if (input_attachment) {
      desc->aux_resource_id = desc->image_gpu_resource_id;
      return;
   }
   uint64_t alternate = view->planes[plane].sampled_alias_gpu_resource_id;
   if (!alternate) {
      desc->aux_resource_id = desc->image_gpu_resource_id;
   } else if (kk_alias_view_is_array(view)) {
      desc->aux_resource_id = desc->image_gpu_resource_id;
      desc->image_gpu_resource_id = alternate;
   } else {
      desc->aux_resource_id = alternate;
   }
}

static void
kk_alias_storage_descriptor(struct kk_storage_image_descriptor *desc,
                            const struct kk_image_view *view)
{
   if (!kk_alias_view_is_1d_or_2d(view) || kk_alias_has_writable_msaa(view))
      return;
   uint64_t alternate = view->planes[0].storage_alias_gpu_resource_id;
   if (!alternate) {
      desc->image_array_gpu_resource_id = desc->image_gpu_resource_id;
   } else if (kk_alias_view_is_array(view)) {
      desc->image_array_gpu_resource_id = desc->image_gpu_resource_id;
      desc->image_gpu_resource_id = alternate;
   } else {
      desc->image_array_gpu_resource_id = alternate;
   }
   kk_sparse_storage_descriptor_aliases(view, desc->image_gpu_resource_id,
                                        desc->image_array_gpu_resource_id);
}
#else
static bool
kk_alias_array_dimension(enum glsl_sampler_dim dim)
{
   return dim == GLSL_SAMPLER_DIM_1D || dim == GLSL_SAMPLER_DIM_2D ||
          dim == GLSL_SAMPLER_DIM_MS;
}



/* Linear Metal 2D-array views support reads/writes but misaddress atomics.
 * An aliased descriptor has distinct plain/array handles. Use its plain handle
 * for array atomics while preserving the single-layer bounds contract. */
static bool
kk_alias_lower_array_atomic(nir_builder *b, nir_intrinsic_instr *intr,
                            nir_deref_instr *deref,
                            const struct lower_descriptors_ctx *ctx,
                            nir_alu_type type, unsigned access, unsigned flags)
{
   if (!nir_intrinsic_image_array(intr) ||
       (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_1D &&
        nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_2D) ||
       (intr->intrinsic != nir_intrinsic_bindless_image_atomic &&
        intr->intrinsic != nir_intrinsic_bindless_image_atomic_swap))
      return false;

   nir_def *descriptor = load_resource_deref_desc(
      b, 2, 64, deref,
      offsetof(struct kk_storage_image_descriptor, image_gpu_resource_id), ctx);
   nir_def *descriptor_addr = load_resource_addr(
      b, 1, 64, deref,
      offsetof(struct kk_storage_image_descriptor, image_gpu_resource_id), ctx);
   /* The sparse accessor distinguishes both zero words from an indirect
    * primary-zero/query descriptor; native IDs retain all opaque bits. */
   nir_def *plain_addr = kk_nir_storage_sparse_handle_address(
      b, descriptor_addr, descriptor, false);
   nir_def *array_addr = kk_nir_storage_sparse_handle_address(
      b, descriptor_addr, descriptor, true);
   nir_def *plain_id = nir_load_global_constant(b, 1, 64, plain_addr, .align_mul = 8);
   nir_def *array_id = nir_load_global_constant(b, 1, 64, array_addr, .align_mul = 8);
   nir_push_if(b, nir_ine(b, plain_id, array_id));
   nir_def *plain_handle = nir_load_texture_handle_kk(
      b, 1, 64, plain_addr, .dest_type = type,
      .image_dim = nir_intrinsic_image_dim(intr), .image_array = false,
      .access = access, .flags = flags);
   nir_intrinsic_instr *plain = nir_instr_as_intrinsic(
      nir_instr_clone(b->shader, &intr->instr));
   nir_intrinsic_set_image_array(plain, false);
   unsigned dimensions = nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_1D ? 1 : 2;
   nir_def *coord = intr->src[1].ssa;
   nir_def *in_layer = nir_ieq_imm(b, nir_channel(b, coord, dimensions), 0);
   nir_def *components[4];
   for (unsigned i = 0; i < 4; i++)
      components[i] = i < dimensions
         ? nir_bcsel(b, in_layer, nir_channel(b, coord, i), nir_imm_int(b, -1))
         : nir_imm_int(b, 0);
   nir_def *plain_coord = nir_vec(b, components, 4);
   nir_builder_instr_insert(b, &plain->instr);
   nir_src_rewrite(&plain->src[0], plain_handle);
   nir_src_rewrite(&plain->src[1], plain_coord);
   nir_push_else(b, NULL);
   nir_intrinsic_instr *array = nir_instr_as_intrinsic(
      nir_instr_clone(b->shader, &intr->instr));
   nir_builder_instr_insert(b, &array->instr);
   nir_pop_if(b, NULL);
   nir_def *result = nir_if_phi(b, &plain->def, &array->def);
   nir_def_replace(&intr->def, result);
   return true;
}

#endif
#endif

/* SPDX-License-Identifier: MIT */
#ifndef KK_WRITABLE_MSAA_NIR_H
#define KK_WRITABLE_MSAA_NIR_H
#include "kk-writable-msaa-layout.h"

/* Included after the existing descriptor-load helpers. DIM_MS storage records
 * use the same 16-byte ABI: a GPU address of sample resource IDs, then uint32
 * sample count and logical view-layer count. Other dimensions retain their ABI.
 * A table entry is never loaded until the sample and pointer have been checked.
 * load_texture_handle_kk has CAN_ELIMINATE but not CAN_REORDER, so these loads
 * cannot be speculated outside the checked control flow.
 */
static nir_def *
kk_msaa_handle(nir_builder *b, nir_def *table, nir_def *sample,
               nir_alu_type type, enum gl_access_qualifier access,
               unsigned flags)
{
   nir_def *entry = nir_iadd(b, table,
      nir_imul_imm(b, nir_u2u64(b, sample), KK_WRITABLE_MSAA_ENTRY_SIZE_B));
   return nir_load_texture_handle_kk(b, 1, 64, entry,
      .dest_type = type, .image_dim = GLSL_SAMPLER_DIM_2D,
      .image_array = true, .access = access, .flags = flags);
}

/* Rejected color reads may inspect only the bounded, immutable footer. In
 * particular this path never loads a resource ID, and null metadata loads none.
 */
static nir_def *
kk_msaa_read_default(nir_builder *b, nir_def *table, nir_def *samples,
                     nir_def *layers, nir_alu_type type, unsigned components,
                     unsigned bits, bool sampled)
{
   nir_def *present = nir_iand(b, nir_ine_imm(b, table, 0),
      nir_ior(b, nir_ieq_imm(b, samples, 2), nir_ieq_imm(b, samples, 4)));
   present = nir_iand(b, present, nir_ugt_imm(b, layers, 0));
   nir_push_if(b, present);
   nir_def *addr = nir_iadd(b, table, nir_imul_imm(b, nir_u2u64(b, samples),
                                                KK_WRITABLE_MSAA_ENTRY_SIZE_B));
   nir_def *defaults = nir_load_global_constant(b, 2, 32, addr,
      .align_mul = 8, .access = 0);
   nir_def *mask = nir_channel(b, defaults, sampled ?
      offsetof(struct kk_writable_msaa_defaults, sampled_default_one_mask) / 4 :
      offsetof(struct kk_writable_msaa_defaults, storage_alpha_one) / 4);
   nir_def *one = nir_alu_type_get_base_type(type) == nir_type_float
                    ? nir_imm_floatN_t(b, 1, bits) : nir_imm_intN_t(b, 1, bits);
   nir_def *zero = nir_imm_intN_t(b, 0, bits);
   nir_def *color[5];
   assert(components <= ARRAY_SIZE(color));
   for (unsigned c = 0; c < components; c++) {
      /* Sparse residency is not a color component. */
      if (c >= 4 || (!sampled && c != 3)) {
         color[c] = zero;
      } else {
         nir_def *is_one = sampled ? nir_test_mask(b, mask, 1u << c)
                                   : nir_ine_imm(b, mask, 0);
         color[c] = nir_bcsel(b, is_one, one, zero);
      }
   }
   nir_def *value = nir_vec(b, color, components);
   nir_push_else(b, NULL);
   nir_def *null_value = nir_imm_zero(b, components, bits);
   nir_pop_if(b, NULL);
   return nir_if_phi(b, value, null_value);
}

static bool
kk_lower_msaa_storage(nir_builder *b, nir_intrinsic_instr *intr,
                      const struct lower_descriptors_ctx *ctx)
{
   if (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_MS)
      return false;
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_sparse_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples:
   case nir_intrinsic_image_deref_levels:
      break;
   default:
      return false;
   }

   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_def *metadata = load_resource_deref_desc(b, 2, 32, deref,
      offsetof(struct kk_storage_image_descriptor, texel_offset), ctx);
   nir_def *samples = nir_channel(b, metadata, 0);
   nir_def *layers = nir_channel(b, metadata, 1);
   nir_def *table = load_resource_deref_desc(b, 1, 64, deref,
      offsetof(struct kk_storage_image_descriptor, image_gpu_resource_id), ctx);
   nir_def *present = nir_iand(b, nir_ine_imm(b, table, 0),
      nir_ior(b, nir_ieq_imm(b, samples, 2), nir_ieq_imm(b, samples, 4)));
   present = nir_iand(b, present, nir_ugt_imm(b, layers, 0));

   if (intr->intrinsic == nir_intrinsic_image_deref_samples ||
       intr->intrinsic == nir_intrinsic_image_deref_levels) {
      nir_def *value = intr->intrinsic == nir_intrinsic_image_deref_samples
                         ? samples : nir_imm_int(b, 1);
      nir_def_replace(&intr->def,
         nir_bcsel(b, present, value, nir_imm_int(b, 0)));
      return true;
   }

   const bool size = intr->intrinsic == nir_intrinsic_image_deref_size;
   const bool store = intr->intrinsic == nir_intrinsic_image_deref_store;
   const bool atomic = nir_intrinsic_has_atomic_op(intr);
   const bool arrayed = nir_intrinsic_image_array(intr);
   const unsigned components = store ? 0 : intr->def.num_components;
   const unsigned bits = store ? 0 : intr->def.bit_size;
   nir_def *sample = size ? nir_imm_int(b, 0) : intr->src[2].ssa;
   nir_def *layer = size || !arrayed ? nir_imm_int(b, 0)
                                    : nir_channel(b, intr->src[1].ssa, 2);
   nir_def *valid = nir_iand(b, present, nir_ult(b, sample, samples));
   if (!size)
      valid = nir_iand(b, valid, nir_ult(b, layer, layers));

   nir_alu_type type = nir_type_uint32;
   if (atomic)
      type = nir_atomic_op_type(nir_intrinsic_atomic_op(intr)) |
             intr->src[3].ssa->bit_size;
   else if (nir_intrinsic_has_dest_type(intr))
      type = nir_intrinsic_dest_type(intr);
   else if (nir_intrinsic_has_src_type(intr))
      type = nir_intrinsic_src_type(intr);

   nir_variable *var = nir_deref_instr_get_variable(deref);
   const enum gl_access_qualifier variable_access = var ? var->data.access : 0;
   const enum gl_access_qualifier access = variable_access |
      (nir_intrinsic_has_access(intr) ? nir_intrinsic_access(intr) : 0);
   const unsigned flags = atomic || nir_alu_type_get_type_size(type) == 64
                             ? MSL_ACCESS_READ_WRITE
                             : msl_convert_access_flag(access);
   nir_push_if(b, valid);
   nir_def *handle = kk_msaa_handle(b, table, sample, type, access, flags);
   nir_intrinsic_instr *physical = nir_instr_as_intrinsic(
      nir_instr_clone(b->shader, &intr->instr));
   nir_intrinsic_set_image_dim(physical, GLSL_SAMPLER_DIM_2D);
   nir_intrinsic_set_image_array(physical, true);
   if (size) {
      physical->num_components = 3;
      physical->def.num_components = 3;
   }
   nir_builder_instr_insert(b, &physical->instr);
   b->cursor = nir_before_instr(&physical->instr);
   if (!size) {
      nir_def *coord = nir_vec4(b, nir_channel(b, intr->src[1].ssa, 0),
         nir_channel(b, intr->src[1].ssa, 1), layer, nir_imm_int(b, 0));
      nir_src_rewrite(&physical->src[1], coord);
      nir_src_rewrite(&physical->src[2], nir_imm_int(b, 0));
      if (store)
         nir_src_rewrite(&physical->src[4], nir_imm_int(b, 0));
      else if (!atomic)
         nir_src_rewrite(&physical->src[3], nir_imm_int(b, 0));
   }
   if (!var)
      nir_src_rewrite(&physical->src[0], nir_imm_int(b, 0));
   nir_rewrite_image_intrinsic(physical, handle,
                              nir_image_intrinsic_type_bindless);
   b->cursor = nir_after_instr(&physical->instr);
   nir_def *value = NULL;
   if (!store) {
      value = &physical->def;
      if (size) {
         value = nir_vec3(b, nir_channel(b, value, 0),
            nir_channel(b, value, 1), layers);
         value = nir_trim_vector(b, value, components);
      }
      nir_push_else(b, NULL);
   }
   nir_def *zero = store ? NULL : !atomic && !size && ctx->robust_msaa_defaults
      ? kk_msaa_read_default(b, table, samples, layers, type, components, bits, false)
      : nir_imm_zero(b, components, bits);
   nir_pop_if(b, NULL);
   if (store)
      nir_instr_remove(&intr->instr);
   else
      nir_def_replace(&intr->def, nir_if_phi(b, value, zero));
   return true;
}

static nir_tex_instr *
kk_msaa_fetch_clone(nir_builder *b, const nir_tex_instr *tex, nir_def *handle,
                    bool physical_array)
{
   nir_tex_instr *fetch = nir_instr_as_tex(
      nir_instr_clone(b->shader, &tex->instr));
   fetch->can_speculate = false;
   nir_builder_instr_insert(b, &fetch->instr);
   b->cursor = nir_before_instr(&fetch->instr);
   nir_tex_instr_add_src(fetch, nir_tex_src_texture_handle, handle);
   if (physical_array && tex->op == nir_texop_txs) {
      fetch->sampler_dim = GLSL_SAMPLER_DIM_2D;
      fetch->is_array = true;
      fetch->def.num_components = 3;
      int lod = nir_tex_instr_src_index(fetch, nir_tex_src_lod);
      if (lod >= 0)
         nir_src_rewrite(&fetch->src[lod].src, nir_imm_int(b, 0));
      else
         nir_tex_instr_add_src(fetch, nir_tex_src_lod, nir_imm_int(b, 0));
   } else if (physical_array) {
      int coord_src = nir_tex_instr_src_index(fetch, nir_tex_src_coord);
      nir_def *old = fetch->src[coord_src].src.ssa;
      nir_def *layer = tex->is_array ? nir_channel(b, old, 2)
                                    : nir_imm_int(b, 0);
      nir_def *coord = nir_vec3(b, nir_channel(b, old, 0),
                                nir_channel(b, old, 1), layer);
      nir_src_rewrite(&fetch->src[coord_src].src, coord);
      nir_tex_instr_remove_src(fetch,
         nir_tex_instr_src_index(fetch, nir_tex_src_ms_index));
      nir_tex_instr_add_src(fetch, nir_tex_src_lod, nir_imm_int(b, 0));
      fetch->op = nir_texop_txf;
      fetch->sampler_dim = GLSL_SAMPLER_DIM_2D;
      fetch->is_array = true;
      fetch->coord_components = 3;
   }
   b->cursor = nir_after_instr(&fetch->instr);
   return fetch;
}

static bool
kk_lower_msaa_fetch(nir_builder *b, nir_tex_instr *tex, nir_def *texture,
                    const struct lower_descriptors_ctx *ctx)
{
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_MS || tex->is_shadow ||
       (tex->op != nir_texop_txf_ms && tex->op != nir_texop_txs &&
        tex->op != nir_texop_texture_samples))
      return false;

   nir_deref_instr *deref = nir_src_as_deref(nir_src_for_ssa(texture));
   /* Input-attachment descriptors always name the native MSAA image, even
    * when storage uses separate sample planes. Their views are synchronized
    * before the subpass; they never publish the auxiliary sample table.
    * Keep these reads on the ordinary native path instead of introducing
    * storage-image selection into a pixel-frequency input-attachment loop. */
   if (glsl_get_sampler_dim(deref->type) == GLSL_SAMPLER_DIM_SUBPASS_MS)
      return false;

   nir_def *table = load_resource_deref_desc(b, 1, 64, deref,
      offsetof(struct kk_sampled_image_descriptor, aux_resource_id), ctx);
   nir_push_if(b, nir_ine_imm(b, table, 0));
   nir_def *loaded_metadata = load_resource_deref_desc(b, 2, 32, deref,
      offsetof(struct kk_sampled_image_descriptor, aux_word), ctx);
   nir_push_else(b, NULL);
   nir_def *empty_metadata = nir_imm_zero(b, 2, 32);
   nir_pop_if(b, NULL);
   nir_def *metadata = nir_if_phi(b, loaded_metadata, empty_metadata);
   nir_def *samples = nir_channel(b, metadata, 0);
   nir_def *layers = nir_channel(b, metadata, 1);
   nir_def *table_present = nir_iand(b, nir_ine_imm(b, table, 0),
      nir_ior(b, nir_ieq_imm(b, samples, 2), nir_ieq_imm(b, samples, 4)));
   table_present = nir_iand(b, table_present, nir_ugt_imm(b, layers, 0));
   const bool fetch = tex->op == nir_texop_txf_ms;
   nir_def *sample = fetch ? tex->src[
      nir_tex_instr_src_index(tex, nir_tex_src_ms_index)].src.ssa
                            : nir_imm_int(b, 0);
   nir_def *layer = nir_imm_int(b, 0);
   if (fetch && tex->is_array) {
      nir_def *coord = tex->src[
         nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa;
      layer = nir_channel(b, coord, 2);
   }

   /* Native M11 descriptors may use aux48 for an array texture ID, with zero
    * aux56. Only valid sample/layer metadata denotes a writable sample table.
    */
   nir_push_if(b, table_present);
   nir_def *valid = nir_iand(b, nir_ine_imm(b, table, 0),
                              nir_ult(b, sample, samples));
   valid = nir_iand(b, valid, nir_ult(b, layer, layers));
   nir_push_if(b, valid);
   nir_def *array_result;
   if (tex->op == nir_texop_texture_samples) {
      array_result = samples;
   } else {
      nir_def *array_handle = kk_msaa_handle(b, table, sample, tex->dest_type,
         ACCESS_NON_WRITEABLE, MSL_ACCESS_READ);
      nir_tex_instr *array_fetch = kk_msaa_fetch_clone(b, tex, array_handle, true);
      array_result = &array_fetch->def;
      if (tex->op == nir_texop_txs) {
         array_result = nir_vec3(b, nir_channel(b, array_result, 0),
            nir_channel(b, array_result, 1), layers);
         array_result = nir_trim_vector(b, array_result, tex->def.num_components);
      }
   }
   nir_push_else(b, NULL);
   nir_def *zero = fetch && ctx->robust_msaa_defaults
      ? kk_msaa_read_default(b, table, samples, layers, tex->dest_type,
                            tex->def.num_components, tex->def.bit_size, true)
      : nir_imm_zero(b, tex->def.num_components, tex->def.bit_size);
   nir_pop_if(b, NULL);
   nir_def *array_value = nir_if_phi(b, array_result, zero);
   nir_push_else(b, NULL);
   /* Nonzero malformed metadata is neither a native record nor a table. */
   nir_push_if(b, nir_iand(b, nir_ieq_imm(b, samples, 0),
                            nir_ieq_imm(b, layers, 0)));
   nir_def *native_addr = load_resource_addr(b, 1, 64, deref,
      offsetof(struct kk_sampled_image_descriptor, image_gpu_resource_id), ctx);
   if (tex->is_array) {
      nir_def *array_addr = load_resource_addr(b, 1, 64, deref,
         offsetof(struct kk_sampled_image_descriptor, aux_resource_id), ctx);
      native_addr = nir_bcsel(b, nir_ine_imm(b, table, 0), array_addr, native_addr);
   }
   nir_def *native_handle = nir_load_texture_handle_kk(b, 1, 64, native_addr,
      .dest_type = tex->dest_type, .image_dim = tex->sampler_dim,
      .image_array = tex->is_array, .flags = MSL_ACCESS_READ);
   nir_tex_instr *native_fetch = kk_msaa_fetch_clone(b, tex, native_handle, false);
   nir_push_else(b, NULL);
   nir_def *invalid_value = nir_imm_zero(b, tex->def.num_components, tex->def.bit_size);
   nir_pop_if(b, NULL);
   nir_def *native_value = nir_if_phi(b, &native_fetch->def, invalid_value);
   nir_pop_if(b, NULL);
   nir_def_replace(&tex->def, nir_if_phi(b, array_value, native_value));
   return true;
}
#endif

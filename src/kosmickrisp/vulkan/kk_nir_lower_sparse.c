/* SPDX-License-Identifier: MIT
 * Sparse color/status splitting follows Mesa's Zink residency lowering.
 * Copyright 2020 Collabora Ltd.
 */
#include "kk_shader.h"
#include "kk_descriptor_types.h"
#include "kk_sparse.h"
#include "compiler/nir/nir_builder.h"

nir_def *
kk_nir_query_record_kind(nir_builder *b, nir_def *record_address)
{
   nir_if *present = nir_push_if(b, nir_ine_imm(b, record_address, 0));
   nir_def *kind = nir_load_global_constant(b, 1, 32,
      nir_iadd_imm(b, record_address, offsetof(struct kk_sparse_query_record, kind)),
      .align_mul = 4);
   nir_push_else(b, present);
   nir_def *native = nir_imm_int(b, KK_QUERY_RECORD_KIND_NATIVE);
   nir_pop_if(b, present);
   return nir_if_phi(b, kind, native);
}

nir_def *
kk_nir_storage_sparse_query_decode(nir_builder *b, nir_def *metadata)
{
   return nir_bcsel(b, nir_ine_imm(b, nir_ushr_imm(b, metadata, 63), 0),
      nir_iand_imm(b, metadata, ~KK_STORAGE_SPARSE_QUERY_TAG), nir_imm_int64(b, 0));
}

nir_def *
kk_nir_storage_sparse_descriptor_query(nir_builder *b, nir_def *descriptor)
{
   nir_def *primary = nir_channel(b, descriptor, 0);
   nir_def *metadata = nir_channel(b, descriptor, 1);
   nir_def *indirect = nir_iand(b, nir_ieq_imm(b, primary, 0),
                                  nir_ine_imm(b, metadata, 0));
   return nir_bcsel(b, indirect, kk_nir_storage_sparse_query_decode(b, metadata),
                    nir_imm_int64(b, 0));
}

nir_def *
kk_nir_storage_sparse_handle_address(nir_builder *b, nir_def *descriptor_address,
                                    nir_def *descriptor, bool array)
{
   nir_def *record = kk_nir_storage_sparse_descriptor_query(b, descriptor);
   unsigned offset = array ? offsetof(struct kk_sparse_query_record, array_resource_id) :
                             offsetof(struct kk_sparse_query_record, plain_resource_id);
   nir_def *ordinary_address = descriptor_address;
   if (array)
      ordinary_address = nir_bcsel(b, nir_ine_imm(b, nir_channel(b, descriptor, 1), 0),
         nir_iadd_imm(b, descriptor_address, 8), descriptor_address);
   return nir_bcsel(b, nir_ine_imm(b, record, 0),
                    nir_iadd_imm(b, record, offset), ordinary_address);
}

nir_def *
kk_nir_storage_sparse_slice_range(nir_builder *b, nir_def *range)
{
   nir_def *metadata = nir_pack_64_2x32(b, range);
   nir_def *record = kk_nir_storage_sparse_query_decode(b, metadata);
   nir_if *sparse = nir_push_if(b, nir_ine_imm(b, record, 0));
   nir_def *slices = nir_load_global_constant(b, 2, 32,
      nir_iadd_imm(b, record, 8), .align_mul = 8);
   nir_push_else(b, sparse);
   nir_pop_if(b, sparse);
   return nir_if_phi(b, slices, range);
}

nir_def *
kk_nir_sparse_view_address(nir_builder *b, nir_def *address,
                            nir_def *query_record, nir_def **lod, bool storage, bool query_array)
{
   nir_def *has_record = nir_ine_imm(b, query_record, 0);
   nir_if *query = nir_push_if(b, has_record);
   nir_def *base = nir_load_global_constant(b, 1, 32,
      nir_iadd_imm(b, query_record, storage ? 64 : 8), .align_mul = 4);
   nir_def *levels = nir_load_global_constant(b, 1, 32,
      nir_iadd_imm(b, query_record, storage ? 68 : 12), .align_mul = 4);
   nir_def *parent_lod = nir_bcsel(b, nir_ult(b, *lod, levels),
      nir_iadd(b, *lod, base), nir_imm_int(b, UINT32_MAX));
   nir_push_else(b, query);
   nir_pop_if(b, query);
   *lod = nir_if_phi(b, parent_lod, *lod);
   nir_def *query_address = query_array ?
      nir_iadd_imm(b, query_record, offsetof(struct kk_sparse_query_record, array_query_resource_id)) :
      query_record;
   return nir_bcsel(b, has_record, query_address, address);
}

nir_def *
kk_nir_sparse_buffer_resident(nir_builder *b, nir_def *metadata, nir_def *coord)
{
   nir_def *address = nir_pack_64_2x32_split(b, nir_channel(b, metadata, 0),
                                          nir_channel(b, metadata, 1));
   nir_def *x = nir_channel(b, coord, 0);
   nir_def *valid = nir_ine_imm(b, x, UINT32_MAX);
   nir_if *check = nir_push_if(b, nir_iand(b, valid, nir_ine_imm(b, address, 0)));
   nir_def *kind_size = nir_channel(b, metadata, 3);
   nir_def *size = nir_u2u64(b, nir_iand_imm(b, kind_size, ~KK_TEXEL_BUFFER_RGB32));
   nir_def *span = nir_bcsel(b, nir_ine_imm(b, nir_iand_imm(b, kind_size, KK_TEXEL_BUFFER_RGB32), 0),
                            nir_imul_imm(b, size, 3), size);
   nir_def *offset = nir_iadd(b, nir_imul(b, nir_u2u64(b, x), size),
                             nir_u2u64(b, nir_channel(b, metadata, 2)));
   nir_def *first = nir_ushr_imm(b, offset, 16);
   nir_def *last = nir_ushr_imm(b, nir_iadd(b, offset, nir_iadd_imm(b, span, -1)), 16);
   nir_def *first_state = nir_load_global_constant(b, 1, 8, nir_iadd(b, address, first), .align_mul = 1);
   nir_def *last_state = nir_load_global_constant(b, 1, 8, nir_iadd(b, address, last), .align_mul = 1);
   nir_def *resident = nir_iand(b, nir_ine_imm(b, first_state, 0), nir_ine_imm(b, last_state, 0));
   nir_push_else(b, check);
   nir_pop_if(b, check);
   return nir_if_phi(b, resident, valid);
}

static bool
lower_sparse(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   nir_def *def = NULL;
   nir_def *resident = NULL;
   b->cursor = nir_after_instr(instr);
   if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      if (!tex->is_sparse) return false;
      if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF) {
         int source = nir_tex_instr_src_index(tex, nir_tex_src_backend1);
         assert(source >= 0);
         resident = nir_ine_imm(b, tex->src[source].src.ssa, 0);
         nir_tex_instr_remove_src(tex, source);
      }
      tex->is_sparse = false;
      def = &tex->def;
   } else if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      switch (intr->intrinsic) {
      case nir_intrinsic_bindless_image_sparse_load:
         if (nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_BUF)
            resident = nir_ine_imm(b, intr->src[2].ssa, 0);
         intr->intrinsic = nir_intrinsic_bindless_image_load;
         intr->num_components--;
         def = &intr->def;
         break;
      case nir_intrinsic_sparse_residency_code_and:
         nir_def_rewrite_uses(&intr->def,
            nir_ior(b, intr->src[0].ssa, intr->src[1].ssa));
         nir_instr_remove(instr);
         return true;
      case nir_intrinsic_is_sparse_texels_resident:
         nir_def_rewrite_uses(&intr->def, nir_ieq_imm(b, intr->src[0].ssa, 0));
         nir_instr_remove(instr);
         return true;
      default:
         return false;
      }
   } else {
      return false;
   }

   /* The intrinsic consumes the color instruction directly, keeping the
    * native sparse result live without a five-component MSL vector. */
   unsigned colors = --def->num_components;
   nir_def *parts[5];
   for (unsigned i = 0; i < colors; ++i)
      parts[i] = nir_channel(b, def, i);
   if (!resident) resident = nir_is_sparse_resident_zink(b, def);
   parts[colors] = nir_b2i32(b, nir_inot(b, resident));
   nir_def *result = nir_vec(b, parts, colors + 1);
   nir_def_rewrite_uses_after(def, result);
   return true;
}

bool
kk_nir_lower_sparse(nir_shader *nir)
{
   bool progress = nir_shader_instructions_pass(nir, lower_sparse,
                                               nir_metadata_control_flow, NULL);
   /* Null-image lowering may have made a vec5 phi. Split it before register
    * lowering; normal shaders retain the existing optimization path. */
   if (progress) {
      NIR_PASS(_, nir, nir_lower_phis_to_scalar, NULL, NULL);
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_dce);
   }
   return progress;
}

/* SPDX-License-Identifier: MIT */
/* Included after the common set/heap descriptor-address helpers. The selected
 * descriptor is routed once, before any sparse/native/alias/null reader. */
#include "kk_layered_bc.h"

static nir_def *
kk_layered_bc_route(nir_builder *b, nir_def *original, nir_def *record,
                    nir_def *layer, bool sampled, bool integer_layer)
{
   nir_def *kind = kk_nir_query_record_kind(b, record);
   nir_push_if(b, nir_ieq_imm(b, kind, KK_QUERY_RECORD_KIND_LAYERED_BC));
   nir_def *count = nir_load_global_constant(b, 1, 32,
      nir_iadd_imm(b, record, offsetof(struct kk_layered_bc_record, layer_count)),
      .align_mul = 4);
   /* A real record always has layers. Guard both count and table pointer
    * before constructing an entry address, including malformed zero counts. */
   nir_def *table = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, record, sampled ?
         offsetof(struct kk_layered_bc_record, sampled_descriptors) :
         offsetof(struct kk_layered_bc_record, storage_descriptors)), .align_mul = 8);
   nir_push_if(b, nir_iand(b, nir_ine_imm(b, count, 0), nir_ine_imm(b, table, 0)));
   nir_def *index = nir_imm_int(b, 0);
   if (layer) {
      index = integer_layer ? layer : nir_f2i32(b, nir_ffloor(b,
         nir_fadd(b, layer, nir_imm_float(b, 0.5))));
      if (sampled && !integer_layer)
         index = nir_imin(b, nir_imax(b, index, nir_imm_int(b, 0)),
                          nir_iadd_imm(b, count, -1));
      /* Rejected integer layers select an owned zero descriptor, never an
       * address computed from the rejected coordinate. */
      index = nir_bcsel(b, nir_ult(b, index, count), index, count);
   }
   nir_def *selected = nir_iadd(b, table,
      nir_imul_imm(b, nir_u2u64(b, index), sampled ?
         sizeof(struct kk_sampled_image_descriptor) :
         sizeof(struct kk_storage_image_descriptor)));
   nir_def *routed = nir_vec2(b, selected, nir_u2u64(b, count));
   nir_push_else(b, NULL);
   nir_def *invalid = nir_vec2(b, original, nir_imm_int64(b, 0));
   nir_pop_if(b, NULL);
   nir_def *present = nir_if_phi(b, routed, invalid);
   nir_push_else(b, NULL);
   nir_def *ordinary = nir_vec2(b, original, nir_imm_int64(b, 0));
   nir_pop_if(b, NULL);
   return nir_if_phi(b, present, ordinary);
}

#include "kk_layered_bc_cube_nir.h"

static void
kk_layered_bc_size(nir_builder *b, nir_def *size, nir_def *count)
{
   b->cursor = nir_after_instr(nir_def_instr(size));
   const unsigned layer_component = size->num_components - 1;
   nir_def *layers = nir_bcsel(b, nir_ine_imm(b, count, 0),
      count, nir_channel(b, size, layer_component));
   nir_def *fixed = nir_vector_insert_imm(b, size, layers, layer_component);
   nir_def_rewrite_uses_after(size, fixed);
}

static bool
kk_layered_bc_instruction(nir_builder *b, nir_instr *instr, void *data)
{
   const struct lower_descriptors_ctx *ctx = data;
   b->cursor = nir_before_instr(instr);
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (strncmp(nir_intrinsic_infos[intr->intrinsic].name, "image_deref_", 12) ||
          !nir_intrinsic_has_image_dim(intr) ||
          (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_1D &&
           nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_2D) ||
          !nir_intrinsic_image_array(intr))
         return false;
      enum pipe_format format = nir_intrinsic_format(intr);
      if (format != PIPE_FORMAT_NONE &&
          util_format_get_blocksize(format) != 8 &&
          util_format_get_blocksize(format) != 16)
         return false;
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
      nir_def *address = load_resource_addr(b, 1, 64, deref, 0, ctx);
      nir_def *descriptor = load_resource_deref_desc(b, 2, 64, deref, 0, ctx);
      nir_def *secondary = nir_channel(b, descriptor, 1);
      nir_push_if(b, nir_iand(b, nir_ieq_imm(b, nir_channel(b, descriptor, 0), 0),
                                nir_ine_imm(b, secondary, 0)));
      nir_def *query = kk_nir_storage_sparse_query_decode(b, secondary);
      nir_push_else(b, NULL);
      nir_def *zero = nir_imm_int64(b, 0);
      nir_pop_if(b, NULL);
      nir_def *record = nir_if_phi(b, query, zero);
      bool coordinate = intr->intrinsic == nir_intrinsic_image_deref_load ||
         intr->intrinsic == nir_intrinsic_image_deref_sparse_load ||
         intr->intrinsic == nir_intrinsic_image_deref_store ||
         intr->intrinsic == nir_intrinsic_image_deref_atomic ||
         intr->intrinsic == nir_intrinsic_image_deref_atomic_swap;
      const unsigned layer_component =
         nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_1D ? 1 : 2;
      nir_def *layer = coordinate ?
         nir_channel(b, intr->src[1].ssa, layer_component) : NULL;
      nir_def *route = kk_layered_bc_route(b, address, record, layer, false, true);
      nir_def *count = nir_u2u32(b, nir_channel(b, route, 1));
      nir_def *active = nir_ine_imm(b, count, 0);
      nir_variable *var = nir_deref_instr_get_variable(deref);
      if (var && nir_intrinsic_has_access(intr))
         nir_intrinsic_set_access(intr, nir_intrinsic_access(intr) | var->data.access);
      nir_deref_instr *selected = nir_build_deref_cast(b, nir_channel(b, route, 0),
         nir_var_uniform, deref->type, 0);
      nir_src_rewrite(&intr->src[0], &selected->def);
      if (coordinate) {
         nir_def *at = intr->src[1].ssa;
         nir_src_rewrite(&intr->src[1], nir_vector_insert_imm(b, at,
            nir_bcsel(b, active, nir_imm_int(b, 0), layer), layer_component));
      }
      if (intr->intrinsic == nir_intrinsic_image_deref_size)
         kk_layered_bc_size(b, &intr->def, count);
      return true;
   }
   if (instr->type != nir_instr_type_tex) return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   /* Comparison-capable formats here are depth formats, which cannot be
    * block views of compressed color images. Keep their native descriptor. */
   if (tex->is_shadow || !tex->is_array ||
       (tex->sampler_dim != GLSL_SAMPLER_DIM_2D &&
        tex->sampler_dim != GLSL_SAMPLER_DIM_SUBPASS))
      return false;
   int texture_src = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   if (texture_src < 0) return false;
   int plane = nir_tex_instr_src_index(tex, nir_tex_src_plane);
   if (plane >= 0 && nir_src_as_uint(tex->src[plane].src) != 0)
      return false;
   nir_deref_instr *deref = nir_src_as_deref(tex->src[texture_src].src);
   if (glsl_get_sampler_dim(deref->type) == GLSL_SAMPLER_DIM_CUBE)
      return false;
   nir_def *address = load_resource_addr(b, 1, 64, deref, 0, ctx);
   nir_def *flags = load_resource_deref_desc(b, 1, 16, deref,
      offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16), ctx);
   nir_push_if(b, nir_ine_imm(b, nir_iand_imm(b, flags, KK_IMAGE_QUERY_RECORD_TAG), 0));
   nir_def *query = load_resource_deref_desc(b, 1, 64, deref,
      offsetof(struct kk_sampled_image_descriptor, aux_word), ctx);
   nir_push_else(b, NULL);
   nir_def *zero = nir_imm_int64(b, 0);
   nir_pop_if(b, NULL);
   nir_def *record = nir_if_phi(b, query, zero);
   int coordinate = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   nir_def *layer = coordinate >= 0 ? nir_channel(b, tex->src[coordinate].src.ssa, 2) : NULL;
   bool integer_layer = tex->op == nir_texop_txf;
   nir_def *route = kk_layered_bc_route(b, address, record, layer, true, integer_layer);
   nir_def *count = nir_u2u32(b, nir_channel(b, route, 1));
   nir_deref_instr *selected = nir_build_deref_cast(b, nir_channel(b, route, 0),
      nir_var_uniform, deref->type, 0);
   /* The original sampler deref is deliberately untouched. */
   nir_src_rewrite(&tex->src[texture_src].src, &selected->def);
   if (coordinate >= 0) {
      nir_def *at = tex->src[coordinate].src.ssa;
      nir_def *z = integer_layer ? nir_imm_int(b, 0) : nir_imm_float(b, 0);
      nir_src_rewrite(&tex->src[coordinate].src, nir_vector_insert_imm(b, at,
         nir_bcsel(b, nir_ine_imm(b, count, 0), z, layer), 2));
   }
   if (tex->op == nir_texop_txs) kk_layered_bc_size(b, &tex->def, count);
   return true;
}

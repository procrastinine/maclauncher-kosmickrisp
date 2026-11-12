/* SPDX-License-Identifier: MIT */
/* Storage-usage 2D residency images share the standard page table and
 * canonical page textures with standard-shaped 3D images. Only 2D image
 * instructions emit this translation, so 3D and non-image code is unchanged.
 * A resident tile selects its canonical page. The native sparse texture of
 * such an image has no mappings and is never read. */

static struct kk_standard_3d_predicate
kk_standard_2d_predicate(nir_builder *b, nir_def *query, nir_def *coord,
                         nir_def *lod, bool storage, bool parent_lod, bool array)
{
   nir_def *original_coord = coord, *original_lod = lod;
   nir_def *kind = kk_nir_query_record_kind(b, query);
   nir_if *standard = nir_push_if(b,
      nir_ieq_imm(b, kind, KK_QUERY_RECORD_KIND_STANDARD_2D));
   nir_def *view = kk_standard_3d_load(b, query,
      offsetof(struct kk_standard_3d_query, logical_view), 1, 64);
   nir_def *table = kk_standard_3d_load(b, view,
      offsetof(struct kk_standard_3d_view, image_table), 1, 64);
   nir_def *zero_one = kk_standard_3d_load(b, view,
      offsetof(struct kk_standard_3d_view, zero_one_mask), 1, 32);
   if (!parent_lod) {
      nir_def *base = kk_standard_3d_load(b, query, storage ? 64 : 8, 1, 32);
      lod = nir_iadd(b, lod, base);
   }
   nir_def *base_layer = kk_standard_3d_load(b, view,
      offsetof(struct kk_standard_3d_view, base_layer), 1, 32);
   nir_def *layer = array ? nir_iadd(b, nir_channel(b, coord, 2), base_layer) : base_layer;
   nir_def *coord_scale = kk_standard_3d_load(b, view,
      offsetof(struct kk_standard_3d_view, coord_scale), 2, 32);
   nir_def *xy = nir_imul(b, nir_trim_vector(b, coord, 2), coord_scale);
   nir_def *shape = kk_standard_3d_load(b, table,
      offsetof(struct kk_standard_3d_table, shape), 2, 32);
   nir_def *extent = kk_standard_3d_load(b, table,
      offsetof(struct kk_standard_3d_table, extent), 2, 32);
   extent = nir_umax(b, nir_ushr(b, extent, nir_umin(b, lod, nir_imm_int(b, 31))),
                     nir_imm_ivec2(b, 1, 1));
   nir_def *levels = kk_standard_3d_load(b, table,
      offsetof(struct kk_standard_3d_table, levels), 1, 32);
   nir_def *layers = kk_standard_3d_load(b, table,
      offsetof(struct kk_standard_3d_table, layers), 1, 32);
   nir_def *valid = nir_iand(b, nir_iand(b, nir_ult(b, lod, levels), nir_ult(b, layer, layers)),
      nir_ball(b, nir_ult(b, xy, extent)));
   nir_if *inside = nir_push_if(b, valid);
   nir_def *tail = kk_standard_3d_load(b, table,
      offsetof(struct kk_standard_3d_table, first_tail), 1, 32);
   nir_if *regular = nir_push_if(b, nir_ult(b, lod, tail));
   nir_def *tiles = nir_udiv(b, nir_iadd(b, extent, nir_iadd_imm(b, shape, -1)), shape);
   nir_def *at = nir_udiv(b, xy, shape);
   nir_def *local = nir_iadd(b, nir_channel(b, at, 0),
      nir_imul(b, nir_channel(b, tiles, 0), nir_channel(b, at, 1)));
   nir_def *start = nir_load_global_constant(b, 1, 64,
      nir_iadd(b, nir_iadd_imm(b, table, offsetof(struct kk_standard_3d_table, level_page)),
         nir_imul_imm(b, nir_u2u64(b, lod), 8)), .align_mul = 8);
   nir_def *regular_index = nir_iadd(b, start, nir_u2u64(b, local));
   nir_def *regular_coord = nir_umod(b, xy, shape);
   nir_push_else(b, regular);
   /* Packed levels follow each other in bytes; decode the byte address into
    * the page's standard-shaped texture as standard 3D tails do. */
   nir_def *tail_offset = nir_load_global_constant(b, 1, 64,
      nir_iadd(b, nir_iadd_imm(b, table, offsetof(struct kk_standard_3d_table, tail_offset_B)),
         nir_imul_imm(b, nir_u2u64(b, lod), 8)), .align_mul = 8);
   nir_def *element = kk_standard_3d_load(b, table,
      offsetof(struct kk_standard_3d_table, element_bytes), 1, 32);
   nir_def *block = kk_standard_3d_load(b, table,
      offsetof(struct kk_standard_3d_table, block), 2, 32);
   nir_def *block_coord = nir_udiv(b, xy, block);
   nir_def *block_extent = nir_udiv(b, nir_iadd(b, extent, nir_iadd_imm(b, block, -1)), block);
   nir_def *linear = nir_iadd(b, nir_u2u64(b, nir_channel(b, block_coord, 0)),
      nir_imul(b, nir_u2u64(b, nir_channel(b, block_extent, 0)),
                  nir_u2u64(b, nir_channel(b, block_coord, 1))));
   nir_def *tail_start = nir_load_global_constant(b, 1, 64,
      nir_iadd(b, nir_iadd_imm(b, table, offsetof(struct kk_standard_3d_table, level_page)),
         nir_imul_imm(b, nir_u2u64(b, tail), 8)), .align_mul = 8);
   nir_def *tail_byte = nir_iadd(b, tail_offset, nir_imul(b, linear, nir_u2u64(b, element)));
   nir_def *tail_index = nir_iadd(b, tail_start, nir_ushr_imm(b, tail_byte, 16));
   nir_def *page_block = nir_udiv(b, nir_u2u32(b,
      nir_iand_imm(b, tail_byte, KK_SPARSE_PAGE_SIZE - 1)), element);
   nir_def *page_x = nir_udiv(b, nir_channel(b, shape, 0), nir_channel(b, block, 0));
   nir_def *tail_coord = nir_vec2(b, nir_umod(b, page_block, page_x),
                                     nir_udiv(b, page_block, page_x));
   tail_coord = nir_iadd(b, nir_imul(b, tail_coord, block), nir_umod(b, xy, block));
   nir_pop_if(b, regular);
   /* Phis lead the block that follows the if. */
   nir_def *page_index = nir_if_phi(b, regular_index, tail_index);
   nir_def *page_coord = nir_if_phi(b, regular_coord, tail_coord);
   nir_def *layer_pages = kk_standard_3d_load(b, table,
      offsetof(struct kk_standard_3d_table, layer_pages), 1, 64);
   nir_def *index = nir_iadd(b, page_index,
      nir_imul(b, nir_u2u64(b, layer), layer_pages));
   nir_def *entries = kk_standard_3d_load(b, table,
      offsetof(struct kk_standard_3d_table, entries), 1, 64);
   nir_def *tile_address = nir_iadd(b, entries,
      nir_imul_imm(b, index, sizeof(struct kk_standard_3d_tile)));
   nir_def *resident = nir_ine_imm(b, kk_standard_3d_load(b, tile_address,
      offsetof(struct kk_standard_3d_tile, resident), 1, 32), 0);
   nir_def *targets = kk_standard_3d_load(b, view,
      offsetof(struct kk_standard_3d_view, targets), 1, 64);
   /* The array slot of a page target holds its one-layer 2D array handle. */
   nir_def *target_address = nir_bcsel(b, resident,
      nir_iadd_imm(b, nir_iadd(b, targets, nir_imul_imm(b, index,
         sizeof(struct kk_storage_image_descriptor))),
         array ? offsetof(struct kk_storage_image_descriptor, image_array_gpu_resource_id) : 0),
      nir_imm_int64(b, 0));
   nir_def *local_coord = nir_udiv(b, page_coord, coord_scale);
   nir_def *canonical_coord = original_coord;
   for (unsigned c = 0; c < 2; c++)
      canonical_coord = nir_vector_insert_imm(b, canonical_coord,
         nir_channel(b, local_coord, c), c);
   if (array)
      canonical_coord = nir_vector_insert_imm(b, canonical_coord,
         nir_imm_zero(b, 1, original_coord->bit_size), 2);
   canonical_coord = nir_bcsel(b, resident, canonical_coord, original_coord);
   nir_def *canonical_lod = nir_bcsel(b, resident, nir_imm_int(b, 0), original_lod);
   nir_push_else(b, inside);
   nir_def *outside = nir_imm_false(b);
   nir_def *no_target = nir_imm_int64(b, 0);
   nir_pop_if(b, inside);
   resident = nir_if_phi(b, resident, outside);
   target_address = nir_if_phi(b, target_address, no_target);
   canonical_coord = nir_if_phi(b, canonical_coord, original_coord);
   canonical_lod = nir_if_phi(b, canonical_lod, original_lod);
   nir_push_else(b, standard);
   nir_def *ordinary = nir_imm_true(b), *ordinary_mask = nir_imm_int(b, 0);
   nir_def *ordinary_target = nir_imm_int64(b, 0);
   nir_pop_if(b, standard);
   return (struct kk_standard_3d_predicate){
      nir_if_phi(b, resident, ordinary), nir_if_phi(b, zero_one, ordinary_mask),
      nir_if_phi(b, target_address, ordinary_target),
      nir_if_phi(b, canonical_coord, original_coord),
      nir_if_phi(b, canonical_lod, original_lod),
   };
}

/* Accesses run only where the predicate selected a page (or the image is not
 * a standard 2D image); elsewhere a load returns zero (alpha one where the
 * view's format has no alpha) and a store or atomic does nothing. */
static void
kk_standard_2d_guard(nir_builder *b, nir_instr *instr, nir_def *def,
                     struct kk_standard_3d_predicate *mask, nir_alu_type type,
                     bool store, bool atomic)
{
   nir_push_if(b, mask->resident);
   nir_instr *copy = nir_instr_clone(b->shader, instr);
   nir_builder_instr_insert(b, copy);
   nir_push_else(b, NULL);
   nir_def *zero = store ? NULL : kk_standard_3d_zero(b, def->num_components,
      def->bit_size, type, atomic ? nir_imm_int(b, 0) : mask->zero_one);
   nir_pop_if(b, NULL);
   if (store) {
      nir_instr_remove(instr);
      return;
   }
   nir_def *value = copy->type == nir_instr_type_tex ? &nir_instr_as_tex(copy)->def :
                                                     &nir_instr_as_intrinsic(copy)->def;
   nir_def_replace(def, nir_if_phi(b, value, zero));
}

/* An unbound tile of a standard 2D image is never read: its native sparse
 * texture has no mappings, and reading an unmapped shader-writable placement
 * sparse texture is not defined. Other images keep the native status. */
static bool
kk_standard_2d_sparse(nir_builder *b, nir_instr *instr, nir_def *def,
                      struct kk_standard_3d_predicate *mask, nir_alu_type type)
{
   unsigned colors = def->num_components - 1;
   nir_push_if(b, mask->resident);
   nir_instr *copy = nir_instr_clone(b->shader, instr);
   nir_builder_instr_insert(b, copy);
   nir_def *copied = copy->type == nir_instr_type_tex ? &nir_instr_as_tex(copy)->def :
                                                       &nir_instr_as_intrinsic(copy)->def;
   nir_def *value = nir_mov(b, copied);
   kk_nir_lower_sparse_instruction(b, copy, NULL, NULL);
   nir_push_else(b, NULL);
   nir_def *zero = kk_standard_3d_zero(b, colors, def->bit_size, type, mask->zero_one);
   zero = nir_vector_insert_imm(b, nir_pad_vector(b, zero, colors + 1),
      nir_imm_intN_t(b, 1, def->bit_size), colors);
   nir_pop_if(b, NULL);
   nir_def_replace(def, nir_if_phi(b, value, zero));
   return true;
}

static bool
kk_standard_2d_lower_image(nir_builder *b, nir_intrinsic_instr *intr,
   nir_deref_instr *deref, nir_alu_type type, nir_def *resource_address,
   const struct lower_descriptors_ctx *ctx)
{
   if (!ctx->standard_2d || nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_2D)
      return false;
   bool sparse = intr->intrinsic == nir_intrinsic_bindless_image_sparse_load;
   bool store = intr->intrinsic == nir_intrinsic_bindless_image_store;
   bool load = intr->intrinsic == nir_intrinsic_bindless_image_load;
   bool atomic = intr->intrinsic == nir_intrinsic_bindless_image_atomic ||
                 intr->intrinsic == nir_intrinsic_bindless_image_atomic_swap;
   if (!sparse && !store && !load && !atomic) return false;
   bool array = nir_intrinsic_image_array(intr);
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *metadata = load_resource_deref_desc(b, 1, 64, deref,
      offsetof(struct kk_storage_image_descriptor, texel_offset), ctx);
   nir_def *query = kk_nir_storage_sparse_query_decode(b, metadata);
   nir_def *lod = atomic ? nir_imm_int(b, 0) : intr->src[store ? 4 : 3].ssa;
   struct kk_standard_3d_predicate mask = kk_standard_2d_predicate(b, query,
      intr->src[1].ssa, lod, true, sparse, array);
   nir_def *address = nir_bcsel(b, nir_ine_imm(b, mask.target_address, 0),
                                  mask.target_address, resource_address);
   nir_def *handle = nir_load_texture_handle_kk(b, 1, 64, address,
      .dest_type = type, .image_dim = GLSL_SAMPLER_DIM_2D,
      .image_array = array, .access = nir_intrinsic_access(intr),
      .flags = msl_convert_access_flag(nir_intrinsic_access(intr)));
   nir_src_rewrite(&intr->src[0], handle);
   nir_src_rewrite(&intr->src[1], mask.coord);
   if (!atomic) nir_src_rewrite(&intr->src[store ? 4 : 3], mask.lod);
   if (sparse)
      return kk_standard_2d_sparse(b, &intr->instr, &intr->def, &mask, type);
   kk_standard_2d_guard(b, &intr->instr, store ? NULL : &intr->def, &mask, type,
                        store, atomic);
   return true;
}

static bool
kk_standard_2d_lower_fetch(nir_builder *b, nir_tex_instr *tex,
   nir_def *texture, unsigned descriptor_offset, nir_def *resource_address,
   const struct lower_descriptors_ctx *ctx)
{
   if (!ctx->standard_2d || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       tex->op != nir_texop_txf) return false;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *query = kk_standard_3d_sample_query(b,
      nir_src_as_deref(nir_src_for_ssa(texture)), descriptor_offset, ctx);
   int lod_source = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   nir_def *lod = lod_source < 0 ? nir_imm_int(b, 0) : tex->src[lod_source].src.ssa;
   nir_def *coord = tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa;
   struct kk_standard_3d_predicate mask = kk_standard_2d_predicate(b, query,
      coord, lod, false, tex->is_sparse, tex->is_array);
   nir_def *address = nir_bcsel(b, nir_ine_imm(b, mask.target_address, 0),
                                  mask.target_address, resource_address);
   nir_def *handle = nir_load_texture_handle_kk(b, 1, 64, address,
      .dest_type = tex->dest_type, .image_dim = GLSL_SAMPLER_DIM_2D,
      .image_array = tex->is_array);
   nir_src_rewrite(&tex->src[nir_tex_instr_src_index(tex, nir_tex_src_texture_handle)].src, handle);
   nir_src_rewrite(&tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src, mask.coord);
   if (lod_source >= 0) nir_src_rewrite(&tex->src[lod_source].src, mask.lod);
   if (tex->is_sparse)
      return kk_standard_2d_sparse(b, &tex->instr, &tex->def, &mask, tex->dest_type);
   kk_standard_2d_guard(b, &tex->instr, &tex->def, &mask, tex->dest_type, false, false);
   return true;
}

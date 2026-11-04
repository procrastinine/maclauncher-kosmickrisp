/* SPDX-License-Identifier: MIT
 * Included by descriptor lowering so sparse IO shares its descriptor access.
 */
static nir_def *
kk_sparse_descriptor_index(nir_builder *b, nir_intrinsic_instr *descriptor,
                           const struct lower_descriptors_ctx *ctx)
{
   if (descriptor->intrinsic == nir_intrinsic_load_heap_descriptor) {
      nir_def *heap = load_root(b, 1, 64,
         nir_imm_int(b, kk_root_descriptor_offset(heap_resource)), 8);
      nir_def *address = nir_iadd(b, heap, nir_u2u64(b, descriptor->src[0].ssa));
      return nir_load_global_constant(b, 1, 32, nir_iadd_imm(b, address, 12), .align_mul = 4);
   }
   nir_intrinsic_instr *idx = nir_src_as_intrinsic(descriptor->src[0]);
   if (idx && is_idx_intrin(idx)) {
      nir_def *index = nir_imm_int(b, 0);
      while (idx->intrinsic == nir_intrinsic_vulkan_resource_reindex) {
         index = nir_iadd(b, index, idx->src[1].ssa);
         idx = nir_src_as_intrinsic(idx->src[0]);
      }
      index = nir_iadd(b, index, idx->src[0].ssa);
      return load_descriptor(b, 1, 32, nir_intrinsic_desc_set(idx),
         nir_intrinsic_binding(idx), index, 12, ctx);
   }
   /* Complex SSBO resource indices are rewritten into bounded descriptor
    * addresses by lower_ssbo_resource_index, after this pass. */
   nir_def *addr = descriptor->src[0].ssa;
   nir_def *base = nir_iand_imm(b, nir_pack_64_2x32(b, nir_trim_vector(b, addr, 2)),
                               BITFIELD64_MASK(56));
   nir_def *offset = nir_iadd_imm(b, nir_channel(b, addr, 3), 12);
   return nir_load_global_constant_bounded(b, 1, 32, base, offset,
      nir_channel(b, addr, 2), .align_mul = 4);
}

static nir_def *
kk_sparse_record(nir_builder *b, nir_def *index)
{
   nir_def *root = load_root(b, 1, 64,
      nir_imm_int(b, kk_root_descriptor_offset(sparse_state)), 8);
   nir_def *high = nir_u2u64(b, nir_ushr_imm(b, index, 20));
   nir_def *mid = nir_u2u64(b, nir_iand_imm(b, nir_ushr_imm(b, index, 10), 1023));
   nir_def *low = nir_u2u64(b, nir_iand_imm(b, index, 1023));
   nir_def *directory = nir_load_global_constant(b, 1, 64,
      nir_iadd(b, root, nir_iadd_imm(b, nir_ishl_imm(b, high, 3), 16)), .align_mul = 8);
   nir_def *leaf = nir_load_global_constant(b, 1, 64,
      nir_iadd(b, directory, nir_ishl_imm(b, mid, 3)), .align_mul = 8);
   return nir_iadd(b, leaf, nir_ishl_imm(b, low, 5));
}

static nir_def *
kk_sparse_scalar_resident(nir_builder *b, nir_def *record, nir_def *address,
                          unsigned size)
{
   nir_def *base = nir_load_global_constant(b, 1, 64, record, .align_mul = 8);
   nir_def *limit = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, record, 8), .align_mul = 8);
   nir_def *offset = nir_isub(b, address, base);
   nir_def *end = nir_iadd_imm(b, address, size - 1);
   nir_def *valid = nir_iand(b, nir_uge(b, address, base),
      nir_iand(b, nir_ult(b, end, limit), nir_uge(b, end, address)));
   nir_if *bounds = nir_push_if(b, valid);
   nir_def *map = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, record, 16), .align_mul = 8);
   nir_def *first = nir_ushr_imm(b, offset, 16);
   nir_def *last = nir_ushr_imm(b, nir_iadd_imm(b, offset, size - 1), 16);
   nir_def *a = nir_load_global_constant(b, 1, 8, nir_iadd(b, map, first), .align_mul = 1);
   nir_def *z = nir_load_global_constant(b, 1, 8, nir_iadd(b, map, last), .align_mul = 1);
   nir_def *resident = nir_iand(b, nir_ine_imm(b, a, 0), nir_ine_imm(b, z, 0));
   nir_push_else(b, bounds);
   nir_def *no = nir_imm_false(b);
   nir_pop_if(b, bounds);
   return nir_if_phi(b, resident, no);
}

static bool
kk_sparse_is_physical(nir_deref_instr *deref)
{
   /* PhysicalStorageBuffer pointers are the SPIR-V user's global derefs.
    * Driver-generated metadata/global intrinsics do not enter this pass. */
   return nir_deref_mode_is(deref, nir_var_mem_global);
}

static nir_def *
kk_sparse_bda_version(nir_builder *b)
{
   nir_def *root = load_root(b, 1, 64,
      nir_imm_int(b, kk_root_descriptor_offset(sparse_state)), 8);
   return nir_load_global(b, 1, 64, root, .align_mul = 8, .access = ACCESS_VOLATILE);
}

/* A single original IO can span records (or an ordinary gap) and pages.
 * Locate its first interval once, then walk forward only as its scalar
 * addresses cross intervals. Cache page status within this IO, never across
 * accesses: version and map observations retain their volatile boundaries. */
static nir_def *
kk_sparse_access_mask_body(nir_builder *b, nir_def *source, nir_def *address,
                           nir_def *size, nir_def *components, nir_def *write_mask,
                      bool range_lookup)
{
   nir_variable *cursor = nir_local_variable_create(b->impl, glsl_uint_type(), "sparse_cursor");
   nir_variable *component = nir_local_variable_create(b->impl, glsl_uint_type(), "sparse_component");
   nir_variable *mask = nir_local_variable_create(b->impl, glsl_uint_type(), "sparse_mask");
   nir_variable *cached_record = nir_local_variable_create(b->impl, glsl_uint64_t_type(), "sparse_record");
   nir_variable *base = nir_local_variable_create(b->impl, glsl_uint64_t_type(), "sparse_base");
   nir_variable *limit = nir_local_variable_create(b->impl, glsl_uint64_t_type(), "sparse_limit");
   nir_variable *map = nir_local_variable_create(b->impl, glsl_uint64_t_type(), "sparse_map");
   nir_variable *page = nir_local_variable_create(b->impl, glsl_uint64_t_type(), "sparse_page");
   nir_variable *page_ok = nir_local_variable_create(b->impl, glsl_bool_type(), "sparse_page_ok");
   nir_variable *page_valid = nir_local_variable_create(b->impl, glsl_bool_type(), "sparse_page_valid");
   nir_def *count = range_lookup ? nir_load_global_constant(b, 1, 32, source, .align_mul = 4) : NULL;
   if (range_lookup) {
      nir_variable *high = nir_local_variable_create(b->impl, glsl_uint_type(), "sparse_high");
      nir_store_var(b, cursor, nir_imm_int(b, 0), 1);
      nir_store_var(b, high, count, 1);
      /* Lower bound on interval end also identifies the next interval when
       * the address starts in an ordinary gap. */
      nir_loop *search = nir_push_loop(b);
      search->control = nir_loop_control_dont_unroll;
      nir_def *lo = nir_load_var(b, cursor), *hi = nir_load_var(b, high);
      nir_if *done = nir_push_if(b, nir_uge(b, lo, hi));
      nir_jump(b, nir_jump_break);
      nir_pop_if(b, done);
      nir_def *mid = nir_iadd(b, lo, nir_ushr_imm(b, nir_isub(b, hi, lo), 1));
      nir_def *entry = nir_iadd(b, source,
         nir_iadd_imm(b, nir_ishl_imm(b, nir_u2u64(b, mid), 5), 16));
      nir_def *end = nir_load_global_constant(b, 1, 64, nir_iadd_imm(b, entry, 8), .align_mul = 8);
      nir_if *after = nir_push_if(b, nir_uge(b, address, end));
      nir_store_var(b, cursor, nir_iadd_imm(b, mid, 1), 1);
      nir_push_else(b, after);
      nir_store_var(b, high, mid, 1);
      nir_pop_if(b, after);
      nir_pop_loop(b, search);
   }
   nir_store_var(b, component, nir_imm_int(b, 0), 1);
   nir_store_var(b, mask, nir_imm_int(b, 0), 1);
   nir_store_var(b, cached_record, nir_imm_int64(b, 0), 1);
   nir_store_var(b, page_valid, nir_imm_false(b), 1);
   nir_store_var(b, page, nir_imm_int64(b, 0), 1);
   nir_store_var(b, page_ok, nir_imm_false(b), 1);
   nir_loop *scalars = nir_push_loop(b);
   scalars->control = nir_loop_control_dont_unroll;
   nir_def *c = nir_load_var(b, component);
   nir_if *done = nir_push_if(b, nir_uge(b, c, components));
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, done);
   nir_def *bit = nir_ishl(b, nir_imm_int(b, 1), c);
   nir_def *scalar_address = nir_iadd(b, address, nir_u2u64(b, nir_imul(b, c, size)));
   nir_if *used = nir_push_if(b, nir_ine_imm(b, nir_iand(b, bit, write_mask), 0));
   nir_def *record = source;
   if (range_lookup) {
      /* Preserve full64 wrap behavior without another directory search. */
      nir_if *wrapped = nir_push_if(b, nir_ult(b, scalar_address, address));
      nir_store_var(b, cursor, nir_imm_int(b, 0), 1);
      nir_pop_if(b, wrapped);
      nir_variable *found = nir_local_variable_create(b->impl, glsl_uint64_t_type(), "sparse_found");
      nir_store_var(b, found, nir_imm_int64(b, 0), 1);
      nir_loop *advance = nir_push_loop(b);
      advance->control = nir_loop_control_dont_unroll;
      nir_def *i = nir_load_var(b, cursor);
      nir_if *end = nir_push_if(b, nir_uge(b, i, count));
      nir_jump(b, nir_jump_break);
      nir_pop_if(b, end);
      nir_def *entry = nir_iadd(b, source,
         nir_iadd_imm(b, nir_ishl_imm(b, nir_u2u64(b, i), 5), 16));
      nir_def *start = nir_load_global_constant(b, 1, 64, entry, .align_mul = 8);
      nir_def *limit = nir_load_global_constant(b, 1, 64, nir_iadd_imm(b, entry, 8), .align_mul = 8);
      nir_if *after = nir_push_if(b, nir_uge(b, scalar_address, limit));
      nir_store_var(b, cursor, nir_iadd_imm(b, i, 1), 1);
      nir_push_else(b, after);
      nir_if *inside = nir_push_if(b, nir_uge(b, scalar_address, start));
      nir_store_var(b, found, nir_load_global_constant(b, 1, 64,
         nir_iadd_imm(b, entry, 16), .align_mul = 8), 1);
      nir_pop_if(b, inside);
      nir_jump(b, nir_jump_break);
      nir_pop_if(b, after);
      nir_pop_loop(b, advance);
      record = nir_load_var(b, found);
   }
   nir_if *present = nir_push_if(b, nir_ine_imm(b, record, 0));
   nir_if *changed = nir_push_if(b, nir_ine(b, record, nir_load_var(b, cached_record)));
   nir_store_var(b, cached_record, record, 1);
   nir_store_var(b, base, nir_load_global_constant(b, 1, 64, record, .align_mul = 8), 1);
   nir_store_var(b, limit, nir_load_global_constant(b, 1, 64, nir_iadd_imm(b, record, 8), .align_mul = 8), 1);
   nir_def *map_address = nir_iadd_imm(b, record, 16);
   nir_store_var(b, map, range_lookup ? nir_load_global(b, 1, 64, map_address,
      .align_mul = 8, .access = ACCESS_VOLATILE) :
      nir_load_global_constant(b, 1, 64, map_address, .align_mul = 8), 1);
   nir_store_var(b, page_valid, nir_imm_false(b), 1);
   nir_pop_if(b, changed);
   nir_def *map_value = nir_load_var(b, map);
   nir_if *mapped = range_lookup ? nir_push_if(b, nir_ine_imm(b, map_value, 0)) : NULL;
   nir_def *start = nir_load_var(b, base);
   nir_def *last = nir_iadd(b, scalar_address,
      nir_u2u64(b, nir_iadd_imm(b, size, -1)));
   nir_def *valid = nir_iand(b, nir_uge(b, scalar_address, start),
      nir_iand(b, nir_ult(b, last, nir_load_var(b, limit)), nir_uge(b, last, scalar_address)));
   nir_if *bounds = nir_push_if(b, valid);
   nir_def *first_page = nir_ushr_imm(b, nir_isub(b, scalar_address, start), 16);
   nir_def *last_page = nir_ushr_imm(b, nir_isub(b, last, start), 16);
   nir_def *resident = NULL;
   for (unsigned end = 0; end < 2; ++end) {
      nir_def *which = end ? last_page : first_page;
      nir_if *uncached = nir_push_if(b, nir_ior(b, nir_inot(b, nir_load_var(b, page_valid)),
         nir_ine(b, which, nir_load_var(b, page))));
      nir_store_var(b, page, which, 1);
      nir_store_var(b, page_ok, nir_ine_imm(b, nir_load_global_constant(b, 1, 8,
         nir_iadd(b, map_value, which), .align_mul = 1), 0), 1);
      nir_store_var(b, page_valid, nir_imm_true(b), 1);
      nir_pop_if(b, uncached);
      nir_def *ok = nir_load_var(b, page_ok);
      resident = end ? nir_iand(b, resident, ok) : ok;
   }
   nir_push_else(b, bounds);
   nir_def *no = nir_imm_false(b);
   nir_pop_if(b, bounds);
   resident = nir_if_phi(b, resident, no);
   if (range_lookup) {
      nir_push_else(b, mapped);
      nir_def *ordinary = nir_imm_true(b);
      nir_pop_if(b, mapped);
      resident = nir_if_phi(b, resident, ordinary);
   }
   nir_push_else(b, present);
   nir_def *ordinary = nir_imm_true(b);
   nir_pop_if(b, present);
   resident = nir_if_phi(b, resident, ordinary);
   nir_if *keep = nir_push_if(b, resident);
   nir_store_var(b, mask, nir_ior(b, nir_load_var(b, mask), bit), 1);
   nir_pop_if(b, keep);
   nir_pop_if(b, used);
   nir_store_var(b, component, nir_iadd_imm(b, c, 1), 1);
   nir_pop_loop(b, scalars);
   return nir_load_var(b, mask);
}

/* Added after application preprocessing/inlining. The ordinary native path
 * never constructs this function. One helper covers all scalar widths,
 * vector widths, write masks and both residency address representations. */
static nir_def *
kk_sparse_access_mask(nir_builder *b, nir_def *source, nir_def *address,
                      unsigned size, unsigned components, unsigned write_mask,
                      bool range_lookup)
{
   const char *name = "__kk_sparse_buffer_access";
   nir_function *helper = NULL;
   nir_foreach_function(function, b->shader) {
      if (strcmp(function->name, name) == 0) { helper = function; break; }
   }
   if (!helper) {
      helper = nir_function_create(b->shader, name);
      helper->dont_inline = true;
      helper->num_params = 7;
      helper->params = rzalloc_array(helper, nir_parameter, 7);
      helper->params[0] = (nir_parameter) {
         .num_components = 1, .bit_size = 64, .mode = nir_parameter_out,
         .is_return = true, .type = glsl_uint_type(),
      };
      for (unsigned i = 1; i < 7; ++i)
         helper->params[i] = (nir_parameter) {
            .num_components = 1, .bit_size = i < 3 ? 64 : 32,
         };
      nir_builder h = nir_builder_create(nir_function_impl_create(helper));
      h.cursor = nir_before_impl(h.impl);
      nir_def *src = nir_load_param(&h, 1), *addr = nir_load_param(&h, 2);
      nir_def *bytes = nir_load_param(&h, 3), *count = nir_load_param(&h, 4);
      nir_def *mask = nir_load_param(&h, 5);
      nir_if *range = nir_push_if(&h, nir_ine_imm(&h, nir_load_param(&h, 6), 0));
      nir_def *bda = kk_sparse_access_mask_body(&h, src, addr, bytes, count, mask, true);
      nir_push_else(&h, range);
      nir_def *indexed = kk_sparse_access_mask_body(&h, src, addr, bytes, count, mask, false);
      nir_pop_if(&h, range);
      nir_def *value = nir_if_phi(&h, bda, indexed);
      nir_deref_instr *out = nir_build_deref_cast(&h, nir_load_param(&h, 0),
         nir_var_function_temp, glsl_uint_type(), 0);
      nir_store_deref(&h, out, value, 1);
   }
   nir_variable *result = nir_local_variable_create(b->impl, glsl_uint_type(),
      ralloc_asprintf(b->shader, "sparse_access_result_%u", exec_list_length(&b->impl->locals)));
   nir_deref_instr *out = nir_build_deref_var(b, result);
   nir_call(b, helper, &out->def, source, address, nir_imm_int(b, size),
      nir_imm_int(b, components), nir_imm_int(b, write_mask), nir_imm_int(b, range_lookup));
   return nir_load_deref(b, out);
}

static bool
kk_sparse_buffer_io(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct lower_descriptors_ctx *ctx = data;
   bool load = intr->intrinsic == nir_intrinsic_load_deref;
   bool store = intr->intrinsic == nir_intrinsic_store_deref;
   bool atomic = intr->intrinsic == nir_intrinsic_deref_atomic ||
                 intr->intrinsic == nir_intrinsic_deref_atomic_swap;
   if (!load && !store && !atomic) return false;
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   bool physical = kk_sparse_is_physical(deref) && deref->def.num_components == 1;
   bool bounded = deref->def.num_components == 4 &&
      nir_deref_mode_is_one_of(deref, nir_var_mem_ssbo | nir_var_mem_ubo | nir_var_mem_global);
   if (!physical && !bounded) return false;
   nir_deref_instr *root = deref;
   while (nir_deref_instr_parent(root)) root = nir_deref_instr_parent(root);
   nir_intrinsic_instr *descriptor = root->deref_type == nir_deref_type_cast ?
      nir_src_as_intrinsic(root->parent) : NULL;

   bool range_lookup = physical || !descriptor ||
      (descriptor->intrinsic != nir_intrinsic_load_vulkan_descriptor &&
       descriptor->intrinsic != nir_intrinsic_load_heap_descriptor);

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *index = range_lookup ? kk_sparse_bda_version(b) :
      kk_sparse_descriptor_index(b, descriptor, ctx);
   nir_def *address = physical ? &deref->def :
      nir_iadd(b, nir_pack_64_2x32(b, nir_trim_vector(b, &deref->def, 2)),
                  nir_u2u64(b, nir_channel(b, &deref->def, 3)));
   unsigned bits = store ? intr->src[1].ssa->bit_size : intr->def.bit_size;
   unsigned components = intr->num_components;
   unsigned size = bits == 1 ? 4 : bits / 8;
   if (load) {
      /* Tier 1 reads are safe even if the native cache retains an unmapped
       * store. Mask each scalar independently, including page crossings. */
      nir_intrinsic_instr *raw = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &intr->instr));
      nir_builder_instr_insert(b, &raw->instr);
      nir_if *sparse = nir_push_if(b, nir_ine_imm(b, index, 0));
      nir_def *record = range_lookup ? NULL : kk_sparse_record(b, index);
      nir_def *resident_mask = kk_sparse_access_mask(b, range_lookup ? index : record,
         address, size, components, BITFIELD_MASK(components), range_lookup);
      nir_def *parts[NIR_MAX_VEC_COMPONENTS];
      for (unsigned c = 0; c < components; ++c) {
         nir_def *resident = nir_ine_imm(b, nir_iand_imm(b, resident_mask, BITFIELD_BIT(c)), 0);
         parts[c] = nir_bcsel(b, resident, nir_channel(b, &raw->def, c), nir_imm_intN_t(b, 0, bits));
      }
      nir_def *masked = nir_vec(b, parts, components);
      nir_push_else(b, sparse);
      nir_pop_if(b, sparse);
      nir_def *result = nir_if_phi(b, masked, &raw->def);
      /* The fixup crosses blocks. rewrite_uses_after requires one block and
       * would rewrite the mask's own source into a self-referential phi. */
      nir_def_rewrite_uses(&intr->def, result);
      nir_instr_remove(&intr->instr);
      return true;
   }

   nir_if *ordinary = nir_push_if(b, nir_ieq_imm(b, index, 0));
   nir_intrinsic_instr *normal = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &intr->instr));
   nir_builder_instr_insert(b, &normal->instr);
   nir_push_else(b, ordinary);
   nir_def *record = range_lookup ? NULL : kk_sparse_record(b, index);
   nir_def *atomic_result = NULL;
   unsigned mask = store ? nir_intrinsic_write_mask(intr) : 1;
   nir_def *resident_mask = kk_sparse_access_mask(b, range_lookup ? index : record,
      address, size, components, mask, range_lookup);
   u_foreach_bit(c, mask) {
      nir_if *mapped = nir_push_if(b, nir_ine_imm(b, nir_iand_imm(b, resident_mask, BITFIELD_BIT(c)), 0));
      nir_intrinsic_instr *scalar = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &intr->instr));
      if (store) {
         nir_deref_instr *component = components > 1 ? nir_build_deref_array_imm(b, deref, c) : deref;
         scalar->src[0] = nir_src_for_ssa(&component->def);
         scalar->src[1] = nir_src_for_ssa(nir_channel(b, intr->src[1].ssa, c));
         scalar->num_components = 1;
         nir_intrinsic_set_write_mask(scalar, 1);
      }
      nir_builder_instr_insert(b, &scalar->instr);
      nir_push_else(b, mapped);
      nir_def *zero = atomic ? nir_imm_intN_t(b, 0, bits) : NULL;
      nir_pop_if(b, mapped);
      if (atomic) atomic_result = nir_if_phi(b, &scalar->def, zero);
   }
   nir_pop_if(b, ordinary);
   if (atomic) nir_def_rewrite_uses(&intr->def, nir_if_phi(b, &normal->def, atomic_result));
   nir_instr_remove(&intr->instr);
   return true;
}

bool
kk_nir_lower_sparse_buffer_io(nir_shader *nir,
   const struct vk_pipeline_robustness_state *rs, uint32_t count,
   struct vk_descriptor_set_layout *const *layouts)
{
   struct lower_descriptors_ctx ctx = {
      .clamp_desc_array_bounds = rs->storage_buffers != VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED ||
                                rs->uniform_buffers != VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED,
      .ssbo_addr_format = kk_buffer_addr_format(rs->storage_buffers),
      .ubo_addr_format = kk_buffer_addr_format(rs->uniform_buffers),
   };
   for (unsigned i = 0; i < count; ++i)
      if (layouts[i]) ctx.set_layouts[i] = vk_to_kk_descriptor_set_layout(layouts[i]);
   return nir_shader_intrinsics_pass(nir, kk_sparse_buffer_io, nir_metadata_none, &ctx);
}

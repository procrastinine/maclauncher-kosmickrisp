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

static nir_def *
kk_sparse_bda_record(nir_builder *b, nir_def *version, nir_def *address)
{
   nir_variable *low = nir_local_variable_create(b->impl, glsl_uint_type(), "sparse_low");
   nir_variable *high = nir_local_variable_create(b->impl, glsl_uint_type(), "sparse_high");
   nir_variable *found = nir_local_variable_create(b->impl,
      glsl_uint64_t_type(), "sparse_range");
   nir_store_var(b, low, nir_imm_int(b, 0), 1);
   nir_store_var(b, high, nir_load_global_constant(b, 1, 32, version, .align_mul = 4), 1);
   nir_store_var(b, found, nir_imm_int64(b, 0), 1);
   nir_loop *loop = nir_push_loop(b);
   nir_def *lo = nir_load_var(b, low), *hi = nir_load_var(b, high);
   nir_if *done = nir_push_if(b, nir_uge(b, lo, hi));
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, done);
   nir_def *mid = nir_iadd(b, lo, nir_ushr_imm(b, nir_isub(b, hi, lo), 1));
   nir_def *entry = nir_iadd(b, version,
      nir_iadd_imm(b, nir_ishl_imm(b, nir_u2u64(b, mid), 5), 16));
   nir_def *start = nir_load_global_constant(b, 1, 64, entry, .align_mul = 8);
   nir_def *limit = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, entry, 8), .align_mul = 8);
   nir_if *before = nir_push_if(b, nir_ult(b, address, start));
   nir_store_var(b, high, mid, 1);
   nir_push_else(b, before);
   nir_if *after = nir_push_if(b, nir_uge(b, address, limit));
   nir_store_var(b, low, nir_iadd_imm(b, mid, 1), 1);
   nir_push_else(b, after);
   nir_store_var(b, found, nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, entry, 16), .align_mul = 8), 1);
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, after);
   nir_pop_if(b, before);
   nir_pop_loop(b, loop);
   return nir_load_var(b, found);
}

static nir_def *
kk_sparse_bda_resident(nir_builder *b, nir_def *version, nir_def *address, unsigned size)
{
   nir_def *record = kk_sparse_bda_record(b, version, address);
   nir_if *found = nir_push_if(b, nir_ine_imm(b, record, 0));
   nir_def *map = nir_load_global(b, 1, 64, nir_iadd_imm(b, record, 16),
      .align_mul = 8, .access = ACCESS_VOLATILE);
   nir_push_else(b, found);
   nir_def *none = nir_imm_int64(b, 0);
   nir_pop_if(b, found);
   map = nir_if_phi(b, map, none);
   nir_if *mapped = nir_push_if(b, nir_ine_imm(b, map, 0));
   nir_def *resident = kk_sparse_scalar_resident(b, record, address, size);
   nir_push_else(b, mapped);
   nir_def *ordinary = nir_imm_true(b);
   nir_pop_if(b, mapped);
   return nir_if_phi(b, resident, ordinary);
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
      nir_def *parts[NIR_MAX_VEC_COMPONENTS];
      for (unsigned c = 0; c < components; ++c) {
         nir_def *scalar_address = nir_iadd_imm(b, address, c * size);
         nir_def *resident = range_lookup ? kk_sparse_bda_resident(b, index, scalar_address, size) :
            kk_sparse_scalar_resident(b, record, scalar_address, size);
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
   u_foreach_bit(c, mask) {
      nir_def *scalar_address = nir_iadd_imm(b, address, c * size);
      nir_if *mapped = nir_push_if(b, range_lookup ? kk_sparse_bda_resident(b, index, scalar_address, size) :
         kk_sparse_scalar_resident(b, record, scalar_address, size));
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

/*
 * Copyright 2022 Alyssa Rosenzweig
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * Copyright 2026 Nilton Perim Neto
 * SPDX-License-Identifier: MIT
 *
 * Format encoding and runtime fetch derived from Mesa draft !45831,
 * commit 83b7c659c4aebd67863bbe105c299b1b383454eb.
 * Static and dynamic input use the same decoder; static metadata folds away.
 */

uint32_t
kk_vbo_encode_format(enum pipe_format format)
{
   if (format == PIPE_FORMAT_NONE || !kk_vbo_supports_format(format))
      return 0;

   const struct util_format_description *desc = util_format_description(format);
   int idx = util_format_get_first_non_void_channel(format);
   if (idx < 0)
      return 0;

   struct util_format_channel_description chan = desc->channel[idx];
   enum kk_vbo_type type;
   switch (chan.type) {
   case UTIL_FORMAT_TYPE_UNSIGNED:
      type = chan.normalized     ? KK_VBO_TYPE_UNORM
             : chan.pure_integer ? KK_VBO_TYPE_UINT
                                 : KK_VBO_TYPE_USCALED;
      break;
   case UTIL_FORMAT_TYPE_SIGNED:
      type = chan.normalized     ? KK_VBO_TYPE_SNORM
             : chan.pure_integer ? KK_VBO_TYPE_SINT
                                 : KK_VBO_TYPE_SSCALED;
      break;
   case UTIL_FORMAT_TYPE_FLOAT:
      type = KK_VBO_TYPE_SFLOAT;
      break;
   default:
      return 0;
   }

   uint32_t fmt = KK_VBO_FMT_VALID | (type << KK_VBO_FMT_TYPE_SHIFT);

   if (is_rg11_b10(desc)) {
      fmt |= KK_VBO_FMT_RG11B10F | (2u << KK_VBO_FMT_SIZE_SHIFT);
   } else if (is_rgb10_a2(desc)) {
      fmt |= KK_VBO_FMT_RGB10A2 | (2u << KK_VBO_FMT_SIZE_SHIFT);
   } else {
      fmt |= util_logbase2(chan.size / 8) << KK_VBO_FMT_SIZE_SHIFT;
      fmt |= (desc->nr_channels - 1) << KK_VBO_FMT_COUNT_SHIFT;
   }

   for (unsigned c = 0; c < 4; ++c) {
      assert(desc->swizzle[c] <= PIPE_SWIZZLE_1);
      fmt |= (uint32_t)desc->swizzle[c] << (KK_VBO_FMT_SWIZZLE_SHIFT + c * 3);
   }

   return fmt;
}

static nir_def *
fmt_field(nir_builder *b, nir_def *fmt, unsigned shift, unsigned mask)
{
   return nir_iand_imm(b, nir_ushr_imm(b, fmt, shift), mask);
}

static nir_def *
load_root(nir_builder *b, nir_def *argbuf, unsigned bit_size, uint64_t offset)
{
   return nir_load_global_constant(b, 1, bit_size,
                                   nir_iadd_imm(b, argbuf, offset));
}

static nir_def *
fetch_component(nir_builder *b, nir_def *addr, nir_def *size_log2, unsigned i)
{
   nir_def *v32, *v16, *v8;

   nir_push_if(b, nir_ieq_imm(b, size_log2, 2));
   {
      v32 = nir_load_global_constant(b, 1, 32, nir_iadd_imm(b, addr, i * 4));
   }
   nir_push_else(b, NULL);
   {
      nir_push_if(b, nir_ieq_imm(b, size_log2, 1));
      {
         v16 = nir_u2u32(b, nir_load_global_constant(
                               b, 1, 16, nir_iadd_imm(b, addr, i * 2)));
      }
      nir_push_else(b, NULL);
      {
         v8 = nir_u2u32(
            b, nir_load_global_constant(b, 1, 8, nir_iadd_imm(b, addr, i)));
      }
      nir_pop_if(b, NULL);
      v16 = nir_if_phi(b, v16, v8);
   }
   nir_pop_if(b, NULL);

   return nir_if_phi(b, v32, v16);
}

static nir_def *
convert_float(nir_builder *b, nir_def *fmt, nir_def *size_log2, nir_def *raw,
              nir_def *u, nir_def *s)
{
   static const unsigned bits8[4] = {8, 8, 8, 8};
   static const unsigned bits16[4] = {16, 16, 16, 16};
   static const unsigned bits_rgb10a2[4] = {10, 10, 10, 2};

   nir_def *is8 = nir_ieq_imm(b, size_log2, 0);
   nir_def *is16 = nir_ieq_imm(b, size_log2, 1);
   nir_def *rgb10a2 = nir_test_mask(b, fmt, KK_VBO_FMT_RGB10A2);
   nir_def *rg11b10 = nir_test_mask(b, fmt, KK_VBO_FMT_RG11B10F);

   nir_def *pu = nir_format_unpack_uint(b, raw, bits_rgb10a2, 4);
   nir_def *ps = nir_format_unpack_sint(b, raw, bits_rgb10a2, 4);

   nir_def *unorm =
      nir_bcsel(b, rgb10a2, nir_format_unorm_to_float(b, pu, bits_rgb10a2),
                nir_bcsel(b, is8, nir_format_unorm_to_float(b, u, bits8),
                          nir_format_unorm_to_float(b, u, bits16)));
   nir_def *snorm =
      nir_bcsel(b, rgb10a2, nir_format_snorm_to_float(b, ps, bits_rgb10a2),
                nir_bcsel(b, is8, nir_format_snorm_to_float(b, s, bits8),
                          nir_format_snorm_to_float(b, s, bits16)));
   nir_def *uscaled = nir_u2f32(b, nir_bcsel(b, rgb10a2, pu, u));
   nir_def *sscaled = nir_i2f32(b, nir_bcsel(b, rgb10a2, ps, s));

   nir_def *rg11b10_val =
      nir_pad_vector_imm_int(b, nir_format_unpack_11f11f10f(b, raw), 0, 4);
   nir_def *sfloat =
      nir_bcsel(b, rg11b10, rg11b10_val,
                nir_bcsel(b, is16, nir_f2f32(b, nir_u2u16(b, u)), u));

   nir_def *type =
      fmt_field(b, fmt, KK_VBO_FMT_TYPE_SHIFT, KK_VBO_FMT_TYPE_MASK);
   nir_def *v = sfloat;
   v = nir_bcsel(b, nir_ieq_imm(b, type, KK_VBO_TYPE_UNORM), unorm, v);
   v = nir_bcsel(b, nir_ieq_imm(b, type, KK_VBO_TYPE_SNORM), snorm, v);
   v = nir_bcsel(b, nir_ieq_imm(b, type, KK_VBO_TYPE_USCALED), uscaled, v);
   v = nir_bcsel(b, nir_ieq_imm(b, type, KK_VBO_TYPE_SSCALED), sscaled, v);
   return v;
}

static nir_def *
convert_int(nir_builder *b, nir_def *fmt, nir_def *raw, nir_def *u, nir_def *s)
{
   static const unsigned bits_rgb10a2[4] = {10, 10, 10, 2};

   nir_def *rgb10a2 = nir_test_mask(b, fmt, KK_VBO_FMT_RGB10A2);
   u =
      nir_bcsel(b, rgb10a2, nir_format_unpack_uint(b, raw, bits_rgb10a2, 4), u);
   s =
      nir_bcsel(b, rgb10a2, nir_format_unpack_sint(b, raw, bits_rgb10a2, 4), s);

   nir_def *type =
      fmt_field(b, fmt, KK_VBO_FMT_TYPE_SHIFT, KK_VBO_FMT_TYPE_MASK);
   return nir_bcsel(b, nir_ieq_imm(b, type, KK_VBO_TYPE_SINT), s, u);
}

static bool
pass(struct nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_input)
      return false;

   struct ctx *ctx = data;
   b->cursor = nir_instr_remove(&intr->instr);

   nir_src *offset_src = nir_get_io_offset_src(intr);
   assert(nir_src_is_const(*offset_src) && "no attribute indirects");
   unsigned index = nir_intrinsic_base(intr) + nir_src_as_uint(*offset_src);

   nir_alu_type base_type =
      nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr));
   bool is_int = base_type == nir_type_int || base_type == nir_type_uint;

   nir_def *argbuf = nir_load_buffer_ptr_kk(b, 1, 64, .binding = 0);
   nir_def *fmt, *divisor, *stride;
   if (ctx->attribs) {
      struct kk_attribute a = ctx->attribs[index];
      uint32_t encoded = kk_vbo_encode_format(a.format);
      if (a.instanced)
         encoded |= KK_VBO_FMT_INSTANCED;
      fmt = nir_imm_int(b, encoded);
      divisor = nir_imm_int(b, a.divisor);
      stride = load_root(b, argbuf, 32,
         offsetof(struct kk_root_descriptor_table, draw.buffer_strides[a.binding]));
   } else {
      nir_def *vi = load_root(b, argbuf, 64,
         offsetof(struct kk_root_descriptor_table, draw.vertex_input));
      vi = nir_iadd_imm(b, vi, index * sizeof(struct kk_vertex_fetch));
      fmt = load_root(b, vi, 32, offsetof(struct kk_vertex_fetch, format));
      divisor = load_root(b, vi, 32, offsetof(struct kk_vertex_fetch, divisor));
      stride = load_root(b, vi, 32, offsetof(struct kk_vertex_fetch, stride));
   }
   nir_def *base = load_root(b, argbuf, 64,
      offsetof(struct kk_root_descriptor_table, draw.attrib_base[index]));

   nir_def *instance_el = nir_bcsel(
      b, nir_ieq_imm(b, divisor, 0), nir_imm_int(b, 0),
      nir_udiv(b, nir_load_instance_id(b), nir_umax_imm(b, divisor, 1)));
   instance_el = nir_iadd(b, instance_el, nir_load_base_instance(b));
   nir_def *el = nir_bcsel(b, nir_test_mask(b, fmt, KK_VBO_FMT_INSTANCED),
                           instance_el, nir_load_vertex_id(b));

   BITSET_SET(b->shader->info.system_values_read, SYSTEM_VALUE_INSTANCE_ID);
   BITSET_SET(b->shader->info.system_values_read, SYSTEM_VALUE_BASE_INSTANCE);
   BITSET_SET(b->shader->info.system_values_read, SYSTEM_VALUE_VERTEX_ID);

   nir_def *valid = nir_test_mask(b, fmt, KK_VBO_FMT_VALID);
   nir_def *fetch = nir_iand(b, valid, nir_ine_imm(b, base, 0));
   if (ctx->requires_robustness2) {
      nir_def *bounds = load_root(
         b, argbuf, 32,
         offsetof(struct kk_root_descriptor_table, draw.attrib_clamps[index]));
      fetch = nir_iand(b, fetch, nir_uge(b, bounds, el));
   }

   nir_def *addr = nir_iadd(b, base, nir_umul_2x32_64(b, el, stride));
   nir_def *size_log2 =
      fmt_field(b, fmt, KK_VBO_FMT_SIZE_SHIFT, KK_VBO_FMT_SIZE_MASK);
   nir_def *count_m1 =
      fmt_field(b, fmt, KK_VBO_FMT_COUNT_SHIFT, KK_VBO_FMT_COUNT_MASK);

   nir_def *zero = nir_imm_int(b, 0);
   nir_def *loaded[4];
   nir_push_if(b, fetch);
   {
      loaded[0] = fetch_component(b, addr, size_log2, 0);
      for (unsigned i = 1; i < 4; ++i) {
         nir_push_if(b, nir_uge_imm(b, count_m1, i));
         nir_def *c = fetch_component(b, addr, size_log2, i);
         nir_pop_if(b, NULL);
         loaded[i] = nir_if_phi(b, c, zero);
      }
   }
   nir_pop_if(b, NULL);

   nir_def *raw[4];
   for (unsigned i = 0; i < 4; ++i)
      raw[i] = nir_if_phi(b, loaded[i], zero);

   nir_def *u = nir_vec(b, raw, 4);
   nir_def *s =
      nir_bcsel(b, nir_ieq_imm(b, size_log2, 0), nir_i2i32(b, nir_u2u8(b, u)),
                nir_bcsel(b, nir_ieq_imm(b, size_log2, 1),
                          nir_i2i32(b, nir_u2u16(b, u)), u));

   nir_def *memory = is_int ? convert_int(b, fmt, raw[0], u, s)
                            : convert_float(b, fmt, size_log2, raw[0], u, s);
   nir_def *zero_out = is_int ? zero : nir_imm_float(b, 0.0);
   nir_def *one_out = is_int ? nir_imm_int(b, 1) : nir_imm_float(b, 1.0);

   nir_def *channels[4] = {NULL};
   for (unsigned i = 0; i < intr->num_components; ++i) {
      unsigned c = nir_intrinsic_component(intr) + i;
      nir_def *swz = fmt_field(b, fmt, KK_VBO_FMT_SWIZZLE_SHIFT + c * 3,
                               KK_VBO_FMT_SWIZZLE_MASK);

      nir_def *ch = nir_channel(b, memory, 0);
      for (unsigned k = 1; k < 4; ++k)
         ch = nir_bcsel(b, nir_ieq_imm(b, swz, PIPE_SWIZZLE_X + k),
                        nir_channel(b, memory, k), ch);
      ch = nir_bcsel(b, nir_ieq_imm(b, swz, PIPE_SWIZZLE_0), zero_out, ch);
      ch = nir_bcsel(b, nir_ieq_imm(b, swz, PIPE_SWIZZLE_1), one_out, ch);
      ch = nir_bcsel(b, valid, ch, c == 3 ? one_out : zero_out);

      if (intr->def.bit_size != 32)
         ch = is_int ? nir_u2uN(b, ch, intr->def.bit_size)
                     : nir_f2fN(b, ch, intr->def.bit_size);
      channels[i] = ch;
   }

   nir_def *logical = nir_vec(b, channels, intr->num_components);
   nir_def_rewrite_uses(&intr->def, logical);
   return true;
}

bool
kk_nir_lower_vbo(nir_shader *nir, struct kk_attribute *attribs, bool robustness2)
{
   assert(nir->info.stage == MESA_SHADER_VERTEX);
   struct ctx ctx = {attribs, robustness2};
   return nir_shader_intrinsics_pass(nir, pass, nir_metadata_none, &ctx);
}

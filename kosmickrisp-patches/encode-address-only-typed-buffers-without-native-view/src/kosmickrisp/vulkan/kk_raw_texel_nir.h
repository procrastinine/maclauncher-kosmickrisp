/* SPDX-License-Identifier: MIT */
#ifndef KK_RAW_TEXEL_NIR_H
#define KK_RAW_TEXEL_NIR_H

#include "compiler/nir/nir_format_convert.h"
#include "vk_format.h"

static bool lower_image_intrin(nir_builder *, nir_intrinsic_instr *,
                              const struct lower_descriptors_ctx *);
static bool lower_tex(nir_builder *, nir_tex_instr *,
                      const struct lower_descriptors_ctx *);

enum kk_raw_texel_operation {
   KK_RAW_TEXEL_READ,
   KK_RAW_TEXEL_WRITE,
   KK_RAW_TEXEL_ATOMIC,
   KK_RAW_TEXEL_SWAP,
};

struct kk_raw_texel_access {
   enum kk_raw_texel_operation operation;
   nir_def *base, *coord, *tail, *data, *compare;
   enum gl_access_qualifier access;
   nir_atomic_op atomic_op;
   nir_alu_type type;
   unsigned components, bits;
   bool sparse_result;
};

static bool
kk_raw_format_matches(enum pipe_format format, const struct kk_raw_texel_access *op)
{
   const bool wide = format == PIPE_FORMAT_R64_UINT || format == PIPE_FORMAT_R64_SINT;
   /* Mesa's reverse Vk mapping omits these storage-only R64 formats. */
   if (!kk_raw_texel_format_supported(format) ||
       (!wide && vk_format_from_pipe_format(format) == VK_FORMAT_UNDEFINED))
      return false;
   if (wide != (op->bits == 64))
      return false;
   if (op->operation == KK_RAW_TEXEL_ATOMIC || op->operation == KK_RAW_TEXEL_SWAP)
      return wide || format == PIPE_FORMAT_R32_UINT || format == PIPE_FORMAT_R32_SINT;
   const nir_alu_type base = nir_alu_type_get_base_type(op->type);
   if (util_format_is_pure_uint(format) != (base == nir_type_uint) ||
       util_format_is_pure_sint(format) != (base == nir_type_int))
      return false;
   if (wide || kk_format_is_rgb32(format))
      return true;
   const struct kk_va_format *native = kk_get_va_format(format);
   return native && (op->operation == KK_RAW_TEXEL_WRITE
                     ? native->texel_buffer.write : native->texel_buffer.read);
}

/* Split only at a real format component/packed-word boundary. This never
 * reads or overwrites padding beyond a three-component logical texel. */
static unsigned
kk_raw_texel_unit(enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);
   unsigned bytes = util_format_get_blocksize(format);
   unsigned unit = MIN2(4u, bytes & -bytes);
   if (desc->is_array && desc->channel[0].size >= 8)
      unit = MIN2(unit, desc->channel[0].size / 8);
   return unit;
}

static nir_def *
kk_raw_unit_resident(nir_builder *b, const struct kk_raw_texel_access *op,
                     unsigned texel_bytes, unsigned offset, unsigned size)
{
   nir_def *map = nir_pack_64_2x32_split(b, nir_channel(b, op->tail, 0),
                                          nir_channel(b, op->tail, 1));
   nir_def *byte = nir_iadd_imm(b,
      nir_iadd(b, nir_imul_imm(b, nir_u2u64(b, op->coord), texel_bytes),
               nir_u2u64(b, nir_channel(b, op->tail, 2))), offset);
   nir_def *shifted = nir_bcsel(b, nir_ine_imm(b, map, 0),
      nir_iadd(b, map, nir_ushr_imm(b, byte, 16)), nir_imm_int64(b, 0));
   nir_def *metadata = nir_vec4(b, nir_unpack_64_2x32_split_x(b, shifted),
      nir_unpack_64_2x32_split_y(b, shifted),
      nir_u2u32(b, nir_iand_imm(b, byte, 65535)), nir_imm_int(b, size));
   return kk_nir_sparse_buffer_resident(b, metadata, nir_imm_int(b, 0));
}

static nir_def *
kk_raw_read_unit(nir_builder *b, const struct kk_raw_texel_access *op,
                 nir_def *address, unsigned bytes, unsigned offset, unsigned unit)
{
   nir_if *present = nir_push_if(b, kk_raw_unit_resident(b, op, bytes, offset, unit));
   nir_def *value = nir_load_global(b, 1, unit * 8, nir_iadd_imm(b, address, offset),
      .align_mul = unit, .access = op->access & ~ACCESS_CAN_SPECULATE);
   nir_push_else(b, present);
   nir_def *zero = nir_imm_intN_t(b, 0, unit * 8);
   nir_pop_if(b, present);
   return nir_if_phi(b, value, zero);
}

static nir_def *
kk_raw_format_access(nir_builder *b, enum pipe_format format,
                      const struct kk_raw_texel_access *op)
{
   const unsigned bytes = util_format_get_blocksize(format);
   nir_def *address = nir_iadd(b, op->base,
      nir_imul_imm(b, nir_u2u64(b, op->coord), bytes));
   nir_def *value = NULL;
   if (op->operation == KK_RAW_TEXEL_ATOMIC || op->operation == KK_RAW_TEXEL_SWAP) {
      nir_if *present = nir_push_if(b, kk_raw_unit_resident(b, op, bytes, 0, bytes));
      if (op->operation == KK_RAW_TEXEL_SWAP)
         value = nir_global_atomic_swap(b, op->bits, address, op->data, op->compare,
            .atomic_op = op->atomic_op, .access = op->access);
      else
         value = nir_global_atomic(b, op->bits, address, op->data,
            .atomic_op = op->atomic_op, .access = op->access);
      nir_push_else(b, present);
      nir_def *zero = nir_imm_zero(b, 1, op->bits);
      nir_pop_if(b, present);
      return nir_if_phi(b, value, zero);
   }
   if (op->bits == 64) {
      if (op->operation == KK_RAW_TEXEL_WRITE) {
         nir_if *present = nir_push_if(b, kk_raw_unit_resident(b, op, bytes, 0, 8));
         nir_store_global(b, nir_channel(b, op->data, 0), address,
            .align_mul = 8, .access = op->access, .write_mask = 1);
         nir_pop_if(b, present);
         return NULL;
      }
      nir_def *channel = kk_raw_read_unit(b, op, address, bytes, 0, 8);
      value = nir_vec4(b, channel, nir_imm_int64(b, 0), nir_imm_int64(b, 0),
                                  nir_imm_int64(b, 1));
   } else {
      const unsigned unit = kk_raw_texel_unit(format);
      if (op->operation == KK_RAW_TEXEL_WRITE) {
         const nir_alu_type base_type = nir_alu_type_get_base_type(op->type);
         nir_def *color = base_type == nir_type_float ? nir_f2f32(b, op->data) :
            base_type == nir_type_int ? nir_i2i32(b, op->data) : nir_u2u32(b, op->data);
         nir_def *packed = nir_format_pack_rgba(b, format, color);
         for (unsigned offset = 0; offset < bytes; offset += unit) {
            nir_def *word = nir_channel(b, packed, offset / 4);
            word = nir_ushr_imm(b, word, (offset % 4) * 8);
            nir_if *present = nir_push_if(b,
               kk_raw_unit_resident(b, op, bytes, offset, unit));
            nir_store_global(b, nir_u2uN(b, word, unit * 8),
               nir_iadd_imm(b, address, offset), .align_mul = unit,
               .access = op->access, .write_mask = 1);
            nir_pop_if(b, present);
         }
         return NULL;
      }
      nir_def *words[4];
      for (unsigned i = 0; i < DIV_ROUND_UP(bytes, 4); ++i)
         words[i] = nir_imm_int(b, 0);
      for (unsigned offset = 0; offset < bytes; offset += unit) {
         nir_def *part = kk_raw_read_unit(b, op, address, bytes, offset, unit);
         words[offset / 4] = nir_ior(b, words[offset / 4],
            nir_ishl_imm(b, nir_u2u32(b, part), (offset % 4) * 8));
      }
      value = nir_format_unpack_rgba(b,
         nir_vec(b, words, DIV_ROUND_UP(bytes, 4)), format);
      if (op->bits != 32)
         value = nir_alu_type_get_base_type(op->type) == nir_type_float
            ? nir_f2fN(b, value, op->bits) : nir_u2uN(b, value, op->bits);
   }
   unsigned colors = op->components - op->sparse_result;
   value = nir_trim_vector(b, value, colors);
   if (op->sparse_result) {
      nir_def *parts[5];
      for (unsigned c = 0; c < colors; ++c)
         parts[c] = nir_channel(b, value, c);
      nir_def *resident = kk_nir_sparse_buffer_resident(b, op->tail, op->coord);
      parts[colors] = nir_u2uN(b, nir_b2i32(b, nir_inot(b, resident)), op->bits);
      value = nir_vec(b, parts, op->components);
   }
   return value;
}

static nir_def *
kk_raw_format_dispatch(nir_builder *b, nir_def *format,
   const struct kk_raw_texel_access *op, const enum pipe_format *formats,
   unsigned count)
{
   if (!count)
      return op->operation == KK_RAW_TEXEL_WRITE ? NULL :
         nir_undef(b, op->components, op->bits);
   nir_if *choice = nir_push_if(b, nir_ieq_imm(b, format, formats[0]));
   nir_def *then_value = kk_raw_format_access(b, formats[0], op);
   nir_push_else(b, choice);
   nir_def *else_value = kk_raw_format_dispatch(b, format, op, formats + 1, count - 1);
   nir_pop_if(b, choice);
   return op->operation == KK_RAW_TEXEL_WRITE ? NULL :
      nir_if_phi(b, then_value, else_value);
}

static nir_def *
kk_raw_access(nir_builder *b, nir_def *word, nir_def *count,
               enum pipe_format known, const struct kk_raw_texel_access *op)
{
   nir_def *valid = nir_iand(b, nir_ine_imm(b, op->base, 0),
                               nir_ult(b, op->coord, count));
   nir_if *bounds = nir_push_if(b, valid);
   nir_def *value;
   if (known != PIPE_FORMAT_NONE) {
      assert(kk_raw_format_matches(known, op));
      value = kk_raw_format_access(b, known, op);
   } else {
      enum pipe_format formats[PIPE_FORMAT_COUNT];
      unsigned n = 0;
      for (enum pipe_format f = PIPE_FORMAT_NONE + 1; f < PIPE_FORMAT_COUNT; ++f) {
         if (kk_raw_format_matches(f, op))
            formats[n++] = f;
      }
      assert(n > 0);
      value = kk_raw_format_dispatch(b,
         nir_iand_imm(b, word, KK_TEXEL_BUFFER_FORMAT_MASK), op, formats, n);
   }
   nir_push_else(b, bounds);
   nir_def *zero = NULL;
   if (op->operation != KK_RAW_TEXEL_WRITE) {
      zero = nir_imm_zero(b, op->components, op->bits);
      if (op->sparse_result)
         zero = nir_vector_insert_imm(b, zero,
            nir_imm_intN_t(b, 1, op->bits), op->components - 1);
   }
   nir_pop_if(b, bounds);
   return op->operation == KK_RAW_TEXEL_WRITE ? NULL : nir_if_phi(b, value, zero);
}

/* Keep an explicit use so native lowering can replace/remove a cloned result
 * while the enclosing raw/native phi continues to follow that replacement. */
static nir_def *
kk_raw_capture_result(nir_builder *b, nir_def *value)
{
   nir_alu_instr *move = nir_alu_instr_create(b->shader, nir_op_mov);
   move->src[0].src = nir_src_for_ssa(value);
   for (unsigned c = 0; c < value->num_components; ++c)
      move->src[0].swizzle[c] = c;
   nir_def_init(&move->instr, &move->def, value->num_components, value->bit_size);
   nir_builder_instr_insert(b, &move->instr);
   return &move->def;
}

static bool
kk_lower_raw_image(nir_builder *b, nir_intrinsic_instr *intr,
                    const struct lower_descriptors_ctx *ctx)
{
   if (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_BUF)
      return false;
   struct kk_raw_texel_access op = {0};
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_sparse_load: op.operation = KK_RAW_TEXEL_READ; break;
   case nir_intrinsic_image_deref_store: op.operation = KK_RAW_TEXEL_WRITE; break;
   case nir_intrinsic_image_deref_atomic: op.operation = KK_RAW_TEXEL_ATOMIC; break;
   case nir_intrinsic_image_deref_atomic_swap: op.operation = KK_RAW_TEXEL_SWAP; break;
   default: return false;
   }
   const bool store = op.operation == KK_RAW_TEXEL_WRITE;
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_def *metadata = load_resource_deref_desc(b, 2, 32, deref, 8, ctx);
   op.base = load_resource_deref_desc(b, 1, 64, deref, 0, ctx);
   op.coord = nir_channel(b, intr->src[1].ssa, 0);
   op.tail = load_resource_deref_desc(b, 4, 32, deref,
      sizeof(struct kk_storage_image_descriptor), ctx);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   op.access = nir_intrinsic_access(intr) | (var ? var->data.access : 0);
   op.type = store ? nir_intrinsic_src_type(intr) :
      nir_intrinsic_has_atomic_op(intr)
         ? nir_atomic_op_type(nir_intrinsic_atomic_op(intr)) | intr->def.bit_size
         : nir_intrinsic_dest_type(intr);
   op.components = store ? 0 : intr->def.num_components;
   op.bits = store ? intr->src[3].ssa->bit_size : intr->def.bit_size;
   op.sparse_result = intr->intrinsic == nir_intrinsic_image_deref_sparse_load;
   if (store || op.operation == KK_RAW_TEXEL_ATOMIC || op.operation == KK_RAW_TEXEL_SWAP)
      op.data = intr->src[3].ssa;
   if (op.operation == KK_RAW_TEXEL_SWAP)
      op.compare = intr->src[4].ssa;
   if (op.operation == KK_RAW_TEXEL_ATOMIC || op.operation == KK_RAW_TEXEL_SWAP)
      op.atomic_op = nir_intrinsic_atomic_op(intr);
   nir_if *mode = nir_push_if(b,
      nir_test_mask(b, nir_channel(b, metadata, 0), KK_TEXEL_BUFFER_RAW_ADDRESS));
   nir_def *raw = kk_raw_access(b, nir_channel(b, metadata, 0),
      nir_channel(b, metadata, 1), nir_intrinsic_format(intr), &op);
   nir_push_else(b, mode);
   nir_intrinsic_instr *native = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &intr->instr));
   nir_builder_instr_insert(b, &native->instr);
   nir_def *native_value = store ? NULL : kk_raw_capture_result(b, &native->def);
   lower_image_intrin(b, native, ctx);
   b->cursor = nir_after_cf_list(&mode->else_list);
   nir_pop_if(b, mode);
   if (store)
      nir_instr_remove(&intr->instr);
   else
      nir_def_replace(&intr->def, nir_if_phi(b, raw, native_value));
   return true;
}

static bool
kk_lower_raw_tex(nir_builder *b, nir_tex_instr *tex,
                  const struct lower_descriptors_ctx *ctx)
{
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_BUF ||
       tex->op != nir_texop_txf)
      return false;
   nir_deref_instr *deref = nir_get_tex_deref(tex, nir_tex_src_texture_deref);
   if (!deref)
      return false;
   int coordinate = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   assert(coordinate >= 0);
   nir_def *metadata = load_resource_deref_desc(b, 2, 32, deref, 8, ctx);
   struct kk_raw_texel_access op = {
      .operation = KK_RAW_TEXEL_READ,
      .base = load_resource_deref_desc(b, 1, 64, deref, 0, ctx),
      .coord = nir_channel(b, tex->src[coordinate].src.ssa, 0),
      .tail = load_resource_deref_desc(b, 4, 32, deref,
                  sizeof(struct kk_storage_image_descriptor), ctx),
      .access = nir_deref_instr_get_variable(deref)
         ? nir_deref_instr_get_variable(deref)->data.access : 0,
      .type = tex->dest_type,
      .bits = tex->def.bit_size,
      .components = tex->def.num_components,
      .sparse_result = tex->is_sparse,
   };
   nir_if *mode = nir_push_if(b,
      nir_test_mask(b, nir_channel(b, metadata, 0), KK_TEXEL_BUFFER_RAW_ADDRESS));
   nir_def *raw = kk_raw_access(b, nir_channel(b, metadata, 0),
      nir_channel(b, metadata, 1), PIPE_FORMAT_NONE, &op);
   nir_push_else(b, mode);
   nir_tex_instr *native = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   nir_builder_instr_insert(b, &native->instr);
   nir_def *native_value = kk_raw_capture_result(b, &native->def);
   lower_tex(b, native, ctx);
   b->cursor = nir_after_cf_list(&mode->else_list);
   nir_pop_if(b, mode);
   nir_def_replace(&tex->def, nir_if_phi(b, raw, native_value));
   return true;
}
#endif

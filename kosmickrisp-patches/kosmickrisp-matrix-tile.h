/* SPDX-License-Identifier: MIT
 * Matrix temporary remapping follows Mesa lavapipe's cooperative-matrix
 * lowering (Copyright 2025 Red Hat), at Mesa 4f071f891d63396c799b267172c19ba6b0301682.
 * Metal owns the per-lane distribution of every native 8x8 fragment.
 */
#pragma once
#include "nir_builder.h"

static bool
kk_matrix_tile_desc(struct glsl_cmat_description d)
{
   return d.scope == SCOPE_SUBGROUP && d.rows == d.cols &&
          (d.rows == 8 || d.rows == 16) &&
          (d.element_type == GLSL_TYPE_FLOAT16 || d.element_type == GLSL_TYPE_FLOAT ||
           d.element_type == GLSL_TYPE_FLOAT_E4M3FN);
}

static unsigned
kk_matrix_components(struct glsl_cmat_description d)
{
   assert(kk_matrix_tile_desc(d));
   return d.rows * d.cols / 32;
}

static struct glsl_cmat_description
kk_matrix_description(nir_src src)
{
   return *glsl_get_cmat_description(nir_src_as_deref(src)->type);
}

static const struct glsl_type *
kk_matrix_tile_type(const struct glsl_type *type)
{
   if (glsl_type_is_cmat(type)) {
      struct glsl_cmat_description d = *glsl_get_cmat_description(type);
      enum glsl_base_type element = d.element_type == GLSL_TYPE_FLOAT_E4M3FN ? GLSL_TYPE_UINT8 : d.element_type;
      return glsl_vector_type(element, kk_matrix_components(d));
   }
   if (glsl_type_is_array(type)) {
      const struct glsl_type *element = glsl_get_array_element(type);
      const struct glsl_type *lowered = kk_matrix_tile_type(element);
      if (element != lowered)
         return glsl_array_type(lowered, glsl_get_length(type), glsl_get_explicit_stride(type));
   }
   return type;
}

static nir_def *
kk_matrix_tile_load(nir_builder *b, nir_src source)
{
   nir_deref_instr *deref = nir_src_as_deref(source);
   struct glsl_cmat_description d = kk_matrix_description(source);
   return nir_build_load_deref(b, kk_matrix_components(d), glsl_base_type_bit_size(d.element_type), &deref->def, 0);
}

static nir_def *
kk_matrix_fragment(nir_builder *b, nir_def *matrix, unsigned fragment)
{
   return nir_vec2(b, nir_channel(b, matrix, 2 * fragment), nir_channel(b, matrix, 2 * fragment + 1));
}

static nir_def *
kk_matrix_to_float(nir_builder *b, nir_def *value, enum glsl_base_type type, unsigned bits)
{
   if (type == GLSL_TYPE_FLOAT_E4M3FN)
      value = nir_e4m3fn2f(b, value);
   return nir_f2fN(b, value, bits);
}

static bool
kk_matrix_tile_intrinsic(nir_builder *b, nir_intrinsic_instr *intr)
{
   nir_def *value = NULL;
   switch (intr->intrinsic) {
   case nir_intrinsic_cmat_length:
      nir_def_replace(&intr->def, nir_imm_int(b, kk_matrix_components(nir_intrinsic_cmat_desc(intr))));
      return true;
   case nir_intrinsic_cmat_construct: {
      unsigned n = kk_matrix_components(kk_matrix_description(intr->src[0]));
      value = nir_replicate(b, intr->src[1].ssa, n);
      break;
   }
   case nir_intrinsic_cmat_bitcast:
   case nir_intrinsic_cmat_copy:
      value = kk_matrix_tile_load(b, intr->src[1]);
      break;
   case nir_intrinsic_cmat_muladd: {
      assert(!nir_intrinsic_saturate(intr));
      struct glsl_cmat_description d = kk_matrix_description(intr->src[0]);
      struct glsl_cmat_description ad = kk_matrix_description(intr->src[1]);
      struct glsl_cmat_description bd = kk_matrix_description(intr->src[2]);
      struct glsl_cmat_description cd = kk_matrix_description(intr->src[3]);
      unsigned bits = glsl_base_type_bit_size(d.element_type);
      assert(bits == 16 || bits == 32);
      nir_def *a = kk_matrix_tile_load(b, intr->src[1]);
      nir_def *c = kk_matrix_tile_load(b, intr->src[2]);
      nir_def *acc = kk_matrix_tile_load(b, intr->src[3]);
      if (ad.element_type == GLSL_TYPE_FLOAT_E4M3FN)
         a = kk_matrix_to_float(b, a, ad.element_type, 16);
      if (bd.element_type == GLSL_TYPE_FLOAT_E4M3FN)
         c = kk_matrix_to_float(b, c, bd.element_type, 16);
      acc = kk_matrix_to_float(b, acc, cd.element_type, bits);
      nir_def *components[8];
      unsigned tiles = d.rows / 8;
      for (unsigned row = 0; row < tiles; row++) {
         for (unsigned col = 0; col < tiles; col++) {
            unsigned frag = row * tiles + col;
            nir_def *v = kk_matrix_fragment(b, acc, frag);
            for (unsigned k = 0; k < tiles; k++)
               v = nir_cmat_mma_kk(b, bits, kk_matrix_fragment(b, a, row * tiles + k),
                                   kk_matrix_fragment(b, c, k * tiles + col), v);
            components[2 * frag] = nir_channel(b, v, 0);
            components[2 * frag + 1] = nir_channel(b, v, 1);
         }
      }
      value = nir_vec(b, components, kk_matrix_components(d));
      break;
   }
   case nir_intrinsic_cmat_load:
   case nir_intrinsic_cmat_store: {
      bool load = intr->intrinsic == nir_intrinsic_cmat_load;
      nir_deref_instr *pointer = nir_src_as_deref(intr->src[load]);
      struct glsl_cmat_description d = kk_matrix_description(intr->src[!load]);
      unsigned bits = glsl_base_type_bit_size(d.element_type);
      bool shared = nir_deref_mode_is(pointer, nir_var_mem_shared);
      assert(nir_deref_mode_is_in_set(pointer, nir_var_mem_ssbo | nir_var_mem_global | nir_var_mem_shared));
      /* Shared derefs become byte offsets in the normal explicit-I/O pass.
       * deref_buffer_address instead requests an SSBO address for an offset
       * address format, so it applies only to device/global memory here. */
      nir_def *address = shared ? &pointer->def : nir_deref_buffer_address(b, 1, 64, &pointer->def);
      unsigned pointee_bytes = glsl_get_bit_size(pointer->type) / 8 * glsl_get_vector_elements(pointer->type);
      nir_def *stride = nir_imul_imm(b, intr->src[2].ssa, pointee_bytes);
      enum glsl_matrix_layout layout = nir_intrinsic_matrix_layout(intr);
      bool column = layout == GLSL_MATRIX_LAYOUT_COLUMN_MAJOR;
      nir_def *map = bits == 8 ? nir_load_cmat_map_kk(b) : nir_imm_int64(b, 0);
      nir_def *source = load ? NULL : kk_matrix_tile_load(b, intr->src[1]);
      nir_def *components[8];
      unsigned tiles = d.rows / 8;
      for (unsigned row = 0; row < tiles; row++) {
         for (unsigned col = 0; col < tiles; col++) {
            unsigned frag = row * tiles + col;
            nir_def *offset = nir_iadd(b, nir_imul_imm(b, stride, 8 * (column ? col : row)),
                                        nir_imm_int(b, 8 * (bits / 8) * (column ? row : col)));
            nir_def *addr = nir_iadd(b, address, nir_u2uN(b, offset, address->bit_size));
            if (load) {
               nir_def *tile = nir_load_cmat_kk(b, bits, addr, stride, map,
                                                .matrix_layout = layout,
                                                .memory_modes = shared ? nir_var_mem_shared : nir_var_mem_global);
               components[2 * frag] = nir_channel(b, tile, 0);
               components[2 * frag + 1] = nir_channel(b, tile, 1);
            } else {
               nir_store_cmat_kk(b, addr, kk_matrix_fragment(b, source, frag), stride, map,
                                   .matrix_layout = layout,
                                   .memory_modes = shared ? nir_var_mem_shared : nir_var_mem_global);
            }
         }
      }
      if (!load) {
         nir_instr_remove(&intr->instr);
         return true;
      }
      value = nir_vec(b, components, kk_matrix_components(d));
      break;
   }
   case nir_intrinsic_cmat_extract:
      nir_def_replace(&intr->def, nir_vector_extract(b, kk_matrix_tile_load(b, intr->src[0]), intr->src[1].ssa));
      return true;
   case nir_intrinsic_cmat_insert:
      value = nir_vector_insert(b, kk_matrix_tile_load(b, intr->src[2]), intr->src[1].ssa, intr->src[3].ssa);
      break;
   case nir_intrinsic_cmat_unary_op:
      b->fp_math_ctrl = nir_intrinsic_fp_math_ctrl(intr);
      value = nir_build_alu1(b, nir_intrinsic_alu_op(intr), kk_matrix_tile_load(b, intr->src[1]));
      break;
   case nir_intrinsic_cmat_binary_op:
      b->fp_math_ctrl = nir_intrinsic_fp_math_ctrl(intr);
      value = nir_build_alu2(b, nir_intrinsic_alu_op(intr), kk_matrix_tile_load(b, intr->src[1]), kk_matrix_tile_load(b, intr->src[2]));
      break;
   case nir_intrinsic_cmat_scalar_op:
      b->fp_math_ctrl = nir_intrinsic_fp_math_ctrl(intr);
      value = nir_build_alu2(b, nir_intrinsic_alu_op(intr), kk_matrix_tile_load(b, intr->src[1]), intr->src[2].ssa);
      break;
   case nir_intrinsic_cmat_convert: {
      struct glsl_cmat_description d = kk_matrix_description(intr->src[0]);
      struct glsl_cmat_description s = kk_matrix_description(intr->src[1]);
      b->fp_math_ctrl = nir_intrinsic_fp_math_ctrl(intr);
      value = kk_matrix_tile_load(b, intr->src[1]);
      if (d.element_type != s.element_type) {
         if (d.element_type == GLSL_TYPE_FLOAT_E4M3FN) {
            value = kk_matrix_to_float(b, value, s.element_type, 32);
            value = nir_intrinsic_saturate(intr) ? nir_f2e4m3fn_sat(b, value) : nir_f2e4m3fn(b, value);
         } else {
            value = kk_matrix_to_float(b, value, s.element_type, glsl_base_type_bit_size(d.element_type));
         }
      }
      break;
   }
   default:
      return false;
   }
   nir_store_deref(b, nir_src_as_deref(intr->src[0]), value, BITFIELD_MASK(value->num_components));
   nir_instr_remove(&intr->instr);
   b->fp_math_ctrl = nir_fp_fast_math;
   return true;
}

static bool
kk_lower_matrix_tile(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_COMPUTE || !nir->info.cs.has_cooperative_matrix)
      return false;
   bool progress = false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_function_temp_variable(var, impl)
         var->type = kk_matrix_tile_type(var->type);
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block_reverse_safe(block, impl) {
         nir_foreach_instr_reverse_safe(instr, block) {
            if (instr->type == nir_instr_type_intrinsic) {
               b.cursor = nir_before_instr(instr);
               progress |= kk_matrix_tile_intrinsic(&b, nir_instr_as_intrinsic(instr));
            } else if (instr->type == nir_instr_type_deref) {
               nir_deref_instr *deref = nir_instr_as_deref(instr);
               deref->type = kk_matrix_tile_type(deref->type);
            }
         }
      }
      nir_progress(progress, impl, nir_metadata_none);
   }
   return progress;
}

static uint8_t
kk_matrix_phi_width(const nir_instr *instr, UNUSED const void *data)
{
   /* A 16x16 matrix has eight opaque elements per invocation. Keep phi
    * registers in native two-element fragments, within Metal's vector limit. */
   return nir_instr_as_phi(instr)->def.num_components > 4 ? 2 : 0;
}

static bool
kk_emit_matrix_tile(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *intr)
{
   if (intr->intrinsic != nir_intrinsic_load_cmat_kk &&
       intr->intrinsic != nir_intrinsic_store_cmat_kk &&
       intr->intrinsic != nir_intrinsic_cmat_mma_kk)
      return false;
   P_IND(ctx, "{\n");
   ctx->indentlevel++;
   if (intr->intrinsic == nir_intrinsic_cmat_mma_kk) {
      for (unsigned i = 0; i < 3; i++) {
         const char *scalar = intr->src[i].ssa->bit_size == 16 ? "half" : "float";
         P_IND(ctx, "simdgroup_matrix<%s, 8, 8> m%u;\n", scalar, i);
         for (unsigned c = 0; c < 2; c++) {
            P_IND(ctx, "m%u.thread_elements()[%u] = ", i, c);
            src_to_msl(ctx, &intr->src[i]);
            P(ctx, ".%c;\n", "xy"[c]);
         }
      }
      P_IND(ctx, "simdgroup_matrix<%s, 8, 8> result;\n", intr->def.bit_size == 16 ? "half" : "float");
      P_IND(ctx, "simdgroup_multiply_accumulate(result, m0, m1, m2);\n");
      P_IND(ctx, "t%u = %s2(result.thread_elements()[0], result.thread_elements()[1]);\n", intr->def.index, intr->def.bit_size == 16 ? "half" : "float");
   } else {
      bool load = intr->intrinsic == nir_intrinsic_load_cmat_kk;
      unsigned bits = load ? intr->def.bit_size : intr->src[1].ssa->bit_size;
      bool fp8 = bits == 8;
      bool shared = nir_intrinsic_memory_modes(intr) == nir_var_mem_shared;
      const char *space = shared ? "threadgroup" : "device";
      const char *scalar = bits == 16 ? "half" : "float";
      bool transpose = nir_intrinsic_matrix_layout(intr) == GLSL_MATRIX_LAYOUT_COLUMN_MAJOR;
      P_IND(ctx, "%s %s uchar* pointer = (%s %s uchar*)", load ? "const" : "", space, load ? "const" : "", space);
      if (shared) P(ctx, "&shared_data[");
      src_to_msl(ctx, &intr->src[0]);
      if (shared) P(ctx, "]");
      P(ctx, ";\n");
      if (fp8) {
         /* Discover element coordinates with a native load; no lane-layout
          * assumption, staging memory, barrier or hidden shared-memory use. */
         P_IND(ctx, "simdgroup_matrix<half, 8, 8> coordinates;\n");
         P_IND(ctx, "simdgroup_load(coordinates, (const device half*)");
         src_to_msl(ctx, &intr->src[load ? 2 : 3]);
         P(ctx, ", 8);\n");
         if (load) {
            P_IND(ctx, "uchar2 values;\n");
         }
         for (unsigned c = 0; c < 2; c++) {
            P_IND(ctx, "uint index%u = uint(coordinates.thread_elements()[%u]);\n", c, c);
            if (load) {
               P_IND(ctx, "values.%c = ", "xy"[c]);
            } else {
               P_INDENT(ctx);
            }
            P(ctx, "((%s %s uchar*)pointer)[(index%u %s 8) * ulong(", load ? "const" : "", space, c, transpose ? "%" : "/");
            src_to_msl(ctx, &intr->src[load ? 1 : 2]);
            P(ctx, ") + (index%u %s 8)]", c, transpose ? "/" : "%");
            if (!load) {
               P(ctx, " = ");
               src_to_msl(ctx, &intr->src[1]);
               P(ctx, ".%c", "xy"[c]);
            }
            P(ctx, ";\n");
         }
         if (load) {
            P_IND(ctx, "t%u = values;\n", intr->def.index);
         }
      } else {
         P_IND(ctx, "simdgroup_matrix<%s, 8, 8> tile;\n", scalar);
         if (!load) for (unsigned c = 0; c < 2; c++) {
            P_IND(ctx, "tile.thread_elements()[%u] = ", c);
            src_to_msl(ctx, &intr->src[1]);
            P(ctx, ".%c;\n", "xy"[c]);
         }
         P_IND(ctx, "simdgroup_%s(tile, (%s %s %s*)pointer, ulong(", load ? "load" : "store", load ? "const" : "", space, scalar);
         src_to_msl(ctx, &intr->src[load ? 1 : 2]);
         P(ctx, ") / %u, ulong2(0), %s);\n", bits / 8, transpose ? "true" : "false");
         if (load) {
            P_IND(ctx, "t%u = %s2(tile.thread_elements()[0], tile.thread_elements()[1]);\n", intr->def.index, scalar);
         }
      }
   }
   ctx->indentlevel--;
   P_IND(ctx, "}\n");
   return true;
}

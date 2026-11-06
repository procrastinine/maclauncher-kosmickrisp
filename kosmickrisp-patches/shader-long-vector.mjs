// SPDX-License-Identifier: MIT
// VTN portions: Copyright © 2015-2016 Intel Corporation.
import fs from 'node:fs';
export const id = 'shader-long-vector';
export const sourceFiles = ['kosmickrisp-patches/vtn-long-vector.h'];
export function apply({edit, edits}) {
  edits.set('src/compiler/spirv/vtn_long_vector.h', fs.readFileSync(new URL('./vtn-long-vector.h', import.meta.url), 'utf8'));
  edit('src/compiler/spirv/vtn_private.h', s => s.replace('   unsigned length;\n', '   unsigned length;\n   bool is_long_vector;\n'));
  edit('src/compiler/spirv/spirv_to_nir.c', s => s
    .replace('static const struct spirv_capabilities implemented_capabilities = {', 'static const struct spirv_capabilities implemented_capabilities = {\n   .LongVectorEXT = true,')
    .replace('   case SpvOpTypeVector: {\n      struct vtn_type *base', '   case SpvOpTypeVectorIdEXT:\n   case SpvOpTypeVector: {\n      struct vtn_type *base')
    .replace('      unsigned elems = w[3];\n', '      unsigned elems = opcode == SpvOpTypeVectorIdEXT ? vtn_constant_uint(b, w[3]) : w[3];\n')
    .replace('      vtn_fail_if((elems < 2 || elems > 4) && (elems != 8) && (elems != 16),', '      bool long_vector = b->enabled_capabilities.LongVectorEXT && (elems == 1 || elems > 4);\n      vtn_fail_if(long_vector ? (elems == 0 || elems > 1024) :\n                  ((elems < 2 || elems > 4) && (elems != 8) && (elems != 16)),')
    .replace('      val->type->base_type = vtn_base_type_vector;\n      val->type->type = glsl_vector_type(glsl_get_base_type(base->type), elems);', '      val->type->base_type = long_vector ? vtn_base_type_array : vtn_base_type_vector;\n      val->type->is_long_vector = long_vector;\n      unsigned stride = glsl_type_is_boolean(base->type) ? 4 : glsl_get_bit_size(base->type) / 8;\n      val->type->type = long_vector ? glsl_array_type(base->type, elems, stride) :\n         glsl_vector_type(glsl_get_base_type(base->type), elems);')
    .replace('      val->type->stride = glsl_type_is_boolean(val->type->type)\n         ? 4 : glsl_get_bit_size(base->type) / 8;', '      val->type->stride = stride;')
    .replace('   case SpvOpTypeVector:\n', '   case SpvOpTypeVectorIdEXT:\n   case SpvOpTypeVector:\n')
    .replace('static void\nvtn_handle_composite(', '#include "vtn_long_vector.h"\n\nstatic void\nvtn_handle_composite(')
    .replace('   struct vtn_ssa_value *ssa = vtn_create_ssa_value(b, type->type);\n\n   switch (opcode)', '   struct vtn_ssa_value *ssa = vtn_create_ssa_value(b, type->type);\n   if (vtn_long_vector_composite(b, opcode, w, count))\n      return;\n\n   switch (opcode)')
    .replace('dest->elems[i] = vtn_nir_select(b, src0,', 'dest->elems[i] = vtn_nir_select(b, glsl_type_is_array(src0->type) ? src0->elems[i] : src0,')
    .replace('cond_val->type->base_type != vtn_base_type_vector) ||\n               !glsl_type_is_boolean(cond_val->type->type),', 'cond_val->type->base_type != vtn_base_type_vector && !cond_val->type->is_long_vector) ||\n               !glsl_type_is_boolean(glsl_without_array(cond_val->type->type)),')
    .replace('cond_val->type->base_type == vtn_base_type_vector &&\n               (res_val->type->base_type != vtn_base_type_vector ||', '(cond_val->type->base_type == vtn_base_type_vector || cond_val->type->is_long_vector) &&\n               ((res_val->type->base_type != vtn_base_type_vector && !res_val->type->is_long_vector) ||'));
  edit('src/compiler/spirv/vtn_alu.c', s => {
    s = s.replace('void\nvtn_handle_alu(struct vtn_builder *b, SpvOp opcode,\n               const uint32_t *w, unsigned count)\n{', 'static struct vtn_ssa_value *\nvtn_alu_result(struct vtn_builder *b, SpvOp opcode,\n               struct vtn_value *dest_val, const struct glsl_type *dest_type,\n               struct vtn_ssa_value **vtn_src, unsigned num_inputs)\n{');
    const start = s.indexOf('   struct vtn_value *dest_val = vtn_untyped_value(b, w[2]);', s.indexOf('vtn_alu_result('));
    const end = s.indexOf('   const bool mediump_16bit =', start);
    s = s.replace(s.slice(start, end), '');
    s = s.replace('      vtn_push_ssa_value(b, w[2], dest);\n      b->nb.fp_math_ctrl = nir_fp_fast_math;\n      return;', '      return dest;');
    s = s.replace('   vtn_push_ssa_value(b, w[2], dest);\n\n   b->nb.fp_math_ctrl = nir_fp_fast_math;\n}', '   return dest;\n}');
    s = s.replace('void\nvtn_handle_integer_dot', aluWrapper + '\nvoid\nvtn_handle_integer_dot');
    s = s.replace('      base_type = glsl_get_base_type(type->type->fields.structure[0].type);', '      base_type = glsl_get_base_type(glsl_without_array(type->type->fields.structure[0].type));');
    s = s.replace('      base_type = glsl_get_base_type(type->type);', '      base_type = glsl_get_base_type(glsl_without_array(type->type));');
    s = s.replace('   struct nir_def *src = vtn_get_nir_ssa(b, w[3]);\n\n   vtn_fail_if(src->num_components', '   if (vtn_long_vector_bitcast(b, w)) return;\n   struct nir_def *src = vtn_get_nir_ssa(b, w[3]);\n\n   vtn_fail_if(src->num_components');
    s = s.replace('      vtn_assert(glsl_type_is_vector_or_scalar(vtn_src[i]->type));\n   }\n\n   /* For all of the opcodes', `      vtn_assert(glsl_type_is_vector_or_scalar(vtn_src[i]->type) || glsl_type_is_array(vtn_src[i]->type));
   }
   if (glsl_type_is_array(vtn_src[0]->type)) {
      bool unsigned0 = opcode == SpvOpUDotKHR || opcode == SpvOpUDotAccSatKHR;
      bool unsigned1 = unsigned0 || opcode == SpvOpSUDotKHR || opcode == SpvOpSUDotAccSatKHR;
      nir_def *sum = nir_imm_intN_t(&b->nb, 0, dest_size);
      unsigned length = glsl_get_length(vtn_src[0]->type);
      vtn_assert(length == glsl_get_length(vtn_src[1]->type));
      for (unsigned i = 0; i < length; i++) {
         nir_def *a = vtn_src[0]->elems[i]->def, *c = vtn_src[1]->elems[i]->def;
         a = unsigned0 ? nir_u2uN(&b->nb, a, dest_size) : nir_i2iN(&b->nb, a, dest_size);
         c = unsigned1 ? nir_u2uN(&b->nb, c, dest_size) : nir_i2iN(&b->nb, c, dest_size);
         sum = nir_iadd(&b->nb, sum, nir_imul(&b->nb, a, c));
      }
      if (num_inputs == 3) sum = unsigned0 ? nir_uadd_sat(&b->nb, sum, src[2]) : nir_iadd_sat(&b->nb, sum, src[2]);
      vtn_push_nir_ssa(b, w[2], sum);
      return;
   }

   /* For all of the opcodes`);
    return s.replace('#include "vtn_private.h"', '#include "vtn_private.h"\n#include "vtn_long_vector.h"');
  });
  edit('src/compiler/spirv/vtn_glsl450.c', s => {
    s = s.replace('static void\nhandle_glsl450_alu(struct vtn_builder *b, enum GLSLstd450 entrypoint,\n                   const uint32_t *w, unsigned count)', 'static struct vtn_ssa_value *\nvtn_glsl450_alu_result(struct vtn_builder *b, enum GLSLstd450 entrypoint,\n                      const struct glsl_type *dest_type, struct vtn_value *dest_val,\n                      struct vtn_ssa_value **vtn_src, unsigned num_inputs)');
    s = s.replace('   const struct glsl_type *dest_type = vtn_get_type(b, w[1])->type;\n\n   const bool mediump_16bit =\n      vtn_value_needs_mediump_16bit(b, vtn_untyped_value(b, w[2]));\n\n   unsigned num_inputs = count - 5;\n   struct vtn_ssa_value *vtn_src[3] = {0};\n   vtn_collect_ssa_sources(b, &w[5], vtn_src, num_inputs);', '   const bool mediump_16bit = vtn_value_needs_mediump_16bit(b, dest_val);');
    s = s.replace('   vtn_push_ssa_value(b, w[2], dest);\n}\n\nstatic void\nhandle_glsl450_interpolation', '   return dest;\n}\n' + glslWrapper + '\nstatic void\nhandle_glsl450_interpolation');
    s = s.replace('   nir_def *src = vtn_src->def;\n\n   struct vtn_ssa_value *dest = vtn_create_ssa_value(b, dest_type);\n\n   switch (opcode)', `   if (glsl_type_is_array(vtn_src->type)) {
      struct vtn_ssa_value *dest = vtn_create_ssa_value(b, dest_type);
      bool pointer_result = opcode == GLSLstd450Modf || opcode == GLSLstd450Frexp;
      struct vtn_pointer *ptr = pointer_result ? vtn_value(b, w[6], vtn_value_type_pointer)->pointer : NULL;
      struct vtn_ssa_value *whole = ptr ? vtn_create_ssa_value(b, ptr->type->pointed->type) : NULL;
      for (unsigned i = 0; i < glsl_get_length(vtn_src->type); i++) {
         nir_def *src = vtn_src->elems[i]->def;
         nir_def *part, *integer;
         if (opcode == GLSLstd450Frexp || opcode == GLSLstd450FrexpStruct) {
            part = nir_frexp_sig(nb, src);
            const glsl_type *integer_type = ptr ? whole->elems[i]->type : dest->elems[1]->elems[i]->type;
            integer = nir_i2iN(nb, nir_frexp_exp(nb, src), glsl_get_bit_size(integer_type));
         } else {
            nir_def *inf = nir_imm_floatN_t(nb, INFINITY, src->bit_size);
            nir_def *sign = nir_imm_intN_t(nb, (uint64_t)1 << (src->bit_size-1), src->bit_size);
            nir_def *signed_zero = nir_iand(nb, src, sign);
            nir_def *abs = nir_fabs(nb, src);
            part = nir_bcsel(nb, nir_ieq(nb, abs, inf), signed_zero, nir_ior(nb, signed_zero, nir_ffract(nb, abs)));
            integer = nir_ior(nb, signed_zero, nir_ffloor(nb, abs));
         }
         if (ptr) { dest->elems[i]->def = part; whole->elems[i]->def = integer; }
         else { dest->elems[0]->elems[i]->def = part; dest->elems[1]->elems[i]->def = integer; }
      }
      if (ptr) vtn_variable_store(b, whole, ptr, 0);
      vtn_push_ssa_value(b, w[2], dest);
      return;
   }
   nir_def *src = vtn_src->def;

   struct vtn_ssa_value *dest = vtn_create_ssa_value(b, dest_type);

   switch (opcode)`);
    return s.replace('#include "vtn_private.h"', '#include "vtn_private.h"\n#include "vtn_long_vector.h"');
  });
  edit('src/compiler/spirv/vtn_subgroup.c', s => {
    s = s.replace('   /* Some of the subgroup operations take an index.', `   if (glsl_type_is_array(src0->type)) {
      struct vtn_ssa_value *dst = vtn_create_ssa_value(b, src0->type);
      for (unsigned i = 0; i < glsl_get_length(src0->type); i++)
         dst->elems[i] = vtn_build_subgroup_instr(b, nir_op, src0->elems[i],
            src1 ? src1->elems[i] : NULL, index, const_idx0, const_idx1);
      return dst;
   }
   /* Some of the subgroup operations take an index.`);
    s = s.replace('switch (glsl_get_base_type(vtn_ssa_value(b, w[4])->type))', 'switch (glsl_get_base_type(glsl_without_array(vtn_ssa_value(b, w[4])->type)))');
    const start=s.indexOf('      nir_def *src0;\n'),end=s.indexOf('      break;\n   }',start);
    if(start<0||end<0)throw Error('Long-vector subgroup vote anchor');
    s=s.replace(s.slice(start,end), `      bool scope = opcode == SpvOpGroupNonUniformAll || opcode == SpvOpGroupAll ||
         opcode == SpvOpGroupNonUniformAny || opcode == SpvOpGroupAny || opcode == SpvOpGroupNonUniformAllEqual;
      struct vtn_ssa_value *src = vtn_ssa_value(b, w[scope ? 4 : 3]);
      vtn_push_nir_ssa(b, w[2], vtn_subgroup_vote(b, op, src));
`);
    return s.replace('void\nvtn_handle_subgroup(', `static nir_def *
vtn_subgroup_vote(struct vtn_builder *b, nir_intrinsic_op op, struct vtn_ssa_value *src)
{
   if (glsl_type_is_array(src->type)) {
      nir_def *result = nir_imm_true(&b->nb);
      for (unsigned i = 0; i < glsl_get_length(src->type); i++)
         result = nir_iand(&b->nb, result, vtn_subgroup_vote(b, op, src->elems[i]));
      return result;
   }
   nir_intrinsic_instr *intrin = nir_intrinsic_instr_create(b->nb.shader, op);
   if (nir_intrinsic_infos[op].src_components[0] == 0) intrin->num_components = src->def->num_components;
   intrin->src[0] = nir_src_for_ssa(src->def);
   nir_def_init(&intrin->instr, &intrin->def, 1, 1);
   nir_builder_instr_insert(&b->nb, &intrin->instr);
   return &intrin->def;
}

void
vtn_handle_subgroup(`);
  });
  edit('src/compiler/spirv/vtn_constants.c', s => {
    const start = s.indexOf('   default: {\n      const glsl_type *dst_type = val->type->type;');
    const end = s.indexOf('      break;\n   } /* default */', start);
    if (start < 0 || end < 0) throw Error('Long-vector constant ALU anchor missing');
    let body = s.slice(start + '   default: {\n'.length, end);
    const inputs = body.slice(body.indexOf('      vtn_assert(count <= 7);'), body.indexOf('      const unsigned dst_bit_size ='));
    body = body.replace('      const glsl_type *dst_type = val->type->type;\n', '').replace(inputs, '');
    body = body.replace('      unsigned num_components = glsl_get_vector_elements(val->type->type);\n', '');
    body = body.replaceAll('src_val[i]->constant->values[c]', 'src_constant[i]->values[c]').replaceAll('val->constant->values', 'constant->values');
    const evaluator = 'static void\nvtn_eval_spec_alu(struct vtn_builder *b, SpvOp opcode, struct vtn_value *val,\n                  const glsl_type *dst_type, nir_constant *constant,\n                  unsigned num_components, unsigned src_count,\n                  const glsl_type **src_type, nir_constant **src_constant)\n{\n' + body + '}\n\n';
    s = s.replace(s.slice(start, end), `   default: {
      unsigned src_count = count - 4;
      vtn_assert(src_count <= 3);
      struct vtn_value *src_val[3] = {0};
      for (unsigned i = 0; i < src_count; i++) src_val[i] = vtn_value(b, w[4+i], vtn_value_type_constant);
      unsigned length = val->type->is_long_vector ? val->type->length : 1;
      if (val->type->is_long_vector) {
         val->constant->num_elements = length;
         val->constant->elements = ralloc_array(val->constant, nir_constant *, length);
      }
      for (unsigned c = 0; c < length; c++) {
         const glsl_type *src_type[3] = {0};
         nir_constant *src_constant[3] = {0};
         for (unsigned i = 0; i < src_count; i++) {
            src_type[i] = src_val[i]->type->is_long_vector ? glsl_get_array_element(src_val[i]->type->type) : src_val[i]->type->type;
            src_constant[i] = src_val[i]->type->is_long_vector ? src_val[i]->constant->elements[c] : src_val[i]->constant;
         }
         nir_constant *dst = val->constant;
         const glsl_type *dst_type = val->type->type;
         if (val->type->is_long_vector) {
            dst = val->constant->elements[c] = rzalloc(val->constant, nir_constant);
            dst_type = glsl_get_array_element(dst_type);
         }
         vtn_eval_spec_alu(b, opcode, val, dst_type, dst, glsl_get_vector_elements(dst_type), src_count, src_type, src_constant);
      }
`);
    s = s.replace('static void\nvtn_handle_spec_constant_op(', evaluator + 'static void\nvtn_handle_spec_constant_op(');
    // Extract/insert already recurse through the array representation. These
    // three specialization operations inspect individual vector components.
    s = s.replace('   switch (opcode) {\n   case SpvOpBitcast:', '   if (vtn_long_vector_spec_op(b, opcode, val, w, count)) return;\n\n   switch (opcode) {\n   case SpvOpBitcast:');
    return s.replace('static void\nvtn_handle_spec_constant_op(', specHelpers + '\nstatic void\nvtn_handle_spec_constant_op(');
  });
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('   *ext = (struct vk_device_extension_table){', '   *ext = (struct vk_device_extension_table){\n      .EXT_shader_long_vector = true,')
    .replace('   *features = (struct vk_features){', '   *features = (struct vk_features){\n      .longVector = true,')
    .replace('   *properties = (struct vk_properties){', '   *properties = (struct vk_properties){\n      .maxVectorComponents = 1024,'));
}

const specHelpers = `
static nir_const_value
vtn_long_const_get(struct vtn_value *val, unsigned i)
{
   if (val->value_type == vtn_value_type_undef) return (nir_const_value){0};
   return val->type->is_long_vector ? val->constant->elements[i]->values[0] : val->constant->values[i];
}

static bool
vtn_long_vector_spec_op(struct vtn_builder *b, SpvOp opcode, struct vtn_value *val, const uint32_t *w, unsigned count)
{
   if (opcode != SpvOpBitcast && opcode != SpvOpVectorShuffle && opcode != SpvOpSelect) return false;
   struct vtn_value *src = vtn_untyped_value(b, w[4]);
   bool is_long = val->type->is_long_vector || src->type->is_long_vector;
   if (opcode == SpvOpVectorShuffle) is_long |= vtn_untyped_value(b, w[5])->type->is_long_vector;
   if (!is_long) return false;
   unsigned length = val->type->is_long_vector ? val->type->length : glsl_get_vector_elements(val->type->type);
   if (val->type->is_long_vector) {
      val->constant->num_elements = length;
      val->constant->elements = ralloc_array(val->constant, nir_constant *, length);
      for (unsigned i = 0; i < length; i++) val->constant->elements[i] = rzalloc(val->constant, nir_constant);
   }
   unsigned src_n = src->type->is_long_vector ? src->type->length : glsl_get_vector_elements(src->type->type);
   uint8_t bits[8192];
   unsigned src_bits = glsl_get_bit_size(glsl_without_array(src->type->type));
   unsigned dst_bits = glsl_get_bit_size(glsl_without_array(val->type->type));
   if (opcode == SpvOpBitcast) {
      vtn_assert(src_n * src_bits == length * dst_bits);
      for (unsigned i = 0; i < src_n; i++) {
         uint64_t v = nir_const_value_as_uint(vtn_long_const_get(src, i), src_bits);
         memcpy(bits + i * src_bits / 8, &v, src_bits / 8);
      }
   }
   for (unsigned i = 0; i < length; i++) {
      nir_const_value value;
      if (opcode == SpvOpBitcast) {
         uint64_t v = 0;
         memcpy(&v, bits + i * dst_bits / 8, dst_bits / 8);
         value = nir_const_value_for_uint(v, dst_bits);
      } else if (opcode == SpvOpVectorShuffle) {
         unsigned index = w[6+i];
         struct vtn_value *src1 = vtn_untyped_value(b, w[5]);
         value = index == UINT32_MAX ? (nir_const_value){0} : index < src_n ? vtn_long_const_get(src,index) : vtn_long_const_get(src1,index-src_n);
      } else {
         bool cond = vtn_long_const_get(src, src->type->base_type == vtn_base_type_scalar ? 0 : i).b;
         value = vtn_long_const_get(vtn_untyped_value(b, w[cond ? 5 : 6]), i);
      }
      if (val->type->is_long_vector) val->constant->elements[i]->values[0] = value;
      else val->constant->values[i] = value;
   }
   return true;
}
`;

const aluWrapper = `
void
vtn_handle_alu(struct vtn_builder *b, SpvOp opcode,
               const uint32_t *w, unsigned count)
{
   struct vtn_value *dest_val = vtn_untyped_value(b, w[2]);
   const struct glsl_type *dest_type = vtn_get_type(b, w[1])->type;
   vtn_handle_fp_fast_math(b, dest_val, vtn_untyped_value(b, w[3]));
   if (glsl_type_is_cmat(dest_type)) {
      vtn_handle_cooperative_alu(b, dest_val, dest_type, opcode, w, count);
   } else {
      unsigned num_inputs = count - 3;
      struct vtn_ssa_value *src[4] = {0};
      vtn_collect_ssa_sources(b, &w[3], src, num_inputs);
      unsigned length = vtn_long_vector_length(dest_type);
      for (unsigned i = 0; i < num_inputs; i++)
         length = MAX2(length, vtn_long_vector_length(src[i]->type));
      struct vtn_ssa_value *dest;
      if (!length) {
         dest = vtn_alu_result(b, opcode, dest_val, dest_type, src, num_inputs);
      } else if (opcode == SpvOpDot) {
         dest = vtn_create_ssa_value(b, dest_type);
         dest->def = vtn_long_vector_dot(b, src[0], src[1]);
      } else if (opcode == SpvOpAny || opcode == SpvOpAll || opcode == SpvOpDot) {
         dest = vtn_create_ssa_value(b, dest_type);
         nir_def *terms[1024];
         for (unsigned i = 0; i < length; i++)
            terms[i] = opcode == SpvOpDot ? nir_fmul(&b->nb, src[0]->elems[i]->def, src[1]->elems[i]->def) : src[0]->elems[i]->def;
         for (unsigned n = length; n > 1; n = DIV_ROUND_UP(n, 2)) {
            for (unsigned i = 0; i < n / 2; i++) {
               nir_def *a = terms[2*i], *c = terms[2*i+1];
               terms[i] = opcode == SpvOpAny ? nir_ior(&b->nb,a,c) : opcode == SpvOpAll ? nir_iand(&b->nb,a,c) : nir_fadd(&b->nb,a,c);
            }
            if (n & 1) terms[n/2] = terms[n-1];
         }
         dest->def = terms[0];
      } else {
         dest = vtn_create_ssa_value(b, dest_type);
         const struct glsl_type *scalar_type = vtn_long_vector_scalar_type(b, dest_type);
         const unsigned math_ctrl = b->nb.fp_math_ctrl;
         for (unsigned i = 0; i < length; i++) {
            struct vtn_ssa_value *elem[4] = {0};
            for (unsigned j = 0; j < num_inputs; j++)
               elem[j] = vtn_long_vector_element(b, src[j], i);
            b->nb.fp_math_ctrl = math_ctrl;
            vtn_long_vector_set_element(dest, i,
               vtn_alu_result(b, opcode, dest_val, scalar_type, elem, num_inputs));
         }
      }
      vtn_push_ssa_value(b, w[2], dest);
   }
   b->nb.fp_math_ctrl = nir_fp_fast_math;
}
`;

const glslWrapper = `
static void
handle_glsl450_alu(struct vtn_builder *b, enum GLSLstd450 entrypoint,
                   const uint32_t *w, unsigned count)
{
   const struct glsl_type *type = vtn_get_type(b, w[1])->type;
   struct vtn_value *dest_val = vtn_untyped_value(b, w[2]);
   unsigned num_inputs = count - 5;
   struct vtn_ssa_value *src[3] = {0};
   vtn_collect_ssa_sources(b, &w[5], src, num_inputs);
   unsigned length = vtn_long_vector_length(type);
   for (unsigned i = 0; i < num_inputs; i++) length = MAX2(length, vtn_long_vector_length(src[i]->type));
   if (!length) {
      vtn_push_ssa_value(b, w[2], vtn_glsl450_alu_result(b, entrypoint, type, dest_val, src, num_inputs));
      return;
   }
   struct vtn_ssa_value *dest = vtn_create_ssa_value(b, type);
   const struct glsl_type *scalar_type = vtn_long_vector_scalar_type(b, type);
   nir_def *dot = NULL, *factor = NULL, *k = NULL, *eta = NULL;
   if (entrypoint == GLSLstd450Distance) {
      struct vtn_ssa_value *diff = vtn_create_ssa_value(b, src[0]->type);
      for (unsigned i = 0; i < length; i++)
         diff->elems[i]->def = nir_fsub(&b->nb, src[0]->elems[i]->def, src[1]->elems[i]->def);
      dest->def = nir_fsqrt(&b->nb, vtn_long_vector_dot(b, diff, diff));
      vtn_push_ssa_value(b, w[2], dest);
      return;
   }
   if (entrypoint == GLSLstd450Length || entrypoint == GLSLstd450Normalize) {
      dot = vtn_long_vector_dot(b, src[0], src[0]);
      if (entrypoint == GLSLstd450Length) {
         dest->def = nir_fsqrt(&b->nb, dot);
         vtn_push_ssa_value(b, w[2], dest);
         return;
      }
      factor = nir_frsq(&b->nb, dot);
   } else if (entrypoint == GLSLstd450FaceForward) {
      dot = vtn_long_vector_dot(b, src[2], src[1]);
      factor = nir_flt(&b->nb, dot, nir_imm_floatN_t(&b->nb, 0, dot->bit_size));
   } else if (entrypoint == GLSLstd450Reflect) {
      factor = nir_fmul_imm(&b->nb, vtn_long_vector_dot(b, src[0], src[1]), 2);
   } else if (entrypoint == GLSLstd450Refract) {
      dot = vtn_long_vector_dot(b, src[0], src[1]);
      eta = nir_f2fN(&b->nb, src[2]->def, dot->bit_size);
      nir_def *one = nir_imm_floatN_t(&b->nb, 1, dot->bit_size);
      k = nir_a_minus_bc(&b->nb, one, eta, nir_fmul(&b->nb, eta, nir_a_minus_bc(&b->nb, one, dot, dot)));
      factor = nir_ffma_weak(&b->nb, eta, dot, nir_fsqrt(&b->nb, k));
   }
   unsigned math_ctrl = b->nb.fp_math_ctrl;
   for (unsigned i = 0; i < length; i++) {
      struct vtn_ssa_value *elem[3] = {0};
      for (unsigned j = 0; j < num_inputs; j++) elem[j] = vtn_long_vector_element(b, src[j], i);
      b->nb.fp_math_ctrl = math_ctrl;
      if (entrypoint == GLSLstd450Normalize)
         dest->elems[i]->def = nir_fmul(&b->nb, elem[0]->def, factor);
      else if (entrypoint == GLSLstd450FaceForward)
         dest->elems[i]->def = nir_bcsel(&b->nb, factor, elem[0]->def, nir_fneg(&b->nb, elem[0]->def));
      else if (entrypoint == GLSLstd450Reflect)
         dest->elems[i]->def = nir_a_minus_bc(&b->nb, elem[0]->def, elem[1]->def, factor);
      else if (entrypoint == GLSLstd450Refract) {
         nir_def *zero = nir_imm_floatN_t(&b->nb, 0, dot->bit_size);
         dest->elems[i]->def = nir_bcsel(&b->nb, nir_flt(&b->nb, k, zero), zero,
            nir_a_minus_bc(&b->nb, nir_fmul(&b->nb, eta, elem[0]->def), factor, elem[1]->def));
      } else vtn_long_vector_set_element(dest, i,
         vtn_glsl450_alu_result(b, entrypoint, scalar_type, dest_val, elem, num_inputs));
   }
   vtn_push_ssa_value(b, w[2], dest);
}
`;

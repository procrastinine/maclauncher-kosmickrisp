/* SPDX-License-Identifier: MIT */
#ifndef KK_VTN_LONG_VECTOR_H
#define KK_VTN_LONG_VECTOR_H

/* Long vectors are scalar arrays in NIR. Their components retain contiguous
 * scalar memory layout, and ordinary scalar ALU supplies the operations. */
static unsigned
vtn_long_vector_length(const struct glsl_type *type)
{
   if (glsl_type_is_array(type)) return glsl_get_length(type);
   if (glsl_type_is_struct(type))
      return vtn_long_vector_length(glsl_get_struct_field(type, 0));
   return 0;
}

static const struct glsl_type *
vtn_long_vector_scalar_type(struct vtn_builder *b, const struct glsl_type *type)
{
   if (glsl_type_is_array(type)) return glsl_get_array_element(type);
   if (glsl_type_is_struct(type)) {
      unsigned count = glsl_get_length(type);
      struct glsl_struct_field *fields = vtn_zalloc_array(b, struct glsl_struct_field, count);
      for (unsigned i = 0; i < count; i++) {
         fields[i] = type->fields.structure[i];
         fields[i].type = vtn_long_vector_scalar_type(b, fields[i].type);
      }
      return glsl_struct_type(fields, count, "long_vector_component", false);
   }
   return type;
}

static struct vtn_ssa_value *
vtn_long_vector_element(struct vtn_builder *b, struct vtn_ssa_value *value, unsigned i)
{
   if (glsl_type_is_array(value->type)) return value->elems[i];
   if (glsl_type_is_scalar(value->type)) return value;
   struct vtn_ssa_value *scalar = vtn_create_ssa_value(b, glsl_get_scalar_type(value->type));
   scalar->def = nir_channel(&b->nb, value->def, i);
   return scalar;
}

static void
vtn_long_vector_set_element(struct vtn_ssa_value *dest, unsigned i, struct vtn_ssa_value *value)
{
   if (glsl_type_is_array(dest->type)) {
      dest->elems[i] = value;
   } else {
      assert(glsl_type_is_struct(dest->type));
      for (unsigned j = 0; j < glsl_get_length(dest->type); j++)
         dest->elems[j]->elems[i] = value->elems[j];
   }
}

static nir_def *
vtn_long_vector_dot(struct vtn_builder *b, struct vtn_ssa_value *a, struct vtn_ssa_value *c)
{
   nir_def *terms[1024];
   unsigned length = glsl_get_length(a->type);
   for (unsigned i = 0; i < length; i++)
      terms[i] = nir_fmul(&b->nb, a->elems[i]->def, c->elems[i]->def);
   for (unsigned n = length; n > 1; n = DIV_ROUND_UP(n, 2)) {
      for (unsigned i = 0; i < n/2; i++)
         terms[i] = nir_fadd(&b->nb, terms[2*i], terms[2*i+1]);
      if (n & 1) terms[n/2] = terms[n-1];
   }
   return terms[0];
}

static bool
vtn_long_vector_composite(struct vtn_builder *b, SpvOp op, const uint32_t *w, unsigned count)
{
   struct vtn_type *type = vtn_get_type(b, w[1]);
   if (op == SpvOpVectorExtractDynamic && vtn_get_value_type(b, w[3])->is_long_vector) {
      struct vtn_ssa_value *src = vtn_ssa_value(b, w[3]);
      nir_def *index = vtn_get_nir_ssa(b, w[4]);
      nir_def *result = nir_undef(&b->nb, 1, glsl_get_bit_size(type->type));
      for (unsigned i = 0; i < glsl_get_length(src->type); i++)
         result = nir_bcsel(&b->nb, nir_ieq_imm(&b->nb, index, i), src->elems[i]->def, result);
      vtn_push_nir_ssa(b, w[2], result);
      return true;
   }
   if (op == SpvOpVectorInsertDynamic && type->is_long_vector) {
      struct vtn_ssa_value *src = vtn_ssa_value(b, w[3]);
      struct vtn_ssa_value *dest = vtn_create_ssa_value(b, type->type);
      nir_def *value = vtn_get_nir_ssa(b, w[4]), *index = vtn_get_nir_ssa(b, w[5]);
      for (unsigned i = 0; i < type->length; i++)
         dest->elems[i]->def = nir_bcsel(&b->nb, nir_ieq_imm(&b->nb, index, i), value, src->elems[i]->def);
      vtn_push_ssa_value(b, w[2], dest);
      return true;
   }
   if (op == SpvOpVectorShuffle && (type->is_long_vector ||
       vtn_get_value_type(b, w[3])->is_long_vector || vtn_get_value_type(b, w[4])->is_long_vector)) {
      struct vtn_ssa_value *a = vtn_ssa_value(b, w[3]), *c = vtn_ssa_value(b, w[4]);
      unsigned na = vtn_get_value_type(b, w[3])->length;
      struct vtn_ssa_value *dest = vtn_create_ssa_value(b, type->type);
      nir_def *short_components[NIR_MAX_VEC_COMPONENTS];
      for (unsigned i = 0; i < type->length; i++) {
         uint32_t index = w[5+i];
         struct vtn_ssa_value *elem;
         if (index == UINT32_MAX) {
            elem = vtn_create_ssa_value(b, type->array_element->type);
            elem->def = nir_undef(&b->nb, 1, glsl_get_bit_size(elem->type));
         } else elem = index < na ? vtn_long_vector_element(b, a, index) : vtn_long_vector_element(b, c, index-na);
         if (type->is_long_vector) dest->elems[i] = elem;
         else short_components[i] = elem->def;
      }
      if (!type->is_long_vector) dest->def = nir_vec(&b->nb, short_components, type->length);
      vtn_push_ssa_value(b, w[2], dest);
      return true;
   }
   if (op == SpvOpCompositeConstruct && type->is_long_vector) {
      struct vtn_ssa_value *dest = vtn_create_ssa_value(b, type->type);
      unsigned pos = 0;
      for (unsigned j = 3; j < count; j++) {
         struct vtn_type *src_type = vtn_get_value_type(b, w[j]);
         struct vtn_ssa_value *src = vtn_ssa_value(b, w[j]);
         unsigned n = src_type->base_type == vtn_base_type_scalar ? 1 : src_type->length;
         for (unsigned i = 0; i < n; i++) {
            vtn_fail_if(pos >= type->length, "Too many long vector components");
            dest->elems[pos++] = vtn_long_vector_element(b, src, i);
         }
      }
      vtn_fail_if(pos != type->length, "Too few long vector components");
      vtn_push_ssa_value(b, w[2], dest);
      return true;
   }
   return false;
}

static bool
vtn_long_vector_bitcast(struct vtn_builder *b, const uint32_t *w)
{
   struct vtn_type *type = vtn_get_type(b, w[1]), *src_type = vtn_get_value_type(b, w[3]);
   if (!type->is_long_vector && !src_type->is_long_vector) return false;
   struct vtn_ssa_value *src = vtn_ssa_value(b, w[3]), *dest = vtn_create_ssa_value(b, type->type);
   unsigned src_n = src_type->length, dst_n = type->length;
   unsigned src_bits = glsl_get_bit_size(glsl_without_array(src_type->type));
   unsigned dst_bits = glsl_get_bit_size(glsl_without_array(type->type));
   vtn_fail_if(src_n * src_bits != dst_n * dst_bits, "Long vector bitcast size mismatch");
   nir_def *components[1024];
   for (unsigned i = 0; i < dst_n; i++) {
      unsigned first = i * dst_bits / src_bits;
      if (dst_bits <= src_bits) {
         nir_def *value = vtn_long_vector_element(b, src, first)->def;
         unsigned shift = i * dst_bits % src_bits;
         components[i] = nir_u2uN(&b->nb, shift ? nir_ushr_imm(&b->nb, value, shift) : value, dst_bits);
      } else {
         unsigned n = dst_bits / src_bits;
         nir_def *value = nir_imm_intN_t(&b->nb, 0, dst_bits);
         for (unsigned j = 0; j < n; j++)
            value = nir_ior(&b->nb, value, nir_ishl_imm(&b->nb, nir_u2uN(&b->nb, vtn_long_vector_element(b, src, first+j)->def, dst_bits), j*src_bits));
         components[i] = value;
      }
      if (type->is_long_vector) dest->elems[i]->def = components[i];
   }
   if (!type->is_long_vector) dest->def = nir_vec(&b->nb, components, dst_n);
   vtn_push_ssa_value(b, w[2], dest);
   return true;
}
#endif

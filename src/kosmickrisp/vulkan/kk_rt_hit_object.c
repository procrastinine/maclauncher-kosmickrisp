/* SPDX-License-Identifier: MIT */
/* Hit objects (SPV_EXT_shader_invocation_reorder) as invocation-private word
 * arrays. The field operations become loads and stores of the hit object's
 * words; RecordFromQuery reads the committed ray query state through rq_load,
 * so this pass runs before the ray query lowering. */
#include "kk_ray_tracing.h"
#include "kk_rt_abi.h"
#include "nir_builder.h"

/* Word offsets in the NIR_HIT_OBJECT_WORDS array. The scheduler writes the
 * same layout for a recorded trace. */
enum {
   KK_HO_KIND = 0,
   KK_HO_RAY_FLAGS = 1,
   KK_HO_ORIGIN = 2,
   KK_HO_TMIN = 5,
   KK_HO_DIRECTION = 6,
   KK_HO_TMAX = 9,
   KK_HO_OBJECT_ORIGIN = 10,
   KK_HO_OBJECT_DIRECTION = 13,
   KK_HO_OBJECT_TO_WORLD = 16,
   KK_HO_WORLD_TO_OBJECT = 28,
   KK_HO_INSTANCE_ID = 40,
   KK_HO_CUSTOM_INDEX = 41,
   KK_HO_PRIMITIVE = 42,
   KK_HO_GEOMETRY = 43,
   KK_HO_HIT_KIND = 44,
   KK_HO_SBT_INDEX = 45,
   KK_HO_SHADER_RECORD = 46,
   KK_HO_VERTICES = 48,
   KK_HO_TIME = 57,
   KK_HO_ATTRIBUTES = 60,
   KK_HO_ATTRIBUTE_WORDS = 8,
   KK_HO_WORDS = 68,
};
_Static_assert(KK_HO_WORDS <= NIR_HIT_OBJECT_WORDS, "hit object layout");

static nir_deref_instr *
word(nir_builder *b, nir_def *hit_object, unsigned index)
{
   nir_deref_instr *deref = nir_def_as_deref(hit_object);
   return nir_build_deref_array_imm(b, deref, index);
}

static nir_def *
load_words(nir_builder *b, nir_def *hit_object, unsigned first, unsigned count,
           unsigned stride, unsigned bit_size)
{
   nir_def *words[4];
   for (unsigned i = 0; i < count; i++)
      words[i] = nir_load_deref(b, word(b, hit_object, first + i * stride));
   nir_def *value = nir_vec(b, words, count);
   if (bit_size == 64)
      return nir_pack_64_2x32(b, value);
   return bit_size == 32 ? value : nir_u2uN(b, value, bit_size);
}

static void
store_words(nir_builder *b, nir_def *hit_object, unsigned first, nir_def *value)
{
   if (value->bit_size == 64)
      value = nir_unpack_64_2x32(b, value);
   else if (value->bit_size != 32)
      value = nir_u2u32(b, value);
   for (unsigned i = 0; i < value->num_components; i++)
      nir_store_deref(b, word(b, hit_object, first + i), nir_channel(b, value, i), 1);
}

static nir_def *
load_field(nir_builder *b, nir_intrinsic_instr *in)
{
   nir_def *hit_object = in->src[0].ssa;
   unsigned n = in->def.num_components, bits = in->def.bit_size;
   unsigned column = nir_intrinsic_column(in);
   switch (nir_intrinsic_hit_object_field(in)) {
   case nir_hit_object_field_kind: return load_words(b, hit_object, KK_HO_KIND, n, 1, bits);
   case nir_hit_object_field_ray_flags: return load_words(b, hit_object, KK_HO_RAY_FLAGS, n, 1, bits);
   case nir_hit_object_field_world_ray_origin: return load_words(b, hit_object, KK_HO_ORIGIN, n, 1, bits);
   case nir_hit_object_field_ray_tmin: return load_words(b, hit_object, KK_HO_TMIN, n, 1, bits);
   case nir_hit_object_field_world_ray_direction: return load_words(b, hit_object, KK_HO_DIRECTION, n, 1, bits);
   case nir_hit_object_field_ray_tmax: return load_words(b, hit_object, KK_HO_TMAX, n, 1, bits);
   case nir_hit_object_field_object_ray_origin: return load_words(b, hit_object, KK_HO_OBJECT_ORIGIN, n, 1, bits);
   case nir_hit_object_field_object_ray_direction: return load_words(b, hit_object, KK_HO_OBJECT_DIRECTION, n, 1, bits);
   /* Matrices are stored as three rows of four, as in kk_rt_cps_context. */
   case nir_hit_object_field_object_to_world: return load_words(b, hit_object, KK_HO_OBJECT_TO_WORLD + column, n, 4, bits);
   case nir_hit_object_field_world_to_object: return load_words(b, hit_object, KK_HO_WORLD_TO_OBJECT + column, n, 4, bits);
   case nir_hit_object_field_instance_id: return load_words(b, hit_object, KK_HO_INSTANCE_ID, n, 1, bits);
   case nir_hit_object_field_instance_custom_index: return load_words(b, hit_object, KK_HO_CUSTOM_INDEX, n, 1, bits);
   case nir_hit_object_field_primitive_index: return load_words(b, hit_object, KK_HO_PRIMITIVE, n, 1, bits);
   case nir_hit_object_field_geometry_index: return load_words(b, hit_object, KK_HO_GEOMETRY, n, 1, bits);
   case nir_hit_object_field_hit_kind: return load_words(b, hit_object, KK_HO_HIT_KIND, n, 1, bits);
   case nir_hit_object_field_sbt_record_index: return load_words(b, hit_object, KK_HO_SBT_INDEX, n, 1, bits);
   case nir_hit_object_field_shader_record:
      return bits == 64 ? load_words(b, hit_object, KK_HO_SHADER_RECORD, 2, 1, 64) :
                          load_words(b, hit_object, KK_HO_SHADER_RECORD, n, 1, bits);
   case nir_hit_object_field_triangle_vertex_positions:
      return load_words(b, hit_object, KK_HO_VERTICES + 3 * column, n, 1, bits);
   case nir_hit_object_field_current_time: return load_words(b, hit_object, KK_HO_TIME, n, 1, bits);
   }
   UNREACHABLE("hit object field");
}

static void
clear(nir_builder *b, nir_def *hit_object)
{
   for (unsigned i = 0; i < KK_HO_WORDS; i++)
      nir_store_deref(b, word(b, hit_object, i), nir_imm_int(b, 0), 1);
}

static void
copy_attributes(nir_builder *b, nir_def *dst, unsigned dst_word, nir_def *src, unsigned src_word)
{
   /* Attribute variables are typed by the application; both sides are
    * reinterpreted as words through a cast. */
   const struct glsl_type *words = glsl_array_type(glsl_uint_type(), KK_HO_ATTRIBUTE_WORDS, 4);
   nir_deref_instr *d = nir_def_as_deref(dst), *s = nir_def_as_deref(src);
   if (dst_word == ~0u)
      d = nir_build_deref_cast(b, &d->def, d->modes, words, 4);
   if (src_word == ~0u)
      s = nir_build_deref_cast(b, &s->def, s->modes, words, 4);
   for (unsigned i = 0; i < KK_HO_ATTRIBUTE_WORDS; i++) {
      nir_deref_instr *from = nir_build_deref_array_imm(b, s, src_word == ~0u ? i : src_word + i);
      nir_deref_instr *to = nir_build_deref_array_imm(b, d, dst_word == ~0u ? i : dst_word + i);
      nir_store_deref(b, to, nir_load_deref(b, from), 1);
   }
}

static bool
lower(nir_builder *b, nir_intrinsic_instr *in, void *unused)
{
   b->cursor = nir_before_instr(&in->instr);
   switch (in->intrinsic) {
   case nir_intrinsic_hit_object_load:
      nir_def_replace(&in->def, load_field(b, in));
      return true;
   case nir_intrinsic_hit_object_set_sbt_record_index:
      store_words(b, in->src[0].ssa, KK_HO_SBT_INDEX, in->src[1].ssa);
      break;
   case nir_intrinsic_hit_object_record_empty:
      clear(b, in->src[0].ssa);
      break;
   case nir_intrinsic_hit_object_record_miss: {
      nir_def *h = in->src[0].ssa;
      clear(b, h);
      store_words(b, h, KK_HO_KIND, nir_imm_int(b, nir_hit_object_kind_miss));
      store_words(b, h, KK_HO_RAY_FLAGS, in->src[1].ssa);
      store_words(b, h, KK_HO_SBT_INDEX, in->src[2].ssa);
      store_words(b, h, KK_HO_ORIGIN, in->src[3].ssa);
      store_words(b, h, KK_HO_TMIN, in->src[4].ssa);
      store_words(b, h, KK_HO_DIRECTION, in->src[5].ssa);
      store_words(b, h, KK_HO_TMAX, in->src[6].ssa);
      break;
   }
   case nir_intrinsic_hit_object_record_from_query: {
      nir_def *h = in->src[0].ssa, *q = in->src[1].ssa;
#define RQ(value, n, ...) nir_rq_load(b, n, 32, q, .ray_query_value = nir_ray_query_value_##value, \
                                      .committed = true, ##__VA_ARGS__)
      nir_def *type = RQ(intersection_type, 1);
      clear(b, h);
      /* A committed intersection records a hit; without one the hit object
       * is empty. */
      store_words(b, h, KK_HO_KIND, nir_bcsel(b, nir_ine_imm(b, type, 0),
                                              nir_imm_int(b, nir_hit_object_kind_hit),
                                              nir_imm_int(b, nir_hit_object_kind_empty)));
      store_words(b, h, KK_HO_RAY_FLAGS, RQ(flags, 1));
      store_words(b, h, KK_HO_ORIGIN, RQ(world_ray_origin, 3));
      store_words(b, h, KK_HO_TMIN, RQ(tmin, 1));
      store_words(b, h, KK_HO_DIRECTION, RQ(world_ray_direction, 3));
      store_words(b, h, KK_HO_TMAX, RQ(intersection_t, 1));
      store_words(b, h, KK_HO_OBJECT_ORIGIN, RQ(intersection_object_ray_origin, 3));
      store_words(b, h, KK_HO_OBJECT_DIRECTION, RQ(intersection_object_ray_direction, 3));
      for (unsigned c = 0; c < 4; c++) {
         nir_def *o2w = RQ(intersection_object_to_world, 3, .column = c);
         nir_def *w2o = RQ(intersection_world_to_object, 3, .column = c);
         for (unsigned r = 0; r < 3; r++) {
            store_words(b, h, KK_HO_OBJECT_TO_WORLD + r * 4 + c, nir_channel(b, o2w, r));
            store_words(b, h, KK_HO_WORLD_TO_OBJECT + r * 4 + c, nir_channel(b, w2o, r));
         }
      }
      store_words(b, h, KK_HO_INSTANCE_ID, RQ(intersection_instance_id, 1));
      store_words(b, h, KK_HO_CUSTOM_INDEX, RQ(intersection_instance_custom_index, 1));
      store_words(b, h, KK_HO_PRIMITIVE, RQ(intersection_primitive_index, 1));
      store_words(b, h, KK_HO_GEOMETRY, RQ(intersection_geometry_index, 1));
      store_words(b, h, KK_HO_HIT_KIND, in->src[4].ssa);
      store_words(b, h, KK_HO_SBT_INDEX, in->src[2].ssa);
      for (unsigned v = 0; v < 3; v++)
         store_words(b, h, KK_HO_VERTICES + 3 * v,
                     RQ(intersection_triangle_vertex_positions, 3, .column = v));
#undef RQ
      copy_attributes(b, h, KK_HO_ATTRIBUTES, in->src[3].ssa, ~0u);
      break;
   }
   case nir_intrinsic_hit_object_get_attributes:
      copy_attributes(b, in->src[1].ssa, ~0u, in->src[0].ssa, KK_HO_ATTRIBUTES);
      break;
   default:
      return false;
   }
   nir_instr_remove(&in->instr);
   return true;
}

bool
kk_nir_lower_hit_objects(nir_shader *shader)
{
   return nir_shader_intrinsics_pass(shader, lower, nir_metadata_control_flow, NULL);
}

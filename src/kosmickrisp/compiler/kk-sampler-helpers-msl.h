/* Copyright 2016 Broadcom
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 * Copyright 2018 Intel Corporation
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * Copyright 2026 LunarG, Inc.
 * Copyright 2026 Google LLC
 * SPDX-License-Identifier: MIT
 * Centered footprints and cube projections follow Mesa nir_lower_tex.c and
 * the merged sampler-minmax lowering. Border reconstruction follows Mesa's
 * kk_nir_lower_custom_border.c, with identical-component preservation.
 */
#include "kk-sampler-helper-flags.h"
#include "kk-sampler-params.h"
#include "kk_sparse_layout.h"

struct kk_msl_sample_key {
   uint32_t signature[8];
   uint32_t arguments[14];
   uint32_t constant_offset;
   int64_t offset[4];
};
struct kk_msl_sample_helper {
   struct kk_msl_sample_key key;
   nir_tex_instr *tex;
   unsigned id;
   struct kk_msl_sample_helper *next;
};
static const nir_tex_src_type kk_sample_args[] = {
   nir_tex_src_bias, nir_tex_src_lod, nir_tex_src_ddx, nir_tex_src_ddy,
   nir_tex_src_min_lod, nir_tex_src_offset, nir_tex_src_comparator,
};
static const char *kk_sample_arg_names[] = {"bias_value", "lod_value", "dx", "dy", "min_lod_value", "offset_value", "compare_value"};

static void
kk_sample_parameters_mode(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex,
                          bool declaration, bool entry)
{
   bool prepared = tex->backend_flags & KK_SAMPLE_PREPARE_2D;
   if (prepared) {
      if (entry) {
         P(ctx, declaration ? ", float source_value, float2 source_dx, float2 source_dy, uint operation" : ", source_value, source_dx, source_dy, operation");
      } else {
         P(ctx, declaration ? ", float2 dx, float2 dy, float sample_lod, uint operation" : ", dx, dy, sample_lod, operation");
      }
   }
   for (unsigned i = 0; i < ARRAY_SIZE(kk_sample_args); ++i) {
      nir_src *src = nir_tex_get_src(tex, kk_sample_args[i]);
      if (prepared && (kk_sample_args[i] == nir_tex_src_bias ||
                       kk_sample_args[i] == nir_tex_src_lod ||
                       kk_sample_args[i] == nir_tex_src_ddx ||
                       kk_sample_args[i] == nir_tex_src_ddy)) continue;
      if (!src || (kk_sample_args[i] == nir_tex_src_offset && nir_src_is_const(*src))) continue;
      if (declaration) {
         unsigned n = src->ssa->num_components;
         P(ctx, ", %s%s %s", kk_sample_args[i] == nir_tex_src_offset ? "int" : "float",
           n == 1 ? "" : n == 2 ? "2" : n == 3 ? "3" : "4", kk_sample_arg_names[i]);
      } else {
         P(ctx, ", %s", kk_sample_arg_names[i]);
      }
   }
}

static void
kk_sample_parameters(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex, bool declaration)
{
   kk_sample_parameters_mode(ctx, tex, declaration, false);
}

static void kk_sample_coord(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex, const char *coord);

/* Sparse adds one state reference to the same footprint functions. Ordinary
 * signatures carry no sparse argument or residency work. */
static void
kk_sample_sparse_arg(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex, bool declaration)
{
   if (tex->backend_flags & KK_SAMPLE_SPARSE) {
      P(ctx, declaration ? ", thread kk_sparse_sample_state &sparse" : ", sparse");
   }
}

static void
kk_sample_sparse_tap(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex,
                     const char *type, unsigned dims)
{
   P(ctx, "    if (sparse.query_address != 0ul) {\n");
   P(ctx, "        uint base = *(constant uint*)(sparse.query_address + %zuul);\n", offsetof(struct kk_sparse_query_record, base_lod));
   P(ctx, "        ulong row = *(constant ulong*)(sparse.query_address + %zuul);\n", offsetof(struct kk_sparse_query_record, sampler_table));
   P(ctx, "        ulong query_slot = sparse.query_address + %zuul;\n",
     tex->is_array && (tex->sampler_dim == GLSL_SAMPLER_DIM_1D || tex->sampler_dim == GLSL_SAMPLER_DIM_2D) ?
     offsetof(struct kk_sparse_query_record, array_query_resource_id) : offsetof(struct kk_sparse_query_record, resource_id));
   if (tex->is_array && tex->sampler_dim == GLSL_SAMPLER_DIM_2D) {
      /* A lowered non-seamless cube is a face-local 2D array. Real array
       * records have no face resource; no kind60 or private view struct. */
      P(ctx, "        if (*(constant ulong*)(sparse.query_address + %zuul) != 0ul) query_slot = sparse.query_address + %zuul;\n",
        offsetof(struct kk_sparse_query_record, face_resource_id), offsetof(struct kk_sparse_query_record, face_resource_id));
   }
   P(ctx, "        auto query_image = *(constant ");
   nir_src *handle = nir_tex_get_src(tex, nir_tex_src_texture_handle);
   msl_emit_texture_type(ctx, nir_instr_as_intrinsic(nir_def_instr(handle->ssa)));
   P(ctx, "*)query_slot;\n");
   P(ctx, "        uint border_class = sparse.physical_facts & %uu;\n", KK_SPARSE_PHYSICAL_BORDER_MASK);
   P(ctx, "        uint address_modes = metadata[23] & 511u;\n");
   P(ctx, "        uint tap_word = border_class == %uu ? %uu : %uu;\n",
     KK_SPARSE_BORDER_ONE, KK_SPARSE_ROW_WHITE_TAP_WORD, KK_SPARSE_ROW_BLACK_TAP_WORD);
   P(ctx, "        sampler tap_sampler = *(constant sampler*)(row + ulong(address_modes) * %uu + ulong(tap_word) * 8ul);\n", KK_SPARSE_SAMPLER_STRIDE);
   P(ctx, "        float4 sparse_coord = tap_coord; bool unnormalized = (metadata[23] & 512u) != 0;\n");
   const char *sizefn[] = {"width", "height", "depth"};
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE) {
      for (unsigned d = 0; d < dims; ++d) {
         P(ctx, "        if (unnormalized) sparse_coord.%c /= float(query_image.get_%s(tap_level + base));\n", "xyz"[d], sizefn[d]);
      }
   }
   P(ctx, "        auto point = query_image.sparse_sample(tap_sampler, ");
   kk_sample_coord(ctx, tex, "sparse_coord");
   P(ctx, ", level(float(tap_level + base)));\n");
   P(ctx, "        sparse.all_resident = sparse.all_resident && point.resident();\n");
   P(ctx, "        bool outside = false;\n");
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE) {
      for (unsigned d = 0; d < dims; ++d) {
         P(ctx, "        outside = outside || (((metadata[23] >> %u) & 7u) == 3u && (sparse_coord.%c < 0.0f || sparse_coord.%c >= 1.0f));\n", d * 3, "xyz"[d], "xyz"[d]);
      }
   }
   if (tex->is_shadow) {
      P(ctx, "        float depth = point.value();\n");
      P(ctx, "        if (outside) {\n            depth = border_class == %uu ? 1.0f : 0.0f;\n", KK_SPARSE_BORDER_ONE);
      P(ctx, "            if (Border) depth = as_type<float>(uint(metadata[12]) | (uint(metadata[13]) << 16));\n        }\n");
      P(ctx, "        return %s(kk_sampler_compare(sparse.params.compare_op, compare_value, depth));\n", type);
   } else {
      P(ctx, "        %s value = %s(point.value());\n", type, type);
      P(ctx, "        if (outside) {\n            value = border_class == %uu ? %s(1) : %s(0, 0, 0, border_class == %uu ? 1 : 0);\n",
        KK_SPARSE_BORDER_ONE, type, type, KK_SPARSE_BORDER_OPAQUE_BLACK);
      if (tex->backend_flags & KK_SAMPLE_BORDER) {
         if (tex->backend_flags & KK_SAMPLE_CUSTOM) {
            if (nir_alu_type_get_base_type(tex->dest_type) == nir_type_float) {
               P(ctx, "            if (Border) value = %s(as_type<float4>(*(constant packed_uint4*)(metadata + 12)));\n", type);
            } else {
               P(ctx, "            if (Border) value = as_type<%s>(uint4(*(constant packed_uint4*)(metadata + 12)));\n", type);
            }
         } else {
            P(ctx, "            if (Border) value = %s(0, 0, 0, 1);\n", type);
         }
      }
      P(ctx, "        }\n        return value;\n");
   }
   P(ctx, "    }\n");
}

static void
kk_sample_key(nir_tex_instr *tex, struct kk_msl_sample_key *key)
{
   memset(key, 0, sizeof(*key));
   bool prepared = tex->backend_flags & KK_SAMPLE_PREPARE_2D;
   key->signature[0] = prepared ? nir_texop_tex : tex->op;
   key->signature[1] = tex->sampler_dim;
   key->signature[2] = tex->is_array;
   key->signature[3] = tex->is_shadow;
   key->signature[4] = tex->component;
   key->signature[5] = tex->dest_type;
   key->signature[6] = tex->coord_components;
   key->signature[7] = tex->backend_flags;
   for (unsigned i = 0; i < ARRAY_SIZE(kk_sample_args); ++i) {
      nir_src *src = nir_tex_get_src(tex, kk_sample_args[i]);
      if (prepared && (kk_sample_args[i] == nir_tex_src_bias ||
                       kk_sample_args[i] == nir_tex_src_lod ||
                       kk_sample_args[i] == nir_tex_src_ddx ||
                       kk_sample_args[i] == nir_tex_src_ddy)) {
         key->arguments[2 * i] = i < 2 ? 1 : 2;
         key->arguments[2 * i + 1] = 32;
         continue;
      }
      if (!src) continue;
      key->arguments[2 * i] = src->ssa->num_components;
      key->arguments[2 * i + 1] = src->ssa->bit_size;
      if (kk_sample_args[i] == nir_tex_src_offset && nir_src_is_const(*src)) {
         key->constant_offset = 1;
         assert(src->ssa->num_components <= ARRAY_SIZE(key->offset));
         for (unsigned c = 0; c < src->ssa->num_components; ++c)
            key->offset[c] = nir_src_comp_as_int(*src, c);
      }
   }
}

static uint32_t
kk_sample_key_hash(const void *key)
{
   return _mesa_hash_data(key, sizeof(struct kk_msl_sample_key));
}

static bool
kk_sample_key_equal(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(struct kk_msl_sample_key)) == 0;
}

static bool
kk_sample_has_footprint(const nir_tex_instr *tex)
{
   return tex->op != nir_texop_tg4 &&
      (tex->backend_flags & (KK_SAMPLE_VIRTUAL | KK_SAMPLE_MINMAX | KK_SAMPLE_SPARSE));
}


static void
kk_sample_coord(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex, const char *coord)
{
   unsigned dims = tex->sampler_dim == GLSL_SAMPLER_DIM_3D ||
                   tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE ? 3 :
                   tex->sampler_dim == GLSL_SAMPLER_DIM_1D ? 1 : 2;
   P(ctx, "%s.%.*s", coord, dims, "xyz");
   if (tex->is_array)
      P(ctx, ", uint(max(int(rint(%s.%c)), 0))", coord, "xyzw"[dims]);
}

static void
kk_sample_native_form(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex,
                       const char *sampler, bool tap, bool explicit_level)
{
   if (tap && (tex->backend_flags & KK_SAMPLE_SPARSE_POINT)) {
      /* Record zero is an ordinary fully resident view. A native gather
       * ignores sampler LOD clamps/filtering, unlike sample(level(0)). Each
       * point is already a texel center, whose lower-left gather lane is3.
       * Return every color channel so signature deduplication never depends
       * on a particular call site's component uses. */
      if (!tex->is_shadow) P(ctx, "%s(", msl_type_for_def(ctx->types, &tex->def));
      for (unsigned c = 0; c < (tex->is_shadow ? 1u : 4u); ++c) {
         if (c) P(ctx, ", ");
         P(ctx, "image.gather%s(%s, ", tex->is_shadow ? "_compare" : "", sampler);
         kk_sample_coord(ctx, tex, "tap_coord");
         if (tex->is_shadow) P(ctx, ", compare_value");
         if (tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE) P(ctx, ", int2(0)");
         if (!tex->is_shadow) P(ctx, ", component::%c", "xyzw"[c]);
         P(ctx, ")[3]");
      }
      if (!tex->is_shadow) P(ctx, ")");
      return;
   }
   P(ctx, "image.%s%s(%s, ", !tap && tex->op == nir_texop_tg4 ? "gather" : "sample",
     tex->is_shadow ? "_compare" : "", sampler);
   kk_sample_coord(ctx, tex, tap ? "tap_coord" : "coord");
   if (tex->is_shadow) P(ctx, ", compare_value");
   if (tap) {
      P(ctx, ", level(float(tap_level))");
   } else {
      bool prepared = tex->backend_flags & KK_SAMPLE_PREPARE_2D;
      bool has_min_lod = nir_tex_get_src(tex, nir_tex_src_min_lod) != NULL;
      bool min_lod_in_level = has_min_lod &&
         (prepared ? explicit_level :
          (nir_tex_get_src(tex, nir_tex_src_lod) != NULL ||
           ((tex->backend_flags & KK_SAMPLE_NATIVE_CUBE_LOD) &&
            nir_tex_get_src(tex, nir_tex_src_ddx) != NULL)));
      if (prepared) {
         if (explicit_level) {
            P(ctx, min_lod_in_level ? ", level(max(sample_lod, min_lod_value))" :
                                     ", level(sample_lod)");
         } else {
            P(ctx, ", gradient2d(dx, dy)");
         }
      }
      for (unsigned i = 0; i < ARRAY_SIZE(kk_sample_args); ++i) {
         nir_src *src = nir_tex_get_src(tex, kk_sample_args[i]);
         if (prepared && (kk_sample_args[i] == nir_tex_src_bias ||
                       kk_sample_args[i] == nir_tex_src_lod ||
                       kk_sample_args[i] == nir_tex_src_ddx ||
                       kk_sample_args[i] == nir_tex_src_ddy)) continue;
         if (!src || (min_lod_in_level &&
                      kk_sample_args[i] == nir_tex_src_min_lod)) continue;
         switch (kk_sample_args[i]) {
         case nir_tex_src_bias: P(ctx, ", bias(bias_value)"); break;
         case nir_tex_src_lod:
            P(ctx, min_lod_in_level ? ", level(max(lod_value, min_lod_value))" :
                                     ", level(lod_value)");
            break;
         case nir_tex_src_ddx:
            if (tex->backend_flags & KK_SAMPLE_NATIVE_CUBE_LOD) {
               P(ctx, min_lod_in_level ? ", level(max(native_cube_lod, min_lod_value))" :
                                        ", level(native_cube_lod)");
            } else {
               P(ctx, ", gradient%s(dx, dy)", texture_dim(tex->sampler_dim));
            }
            break;
         case nir_tex_src_min_lod: P(ctx, ", min_lod_clamp(min_lod_value)"); break;
         case nir_tex_src_offset:
            P(ctx, ", ");
            if (nir_src_is_const(*src)) msl_src_as_const(ctx, src);
            else P(ctx, "offset_value");
            break;
         default: break;
         }
      }
      if (tex->op == nir_texop_tg4) {
         if (!nir_tex_get_src(tex, nir_tex_src_offset) && tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE)
            P(ctx, ", int2(0)");
         if (!tex->is_shadow) P(ctx, ", component::%c", "xyzw"[tex->component]);
      }
   }
   P(ctx, ")");
}

static void
kk_sample_native(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex,
                  const char *sampler, bool tap)
{
   if (!tap && (tex->backend_flags & KK_SAMPLE_PREPARE_2D)) {
      P(ctx, "(operation == 1u ? ");
      kk_sample_native_form(ctx, tex, sampler, false, true);
      P(ctx, " : ");
      kk_sample_native_form(ctx, tex, sampler, false, false);
      P(ctx, ")");
   } else {
      kk_sample_native_form(ctx, tex, sampler, tap, false);
   }
}

static void
kk_sample_texture_param(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex)
{
   nir_src *handle = nir_tex_get_src(tex, nir_tex_src_texture_handle);
   msl_emit_texture_type(ctx, nir_instr_as_intrinsic(nir_def_instr(handle->ssa)));
   P(ctx, " image");
}

static void
kk_sample_border(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex, const char *type,
                  bool tap)
{
   P(ctx, "    %s one = ", type); kk_sample_native(ctx, tex, "native_sampler", tap); P(ctx, ";\n");
   if ((tex->backend_flags & KK_SAMPLE_BORDER) && !tex->is_shadow) {
      P(ctx, "    if (!Border) return one;\n");
      P(ctx, "    %s zero = ", type);
      kk_sample_native(ctx, tex, "table.handles[metadata[5]]", tap); P(ctx, ";\n");
      nir_alu_type base = nir_alu_type_get_base_type(tex->dest_type);
      if (tex->backend_flags & KK_SAMPLE_CUSTOM) {
         P(ctx, "    uint4 border_bits = uint4(*(constant packed_uint4*)(metadata + 12));\n");
         if (base == nir_type_float) {
            P(ctx, "    %s color = %s(as_type<float4>(border_bits));\n", type, type);
         } else {
            P(ctx, "    %s color = as_type<%s>(border_bits);\n", type, type);
         }
      } else {
         P(ctx, "    %s color = %s(0, 0, 0, 1);\n", type, type);
      }
      if (!tap && tex->op == nir_texop_tg4)
         P(ctx, "    color = %s(color.%c);\n", type, "xyzw"[tex->component]);
      if (base == nir_type_float && (tap || tex->op != nir_texop_tg4)) {
         const char *bits = tex->def.bit_size == 16 ? "ushort4" : "uint4";
         P(ctx, "    %s blended = mix(zero, one, color);\n", type);
         P(ctx, "    return select(blended, zero, as_type<%s>(zero) == as_type<%s>(one));\n", bits, bits);
      } else if (base == nir_type_float) {
         const char *bits = tex->def.bit_size == 16 ? "ushort4" : "uint4";
         P(ctx, "    return select(color, zero, as_type<%s>(zero) == as_type<%s>(one));\n", bits, bits);
      } else {
         P(ctx, "    return select(color, zero, zero == one);\n");
      }
   } else {
      if (tex->is_shadow && tex->op == nir_texop_tg4 &&
          tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE &&
          (tex->backend_flags & KK_SAMPLE_BORDER)) {
         P(ctx, "    if (Border && (kk_sampler_emulation_active && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) != 0)) {\n");
         P(ctx, "        float2 size = float2(image.get_width(), image.get_height());\n");
         P(ctx, "        bool unnormalized = (metadata[23] & 512u) != 0;\n");
         P(ctx, "        float2 base = floor((unnormalized ? coord.xy : coord.xy * size) - 0.5f);\n");
         nir_src *offset = nir_tex_get_src(tex, nir_tex_src_offset);
         if (offset) {
            P(ctx, "        base += float2(");
            if (nir_src_is_const(*offset)) msl_src_as_const(ctx, offset); else P(ctx, "offset_value");
            P(ctx, ");\n");
         }
         P(ctx, "        constexpr uint2 corners[4] = {uint2(0,1), uint2(1,1), uint2(1,0), uint2(0,0)};\n");
         P(ctx, "        float color = as_type<float>(uint(metadata[12]) | (uint(metadata[13]) << 16));\n");
         P(ctx, "        uint compare_op = kk_sampler_params_from_meta(metadata).compare_op;\n");
         P(ctx, "        for (uint i = 0u; i < 4u; ++i) {\n            float2 center = base + float2(corners[i]) + 0.5f;\n            bool outside = (((metadata[23] & 7u) == 3u) && (center.x < 0.0f || center.x >= size.x)) || ((((metadata[23] >> 3) & 7u) == 3u) && (center.y < 0.0f || center.y >= size.y));\n            if (outside) one[i] = kk_sampler_compare(compare_op, compare_value, color) ? 1.0f : 0.0f;\n        }\n    }\n");
      }
      P(ctx, "    return one;\n");
   }
}

/* One footprint serves native reduction and virtual weighted/PCF sampling.
 * Every tap is compared before interpolation; min/max ignores zero weights. */
static void
kk_sample_emit_one(struct nir_to_msl_ctx *ctx, struct kk_msl_sample_helper *helper)
{
   nir_tex_instr *tex = helper->tex;
   unsigned id = helper->id;
   const char *type = msl_type_for_def(ctx->types, &tex->def);
   bool cube = tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE;
   unsigned dims = tex->sampler_dim == GLSL_SAMPLER_DIM_3D ? 3 :
                   tex->sampler_dim == GLSL_SAMPLER_DIM_1D ? 1 : 2;
   bool reduction = tex->backend_flags & KK_SAMPLE_MINMAX;
   bool reduction_only = tex->backend_flags & (KK_SAMPLE_MIN | KK_SAMPLE_MAX);
   bool weighted_only = (tex->backend_flags & KK_SAMPLE_PREPARE_2D) && !reduction;
   bool gather = tex->op == nir_texop_tg4;
   bool emulation_only = tex->backend_flags & KK_SAMPLE_EMULATION_ONLY;
   /* Keep one call boundary around descriptor-selected emulation. Native
    * samples use the ordinary emitter; conservative helpers retain theirs. */
   /* Immutable MIN/MAX has no weighted arm. Inline its tap tree so texture
    * and sampler state can be shared across the footprint; retain the cold
    * outer call boundary and leave mutable/weighted helper emission alone. */
   const char *internal_attribute = emulation_only || reduction_only ?
      "always_inline" : "noinline";
   if (tex->op == nir_texop_lod) {
      P(ctx, "static __attribute__((always_inline)) float2 kk_sample_%u(", id);
      kk_sample_texture_param(ctx, tex);
      P(ctx, ", sampler native_sampler, constant SamplerTable &table, ulong address, float4 coord");
      kk_sample_parameters(ctx, tex, true);
      P(ctx, ", float reduction_lod) {\n    constant ushort *metadata = (constant ushort*)address;\n");
      if (ctx->shader->info.stage != MESA_SHADER_COMPUTE) {
         P(ctx, "    float native_unclamped = image.calculate_unclamped_lod(native_sampler, coord.%.*s);\n", cube ? 3 : dims, "xyz");
         P(ctx, "    float native_clamped = image.calculate_clamped_lod(native_sampler, coord.%.*s);\n", cube ? 3 : dims, "xyz");
      }
      if (tex->backend_flags & KK_SAMPLE_VIRTUAL) {
         P(ctx, "    if ((kk_sampler_emulation_active && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) != 0)) {\n");
         P(ctx, "        constant kk_sampler_params &params = kk_sampler_params_from_meta(metadata);\n");
         P(ctx, "        float lod = clamp(reduction_lod, params.min_lod, params.max_lod);\n");
         P(ctx, "        lod = clamp(lod, 0.0f, float(image.get_num_mip_levels() - 1u));\n");
         P(ctx, "        if ((params.flags & KK_SAMPLER_PARAMS_MIPMAP_LINEAR) == 0) lod = fmax(ceil(lod + 0.5f) - 1.0f, 0.0f);\n");
         P(ctx, "        return float2(lod, reduction_lod);\n    }\n");
      }
      if (ctx->shader->info.stage == MESA_SHADER_COMPUTE) {
         P(ctx, "    float low = kk_sampler_metadata_value(ulong(metadata), KK_SAMPLER_META_MIN_LOD, false), high = kk_sampler_metadata_value(ulong(metadata), KK_SAMPLER_META_MAX_LOD, false);\n");
         P(ctx, "    float lod = clamp(clamp(reduction_lod, low, high), 0.0f, float(image.get_num_mip_levels() - 1u));\n");
         P(ctx, "    if (kk_sampler_metadata_value(ulong(metadata), KK_SAMPLER_META_MIPMAP, false) == 0.0f) lod = fmax(ceil(lod + 0.5f) - 1.0f, 0.0f);\n");
         P(ctx, "    return float2(lod, reduction_lod);\n}\n");
      } else {
         P(ctx, "    float low = kk_sampler_metadata_value(ulong(metadata), KK_SAMPLER_META_MIN_LOD, false), high = kk_sampler_metadata_value(ulong(metadata), KK_SAMPLER_META_MAX_LOD, false);\n");
         P(ctx, "    float unclamped = native_unclamped, lod = native_clamped;\n");
         if (nir_tex_get_src(tex, nir_tex_src_bias)) P(ctx, "    lod += bias_value;\n");
         P(ctx, "    lod = clamp(lod, low, high);\n");
         if (reduction) {
            P(ctx, "    if (metadata[21] != 0u) {\n        lod = clamp(clamp(unclamped, low, high), 0.0f, float(image.get_num_mip_levels() - 1u));\n        if (kk_sampler_metadata_value(ulong(metadata), KK_SAMPLER_META_MIPMAP, false) == 0.0f) lod = fmax(ceil(lod + 0.5f) - 1.0f, 0.0f);\n    }\n");
         }
         P(ctx, "    return float2(lod, unclamped);\n}\n");
      }
      return;
   }

   bool fp = nir_alu_type_get_base_type(tex->dest_type) == nir_type_float;
   bool flush = fp && tex->def.bit_size == 32 &&
      nir_is_denorm_flush_to_zero(ctx->shader->info.float_controls_execution_mode, 32);
   const char *accum = tex->is_shadow ? "float" : "float4";
   if (kk_sample_has_footprint(tex)) {
      if (!weighted_only) {
      P(ctx, "static %s kk_reduce_%u(%s a, %s b, uint mode) {\n", type, id, type, type);
      if (flush) P(ctx, "    #pragma METAL fp math_mode(safe)\n");
      if (reduction_only) {
         P(ctx, "    %s value = %s(a, b);\n", type,
           tex->backend_flags & KK_SAMPLE_MIN ? (fp ? "fmin" : "min") :
                                               (fp ? "fmax" : "max"));
      } else {
         P(ctx, "    %s value = mode == 1u ? %s(a, b) : %s(a, b);\n", type, fp ? "fmin" : "min", fp ? "fmax" : "max");
      }
      if (flush && !tex->is_shadow)
         P(ctx, "    value = as_type<%s>(select(as_type<uint4>(value), as_type<uint4>(value) & 0x80000000u, (as_type<uint4>(value) & 0x7f800000u) == 0u));\n", type);
      P(ctx, "    return value;\n}\n");
      }
      P(ctx, "template<bool Border> static __attribute__((%s)) %s kk_tap_%u(", internal_attribute, type, id);
      kk_sample_texture_param(ctx, tex);
      P(ctx, ", sampler native_sampler, constant SamplerTable &table, constant ushort *metadata, float4 tap_coord, uint tap_level");
      if (tex->is_shadow) P(ctx, ", float compare_value");
      kk_sample_sparse_arg(ctx, tex, true);
      P(ctx, ") {\n");
      if (tex->backend_flags & KK_SAMPLE_SPARSE) {
         kk_sample_sparse_tap(ctx, tex, type, dims);
      }
      if (tex->is_shadow && !cube) {
         P(ctx, "    if (Border && (kk_sampler_emulation_active && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) != 0)) {\n");
         P(ctx, "        bool outside = false, unnormalized = (metadata[23] & 512u) != 0;\n");
         const char *sizefn[] = {"width", "height", "depth"};
         for (unsigned d = 0; d < dims; ++d)
            P(ctx, "        outside = outside || (((metadata[23] >> %u) & 7u) == 3u && (tap_coord.%c < 0.0f || tap_coord.%c >= (unnormalized ? float(image.get_%s(tap_level)) : 1.0f)));\n", d * 3, "xyz"[d], "xyz"[d], sizefn[d]);
         P(ctx, "        if (outside) return %s(kk_sampler_compare(kk_sampler_params_from_meta(metadata).compare_op, compare_value, as_type<float>(uint(metadata[12]) | (uint(metadata[13]) << 16))));\n    }\n", type);
      }
      kk_sample_border(ctx, tex, type, true);
      P(ctx, "}\n");
      P(ctx, "template<bool Border> static __attribute__((%s)) %s kk_level_%u(", internal_attribute, type, id);
      kk_sample_texture_param(ctx, tex);
      P(ctx, ", sampler native_sampler, constant SamplerTable &table, constant ushort *metadata, float4 coord, uint mip, bool linear_filter, uint mode, float3 texel_offset");
      if (tex->is_shadow) P(ctx, ", float compare_value");
      kk_sample_sparse_arg(ctx, tex, true);
      P(ctx, ") {\n    float3 size = float3(float(image.get_width(mip)), ");
      if (dims >= 2) { P(ctx, "float(image.get_height(mip))"); } else { P(ctx, "1.0f"); }
      if (dims == 3) { P(ctx, ", float(image.get_depth(mip)));\n"); } else { P(ctx, ", 1.0f);\n"); }
      if (cube) {
         P(ctx, "    float3 a = abs(coord.xyz); bool is_x = a.x >= a.y && a.x >= a.z; bool is_y = a.y >= a.z;\n");
         P(ctx, "    uint face = is_x ? (coord.x < 0 ? 1u : 0u) : (is_y ? (coord.y < 0 ? 3u : 2u) : (coord.z < 0 ? 5u : 4u));\n");
         P(ctx, "    float u = is_x ? (coord.x < 0 ? coord.z : -coord.z) : (is_y ? coord.x : (coord.z < 0 ? -coord.x : coord.x));\n");
         P(ctx, "    float v = is_x ? -coord.y : (is_y ? (coord.y < 0 ? -coord.z : coord.z) : -coord.y);\n");
         P(ctx, "    float3 uv = float3((float2(u, v) * (1.0f / fmax(a.x, fmax(a.y, a.z))) + 1.0f) * 0.5f, 0);\n");
      } else P(ctx, "    float3 uv = coord.xyz;\n");
      P(ctx, "    bool unnormalized = (metadata[23] & 512u) != 0;\n");
      P(ctx, "    float3 p = (unnormalized ? uv : uv * size) - (linear_filter ? 0.5f : 0.0f);\n");
      P(ctx, "    float3 base = floor(p), fraction = p - base; base += texel_offset;\n");
      if (weighted_only) {
         P(ctx, "    %s weighted = %s(0);\n", accum, accum);
      } else if (reduction_only) {
         P(ctx, "    %s result = %s(0); bool first = true;\n", type, type);
      } else {
         P(ctx, "    %s result = %s(0); %s weighted = %s(0); bool first = true;\n", type, type, accum, accum);
      }
      P(ctx, "    for (uint corner = 0; corner < %uu; ++corner) {\n        float weight = 1.0f; float3 center = base;\n", 1u << dims);
      for (unsigned d = 0; d < dims; ++d)
         P(ctx, "        bool hi%u = (corner & %uu) != 0; weight *= linear_filter ? (hi%u ? fraction.%c : 1.0f - fraction.%c) : (hi%u ? 0.0f : 1.0f); center.%c += hi%u ? 1.5f : 0.5f;\n", d, 1u << d, d, "xyz"[d], "xyz"[d], d, "xyz"[d], d);
      P(ctx, "        if (weight == 0.0f) continue; center = unnormalized ? center : center / size;\n");
      if (cube) {
         P(ctx, "        float u = center.x * 2.0f - 1.0f, v = center.y * 2.0f - 1.0f;\n");
         P(ctx, "        float3 direction = face == 0 ? float3(1, -v, -u) : face == 1 ? float3(-1, -v, u) : face == 2 ? float3(u, 1, v) : face == 3 ? float3(u, -1, -v) : face == 4 ? float3(u, -v, 1) : float3(-u, -v, -1);\n");
         P(ctx, "        float4 tap_coord = float4(direction, coord.w);\n");
      } else {
         P(ctx, "        float4 tap_coord = float4(center, 0);\n");
         if (tex->is_array) P(ctx, "        tap_coord.%c = coord.%c;\n", "xyzw"[dims], "xyzw"[dims]);
      }
      P(ctx, "        %s tap = kk_tap_%u<Border>(image, native_sampler, table, metadata, tap_coord, mip", type, id);
      if (tex->is_shadow) P(ctx, ", compare_value");
      kk_sample_sparse_arg(ctx, tex, false);
      P(ctx, ");\n        if (!linear_filter) return tap;\n");
      if (weighted_only) {
         P(ctx, "        weighted += %s(tap) * weight;\n    }\n    return %s(weighted);\n}\n", accum, type);
      } else if (reduction_only) {
         P(ctx, "        result = first ? tap : kk_reduce_%u(result, tap, mode);\n        first = false;\n    }\n    return result;\n}\n", id);
      } else {
         P(ctx, "        if (mode == 0u) weighted += %s(tap) * weight;\n", accum);
         P(ctx, "        else result = first ? tap : kk_reduce_%u(result, tap, mode);\n        first = false;\n    }\n    return mode == 0u ? %s(weighted) : result;\n}\n", id, type);
      }

      P(ctx, "template<bool Border> static __attribute__((%s)) %s kk_reduction_%u(", internal_attribute, type, id);
      kk_sample_texture_param(ctx, tex);
      P(ctx, ", sampler native_sampler, constant SamplerTable &table, constant ushort *metadata, float4 coord");
      kk_sample_parameters(ctx, tex, true);
      P(ctx, ", float reduction_lod, uint mode");
      kk_sample_sparse_arg(ctx, tex, true);
      P(ctx, ") {\n");
      P(ctx, "    bool virtual_sampler = (kk_sampler_emulation_active && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) != 0);\n");
      if (tex->backend_flags & KK_SAMPLE_SPARSE) {
         P(ctx, "    float low_lod, high_lod; bool mip_linear;\n    if (sparse.query_address != 0ul && (virtual_sampler || metadata[21] == 0u)) {\n        low_lod = sparse.params.min_lod; high_lod = sparse.params.max_lod; mip_linear = (sparse.params.flags & KK_SAMPLER_PARAMS_MIPMAP_LINEAR) != 0;\n    } else if (virtual_sampler) {\n");
      } else {
         P(ctx, "    float low_lod, high_lod; bool mip_linear;\n    if (virtual_sampler) {\n");
      }
      P(ctx, "        constant kk_sampler_params &params = kk_sampler_params_from_meta(metadata);\n        low_lod = params.min_lod; high_lod = params.max_lod; mip_linear = (params.flags & KK_SAMPLER_PARAMS_MIPMAP_LINEAR) != 0;\n    } else {\n        low_lod = kk_sampler_metadata_value(ulong(metadata), KK_SAMPLER_META_MIN_LOD, false); high_lod = kk_sampler_metadata_value(ulong(metadata), KK_SAMPLER_META_MAX_LOD, false); mip_linear = kk_sampler_metadata_value(ulong(metadata), KK_SAMPLER_META_MIPMAP, false) == 1.0f;\n    }\n");
      P(ctx, "    float lod = clamp(reduction_lod, low_lod, high_lod);\n");
      if (nir_tex_get_src(tex, nir_tex_src_min_lod)) P(ctx, "    lod = fmax(lod, min_lod_value);\n");
      P(ctx, "    bool linear_filter = (metadata[22] & (lod <= 0.0f ? 2u : 1u)) != 0;\n");
      P(ctx, "    uint levels = image.get_num_mip_levels();\n");
      if (tex->backend_flags & KK_SAMPLE_SPARSE) {
         P(ctx, "    if (sparse.query_address != 0ul) levels = *(constant uint*)(sparse.query_address + %zuul);\n", offsetof(struct kk_sparse_query_record, level_count));
      }
      P(ctx, "    lod = clamp(lod, 0.0f, float(levels - 1u));\n");
      P(ctx, "    float nearest = fmax(ceil(lod + 0.5f) - 1.0f, 0.0f); uint low = uint(mip_linear ? floor(lod) : nearest);\n");
      P(ctx, "    float3 texel_offset = float3(0);\n");
      nir_src *offset = nir_tex_get_src(tex, nir_tex_src_offset);
      if (offset) {
         P(ctx, "    texel_offset.%.*s = float%s(", dims, "xyz", dims == 1 ? "" : dims == 2 ? "2" : "3");
         if (nir_src_is_const(*offset)) msl_src_as_const(ctx, offset); else P(ctx, "offset_value");
         P(ctx, ");\n");
      }
      P(ctx, "    %s value = kk_level_%u<Border>(image, native_sampler, table, metadata, coord, low, linear_filter, mode, texel_offset", type, id);
      if (tex->is_shadow) P(ctx, ", compare_value");
      kk_sample_sparse_arg(ctx, tex, false);
      P(ctx, ");\n    uint next = low + 1u; if (mip_linear && float(low) < lod && next < levels) {\n");
      P(ctx, "        %s upper = kk_level_%u<Border>(image, native_sampler, table, metadata, coord, next, linear_filter, mode, texel_offset", type, id);
      if (tex->is_shadow) P(ctx, ", compare_value");
      kk_sample_sparse_arg(ctx, tex, false);
      if (weighted_only) {
         P(ctx, ");\n        value = %s(mix(%s(value), %s(upper), lod - float(low)));\n    }\n    return value;\n}\n", type, accum, accum);
      } else if (reduction_only) {
         P(ctx, ");\n        value = kk_reduce_%u(value, upper, mode);\n    }\n    return value;\n}\n", id);
      } else {
         P(ctx, ");\n        value = mode == 0u ? %s(mix(%s(value), %s(upper), lod - float(low))) : kk_reduce_%u(value, upper, mode);\n    }\n    return value;\n}\n", type, accum, accum, id);
      }
   }
   if (kk_sample_has_footprint(tex) &&
       (tex->backend_flags & (KK_SAMPLE_VIRTUAL | KK_SAMPLE_SPARSE))) {
      P(ctx, "template<bool Border> static __attribute__((%s)) %s kk_virtual_%u(", internal_attribute, type, id);
      kk_sample_texture_param(ctx, tex);
      P(ctx, ", sampler native_sampler, constant SamplerTable &table, constant ushort *metadata, float4 coord");
      kk_sample_parameters(ctx, tex, true);
      P(ctx, ", float reduction_lod, uint mode");
      kk_sample_sparse_arg(ctx, tex, true);
      P(ctx, ") {\n");
      if (nir_tex_get_src(tex, nir_tex_src_ddx) ||
          (tex->backend_flags & KK_SAMPLE_PREPARE_2D)) {
         unsigned gradient_dims = tex->backend_flags & KK_SAMPLE_PREPARE_2D ? 2 :
            nir_tex_get_src(tex, nir_tex_src_ddx)->ssa->num_components;
         if (tex->backend_flags & KK_SAMPLE_SPARSE) {
            P(ctx, "    kk_sampler_params params = sparse.query_address != 0ul ? sparse.params : kk_sampler_params_from_meta(metadata);\n");
         } else {
            P(ctx, "    constant kk_sampler_params &params = kk_sampler_params_from_meta(metadata);\n");
         }
         if (tex->backend_flags & KK_SAMPLE_PREPARE_2D) {
            P(ctx, "    if (operation == 1u) return kk_reduction_%u<Border>(image, native_sampler, table, metadata, coord", id);
            kk_sample_parameters(ctx, tex, false);
            P(ctx, ", reduction_lod, mode);\n");
         }
         /* This is exactly the old count-one case, before any footprint
          * dimensions, lengths or ratios are evaluated. */
         P(ctx, "    if ((params.flags & KK_SAMPLER_PARAMS_ANISOTROPY_ENABLE) == 0 || params.max_anisotropy < 2.0f) return kk_reduction_%u<Border>(image, native_sampler, table, metadata, coord", id);
         kk_sample_parameters(ctx, tex, false);
         P(ctx, ", reduction_lod, mode"); kk_sample_sparse_arg(ctx, tex, false); P(ctx, ");\n");
         P(ctx, "    float3 gx = float3(dx");
         for (unsigned c = gradient_dims; c < 3; ++c) P(ctx, ", 0.0f");
         P(ctx, "), gy = float3(dy");
         for (unsigned c = gradient_dims; c < 3; ++c) P(ctx, ", 0.0f");
         P(ctx, ");\n    float3 footprint_x = gx, footprint_y = gy;\n");
         if (cube) {
            P(ctx, "    float3 a = abs(coord.xyz); uint axis = a.x >= a.y && a.x >= a.z ? 0u : (a.y >= a.z ? 1u : 2u);\n");
            P(ctx, "    float major = abs(coord[axis]), sign_major = coord[axis] < 0.0f ? -1.0f : 1.0f;\n");
            P(ctx, "    uint u_axis = axis == 0u ? 2u : 0u, v_axis = axis == 1u ? 2u : 1u;\n");
            P(ctx, "    float2 uv = float2(coord[u_axis], coord[v_axis]);\n");
            P(ctx, "    footprint_x = float3((float2(gx[u_axis], gx[v_axis]) * major - uv * (gx[axis] * sign_major)) * (0.5f / (major * major)), 0.0f);\n");
            P(ctx, "    footprint_y = float3((float2(gy[u_axis], gy[v_axis]) * major - uv * (gy[axis] * sign_major)) * (0.5f / (major * major)), 0.0f);\n");
         }
         P(ctx, "    if ((metadata[23] & 512u) == 0) {\n        float3 size = float3(float(image.get_width()), ");
         if (dims >= 2) { P(ctx, "float(image.get_height())"); } else { P(ctx, "1.0f"); }
         if (dims == 3) { P(ctx, ", float(image.get_depth()));\n"); } else { P(ctx, ", 1.0f);\n"); }
         P(ctx, "        footprint_x *= size; footprint_y *= size;\n    }\n");
         P(ctx, "    float lx = length(footprint_x), ly = length(footprint_y), major_length = fmax(lx, ly), minor_length = fmin(lx, ly);\n");
         /* Match create_sampler's native power-of-two policy, retaining the
          * original fp32 input in the shared record and testing enable first. */
         P(ctx, "    uint requested = uint(params.max_anisotropy);\n    float limit = float(1u << (32u - clz(requested - 1u)));\n");
         P(ctx, "    float ratio = clamp(major_length / fmax(minor_length, 0x1p-126f), 1.0f, limit);\n");
         P(ctx, "    uint count = uint(ceil(ratio));\n    if (count == 1u) return kk_reduction_%u<Border>(image, native_sampler, table, metadata, coord", id);
         kk_sample_parameters(ctx, tex, false); P(ctx, ", reduction_lod, mode"); kk_sample_sparse_arg(ctx, tex, false); P(ctx, ");\n");
         P(ctx, "    float lod = reduction_lod - log2(ratio);\n");
         P(ctx, "    float3 direction = lx >= ly ? gx : gy;\n");
         if (weighted_only) {
            P(ctx, "    %s weighted = %s(0);\n", accum, accum);
         } else if (reduction_only) {
            P(ctx, "    %s result = %s(0);\n", type, type);
         } else {
            P(ctx, "    %s result = %s(0); %s weighted = %s(0);\n", type, type, accum, accum);
         }
         P(ctx, "    for (uint tap = 0u; tap < count; ++tap) {\n        float4 sample_coord = coord;\n");
         P(ctx, "        sample_coord.%.*s += direction.%.*s * ((float(tap) + 0.5f) / float(count) - 0.5f);\n", cube ? 3 : dims, "xyz", cube ? 3 : dims, "xyz");
         P(ctx, "        %s value = kk_reduction_%u<Border>(image, native_sampler, table, metadata, sample_coord", type, id);
         kk_sample_parameters(ctx, tex, false);
         P(ctx, ", lod, mode"); kk_sample_sparse_arg(ctx, tex, false);
         if (weighted_only) {
            P(ctx, ");\n        weighted += %s(value);\n    }\n    return %s(weighted / float(count));\n", accum, type);
         } else if (reduction_only) {
            P(ctx, ");\n        result = tap == 0u ? value : kk_reduce_%u(result, value, mode);\n    }\n    return result;\n", id);
         } else {
            P(ctx, ");\n        if (mode == 0u) weighted += %s(value);\n", accum);
            P(ctx, "        else result = tap == 0u ? value : kk_reduce_%u(result, value, mode);\n    }\n    return mode == 0u ? %s(weighted / float(count)) : result;\n", id, type);
         }
      } else {
         P(ctx, "    return kk_reduction_%u<Border>(image, native_sampler, table, metadata, coord", id);
         kk_sample_parameters(ctx, tex, false); P(ctx, ", reduction_lod, mode"); kk_sample_sparse_arg(ctx, tex, false); P(ctx, ");\n");
      }
      P(ctx, "}\n");
   }
   P(ctx, "template<bool Border> static __attribute__((%s)) %s kk_native_%u(", internal_attribute, type, id);
   kk_sample_texture_param(ctx, tex);
   P(ctx, ", sampler native_sampler, constant SamplerTable &table, constant ushort *metadata, float4 coord");
   kk_sample_parameters(ctx, tex, true);
   if (tex->backend_flags & KK_SAMPLE_NATIVE_CUBE_LOD) P(ctx, ", float native_cube_lod");
   P(ctx, ") {\n");
   kk_sample_border(ctx, tex, type, false); P(ctx, "}\n");

   P(ctx, "static __attribute__((noinline)) %s kk_sample_%u(", type, id);
   kk_sample_texture_param(ctx, tex);
   P(ctx, ", sampler native_sampler, constant SamplerTable &table, ulong address, float4 coord");
   kk_sample_parameters_mode(ctx, tex, true, true);
   if (nir_tex_get_src(tex, nir_tex_src_backend2)) P(ctx, ", float reduction_lod");
   kk_sample_sparse_arg(ctx, tex, true);
   P(ctx, ") {\n    constant ushort *metadata = (constant ushort*)address;\n");
   if (tex->backend_flags & KK_SAMPLE_MIN) {
      P(ctx, "    uint mode = kk_sampler_emulation_active ? 1u : 0u;\n");
   } else if (tex->backend_flags & KK_SAMPLE_MAX) {
      P(ctx, "    uint mode = kk_sampler_emulation_active ? 2u : 0u;\n");
   } else if (reduction) {
      P(ctx, "    uint mode = kk_sampler_emulation_active ? metadata[21] : 0u;\n");
   } else {
      P(ctx, tex->backend_flags & KK_SAMPLE_SPARSE ? "    uint mode = 0u;\n" : "    constexpr uint mode = 0u;\n");
   }
   if (tex->backend_flags & KK_SAMPLE_SPARSE) {
      P(ctx, "    if (sparse.query_address != 0ul && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) == 0 && mode == 0u) mode = (sparse.physical_facts & %uu) >> %uu;\n", KK_SPARSE_PHYSICAL_REDUCTION_MASK, KK_SPARSE_PHYSICAL_REDUCTION_SHIFT);
   }
   if (tex->backend_flags & KK_SAMPLE_BORDER_KNOWN) {
      P(ctx, "    bool border = kk_sampler_emulation_active;\n");
   } else if (tex->backend_flags & KK_SAMPLE_BORDER) {
      P(ctx, "    bool border = kk_sampler_emulation_active && short(metadata[5]) >= 0;\n");
   } else {
      P(ctx, "    constexpr bool border = false;\n");
   }
   if (tex->backend_flags & KK_SAMPLE_PREPARE_2D) {
      /* Only implicit callers (operation0) require quad derivatives; their
       * descriptor dispatch is quad-uniform. Explicit callers supply inputs. */
      P(ctx, "    float2 dx = source_dx, dy = source_dy;\n");
      /* The descriptor pass admits prepared implicit operations only in
       * fragments. Other stages supply explicit level/gradient operands. */
      if (ctx->shader->info.stage == MESA_SHADER_FRAGMENT) {
         P(ctx, "    if (operation == 0u) { dx = dfdx(coord.xy); dy = dfdy(coord.xy); }\n");
      }
      P(ctx, "    float sampler_bias = kk_sampler_metadata_value(address, %uu, %s);\n",
        KK_SAMPLER_META_BIAS, tex->backend_flags & KK_SAMPLE_LOWER_NATIVE_BIAS ? "true" : "false");
      if (tex->backend_flags & KK_SAMPLE_LOWER_NATIVE_BIAS) {
         P(ctx, "    float total_bias = (kk_sampler_emulation_active && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) != 0) ? sampler_bias + source_value : float(half(sampler_bias) + half(source_value));\n");
      } else {
         P(ctx, "    float total_bias = sampler_bias + source_value;\n");
      }
      P(ctx, "    float sample_lod = operation == 1u ? total_bias : 0.0f;\n");
      P(ctx, "    if (operation == 2u) { float scale = exp2(sampler_bias); dx *= scale; dy *= scale; }\n");
      if (kk_sample_has_footprint(tex)) {
         P(ctx, "    float reduction_lod = sample_lod;\n");
         P(ctx, "    if (operation != 1u && ((kk_sampler_emulation_active && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) != 0) || mode != 0u)) {\n");
         P(ctx, "        float2 size = float2(image.get_width(), image.get_height());\n");
         P(ctx, "        float2 gx = dx * size, gy = dy * size;\n");
         P(ctx, "        reduction_lod = log2(fmax(sqrt(gx.x * gx.x + gx.y * gx.y), sqrt(gy.x * gy.x + gy.y * gy.y))) + (operation == 0u ? total_bias : 0.0f);\n    }\n");
      }
      P(ctx, "    if (operation == 0u) { float scale = exp2(total_bias); dx *= scale; dy *= scale; }\n");
   }
   if (kk_sample_has_footprint(tex)) {
      P(ctx, tex->backend_flags & KK_SAMPLE_VIRTUAL ? "    bool virtual_sampler = (kk_sampler_emulation_active && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) != 0);\n" : "    constexpr bool virtual_sampler = false;\n");
      if (tex->backend_flags & KK_SAMPLE_SPARSE_POINT) {
         P(ctx, "    if (!border) return kk_level_%u<false>(image, native_sampler, table, metadata, coord, 0u, false, 0u, float3(0)", id);
         if (tex->is_shadow) P(ctx, ", compare_value");
         kk_sample_sparse_arg(ctx, tex, false); P(ctx, ");\n");
         P(ctx, "    return kk_level_%u<true>(image, native_sampler, table, metadata, coord, 0u, false, 0u, float3(0)", id);
         if (tex->is_shadow) P(ctx, ", compare_value");
         kk_sample_sparse_arg(ctx, tex, false); P(ctx, ");\n");
      }
      if (tex->backend_flags & KK_SAMPLE_SPARSE) {
         P(ctx, "    if (virtual_sampler || mode != 0u || sparse.query_address != 0ul) {\n");
      } else {
         P(ctx, "    if (virtual_sampler || mode != 0u) {\n");
      }
      for (unsigned border = 0; border < 2; ++border) {
         if (border && !(tex->backend_flags & KK_SAMPLE_BORDER)) continue;
         P(ctx, "        if (%s) {\n", border ? "border" : "!border");
         if (tex->backend_flags & (KK_SAMPLE_VIRTUAL | KK_SAMPLE_SPARSE)) {
            P(ctx, "            if (virtual_sampler%s) return kk_virtual_%u<%s>(image, native_sampler, table, metadata, coord", tex->backend_flags & KK_SAMPLE_SPARSE ? " || sparse.query_address != 0ul" : "", id, border ? "true" : "false");
            kk_sample_parameters(ctx, tex, false); P(ctx, ", reduction_lod, mode"); kk_sample_sparse_arg(ctx, tex, false); P(ctx, ");\n");
         }
         P(ctx, "            return kk_reduction_%u<%s>(image, native_sampler, table, metadata, coord", id, border ? "true" : "false");
         kk_sample_parameters(ctx, tex, false); P(ctx, ", reduction_lod, mode"); kk_sample_sparse_arg(ctx, tex, false); P(ctx, ");\n        }\n");
      }
      P(ctx, "    }\n");
   }
   if (!emulation_only) {
      P(ctx, "    if (!border) return kk_native_%u<false>(image, native_sampler, table, metadata, coord", id);
      kk_sample_parameters(ctx, tex, false);
      if (tex->backend_flags & KK_SAMPLE_NATIVE_CUBE_LOD) P(ctx, ", reduction_lod");
      P(ctx, ");\n");
   }
   if (tex->backend_flags & KK_SAMPLE_BORDER) {
      P(ctx, "    return kk_native_%u<true>(image, native_sampler, table, metadata, coord", id);
      kk_sample_parameters(ctx, tex, false);
   if (tex->backend_flags & KK_SAMPLE_NATIVE_CUBE_LOD) P(ctx, ", reduction_lod");
   P(ctx, ");\n");
   } else P(ctx, "    return %s(0);\n", type);
   P(ctx, "}\n");
   if (tex->backend_flags & KK_SAMPLE_SPARSE) {
      assert(!gather && tex->op != nir_texop_lod);
      P(ctx, "static __attribute__((noinline)) kk_sparse_sample_result<%s> kk_sparse_sample_%u(", type, id);
      kk_sample_texture_param(ctx, tex);
      P(ctx, ", sampler native_sampler, constant SamplerTable &table, ulong address, float4 coord");
      kk_sample_parameters(ctx, tex, true);
      P(ctx, ", float reduction_lod, ulong native_params_address, ulong query_address, uint physical_facts) {\n");
      P(ctx, "    kk_sparse_sample_state sparse; sparse.query_address = query_address; sparse.physical_facts = physical_facts; sparse.all_resident = true;\n");
      P(ctx, "    constant ushort *metadata = (constant ushort*)address;\n");
      P(ctx, "    if (query_address != 0ul) {\n        bool virtual_sampler = (kk_sampler_emulation_active && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) != 0);\n        sparse.params = virtual_sampler ? kk_sampler_params_from_meta(metadata) : *(constant kk_sampler_params*)native_params_address;\n");
      if (!(tex->backend_flags & KK_SAMPLE_SPARSE_POINT)) {
         P(ctx, "        if (!virtual_sampler) reduction_lod += sparse.params.lod_bias;\n");
      }
      P(ctx, "    }\n    %s value = kk_sample_%u(image, native_sampler, table, address, coord", type, id);
      kk_sample_parameters(ctx, tex, false);
      P(ctx, ", reduction_lod, sparse);\n    return {value, sparse.all_resident};\n}\n");
   }
}

static void
kk_sample_emit_metadata(struct nir_to_msl_ctx *ctx, bool sparse, bool compare)
{
   P(ctx, "%s", kk_sampler_params_msl_declaration());
   if (sparse) {
      P(ctx, "struct kk_sparse_sample_state { ulong query_address; kk_sampler_params params; uint physical_facts; bool all_resident; };\n");
      P(ctx, "template<typename T> struct kk_sparse_sample_result { T color; bool all_resident; T value() const { return color; } bool resident() const { return all_resident; } };\n");
   }
   P(ctx, "static constant kk_sampler_params &kk_sampler_params_from_meta(constant ushort *metadata) {\n");
   P(ctx, "    ulong address = ulong(metadata[6]) | (ulong(metadata[7]) << 16) | (ulong(metadata[8]) << 32) | (ulong(metadata[11]) << 48);\n");
   P(ctx, "    return *(constant kk_sampler_params *)address;\n}\n");
   if (compare) {
      P(ctx, "static bool kk_sampler_compare(uint op, float reference, float depth) {\n");
      P(ctx, "    #pragma METAL fp math_mode(safe)\n    switch (op) {\n    case 0u: return false;\n    case 1u: return reference < depth;\n    case 2u: return reference == depth;\n    case 3u: return reference <= depth;\n    case 4u: return reference > depth;\n    case 5u: return reference != depth;\n    case 6u: return reference >= depth;\n    case 7u: return true;\n    }\n    return false;\n}\n");
   }
   /* One selector declaration supplies helper calls and the public query. */
#define KK_METADATA_CONSTANT(name) P(ctx, "constant uint " #name " = %uu;\n", name)
   KK_METADATA_CONSTANT(KK_SAMPLER_META_MIN_LOD);
   KK_METADATA_CONSTANT(KK_SAMPLER_META_MAX_LOD);
   KK_METADATA_CONSTANT(KK_SAMPLER_META_MIPMAP);
#undef KK_METADATA_CONSTANT
   P(ctx, "static float kk_sampler_metadata_value(ulong address, uint field, bool lower_native_bias) {\n");
   P(ctx, "    constant ushort *metadata = (constant ushort *)address;\n");
   P(ctx, "    bool virtual_sampler = (kk_sampler_emulation_active && (metadata[22] & KK_SAMPLER_FILTER_VIRTUAL) != 0);\n");
   P(ctx, "    if (field == %uu) return virtual_sampler ? 1.0f : 0.0f;\n", KK_SAMPLER_META_VIRTUAL);
   /* Query bias remains numeric at byte40 on both descriptor encodings.
    * Native bias queries need no original-record pointer reconstruction. */
   P(ctx, "    if (!virtual_sampler && field == %uu) {\n", KK_SAMPLER_META_BIAS);
   P(ctx, "        if ((metadata[22] & KK_SAMPLER_FILTER_PARAMS_POINTER) != 0) return lower_native_bias && %s ? float(as_type<half>(metadata[20])) : 0.0f;\n", ctx->native_sampler_lod_bias ? "false" : "true");
   P(ctx, "        return lower_native_bias ? float(as_type<half>(metadata[6])) : 0.0f;\n    }\n");
   P(ctx, "    if (!virtual_sampler && field == %uu) return float(as_type<half>(metadata[20]));\n", KK_SAMPLER_META_QUERY_BIAS);
   P(ctx, "    if ((metadata[22] & (KK_SAMPLER_FILTER_PARAMS_POINTER | KK_SAMPLER_FILTER_VIRTUAL)) != 0) {\n        constant kk_sampler_params &params = kk_sampler_params_from_meta(metadata);\n        switch (field) {\n");
   P(ctx, "        case %uu: return virtual_sampler ? params.lod_bias : (lower_native_bias && %s ? float(half(params.lod_bias)) : 0.0f);\n", KK_SAMPLER_META_BIAS, ctx->native_sampler_lod_bias ? "false" : "true");
   P(ctx, "        case %uu: return virtual_sampler ? params.lod_bias : float(half(params.lod_bias));\n", KK_SAMPLER_META_QUERY_BIAS);
   P(ctx, "        case %uu: return virtual_sampler ? params.min_lod : float(half(params.min_lod));\n", KK_SAMPLER_META_MIN_LOD);
   P(ctx, "        case %uu: return virtual_sampler ? params.max_lod : float(half(params.max_lod));\n", KK_SAMPLER_META_MAX_LOD);
   P(ctx, "        case %uu: return (params.flags & KK_SAMPLER_PARAMS_MIPMAP_LINEAR) != 0 ? 1.0f : 0.0f;\n", KK_SAMPLER_META_MIPMAP);
   P(ctx, "        case %uu: return (params.flags & KK_SAMPLER_PARAMS_ANISOTROPY_ENABLE) != 0 ? params.max_anisotropy : 1.0f;\n", KK_SAMPLER_META_MAX_ANISOTROPY);
   P(ctx, "        case %uu: return float(params.reduction_mode);\n", KK_SAMPLER_META_REDUCTION_MODE);
   P(ctx, "        }\n    } else {\n        switch (field) {\n");
   P(ctx, "        case %uu: return lower_native_bias ? float(as_type<half>(metadata[6])) : 0.0f;\n", KK_SAMPLER_META_BIAS);
   P(ctx, "        case %uu: return float(as_type<half>(metadata[20]));\n", KK_SAMPLER_META_QUERY_BIAS);
   P(ctx, "        case %uu: return float(as_type<half>(metadata[7]));\n", KK_SAMPLER_META_MIN_LOD);
   P(ctx, "        case %uu: return float(as_type<half>(metadata[8]));\n", KK_SAMPLER_META_MAX_LOD);
   P(ctx, "        case %uu: return float(metadata[11]);\n", KK_SAMPLER_META_MIPMAP);
   /* The legacy numeric packet has no original identity for these queries.
    * Current producers always publish the pointer. Never invent a physical
    * default if an obsolete packet is supplied to a new selector. */
   P(ctx, "        case %uu:\n        case %uu: return as_type<float>(0x7fc00000u);\n", KK_SAMPLER_META_MAX_ANISOTROPY, KK_SAMPLER_META_REDUCTION_MODE);
   P(ctx, "        }\n    }\n    return 0.0f;\n}\n");
}

static void
kk_sample_emit_helpers(struct nir_to_msl_ctx *ctx)
{
   bool metadata = false, sparse = false, compare = false;
   struct kk_msl_sample_helper *first = NULL, **tail = &first;
   nir_foreach_function_impl(impl, ctx->shader) nir_foreach_block(block, impl) nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_tex) continue;
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      if (!(tex->backend_flags & (KK_SAMPLE_HELPER | KK_SAMPLE_METADATA))) continue;
      metadata |= (tex->backend_flags & KK_SAMPLE_METADATA) ||
                  kk_sample_has_footprint(tex) || tex->op == nir_texop_lod ||
                  (tex->is_shadow && tex->op == nir_texop_tg4 &&
                   (tex->backend_flags & KK_SAMPLE_BORDER));
      if (tex->backend_flags & KK_SAMPLE_METADATA) continue;
      sparse |= tex->backend_flags & KK_SAMPLE_SPARSE;
      compare |= tex->is_shadow;
      if (!ctx->sampler_helpers)
         ctx->sampler_helpers = _mesa_hash_table_create(ctx->text, kk_sample_key_hash, kk_sample_key_equal);
      struct kk_msl_sample_key key;
      kk_sample_key(tex, &key);
      if (_mesa_hash_table_search(ctx->sampler_helpers, &key)) continue;
      struct kk_msl_sample_helper *helper = rzalloc(ctx->text, struct kk_msl_sample_helper);
      helper->key = key;
      helper->tex = tex; helper->id = ctx->sampler_helpers->entries;
      _mesa_hash_table_insert(ctx->sampler_helpers, &helper->key, helper);
      *tail = helper;
      tail = &helper->next;
   }
   if (metadata) kk_sample_emit_metadata(ctx, sparse, compare);
   for (struct kk_msl_sample_helper *helper = first; helper; helper = helper->next)
      kk_sample_emit_one(ctx, helper);
}

static bool
kk_sample_emit_call(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex)
{
   if (tex->backend_flags & KK_SAMPLE_METADATA) {
      P_IND(ctx, "t%u = kk_sampler_metadata_value(as_type<ulong>(as_type<uint4>(", tex->def.index);
      src_to_msl(ctx, nir_tex_get_src(tex, nir_tex_src_backend1));
      P(ctx, ").xy), %uu, %s);\n",
         (tex->backend_flags & KK_SAMPLE_METADATA_MASK) >> KK_SAMPLE_METADATA_SHIFT,
         tex->backend_flags & KK_SAMPLE_LOWER_NATIVE_BIAS ? "true" : "false");
      return true;
   }
   if (!(tex->backend_flags & KK_SAMPLE_HELPER)) return false;
   struct kk_msl_sample_key key;
   kk_sample_key(tex, &key);
   struct hash_entry *entry = _mesa_hash_table_search(ctx->sampler_helpers, &key);assert(entry);
   struct kk_msl_sample_helper *helper = entry->data;
   bool sparse = tex->backend_flags & KK_SAMPLE_SPARSE;
   bool guarded = !sparse && tex->op != nir_texop_lod;
   if (guarded) {
      P_IND(ctx, "if (kk_sampler_emulation_active) {\n");
      ctx->indentlevel++;
   }
   if (sparse) {
      P_IND(ctx, "auto sparse%u = kk_sparse_sample_%u(", tex->def.index, helper->id);
   } else {
      P_IND(ctx, "t%u = kk_sample_%u(", tex->def.index, helper->id);
   }
   src_to_msl(ctx, nir_tex_get_src(tex, nir_tex_src_texture_handle));P(ctx, ", ");
   src_to_msl(ctx, nir_tex_get_src(tex, nir_tex_src_sampler_handle));P(ctx, ", sampler_table, as_type<ulong>(as_type<uint4>(");
   src_to_msl(ctx, nir_tex_get_src(tex, nir_tex_src_backend1));P(ctx, ").xy), float4(");
   nir_src *coord = nir_tex_get_src(tex, nir_tex_src_coord);src_to_msl(ctx, coord);
   for (unsigned i = coord->ssa->num_components; i < 4; ++i) P(ctx, ", 0.0f");
   P(ctx, ")");
   bool prepared = tex->backend_flags & KK_SAMPLE_PREPARE_2D;
   if (prepared) {
      nir_src *value = nir_tex_get_src(tex, tex->op == nir_texop_txl ?
                                         nir_tex_src_lod : nir_tex_src_bias);
      P(ctx, ", ");
      if (value) src_to_msl(ctx, value); else P(ctx, "0.0f");
      const nir_tex_src_type gradients[] = {nir_tex_src_ddx, nir_tex_src_ddy};
      for (unsigned i = 0; i < ARRAY_SIZE(gradients); ++i) {
         nir_src *gradient = nir_tex_get_src(tex, gradients[i]);
         P(ctx, ", ");
         if (gradient) src_to_msl(ctx, gradient); else P(ctx, "float2(0.0f)");
      }
      P(ctx, ", %uu", tex->op == nir_texop_txl ? 1u : tex->op == nir_texop_txd ? 2u : 0u);
   }
   for (unsigned i = 0; i < ARRAY_SIZE(kk_sample_args); ++i) {
      nir_src *src = nir_tex_get_src(tex, kk_sample_args[i]);
      if (prepared && (kk_sample_args[i] == nir_tex_src_bias ||
                       kk_sample_args[i] == nir_tex_src_lod ||
                       kk_sample_args[i] == nir_tex_src_ddx ||
                       kk_sample_args[i] == nir_tex_src_ddy)) continue;
      if (!src || (kk_sample_args[i] == nir_tex_src_offset && nir_src_is_const(*src))) continue;
      P(ctx, ", ");src_to_msl(ctx, src);
   }
   if (nir_tex_get_src(tex, nir_tex_src_backend2)) {
      P(ctx, ", ");src_to_msl(ctx, nir_tex_get_src(tex, nir_tex_src_backend2));P(ctx, ".x");
   }
   if (sparse) {
      P(ctx, ", as_type<ulong>(as_type<uint4>(");
      src_to_msl(ctx, nir_tex_get_src(tex, nir_tex_src_backend1)); P(ctx, ").zw), as_type<ulong>(as_type<uint4>(");
      src_to_msl(ctx, nir_tex_get_src(tex, nir_tex_src_backend2)); P(ctx, ").yz), as_type<uint4>(");
      src_to_msl(ctx, nir_tex_get_src(tex, nir_tex_src_backend2)); P(ctx, ").w");
   }
   P(ctx, ");\n");
   if (sparse) {
      P_IND(ctx, "t%u = sparse%u.value();\n", tex->def.index, tex->def.index);
   }
   if (guarded) {
      ctx->indentlevel--;
      P_IND(ctx, "} else {\n");
      ctx->indentlevel++;
      unsigned saved = tex->backend_flags;
      tex->backend_flags &= ~KK_SAMPLE_HELPER;
      tex_to_msl(ctx, tex);
      tex->backend_flags = saved;
      ctx->indentlevel--;
      P_IND(ctx, "}\n");
   }
   return true;
}

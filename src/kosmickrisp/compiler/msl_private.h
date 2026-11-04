/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "util/string_buffer.h"
#include "nir.h"

struct io_slot_info {
   nir_alu_type type;
   uint32_t interpolation;
   unsigned num_components;
   bool centroid;
   bool sample;
   bool uses_interpolant;
   bool per_vertex;
};

/* Metal accepts [[point_size]] only in point-topology Mesh vertices. Other
 * topologies keep the shader's PointSize writes in threadgroup storage and
 * leave it out of the native vertex record. */
static inline uint64_t
kk_mesh_native_vertex_outputs(const nir_shader *s, uint64_t mask)
{
   return s->info.mesh.primitive_type == MESA_PRIM_POINTS ? mask :
      mask & ~VARYING_BIT_PSIZ;
}

struct kk_mesh_msl_info {
   bool queries;
   uint32_t retained_vertex_stride, retained_primitive_stride;
   uint32_t retained_fields[4][4];
   uint32_t view_mask, view_index_offset;
   bool flat_heap, projected_basis;
   unsigned flat_heap_offset;
   bool provoking_last;
};

struct nir_to_msl_ctx {
   FILE *output;
   struct hash_table *types;
   struct hash_table *sampler_helpers;
   nir_shader *shader;
   nir_function *current_function;
   struct _mesa_string_buffer *text;
   uint64_t disabled_workarounds;
   bool native_sampler_lod_bias;
   bool native_query_texture_lod;
   /* ulong textures and their min/max atomics (Apple8 macOS and later). */
   bool native_atomic64_minmax;
   const struct msl_per_vertex_inputs *per_vertex_inputs;
   bool uses_per_draw_data;
   bool has_sampler_helpers;
   bool uses_sampler_emulation;
   bool requires_sampler_emulation;
   bool sparse_residency;
   bool interlock;
   bool requires_msl41;
   bool uses_matrix_coordinates;
   bool uses_poly_layer_selector;
   unsigned short indentlevel;
   /* First construct that has no MSL form; the translation then fails. */
   const char *error;
   /* While a function body is printed: SSA values whose instruction assigns
    * every component unconditionally. Their declarations need no zero. */
   BITSET_WORD *assigned_defs;
   /* Floating-point math mode of the open ALU statement scope, if any.
    * Consecutive ALU statements with the same mode share one scope. */
   const char *open_math_mode;
   /* The function body may test the root buffer address through kk_buf0
    * and kk_nobuf0, which are declared before the body when it does. */
   bool buf0_guard;
   bool buf0_guard_used;
   bool uses_pipeline_statistics;
   bool uses_atomic64 : 1;
   bool uses_image_atomic64 : 1;
   struct io_slot_info inputs_info[NUM_TOTAL_VARYING_SLOTS];
   struct io_slot_info outputs_info[NUM_TOTAL_VARYING_SLOTS];
   /* Initialized only for Task/Mesh. Keep this stage-local data last. */
   struct kk_mesh_msl_info mesh;
};

#define P_IND(ctx, ...)                                                        \
   do {                                                                        \
      for (unsigned i = 0; i < (ctx)->indentlevel; i++)                        \
         _mesa_string_buffer_append((ctx)->text, "    ");                      \
      _mesa_string_buffer_printf((ctx)->text, __VA_ARGS__);                    \
   } while (0);

#define P(ctx, ...) _mesa_string_buffer_printf((ctx)->text, __VA_ARGS__);

static inline void
msl_def_assigned(struct nir_to_msl_ctx *ctx, const nir_def *def)
{
   if (ctx->assigned_defs)
      BITSET_SET(ctx->assigned_defs, def->index);
}

static inline void
msl_fail(struct nir_to_msl_ctx *ctx, const char *message)
{
   if (!ctx->error)
      ctx->error = message;
}

#define P_INDENT(ctx)                                                          \
   do {                                                                        \
      for (unsigned i = 0; i < (ctx)->indentlevel; i++)                        \
         _mesa_string_buffer_append((ctx)->text, "    ");                      \
   } while (0)

/* Perform type inference. The returned value is a
 * map from nir_def* to base type.*/

struct hash_table *msl_infer_types(struct nir_to_msl_ctx *ctx);

const char *msl_type_for_def(struct hash_table *types, nir_def *def);

const char *msl_uint_type(uint8_t bit_size, uint8_t num_components);

const char *msl_type_for_src(struct hash_table *types, nir_src *src);

const char *msl_bitcast_for_src(struct hash_table *types, nir_src *src);

const char *msl_printed_type_for_src(struct hash_table *types, nir_src *src,
                                     unsigned num_components);

void msl_src_as_const(struct nir_to_msl_ctx *ctx, nir_src *src);

void msl_emit_io_blocks(struct nir_to_msl_ctx *ctx, nir_shader *shader);

void msl_emit_output_var(struct nir_to_msl_ctx *ctx, nir_shader *shader);

void msl_gather_io_info(struct nir_to_msl_ctx *ctx,
                        struct io_slot_info *info_array_input,
                        struct io_slot_info *info_array_output);

void msl_input_name(struct nir_to_msl_ctx *ctx, unsigned location,
                    unsigned component);

void msl_output_name(struct nir_to_msl_ctx *ctx, unsigned location,
                     unsigned component, bool from_input);

uint32_t msl_input_num_components(struct nir_to_msl_ctx *ctx,
                                  uint32_t location);

uint32_t msl_output_num_components(struct nir_to_msl_ctx *ctx,
                                   uint32_t location);

bool msl_src_is_float(struct nir_to_msl_ctx *ctx, nir_src *src);
bool msl_def_is_sampler(struct nir_to_msl_ctx *ctx, nir_def *def);
bool msl_def_is_texture(struct nir_to_msl_ctx *ctx, nir_def *def);

void msl_nir_lower_subgroups(nir_shader *nir);

bool msl_nir_lower_algebraic_late(nir_shader *shader);

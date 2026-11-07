/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_CELLS_NIR_H
#define KK_FRAGMENT_SHADING_RATE_CELLS_NIR_H
#include "kk_fragment_shading_rate_transaction_nir.h"

/* One dispatch assigns256 linear lanes to four8x8 regions. Four ordered
 * region_base values cover the existing32x32 leaf, not a framebuffer-sized
 * allocation. The encoder/scheduler owns that order and capture lifetime. */
struct kk_vrs_cells_nir {
   struct kk_interlock_framebuffer_nir *framebuffer;
   unsigned samples;
   nir_variable *env, *storage, *cell, *extent, *lane, *rate, *original_draw;
   nir_variable *geometric, *passing, *final, *found, *source_pixel, *source_sample;
   nir_variable *anchor, *anchor_coord, *centroid;
};

static nir_def *
kk_vrs_cells_parameters(nir_builder *b, void *data)
{
   return nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, nir_load_per_draw_ptr_kk(b, 1, 64),
         offsetof(struct kk_vrs_transaction_context, envelope)), .align_mul = 8);
}

static nir_def *
kk_vrs_cells_record(nir_builder *b, void *data, nir_def *sample)
{
   struct kk_vrs_cells_nir *s = data;
   return kk_interlock_record(b, nir_load_var(b, s->env),
      nir_load_var(b, s->anchor), sample);
}

static nir_def *
kk_vrs_cells_sample_position(nir_builder *b, void *data, nir_def *sample)
{
   struct kk_vrs_cells_nir *s = data;
   nir_def *offset = nir_u2u64(b, nir_imul_imm(b, sample, 2 * sizeof(float)));
   return nir_load_global_constant(b, 2, 32, nir_iadd(b,
      nir_iadd_imm(b, nir_load_var(b, s->env),
         offsetof(struct kk_interlock_envelope, sample_positions)), offset), .align_mul = 4);
}

static nir_def *
kk_vrs_cells_begin(nir_builder *b, struct kk_interlock_program_nir *program, void *data)
{
   struct kk_vrs_cells_nir *s = data;
   nir_def *context = nir_load_per_draw_ptr_kk(b, 1, 64);
   nir_def *env = s->framebuffer->transaction.render->parameters(b,
      s->framebuffer->transaction.render->data);
   nir_store_var(b, s->env, env, 1);
   nir_store_var(b, s->storage, kk_interlock_env_load(b, context, 1, 32,
      offsetof(struct kk_vrs_transaction_context, capture_primitive)), 1);
   nir_store_var(b, s->original_draw, kk_interlock_env_load(b, context, 1, 64,
      offsetof(struct kk_vrs_context, original_per_draw)), 1);
   nir_def *index = nir_load_local_invocation_index(b);
   nir_def *region = nir_iadd(b, nir_ushr_imm(b, index, 6),
      kk_interlock_env_load(b, context, 1, 32,
         offsetof(struct kk_vrs_transaction_context, region_base)));
   nir_def *origin = nir_vec2(b,
      kk_interlock_env_load(b, context, 1, 32, offsetof(struct kk_vrs_context, origin_x)),
      kk_interlock_env_load(b, context, 1, 32, offsetof(struct kk_vrs_context, origin_y)));
   nir_def *region_pixel = nir_iadd(b, origin, nir_imul_imm(b,
      nir_vec2(b, nir_iand_imm(b, region, 3), nir_ushr_imm(b, region, 2)), 8));
   nir_def *extent = kk_nir_vrs_combined_extent(b, context, region_pixel, s->samples);
   nir_store_var(b, s->extent, extent, 3);
   nir_def *columns = nir_udiv(b, nir_imm_int(b, 8), nir_channel(b, extent, 0));
   nir_def *rows = nir_udiv(b, nir_imm_int(b, 8), nir_channel(b, extent, 1));
   nir_def *quad_columns = nir_ushr_imm(b, columns, 1);
   nir_def *quad = nir_ushr_imm(b, nir_iand_imm(b, index, 63), 2);
   nir_def *lane = nir_iand_imm(b, index, 3);
   nir_def *logical = nir_vec2(b,
      nir_iadd(b, nir_imul_imm(b, nir_umod(b, quad, quad_columns), 2), nir_iand_imm(b, lane, 1)),
      nir_iadd(b, nir_imul_imm(b, nir_udiv(b, quad, quad_columns), 2), nir_ushr_imm(b, lane, 1)));
   nir_store_var(b, s->cell, nir_iadd(b, region_pixel, nir_imul(b, logical, extent)), 3);
   nir_store_var(b, s->lane, lane, 1);
   nir_store_var(b, s->rate, nir_ior(b,
      nir_ishl_imm(b, nir_ufind_msb(b, nir_channel(b, extent, 0)), 2),
      nir_ufind_msb(b, nir_channel(b, extent, 1))), 1);
   nir_store_var(b, s->geometric, nir_imm_int(b, 0), 1);
   nir_store_var(b, s->passing, nir_imm_int(b, 0), 1);
   nir_store_var(b, s->final, nir_imm_int(b, 0), 1);
   nir_store_var(b, s->found, nir_imm_false(b), 1);
   nir_store_var(b, s->source_pixel, nir_load_var(b, s->cell), 3);
   nir_store_var(b, s->source_sample, nir_imm_int(b, 0), 1);
   /* Every active four-lane group owns one full logical derivative quad. */
   return nir_iand(b, nir_ult_imm(b, region, 16),
      nir_ult(b, quad, nir_ushr_imm(b, nir_imul(b, columns, rows), 2)));
}

static nir_def *
kk_vrs_cells_count(nir_builder *b, struct kk_interlock_program_nir *program, void *data)
{
   struct kk_vrs_cells_nir *s = data;
   nir_def *extent = nir_load_var(b, s->extent);
   return nir_imul_imm(b, nir_imul(b, nir_channel(b, extent, 0),
      nir_channel(b, extent, 1)), s->samples);
}

static nir_def *
kk_vrs_cells_shift(nir_builder *b, struct kk_interlock_program_nir *program,
   nir_def *slot, void *data)
{
   struct kk_vrs_cells_nir *s = data;
   return nir_isub(b, kk_vrs_cells_count(b, program, data),
      nir_imul_imm(b, nir_iadd_imm(b, nir_udiv_imm(b, slot, s->samples), 1), s->samples));
}

static struct kk_interlock_coarse_sample_nir
kk_vrs_cells_sample(nir_builder *b, struct kk_interlock_program_nir *program,
   nir_def *slot, bool after_body, void *data)
{
   struct kk_vrs_cells_nir *s = data;
   nir_def *env = nir_load_var(b, s->env);
   nir_def *ordinal = nir_udiv_imm(b, slot, s->samples);
   nir_def *sample = nir_umod_imm(b, slot, s->samples);
   nir_def *fw = nir_channel(b, nir_load_var(b, s->extent), 0);
   nir_def *pixel = nir_iadd(b, nir_load_var(b, s->cell),
      nir_vec2(b, nir_umod(b, ordinal, fw), nir_udiv(b, ordinal, fw)));
   nir_def *storage = nir_load_var(b, s->storage);
   nir_def *cell = kk_interlock_cell(b, env, pixel, storage);
   nir_def *original = nir_load_global_constant(b, 1, 32,
      kk_interlock_word_address(b, kk_interlock_env(b, env, coverage), cell), .align_mul = 4);
   nir_def *mask = nir_iand_imm(b, original, BITFIELD_MASK(s->samples));
   nir_def *shift = kk_vrs_cells_shift(b, program, slot, data);
   if (!after_body) {
      nir_store_var(b, s->geometric, nir_ior(b, nir_load_var(b, s->geometric),
         nir_ishl(b, mask, shift)), 1);
      nir_def *covered = nir_ine_imm(b, mask, 0);
      nir_def *first = nir_iand(b, covered, nir_inot(b, nir_load_var(b, s->found)));
      nir_store_var(b, s->source_pixel,
         nir_bcsel(b, first, pixel, nir_load_var(b, s->source_pixel)), 3);
      nir_store_var(b, s->source_sample, nir_bcsel(b, first,
         nir_find_lsb(b, nir_umax(b, mask, nir_imm_int(b, 1))),
         nir_load_var(b, s->source_sample)), 1);
      nir_store_var(b, s->found, nir_ior(b, covered, nir_load_var(b, s->found)), 1);
   }
   nir_def *coverage = after_body && s->framebuffer->early ?
      nir_iand_imm(b, nir_ushr(b, nir_load_var(b, s->passing), shift), BITFIELD_MASK(s->samples)) : mask;
   nir_def *anchor = kk_interlock_quad_anchor(b, env, pixel, storage);
   nir_def *record = kk_interlock_record(b, env, anchor, sample);
   nir_def *zw = kk_interlock_quad_load(b,
      nir_iadd_imm(b, record, s->framebuffer->transaction.render->inputs.stride), 8, 2, 32, pixel);
   nir_def *metadata = nir_load_global_constant(b, 4, 32, nir_iadd(b,
      kk_interlock_env(b, env, metadata), nir_u2u64(b, nir_imul_imm(b, anchor, 16))), .align_mul = 16);
   nir_def *origin = nir_vec2(b, kk_interlock_env(b, env, tile_x), kk_interlock_env(b, env, tile_y));
   nir_def *center = nir_fadd_imm(b, nir_u2f32(b, pixel), .5f);
   return (struct kk_interlock_coarse_sample_nir){
      .pixel = nir_isub(b, pixel, origin),
      .coord = nir_vec4(b, nir_channel(b, center, 0), nir_channel(b, center, 1),
         nir_channel(b, zw, 0), nir_channel(b, zw, 1)),
      .sample = sample, .coverage = coverage, .storage_primitive = storage,
      .primitive = nir_channel(b, metadata, 0),
      .front_face = nir_ine_imm(b, nir_channel(b, metadata, 1), 0),
      .layer = nir_channel(b, metadata, 2), .view_index = nir_channel(b, metadata, 3),
   };
}

static void
kk_vrs_cells_tested(nir_builder *b, struct kk_interlock_program_nir *program,
   nir_def *slot, nir_def *passed, bool after_body, void *data)
{
   struct kk_vrs_cells_nir *s = data;
   nir_variable *dst = after_body ? s->final : s->passing;
   nir_def *sample = nir_umod_imm(b, slot, s->samples);
   nir_def *bit = nir_ishl(b, nir_imm_int(b, 1),
      nir_iadd(b, kk_vrs_cells_shift(b, program, slot, data), sample));
   nir_def *old = nir_iand(b, nir_load_var(b, dst), nir_inot(b, bit));
   nir_store_var(b, dst, nir_ior(b, old, nir_bcsel(b, passed, bit, nir_imm_int(b, 0))), 1);
}

static void
kk_vrs_cells_body(nir_builder *b, struct kk_interlock_program_nir *program,
   nir_def *surviving, void *data)
{
   struct kk_vrs_cells_nir *s = data;
   struct kk_interlock_transaction_nir *t = &s->framebuffer->transaction;
   nir_def *env = nir_load_var(b, s->env);
   nir_def *source = nir_load_var(b, s->source_pixel);
   nir_def *sample = nir_load_var(b, s->source_sample);
   nir_def *found = nir_load_var(b, s->found);
   /* A helper borrows a real packet from the same logical quad. Its affine
    * planes are evaluated at that helper's own logical center below. */
   nir_def *borrowed = nir_imm_false(b);
   for (unsigned lane = 0; lane < 4; lane++) {
      nir_def *valid = nir_quad_broadcast(b, found, nir_imm_int(b, lane));
      nir_def *use = nir_iand(b, nir_inot(b, nir_ior(b, found, borrowed)), valid);
      source = nir_bcsel(b, use, nir_quad_broadcast(b,
         nir_load_var(b, s->source_pixel), nir_imm_int(b, lane)), source);
      sample = nir_bcsel(b, use, nir_quad_broadcast(b,
         nir_load_var(b, s->source_sample), nir_imm_int(b, lane)), sample);
      borrowed = nir_ior(b, borrowed, valid);
   }
   nir_def *anchor = kk_interlock_quad_anchor(b, env, source, nir_load_var(b, s->storage));
   nir_store_var(b, s->anchor, anchor, 1);
   nir_def *anchor_center = nir_fadd_imm(b, nir_u2f32(b, source), .5f);
   nir_store_var(b, s->anchor_coord, nir_vec4(b,
      nir_channel(b, anchor_center, 0), nir_channel(b, anchor_center, 1),
      nir_imm_float(b, 0), nir_imm_float(b, 1)), 15);
   nir_def *record = kk_interlock_record(b, env, anchor, nir_imm_int(b, 0));
   nir_def *extent = nir_load_var(b, s->extent);
   nir_def *center = nir_fadd(b, nir_u2f32(b, nir_load_var(b, s->cell)),
      nir_fmul_imm(b, nir_u2f32(b, extent), .5f));
   nir_def *base_offset = nir_fadd_imm(b,
      kk_vrs_cells_sample_position(b, data, nir_imm_int(b, 0)), -.5f);
   nir_def *zw = kk_interlock_captured_offset(b, record,
      nir_iadd_imm(b, record, t->render->inputs.stride), &t->render->inputs,
      8, 2, source, nir_fsub(b, center, anchor_center), base_offset, false);
   nir_store_var(b, program->coord, nir_vec4(b,
      nir_channel(b, center, 0), nir_channel(b, center, 1),
      nir_channel(b, zw, 0), nir_channel(b, zw, 1)), 15);
   nir_store_var(b, s->centroid, nir_bcsel(b, found,
      nir_fadd(b, nir_u2f32(b, source), kk_vrs_cells_sample_position(b, data, sample)), center), 3);
   nir_store_var(b, t->pixel, source, 3);
   nir_store_var(b, t->primitive, nir_load_var(b, s->storage), 1);
   nir_store_var(b, t->anchor, anchor, 1);
   nir_store_var(b, program->sample, sample, 1);
   nir_store_var(b, program->sample_position, kk_vrs_cells_sample_position(b, data, sample), 3);
   nir_def *metadata = nir_load_global_constant(b, 4, 32, nir_iadd(b,
      kk_interlock_env(b, env, metadata), nir_u2u64(b, nir_imul_imm(b, anchor, 16))), .align_mul = 16);
   nir_store_var(b, program->primitive, nir_channel(b, metadata, 0), 1);
   nir_store_var(b, program->front_face, nir_ine_imm(b, nir_channel(b, metadata, 1), 0), 1);
   nir_store_var(b, program->layer, nir_channel(b, metadata, 2), 1);
   nir_store_var(b, program->view_index, nir_channel(b, metadata, 3), 1);
   nir_store_var(b, program->coverage, nir_load_var(b,
      s->framebuffer->post_depth_coverage ? s->passing : s->geometric), 1);
}

static struct kk_vrs_cells_nir
kk_vrs_cells_init(nir_shader *shader, struct kk_interlock_framebuffer_nir *framebuffer,
   unsigned samples)
{
   struct kk_vrs_cells_nir s = {.framebuffer = framebuffer, .samples = samples};
#define CELL_VAR(field, type) s.field = kk_interlock_program_var(shader, type, "vrs_" #field)
   CELL_VAR(env, glsl_uint64_t_type()); CELL_VAR(storage, glsl_uint_type());
   CELL_VAR(cell, glsl_uvec2_type()); CELL_VAR(extent, glsl_uvec2_type());
   CELL_VAR(lane, glsl_uint_type()); CELL_VAR(rate, glsl_uint_type());
   CELL_VAR(original_draw, glsl_uint64_t_type());
   CELL_VAR(geometric, glsl_uint_type()); CELL_VAR(passing, glsl_uint_type());
   CELL_VAR(final, glsl_uint_type()); CELL_VAR(found, glsl_bool_type());
   CELL_VAR(source_pixel, glsl_uvec2_type()); CELL_VAR(source_sample, glsl_uint_type());
   CELL_VAR(anchor, glsl_uint_type()); CELL_VAR(anchor_coord, glsl_vec4_type());
   CELL_VAR(centroid, glsl_vec2_type());
#undef CELL_VAR
   return s;
}

static bool
kk_vrs_cells_remove_interlock(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_begin_invocation_interlock &&
       intr->intrinsic != nir_intrinsic_end_invocation_interlock)
      return false;
   nir_instr_remove(&intr->instr);
   return true;
}

/* Produces one in-encoder tile-kernel body, not encoder/global ordering. Each
 * original primitive job is ordered by the existing continuation; its logical
 * cells touch disjoint fine shadows. Only the wrapper's generated ROG pair is
 * removed. An application interlock or sample-frequency program requires its
 * mandated native1x1 execution and is not a coarse-body input. */
static bool
kk_vrs_cells_shader(nir_shader *shader,
   struct kk_interlock_framebuffer_nir *framebuffer, unsigned samples,
   unsigned *local_output_bytes)
{
   if (!shader || !framebuffer || !framebuffer->transaction.render ||
       framebuffer->transaction.render->sample_shading ||
       (samples != 1 && samples != 2 && samples != 4))
      return false;
   nir_foreach_function_impl(impl, shader) nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
         if (op == nir_intrinsic_begin_invocation_interlock ||
             op == nir_intrinsic_end_invocation_interlock)
            return false;
      }
   struct kk_vrs_cells_nir cells = kk_vrs_cells_init(shader, framebuffer, samples);
   struct kk_vrs_transaction_nir source = {
      .cells = {.data = &cells, .begin_cell = kk_vrs_cells_begin,
         .sample_count = kk_vrs_cells_count, .sample = kk_vrs_cells_sample,
         .tested = kk_vrs_cells_tested, .begin_body = kk_vrs_cells_body},
      .logical = {.logical_lane = cells.lane, .rate = cells.rate,
         .original_per_draw = cells.original_draw},
      .inputs = {.capture = {.coord = cells.anchor_coord, .data = &cells,
         .record = kk_vrs_cells_record, .sample_position = kk_vrs_cells_sample_position},
         .centroid = cells.centroid},
      .sample_shift = kk_vrs_cells_shift, .samples = samples,
   };
   if (!kk_vrs_wrap_framebuffer_transaction(shader, framebuffer, &source, local_output_bytes))
      return false;
   nir_shader_intrinsics_pass(shader, kk_vrs_cells_remove_interlock,
      nir_metadata_none, NULL);
   shader->info.stage = MESA_SHADER_COMPUTE;
   memset(&shader->info.cs, 0, sizeof(shader->info.cs));
   shader->info.workgroup_size[0] = 16;
   shader->info.workgroup_size[1] = 16;
   shader->info.workgroup_size[2] = 1;
   shader->info.derivative_group = DERIVATIVE_GROUP_LINEAR;
   NIR_PASS(_, shader, nir_opt_dce);
   nir_shader_gather_info(shader, nir_shader_get_entrypoint(shader));
   return true;
}
#endif

/* SPDX-License-Identifier: MIT */
/*
 * VK_KHR_fragment_shading_rate through Metal quad groups.
 *
 * Fragments are at most 2x2 pixels and aligned, so every fragment is one
 * Metal quad or one half of it. Each quad lane evaluates one fragment of a
 * virtual quad of fragments:
 *
 * - The lowest covered lane of each fragment is its "real" lane. It runs the
 *   application side effects at the fragment center, and its outputs, its
 *   discard state and its result are broadcast to the other pixels.
 * - The remaining lanes evaluate the neighboring fragments, which only feed
 *   derivatives. They behave as helper invocations.
 *
 * Inputs are extrapolated from pixel centers to the lane's fragment center.
 * Attribute*W and 1/W are affine in screen space, so the extrapolation is
 * exact up to rounding. Derivatives are recomputed from the actual lane
 * positions, which handles fragments of two rows or columns whose real lanes
 * sit in different columns or rows.
 */
#include "kk_nir_lower_fsr.h"

#include "kk_cmd_buffer.h"
#include "kk_private.h"
#include "kosmickrisp/compiler/kk_fragment_shading_rate.h"
#include "kosmickrisp/compiler/nir_to_msl.h"

#include "nir_builder.h"

struct fsr_ctx {
   const struct kk_fsr_lower_options *opts;
   nir_variable *demoted;
   nir_def *qx, *qy;      /* pixel parity in the quad, u32 */
   nir_def *dx, *dy;      /* lane position minus pixel center, float */
   nir_def *vx, *vy;      /* lane position, float */
   nir_def *fw;           /* 1/W at the lane position */
   nir_def *coord;        /* FragCoord at the lane position */
   nir_def *real;         /* this lane runs its fragment, bool */
   nir_def *sel;          /* xor distance to the real lane, u32 */
   bool sample_group;     /* the shader may run once per covered sample */
   nir_def *ox, *oy;      /* fragment origin in pixels, u32 */
   nir_def *lead;         /* real lane of this sample invocation, bool */
   nir_def *won;          /* this invocation runs the fragment's side effects */
   nir_def *slot, *tag;   /* fragment slot address (u64, 0 if none) and tag */
   /* Presence handshake inputs (sample_group only). */
   nir_def *px, *py, *p, *alive, *group, *wide, *tall;
   nir_def *bits[4];      /* 1 << (p ^ k) */
   nir_variable *covered_var;
   nir_instr *covered_store;
   nir_def *rate;         /* final rate, Vulkan encoding */
   nir_def *fx, *fy;      /* fragment width and height, float */
   /* Neighbor displacements for the derivative solve. */
   nir_def *dhx, *dhy, *dvx, *dvy, *inv_det;
   nir_instr *top_end;    /* last instruction of the setup block */
};

static nir_def *
fsr_from(nir_builder *b, nir_def *v, nir_def *distance)
{
   /* The source lane for an xor distance in the quad: 1 horizontal,
    * 2 vertical, 3 diagonal. All quad lanes are active here. */
   nir_def *h = nir_quad_swap_horizontal(b, v);
   nir_def *vv = nir_quad_swap_vertical(b, v);
   nir_def *d = nir_quad_swap_diagonal(b, v);
   return nir_bcsel(b, nir_ieq_imm(b, distance, 0), v,
          nir_bcsel(b, nir_ieq_imm(b, distance, 1), h,
          nir_bcsel(b, nir_ieq_imm(b, distance, 2), vv, d)));
}

static nir_def *
fsr_broadcast(nir_builder *b, struct fsr_ctx *ctx, nir_def *v)
{
   if (v->bit_size == 1)
      return nir_ine_imm(b, fsr_from(b, nir_b2i32(b, v), ctx->sel), 0);
   return fsr_from(b, v, ctx->sel);
}

/* Value of an affine screen-space function at the lane position, from its
 * values at the hardware pixel centers. */
static nir_def *
fsr_combine(nir_builder *b, struct fsr_ctx *ctx, nir_def *g, nir_def *h, nir_def *v)
{
   nir_def *gx = nir_bcsel(b, nir_ieq_imm(b, ctx->qx, 0), nir_fsub(b, h, g),
                           nir_fsub(b, g, h));
   nir_def *gy = nir_bcsel(b, nir_ieq_imm(b, ctx->qy, 0), nir_fsub(b, v, g),
                           nir_fsub(b, g, v));
   return nir_fadd(b, g, nir_fadd(b, nir_fmul(b, gx, ctx->dx),
                                  nir_fmul(b, gy, ctx->dy)));
}

static nir_def *
fsr_extrapolate(nir_builder *b, struct fsr_ctx *ctx, nir_def *g)
{
   nir_def *h = nir_quad_swap_horizontal(b, g);
   nir_def *v = nir_quad_swap_vertical(b, g);
   return fsr_combine(b, ctx, g, h, v);
}

/* With the slot handshake, the neighbor values are read at the current
 * cursor, before the handshake's atomics: a lane that does not run the
 * sample has its atomics suppressed, and values read from it afterwards
 * are not reliable. The combination is built at post. */
static nir_def *
fsr_extrapolate_at(nir_builder *b, struct fsr_ctx *ctx, nir_def *g, nir_cursor post)
{
   nir_def *h = nir_quad_swap_horizontal(b, g);
   nir_def *v = nir_quad_swap_vertical(b, g);
   b->cursor = post;
   return fsr_combine(b, ctx, g, h, v);
}

/* Fragment-scale derivatives at the lane positions: solve the 2x2 system
 * formed by the horizontal and vertical neighbor differences. */
static void
fsr_derivatives(nir_builder *b, struct fsr_ctx *ctx, nir_def *value,
                nir_def **ddx, nir_def **ddy)
{
   nir_def *v = value->bit_size == 32 ? value : nir_f2f32(b, value);
   nir_def *dh = nir_fsub(b, nir_quad_swap_horizontal(b, v), v);
   nir_def *dv = nir_fsub(b, nir_quad_swap_vertical(b, v), v);
   nir_def *gx = nir_fmul(b, nir_fsub(b, nir_fmul(b, dh, ctx->dvy),
                                      nir_fmul(b, dv, ctx->dhy)), ctx->inv_det);
   nir_def *gy = nir_fmul(b, nir_fsub(b, nir_fmul(b, dv, ctx->dhx),
                                      nir_fmul(b, dh, ctx->dvx)), ctx->inv_det);
   *ddx = nir_fmul(b, gx, ctx->fx);
   *ddy = nir_fmul(b, gy, ctx->fy);
   if (value->bit_size != 32) {
      *ddx = nir_f2fN(b, *ddx, value->bit_size);
      *ddy = nir_f2fN(b, *ddy, value->bit_size);
   }
}

static nir_def *
fsr_load_root(nir_builder *b, unsigned components, unsigned bit_size,
              unsigned offset)
{
   nir_def *root = nir_load_buffer_ptr_kk(b, 1, 64, .binding = KK_ARGUMENT_ROOT);
   return nir_build_load_global_constant(
      b, components, bit_size, nir_iadd_imm(b, root, offset),
      .align_mul = bit_size / 8, .access = ACCESS_CAN_SPECULATE);
}

static void
fsr_setup(nir_builder *b, struct fsr_ctx *ctx)
{
   const struct kk_fsr_lower_options *opts = ctx->opts;
   nir_def *frag = nir_load_frag_coord(b);
   nir_def *fcx = nir_channel(b, frag, 0), *fcy = nir_channel(b, frag, 1);
   /* Neighbor depth and 1/W before the slot handshake (fsr_extrapolate_at). */
   nir_def *fz = NULL, *fwv = NULL, *zh = NULL, *zv = NULL, *wh = NULL, *wv = NULL;
   if (ctx->sample_group) {
      fz = nir_channel(b, frag, 2);
      fwv = nir_channel(b, frag, 3);
      zh = nir_quad_swap_horizontal(b, fz);
      zv = nir_quad_swap_vertical(b, fz);
      wh = nir_quad_swap_horizontal(b, fwv);
      wv = nir_quad_swap_vertical(b, fwv);
   }
   nir_def *px = nir_f2u32(b, nir_ffloor(b, fcx));
   nir_def *py = nir_f2u32(b, nir_ffloor(b, fcy));
   ctx->qx = nir_iand_imm(b, px, 1);
   ctx->qy = nir_iand_imm(b, py, 1);

   /* Combined rate: pipeline, then primitive, then attachment. */
   nir_def *state = opts->dynamic_state
      ? fsr_load_root(b, 1, 32, offsetof(struct kk_root_descriptor_table, fsr.state))
      : nir_imm_int(b, opts->static_state);
   nir_def *rate = nir_iand_imm(b, state, 0xf);
   nir_def *op0 = nir_ubitfield_extract_imm(b, state, 4, 3);
   nir_def *op1 = nir_ubitfield_extract_imm(b, state, 8, 3);
   nir_def *primitive = nir_imm_int(b, 0);
   if (opts->primitive_rate) {
      nir_io_semantics io = {.location = VARYING_SLOT_PRIMITIVE_SHADING_RATE,
                             .num_slots = 1};
      primitive = nir_iand_imm(b, nir_load_input(b, 1, 32, nir_imm_int(b, 0),
         .base = 0, .component = 0, .dest_type = nir_type_int32,
         .io_semantics = io), 0xf);
      b->shader->info.inputs_read |= VARYING_BIT_PRIMITIVE_SHADING_RATE;
      if (opts->primitive_rate_per_primitive)
         b->shader->info.per_primitive_inputs |= VARYING_BIT_PRIMITIVE_SHADING_RATE;
   }
   /* The existing coarse engine's combiners, on pixel extents. */
   nir_def *extent = kk_nir_vrs_combine_extent(b, kk_nir_vrs_decode_extent(b, rate),
                                               kk_nir_vrs_decode_extent(b, primitive), op0);
   /* Placeholder for the attachment rate; kk_nir_lower_fsr_late reads
    * the attachment after descriptor and texture lowering. */
   nir_def *attachment = opts->attachment_rate ? nir_load_frag_shading_rate(b)
                                               : nir_imm_int(b, 0);
   extent = kk_nir_vrs_combine_extent(b, extent, kk_nir_vrs_decode_extent(b, attachment), op1);
   /* Every combination of 1 and 2 pixels is supported: clamp each axis. */
   nir_def *lw = nir_b2i32(b, nir_uge_imm(b, nir_channel(b, extent, 0), 2));
   nir_def *lh = nir_b2i32(b, nir_uge_imm(b, nir_channel(b, extent, 1), 2));
   if (opts->dynamic_sample_mask) {
      /* A partial sample mask selects 1x1. */
      nir_def *mask = fsr_load_root(b, 1, 8, offsetof(struct kk_root_descriptor_table,
                                                       draw.sample_mask));
      nir_def *full = nir_ieq_imm(b, nir_iand_imm(b, nir_u2u32(b, mask), opts->full_sample_mask),
                                  opts->full_sample_mask);
      lw = nir_bcsel(b, full, lw, nir_imm_int(b, 0));
      lh = nir_bcsel(b, full, lh, nir_imm_int(b, 0));
   }
   ctx->rate = nir_ior(b, nir_ishl_imm(b, lw, 2), lh);
   nir_def *wide = nir_ieq_imm(b, lw, 1), *tall = nir_ieq_imm(b, lh, 1);
   ctx->fx = nir_bcsel(b, wide, nir_imm_float(b, 2.0f), nir_imm_float(b, 1.0f));
   ctx->fy = nir_bcsel(b, tall, nir_imm_float(b, 2.0f), nir_imm_float(b, 1.0f));

   /* Real lane: lowest covered quad position of this lane's fragment. */
   nir_def *p = nir_ior(b, ctx->qx, nir_ishl_imm(b, ctx->qy, 1));
   nir_def *alive = nir_b2i32(b, nir_inot(b, nir_load_helper_invocation(b, 1)));
   nir_def *alive_h = nir_quad_swap_horizontal(b, alive);
   nir_def *alive_v = nir_quad_swap_vertical(b, alive);
   nir_def *alive_d = nir_quad_swap_diagonal(b, alive);
   nir_def *one = nir_imm_int(b, 1);
   nir_def *bit0 = nir_ishl(b, one, p);
   nir_def *bit1 = nir_ishl(b, one, nir_ixor(b, p, nir_imm_int(b, 1)));
   nir_def *bit2 = nir_ishl(b, one, nir_ixor(b, p, nir_imm_int(b, 2)));
   nir_def *bit3 = nir_ishl(b, one, nir_ixor(b, p, nir_imm_int(b, 3)));
   nir_def *zero = nir_imm_int(b, 0);
   nir_def *group = nir_ior(b, bit0,
      nir_ior(b, nir_bcsel(b, wide, bit1, zero),
         nir_ior(b, nir_bcsel(b, tall, bit2, zero),
                    nir_bcsel(b, nir_iand(b, wide, tall), bit3, zero))));
   nir_def *covered = nir_ior(b, nir_imul(b, alive, bit0),
      nir_ior(b, nir_imul(b, alive_h, bit1),
         nir_ior(b, nir_imul(b, alive_v, bit2), nir_imul(b, alive_d, bit3))));
   if (ctx->sample_group) {
      /* fsr_claim replaces the helper-flag coverage with the lanes whose
       * memory operations run in this sample invocation. */
      ctx->px = px;
      ctx->py = py;
      ctx->p = p;
      ctx->alive = alive;
      ctx->group = group;
      ctx->wide = wide;
      ctx->tall = tall;
      ctx->bits[0] = bit0;
      ctx->bits[1] = bit1;
      ctx->bits[2] = bit2;
      ctx->bits[3] = bit3;
      ctx->covered_var = nir_local_variable_create(b->impl, glsl_uint_type(), "kk_fsr_covered");
      nir_store_var(b, ctx->covered_var, covered, 1);
      assert(b->cursor.option == nir_cursor_after_instr);
      ctx->covered_store = b->cursor.instr;
      covered = nir_load_var(b, ctx->covered_var);
   }
   nir_def *candidates = nir_iand(b, covered, group);
   nir_def *real_pos = nir_find_lsb(b, nir_bcsel(b, nir_ine_imm(b, candidates, 0),
                                                 candidates, group));
   ctx->real = nir_iand(b, nir_ieq(b, real_pos, p), nir_ine_imm(b, alive, 0));
   ctx->sel = nir_ixor(b, p, real_pos);

   /* Lane position: the fragment center of the real lane, shifted by whole
    * fragments for the other lanes of the same fragment. */
   nir_def *rqx = nir_iand_imm(b, real_pos, 1);
   nir_def *rqy = nir_ushr_imm(b, real_pos, 1);
   nir_def *ox = nir_bcsel(b, wide, nir_iand_imm(b, px, ~1u), px);
   nir_def *oy = nir_bcsel(b, tall, nir_iand_imm(b, py, ~1u), py);
   ctx->ox = ox;
   ctx->oy = oy;
   nir_def *half_x = nir_fmul_imm(b, ctx->fx, 0.5);
   nir_def *half_y = nir_fmul_imm(b, ctx->fy, 0.5);
   nir_def *shift_x = nir_i2f32(b, nir_isub(b, ctx->qx, rqx));
   nir_def *shift_y = nir_i2f32(b, nir_isub(b, ctx->qy, rqy));
   ctx->vx = nir_fadd(b, nir_fadd(b, nir_u2f32(b, ox), half_x), nir_fmul(b, shift_x, ctx->fx));
   ctx->vy = nir_fadd(b, nir_fadd(b, nir_u2f32(b, oy), half_y), nir_fmul(b, shift_y, ctx->fy));
   ctx->dx = nir_fsub(b, ctx->vx, nir_fadd_imm(b, nir_u2f32(b, px), 0.5));
   ctx->dy = nir_fsub(b, ctx->vy, nir_fadd_imm(b, nir_u2f32(b, py), 0.5));

   ctx->dhx = nir_fsub(b, nir_quad_swap_horizontal(b, ctx->vx), ctx->vx);
   ctx->dhy = nir_fsub(b, nir_quad_swap_horizontal(b, ctx->vy), ctx->vy);
   ctx->dvx = nir_fsub(b, nir_quad_swap_vertical(b, ctx->vx), ctx->vx);
   ctx->dvy = nir_fsub(b, nir_quad_swap_vertical(b, ctx->vy), ctx->vy);
   ctx->inv_det = nir_frcp(b, nir_fsub(b, nir_fmul(b, ctx->dhx, ctx->dvy),
                                       nir_fmul(b, ctx->dhy, ctx->dvx)));

   if (ctx->sample_group) {
      ctx->fw = fsr_combine(b, ctx, fwv, wh, wv);
      ctx->coord = nir_vec4(b, ctx->vx, ctx->vy, fsr_combine(b, ctx, fz, zh, zv), ctx->fw);
   } else {
      ctx->fw = fsr_extrapolate(b, ctx, nir_channel(b, frag, 3));
      ctx->coord = nir_vec4(b, ctx->vx, ctx->vy,
                            fsr_extrapolate(b, ctx, nir_channel(b, frag, 2)), ctx->fw);
   }
   ctx->top_end = nir_def_instr(ctx->coord);
}

static bool
fsr_is_output(nir_intrinsic_instr *intr)
{
   return intr->intrinsic == nir_intrinsic_store_output;
}

bool
kk_nir_fsr_supported(nir_shader *nir)
{
   if (nir->info.fs.uses_sample_shading || nir->info.fs.uses_fbfetch_output ||
       (nir->info.outputs_written & (BITFIELD64_BIT(FRAG_RESULT_DEPTH) |
                                     BITFIELD64_BIT(FRAG_RESULT_STENCIL) |
                                     BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK))) ||
       nir->info.fs.sample_interlock_ordered || nir->info.fs.sample_interlock_unordered ||
       nir->info.fs.pixel_interlock_ordered || nir->info.fs.pixel_interlock_unordered)
      return false;

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_block *last = nir_impl_last_block(impl);
   nir_foreach_function_impl(fimpl, nir) {
      if (fimpl != impl)
         return false;
   }
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         switch (intr->intrinsic) {
         case nir_intrinsic_load_sample_mask_in:
         case nir_intrinsic_load_sample_id:
         case nir_intrinsic_load_sample_pos:
         case nir_intrinsic_load_sample_pos_or_center:
         case nir_intrinsic_load_barycentric_sample:
         case nir_intrinsic_load_barycentric_at_sample:
         case nir_intrinsic_load_barycentric_at_offset:
         case nir_intrinsic_load_barycentric_coord_pixel:
         case nir_intrinsic_load_barycentric_coord_centroid:
         case nir_intrinsic_load_barycentric_coord_sample:
         case nir_intrinsic_load_barycentric_coord_at_offset:
         case nir_intrinsic_load_barycentric_coord_at_sample:
         case nir_intrinsic_load_input_vertex:
         case nir_intrinsic_load_per_vertex_input:
         case nir_intrinsic_load_output:
            return false;
         case nir_intrinsic_load_interpolated_input:
            if (!nir_src_is_const(intr->src[1]))
               return false;
            break;
         case nir_intrinsic_store_output:
            /* Outputs are copied once at the end; the broadcast needs every
             * quad lane. */
            if (block != last || !nir_src_is_const(intr->src[1]))
               return false;
            break;
         default:
            break;
         }
      }
   }
   return true;
}

static bool
fsr_lower_discards(nir_builder *b, struct fsr_ctx *ctx, nir_intrinsic_instr *intr)
{
   nir_def *cond;
   switch (intr->intrinsic) {
   case nir_intrinsic_demote:
   case nir_intrinsic_terminate:
      cond = nir_imm_true(b);
      break;
   case nir_intrinsic_demote_if:
   case nir_intrinsic_terminate_if:
      cond = intr->src[0].ssa;
      break;
   default:
      return false;
   }
   b->cursor = nir_before_instr(&intr->instr);
   nir_store_var(b, ctx->demoted, nir_ior(b, nir_load_var(b, ctx->demoted), cond), 1);
   nir_instr_remove(&intr->instr);
   return true;
}

static bool
fsr_tex_derivatives(nir_builder *b, struct fsr_ctx *ctx, nir_tex_instr *tex)
{
   if ((tex->op != nir_texop_tex && tex->op != nir_texop_txb) ||
       tex->sampler_dim == GLSL_SAMPLER_DIM_BUF || tex->sampler_dim == GLSL_SAMPLER_DIM_MS)
      return false;
   int coord_index = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   if (coord_index < 0)
      return false;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *coord = tex->src[coord_index].src.ssa;
   unsigned components = tex->coord_components - (tex->is_array ? 1 : 0);
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_1D && components < 1)
      components = 1;
   nir_def *uv = nir_trim_vector(b, coord, components);
   nir_def *ddx, *ddy;
   fsr_derivatives(b, ctx, uv, &ddx, &ddy);
   int bias_index = nir_tex_instr_src_index(tex, nir_tex_src_bias);
   if (bias_index >= 0) {
      nir_def *scale = nir_fexp2(b, nir_f2f32(b, tex->src[bias_index].src.ssa));
      if (ddx->bit_size != 32)
         scale = nir_f2fN(b, scale, ddx->bit_size);
      ddx = nir_fmul(b, ddx, scale);
      ddy = nir_fmul(b, ddy, scale);
      nir_tex_instr_remove_src(tex, bias_index);
   }
   tex->op = nir_texop_txd;
   nir_tex_instr_add_src(tex, nir_tex_src_ddx, ddx);
   nir_tex_instr_add_src(tex, nir_tex_src_ddy, ddy);
   return true;
}

static bool
fsr_rewrite(nir_builder *b, struct fsr_ctx *ctx, nir_instr *instr)
{
   if (instr->type == nir_instr_type_tex)
      return fsr_tex_derivatives(b, ctx, nir_instr_as_tex(instr));
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   b->cursor = nir_before_instr(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_fine:
   case nir_intrinsic_ddy_coarse: {
      nir_def *ddx, *ddy;
      fsr_derivatives(b, ctx, intr->src[0].ssa, &ddx, &ddy);
      bool x = intr->intrinsic == nir_intrinsic_ddx ||
               intr->intrinsic == nir_intrinsic_ddx_fine ||
               intr->intrinsic == nir_intrinsic_ddx_coarse;
      nir_def_replace(&intr->def, x ? ddx : ddy);
      return true;
   }
   case nir_intrinsic_load_frag_shading_rate:
      nir_def_replace(&intr->def, ctx->rate);
      return true;
   case nir_intrinsic_load_helper_invocation:
   case nir_intrinsic_is_helper_invocation:
      nir_def_replace(&intr->def, nir_ior(b, nir_inot(b, ctx->real),
                                          nir_load_var(b, ctx->demoted)));
      return true;
   case nir_intrinsic_store_output: {
      /* Sample invocations exchange all outputs together at the end. */
      if (ctx->sample_group)
         return false;
      nir_def *value = fsr_broadcast(b, ctx, intr->src[0].ssa);
      nir_src_rewrite(&intr->src[0], value);
      return true;
   }
   default:
      return false;
   }
}

/* Move every input load to the setup block and evaluate it at the lane
 * position. */
static void
fsr_lower_inputs(nir_builder *b, struct fsr_ctx *ctx, nir_function_impl *impl)
{
   nir_foreach_block_safe(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (instr->index != UINT32_MAX)
            continue;
         if (intr->intrinsic != nir_intrinsic_load_interpolated_input &&
             intr->intrinsic != nir_intrinsic_load_point_coord)
            continue;
         nir_cursor post = nir_after_instr(ctx->top_end);
         /* With the slot handshake, loads and neighbor reads go before it. */
         const bool pre = ctx->covered_store != NULL;
         b->cursor = pre ? nir_before_instr(ctx->covered_store) : post;
         nir_def *value;
         if (intr->intrinsic == nir_intrinsic_load_point_coord) {
            value = pre ? fsr_extrapolate_at(b, ctx, nir_load_point_coord(b), post)
                        : fsr_extrapolate(b, ctx, nir_load_point_coord(b));
         } else {
            nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
            nir_intrinsic_instr *new_bary =
               nir_instr_as_intrinsic(nir_instr_clone(b->shader, &bary->instr));
            nir_builder_instr_insert(b, &new_bary->instr);
            /* The offset is built before the load that reads it. */
            nir_def *offset = nir_imm_int(b, nir_src_as_uint(intr->src[1]));
            nir_intrinsic_instr *load =
               nir_instr_as_intrinsic(nir_instr_clone(b->shader, &intr->instr));
            nir_builder_instr_insert(b, &load->instr);
            nir_src_rewrite(&load->src[0], &new_bary->def);
            nir_src_rewrite(&load->src[1], offset);
            nir_def *v = &load->def;
            if (v->bit_size != 32)
               v = nir_f2f32(b, v);
            if (nir_intrinsic_interp_mode(bary) == INTERP_MODE_NOPERSPECTIVE) {
               value = pre ? fsr_extrapolate_at(b, ctx, v, post) : fsr_extrapolate(b, ctx, v);
            } else {
               nir_def *w = nir_channel(b, nir_load_frag_coord(b), 3);
               nir_def *vw = nir_fmul(b, v, w);
               value = nir_fdiv(b, pre ? fsr_extrapolate_at(b, ctx, vw, post)
                                       : fsr_extrapolate(b, ctx, vw), ctx->fw);
            }
            if (intr->def.bit_size != 32)
               value = nir_f2fN(b, value, intr->def.bit_size);
         }
         ctx->top_end = nir_def_instr(value);
         nir_def_replace(&intr->def, value);
      }
   }
   /* FragCoord loads outside the setup become the lane position. */
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_frag_coord ||
             instr->index != UINT32_MAX)
            continue;
         nir_def_replace(&intr->def, ctx->coord);
      }
   }
}

/* Metal runs a multisampled fragment shader that writes memory once per
 * covered sample. Each fragment then has a slot in a device buffer. The first
 * sample invocation whose real lane claims the slot runs the fragment's side
 * effects and publishes its outputs and discard state; the real lanes of the
 * other sample invocations wait for them.
 *
 * The tag identifies the draw, the rasterized primitive and the fragment, and
 * its low byte is the render pass epoch (the low byte of the draw serial).
 * Even and odd epochs use separate halves of the table, and a slot is free
 * when it is zero or holds the tag of a pass at least FSR_SLOT_AGE epochs
 * older; the slot index is a hash of the tag, probed linearly within the
 * half. Metal runs the fragments of all draws of a pass tile by tile, so a
 * fragment of the last draw can run before one of the first draw in another
 * tile: a claim never replaces a slot of the current pass, and the previous
 * pass, which can still be running, uses the other half. A waiting
 * invocation therefore always finds its own publication. A fragment that
 * finds no free slot within the probe limit runs on its own, as without the
 * slots, and an invocation whose publication does not arrive within the wait
 * limit keeps its own results. */
#define FSR_SLOT_READY 4
#define FSR_SLOT_DISCARD 8
#define FSR_SLOT_PRESENCE 12
#define FSR_SLOT_SAMPLES 8
#define FSR_SLOT_DATA (FSR_SLOT_PRESENCE + 4 * FSR_SLOT_SAMPLES)
#define FSR_PRESENCE_TRIES 64
#define FSR_SLOT_PROBES 64
#define FSR_SLOT_AGE 2
#define FSR_SLOT_TRIES (1u << 12)

static unsigned
fsr_output_words(nir_function_impl *impl)
{
   unsigned words = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic ||
             !fsr_is_output(nir_instr_as_intrinsic(instr)))
            continue;
         nir_def *v = nir_instr_as_intrinsic(instr)->src[0].ssa;
         words += v->num_components * DIV_ROUND_UP(v->bit_size, 32);
      }
   }
   return words;
}

static nir_def *
fsr_slot_read(nir_builder *b, nir_def *addr)
{
   return nir_global_atomic(b, 32, addr, nir_imm_int(b, 0), .atomic_op = nir_atomic_op_iadd);
}

static void
fsr_slot_write(nir_builder *b, nir_def *addr, nir_def *value)
{
   nir_global_atomic(b, 32, addr, value, .atomic_op = nir_atomic_op_xchg);
}

static void
fsr_slot_fence(nir_builder *b)
{
   nir_barrier(b, .execution_scope = SCOPE_NONE, .memory_scope = SCOPE_DEVICE,
               .memory_semantics = NIR_MEMORY_ACQ_REL,
               .memory_modes = nir_var_mem_global);
}

static void
fsr_claim(nir_builder *b, struct fsr_ctx *ctx, unsigned stride)
{
   /* Before the real lane is chosen: every executing lane of the fragment
    * finds the slot, then announces itself in the slot's presence word for
    * its sample. */
   b->cursor = nir_after_instr(ctx->covered_store);
   nir_def *serial = fsr_load_root(b, 1, 32, offsetof(struct kk_root_descriptor_table, fsr.serial));
   nir_def *slots = fsr_load_root(b, 1, 64, offsetof(struct kk_root_descriptor_table, fsr.slots));
   nir_def *prim = nir_load_raster_primitive_id_kk(b);
   nir_def *ox = nir_bcsel(b, ctx->wide, nir_iand_imm(b, ctx->px, ~1u), ctx->px);
   nir_def *oy = nir_bcsel(b, ctx->tall, nir_iand_imm(b, ctx->py, ~1u), ctx->py);
   nir_def *origin = nir_ior(b, ox, nir_ishl_imm(b, oy, 16));
   nir_def *h = nir_ixor(b, nir_imul_imm(b, serial, 0x9e3779b1u),
      nir_ixor(b, nir_imul_imm(b, nir_iadd_imm(b, prim, 1), 0x85ebca77u),
                  nir_imul_imm(b, origin, 0xc2b2ae3du)));
   h = nir_imul_imm(b, nir_ixor(b, h, nir_ushr_imm(b, h, 15)), 0x2c1b3c6du);
   h = nir_ixor(b, h, nir_ushr_imm(b, h, 12));
   nir_def *serial8 = nir_iand_imm(b, serial, 0xff);
   ctx->tag = nir_ior(b, nir_ior_imm(b, nir_iand_imm(b, h, 0xfffffe00u), 0x100), serial8);
   /* Even and odd epochs use separate halves of the table. */
   const unsigned bits = util_logbase2(KK_FSR_SLOT_BYTES / stride) - 1;
   assert(bits > 0 && bits < 31);
   nir_def *base = nir_ushr_imm(b, nir_imul_imm(b, nir_ixor(b, h, nir_ushr_imm(b, h, 16)), 0x7feb352du),
                                32 - bits);
   nir_def *half = nir_ishl_imm(b, nir_iand_imm(b, serial, 1), bits);
   nir_def *active = nir_ine_imm(b, slots, 0);
   nir_def *sid = nir_iand_imm(b, nir_load_sample_id(b), FSR_SLOT_SAMPLES - 1);
   BITSET_SET(b->shader->info.system_values_read, SYSTEM_VALUE_SAMPLE_ID);
   nir_def *key = nir_ishl_imm(b, ctx->tag, 5);
   nir_def *own = ctx->bits[0];

   nir_variable *won = nir_local_variable_create(b->impl, glsl_bool_type(), "kk_fsr_won");
   nir_variable *slot = nir_local_variable_create(b->impl, glsl_uint64_t_type(), "kk_fsr_slot");
   nir_variable *probe = nir_local_variable_create(b->impl, glsl_uint_type(), "kk_fsr_probe");
   nir_variable *tries = nir_local_variable_create(b->impl, glsl_uint_type(), "kk_fsr_tries");
   nir_variable *present = nir_local_variable_create(b->impl, glsl_uint_type(), "kk_fsr_present");
   nir_variable *word = nir_local_variable_create(b->impl, glsl_uint_type(), "kk_fsr_word");
   nir_store_var(b, won, nir_imm_true(b), 1);
   nir_store_var(b, slot, nir_imm_int64(b, 0), 1);
   nir_store_var(b, probe, nir_imm_int(b, 0), 1);
   nir_store_var(b, tries, nir_imm_int(b, 0), 1);
   nir_store_var(b, present, nir_imm_int(b, 0), 1);
   nir_push_if(b, nir_iand(b, nir_ine_imm(b, ctx->alive, 0), active));
   {
      nir_push_loop(b);
      {
         nir_def *i = nir_load_var(b, probe);
         nir_def *index = nir_ior(b, nir_iand_imm(b, nir_iadd(b, base, i), BITFIELD_MASK(bits)),
                                  half);
         nir_def *addr = nir_iadd(b, slots, nir_u2u64(b, nir_imul_imm(b, index, stride)));
         nir_def *old = fsr_slot_read(b, addr);
         nir_def *n = nir_iadd_imm(b, nir_load_var(b, tries), 1);
         nir_store_var(b, tries, n, 1);
         /* Another lane or sample invocation of this fragment holds the slot. */
         nir_push_if(b, nir_ieq(b, old, ctx->tag));
         {
            nir_store_var(b, won, nir_imm_false(b), 1);
            nir_store_var(b, slot, addr, 1);
            nir_jump(b, nir_jump_break);
         }
         nir_pop_if(b, NULL);
         nir_def *age = nir_iand_imm(b, nir_isub(b, serial8, old), 0xff);
         nir_def *free_slot = nir_ior(b, nir_ieq_imm(b, old, 0),
                                      nir_uge_imm(b, age, FSR_SLOT_AGE));
         nir_push_if(b, free_slot);
         {
            nir_def *seen = nir_global_atomic_swap(b, 32, addr, old, ctx->tag,
                                                   .atomic_op = nir_atomic_op_cmpxchg);
            nir_push_if(b, nir_ieq(b, seen, old));
            {
               nir_store_var(b, slot, addr, 1);
               nir_jump(b, nir_jump_break);
            }
            /* Another lane of this fragment claimed it first. */
            nir_push_if(b, nir_ieq(b, seen, ctx->tag));
            {
               nir_store_var(b, won, nir_imm_false(b), 1);
               nir_store_var(b, slot, addr, 1);
               nir_jump(b, nir_jump_break);
            }
            nir_pop_if(b, NULL);
            nir_pop_if(b, NULL);
            /* Lost the race to another fragment: examine the same slot again. */
         }
         nir_push_else(b, NULL);
         {
            nir_store_var(b, probe, nir_iadd_imm(b, i, 1), 1);
         }
         nir_pop_if(b, NULL);
         nir_break_if(b, nir_ior(b, nir_uge_imm(b, nir_load_var(b, probe), FSR_SLOT_PROBES),
                                 nir_uge_imm(b, n, FSR_SLOT_TRIES)));
      }
      nir_pop_loop(b, NULL);

      /* Presence: the word holds the tag shifted by 5, the claim bit (bit 4,
       * set by the lane that claimed the slot) and one bit per quad
       * position. A word of another tag is stale and is replaced. Only
       * lanes that run this sample execute these atomics, and each one reads
       * the word after every lane of its quad has added its bits; nothing
       * waits for another simdgroup. Quad shuffles of values computed by
       * the claim are not exact under per-sample execution, so the claim
       * travels through the word too. */
      nir_def *s = nir_load_var(b, slot);
      nir_push_if(b, nir_ine_imm(b, s, 0));
      {
         nir_def *paddr = nir_iadd(b, nir_iadd_imm(b, s, FSR_SLOT_PRESENCE),
                                   nir_u2u64(b, nir_ishl_imm(b, sid, 2)));
         nir_store_var(b, word, nir_imm_int(b, 0), 1);
         nir_store_var(b, tries, nir_imm_int(b, 0), 1);
         nir_push_loop(b);
         {
            nir_def *w = nir_load_var(b, word);
            nir_def *mine = nir_ieq(b, nir_iand_imm(b, w, ~0x1fu), key);
            nir_def *claimed = nir_bcsel(b, nir_load_var(b, won), nir_imm_int(b, 0x10),
                                         nir_imm_int(b, 0));
            nir_def *next = nir_ior(b, nir_bcsel(b, mine, w, key), nir_ior(b, own, claimed));
            nir_def *seen = nir_global_atomic_swap(b, 32, paddr, w, next,
                                                   .atomic_op = nir_atomic_op_cmpxchg);
            nir_store_var(b, word, seen, 1);
            nir_def *t = nir_iadd_imm(b, nir_load_var(b, tries), 1);
            nir_store_var(b, tries, t, 1);
            nir_break_if(b, nir_ior(b, nir_ieq(b, seen, w),
                                    nir_uge_imm(b, t, FSR_PRESENCE_TRIES)));
         }
         nir_pop_loop(b, NULL);
         nir_def *r = fsr_slot_read(b, paddr);
         nir_store_var(b, present, nir_bcsel(b, nir_ieq(b, nir_iand_imm(b, r, ~0x1fu), key),
                                             nir_iand_imm(b, r, 0x1f), nir_imm_int(b, 0)), 1);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);
   /* A lane that has its own bit knows the executing lanes of its quad;
    * otherwise the helper flags decide, as without the handshake. */
   nir_def *pres = nir_load_var(b, present);
   nir_def *covered = nir_load_var(b, ctx->covered_var);
   /* Lanes that do not run the sample read garbage, so their view of the
    * fragment can differ from the executing lanes' view. Lane positions,
    * and therefore the derivative system, must agree across the quad: a
    * lane takes a view only when it is marked, contains its reporter, lies
    * within the helper coverage and every lane it names reports the same
    * view. The executing lanes' identical views always pass. A lane whose
    * own view fails adopts the largest passing view of a neighbor (the
    * lowest position on ties), otherwise the helper coverage. */
   nir_def *rep = nir_bcsel(b, nir_ine_imm(b, nir_iand(b, pres, own), 0),
                            nir_ior_imm(b, nir_iand_imm(b, pres, 0xf), 0x10), nir_imm_int(b, 0));
   nir_def *r[4] = {rep, nir_quad_swap_horizontal(b, rep), nir_quad_swap_vertical(b, rep),
                    nir_quad_swap_diagonal(b, rep)};
   nir_def *ok[4], *score[4];
   for (unsigned j = 0; j < 4; ++j) {
      /* r[j] comes from quad position p ^ j; ctx->bits[j] is its bit. */
      nir_def *valid = nir_iand(b, nir_ine_imm(b, nir_iand_imm(b, r[j], 0x10), 0),
         nir_iand(b, nir_ine_imm(b, nir_iand(b, r[j], ctx->bits[j]), 0),
                     nir_ieq_imm(b, nir_iand(b, nir_iand_imm(b, r[j], 0xf), nir_inot(b, covered)), 0)));
      nir_def *agree = nir_imm_true(b);
      for (unsigned k = 0; k < 4; ++k) {
         /* Position p ^ k reports r[k]. */
         nir_def *named = nir_ine_imm(b, nir_iand(b, r[j], ctx->bits[k]), 0);
         agree = nir_iand(b, agree, nir_ior(b, nir_inot(b, named), nir_ieq(b, r[k], r[j])));
      }
      ok[j] = nir_iand(b, valid, agree);
      nir_def *pos = nir_ixor(b, ctx->p, nir_imm_int(b, j));
      score[j] = nir_bcsel(b, ok[j],
         nir_iadd_imm(b, nir_isub(b, nir_imul_imm(b, nir_bit_count(b, nir_iand_imm(b, r[j], 0xf)), 4), pos), 4),
         nir_imm_int(b, 0));
   }
   nir_def *best = covered, *best_score = nir_imm_int(b, 0);
   for (unsigned j = 1; j < 4; ++j) {
      nir_def *better = nir_ult(b, best_score, score[j]);
      best = nir_bcsel(b, better, nir_iand_imm(b, r[j], 0xf), best);
      best_score = nir_umax(b, best_score, score[j]);
   }
   nir_def *known = ok[0];
   nir_store_var(b, ctx->covered_var,
                 nir_bcsel(b, known, nir_iand_imm(b, pres, 0xf), best), 1);

   /* After the setup: the sample invocation that holds the claiming lane
    * runs the side effects through its real lane. A lane without its own
    * presence bit (no slot) falls back to the quad's claim results. */
   b->cursor = nir_after_instr(ctx->top_end);
   nir_def *wonbit = nir_b2i32(b, nir_load_var(b, won));
   nir_def *wonmask = nir_ior(b, nir_imul(b, wonbit, ctx->bits[0]),
      nir_ior(b, nir_imul(b, nir_quad_swap_horizontal(b, wonbit), ctx->bits[1]),
         nir_ior(b, nir_imul(b, nir_quad_swap_vertical(b, wonbit), ctx->bits[2]),
                    nir_imul(b, nir_quad_swap_diagonal(b, wonbit), ctx->bits[3]))));
   nir_def *candidates = nir_iand(b, nir_load_var(b, ctx->covered_var), ctx->group);
   nir_def *group_won = nir_bcsel(b, known, nir_ine_imm(b, nir_iand_imm(b, pres, 0x10), 0),
                                  nir_ine_imm(b, nir_iand(b, wonmask, candidates), 0));
   ctx->won = group_won;
   ctx->slot = nir_load_var(b, slot);
   ctx->lead = ctx->real;
   ctx->real = nir_iand(b, ctx->real, group_won);
   ctx->top_end = nir_def_instr(ctx->real);
}

/* At the end of the shader: the winner publishes, the other real lanes take
 * its outputs and discard state, and quads broadcast as usual. */
static void
fsr_exchange(nir_builder *b, struct fsr_ctx *ctx, nir_function_impl *impl)
{
   nir_intrinsic_instr *stores[64];
   unsigned n = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic ||
             !fsr_is_output(nir_instr_as_intrinsic(instr)))
            continue;
         assert(n < ARRAY_SIZE(stores));
         stores[n++] = nir_instr_as_intrinsic(instr);
         nir_instr_remove(instr);
      }
   }
   b->cursor = nir_after_impl(impl);
   nir_def *words[64 * 8];
   unsigned count = 0;
   for (unsigned i = 0; i < n; ++i) {
      nir_def *v = stores[i]->src[0].ssa;
      for (unsigned c = 0; c < v->num_components; ++c) {
         nir_def *s = nir_channel(b, v, c);
         if (s->bit_size == 64) {
            words[count++] = nir_unpack_64_2x32_split_x(b, s);
            words[count++] = nir_unpack_64_2x32_split_y(b, s);
         } else {
            words[count++] = s->bit_size < 32 ? nir_u2u32(b, s) : s;
         }
      }
   }
   nir_def *demoted = nir_b2i32(b, nir_load_var(b, ctx->demoted));
   nir_def *active = nir_ine_imm(b, ctx->slot, 0);

   nir_push_if(b, nir_iand(b, ctx->real, active));
   {
      for (unsigned k = 0; k < count; ++k)
         fsr_slot_write(b, nir_iadd_imm(b, ctx->slot, FSR_SLOT_DATA + 4 * k), words[k]);
      fsr_slot_write(b, nir_iadd_imm(b, ctx->slot, FSR_SLOT_DISCARD), demoted);
      fsr_slot_fence(b);
      fsr_slot_write(b, nir_iadd_imm(b, ctx->slot, FSR_SLOT_READY), ctx->tag);
   }
   nir_pop_if(b, NULL);

   nir_variable *tries = nir_local_variable_create(impl, glsl_uint_type(), "kk_fsr_wait");
   nir_variable *ready = nir_local_variable_create(impl, glsl_uint_type(), "kk_fsr_ready");
   nir_store_var(b, tries, nir_imm_int(b, 0), 1);
   nir_store_var(b, ready, nir_imm_int(b, 0), 1);
   nir_if *wait = nir_push_if(b, nir_iand(b, nir_iand(b, ctx->lead, nir_inot(b, ctx->won)), active));
   nir_def *taken[64 * 8 + 1];
   {
      nir_push_loop(b);
      {
         nir_def *r = fsr_slot_read(b, nir_iadd_imm(b, ctx->slot, FSR_SLOT_READY));
         nir_store_var(b, ready, r, 1);
         nir_def *t = nir_iadd_imm(b, nir_load_var(b, tries), 1);
         nir_store_var(b, tries, t, 1);
         nir_break_if(b, nir_ior(b, nir_ieq(b, r, ctx->tag), nir_uge_imm(b, t, FSR_SLOT_TRIES)));
      }
      nir_pop_loop(b, NULL);
      fsr_slot_fence(b);
      nir_def *got = nir_ieq(b, nir_load_var(b, ready), ctx->tag);
      for (unsigned k = 0; k < count; ++k)
         taken[k] = nir_bcsel(b, got,
            fsr_slot_read(b, nir_iadd_imm(b, ctx->slot, FSR_SLOT_DATA + 4 * k)), words[k]);
      taken[count] = nir_bcsel(b, got,
         fsr_slot_read(b, nir_iadd_imm(b, ctx->slot, FSR_SLOT_DISCARD)), demoted);
   }
   nir_push_else(b, wait);
   nir_pop_if(b, wait);
   for (unsigned k = 0; k < count; ++k)
      words[k] = nir_if_phi(b, taken[k], words[k]);
   demoted = nir_if_phi(b, taken[count], demoted);

   unsigned k = 0;
   for (unsigned i = 0; i < n; ++i) {
      nir_def *v = stores[i]->src[0].ssa;
      nir_def *c[NIR_MAX_VEC_COMPONENTS];
      for (unsigned j = 0; j < v->num_components; ++j) {
         unsigned bits = v->bit_size;
         if (bits == 64) {
            c[j] = nir_pack_64_2x32_split(b, words[k], words[k + 1]);
            k += 2;
         } else {
            c[j] = bits < 32 ? nir_u2uN(b, words[k], bits) : words[k];
            ++k;
         }
      }
      nir_def *value = fsr_broadcast(b, ctx, nir_vec(b, c, v->num_components));
      nir_builder_instr_insert(b, &stores[i]->instr);
      nir_src_rewrite(&stores[i]->src[0], value);
   }
   nir_store_var(b, ctx->demoted, nir_ine_imm(b, demoted, 0), 1);
}

bool
kk_nir_lower_fsr(nir_shader *nir, const struct kk_fsr_lower_options *opts)
{
   assert(nir->info.stage == MESA_SHADER_FRAGMENT);
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   struct fsr_ctx ctx = {.opts = opts};

   /* Mark every existing instruction; setup instructions stay unmarked. */
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block)
         instr->index = UINT32_MAX;
   }

   /* Metal runs a multisampled fragment shader that writes memory once per
    * covered sample. */
   if (opts->multisampled) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic &&
                nir_intrinsic_writes_external_memory(nir_instr_as_intrinsic(instr)))
               ctx.sample_group = true;
         }
      }
   }

   unsigned stride = ctx.sample_group
      ? ALIGN_POT(FSR_SLOT_DATA + 4 * fsr_output_words(impl), 16) : 0;
   nir_builder b = nir_builder_at(nir_before_impl(impl));
   ctx.demoted = nir_local_variable_create(impl, glsl_bool_type(), "kk_fsr_demoted");
   nir_store_var(&b, ctx.demoted, nir_imm_false(&b), 1);
   fsr_setup(&b, &ctx);

   /* Discards become a flag so every quad lane reaches the broadcast. */
   nir_foreach_block_safe(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type == nir_instr_type_intrinsic && instr->index == UINT32_MAX)
            fsr_lower_discards(&b, &ctx, nir_instr_as_intrinsic(instr));
      }
   }
   /* Writes of the lanes that are not real become helper writes. */
   nir_lower_helper_writes(nir, true);
   if (ctx.sample_group) {
      b.cursor = nir_after_instr(ctx.top_end);
      fsr_claim(&b, &ctx, stride);
   }

   fsr_lower_inputs(&b, &ctx, impl);
   nir_foreach_block_safe(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         /* New helper queries from the write guard carry index 0. */
         if (instr->index == UINT32_MAX ||
             (instr->type == nir_instr_type_intrinsic &&
              nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_is_helper_invocation))
            fsr_rewrite(&b, &ctx, instr);
      }
   }

   if (ctx.sample_group)
      fsr_exchange(&b, &ctx, impl);
   /* The real lane's discard decides the fragment. */
   b.cursor = nir_after_impl(impl);
   nir_demote_if(&b, fsr_broadcast(&b, &ctx, nir_load_var(&b, ctx.demoted)));

   nir->info.fs.needs_full_quad_helper_invocations = true;
   nir->info.fs.uses_discard = true;
   nir_progress(true, impl, nir_metadata_none);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_opt_dce);
   return true;
}


/* Replaces the attachment placeholder with a read of the shading-rate
 * attachment texel covering the pixel. A 2x2 aligned group never crosses an
 * 8x8 texel, so every lane of a fragment reads the same rate. The raster
 * primitive ID becomes Metal's [[primitive_id]]. */
static bool
fsr_late(nir_builder *b, nir_intrinsic_instr *intr, UNUSED void *data)
{
   if (intr->intrinsic == nir_intrinsic_load_raster_primitive_id_kk) {
      b->cursor = nir_before_instr(&intr->instr);
      nir_def_replace(&intr->def, nir_load_primitive_id(b));
      BITSET_SET(b->shader->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID);
      return true;
   }
   if (intr->intrinsic != nir_intrinsic_load_frag_shading_rate)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *frag = nir_load_frag_coord(b);
   nir_def *px = nir_f2u32(b, nir_ffloor(b, nir_channel(b, frag, 0)));
   nir_def *py = nir_f2u32(b, nir_ffloor(b, nir_channel(b, frag, 1)));
   nir_def *addr = nir_iadd_imm(b, nir_load_buffer_ptr_kk(b, 1, 64, .binding = KK_ARGUMENT_ROOT),
                                offsetof(struct kk_root_descriptor_table, fsr.attachment));
   nir_def *id = nir_build_load_global_constant(b, 1, 64, addr, .align_mul = 8,
                                                .access = ACCESS_CAN_SPECULATE);
   nir_def *rate;
   nir_push_if(b, nir_ine_imm(b, id, 0));
   {
      nir_def *handle = nir_load_texture_handle_kk(
         b, 1, 64, addr, .dest_type = nir_type_uint32,
         .image_dim = GLSL_SAMPLER_DIM_2D, .image_array = false);
      nir_tex_instr *tex = nir_tex_instr_create(b->shader, 3);
      tex->op = nir_texop_txf;
      tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
      tex->dest_type = nir_type_uint32;
      tex->coord_components = 2;
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord,
         nir_vec2(b, nir_ushr_imm(b, px, util_logbase2(KK_FSR_TEXEL_SIZE)),
                     nir_ushr_imm(b, py, util_logbase2(KK_FSR_TEXEL_SIZE))));
      tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(b, 0));
      tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_texture_handle, handle);
      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(b, &tex->instr);
      rate = nir_iand_imm(b, nir_channel(b, &tex->def, 0), 0xf);
   }
   nir_push_else(b, NULL);
   nir_def *none = nir_imm_int(b, 0);
   nir_pop_if(b, NULL);
   nir_def_replace(&intr->def, nir_if_phi(b, rate, none));
   return true;
}

bool
kk_nir_lower_fsr_late(nir_shader *nir)
{
   return nir_shader_intrinsics_pass(nir, fsr_late, nir_metadata_none, NULL);
}

struct fsr_root_range {
   unsigned begin, end;
};

static bool
fsr_reads_root(UNUSED nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct fsr_root_range *range = data;
   if (intr->intrinsic != nir_intrinsic_load_global_constant)
      return false;
   nir_scalar addr = nir_get_scalar(intr->src[0].ssa, 0);
   if (!nir_scalar_is_alu(addr) || nir_scalar_alu_op(addr) != nir_op_iadd)
      return false;
   for (unsigned i = 0; i < 2; ++i) {
      nir_scalar base = nir_scalar_chase_alu_src(addr, i);
      nir_scalar offset = nir_scalar_chase_alu_src(addr, !i);
      if (nir_scalar_is_intrinsic(base) &&
          nir_scalar_intrinsic_op(base) == nir_intrinsic_load_buffer_ptr_kk &&
          nir_intrinsic_binding(nir_def_as_intrinsic(base.def)) == KK_ARGUMENT_ROOT &&
          nir_scalar_is_const(offset) &&
          nir_scalar_as_uint(offset) >= range->begin &&
          nir_scalar_as_uint(offset) < range->end)
         return true;
   }
   return false;
}

/* Whether the draw must provide a serial and the fragment slots. */
bool
kk_nir_fsr_uses_slots(nir_shader *nir)
{
   const unsigned serial = offsetof(struct kk_root_descriptor_table, fsr.serial);
   struct fsr_root_range range = {serial, serial + 4};
   return nir->info.stage == MESA_SHADER_FRAGMENT &&
          nir_shader_intrinsics_pass(nir, fsr_reads_root, nir_metadata_all, &range);
}

/* Whether the root upload must include the rate state after the push
 * constants: dynamic rate and combiners, the attachment or the slots. */
bool
kk_nir_fsr_reads_root(nir_shader *nir)
{
   struct fsr_root_range range = {offsetof(struct kk_root_descriptor_table, fsr),
      offsetof(struct kk_root_descriptor_table, fsr) + sizeof(struct kk_root_fsr)};
   return nir->info.stage == MESA_SHADER_FRAGMENT &&
          nir_shader_intrinsics_pass(nir, fsr_reads_root, nir_metadata_all, &range);
}

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
fsr_extrapolate(nir_builder *b, struct fsr_ctx *ctx, nir_def *g)
{
   nir_def *h = nir_quad_swap_horizontal(b, g);
   nir_def *v = nir_quad_swap_vertical(b, g);
   nir_def *gx = nir_bcsel(b, nir_ieq_imm(b, ctx->qx, 0), nir_fsub(b, h, g),
                           nir_fsub(b, g, h));
   nir_def *gy = nir_bcsel(b, nir_ieq_imm(b, ctx->qy, 0), nir_fsub(b, v, g),
                           nir_fsub(b, g, v));
   return nir_fadd(b, g, nir_fadd(b, nir_fmul(b, gx, ctx->dx),
                                  nir_fmul(b, gy, ctx->dy)));
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

/* Rates as log2 width in bits 2-3 and log2 height in bits 0-1. */
static nir_def *
fsr_combine(nir_builder *b, nir_def *a, nir_def *c, nir_def *op)
{
   nir_def *aw = nir_ubitfield_extract_imm(b, a, 2, 2);
   nir_def *ah = nir_iand_imm(b, a, 3);
   nir_def *cw = nir_ubitfield_extract_imm(b, c, 2, 2);
   nir_def *ch = nir_iand_imm(b, c, 3);
   nir_def *w = nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR), cw,
                nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR), nir_umin(b, aw, cw),
                nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR), nir_umax(b, aw, cw),
                nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR), nir_iadd(b, aw, cw), aw))));
   nir_def *h = nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR), ch,
                nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR), nir_umin(b, ah, ch),
                nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR), nir_umax(b, ah, ch),
                nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR), nir_iadd(b, ah, ch), ah))));
   return nir_ior(b, nir_ishl_imm(b, w, 2), h);
}

static void
fsr_setup(nir_builder *b, struct fsr_ctx *ctx)
{
   const struct kk_fsr_lower_options *opts = ctx->opts;
   nir_def *frag = nir_load_frag_coord(b);
   nir_def *fcx = nir_channel(b, frag, 0), *fcy = nir_channel(b, frag, 1);
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
   }
   rate = fsr_combine(b, rate, primitive, op0);
   /* Placeholder for the attachment rate; kk_nir_lower_fsr_attachment reads
    * the attachment after descriptor and texture lowering. */
   nir_def *attachment = opts->attachment_rate ? nir_load_frag_shading_rate(b)
                                               : nir_imm_int(b, 0);
   rate = fsr_combine(b, rate, attachment, op1);
   /* Every combination of 1 and 2 pixels is supported: clamp each axis. */
   nir_def *lw = nir_umin(b, nir_ubitfield_extract_imm(b, rate, 2, 2), nir_imm_int(b, 1));
   nir_def *lh = nir_umin(b, nir_iand_imm(b, rate, 3), nir_imm_int(b, 1));
   if (opts->dynamic_sample_mask) {
      /* A partial sample mask selects 1x1. */
      nir_def *mask = fsr_load_root(b, 1, 8, offsetof(struct kk_root_descriptor_table,
                                                       draw.sample_mask));
      nir_def *full = nir_ieq_imm(b, nir_u2u32(b, mask), 0xff);
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

   ctx->fw = fsr_extrapolate(b, ctx, nir_channel(b, frag, 3));
   ctx->coord = nir_vec4(b, ctx->vx, ctx->vy,
                         fsr_extrapolate(b, ctx, nir_channel(b, frag, 2)), ctx->fw);
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
         b->cursor = nir_after_instr(ctx->top_end);
         nir_def *value;
         if (intr->intrinsic == nir_intrinsic_load_point_coord) {
            value = fsr_extrapolate(b, ctx, nir_load_point_coord(b));
         } else {
            nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
            nir_intrinsic_instr *new_bary =
               nir_instr_as_intrinsic(nir_instr_clone(b->shader, &bary->instr));
            nir_builder_instr_insert(b, &new_bary->instr);
            nir_intrinsic_instr *load =
               nir_instr_as_intrinsic(nir_instr_clone(b->shader, &intr->instr));
            nir_builder_instr_insert(b, &load->instr);
            nir_src_rewrite(&load->src[0], &new_bary->def);
            nir_src_rewrite(&load->src[1], nir_imm_int(b, nir_src_as_uint(intr->src[1])));
            nir_def *v = &load->def;
            if (v->bit_size != 32)
               v = nir_f2f32(b, v);
            if (nir_intrinsic_interp_mode(bary) == INTERP_MODE_NOPERSPECTIVE) {
               value = fsr_extrapolate(b, ctx, v);
            } else {
               nir_def *w = nir_channel(b, nir_load_frag_coord(b), 3);
               value = nir_fdiv(b, fsr_extrapolate(b, ctx, nir_fmul(b, v, w)), ctx->fw);
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

static bool
fsr_fine_rate(nir_builder *b, nir_intrinsic_instr *intr, UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_frag_shading_rate)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def, nir_imm_int(b, 0));
   return true;
}

bool
kk_nir_lower_fsr_fine(nir_shader *nir)
{
   return nir_shader_intrinsics_pass(nir, fsr_fine_rate, nir_metadata_control_flow, NULL);
}

/* Replaces the attachment placeholder with a read of the shading-rate
 * attachment texel covering the pixel. A 2x2 aligned group never crosses an
 * 8x8 texel, so every lane of a fragment reads the same rate. */
static bool
fsr_attachment_read(nir_builder *b, nir_intrinsic_instr *intr, UNUSED void *data)
{
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
kk_nir_lower_fsr_attachment(nir_shader *nir)
{
   return nir_shader_intrinsics_pass(nir, fsr_attachment_read, nir_metadata_none, NULL);
}

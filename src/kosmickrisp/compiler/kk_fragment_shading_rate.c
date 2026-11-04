/* SPDX-License-Identifier: MIT */
#include "kk_fragment_shading_rate.h"
#include "nir_builder.h"
#include "nir_to_msl.h"
#include <vulkan/vulkan_core.h>
#include "nir_control_flow.h"
#include "util/set.h"
#include "msl_compute_derivatives.h"
#include "msl_virtual_subgroups.h"

struct coarse_values {
   nir_def *mask, *helper, *position, *rate, *primitive, *result;
};

static nir_def *
context_load(nir_builder *b, nir_def *context, unsigned offset, unsigned bits)
{
   return nir_build_load_global_constant(b, 1, bits,
      nir_iadd_imm(b, context, offset), .align_mul = bits / 8);
}
#define CTX(b, ptr, field, bits) context_load(b, ptr, \
   offsetof(struct kk_vrs_context, field), bits)

static bool
lower_application(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct coarse_values *v = data;
   nir_def *replace;
   b->cursor = nir_before_instr(&intr->instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_frag_coord: replace = v->position; break;
   case nir_intrinsic_load_frag_shading_rate: replace = v->rate; break;
   case nir_intrinsic_load_helper_invocation:
   case nir_intrinsic_is_helper_invocation: replace = v->helper; break;
   case nir_intrinsic_increment_pipeline_statistic_kk:
      nir_push_if(b, nir_inot(b, v->helper));
      nir_increment_pipeline_statistic_kk(b, intr->src[0].ssa);
      nir_pop_if(b, NULL);
      nir_instr_remove(&intr->instr);
      return true;
   case nir_intrinsic_ballot:
   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_all:
   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan:
   case nir_intrinsic_exclusive_scan:
   case nir_intrinsic_elect:
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_vote_feq:
   case nir_intrinsic_vote_ieq:
      return msl_lower_virtual_subgroup(b, intr, v->helper);
   case nir_intrinsic_load_sample_mask_in: replace = v->mask; break;
   case nir_intrinsic_load_primitive_id: replace = v->primitive; break;
   case nir_intrinsic_store_output: {
      assert(nir_intrinsic_io_semantics(intr).location == FRAG_RESULT_DATA0);
      unsigned offset = offsetof(struct kk_vrs_result, color) +
                        nir_intrinsic_component(intr) * sizeof(float);
      nir_push_if(b, nir_inot(b, v->helper));
      nir_store_global(b, intr->src[0].ssa, nir_iadd_imm(b, v->result, offset),
                       .write_mask = nir_intrinsic_write_mask(intr),
                       .align_mul = sizeof(float));
      nir_pop_if(b, NULL);
      nir_instr_remove(&intr->instr);
      return true;
   }
   default: return false;
   }
   nir_def_rewrite_uses(&intr->def, replace);
   nir_instr_remove(&intr->instr);
   return true;
}


static nir_def *
clamp_extent(nir_builder *b, nir_def *v)
{
   nir_def *w = nir_umin(b, nir_channel(b, v, 0), nir_imm_int(b, 4));
   nir_def *h = nir_umin(b, nir_channel(b, v, 1), nir_imm_int(b, 4));
   nir_def *cw = nir_bcsel(b, nir_iand(b, nir_ieq_imm(b, w, 4),
      nir_ieq_imm(b, h, 1)), nir_imm_int(b, 2), w);
   nir_def *ch = nir_bcsel(b, nir_iand(b, nir_ieq_imm(b, h, 4),
      nir_ieq_imm(b, w, 1)), nir_imm_int(b, 2), h);
   return nir_vec2(b, cw, ch);
}

static nir_def *
combine_extent(nir_builder *b, nir_def *a, nir_def *c, nir_def *op)
{
   nir_def *v = nir_imul(b, a, c);
   v = nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR), nir_umax(b, a, c), v);
   v = nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR), nir_umin(b, a, c), v);
   v = nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR), c, v);
   return nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR), a, v);
}

static nir_def *
decode_extent(nir_builder *b, nir_def *rate)
{
   return nir_vec2(b,
      nir_ishl(b, nir_imm_int(b, 1), nir_iand_imm(b, nir_ushr_imm(b, rate, 2), 3)),
      nir_ishl(b, nir_imm_int(b, 1), nir_iand_imm(b, rate, 3)));
}

static nir_def *
load_rate_image(nir_builder *b, nir_def *context, nir_def *x, nir_def *y)
{
   nir_def *zero = nir_imm_int(b, 0);
   nir_def *tx = nir_udiv(b, x, CTX(b, context, texel_width, 32));
   nir_def *ty = nir_udiv(b, y, CTX(b, context, texel_height, 32));
   nir_def *valid = nir_iand(b, nir_ine_imm(b, CTX(b, context, has_attachment, 32), 0),
      nir_iand(b, nir_ult(b, tx, CTX(b, context, rate_image_width, 32)),
                  nir_ult(b, ty, CTX(b, context, rate_image_height, 32))));
   nir_push_if(b, valid);
   nir_def *handle = nir_load_texture_handle_kk(b, 1, 64,
      nir_iadd_imm(b, context, offsetof(struct kk_vrs_context, rate_texture)),
      .dest_type = nir_type_uint32, .image_dim = GLSL_SAMPLER_DIM_2D,
      .image_array = false, .flags = MSL_ACCESS_READ);
   nir_tex_instr *tex = nir_tex_instr_create(b->shader, 3);
   tex->op = nir_texop_txf;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->dest_type = nir_type_uint32;
   tex->coord_components = 2;
   tex->src[0].src_type = nir_tex_src_texture_handle;
   tex->src[0].src = nir_src_for_ssa(handle);
   tex->src[1].src_type = nir_tex_src_coord;
   tex->src[1].src = nir_src_for_ssa(nir_vec2(b, tx, ty));
   tex->src[2].src_type = nir_tex_src_lod;
   tex->src[2].src = nir_src_for_ssa(zero);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(b, &tex->instr);
   nir_def *rate = nir_channel(b, &tex->def, 0);
   nir_pop_if(b, NULL);
   return nir_if_phi(b, rate, zero);
}

bool
kk_nir_lower_coarse_fragment(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT ||
       (nir->info.outputs_written & ~BITFIELD64_BIT(FRAG_RESULT_DATA0)))
      return false;
   /* Do not pretend that this first internal form implements these modes. */
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_demote: case nir_intrinsic_demote_if:
            case nir_intrinsic_terminate: case nir_intrinsic_terminate_if:
            case nir_intrinsic_load_input: case nir_intrinsic_load_interpolated_input:
            case nir_intrinsic_load_per_draw_ptr_kk:
               return false;
            default: break;
            }
         }
      }
   }
   /* Guard actual application stores and atomics before changing its stage.
    * Queries introduced here are resolved to logical helper state below. */
   NIR_PASS(_, nir, nir_lower_helper_writes, true);
   NIR_PASS(_, nir, nir_lower_undef_to_zero, NULL);
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_cf_list application;
   nir_cf_list_extract(&application, &impl->body);
   nir->info.stage = MESA_SHADER_COMPUTE;
   memset(&nir->info.cs, 0, sizeof(nir->info.cs));
   nir->info.workgroup_size[0] = 16;
   nir->info.workgroup_size[1] = 16;
   nir->info.workgroup_size[2] = 1;
   nir->info.derivative_group = DERIVATIVE_GROUP_LINEAR;
   nir_builder b = nir_builder_create(impl);
   b.cursor = nir_before_cf_list(&impl->body);
   nir_def *context = nir_load_per_draw_ptr_kk(&b, 1, 64);
   nir_def *coverage = CTX(&b, context, coverage, 64);
   nir_def *sample_mask = CTX(&b, context, sample_mask, 32);
   nir_def *results = CTX(&b, context, results, 64);
   nir_def *diagnostics = CTX(&b, context, diagnostics, 64);
   nir_def *width = CTX(&b, context, width, 32);
   nir_def *height = CTX(&b, context, height, 32);
   nir_def *index = nir_load_local_invocation_index(&b);
   nir_def *group = nir_load_workgroup_id(&b);
   nir_def *region = nir_ushr_imm(&b, index, 6);
   nir_def *rx = nir_iadd(&b, nir_imul_imm(&b, nir_channel(&b, group, 0), 16),
      nir_imul_imm(&b, nir_iand_imm(&b, region, 1), 8));
   nir_def *ry = nir_iadd(&b, nir_imul_imm(&b, nir_channel(&b, group, 1), 16),
      nir_imul_imm(&b, nir_ushr_imm(&b, region, 1), 8));
   nir_def *pipeline = clamp_extent(&b, nir_vec2(&b,
      CTX(&b, context, fragment_width, 32), CTX(&b, context, fragment_height, 32)));
   nir_def *primitive = clamp_extent(&b, decode_extent(&b, CTX(&b, context, primitive_rate, 32)));
   nir_def *attachment = clamp_extent(&b, decode_extent(&b, load_rate_image(&b, context,
      nir_iadd(&b, rx, CTX(&b, context, origin_x, 32)),
      nir_iadd(&b, ry, CTX(&b, context, origin_y, 32)))));
   nir_def *extent = clamp_extent(&b, combine_extent(&b,
      clamp_extent(&b, combine_extent(&b, pipeline, primitive, CTX(&b, context, combiner0, 32))),
      attachment, CTX(&b, context, combiner1, 32)));
   nir_def *fw = nir_channel(&b, extent, 0), *fh = nir_channel(&b, extent, 1);
   nir_def *local = nir_iand_imm(&b, index, 63);
   nir_def *quad = nir_ushr_imm(&b, local, 2), *lane = nir_iand_imm(&b, local, 3);
   nir_def *columns = nir_udiv(&b, nir_imm_int(&b, 8), fw);
   nir_def *rows = nir_udiv(&b, nir_imm_int(&b, 8), fh);
   nir_def *quad_columns = nir_ushr_imm(&b, columns, 1);
   nir_def *cx = nir_iadd(&b, nir_imul_imm(&b, nir_umod(&b, quad, quad_columns), 2), nir_iand_imm(&b, lane, 1));
   nir_def *cy = nir_iadd(&b, nir_imul_imm(&b, nir_udiv(&b, quad, quad_columns), 2), nir_ushr_imm(&b, lane, 1));
   nir_def *x = nir_iadd(&b, rx, nir_imul(&b, cx, fw));
   nir_def *y = nir_iadd(&b, ry, nir_imul(&b, cy, fh));
   nir_def *active = nir_ult(&b, local, nir_imul(&b, columns, rows));
   nir_def *area = nir_imul(&b, fw, fh);
   nir_def *mask = nir_imm_int(&b, 0), *z = nir_imm_float(&b, 0), *rw = nir_imm_float(&b, 0);
   for (unsigned p = 0; p < 16; p++) {
      nir_def *px = nir_iadd(&b, x, nir_umod(&b, nir_imm_int(&b, p), fw));
      nir_def *py = nir_iadd(&b, y, nir_udiv(&b, nir_imm_int(&b, p), fw));
      nir_def *valid = nir_iand(&b, nir_iand(&b, active, nir_ult(&b, nir_imm_int(&b, p), area)),
         nir_iand(&b, nir_ult(&b, px, width), nir_ult(&b, py, height)));
      nir_def *zero_mask = nir_imm_int(&b, 0);
      nir_def *zero_zw = nir_imm_zero(&b, 2, 32);
      nir_push_if(&b, valid);
      nir_def *pixel = nir_iadd(&b, nir_imul(&b, py, width), px);
      nir_def *addr = nir_iadd(&b, coverage,
         nir_imul_imm(&b, nir_u2u64(&b, pixel), sizeof(struct kk_vrs_coverage)));
      nir_def *fine = nir_load_global(&b, 1, 32, addr, .align_mul = 4);
      nir_def *zw = nir_load_global(&b, 2, 32, nir_iadd_imm(&b, addr,
         offsetof(struct kk_vrs_coverage, z)), .align_mul = 4);
      nir_pop_if(&b, NULL);
      fine = nir_if_phi(&b, fine, zero_mask);
      fine = nir_iand(&b, fine, sample_mask);
      zw = nir_if_phi(&b, zw, zero_zw);
      nir_def *first = nir_iand(&b, nir_ieq_imm(&b, mask, 0), nir_ine_imm(&b, fine, 0));
      z = nir_bcsel(&b, first, nir_channel(&b, zw, 0), z);
      rw = nir_bcsel(&b, first, nir_channel(&b, zw, 1), rw);
      mask = nir_ior(&b, mask, nir_ishl(&b, nir_iand_imm(&b, fine, 1), nir_iadd_imm(&b, area, -1 - p)));
   }
   nir_def *quad_mask = nir_imm_int(&b, 0);
   for (unsigned l = 0; l < 4; l++)
      quad_mask = nir_ior(&b, quad_mask, nir_quad_broadcast(&b, mask, nir_imm_int(&b, l)));
   struct coarse_values v = {
      .mask = mask, .helper = nir_ieq_imm(&b, mask, 0),
      .position = nir_vec4(&b,
         nir_fadd(&b, nir_u2f32(&b, nir_iadd(&b, x, CTX(&b, context, origin_x, 32))), nir_fmul_imm(&b, nir_u2f32(&b, fw), 0.5)),
         nir_fadd(&b, nir_u2f32(&b, nir_iadd(&b, y, CTX(&b, context, origin_y, 32))), nir_fmul_imm(&b, nir_u2f32(&b, fh), 0.5)), z, rw),
      .rate = nir_ior(&b, nir_ishl_imm(&b, nir_ufind_msb(&b, fw), 2), nir_ufind_msb(&b, fh)),
      .primitive = CTX(&b, context, api_primitive, 32),
   };
   nir_def *cell = nir_iadd(&b, nir_imul(&b, y, width), x);
   v.result = nir_iadd(&b, results, nir_imul_imm(&b, nir_u2u64(&b, cell), sizeof(struct kk_vrs_result)));
   nir_push_if(&b, nir_ine_imm(&b, quad_mask, 0));
   nir_push_if(&b, nir_ine_imm(&b, diagnostics, 0));
   nir_def *diagnostic_offset = nir_bcsel(&b, v.helper, nir_imm_int64(&b, 4), nir_imm_int64(&b, 0));
   nir_global_atomic(&b, 32, nir_iadd(&b, diagnostics, diagnostic_offset), nir_imm_int(&b, 1),
                     .atomic_op = nir_atomic_op_iadd);
   nir_pop_if(&b, NULL);
   nir_push_if(&b, nir_inot(&b, v.helper));
   nir_store_global(&b, v.mask, nir_iadd_imm(&b, v.result, offsetof(struct kk_vrs_result, mask)), .write_mask = 1, .align_mul = 4);
   nir_store_global(&b, v.primitive, nir_iadd_imm(&b, v.result, offsetof(struct kk_vrs_result, api_primitive)), .write_mask = 1, .align_mul = 4);
   nir_store_global(&b, v.rate, nir_iadd_imm(&b, v.result, offsetof(struct kk_vrs_result, rate)), .write_mask = 1, .align_mul = 4);
   nir_def *map = CTX(&b, context, pixel_results, 64);
   for (unsigned p = 0; p < 16; p++) {
      nir_def *bit = nir_ishl(&b, nir_imm_int(&b, 1), nir_iadd_imm(&b, area, -1 - p));
      nir_push_if(&b, nir_iand(&b, nir_ult(&b, nir_imm_int(&b, p), area),
         nir_ine_imm(&b, nir_iand(&b, mask, bit), 0)));
      nir_def *px = nir_iadd(&b, x, nir_umod(&b, nir_imm_int(&b, p), fw));
      nir_def *py = nir_iadd(&b, y, nir_udiv(&b, nir_imm_int(&b, p), fw));
      nir_def *pixel = nir_iadd(&b, nir_imul(&b, py, width), px);
      nir_store_global(&b, cell, nir_iadd(&b, map, nir_imul_imm(&b, nir_u2u64(&b, pixel), 4)), .write_mask = 1, .align_mul = 4);
      nir_pop_if(&b, NULL);
   }

   nir_pop_if(&b, NULL);
   b.cursor = nir_cf_reinsert(&application, b.cursor);
   nir_pop_if(&b, NULL);
   nir_shader_intrinsics_pass(nir, lower_application, nir_metadata_none, &v);
   kk_lower_compute_derivatives(nir);
   nir_progress(true, impl, nir_metadata_none);
   nir->info.inputs_read = 0;
   nir->info.outputs_written = 0;
   nir_shader_gather_info(nir, impl);
   nir_validate_shader(nir, "after bounded coarse-fragment lowering");
   return true;
}


/* Native-fragment execution for required rates. Explicit texture inputs are
 * retained. Native kill/demote keep their control-flow semantics while replay
 * publication and dynamic helper guards preserve their logical effects. */
struct native_coarse_values {
   struct set *application;
   nir_def *position, *centroid_xy, *native_position, *original_per_draw;
   nir_def *helper, *mask, *rate, *logical, *primitive;
};

static nir_def *
quad_select(nir_builder *b, nir_def *value, nir_def *lane)
{
   nir_def *r = nir_quad_broadcast(b, value, nir_imm_int(b, 3));
   for (int i = 2; i >= 0; i--)
      r = nir_bcsel(b, nir_ieq_imm(b, lane, i),
         nir_quad_broadcast(b, value, nir_imm_int(b, i)), r);
   return r;
}

static nir_def *
logical_value(nir_builder *b, struct native_coarse_values *v,
              nir_def *value, nir_def *wanted)
{
   nir_def *physical = nir_imm_int(b, 0);
   for (unsigned p = 0; p < 4; p++)
      physical = nir_bcsel(b, nir_ieq(b,
         nir_quad_broadcast(b, v->logical, nir_imm_int(b, p)), wanted),
         nir_imm_int(b, p), physical);
   return quad_select(b, value, physical);
}

/* Keep subgroup indices and explicit quad operations consistent with the
 * logical derivative quad. The permutation is local to each four-lane group;
 * no invocations are packed across different native subgroups. */
static nir_def *
logical_subgroup_id(nir_builder *b, struct native_coarse_values *v)
{
   return nir_ior(b, nir_iand_imm(b, nir_load_subgroup_invocation(b), ~3u), v->logical);
}

static nir_def *
logical_subgroup_ballot(nir_builder *b, struct native_coarse_values *v,
                        nir_def *predicate)
{
   nir_def *live = nir_inot(b, nir_ior(b, v->helper, nir_is_helper_invocation(b, 1)));
   nir_def *result = nir_imm_int(b, 0);
   for (unsigned lane = 0; lane < 4; lane++) {
      nir_def *mask = nir_ballot(b, 1, 32, nir_iand(b, live,
         nir_iand(b, predicate, nir_ieq_imm(b, v->logical, lane))));
      /* Collapse only this quad's four physical bits to its base bit. */
      nir_def *base = mask;
      for (unsigned shift = 1; shift < 4; shift++)
         base = nir_ior(b, base, nir_ushr_imm(b, mask, shift));
      result = nir_ior(b, result,
         nir_ishl_imm(b, nir_iand_imm(b, base, 0x11111111u), lane));
   }
   return result;
}

static nir_def *
physical_subgroup_index(nir_builder *b, struct native_coarse_values *v,
                        nir_def *logical)
{
   nir_def *base = nir_iand_imm(b, logical, ~3u);
   nir_def *wanted = nir_iand_imm(b, logical, 3);
   nir_def *live = nir_ballot(b, 1, 32,
      nir_inot(b, nir_ior(b, v->helper, nir_is_helper_invocation(b, 1))));
   nir_def *physical = nir_imm_int(b, 0);
   for (unsigned lane = 0; lane < 4; lane++) {
      nir_def *candidate = nir_ior_imm(b, base, lane);
      nir_def *mapped = nir_shuffle(b, v->logical, candidate);
      nir_def *active = nir_ine_imm(b, nir_iand(b, live,
         nir_ishl(b, nir_imm_int(b, 1), candidate)), 0);
      physical = nir_bcsel(b, nir_iand(b, active, nir_ieq(b, mapped, wanted)),
                           candidate, physical);
   }
   /* An inactive/out-of-range requested invocation has undefined API data.
    * It must not allow an inactive candidate to override a real live match. */
   return physical;
}

static nir_def *
interpolate_logical_pixel(nir_builder *b, struct native_coarse_values *v,
                          nir_def *native, bool perspective, nir_def *point)
{
   unsigned bits = native->bit_size;
   nir_def *value = bits == 32 ? native : nir_f2f32(b, native);
   nir_def *weighted = perspective
      ? nir_fmul(b, value, nir_channel(b, v->native_position, 3)) : value;
   nir_def *base = nir_quad_broadcast(b, weighted, nir_imm_int(b, 0));
   nir_def *dx = nir_fsub(b, nir_quad_broadcast(b, weighted, nir_imm_int(b, 1)), base);
   nir_def *dy = nir_fsub(b, nir_quad_broadcast(b, weighted, nir_imm_int(b, 2)), base);
   nir_def *origin = nir_quad_broadcast(b, v->native_position, nir_imm_int(b, 0));
   nir_def *x = nir_fsub(b, nir_channel(b, point, 0), nir_channel(b, origin, 0));
   nir_def *y = nir_fsub(b, nir_channel(b, point, 1), nir_channel(b, origin, 1));
   nir_def *logical = nir_fadd(b, base, nir_fadd(b, nir_fmul(b, dx, x), nir_fmul(b, dy, y)));
   if (perspective) {
      nir_def *right = nir_quad_broadcast(b, v->native_position, nir_imm_int(b, 1));
      nir_def *down = nir_quad_broadcast(b, v->native_position, nir_imm_int(b, 2));
      nir_def *w = nir_fadd(b, nir_channel(b, origin, 3), nir_fadd(b,
         nir_fmul(b, x, nir_fsub(b, nir_channel(b, right, 3), nir_channel(b, origin, 3))),
         nir_fmul(b, y, nir_fsub(b, nir_channel(b, down, 3), nir_channel(b, origin, 3)))));
      logical = nir_fdiv(b, logical, w);
   }
   return bits == 32 ? logical : nir_f2fN(b, logical, bits);
}

static bool
lower_native_application(nir_builder *b, nir_instr *instr, void *data)
{
   struct native_coarse_values *v = data;
   if (!_mesa_set_search(v->application, instr) || instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   nir_def *replace = NULL;
   bool vertical = false, coarse = false, derivative = false;
   if (intr->intrinsic == nir_intrinsic_load_interpolated_input ||
       intr->intrinsic == nir_intrinsic_load_barycentric_coord_pixel ||
       intr->intrinsic == nir_intrinsic_load_barycentric_coord_centroid ||
       intr->intrinsic == nir_intrinsic_load_barycentric_coord_at_offset) {
      bool input = intr->intrinsic == nir_intrinsic_load_interpolated_input;
      nir_intrinsic_instr *bary = input ? nir_def_as_intrinsic(intr->src[0].ssa) : intr;
      bool centroid = bary->intrinsic == nir_intrinsic_load_barycentric_centroid ||
                      bary->intrinsic == nir_intrinsic_load_barycentric_coord_centroid;
      bool offset = bary->intrinsic == nir_intrinsic_load_barycentric_at_offset ||
                    bary->intrinsic == nir_intrinsic_load_barycentric_coord_at_offset;
      unsigned mode = nir_intrinsic_interp_mode(bary);
      nir_def *point = centroid ? v->centroid_xy : v->position;
      nir_def *native = &intr->def;
      b->cursor = nir_before_instr(instr);
      if (offset)
         point = nir_fadd(b, nir_trim_vector(b, v->position, 2), bary->src[0].ssa);
      if (centroid || offset) {
         if (input)
            nir_src_rewrite(&intr->src[0], nir_load_barycentric_pixel(b, 32, .interp_mode = mode));
         else
            native = nir_load_barycentric_coord_pixel(b, 32, .interp_mode = mode);
      }
      b->cursor = nir_after_instr(instr);
      nir_def *logical = interpolate_logical_pixel(b, v, native,
         mode != INTERP_MODE_NOPERSPECTIVE, point);
      nir_def_rewrite_uses_after(&intr->def, logical);
      return true; /* Keep the native center input as the plane's source. */
   }
   b->cursor = nir_before_instr(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_frag_coord: replace = v->position; break;
   case nir_intrinsic_load_per_draw_ptr_kk: replace = v->original_per_draw; break;
   case nir_intrinsic_load_frag_shading_rate: replace = v->rate; break;
   case nir_intrinsic_load_primitive_id: replace = v->primitive; break;
   case nir_intrinsic_load_sample_mask_in:
      replace = nir_bcsel(b, nir_is_helper_invocation(b, 1), nir_imm_int(b, 0), v->mask);
      break;
   case nir_intrinsic_load_helper_invocation: replace = v->helper; break;
   case nir_intrinsic_increment_pipeline_statistic_kk:
      /* Native helper writes are suppressed by Metal; only the already
       * available virtual-helper predicate needs an explicit guard here. */
      nir_push_if(b, nir_inot(b, v->helper));
      nir_increment_pipeline_statistic_kk(b, intr->src[0].ssa);
      nir_pop_if(b, NULL);
      nir_instr_remove(instr);
      return true;
   case nir_intrinsic_load_subgroup_invocation:
      replace = logical_subgroup_id(b, v);
      break;
   case nir_intrinsic_ballot: {
      nir_def *mask = logical_subgroup_ballot(b, v, intr->src[0].ssa);
      nir_def *words[NIR_MAX_VEC_COMPONENTS];
      words[0] = nir_u2uN(b, mask, intr->def.bit_size);
      for (unsigned i = 1; i < intr->def.num_components; i++)
         words[i] = nir_imm_intN_t(b, 0, intr->def.bit_size);
      replace = nir_vec(b, words, intr->def.num_components);
      break;
   }
   case nir_intrinsic_first_invocation:
   case nir_intrinsic_elect:
   case nir_intrinsic_read_first_invocation: {
      nir_def *first = nir_find_lsb(b, logical_subgroup_ballot(b, v, nir_imm_true(b)));
      if (intr->intrinsic == nir_intrinsic_first_invocation)
         replace = first;
      else if (intr->intrinsic == nir_intrinsic_read_first_invocation)
         replace = nir_shuffle(b, intr->src[0].ssa, physical_subgroup_index(b, v, first));
      else
         replace = nir_iand(b, nir_inot(b, nir_ior(b, v->helper,
            nir_is_helper_invocation(b, 1))), nir_ieq(b, logical_subgroup_id(b, v), first));
      break;
   }
   case nir_intrinsic_read_invocation:
   case nir_intrinsic_shuffle:
   case nir_intrinsic_shuffle_up:
   case nir_intrinsic_shuffle_down:
   case nir_intrinsic_shuffle_xor: {
      nir_def *index = intr->src[1].ssa;
      if (intr->intrinsic == nir_intrinsic_shuffle_up)
         index = nir_isub(b, logical_subgroup_id(b, v), index);
      else if (intr->intrinsic == nir_intrinsic_shuffle_down)
         index = nir_iadd(b, logical_subgroup_id(b, v), index);
      else if (intr->intrinsic == nir_intrinsic_shuffle_xor)
         index = nir_ixor(b, logical_subgroup_id(b, v), index);
      replace = nir_shuffle(b, intr->src[0].ssa, physical_subgroup_index(b, v, index));
      break;
   }
   case nir_intrinsic_quad_broadcast:
      replace = logical_value(b, v, intr->src[0].ssa, intr->src[1].ssa);
      break;
   case nir_intrinsic_quad_swap_horizontal:
   case nir_intrinsic_quad_swap_vertical:
   case nir_intrinsic_quad_swap_diagonal: {
      unsigned direction = intr->intrinsic == nir_intrinsic_quad_swap_horizontal ? 1 :
         intr->intrinsic == nir_intrinsic_quad_swap_vertical ? 2 : 3;
      replace = logical_value(b, v, intr->src[0].ssa, nir_ixor(b, v->logical, nir_imm_int(b, direction)));
      break;
   }
   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_all:
   case nir_intrinsic_vote_feq:
   case nir_intrinsic_vote_ieq:
      return msl_lower_virtual_subgroup(b, intr,
         nir_ior(b, v->helper, nir_is_helper_invocation(b, 1)));
   case nir_intrinsic_is_helper_invocation:
      /* Dynamic queries (including the shared helper-write guards) must
       * continue to observe native demotion at this application point. */
      replace = nir_ior(b, v->helper, nir_is_helper_invocation(b, 1));
      break;
   case nir_intrinsic_ddx: case nir_intrinsic_ddx_fine: derivative = true; break;
   case nir_intrinsic_ddx_coarse: derivative = true; coarse = true; break;
   case nir_intrinsic_ddy: case nir_intrinsic_ddy_fine: derivative = true; vertical = true; break;
   case nir_intrinsic_ddy_coarse: derivative = true; vertical = true; coarse = true; break;
   default: return false;
   }
   b->cursor = nir_before_instr(instr);
   if (derivative) {
      nir_def *first = coarse ? nir_imm_int(b, 0) : nir_iand_imm(b, v->logical, vertical ? 1 : 2);
      nir_def *second = nir_ior_imm(b, first, vertical ? 2 : 1);
      replace = nir_fsub(b, logical_value(b, v, intr->src[0].ssa, second),
                           logical_value(b, v, intr->src[0].ssa, first));
   }
   nir_def_rewrite_uses(&intr->def, replace);
   nir_instr_remove(instr);
   return true;
}

struct vrs_payload_output {
   nir_variable *variable;
   unsigned offset, components, bits;
};

struct vrs_payload_layout {
   unsigned count, stride;
   struct vrs_payload_output outputs[FRAG_RESULT_MAX * 8];
};

static bool
vrs_payload_layout(nir_shader *nir, struct vrs_payload_layout *layout)
{
   memset(layout, 0, sizeof(*layout));
   unsigned cursor = sizeof(struct kk_vrs_result);
   nir_foreach_variable_with_modes(var, nir, nir_var_shader_temp) {
      if (!var->name || strcmp(var->name, "kk_vrs_application_output")) continue;
      unsigned components = glsl_get_vector_elements(var->type);
      unsigned bits = glsl_get_bit_size(var->type);
      if ((!glsl_type_is_scalar(var->type) && !glsl_type_is_vector(var->type)) ||
          !components || components > 4 || (bits != 16 && bits != 32 && bits != 64) ||
          layout->count == ARRAY_SIZE(layout->outputs)) return false;
      unsigned offset;
      if (var->data.driver_location == FRAG_RESULT_DATA0 * 8 &&
          glsl_get_base_type(var->type) == GLSL_TYPE_FLOAT && bits == 32 && components == 4)
         offset = offsetof(struct kk_vrs_result, color);
      else {
         offset = ALIGN_POT(cursor, bits / 8);
         cursor = offset + components * (bits / 8);
      }
      layout->outputs[layout->count++] = (struct vrs_payload_output){
         .variable = var, .offset = offset, .components = components, .bits = bits,
      };
   }
   layout->stride = ALIGN_POT(cursor, 16);
   return true;
}

bool
kk_nir_vrs_uses_per_draw(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block)
         if (instr->type == nir_instr_type_intrinsic &&
             nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_per_draw_ptr_kk)
            return true;
   return false;
}

uint32_t
kk_nir_vrs_result_stride(nir_shader *nir)
{
   struct vrs_payload_layout layout;
   return vrs_payload_layout(nir, &layout) ? layout.stride : 0;
}

/* Compile clone only. Introduce derivative instructions before recording the
 * application instruction set so they use the same logical-quad lowering as
 * explicit derivatives. Descriptor/sampler/helper sources retain their actual
 * identity and tail; array-layer coordinates are not footprint dimensions. */
static bool
native_implicit_gradients(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_tex) continue;
         nir_tex_instr *tex = nir_instr_as_tex(instr);
         if (tex->op != nir_texop_tex && tex->op != nir_texop_txb) continue;
         int coordinate = nir_tex_instr_src_index(tex, nir_tex_src_coord);
         if (coordinate < 0 || nir_get_tex_src(tex, nir_tex_src_projector) ||
             tex->coord_components <= tex->is_array)
            return false;
         b.cursor = nir_before_instr(instr);
         nir_def *coord = nir_trim_vector(&b, tex->src[coordinate].src.ssa,
                                         tex->coord_components - tex->is_array);
         nir_def *dx = nir_ddx(&b, coord), *dy = nir_ddy(&b, coord);
         int bias_index = nir_tex_instr_src_index(tex, nir_tex_src_bias);
         if (bias_index >= 0) {
            nir_def *bias = nir_f2fN(&b, tex->src[bias_index].src.ssa, dx->bit_size);
            nir_def *scale = nir_fexp2(&b, bias);
            dx = nir_fmul(&b, dx, scale);
            dy = nir_fmul(&b, dy, scale);
            nir_tex_instr_remove_src(tex, bias_index);
         }
         nir_tex_instr_add_src(tex, nir_tex_src_ddx, dx);
         nir_tex_instr_add_src(tex, nir_tex_src_ddy, dy);
         tex->op = nir_texop_txd;
      }
      nir_progress(true, impl, nir_metadata_none);
   }
   return true;
}

static bool
native_reduce_only(const nir_intrinsic_instr *intr, const void *data)
{
   return intr->intrinsic == nir_intrinsic_reduce ||
          intr->intrinsic == nir_intrinsic_inclusive_scan ||
          intr->intrinsic == nir_intrinsic_exclusive_scan;
}

static bool
lower_native_coarse_fragment(nir_shader *nir, const struct vrs_payload_layout *linked)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT ||
       (!linked && nir->info.outputs_written != BITFIELD64_BIT(FRAG_RESULT_DATA0))) return false;
   if (!native_implicit_gradients(nir)) return false;
   bool needs_centroid = false, needs_reduce = false;
   nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_tex) {
            const nir_tex_instr *tex = nir_instr_as_tex(instr);
            switch (tex->op) {
            case nir_texop_txl:
            case nir_texop_txd:
            case nir_texop_txf:
            case nir_texop_txf_ms:
            case nir_texop_txs:
            case nir_texop_query_levels:
            case nir_texop_texture_samples:
               break; /* No native implicit derivative is consumed. */
            default: return false;
            }
         }
         if (instr->type != nir_instr_type_intrinsic) continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         switch (intr->intrinsic) {
         case nir_intrinsic_reduce:
         case nir_intrinsic_inclusive_scan:
         case nir_intrinsic_exclusive_scan:
            needs_reduce = true;
            break;
         case nir_intrinsic_demote: case nir_intrinsic_demote_if:
         case nir_intrinsic_terminate: case nir_intrinsic_terminate_if:
            break;
         case nir_intrinsic_load_barycentric_coord_centroid:
            needs_centroid = true;
            break;
         case nir_intrinsic_load_barycentric_coord_sample:
         case nir_intrinsic_load_barycentric_coord_at_sample:
         case nir_intrinsic_load_output:
            return false;
         case nir_intrinsic_load_interpolated_input: {
            nir_instr *source = nir_def_instr(intr->src[0].ssa);
            if (source->type != nir_instr_type_intrinsic) return false;
            nir_intrinsic_op bary = nir_instr_as_intrinsic(source)->intrinsic;
            if (bary != nir_intrinsic_load_barycentric_pixel &&
                bary != nir_intrinsic_load_barycentric_centroid &&
                bary != nir_intrinsic_load_barycentric_at_offset) return false;
            needs_centroid |= bary == nir_intrinsic_load_barycentric_centroid;
            break;
         }
         default: break;
         }
      }
   if (needs_reduce) {
      const nir_lower_subgroups_options options = {
         .subgroup_size = 32, .ballot_bit_size = 32, .ballot_components = 1,
         .lower_reduce = true, .filter = native_reduce_only,
      };
      NIR_PASS(_, nir, nir_lower_subgroups, &options);
   }
   nir_variable *output = NULL;
   nir_foreach_variable_with_modes(var, nir, nir_var_shader_out)
      if (var->data.location == FRAG_RESULT_DATA0) output = var;
   if (!linked && (!output || glsl_get_vector_elements(output->type) != 4))
      return false;
   nir_variable *temporary = linked ? NULL :
      nir_variable_create(nir, nir_var_shader_temp, output->type,
                          "vrs_application_color");
   nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_deref) continue;
         nir_deref_instr *deref = nir_instr_as_deref(instr);
         if (!linked && deref->deref_type == nir_deref_type_var && deref->var == output)
            deref->var = temporary;
      }
   nir_fixup_deref_modes(nir);
   NIR_PASS(_, nir, nir_lower_helper_writes, true);
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   struct native_coarse_values v = {.application = _mesa_pointer_set_create(NULL)};
   nir_foreach_block(block, impl) nir_foreach_instr(instr, block)
      _mesa_set_add(v.application, instr);
   nir_cf_list application;
   nir_cf_list_extract(&application, &impl->body);
   nir_builder b = nir_builder_create(impl);
   b.cursor = nir_before_cf_list(&impl->body);
   nir_def *context = nir_load_per_draw_ptr_kk(&b, 1, 64);
   nir_def *native_position = nir_load_frag_coord(&b);
   v.native_position = native_position;
   v.original_per_draw = CTX(&b, context, original_per_draw, 64);
   nir_def *native_helper = nir_is_helper_invocation(&b, 1);
   nir_def *native_mask = nir_load_sample_mask_in(&b);
   nir_def *origin_x = CTX(&b, context, origin_x, 32);
   nir_def *origin_y = CTX(&b, context, origin_y, 32);
   nir_def *ix = nir_isub(&b, nir_f2u32(&b, nir_channel(&b, native_position, 0)), origin_x);
   nir_def *iy = nir_isub(&b, nir_f2u32(&b, nir_channel(&b, native_position, 1)), origin_y);
   nir_def *px = nir_iand_imm(&b, ix, 1), *py = nir_iand_imm(&b, iy, 1);
   nir_def *physical = nir_ior(&b, px, nir_ishl_imm(&b, py, 1));
   nir_def *limit = nir_imm_ivec2(&b, 2, 2);
   nir_def *p = nir_umin(&b, nir_vec2(&b, CTX(&b, context, fragment_width, 32),
      CTX(&b, context, fragment_height, 32)), limit);
   nir_def *prim = nir_umin(&b, decode_extent(&b, CTX(&b, context, primitive_rate, 32)), limit);
   nir_def *att = nir_umin(&b, decode_extent(&b, load_rate_image(&b, context,
      nir_iadd(&b, nir_iand_imm(&b, ix, ~1u), origin_x),
      nir_iadd(&b, nir_iand_imm(&b, iy, ~1u), origin_y))), limit);
   nir_def *extent = nir_umin(&b, combine_extent(&b,
      nir_umin(&b, combine_extent(&b, p, prim, CTX(&b, context, combiner0, 32)), limit),
      att, CTX(&b, context, combiner1, 32)), limit);
   nir_def *fw = nir_channel(&b, extent, 0), *fh = nir_channel(&b, extent, 1);
   nir_def *samples = nir_umax(&b, CTX(&b, context, reserved, 32), nir_imm_int(&b, 1));
   nir_def *group_x = nir_udiv(&b, px, fw), *group_y = nir_udiv(&b, py, fh);
   nir_def *cell_x = nir_imul(&b, nir_udiv(&b, ix, fw), fw);
   nir_def *cell_y = nir_imul(&b, nir_udiv(&b, iy, fh), fh);
   nir_def *coverage = CTX(&b, context, coverage, 64);
   nir_def *sample_mask = CTX(&b, context, sample_mask, 32);
   nir_def *width = CTX(&b, context, width, 32);
   nir_def *height = CTX(&b, context, height, 32);
   nir_def *mask = nir_imm_int(&b, 0), *canonical = nir_imm_int(&b, 4);
   /* Coverage was unioned across all native subdivisions of this API primitive.
    * Canonical ownership is selected from that union, never a partial quad. */
   for (int i = 3; i >= 0; i--) {
      nir_def *fx = nir_iadd(&b, cell_x, nir_imm_int(&b, i & 1));
      nir_def *fy = nir_iadd(&b, cell_y, nir_imm_int(&b, i >> 1));
      nir_def *in = nir_iand(&b, nir_iand(&b, nir_ult(&b, nir_imm_int(&b, i & 1), fw),
         nir_ult(&b, nir_imm_int(&b, i >> 1), fh)),
         nir_iand(&b, nir_ult(&b, fx, width), nir_ult(&b, fy, height)));
      nir_def *zero_coverage = nir_imm_int(&b, 0);
      nir_push_if(&b, in);
      nir_def *offset = nir_iadd(&b, nir_imul(&b, fy, width), fx);
      nir_def *fine = nir_load_global(&b, 1, 32, nir_iadd(&b, coverage,
         nir_imul_imm(&b, nir_u2u64(&b, offset), sizeof(struct kk_vrs_coverage))), .align_mul = 4);
      nir_pop_if(&b, NULL);
      fine = nir_if_phi(&b, fine, zero_coverage);
      fine = nir_iand(&b, fine, sample_mask);
      canonical = nir_bcsel(&b, nir_ine_imm(&b, fine, 0), nir_imm_int(&b, i), canonical);
      nir_def *pixel = nir_iadd(&b, nir_imm_int(&b, i & 1), nir_imul(&b, nir_imm_int(&b, i >> 1), fw));
      nir_def *shift = nir_imul(&b, samples, nir_isub(&b,
         nir_iadd_imm(&b, nir_imul(&b, fw, fh), -1), pixel));
      mask = nir_ior(&b, mask, nir_ishl(&b, fine, shift));
   }
   nir_def *owner = nir_iadd(&b, nir_imul(&b, group_x, fw),
      nir_imul_imm(&b, nir_imul(&b, group_y, fh), 2));
   owner = nir_iadd(&b, owner, nir_iand_imm(&b, canonical, 1));
   owner = nir_iadd(&b, owner, nir_iand_imm(&b, canonical, 2));
   nir_def *first = nir_imm_int(&b, 0), *all = nir_imm_int(&b, 0);
   for (int i = 3; i >= 0; i--) {
      nir_def *covered = nir_iand(&b,
         nir_ieq_imm(&b, nir_quad_broadcast(&b, nir_b2i32(&b, native_helper), nir_imm_int(&b, i)), 0),
         nir_ine_imm(&b, nir_quad_broadcast(&b, native_mask, nir_imm_int(&b, i)), 0));
      first = nir_bcsel(&b, covered, nir_imm_int(&b, i), first);
      all = nir_ior(&b, all, nir_b2i32(&b, covered));
   }
   nir_def *own_x = nir_iand_imm(&b, owner, 1), *own_y = nir_ushr_imm(&b, owner, 1);
   nir_def *lx = nir_bcsel(&b, nir_ieq_imm(&b, fw, 2),
      nir_ixor(&b, nir_ixor(&b, px, own_x), nir_iand_imm(&b, nir_udiv(&b, ix, fw), 1)), px);
   nir_def *ly = nir_bcsel(&b, nir_ieq_imm(&b, fh, 2),
      nir_ixor(&b, nir_ixor(&b, py, own_y), nir_iand_imm(&b, nir_udiv(&b, iy, fh), 1)), py);
   v.logical = nir_ior(&b, lx, nir_ishl_imm(&b, ly, 1));
   v.helper = nir_ior(&b, native_helper, nir_ior(&b, nir_ieq_imm(&b, native_mask, 0),
      nir_ior(&b, nir_ine(&b, physical, owner), nir_ieq_imm(&b, mask, 0))));
   v.mask = nir_bcsel(&b, v.helper, nir_imm_int(&b, 0), mask);
   v.primitive = CTX(&b, context, api_primitive, 32);
   v.rate = nir_ior(&b, nir_ishl_imm(&b, nir_ufind_msb(&b, fw), 2), nir_ufind_msb(&b, fh));
   nir_def *qx = nir_imul(&b, nir_udiv(&b, ix, nir_imul_imm(&b, fw, 2)), nir_imul_imm(&b, fw, 2));
   nir_def *qy = nir_imul(&b, nir_udiv(&b, iy, nir_imul_imm(&b, fh, 2)), nir_imul_imm(&b, fh, 2));
   nir_def *sx = nir_fadd(&b, nir_u2f32(&b, nir_iadd(&b, origin_x, nir_iadd(&b, qx, nir_imul(&b, lx, fw)))), nir_fmul_imm(&b, nir_u2f32(&b, fw), .5));
   nir_def *sy = nir_fadd(&b, nir_u2f32(&b, nir_iadd(&b, origin_y, nir_iadd(&b, qy, nir_imul(&b, ly, fh)))), nir_fmul_imm(&b, nir_u2f32(&b, fh), .5));
   nir_def *base = nir_quad_broadcast(&b, native_position, nir_imm_int(&b, 0));
   nir_def *dx = nir_fsub(&b, nir_quad_broadcast(&b, native_position, nir_imm_int(&b, 1)), base);
   nir_def *dy = nir_fsub(&b, nir_quad_broadcast(&b, native_position, nir_imm_int(&b, 2)), base);
   nir_def *interp = nir_fadd(&b, base, nir_fadd(&b,
      nir_fmul(&b, dx, nir_fsub(&b, sx, nir_channel(&b, base, 0))),
      nir_fmul(&b, dy, nir_fsub(&b, sy, nir_channel(&b, base, 1)))));
   v.position = nir_vec4(&b, sx, sy, nir_channel(&b, interp, 2), nir_channel(&b, interp, 3));
   v.centroid_xy = nir_vec2(&b, sx, sy);
   if (needs_centroid) {
      /* Select from the logical cell, including helper evaluations. Native
       * centroid positions belong to physical pixels and cannot be reused. */
      nir_def *ax = nir_f2i32(&b, nir_fsub(&b, sx, nir_fmul_imm(&b, nir_u2f32(&b, fw), .5)));
      nir_def *ay = nir_f2i32(&b, nir_fsub(&b, sy, nir_fmul_imm(&b, nir_u2f32(&b, fh), .5)));
      /* Lower coverage indices belong to later spatial pixels. Overwrite
       * with each covered candidate so the final point is the lowest bit. */
      for (unsigned pixel = 0; pixel < 4; pixel++) {
         nir_def *cx = nir_iadd_imm(&b, ax, pixel & 1);
         nir_def *cy = nir_iadd_imm(&b, ay, pixel >> 1);
         nir_def *valid = nir_iand(&b,
            nir_iand(&b, nir_ult(&b, nir_imm_int(&b, pixel & 1), fw),
                        nir_ult(&b, nir_imm_int(&b, pixel >> 1), fh)),
            nir_iand(&b, nir_ult(&b, nir_isub(&b, cx, origin_x), width),
                        nir_ult(&b, nir_isub(&b, cy, origin_y), height)));
         nir_def *zero = nir_imm_int(&b, 0);
         nir_push_if(&b, valid);
         nir_def *index = nir_iadd(&b, nir_imul(&b, nir_isub(&b, cy, origin_y), width),
            nir_isub(&b, cx, origin_x));
         nir_def *covered = nir_load_global(&b, 1, 32, nir_iadd(&b, coverage,
            nir_imul_imm(&b, nir_u2u64(&b, index), sizeof(struct kk_vrs_coverage))), .align_mul = 4);
         nir_pop_if(&b, NULL);
         covered = nir_if_phi(&b, covered, zero);
         covered = nir_iand(&b, covered, sample_mask);
         /* Coverage bits name the original raster samples. Query their
          * actual native positions, including programmable/Bresenham state,
          * rather than treating a partially covered MSAA pixel as its centre.
          * Empty cells use valid sample0 only for the unselected candidate. */
         nir_def *sample = nir_find_lsb(&b, nir_umax(&b, covered, nir_imm_int(&b, 1)));
         nir_def *location = nir_load_sample_pos_from_id(&b, 32, sample);
         nir_def *point = nir_fadd(&b, nir_vec2(&b,
            nir_i2f32(&b, cx), nir_i2f32(&b, cy)), location);
         v.centroid_xy = nir_bcsel(&b, nir_ine_imm(&b, covered, 0), point, v.centroid_xy);
      }
   }
   nir_def *diagnostics = CTX(&b, context, diagnostics, 64);
   /* Quad operations execute before selecting the single diagnostic writer. */
   nir_def *live = nir_imm_int(&b, 0);
   for (unsigned i = 0; i < 4; i++)
      live = nir_iadd(&b, live, nir_b2i32(&b, nir_ieq_imm(&b,
         nir_quad_broadcast(&b, nir_b2i32(&b, v.helper), nir_imm_int(&b, i)), 0)));
   nir_push_if(&b, nir_iand(&b, nir_ine_imm(&b, diagnostics, 0),
      nir_iand(&b, nir_ine_imm(&b, all, 0), nir_ieq(&b, physical, first))));
   nir_global_atomic(&b, 32, diagnostics, live, .atomic_op = nir_atomic_op_iadd);
   nir_global_atomic(&b, 32, nir_iadd_imm(&b, diagnostics, 4),
      nir_isub(&b, nir_imm_int(&b, 4), live), .atomic_op = nir_atomic_op_iadd);
   nir_pop_if(&b, NULL);
   if (linked) {
      for (unsigned i = 0; i < linked->count; i++) {
         const struct vrs_payload_output *o = &linked->outputs[i];
         nir_store_var(&b, o->variable, nir_imm_zero(&b, o->components, o->bits),
                       BITFIELD_MASK(o->components));
      }
   } else {
      nir_store_var(&b, temporary, nir_imm_zero(&b, 4, 32), 15);
   }
   nir_def *cell = nir_iadd(&b, nir_imul(&b, cell_y, width), cell_x);
   nir_def *result = nir_iadd(&b, CTX(&b, context, results, 64),
      nir_imul_imm(&b, nir_u2u64(&b, cell), linked ? linked->stride : sizeof(struct kk_vrs_result)));
   /* Publish an empty replay entry before the application can terminate or
    * demote. Its unique owner and inverse map are unchanged by shader kill. */
   nir_push_if(&b, nir_inot(&b, v.helper));
   nir_store_global(&b, nir_imm_int(&b, 0), nir_iadd_imm(&b, result,
      offsetof(struct kk_vrs_result, mask)), .write_mask = 1, .align_mul = 4);
   nir_store_global(&b, v.primitive, nir_iadd_imm(&b, result,
      offsetof(struct kk_vrs_result, api_primitive)), .write_mask = 1, .align_mul = 4);
   nir_store_global(&b, v.rate, nir_iadd_imm(&b, result,
      offsetof(struct kk_vrs_result, rate)), .write_mask = 1, .align_mul = 4);
   nir_def *map = CTX(&b, context, pixel_results, 64);
   for (unsigned i = 0; i < 4; i++) {
      nir_def *fx = nir_iadd(&b, cell_x, nir_imm_int(&b, i & 1));
      nir_def *fy = nir_iadd(&b, cell_y, nir_imm_int(&b, i >> 1));
      nir_push_if(&b, nir_iand(&b,
         nir_ult(&b, nir_imm_int(&b, i & 1), fw),
         nir_ult(&b, nir_imm_int(&b, i >> 1), fh)));
      nir_def *pixel = nir_iadd(&b, nir_imul(&b, fy, width), fx);
      nir_store_global(&b, cell, nir_iadd(&b, map,
         nir_imul_imm(&b, nir_u2u64(&b, pixel), 4)), .write_mask = 1, .align_mul = 4);
      nir_pop_if(&b, NULL);
   }
   nir_pop_if(&b, NULL);
   b.cursor = nir_cf_reinsert(&application, b.cursor);
   /* Native demotion remains native; only a still-live logical owner may
    * publish application results. Termination leaves the earlier empty mask. */
   nir_push_if(&b, nir_inot(&b, nir_ior(&b, v.helper, nir_is_helper_invocation(&b, 1))));
   if (linked) {
      for (unsigned i = 0; i < linked->count; i++) {
         const struct vrs_payload_output *o = &linked->outputs[i];
         nir_store_global(&b, nir_load_var(&b, o->variable),
            nir_iadd_imm(&b, result, o->offset),
            .write_mask = BITFIELD_MASK(o->components), .align_mul = o->bits / 8);
      }
   } else {
      nir_store_global(&b, nir_load_var(&b, temporary), nir_iadd_imm(&b, result,
         offsetof(struct kk_vrs_result, color)), .write_mask = 15, .align_mul = 4);
   }
   nir_store_global(&b, mask, nir_iadd_imm(&b, result,
      offsetof(struct kk_vrs_result, mask)), .write_mask = 1, .align_mul = 4);
   nir_pop_if(&b, NULL);
   /* Native replay, after this draw completes, consumes the unique result.
    * No fragment waits for another fragment or repeats application effects. */
   if (!linked) nir_store_var(&b, output, nir_imm_zero(&b, 4, 32), 15);
   nir_shader_instructions_pass(nir, lower_native_application, nir_metadata_none, &v);
   _mesa_set_destroy(v.application, NULL);
   nir_progress(true, impl, nir_metadata_none);
   nir_shader_gather_info(nir, impl);
   nir_validate_shader(nir, "after native logical coarse fragment prototype");
   return true;
}

/* Keep the already-linked application distinct from its fine output epilogue.
 * The retained source may keep this call; the compilation clone must flatten
 * it before any entrypoint-only consumer or the call-free MSL emitter. */
struct vrs_output_slot {
   nir_variable *temporary;
   nir_io_semantics semantics;
   nir_alu_type type;
   unsigned base, components, bits, mask;
   bool used;
};

bool
kk_nir_vrs_split_application(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT || !nir->info.io_lowered)
      return false;
   nir_function_impl *application = nir_shader_get_entrypoint(nir);
   struct vrs_output_slot slots[FRAG_RESULT_MAX * 8] = {0};
   nir_foreach_block(block, application) nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_intrinsic) continue;
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      bool store = intr->intrinsic == nir_intrinsic_store_output;
      bool load = intr->intrinsic == nir_intrinsic_load_output;
      if (!store && !load) continue;
      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      if (load && sem.fb_fetch_output) continue;
      if (!nir_src_is_const(intr->src[store ? 1 : 0])) return false;
      unsigned offset = nir_src_as_uint(intr->src[store ? 1 : 0]);
      if (offset >= FRAG_RESULT_MAX || sem.location + offset >= FRAG_RESULT_MAX)
         return false;
      sem.location += offset;
      sem.num_slots = 1;
      struct vrs_output_slot *slot = &slots[8 * sem.location + 4 * sem.dual_source_blend_index + 2 * sem.high_16bits + sem.high_dvec2];
      unsigned bits = store ? intr->src[0].ssa->bit_size : intr->def.bit_size;
      nir_alu_type type = store ? nir_intrinsic_src_type(intr) : nir_intrinsic_dest_type(intr);
      if (slot->used && (slot->bits != bits || slot->type != type ||
                        slot->base != nir_intrinsic_base(intr) + offset)) return false;
      slot->used = true;
      slot->semantics = sem;
      slot->base = nir_intrinsic_base(intr) + offset;
      slot->bits = bits;
      slot->type = type;
      slot->components = MAX2(slot->components, nir_intrinsic_component(intr) + intr->num_components);
      if (slot->components > 4) return false;
      if (store) slot->mask |= nir_intrinsic_write_mask(intr) << nir_intrinsic_component(intr);
   }
   for (unsigned i = 0; i < ARRAY_SIZE(slots); i++) {
      struct vrs_output_slot *slot = &slots[i];
      if (!slot->used) continue;
      const struct glsl_type *type = glsl_vector_type(
         nir_get_glsl_base_type_for_nir_type(slot->type), slot->components);
      slot->temporary = nir_variable_create(nir, nir_var_shader_temp, type,
                                            "kk_vrs_application_output");
      slot->temporary->data.location = slot->semantics.location;
      slot->temporary->data.index = slot->semantics.dual_source_blend_index;
      slot->temporary->data.driver_location = i;
   }
   nir_builder b = nir_builder_create(application);
   nir_foreach_block(block, application) nir_foreach_instr_safe(instr, block) {
      if (instr->type != nir_instr_type_intrinsic) continue;
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      bool store = intr->intrinsic == nir_intrinsic_store_output;
      bool load = intr->intrinsic == nir_intrinsic_load_output;
      if (!store && !load) continue;
      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      if (load && sem.fb_fetch_output) continue;
      sem.location += nir_src_as_uint(intr->src[store ? 1 : 0]);
      struct vrs_output_slot *slot = &slots[8 * sem.location + 4 * sem.dual_source_blend_index + 2 * sem.high_16bits + sem.high_dvec2];
      unsigned component = nir_intrinsic_component(intr);
      b.cursor = nir_before_instr(instr);
      if (store) {
         nir_def *values[4];
         for (unsigned i = 0; i < slot->components; i++)
            values[i] = i >= component && i < component + intr->num_components
               ? nir_channel(&b, intr->src[0].ssa, i - component)
               : nir_undef(&b, 1, slot->bits);
         nir_store_var(&b, slot->temporary, nir_vec(&b, values, slot->components),
            nir_intrinsic_write_mask(intr) << component);
      } else {
         nir_def *value = nir_load_var(&b, slot->temporary);
         unsigned channels[4];
         for (unsigned i = 0; i < intr->def.num_components; i++) channels[i] = component + i;
         nir_def_rewrite_uses(&intr->def, nir_swizzle(&b, value, channels,
                                                     intr->def.num_components));
      }
      nir_instr_remove(instr);
   }
   nir_function *function = application->function;
   const char *entry_name = ralloc_strdup(nir, function->name);
   function->name = ralloc_strdup(function, "kk_vrs_application");
   function->is_entrypoint = false;
   nir_function *wrapper = nir_function_create(nir, entry_name);
   wrapper->is_entrypoint = true;
   nir_function_impl *impl = nir_function_impl_create(wrapper);
   b = nir_builder_create(impl);
   b.cursor = nir_before_cf_list(&impl->body);
   nir_builder_instr_insert(&b, &nir_call_instr_create(nir, function)->instr);
   for (unsigned i = 0; i < ARRAY_SIZE(slots); i++) {
      struct vrs_output_slot *slot = &slots[i];
      if (!slot->used || !slot->mask) continue;
      nir_store_output(&b, nir_load_var(&b, slot->temporary), nir_imm_int(&b, 0),
         .base = slot->base, .component = 0, .write_mask = slot->mask,
         .src_type = slot->type, .io_semantics = slot->semantics);
   }
   nir_progress(true, application, nir_metadata_none);
   nir_progress(true, impl, nir_metadata_none);
   nir_validate_shader(nir, "after linked VRS application split");
   return true;
}

void
kk_nir_vrs_flatten(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   nir_validate_shader(nir, "after VRS compilation clone flattening");
}

/* Compilation clone only: the retained wrapper keeps the fine epilogue for
 * replay. Execute the named application through the same coverage/quad route
 * used by the native component, without executing that epilogue coarsely. */
bool
kk_nir_lower_linked_native_coarse_fragment(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT || !nir->info.io_lowered)
      return false;
   nir_function *application = NULL;
   nir_foreach_function(function, nir)
      if (!strcmp(function->name, "kk_vrs_application")) application = function;
   if (!application || !application->impl || application->num_params) return false;
   struct vrs_payload_layout layout;
   if (!vrs_payload_layout(nir, &layout)) return false;
   nir_shader_get_entrypoint(nir)->function->is_entrypoint = false;
   application->is_entrypoint = true;
   kk_nir_vrs_flatten(nir);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   return lower_native_coarse_fragment(nir, &layout);
}

bool
kk_nir_lower_native_coarse_fragment(nir_shader *nir)
{
   return lower_native_coarse_fragment(nir, NULL);
}

static bool
lower_fine_fragment_rate(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_frag_shading_rate) return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def, nir_imm_int(b, 0));
   return true;
}

void
kk_nir_vrs_lower_fine_rate(nir_shader *nir)
{
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FRAG_SHADING_RATE))
      nir_shader_intrinsics_pass(nir, lower_fine_fragment_rate,
                                 nir_metadata_control_flow, NULL);
}

/* Replay executes the original once-lowered fine epilogue after substituting
 * captured application results. The application function is removed, so its
 * external effects cannot run again. Typed exports use the declared payload;
 * masks are sliced by original raster sample index for each fine pixel. */
bool
kk_nir_lower_linked_replay_fragment(nir_shader *nir, unsigned samples)
{
   if ((samples != 1 && samples != 2 && samples != 4) ||
       nir->info.stage != MESA_SHADER_FRAGMENT ||
       !nir->info.io_lowered) return false;
   struct vrs_payload_layout layout;
   if (!vrs_payload_layout(nir, &layout)) return false;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_call_instr *call = NULL;
   nir_foreach_block(block, impl) nir_foreach_instr(instr, block) {
      if (instr->type == nir_instr_type_call) {
         if (call || strcmp(nir_instr_as_call(instr)->callee->name,
                            "kk_vrs_application")) return false;
         call = nir_instr_as_call(instr);
      }

   }
   if (!call) return false;
   nir_builder b = nir_builder_create(impl);
   b.cursor = nir_before_cf_list(&impl->body);
   nir_def *context = nir_load_per_draw_ptr_kk(&b, 1, 64);
   nir_def *original_per_draw = CTX(&b, context, original_per_draw, 64);
   nir_foreach_block(block, impl) nir_foreach_instr_safe(instr, block) {
      if (instr->type != nir_instr_type_intrinsic) continue;
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic == nir_intrinsic_load_per_draw_ptr_kk && &intr->def != context) {
         nir_def_rewrite_uses(&intr->def, original_per_draw);
         nir_instr_remove(instr);
      }
   }
   nir_def *position = nir_load_frag_coord(&b);
   nir_def *x = nir_isub(&b, nir_f2u32(&b, nir_channel(&b, position, 0)),
      CTX(&b, context, origin_x, 32));
   nir_def *y = nir_isub(&b, nir_f2u32(&b, nir_channel(&b, position, 1)),
      CTX(&b, context, origin_y, 32));
   nir_def *valid = nir_iand(&b, nir_inot(&b, nir_is_helper_invocation(&b, 1)),
      nir_iand(&b, nir_ult(&b, x, CTX(&b, context, width, 32)),
                  nir_ult(&b, y, CTX(&b, context, height, 32))));
   nir_def *zero_mask = nir_imm_int(&b, 0);
   nir_push_if(&b, valid);
   nir_def *pixel = nir_iadd(&b, nir_imul(&b, y, CTX(&b, context, width, 32)), x);
   nir_def *cell = nir_load_global(&b, 1, 32, nir_iadd(&b,
      CTX(&b, context, pixel_results, 64),
      nir_imul_imm(&b, nir_u2u64(&b, pixel), sizeof(uint32_t))), .align_mul = 4);
   nir_def *result = nir_iadd(&b, CTX(&b, context, results, 64),
      nir_imul_imm(&b, nir_u2u64(&b, cell), layout.stride));
   nir_def *rate = nir_load_global(&b, 1, 32, nir_iadd_imm(&b, result,
      offsetof(struct kk_vrs_result, rate)), .align_mul = 4);
   nir_def *mask = nir_load_global(&b, 1, 32, nir_iadd_imm(&b, result,
      offsetof(struct kk_vrs_result, mask)), .align_mul = 4);
   nir_def *extent = decode_extent(&b, rate);
   nir_def *w = nir_channel(&b, extent, 0), *h = nir_channel(&b, extent, 1);
   nir_def *p = nir_iadd(&b, nir_imul(&b, nir_umod(&b, y, h), w), nir_umod(&b, x, w));
   nir_def *bit = nir_isub(&b, nir_iadd_imm(&b, nir_imul(&b, w, h), -1), p);
   nir_def *shift = nir_imul_imm(&b, bit, samples);
   unsigned sample_bits = BITFIELD_MASK(samples);
   nir_def *coverage = nir_iand_imm(&b, nir_ushr(&b, mask, shift), sample_bits);
   /* Empty/killed entries have no application payload. Do not load stale
    * storage or feed undefined depth/color values into the fine epilogue. */
   for (unsigned i = 0; i < layout.count; i++) {
      const struct vrs_payload_output *o = &layout.outputs[i];
      nir_store_var(&b, o->variable, nir_imm_zero(&b, o->components, o->bits),
         BITFIELD_MASK(o->components));
   }
   nir_push_if(&b, nir_ine_imm(&b, coverage, 0));
   for (unsigned i = 0; i < layout.count; i++) {
      const struct vrs_payload_output *o = &layout.outputs[i];
      nir_def *value = nir_load_global(&b, o->components, o->bits,
         nir_iadd_imm(&b, result, o->offset), .align_mul = o->bits / 8);
      /* Application SampleMask uses the coarse pixel/sample bit order.
       * Slice it before the once-lowered fine epilogue combines static masks
       * or alpha coverage. Intersecting every pixel with bit zero is wrong. */
      if (o->variable->data.location == FRAG_RESULT_SAMPLE_MASK)
         value = nir_iand_imm(&b, nir_ushr(&b, value, shift), sample_bits);
      nir_store_var(&b, o->variable, value, BITFIELD_MASK(o->components));
   }
   nir_pop_if(&b, NULL);
   nir_pop_if(&b, NULL);
   coverage = nir_if_phi(&b, coverage, zero_mask);
   nir_instr_remove(&call->instr);
   nir_remove_non_entrypoints(nir);
   bool writes_mask = false;
   nir_foreach_block(block, impl) nir_foreach_instr_safe(instr, block) {
      if (instr->type != nir_instr_type_intrinsic) continue;
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic != nir_intrinsic_store_output ||
          nir_intrinsic_io_semantics(intr).location != FRAG_RESULT_SAMPLE_MASK) continue;
      b.cursor = nir_before_instr(instr);
      nir_src_rewrite(&intr->src[0], nir_iand(&b, intr->src[0].ssa, coverage));
      writes_mask = true;
   }
   if (!writes_mask) {
      b.cursor = nir_after_impl(impl);
      nir_store_output(&b, coverage, nir_imm_int(&b, 0), .write_mask = 1,
         .src_type = nir_type_uint32,
         .io_semantics = (nir_io_semantics){.location = FRAG_RESULT_SAMPLE_MASK, .num_slots = 1});
   }
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   nir_progress(true, impl, nir_metadata_none);
   nir_shader_gather_info(nir, impl);
   nir_validate_shader(nir, "after retained fine fragment replay");
   return true;
}

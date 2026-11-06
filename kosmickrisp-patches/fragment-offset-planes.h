/* SPDX-License-Identifier: MIT */
#ifndef MCL_KOSMICKRISP_FRAGMENT_OFFSET_PLANES_H
#define MCL_KOSMICKRISP_FRAGMENT_OFFSET_PLANES_H

struct kk_offset_plane {
   nir_intrinsic_instr *input;
   enum glsl_interp_mode mode;
   unsigned offset;
   nir_def *value, *dx, *dy, *valid;
};

static bool
kk_offset_input(nir_intrinsic_instr *intr)
{
   if (intr->intrinsic != nir_intrinsic_load_interpolated_input ||
       intr->def.bit_size != 32 || !nir_src_is_const(intr->src[1]))
      return false;
   nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
   return bary->intrinsic == nir_intrinsic_load_barycentric_at_offset;
}

static bool
kk_offset_same_input(const struct kk_offset_plane *plane,
                     nir_intrinsic_instr *intr, enum glsl_interp_mode mode)
{
   return plane->mode == mode && plane->input->def.num_components ==
          intr->def.num_components && plane->offset == nir_src_as_uint(intr->src[1]) &&
          memcmp(plane->input->const_index, intr->const_index,
                 nir_intrinsic_infos[intr->intrinsic].num_indices *
                    sizeof(intr->const_index[0])) == 0;
}

/* Center input values are rational screen-space planes. Multiplying a smooth
 * value by reciprocal clip-W makes its numerator affine. Hoist the planes and
 * fine derivatives to the entry block: explicit interpolation remains valid
 * inside divergent control flow, where a newly inserted derivative would not.
 * A reciprocal-W pole can cross a partially covered quad. The center values
 * cannot recover its numerator there, so retain native offset interpolation
 * for those quads. Dynamic input locations and other widths also stay native.
 */
static bool
kk_nir_lower_fs_offset_planes(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      return false;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   unsigned count = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic &&
             kk_offset_input(nir_instr_as_intrinsic(instr)))
            count++;
      }
   }
   if (!count)
      return nir_no_progress(impl);

   /* Snapshot inputs before adding native fallback branches. */
   nir_intrinsic_instr **inputs = ralloc_array(NULL, nir_intrinsic_instr *, count);
   unsigned index = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic &&
             kk_offset_input(nir_instr_as_intrinsic(instr)))
            inputs[index++] = nir_instr_as_intrinsic(instr);
      }
   }
   struct kk_offset_plane *planes = rzalloc_array(NULL, struct kk_offset_plane, count);
   unsigned plane_count = 0;
   nir_builder entry = nir_builder_create(impl);
   entry.cursor = nir_before_impl(impl);
   entry.fp_math_ctrl = nir_fp_no_fast_math;
   nir_def *q = NULL, *qx = NULL, *qy = NULL, *valid_q = NULL;
   nir_builder b = nir_builder_create(impl);
   b.fp_math_ctrl = nir_fp_no_fast_math;

   for (unsigned j = 0; j < count; j++) {
      nir_intrinsic_instr *intr = inputs[j];
      nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
      enum glsl_interp_mode mode = nir_intrinsic_interp_mode(bary);
      bool linear = mode == INTERP_MODE_NOPERSPECTIVE;
      struct kk_offset_plane *plane = NULL;
      for (unsigned i = 0; i < plane_count; i++) {
         if (kk_offset_same_input(&planes[i], intr, mode)) {
            plane = &planes[i];
            break;
         }
      }
      if (!plane) {
         plane = &planes[plane_count++];
         plane->input = intr;
         plane->mode = mode;
         plane->offset = nir_src_as_uint(intr->src[1]);
         nir_def *pixel = nir_load_barycentric_pixel(
            &entry, 32, .interp_mode = mode);
         nir_intrinsic_instr *center = nir_instr_as_intrinsic(
            nir_instr_clone(nir, &intr->instr));
         /* Cloned sources are not in use lists until insertion. */
         center->src[0].ssa = pixel;
         center->src[1].ssa = nir_imm_int(&entry, plane->offset);
         nir_builder_instr_insert(&entry, &center->instr);
         plane->value = &center->def;
         if (!linear) {
            if (!q) {
               q = nir_channel(&entry, nir_load_frag_coord(&entry), 3);
               qx = nir_ddx_fine(&entry, q);
               qy = nir_ddy_fine(&entry, q);
               /* Keep all quad centers away from poles and extreme scales.
                * Intermediate products must remain normal float32 values. */
               nir_def *radius = nir_fadd(&entry, nir_fabs(&entry, qx),
                                                   nir_fabs(&entry, qy));
               valid_q = nir_iand(&entry,
                  nir_flt_imm(&entry, nir_fadd(&entry, q, radius), 0x1p16f),
                  nir_flt(&entry, nir_imm_float(&entry, 0x1p-16f),
                                    nir_fsub(&entry, q, radius)));
            }
            /* Bound input magnitudes across the derivative quad too. Zero
             * is valid; tiny or huge nonzero values stay on the native path. */
            nir_def *magnitude = nir_fabs(&entry, plane->value);
            nir_def *normal = nir_ior(&entry,
               nir_feq_imm(&entry, magnitude, 0.0f),
               nir_iand(&entry,
                  nir_fge(&entry, magnitude, nir_imm_float(&entry, 0x1p-100f)),
                  nir_flt_imm(&entry, magnitude, 0x1p100f)));
            plane->valid = nir_iand(&entry, valid_q,
               nir_quad_vote_all(&entry, 1, nir_ball(&entry, normal)));
            plane->value = nir_fmul(&entry, plane->value, q);
         }
         plane->dx = nir_ddx_fine(&entry, plane->value);
         plane->dy = nir_ddy_fine(&entry, plane->value);
         if (linear)
            plane->valid = nir_imm_true(&entry);
         nir_def *values[] = {plane->value, plane->dx, plane->dy};
         for (unsigned i = 0; i < ARRAY_SIZE(values); i++)
            plane->valid = nir_iand(&entry, plane->valid,
               nir_ball(&entry, nir_fisfinite(&entry, values[i])));
      }
      b.cursor = nir_before_instr(&intr->instr);
      nir_def *offset = bary->src[0].ssa;
      nir_def *x = nir_channel(&b, offset, 0);
      nir_def *y = nir_channel(&b, offset, 1);
      nir_def *value = nir_fadd(&b, plane->value,
         nir_fadd(&b, nir_fmul(&b, plane->dx, x),
                      nir_fmul(&b, plane->dy, y)));
      if (!linear) {
         nir_def *denominator = nir_fadd(&b, q,
            nir_fadd(&b, nir_fmul(&b, qx, x), nir_fmul(&b, qy, y)));
         value = nir_fdiv(&b, value, denominator);
      }
      {
         nir_push_if(&b, plane->valid);
         nir_def *arithmetic = nir_mov(&b, value);
         nir_push_else(&b, NULL);
         nir_intrinsic_instr *native = nir_instr_as_intrinsic(
            nir_instr_clone(nir, &intr->instr));
         nir_builder_instr_insert(&b, &native->instr);
         nir_pop_if(&b, NULL);
         value = nir_if_phi(&b, arithmetic, &native->def);
      }
      nir_def_replace(&intr->def, value);
   }
   ralloc_free(inputs);
   ralloc_free(planes);
   nir_progress(true, impl, nir_metadata_none);
   nir_opt_dce(nir);
   nir_shader_gather_info(nir, impl);
   return true;
}

#endif

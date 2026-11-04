/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_ATTACHMENTS_NIR_H
#define KK_FRAGMENT_SHADING_RATE_ATTACHMENTS_NIR_H
#include "kk_interlock_framebuffer_nir.h"
#include "kk_depth_bounds_types.h"

struct kk_vrs_attachment_nir {
   struct kk_interlock_program_nir *program;
   bool custom_resolve, failed;
};

/* Use the same stored-sample reader as DS/blend. The original sample operand
 * is independent of the representative input-attachment pixel. */
static nir_def *
kk_vrs_attachment_shadow(nir_builder *b, unsigned components, unsigned bits,
   nir_alu_type type, nir_def *offset, nir_def *sample, nir_io_semantics io,
   unsigned component)
{
   return nir_load_tile_image(b, components, bits, offset, sample,
      .dest_type = type, .io_semantics = io, .component = component,
      .access = ACCESS_COHERENT);
}

static bool
kk_vrs_application_attachment(nir_builder *b, nir_instr *instr, void *data)
{
   struct kk_vrs_attachment_nir *s = data;
   struct kk_interlock_framebuffer_nir *framebuffer = s->program->data;
   struct kk_interlock_transaction_nir *t = &framebuffer->transaction;
   b->cursor = nir_before_instr(instr);
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic != nir_intrinsic_load_output || s->custom_resolve)
         return false;
      nir_io_semantics io = nir_intrinsic_io_semantics(intr);
      if (io.location < FRAG_RESULT_DATA0 || io.location >= FRAG_RESULT_DATA0 + 8)
         return false;
      /* Application output variables were replaced by typed temporaries at
       * splitting. A native output fetch left in this function is an input
       * attachment; fine blending is in the separate epilogue. */
      nir_def *sample = nir_load_sample_id(b);
      nir_def_replace(&intr->def, kk_vrs_attachment_shadow(b,
         intr->def.num_components, intr->def.bit_size, nir_intrinsic_dest_type(intr),
         intr->src[0].ssa, sample, io, nir_intrinsic_component(intr)));
      return true;
   }
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   if ((!tex->input_attachment_index && !tex->input_attachment_depth &&
        !tex->input_attachment_stencil) ||
       (tex->op != nir_texop_txf && tex->op != nir_texop_txf_ms))
      return false;
   if (tex->is_sparse || tex->def.num_components > 4) {
      s->failed = true;
      return false;
   }
   int array = nir_tex_instr_src_index(tex, nir_tex_src_texture_offset);
   if (tex->input_attachment_index && tex->texture_array_size > 1 && array < 0) {
      s->failed = true; /* Missing original array metadata is not index zero. */
      return false;
   }
   nir_def *index = nir_iadd_imm(b, array >= 0 ? tex->src[array].src.ssa : nir_imm_int(b, 0), tex->texture_index);
   int ms = nir_tex_instr_src_index(tex, nir_tex_src_ms_index);
   nir_def *sample = ms >= 0 ? tex->src[ms].src.ssa : nir_imm_int(b, 0);
   nir_def *params = t->render->parameters(b, t->render->data);
   nir_def *stored[10];
   unsigned count = 0;
   if (!s->custom_resolve) {
      for (unsigned i = 0; i < 10; i++) {
         bool indexed = tex->input_attachment_index;
         bool floating = nir_alu_type_get_base_type(tex->dest_type) == nir_type_float;
         if ((!indexed && i != (tex->input_attachment_depth ? 8 : 9)) ||
             (i == 8 && !floating) || (i == 9 && floating))
            continue;
         unsigned field = i < 8 ? offsetof(struct kk_depth_bounds_envelope, input_color_indices) + i * 4 :
            i == 8 ? offsetof(struct kk_depth_bounds_envelope, input_depth_index) :
                     offsetof(struct kk_depth_bounds_envelope, input_stencil_index);
         nir_def *mapped = kk_interlock_env_load(b, params, 1, 32, field);
         nir_def *matches = indexed ? nir_iand(b,
            nir_ine_imm(b, mapped, MESA_VK_ATTACHMENT_UNUSED), nir_ieq(b, mapped, index)) :
            nir_ine_imm(b, kk_interlock_env_load(b, params, 1, 64,
               i == 8 ? offsetof(struct kk_interlock_envelope, depth) :
                        offsetof(struct kk_interlock_envelope, stencil)), 0);
         nir_push_if(b, matches);
         stored[count++] = kk_vrs_attachment_shadow(b, tex->def.num_components,
            tex->def.bit_size, tex->dest_type, nir_imm_int(b, 0), sample,
            (nir_io_semantics){.location = i < 8 ? FRAG_RESULT_DATA0 + i :
               i == 8 ? FRAG_RESULT_DEPTH : FRAG_RESULT_STENCIL, .num_slots = 1}, 0);
         nir_push_else(b, NULL);
      }
   }
   nir_tex_instr *fallback = nir_instr_as_tex(nir_instr_clone(b->shader, instr));
   fallback->can_speculate = false;
   /* Preserve the source's real descriptor tail. Its typed resource load
    * belongs inside the unmapped branch when it is a direct handle load. */
   int handle = nir_tex_instr_src_index(fallback, nir_tex_src_texture_handle);
   if (handle >= 0) {
      nir_instr *def = nir_def_instr(fallback->src[handle].src.ssa);
      if (def->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(def)->intrinsic == nir_intrinsic_load_texture_handle_kk) {
         nir_intrinsic_instr *copy = nir_instr_as_intrinsic(nir_instr_clone(b->shader, def));
         nir_builder_instr_insert(b, &copy->instr);
         nir_src_rewrite(&fallback->src[handle].src, &copy->def);
      }
   }
   int coord = nir_tex_instr_src_index(fallback, nir_tex_src_coord);
   if (coord >= 0) {
      nir_def *original = fallback->src[coord].src.ssa;
      nir_def *offset = nir_isub(b, nir_channels(b, original, 3),
         nir_f2i32(b, nir_channels(b, nir_load_frag_coord(b), 3)));
      nir_def *pixel = nir_iadd(b, nir_load_var(b, t->pixel), offset);
      nir_def *components[4];
      for (unsigned c = 0; c < fallback->coord_components; c++)
         components[c] = c < 2 ? nir_channel(b, pixel, c) : nir_channel(b, original, c);
      nir_src_rewrite(&fallback->src[coord].src, nir_vec(b, components, fallback->coord_components));
   }
   nir_builder_instr_insert(b, &fallback->instr);
   nir_def *result = &fallback->def;
   while (count) {
      nir_pop_if(b, NULL);
      result = nir_if_phi(b, stored[--count], result);
   }
   nir_def_replace(&tex->def, result);
   return true;
}

static bool
kk_vrs_lower_application_attachments(nir_function *application,
   struct kk_interlock_program_nir *program, bool custom_resolve)
{
   struct kk_vrs_attachment_nir state = {.program = program, .custom_resolve = custom_resolve};
   nir_function_instructions_pass(application->impl, kk_vrs_application_attachment,
      nir_metadata_none, &state);
   return !state.failed;
}
#endif

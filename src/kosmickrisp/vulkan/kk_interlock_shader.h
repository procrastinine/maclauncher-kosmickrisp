/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_SHADER_H
#define KK_INTERLOCK_SHADER_H

#include "kk_interlock_framebuffer_nir.h"
#include "kk_interlock_replay_nir.h"
#include "kk_interlock_load_nir.h"
#include "util/u_atomic.h"

#include "kk_interlock_shader_data.h"

/* The shared compiler implementation is defined later in kk_shader.c. */
static void kk_compile_msl_options(struct kk_device *dev,
   nir_shader *nir, const struct vk_graphics_pipeline_state *state,
   struct msl_compile_data *data, const struct kk_compile_options *opts,
   bool *has_framebuffer_reads, uint8_t *framebuffer_read_mask);

static void
kk_interlock_shader_finish(struct kk_interlock_shader *shader)
{
   if (shader && p_atomic_dec_zero(&shader->references))
      ralloc_free(shader);
}

static struct kk_interlock_shader *
kk_interlock_shader_ref(struct kk_interlock_shader *shader)
{
   if (shader)
      p_atomic_inc(&shader->references);
   return shader;
}

static nir_def *
kk_interlock_shader_parameters(nir_builder *b, void *data)
{
   return nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, nir_load_geometry_param_buffer_poly(b),
         offsetof(struct poly_geometry_params, capture)), .align_mul = 8);
}

static void
kk_interlock_compile_program(struct kk_device *dev,
   struct kk_interlock_shader *shader, enum kk_interlock_program program,
   nir_shader *nir, const struct vk_graphics_pipeline_state *state,
   const struct vk_pipeline_robustness_state *robustness)
{
   msl_preprocess_nir(nir);
   /* Application descriptors were lowered before constructing these internal
    * programs. Match the shared auxiliary-program lowering convention. */
   kk_nir_lower_descriptors(nir, 0, robustness, 0, NULL);
   /* Track reads remaining in the compiled native program. Source fetch and
    * blend read the transaction shadow; replay only writes attachments. */
   bool *framebuffer_reads = program <= KK_INTERLOCK_REPLAY ?
      &shader->info.framebuffer_reads[program] : NULL;
   kk_compile_msl_options(dev, nir, state, &shader->program[program], NULL,
      framebuffer_reads, NULL);
   ralloc_steal(shader, shader->program[program].code);
   ralloc_steal(shader, shader->program[program].entrypoint_name);
}

/* The source program has already used the ordinary attachment swizzle. Restore
 * its raw color to logical channels before the shared fixed-function passes;
 * their final stores are converted back by the same ordinary swizzle pass. */
static VkFormat
kk_interlock_color_format(const struct vk_graphics_pipeline_state *state,
   unsigned logical)
{
   for (unsigned i = 0; i < state->rp->color_attachment_count; ++i)
      if (kk_get_logical_color_att_index(state, i) == logical)
         return state->rp->color_attachment_formats[i];
   return VK_FORMAT_UNDEFINED;
}

static bool
kk_interlock_restore_sample_source(nir_builder *b, nir_intrinsic_instr *intr,
   void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   unsigned slot = nir_intrinsic_io_semantics(intr).location;
   if (slot < FRAG_RESULT_DATA0 || slot >= FRAG_RESULT_DATA0 + 8)
      return false;
   const struct vk_graphics_pipeline_state *state = data;
   VkFormat format = kk_interlock_color_format(state, slot - FRAG_RESULT_DATA0);
   if (format == VK_FORMAT_UNDEFINED)
      return false;
   const struct kk_va_format *native = kk_get_va_format(vk_format_to_pipe_format(format));
   if (native->is_native)
      return false;
   unsigned channels[] = {native->unswizzle.red, native->unswizzle.green,
      native->unswizzle.blue, native->unswizzle.alpha};
   b->cursor = nir_before_instr(&intr->instr);
   nir_src_rewrite(&intr->src[0], nir_swizzle(b, intr->src[0].ssa,
      channels, intr->src[0].ssa->num_components));
   return true;
}

static void
kk_interlock_lower_sample_outputs(nir_shader *nir, void *data)
{
   const struct vk_graphics_pipeline_state *state = data;
   nir_shader_intrinsics_pass(nir, kk_interlock_restore_sample_source,
      nir_metadata_control_flow, data);
   kk_lower_multisample_alpha(nir, state);
   if (state->cb)
      kk_lower_fs_blend(nir, state);
   nir_shader_intrinsics_pass(nir, kk_nir_swizzle_fragment_output,
      nir_metadata_control_flow, data);
}

/* The native fallback is compiled after the transaction has captured all raw
 * source components, including alpha. Limit only its physical attachment IO. */
static bool
kk_interlock_trim_native_output(nir_builder *b, nir_intrinsic_instr *intr,
   void *data)
{
   if (intr->intrinsic == nir_intrinsic_load_tile_image) {
      /* Selected draws consume the indexed shadow read in the transaction.
       * The native pipeline's current-sample IO is compiled separately. */
      b->cursor = nir_before_instr(&intr->instr);
      nir_def *value = nir_load_output(b, intr->def.num_components, intr->def.bit_size,
         nir_imm_int(b, 0), .component = nir_intrinsic_component(intr),
         .dest_type = nir_intrinsic_dest_type(intr),
         .io_semantics = nir_intrinsic_io_semantics(intr));
      nir_def_replace(&intr->def, value);
      return true;
   }
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   unsigned slot = nir_intrinsic_io_semantics(intr).location;
   if (slot < FRAG_RESULT_DATA0 || slot >= FRAG_RESULT_DATA0 + 8)
      return false;
   const struct vk_graphics_pipeline_state *state = data;
   unsigned logical = slot - FRAG_RESULT_DATA0;
   VkFormat format = kk_interlock_color_format(state, logical);
   if (format == VK_FORMAT_UNDEFINED)
      return false;
   unsigned count = util_format_get_nr_components(vk_format_to_pipe_format(format));
   unsigned first = nir_intrinsic_component(intr);
   if (first >= count) {
      nir_instr_remove(&intr->instr);
      return true;
   }
   unsigned components = MIN2(intr->num_components, count - first);
   unsigned mask = nir_intrinsic_write_mask(intr) & BITFIELD_MASK(components);
   if (!mask) {
      nir_instr_remove(&intr->instr);
      return true;
   }
   if (components == intr->num_components && mask == nir_intrinsic_write_mask(intr))
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_src_rewrite(&intr->src[0], nir_trim_vector(b, intr->src[0].ssa, components));
   intr->num_components = components;
   nir_intrinsic_set_write_mask(intr, mask);
   return true;
}

static VkResult
kk_interlock_compile_fragment(struct kk_device *dev, struct kk_shader *owner,
   nir_shader *original, const struct kk_shader *previous,
   const struct vk_graphics_pipeline_state *state,
   const struct vk_pipeline_robustness_state *robustness,
   bool sample_shading)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);
   struct kk_interlock_shader *shader = rzalloc(NULL, struct kk_interlock_shader);
   if (!shader)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   shader->references = 1;
   owner->interlock = shader;
   struct kk_interlock_render_nir render = {
      .parameters = kk_interlock_shader_parameters,
      .geometry = previous && previous->info.has_geometry ?
         &previous->info.polygon : NULL,
      .point_coord = kk_interlock_source_point_coord,
      .sample_shading = sample_shading,
      .lower_sample_outputs = kk_interlock_lower_sample_outputs,
      .sample_outputs_data = (void *)state,
   };
   shader->info.source_vertices_per_primitive = previous && previous->info.has_geometry ?
      mesa_vertices_per_prim(previous->info.gs.mode) : 3;
   kk_interlock_capture_layout(original, &render.inputs);
   shader->info.inputs = render.inputs;
   const struct vk_render_pass_state *rp = state->rp;
   for (unsigned i = 0; i < rp->color_attachment_count; ++i) {
      unsigned logical = kk_get_logical_color_att_index(state, i);
      if (logical != MESA_VK_ATTACHMENT_UNUSED)
         shader->info.color_format[logical] =
            vk_format_to_pipe_format(rp->color_attachment_formats[i]);
   }
   shader->info.depth = rp->depth_attachment_format != VK_FORMAT_UNDEFINED;
   shader->info.stencil = rp->stencil_attachment_format != VK_FORMAT_UNDEFINED;
   switch (rp->depth_attachment_format) {
   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_D16_UNORM_S8_UINT: shader->info.depth_bits = 16; break;
   case VK_FORMAT_X8_D24_UNORM_PACK32:
   case VK_FORMAT_D24_UNORM_S8_UINT: shader->info.depth_bits = 24; break;
   default: shader->info.depth_bits = 32; break;
   }
   unsigned counts = pdev->vk.properties.framebufferNoAttachmentsSampleCounts;
   unsigned max_samples = 1u << (util_last_bit(counts) - 1u);
   nir_builder capture = kk_interlock_capture_shader(original->options,
      &render, max_samples, NULL);
   kk_interlock_compile_program(dev, shader, KK_INTERLOCK_CAPTURE,
      capture.shader, state, robustness);

   nir_shader *transaction = nir_shader_clone(NULL, original);
   struct kk_interlock_framebuffer_nir framebuffer = {
      .transaction = {.render = &render},
      .depth_bits = shader->info.depth_bits,
   };
   memcpy(framebuffer.color_format, shader->info.color_format,
      sizeof(framebuffer.color_format));
   kk_interlock_wrap_framebuffer(transaction, &framebuffer);
   kk_interlock_compile_program(dev, shader, KK_INTERLOCK_TRANSACTION,
      transaction, state, robustness);

   struct kk_interlock_replay_info replay = {
      .depth = shader->info.depth, .stencil = shader->info.stencil,
   };
   memcpy(replay.color_format, shader->info.color_format, sizeof(replay.color_format));
   kk_interlock_compile_program(dev, shader, KK_INTERLOCK_REPLAY,
      kk_interlock_replay_shader(original->options, &render, &replay), state, robustness);
   kk_interlock_compile_program(dev, shader, KK_INTERLOCK_TILE_VERTEX,
      kk_interlock_tile_vertex_shader(original->options), state, robustness);
   kk_interlock_compile_program(dev, shader, KK_INTERLOCK_LOAD,
      kk_interlock_load_shader(original->options, &render, &replay), state, robustness);
   return VK_SUCCESS;
}

static void kk_msl_serialize(const struct msl_compile_data *, struct blob *);
static VkResult kk_msl_deserialize(struct blob_reader *, struct msl_compile_data *);

static void
kk_interlock_shader_serialize(const struct kk_shader *owner, struct blob *blob)
{
   if (!owner->info.interlock_envelope)
      return;
   blob_write_uint8(blob, owner->interlock != NULL);
   if (!owner->interlock)
      return;
   blob_write_bytes(blob, &owner->interlock->info, sizeof(owner->interlock->info));
   for (unsigned i = 0; i < KK_INTERLOCK_PROGRAM_COUNT; ++i)
      kk_msl_serialize(&owner->interlock->program[i], blob);
}

static VkResult
kk_interlock_shader_deserialize(struct kk_shader *owner, struct blob_reader *blob)
{
   if (!owner->info.interlock_envelope)
      return VK_SUCCESS;
   bool present = blob_read_uint8(blob);
   if (blob->overrun)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   if (!present)
      return VK_SUCCESS;
   struct kk_interlock_shader *shader = rzalloc(NULL, struct kk_interlock_shader);
   if (!shader)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   shader->references = 1;
   owner->interlock = shader;
   blob_copy_bytes(blob, &shader->info, sizeof(shader->info));
   if (blob->overrun)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   for (unsigned i = 0; i < KK_INTERLOCK_PROGRAM_COUNT; ++i) {
      struct msl_compile_data *program = &shader->program[i];
      VkResult result = kk_msl_deserialize(blob, program);
      ralloc_steal(shader, program->code);
      ralloc_steal(shader, program->entrypoint_name);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

#endif

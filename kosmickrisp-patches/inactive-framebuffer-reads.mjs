// SPDX-License-Identifier: MIT

/** Match framebuffer-fetch inputs to the real dynamic-rendering attachments. */
export const id = 'inactive-framebuffer-reads';

export function apply({edit}) {
  edit('src/kosmickrisp/compiler/msl_iomap.c', s => s
    .replace('   P(ctx, "struct FragmentIn {\\n");', `   /* Function constants remove inactive framebuffer inputs from the Metal
    * interface. Undefined constants preserve the fully active interface. */
   u_foreach_bit64(location, shader->info.outputs_read) {
      if (location < FRAG_RESULT_DATA0 || location > FRAG_RESULT_DATA7)
         continue;
      unsigned color = location - FRAG_RESULT_DATA0;
      P(ctx, "constant bool kk_color_enabled_%u [[function_constant(%u)]];\\n",
        color, color);
      P(ctx, "constant bool kk_color_present_%u = "
             "is_function_constant_defined(kk_color_enabled_%u) ? "
             "kk_color_enabled_%u : true;\\n", color, color, color);
   }
   P(ctx, "struct FragmentIn {\\n");`)
    .replace(`      P_IND(ctx, "%s%s %s [[%s]];\\n", type, vector_suffix,
            FS_OUTPUT_NAME[location], FS_OUTPUT_SEMANTIC[location]);`, `      if (location >= FRAG_RESULT_DATA0 && location <= FRAG_RESULT_DATA7) {
         P_IND(ctx, "%s%s %s [[%s, function_constant(kk_color_present_%u)]];\\n",
               type, vector_suffix, FS_OUTPUT_NAME[location],
               FS_OUTPUT_SEMANTIC[location], (unsigned)(location - FRAG_RESULT_DATA0));
      } else {
         P_IND(ctx, "%s%s %s [[%s]];\\n", type, vector_suffix,
               FS_OUTPUT_NAME[location], FS_OUTPUT_SEMANTIC[location]);
      }`)
    .replace(`         P_IND(ctx, "out.%s = in.%s;\\n", FS_OUTPUT_NAME[location],
               FS_OUTPUT_NAME[location]);`, `         if (location >= FRAG_RESULT_DATA0 && location <= FRAG_RESULT_DATA7) {
            struct io_slot_info info = ctx->outputs_info[location];
            P_IND(ctx, "out.%s = kk_color_present_%u ? in.%s : %s%s(0);\\n",
                  FS_OUTPUT_NAME[location], (unsigned)(location - FRAG_RESULT_DATA0),
                  FS_OUTPUT_NAME[location], alu_type_to_string(info.type),
                  vector_suffixes[info.num_components]);
         } else {
            P_IND(ctx, "out.%s = in.%s;\\n", FS_OUTPUT_NAME[location],
                  FS_OUTPUT_NAME[location]);
         }`)
    .replace(`   if (load_from_input) {
      P(ctx, "in.");`, `   if (load_from_input && ctx->shader->info.stage == MESA_SHADER_FRAGMENT &&
       location >= FRAG_RESULT_DATA0 && location <= FRAG_RESULT_DATA7) {
      struct io_slot_info info = ctx->outputs_info[location];
      P(ctx, "(kk_color_present_%u ? in.%s : %s%s(0))",
        location - FRAG_RESULT_DATA0, FS_OUTPUT_NAME[location],
        alu_type_to_string(info.type), vector_suffixes[info.num_components]);
      return;
   }
   if (load_from_input) {
      P(ctx, "in.");`));

  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s.replace(
    '   bool uses_per_draw_data;',
    '   bool uses_per_draw_data;\n   bool has_framebuffer_reads;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('               const struct kk_compile_options *opts)\n{\n   NIR_PASS(_, nir, kk_nir_lower_poly);',
      '               const struct kk_compile_options *opts,\n               bool *has_framebuffer_reads)\n{\n   NIR_PASS(_, nir, kk_nir_lower_poly);')
    .replace('   modify_nir_info(nir);',
      `   modify_nir_info(nir);
   if (has_framebuffer_reads)
      *has_framebuffer_reads = nir->info.stage == MESA_SHADER_FRAGMENT &&
                               nir->info.outputs_read != 0;`)
    .replace('   kk_compile_msl_options(pdev, nir, state, data, NULL);',
      '   kk_compile_msl_options(pdev, nir, state, data, NULL, NULL);')
    .replace('   kk_compile_msl_options(pdev, nir, state, &shader->msl_data[stage], opts);',
      '   kk_compile_msl_options(pdev, nir, state, &shader->msl_data[stage], opts,\n                          &shader->info.has_framebuffer_reads);')
    .replace('      info->vs.additional_stages_bits |= BITFIELD_BIT(stage);',
      `      if (stage == MESA_SHADER_FRAGMENT)
         info->has_framebuffer_reads = s->info.has_framebuffer_reads;
      info->vs.additional_stages_bits |= BITFIELD_BIT(stage);`)
    .replace('      mtl_new_render_pipeline(device->mtl_compiler_handle, pipeline_descriptor);',
      '      kk_attachment_compile_pipeline(device, vs, pipeline_descriptor);'));

  edit('src/kosmickrisp/vulkan/kk_unused_attachments.h', s => s
    .replace('#include "util/simple_mtx.h"', `#include "util/simple_mtx.h"

/* The retained descriptor stays unspecialized for later format variants. */
static mtl_render_pipeline_state *
kk_attachment_compile_pipeline(struct kk_device *device,
   struct kk_shader *shader, mtl_render_pipeline_descriptor *descriptor)
{
   if (!shader->info.has_framebuffer_reads)
      return mtl_new_render_pipeline(device->mtl_compiler_handle, descriptor);
   return mtl_new_render_pipeline_for_attachments(device->mtl_compiler_handle,
      descriptor, shader->info.vs.rt_formats, MAX_DRAW_BUFFERS, 0,
      shader->info.vs.sample_count, true);
}`)
    .replace('         descriptor, formats, MAX_DRAW_BUFFERS, disabled, samples);',
      '         descriptor, formats, MAX_DRAW_BUFFERS, disabled, samples,\n         shader->info.has_framebuffer_reads);'));
  edit('src/kosmickrisp/bridge/mtl_compiler.h', s => s.replace(
    '   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,\n   uint32_t samples);',
    '   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,\n   uint32_t samples, bool framebuffer_reads);'));
  edit('src/kosmickrisp/bridge/mtl_compiler.m', s => s
    .replace('#include <Metal/MTL4LibraryFunctionDescriptor.h>',
      '#include <Metal/MTL4LibraryFunctionDescriptor.h>\n#include <Metal/MTL4SpecializedFunctionDescriptor.h>')
    .replace('   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,\n   uint32_t samples)\n{',
      '   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,\n   uint32_t samples, bool framebuffer_reads)\n{')
    .replace('      copy.rasterSampleCount = samples;', `      copy.rasterSampleCount = samples;
      if (framebuffer_reads) {
         MTLFunctionConstantValues *values = [[MTLFunctionConstantValues new] autorelease];
         for (uint32_t i = 0; i < count; ++i) {
            bool active = formats[i] != MTL_PIXEL_FORMAT_INVALID;
            [values setConstantValue:&active type:MTLDataTypeBool atIndex:i];
         }
         MTL4SpecializedFunctionDescriptor *fragment =
            [[MTL4SpecializedFunctionDescriptor new] autorelease];
         fragment.functionDescriptor = copy.fragmentFunctionDescriptor;
         fragment.constantValues = values;
         copy.fragmentFunctionDescriptor = fragment;
      }`));
}

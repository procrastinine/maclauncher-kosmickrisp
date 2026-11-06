// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'sampler-emulation-helpers';
export const sourceFiles = ['kk-sampler-helper-flags.h','kk-sampler-helpers-nir.h','kk-sampler-helpers-msl.h'].map(n=>'kosmickrisp-patches/'+n);
export function apply({edit,edits}) {
  for(const name of ['kk-sampler-helper-flags.h','kk-sampler-helpers-msl.h'])edits.set('src/kosmickrisp/compiler/'+name,fs.readFileSync(new URL(name,import.meta.url),'utf8'));
  edits.set('src/kosmickrisp/vulkan/kk-sampler-helpers-nir.h',fs.readFileSync(new URL('kk-sampler-helpers-nir.h',import.meta.url),'utf8'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c',s=>s.replace(
    '#include "kk_sampler_minmax.h"',
    '#include "kk_sampler_minmax.h"\n#include "kk-sampler-helpers-nir.h"').replace(
    '      plane * sizeof(struct kk_sampled_image_descriptor);',
    '      plane * sizeof(struct kk_sampled_image_descriptor);\n\n   kk_sample_descriptor(b, tex, sampler, plane_offset_B, ctx);'));
  edit('src/kosmickrisp/vulkan/kk_shader.h',s=>s.replace(
    'bool kk_nir_lower_sampler_minmax(nir_shader *nir,',
    'bool kk_nir_lower_sampler_helpers(nir_shader *nir, uint32_t set_layout_count,\n   struct vk_descriptor_set_layout *const *set_layouts, bool minmax, bool custom);\n\nbool kk_nir_lower_sampler_minmax(nir_shader *nir,'));
  edit('src/kosmickrisp/vulkan/kk_shader.c',s=>s.replace(
    '   if ((features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX)) && pdev->info.gpu_apple_family < 10)\n      NIR_PASS(_, nir, kk_nir_lower_sampler_minmax, set_layout_count, set_layouts);\n\n   /* Fixed opaque-black borders also use this shared lowering. */\n   NIR_PASS(_, nir, kk_nir_lower_custom_border);',
    '   NIR_PASS(_, nir, kk_nir_lower_sampler_helpers, set_layout_count, set_layouts,\n      (features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX)) && pdev->info.gpu_apple_family < 10,\n      features & BITFIELD_BIT(KK_FEAT_CUSTOM_BORDER));'));
  edit('src/kosmickrisp/compiler/msl_private.h',s=>s.replace(
    '   struct hash_table *types;',
    '   struct hash_table *types;\n   struct hash_table *sampler_helpers;'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c',s=>s.replace(
    'static void\ntex_to_msl(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex)\n{',
    '#include "kk-sampler-helpers-msl.h"\n\nstatic void\ntex_to_msl(struct nir_to_msl_ctx *ctx, nir_tex_instr *tex)\n{\n   if (kk_sample_emit_call(ctx, tex)) return;').replace(
    '   msl_emit_io_blocks(&ctx, shader);',
    '   msl_emit_io_blocks(&ctx, shader);\n   kk_sample_emit_helpers(&ctx);').replace(
    '   _mesa_hash_table_destroy(ctx.types, NULL);',
    '   if (ctx.sampler_helpers) _mesa_hash_table_destroy(ctx.sampler_helpers, NULL);\n   _mesa_hash_table_destroy(ctx.types, NULL);'));
}

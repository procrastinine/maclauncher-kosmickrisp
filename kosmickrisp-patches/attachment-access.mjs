// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

/** Extend common color fetches with multisampling and dynamic array indices. */
export const id = 'attachment-access';
export const sourceFiles = [
  'kosmickrisp-patches/kosmickrisp-attachment-samples.h',
  'kosmickrisp-patches/kosmickrisp-attachment-device.h',
];
export function apply({base: sourceRoot, edits}) {
  const edit = (name, apply) => edits.set(name, apply(
    edits.get(name) ?? fs.readFileSync(path.join(sourceRoot, name), 'utf8')));
  const owned = name => fs.readFileSync(path.join(import.meta.dirname, name), 'utf8');
  edits.set('src/kosmickrisp/compiler/msl_attachment_samples.h', owned('kosmickrisp-attachment-samples.h'));
  edits.set('src/kosmickrisp/vulkan/kk_attachment_device.h', owned('kosmickrisp-attachment-device.h'));
  edit('src/kosmickrisp/compiler/msl_nir_lower_input_attachments.c', s => s
    .replace('   const struct vk_color_attachment_location_state *cal;',
             '   const struct vk_color_attachment_location_state *cal;\n   const struct vk_render_pass_state *rp;')
    .replace('   const struct vk_color_attachment_location_state *cal)',
             '   const struct vk_color_attachment_location_state *cal,\n   const struct vk_render_pass_state *rp)')
    .replace('         .cal = cal,', '         .cal = cal,\n         .rp = rp,')
    .replace('   /* Lower depth/stencil and multisample as texture reads due to Metal\n    * limitations. */',
             '   /* Lower the remaining depth/stencil, unmapped and other-sample\n    * reads through texture descriptors. */')
    .replace(/static bool\nlower_ia\([\s\S]*?\n}\n\nstatic bool\nlower_input_attachment/,
             '#include "msl_attachment_samples.h"\n\nstatic bool\nlower_ia(nir_builder *b, nir_intrinsic_instr *intrin,\n         const struct lower_ia_state *state)\n{\n   return msl_attachment_sample_fetch(b, intrin, state);\n}\n\nstatic bool\nlower_input_attachment'));
  edit('src/kosmickrisp/compiler/nir_to_msl.h', s => s
    .replace('struct vk_color_attachment_location_state;',
             'struct vk_color_attachment_location_state;\nstruct vk_render_pass_state;')
    .replace('   const struct vk_color_attachment_location_state *cal);',
             '   const struct vk_color_attachment_location_state *cal,\n   const struct vk_render_pass_state *rp);'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('msl_nir_lower_input_attachments(nir, state->ial, state->cal);',
             'msl_nir_lower_input_attachments(nir, state->ial, state->cal,\n                                      state->rp);'));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('static uint32_t\nkk_get_vk_version()',
             '#include "kk_attachment_device.h"\n\nstatic uint32_t\nkk_get_vk_version()')
    .replace('   kk_get_device_features(pdev, &supported_extensions, &supported_features);',
             '   kk_get_device_features(pdev, &supported_extensions, &supported_features);\n   kk_attachment_device_features(pdev, &supported_extensions, &supported_features);'));
}

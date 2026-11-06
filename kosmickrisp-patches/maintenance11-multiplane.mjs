// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'maintenance11-multiplane';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-maintenance11-multiplane.h'];
export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_maintenance11_multiplane.h',
    fs.readFileSync(new URL('./kosmickrisp-maintenance11-multiplane.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_maintenance11.h', s => s
    .replace('   /* Writable MSAA owns aux48/table and aux56/sample-layer metadata. */\n   if (kk_alias_has_writable_msaa(view))',
      `   /* Whole multi-plane views retain the primary/plane1/plane2 chain.
    * An individually selected plane is still an ordinary single-plane view. */
   if (view->plane_count != 1)
      return;
   /* Writable MSAA owns aux48/table and aux56/sample-layer metadata. */
   if (kk_alias_has_writable_msaa(view))`)
    .replace(/static unsigned\nkk_alias_sampled_offset\(const nir_tex_instr \*tex, unsigned offset\)\n\{[\s\S]*?\n\}\n/, ''));
  edit('src/kosmickrisp/vulkan/kk_descriptor_set.c', s => s
    .replace('#include "kk_maintenance11.h"',
      '#include "kk_maintenance11.h"\n#include "kk_maintenance11_multiplane.h"')
    .replace('         desc[plane].image_min_lod_uint16 = min_lod;',
      '         desc[plane].image_min_lod_uint16 = min_lod;\n         kk_alias_sampled_tag(&desc[plane], view, is_input_attachment);'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s
    .replace('#include "kk_maintenance11.h"',
      '#include "kk_maintenance11.h"\n#include "kk_maintenance11_multiplane.h"')
    .replace('      offs = kk_alias_sampled_offset(tex, offs);\n', '')
    .replace('         image_id_offset_B + offs, ctx);',
      `         image_id_offset_B + offs, ctx);
      resource_addr = kk_alias_sampled_address(b, tex,
         nir_src_as_deref(nir_src_for_ssa(texture)), resource_addr,
         image_plane_offset_B, offs, ctx);`));
}

// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';
export const id = 'image-sliced-view-of-3d';
export const sourceFiles = ['kosmickrisp-patches/image-sliced-view-of-3d.h'];
export function apply({base: sourceRoot, edits}) {
  const edit = (file, apply) => {
    const name = 'src/kosmickrisp/' + file;
    edits.set(name, apply(edits.get(name) ?? fs.readFileSync(path.join(sourceRoot, name), 'utf8')));
  };
  const header = fs.readFileSync(new URL('./image-sliced-view-of-3d.h', import.meta.url), 'utf8');
  edits.set('src/kosmickrisp/vulkan/kk_sliced_3d.h', header);
  edit('vulkan/kk_physical_device.c', s => s
    .replace('      .EXT_image_view_min_lod =', '      .EXT_image_sliced_view_of_3d = true,\n      .EXT_image_view_min_lod =')
    .replace('      .minLod = supported_extensions->EXT_image_view_min_lod,',
      '      .minLod = supported_extensions->EXT_image_view_min_lod,\n      .imageSlicedViewOf3D = true,'));
  edit('vulkan/kk_descriptor_set.c', s => s
    .replace('#include "kk_sampler.h"', '#include "kk_sampler.h"\n#define KK_SLICED_3D_DESCRIPTOR\n#include "kk_sliced_3d.h"')
    .replace('      desc.image_gpu_resource_id = view->planes[plane].storage_gpu_resource_id;',
      '      desc.image_gpu_resource_id = view->planes[plane].storage_gpu_resource_id;\n      kk_sliced_3d_descriptor(&desc, &view->vk);'));
  edit('vulkan/kk_shader.c', s => s
    .replace('#include "nir_builder.h"', '#include "nir_builder.h"\n#include "kk_sliced_3d.h"')
    .replace('   /* KK_WORKAROUND_16 - must be called after kk_nir_lower_descriptors(),',
      '   NIR_PASS(_, nir, kk_nir_lower_sliced_3d);\n\n   /* KK_WORKAROUND_16 - must be called after kk_nir_lower_descriptors(),'));
}

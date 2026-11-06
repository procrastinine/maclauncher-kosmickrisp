// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'maintenance11';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-maintenance11.h'];

export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_maintenance11.h',
    fs.readFileSync(new URL('./kosmickrisp-maintenance11.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .KHR_maintenance9 = true,',
      '      .KHR_maintenance9 = true,\n      .KHR_maintenance11 = true,')
    .replace('      .maintenance9 = true,',
      '      .maintenance9 = true,\n\n      /* VK_KHR_maintenance11 */\n      .maintenance11 = true,'));
  edit('src/kosmickrisp/vulkan/kk_descriptor_types.h', s => s
    .replace('   uint32_t texel_offset;\n   uint32_t texel_count;',
      `   union {
      struct { uint32_t texel_offset, texel_count; };
      uint64_t image_array_gpu_resource_id;
   };`)
    .replace('static_assert(sizeof(struct kk_storage_image_descriptor) == 16,',
      `static_assert(offsetof(struct kk_storage_image_descriptor, image_array_gpu_resource_id) ==
              sizeof(uint64_t),
              "2D array handles and texel/3D metadata occupy disjoint view classes");
static_assert(offsetof(struct kk_sampled_image_descriptor, aux_resource_id) == 48,
              "array aliasing retains the sampled descriptor auxiliary ABI");
static_assert(sizeof(struct kk_storage_image_descriptor) == 16,`));
  edit('src/kosmickrisp/vulkan/kk_image_view.h', s => s.replace(
    '      uint64_t sampled_gpu_resource_id;',
    `      mtl_texture *mtl_handle_sampled_alias;
      mtl_texture *mtl_handle_storage_alias;
      uint64_t sampled_alias_gpu_resource_id;
      uint64_t storage_alias_gpu_resource_id;
      uint64_t sampled_gpu_resource_id;`));
  edit('src/kosmickrisp/vulkan/kk_image_view.c', s => s
    .replace('#include "vk_format.h"',
      '#include "vk_format.h"\n#define KK_MAINTENANCE11_VIEW\n#include "kk_maintenance11.h"')
    .replace('   }\n\n   return VK_SUCCESS;\n}',
      `      VkResult alias_result = kk_image_view_init_alias(dev, view, view_plane,
                                                       mtl_handle, &view_layout);
      if (alias_result != VK_SUCCESS) {
         kk_image_view_finish(dev, view);
         return alias_result;
      }
   }

   return VK_SUCCESS;
}`)
    .replace('      if (view->planes[plane].mtl_handle_sampled)',
      '      kk_image_view_finish_alias(view, plane);\n      if (view->planes[plane].mtl_handle_sampled)'));
  edit('src/kosmickrisp/vulkan/kk_descriptor_set.c', s => s
    .replace('#include "kk_sampler.h"',
      '#include "kk_sampler.h"\n#define KK_MAINTENANCE11_DESCRIPTOR\n#include "kk_maintenance11.h"')
    .replace('         float min_lod = MAX2',
      '         kk_alias_sampled_descriptor(&desc[plane], view, plane, is_input_attachment);\n         float min_lod = MAX2')
    .replace('      desc.image_gpu_resource_id = view->planes[plane].storage_gpu_resource_id;',
      '      desc.image_gpu_resource_id = view->planes[plane].storage_gpu_resource_id;\n      kk_alias_storage_descriptor(&desc, view);'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s
    .replace('static bool\nlower_image_intrin(',
      '#include "kk_maintenance11.h"\n\nstatic bool\nlower_image_intrin(')
    .replace('   nir_rewrite_image_intrinsic(intr, handle, nir_image_intrinsic_type_bindless);',
      `   nir_rewrite_image_intrinsic(intr, handle, nir_image_intrinsic_type_bindless);
   if (kk_alias_lower_array_atomic(b, intr, deref, ctx, type,
                                  nir_intrinsic_access(intr) | access,
                                  msl_convert_access_flag(access)))
      return true;`)
    .replace('      nir_def *resource_addr = load_resource_addr(\n         b, 1, 64, nir_src_as_deref(nir_src_for_ssa(texture)),',
      '      offs = kk_alias_sampled_offset(tex, offs);\n      nir_def *resource_addr = load_resource_addr(\n         b, 1, 64, nir_src_as_deref(nir_src_for_ssa(texture)),'));
}

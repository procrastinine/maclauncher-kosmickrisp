// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'non-seamless-cube';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-non-seamless-cube.h'];
export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_descriptor_types.h', (s, replace) => replace(
    '   uint64_t aux_word;\n};',
    `   uint64_t aux_word;
};

#define KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE BITFIELD_BIT(2)
static_assert((KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE & 3u) == 0,
              "cube flag does not overlap the two linear-filter bits");
static_assert(KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE <= UINT16_MAX,
              "cube flag fits existing sampler filter metadata");
static_assert(KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE ==
              VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT,
              "sampler cube flag retains its Vulkan bit encoding");`));
  edit('src/kosmickrisp/vulkan/kk_sampler.c', (s, replace) => replace(
    '#include "kk_sampler.h"',
    '#include "kk_sampler.h"\n#include "kk_descriptor_types.h"').replace(
    '   sampler->plane_count = 1;',
    `   sampler->plane_count = 1;
   /* The filter metadata uses bits zero and one; bit two carries the
    * cube-only flag without increasing the sampler allocation. */
   sampler->filter_bits |=
      (pCreateInfo->flags & VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT) ?
         KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE : 0;
`));
  edit('src/kosmickrisp/vulkan/kk_image_view.h', (s, replace) => replace(
    '      uint64_t sampled_gpu_resource_id;',
    `      mtl_texture *mtl_handle_cube_face;
      uint64_t cube_face_gpu_resource_id;
      uint64_t sampled_gpu_resource_id;`));
  edit('src/kosmickrisp/vulkan/kk_image_view.c', (s, replace) => replace(
    '      if (view->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT) {',
    `      if ((view->vk.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
          dev->vk.enabled_features.nonSeamlessCubeMap &&
          (view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
           view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
         struct kk_view_layout face_layout = view_layout;
         face_layout.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
         view->planes[view_plane].mtl_handle_cube_face =
            mtl_new_texture_view_with(mtl_handle, &face_layout);
         view->planes[view_plane].cube_face_gpu_resource_id =
            mtl_texture_get_gpu_resource_id(view->planes[view_plane].mtl_handle_cube_face);
      }

      if (view->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT) {`).replace(
    '      if (view->planes[plane].mtl_handle_sampled)',
    `      if (view->planes[plane].mtl_handle_cube_face)
         mtl_release(view->planes[plane].mtl_handle_cube_face);
      if (view->planes[plane].mtl_handle_sampled)`));
  edit('src/kosmickrisp/vulkan/kk_descriptor_set.c', (s, replace) => replace(
    '         float min_lod = MAX2(view->vk.min_lod - view->vk.base_mip_level, 0.0);',
    `         if (view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
             view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
            assert(plane_count == 1 && plane == 0);
            desc[plane].aux_resource_id =
               view->planes[plane].cube_face_gpu_resource_id;
         }
         float min_lod = MAX2(view->vk.min_lod - view->vk.base_mip_level, 0.0);`));
  edit('src/kosmickrisp/vulkan/kk_shader.h', (s, replace) => replace(
    '   KK_FEAT_COUNT,', '   KK_FEAT_NON_SEAMLESS_CUBE,\n   KK_FEAT_COUNT,').replace(
    'bool kk_nir_lower_sampler_minmax(',
    `bool kk_nir_lower_non_seamless_cube(nir_shader *nir,
      uint32_t set_layout_count,
      struct vk_descriptor_set_layout *const *set_layouts);

bool kk_nir_lower_sampler_minmax(`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', (s, replace) => replace(
    '   if (feats->samplerFilterMinmax)',
    `   if (feats->nonSeamlessCubeMap)
      key |= BITFIELD_BIT(KK_FEAT_NON_SEAMLESS_CUBE);
   if (feats->samplerFilterMinmax)`).replace(
    '   NIR_PASS(_, nir, msl_lower_textures, pdev->info.gpu_apple_family < 10);',
    `   if (features & BITFIELD_BIT(KK_FEAT_NON_SEAMLESS_CUBE))
      NIR_PASS(_, nir, kk_nir_lower_non_seamless_cube, set_layout_count, set_layouts);

   NIR_PASS(_, nir, msl_lower_textures, pdev->info.gpu_apple_family < 10);`));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', (s, replace) => replace(
    '#include "kk_sampler_minmax.h"',
    '#include "kk_sampler_minmax.h"\n#include "kk_non_seamless_cube.h"').replace(
    '   const uint64_t plane_offset_B =',
    `   const nir_deref_instr *texture_deref =
      nir_src_as_deref(nir_src_for_ssa(texture));
   const bool cube_face = tex->sampler_dim == GLSL_SAMPLER_DIM_2D &&
      glsl_get_sampler_dim(glsl_without_array(texture_deref->type)) == GLSL_SAMPLER_DIM_CUBE;
   const uint64_t plane_offset_B =`).replace(
    `      unsigned offs =
         offsetof(struct kk_sampled_image_descriptor, image_gpu_resource_id);

      nir_def *resource_addr = load_resource_addr(`,
    `      unsigned offs = (cube_face)
         ? offsetof(struct kk_sampled_image_descriptor, aux_resource_id)
         : offsetof(struct kk_sampled_image_descriptor, image_gpu_resource_id);

      nir_def *resource_addr = load_resource_addr(`));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', (s, replace) => replace(
    '      .EXT_custom_border_color =',
    '      .EXT_non_seamless_cube_map = true,\n      .EXT_custom_border_color =').replace(
    '      .shaderInt64 = true,',
    '      .shaderInt64 = true,\n      .nonSeamlessCubeMap = true,'));
  edits.set('src/kosmickrisp/vulkan/kk_non_seamless_cube.h',
    fs.readFileSync(new URL('./kosmickrisp-non-seamless-cube.h', import.meta.url), 'utf8'));
}

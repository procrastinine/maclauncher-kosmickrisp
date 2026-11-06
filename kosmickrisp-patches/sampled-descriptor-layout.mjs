// SPDX-License-Identifier: MIT
export const id = 'sampled-descriptor-layout';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_descriptor_types.h', s => s
    .replace('   uint64_t pad_to_power_2[2];',
      '   uint64_t aux_resource_id;\n   uint64_t aux_word;')
    .replace('static_assert(sizeof(struct kk_sampled_image_descriptor) == 64,',
      `static_assert(offsetof(struct kk_sampled_image_descriptor, aux_resource_id) == 48,
              "sampled image auxiliary resource ID offset");
static_assert(offsetof(struct kk_sampled_image_descriptor, aux_word) == 56,
              "sampled image auxiliary word offset");
static_assert(sizeof(struct kk_sampled_image_descriptor) == 64,`));
}

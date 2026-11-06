// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

/** Keep zero-initialized image storage and capability reporting isolated. */
export const id = 'zero-initialize-device-memory';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-zero-init.h'];
export function apply({base: sourceRoot, edits}) {
  const edit = (file, apply) => {
    const name = 'src/kosmickrisp/vulkan/' + file;
    const before = edits.get(name) ?? fs.readFileSync(path.join(sourceRoot, name), 'utf8');
    const replace = (s, from, to) => {
      if (!s.includes(from)) throw new Error(`${name}: missing zero-initialize anchor`);
      return s.replace(from, to);
    };
    edits.set(name, apply(before, replace));
  };
  const header = 'src/kosmickrisp/vulkan/kk_zero_init.h';
  edits.set(header, fs.readFileSync(new URL('./kosmickrisp-zero-init.h', import.meta.url), 'utf8'));
  edit('kk_physical_device.c', (s, replace) => {
    s = replace(s, '#include "kk_physical_device.h"', '#include "kk_physical_device.h"\n#include "kk_zero_init.h"');
    return replace(s, '   kk_get_device_features(pdev, &supported_extensions, &supported_features);',
      `   kk_get_device_features(pdev, &supported_extensions, &supported_features);
   kk_zero_init_features(&supported_extensions, &supported_features);`);
  });
  edit('kk_image.c', (s, replace) => {
    return replace(s, `      kk_image_layout_init(dev, &image->vk, vk_format_to_pipe_format(format),
                           width_scale, height_scale,
                           &image->planes[plane].layout);`,
      `      kk_image_layout_init(dev, &image->vk,
         vk_format_to_pipe_format(format), width_scale, height_scale,
         &image->planes[plane].layout, pCreateInfo->initialLayout);`);
  });
  edit('kk_image_layout.h', (s, replace) => replace(s,
    '                          struct kk_image_layout *layout);',
    '                          struct kk_image_layout *layout,\n                          VkImageLayout initial_layout);'));
  edit('kk_image_layout.c', (s, replace) => {
    s = replace(s, '#include "kk_image_layout.h"', '#include "kk_image_layout.h"\n#include "kk_zero_init.h"');
    s = replace(s, '                     const uint8_t height_scale, struct kk_image_layout *layout)',
      '                     const uint8_t height_scale, struct kk_image_layout *layout,\n                     VkImageLayout initial_layout)');
    return replace(s, `   layout->optimized_layout = kk_image_layout_can_optimize(
      pdev, usage, image->tiling, image->create_flags, format);`,
      `   layout->optimized_layout = kk_zero_init_optimized_layout(
      kk_image_layout_can_optimize(pdev, usage, image->tiling,
                                    image->create_flags, format),
      initial_layout);`);
  });
}

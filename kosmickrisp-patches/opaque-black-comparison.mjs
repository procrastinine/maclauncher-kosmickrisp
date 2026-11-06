// SPDX-License-Identifier: MIT
export const id = 'opaque-black-comparison';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_sampler.c', (s, replace) => replace(
    '                        is_border_color_custom(pCreateInfo->borderColor);',
    `                        is_border_color_custom(pCreateInfo->borderColor) &&
                        /* RGBA4 needs the colour workaround. Shadow
                         * instructions must retain the native depth border. */
                        !(pCreateInfo->compareEnable &&
                          pCreateInfo->borderColor ==
                             VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);`));
}

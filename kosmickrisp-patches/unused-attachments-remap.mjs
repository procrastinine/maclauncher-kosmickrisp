// SPDX-License-Identifier: MIT

/** Preserve live remapped outputs when unused format slots share a location. */
export const id = 'unused-attachments-remap';

export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_shader.c', (source, replace) => replace(
    '   for (unsigned i = 0; i < ARRAY_SIZE(opts.rt); ++i) {\n      uint8_t logical_index = kk_get_logical_color_att_index(state, i);',
    `   /* Only declared physical attachments have formats to map. Identity
    * entries beyond this range may alias a live remapped output and must not
    * overwrite its format before unused-output removal. */
   for (unsigned i = 0; i < state->rp->color_attachment_count; ++i) {
      uint8_t logical_index = kk_get_logical_color_att_index(state, i);`));
}

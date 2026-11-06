// SPDX-License-Identifier: MIT
// The upload BO cache count follows its list through creation and trim.
export const id = 'command-upload-pool-count';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_pool.c', source => source.replaceAll(
    '   list_inithead(&pool->free_bos);',
    '   list_inithead(&pool->free_bos);\n   pool->num_free_bos = 0;'));
}

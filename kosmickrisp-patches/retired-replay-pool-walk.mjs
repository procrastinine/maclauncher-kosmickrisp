// SPDX-License-Identifier: MIT
// Replay buffers are reset individually before joining the free list.
export const id = 'retired-replay-pool-walk';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_queue.c', source => source.replace(
    '      kk_ResetCommandPool(kk_device_to_handle(dev),\n' +
    '                          kk_cmd_pool_to_handle(queue->cmd_pool), 0u);',
    '      /* Every private replay buffer was already reset before returning\n' +
    '       * to the free list; a zero-flag pool walk adds no cleanup. */'));
}

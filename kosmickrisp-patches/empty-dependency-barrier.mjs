// SPDX-License-Identifier: MIT
// An empty synchronization2 dependency has no stage or access scopes.
export const id = 'empty-dependency-barrier';

export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', (_source, replace) => replace(
`kk_CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                       const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);`,
`kk_CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                       const VkDependencyInfo *pDependencyInfo)
{
   /* Synchronization2 carries execution scopes in the barrier elements. An
    * entirely empty dependency needs neither an encoder barrier nor a render
    * pass restart. Keep extension chains and dependency flags on the existing
    * path, and preserve barriers with stage scopes but empty access masks. */
   if (pDependencyInfo->pNext == NULL &&
       pDependencyInfo->dependencyFlags == 0 &&
       pDependencyInfo->memoryBarrierCount == 0 &&
       pDependencyInfo->bufferMemoryBarrierCount == 0 &&
       pDependencyInfo->imageMemoryBarrierCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);`));
}

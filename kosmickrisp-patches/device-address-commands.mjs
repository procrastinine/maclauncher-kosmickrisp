// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'device-address-commands';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-address-copy.h'];

export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .KHR_maintenance10 = true,',
      '      .KHR_maintenance10 = true,\n      .KHR_device_address_commands = true,')
    .replace('      .maintenance10 = true,',
      '      .maintenance10 = true,\n\n      /* VK_KHR_device_address_commands */\n      .deviceAddressCommands = true,'));

  edit('src/kosmickrisp/vulkan/kk_cmd_meta.c', s => s.replace(
    '   buffer->metal.handle = buf.buffer;',
    '   buffer->metal.handle = buf.buffer;\n   buffer->metal.offset = buf.offset;'));

  // Handle-based copies use Mesa's common address conversion, just as fill
  // and update already do. All three share the existing meta implementation.
  edit('src/kosmickrisp/vulkan/kk_cmd_copy.c', s => s.replace(
    /VKAPI_ATTR void VKAPI_CALL\nkk_CmdCopyBuffer2\([\s\S]*?\n}\n\nstruct kk_buffer_image_copy_info/,
    'struct kk_buffer_image_copy_info'));
  edit('src/kosmickrisp/vulkan/kk_cmd_meta.c', s => s.replace(
    'VKAPI_ATTR void VKAPI_CALL\nkk_CmdFillMemoryKHR',
    `VKAPI_ATTR void VKAPI_CALL
kk_CmdCopyMemoryKHR(VkCommandBuffer commandBuffer,
                    const VkCopyDeviceMemoryInfoKHR *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_meta_save save;
   kk_meta_begin(cmd, &save, VK_PIPELINE_BIND_POINT_COMPUTE,
                 "meta:vkCmdCopyMemoryKHR");
   vk_meta_copy_memory(&cmd->vk, &dev->meta, info);
   kk_meta_end(cmd, &save, VK_PIPELINE_BIND_POINT_COMPUTE);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdFillMemoryKHR`));

  edits.set('src/kosmickrisp/vulkan/kk_address_copy.h',
    fs.readFileSync(new URL('./kosmickrisp-address-copy.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_cmd_copy.c', s => s + '\n#include "kk_address_copy.h"\n');

  // Address range barriers have the same access semantics as buffer barriers.
  // Metal uses encoder-wide synchronization, independent of resource handles.
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s.replace(
    '   if (src & any_write_access)',
    `   const VkMemoryRangeBarriersInfoKHR *ranges = vk_find_struct_const(
      dep->pNext, MEMORY_RANGE_BARRIERS_INFO_KHR);
   for (uint32_t i = 0; ranges && i < ranges->memoryRangeBarrierCount; ++i) {
      src |= ranges->pMemoryRangeBarriers[i].srcAccessMask;
      dst |= ranges->pMemoryRangeBarriers[i].dstAccessMask;
   }

   if (src & any_write_access)`));

  // The exposed transform-feedback extension also needs the address variants.
  // Common Vulkan adapters keep the original handle commands on this path.
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace(`kk_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer commandBuffer,
   uint32_t firstBinding, uint32_t bindingCount, const VkBuffer *pBuffers,
   const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes)`,
      `kk_CmdBindTransformFeedbackBuffers2EXT(VkCommandBuffer commandBuffer,
   uint32_t firstBinding, uint32_t bindingCount,
   const VkBindTransformFeedbackBuffer2InfoEXT *bindings)`)
    .replace(`      VK_FROM_HANDLE(kk_buffer, buffer, pBuffers[i]);
      cmd->state.gfx.xfb[firstBinding + i] = kk_buffer_addr_range(buffer,
         pOffsets[i], pSizes ? pSizes[i] : VK_WHOLE_SIZE);`,
      `      cmd->state.gfx.xfb[firstBinding + i] = (struct kk_addr_range){
         .addr = bindings[i].addressRange.address,
         .range = bindings[i].addressRange.size,
      };`)
    .replace('   uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets, bool begin)',
      '   uint32_t count, const VkBindTransformFeedbackBuffer2InfoEXT *buffers, bool begin)')
    .replace('                      buffers[slot - first] != VK_NULL_HANDLE;',
      '                      buffers[slot - first].addressRange.address != 0;')
    .replace(`         VK_FROM_HANDLE(kk_buffer, buffer, buffers[slot - first]);
         source = vk_buffer_address(&buffer->vk, offsets ? offsets[slot - first] : 0);`,
      '         source = buffers[slot - first].addressRange.address;')
    .replace(`kk_CmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets)`,
      `kk_CmdBeginTransformFeedback2EXT(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBindTransformFeedbackBuffer2InfoEXT *buffers)`)
    .replace('   kk_begin_end_xfb(commandBuffer, first, count, buffers, offsets, true);',
      '   kk_begin_end_xfb(commandBuffer, first, count, buffers, true);')
    .replace(`kk_CmdEndTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets)`,
      `kk_CmdEndTransformFeedback2EXT(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBindTransformFeedbackBuffer2InfoEXT *buffers)`)
    .replace('   kk_begin_end_xfb(commandBuffer, first, count, buffers, offsets, false);',
      '   kk_begin_end_xfb(commandBuffer, first, count, buffers, false);')
    .replace(`kk_CmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer,
   uint32_t instanceCount, uint32_t firstInstance, VkBuffer counterBuffer,
   VkDeviceSize counterBufferOffset, uint32_t counterOffset, uint32_t vertexStride)`,
      `kk_CmdDrawIndirectByteCount2EXT(VkCommandBuffer commandBuffer,
   uint32_t instanceCount, uint32_t firstInstance,
   const VkBindTransformFeedbackBuffer2InfoEXT *counter,
   uint32_t counterOffset, uint32_t vertexStride)`)
    .replace('   VK_FROM_HANDLE(kk_buffer, counter, counterBuffer);\n', '')
    .replace('      vk_buffer_address(&counter->vk, counterBufferOffset), draw,',
      '      counter->addressRange.address, draw,'));
}

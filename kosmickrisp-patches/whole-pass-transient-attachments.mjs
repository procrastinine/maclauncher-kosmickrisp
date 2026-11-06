// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'whole-pass-transient-attachments';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-whole-pass.h',
  'kosmickrisp-patches/kosmickrisp-transient.h'];
export function apply({edit, edits}) {
  const change = (file, from, to) =>
    edit('src/kosmickrisp/' + file, (_s, replace) => replace(from, to));
  edits.set('src/kosmickrisp/vulkan/kk_whole_pass.h',
    fs.readFileSync(new URL('./kosmickrisp-whole-pass.h', import.meta.url), 'utf8'));
  change('vulkan/kk_device.h',
    '   struct vk_device_dispatch_table exposed_dispatch_table;',
    '   struct vk_device_dispatch_table exposed_dispatch_table;\n\n' +
    '   PFN_vkCmdBeginRendering whole_pass_begin_rendering;\n' +
    '   PFN_vkCmdEndRendering2KHR whole_pass_end_rendering;');
  change('vulkan/kk_device.c',
    '   dev->vk.command_dispatch_table = &dev->vk.dispatch_table;',
    '   dev->whole_pass_begin_rendering = dev->vk.dispatch_table.CmdBeginRendering;\n' +
    '   dev->whole_pass_end_rendering = dev->vk.dispatch_table.CmdEndRendering2KHR;\n' +
    '   dev->vk.dispatch_table.CmdBeginRendering = kk_whole_pass_begin_rendering;\n' +
    '   dev->vk.dispatch_table.CmdEndRendering = kk_whole_pass_end_rendering;\n' +
    '   dev->vk.dispatch_table.CmdEndRendering2KHR = kk_whole_pass_end_rendering2;\n' +
    '   dev->vk.command_dispatch_table = &dev->vk.dispatch_table;');
  change('vulkan/kk_cmd_buffer.h', 'struct kk_cmd_buffer {',
    'struct kk_whole_pass_state {\n' +
    '   bool suspended;\n   VkRect2D area;\n' +
    '   uint32_t layers, view_mask, color_count;\n' +
    '   VkImageView colors[KK_MAX_RTS], depth, stencil;\n' +
    '   VkSampleCountFlagBits implicit_samples;\n};\n\n' +
    'struct kk_cmd_buffer {');
  change('vulkan/kk_cmd_buffer.h', '   struct vk_command_buffer vk;',
    '   struct vk_command_buffer vk;\n   struct kk_whole_pass_state whole_pass;');
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => {
    const declarations = '\nvoid kk_whole_pass_flush_suspended(struct kk_cmd_buffer *cmd);\n' +
      'bool kk_whole_pass_local_barrier(const VkDependencyInfo *dep);\n' +
      'void kk_init_rendering_locations(struct kk_cmd_buffer *cmd, const VkRenderingInfo *info);\n' +
      'VKAPI_ATTR void VKAPI_CALL kk_whole_pass_begin_rendering(VkCommandBuffer, const VkRenderingInfo *);\n' +
      'VKAPI_ATTR void VKAPI_CALL kk_whole_pass_end_rendering(VkCommandBuffer);\n' +
      'VKAPI_ATTR void VKAPI_CALL kk_whole_pass_end_rendering2(VkCommandBuffer, const VkRenderingEndInfoKHR *);\n';
    return s.replace('#endif', declarations + '\n#endif');
  });
  change('vulkan/kk_cmd_buffer.h',
    'bool kk_whole_pass_local_barrier(const VkDependencyInfo *dep);',
    'VkAccessFlags2 kk_whole_pass_attachment_access(VkAccessFlags2, VkPipelineStageFlags2);\n' +
    'bool kk_whole_pass_attachment_barrier(const VkDependencyInfo *dep);\n' +
    'bool kk_whole_pass_local_barrier(const VkDependencyInfo *dep);');
  for (const type of ['Memory', 'BufferMemory', 'ImageMemory']) {
    const member = 'dep->p' + type + 'Barriers[i]';
    for (const side of ['src', 'dst'])
      change('vulkan/kk_cmd_buffer.c',
        '      ' + side + ' |= ' + member + '.' + side + 'AccessMask;',
        '      ' + side + ' |= kk_whole_pass_attachment_access(' + member +
        '.' + side + 'AccessMask, ' + member + '.' + side + 'StageMask);');
  }
  change('vulkan/kk_cmd_buffer.c',
    /      \/\* Multisample attachments require[\s\S]*?       \*\/\n/,
    '      /* Framebuffer-local dependencies remain on tile. Texture visibility\n' +
    '       * still uses the shared encoder-split decision. */\n');
  change('vulkan/kk_cmd_buffer.c', '   cmd->uses_heap = false;',
    '   cmd->uses_heap = false;\n   memset(&cmd->whole_pass, 0, sizeof(cmd->whole_pass));');
  change('vulkan/kk_cmd_buffer.c', '   assert(cmd);\n\n   /* Render first',
    '   assert(cmd);\n   kk_whole_pass_flush_suspended(cmd);\n\n   /* Render first');
  change('vulkan/kk_cmd_buffer.c',
    '      if (cmd->state.gfx.render.samples > 1 ||\n          kk_barrier_requires_encoder_split(cmd, pDependencyInfo)) {',
    '      const bool local = kk_whole_pass_local_barrier(pDependencyInfo);\n' +
    '      if (kk_barrier_requires_encoder_split(cmd, pDependencyInfo) ||\n' +
    '          (cmd->state.gfx.render.samples > 1 && !local)) {');
  change('vulkan/kk_cmd_buffer.c',
    '         mtl_barrier_after_encoder_stages(cmd->metal.render, MTL_STAGE_VERTEX,\n                                          MTL_STAGE_FRAGMENT);',
    '         if (!kk_whole_pass_attachment_barrier(pDependencyInfo))\n' +
    '            mtl_barrier_after_encoder_stages(cmd->metal.render,\n' +
    '               local ? MTL_STAGE_FRAGMENT : MTL_STAGE_VERTEX, MTL_STAGE_FRAGMENT);');
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s + '\n#include "kk_whole_pass.h"\n');
  const locations = '   const VkRenderingAttachmentLocationInfoKHR ral_info = {\n' +
    '      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR,\n' +
    '      .colorAttachmentCount = pRenderingInfo->colorAttachmentCount,\n' +
    '   };\n   vk_cmd_set_rendering_attachment_locations(&cmd->vk, &ral_info);';
  change('vulkan/kk_cmd_draw.c', locations, '   kk_init_rendering_locations(cmd, pRenderingInfo);');
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s +
    '\nvoid\nkk_init_rendering_locations(struct kk_cmd_buffer *cmd, const VkRenderingInfo *pRenderingInfo)\n{\n' +
    locations + '\n}\n');
  // Retain the existing arena-backed command IR for one-time buffers too.
  // Ordinary buffers encode at EndCommandBuffer; suspended scopes encode at
  // submission, where command-buffer order is known.
  change('util/kk_dispatch_cmd_gen.py',
    /    if \(cmd->enqueue_commands\) \{\n      vk_cmd_enqueue_device_entrypoints\.(.*);\n      return;\n    \}\n\n    dev->vk\.dispatch_table\.\1;\n    if \(!cmd->one_time_submit\) \{\n      vk_cmd_enqueue_device_entrypoints\.\1;\n    \}/,
    '    vk_cmd_enqueue_device_entrypoints.$1;');
  change('util/kk_dispatch_cmd_gen.py',
    /    if \(cmd->enqueue_commands\) \{\n      return vk_cmd_enqueue_device_entrypoints\.(.*);\n    \}\n\n    VkResult result = dev->vk\.dispatch_table\.\1;\n    if \(result != VK_SUCCESS\)\n      return result;\n\n    if \(!cmd->one_time_submit\) \{\n      result = vk_cmd_enqueue_device_entrypoints\.\1;\n    \}\n    return result;/,
    '    return vk_cmd_enqueue_device_entrypoints.$1;');
  change('vulkan/kk_cmd_buffer.h',
    '   struct kk_whole_pass_state whole_pass;',
    '   struct kk_whole_pass_state whole_pass;\n   bool deferred_render_chain;');
  change('vulkan/kk_cmd_buffer.h',
    'void kk_whole_pass_flush_suspended(struct kk_cmd_buffer *cmd);',
    'bool kk_whole_pass_needs_submit(struct kk_cmd_buffer *cmd);\n' +
    'void kk_whole_pass_command_boundary(struct kk_cmd_buffer *cmd);\n' +
    'void kk_whole_pass_flush_suspended(struct kk_cmd_buffer *cmd);');
  change('vulkan/kk_cmd_buffer.c', '#include "vk_common_entrypoints.h"',
    '#include "vk_common_entrypoints.h"\n#include "vk_cmd_queue.h"');
  change('vulkan/kk_cmd_buffer.c',
    '   memset(&cmd->whole_pass, 0, sizeof(cmd->whole_pass));',
    '   memset(&cmd->whole_pass, 0, sizeof(cmd->whole_pass));\n' +
    '   cmd->deferred_render_chain = false;');
  change('vulkan/kk_cmd_buffer.c',
    '   end_recording(cmd);\n\n   return vk_command_buffer_end(&cmd->vk);',
    '   if (cmd->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY &&\n' +
    '       cmd->vk.record_result == VK_SUCCESS) {\n' +
    '      cmd->deferred_render_chain = kk_whole_pass_needs_submit(cmd);\n' +
    '      if (!cmd->enqueue_commands && !cmd->deferred_render_chain)\n' +
    '         vk_cmd_queue_execute(&cmd->vk.cmd_queue, commandBuffer,\n' +
    '                              &kk_cmd_buffer_device(cmd)->vk.dispatch_table);\n' +
    '   }\n' +
    '   end_recording(cmd);\n\n   return vk_command_buffer_end(&cmd->vk);');

  change('vulkan/kk_queue.c',
    'rerecord_and_commit_cmd_buffer(struct kk_queue *queue,\n                               struct kk_cmd_buffer *cmd)',
    'rerecord_and_commit_cmd_buffers(struct kk_queue *queue,\n' +
    '                                struct vk_command_buffer **commands,\n' +
    '                                uint32_t count, uint32_t *consumed, bool *committed)');
  change('vulkan/kk_queue.c',
    '   vk_cmd_queue_execute(&cmd->vk.cmd_queue, rerecord_handle,\n                        &dev->vk.dispatch_table);',
    '   for (*consumed = 0; *consumed < count; (*consumed)++) {\n' +
    '      struct kk_cmd_buffer *source =\n' +
    '         container_of(commands[*consumed], struct kk_cmd_buffer, vk);\n' +
    '      if (*consumed)\n' +
    '         kk_whole_pass_command_boundary(rerecord);\n' +
    '      vk_cmd_queue_execute(&source->vk.cmd_queue, rerecord_handle,\n' +
    '                           &dev->vk.dispatch_table);\n' +
    '      if (!rerecord->whole_pass.suspended) {\n' +
    '         (*consumed)++;\n         break;\n      }\n   }');
  // Keep sparse late-residency replay in this same continuation path.
  change('vulkan/kk_queue.c',
    '      if (cmd_buffer->submitted || cmd_buffer->enqueue_commands || (!cmd_buffer->sparse_guarded && kk_sparse_residency_used(dev))) {\n' +
    '         VkResult result = rerecord_and_commit_cmd_buffer(queue, cmd_buffer);\n' +
    '         if (result != VK_SUCCESS)\n            return result;\n' +
    '      } else if (kk_cmd_buffer_has_work(cmd_buffer)) {',
    '      if (cmd_buffer->submitted || cmd_buffer->enqueue_commands ||\n' +
    '          cmd_buffer->deferred_render_chain ||\n' +
    '          (!cmd_buffer->sparse_guarded && kk_sparse_residency_used(dev))) {\n' +
    '         uint32_t consumed = 0;\n         bool committed = false;\n' +
    '         VkResult result = rerecord_and_commit_cmd_buffers(queue,\n' +
    '            &submit->command_buffers[i], submit->command_buffer_count - i,\n' +
    '            &consumed, &committed);\n' +
    '         if (result != VK_SUCCESS)\n            return result;\n' +
    '         for (uint32_t j = 0; committed && j < consumed; j++) {\n' +
    '            struct kk_cmd_buffer *source = container_of(\n' +
    '               submit->command_buffers[i + j], struct kk_cmd_buffer, vk);\n' +
    '            source->submitted = true;\n' +
    '            if (source->drawable) {\n' +
    '               mtl_release(source->drawable);\n               source->drawable = NULL;\n' +
    '            }\n         }\n' +
    '         i += consumed - 1;\n         continue;\n' +
    '      } else if (kk_cmd_buffer_has_work(cmd_buffer)) {');

  edits.set('src/kosmickrisp/vulkan/kk_transient.h',
    fs.readFileSync(new URL('./kosmickrisp-transient.h', import.meta.url), 'utf8'));
  change('vulkan/kk_physical_device.c',
    '   assert(pdev->mem_heap_count <= ARRAY_SIZE(pdev->mem_heaps));',
    `   pdev->mem_types[pdev->mem_type_count++] = (VkMemoryType){
      .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                       VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT,
      .heapIndex = sysmem_heap_idx,
   };

   assert(pdev->mem_heap_count <= ARRAY_SIZE(pdev->mem_heaps));`);
  change('vulkan/kk_device_memory.h', '   void *map;',
    '   void *map;\n   bool lazy;\n   uint32_t image_bind_count;\n   simple_mtx_t transient_mutex;');
  change('vulkan/kk_device_memory.h', '#include "util/list.h"',
    '#include "util/list.h"\n#include "util/simple_mtx.h"');
  change('vulkan/kk_device_memory.h', 'extern const VkExternalMemoryProperties kk_mtlheap_mem_props;',
    'VkResult kk_device_memory_commit_private(struct kk_device *, struct kk_device_memory *);\n\n' +
    'extern const VkExternalMemoryProperties kk_mtlheap_mem_props;');
  change('vulkan/kk_device_memory.c', '   bool import_metal = metal_info && metal_info->handleType;',
    `   simple_mtx_init(&mem->transient_mutex, mtx_plain);
   mem->lazy = properties & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;

   bool import_metal = metal_info && metal_info->handleType;`);
  change('vulkan/kk_device_memory.c', '   if (import_metal || import_host) {',
    '   if (mem->lazy && !import_metal && !import_host) {\n' +
    '      /* Tile storage has no persistent allocation until it is needed. */\n' +
    '   } else if (import_metal || import_host) {');
  change('vulkan/kk_device_memory.c', 'fail_alloc:\n   vk_device_memory_destroy',
    'fail_alloc:\n   simple_mtx_destroy(&mem->transient_mutex);\n   vk_device_memory_destroy');
  change('vulkan/kk_device_memory.c',
    '   kk_destroy_bo(dev, mem->bo);\n\n' +
    '   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);',
    '   if (mem->bo)\n      kk_destroy_bo(dev, mem->bo);\n' +
    '   simple_mtx_destroy(&mem->transient_mutex);\n\n' +
    '   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);');
  change('vulkan/kk_device_memory.c', '   if (!mem->bo->cpu)',
    '   if (!mem->bo || !mem->bo->cpu)');
  change('vulkan/kk_device_memory.c', '   *pCommittedMemoryInBytes = mem->bo->size_B;',
    `   simple_mtx_lock(&mem->transient_mutex);
   *pCommittedMemoryInBytes = mem->bo ? mem->vk.size : 0;
   simple_mtx_unlock(&mem->transient_mutex);`);
  change('vulkan/kk_device_memory.c', '   if (!mem->bo->mtl_handle)',
    '   if (!mem->bo || !mem->bo->mtl_handle)');
  change('vulkan/kk_device_memory.c',
    '      if ((mode == MTL_STORAGE_MODE_SHARED && host_visible) ||',
    '      if (pdev->mem_types[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)\n' +
    '         continue;\n      if ((mode == MTL_STORAGE_MODE_SHARED && host_visible) ||');
  change('vulkan/kk_device_memory.c', '#include <sys/mman.h>',
    '#include <sys/mman.h>\n\n' +
    'static VkResult kk_device_memory_zero_init(struct kk_device *, struct kk_device_memory *);');
  change('vulkan/kk_device_memory.c', `      if (mem->vk.alloc_flags & VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT) {
         if (mem->bo->cpu) {
            memset(mem->bo->cpu, 0, mem->bo->size_B);
         } else {
            enum mtl_command_queue_error error =
               mtl_buffer_zero_sync(dev->mtl_handle, mem->bo->map);
            if (error != MTL_COMMAND_QUEUE_ERROR_NONE) {
               kk_destroy_bo(dev, mem->bo);
               if (error == MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY)
                  result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
               else
                  result = vk_device_set_lost(&dev->vk,
                                             "Device memory zero fill failed");
               goto fail_alloc;
            }
         }
      }
`,
    `      result = kk_device_memory_zero_init(dev, mem);
      if (result != VK_SUCCESS) {
         kk_destroy_bo(dev, mem->bo);
         mem->bo = NULL;
         goto fail_alloc;
      }
`);
  edit('src/kosmickrisp/vulkan/kk_device_memory.c', s => s + `
/* Shared initialization for immediate and deferred private commitment. */
static VkResult
kk_device_memory_zero_init(struct kk_device *dev, struct kk_device_memory *mem)
{
   if (!(mem->vk.alloc_flags & VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT))
      return VK_SUCCESS;
   if (mem->bo->cpu) {
      memset(mem->bo->cpu, 0, mem->bo->size_B);
      return VK_SUCCESS;
   }
   enum mtl_command_queue_error error =
      mtl_buffer_zero_sync(dev->mtl_handle, mem->bo->map);
   if (error == MTL_COMMAND_QUEUE_ERROR_NONE)
      return VK_SUCCESS;
   if (error == MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   return vk_device_set_lost(&dev->vk, "Device memory zero fill failed");
}

/* The caller holds transient_mutex. Use the ordinary private heap path. */
VkResult
kk_device_memory_commit_private(struct kk_device *dev, struct kk_device_memory *mem)
{
   if (mem->bo)
      return VK_SUCCESS;
   uint64_t alignment = 4096;
   os_get_page_size(&alignment);
   VkResult result = kk_alloc_bo_options(dev, &dev->vk.base,
      align64(mem->vk.size, alignment), alignment,
      MTL_RESOURCE_TRACKING_MODE_UNTRACKED | MTL_RESOURCE_STORAGE_MODE_PRIVATE,
      &mem->bo);
   if (result == VK_SUCCESS)
      result = kk_device_memory_zero_init(dev, mem);
   if (result != VK_SUCCESS && mem->bo) {
      kk_destroy_bo(dev, mem->bo);
      mem->bo = NULL;
   }
   return result;
}
`);
  change('vulkan/kk_device_memory.c', '   return mem->bo->gpu;',
    '   return mem->bo ? mem->bo->gpu : 0;');
  // The lazy type is for transient images only; ordinary buffers retain all
  // existing private/shared choices.
  change('vulkan/kk_buffer.c', '   vk_foreach_struct(sType, ext, pMemoryRequirements->pNext) {',
    `   for (uint32_t i = 0; i < pdev->mem_type_count; i++) {
      if (pdev->mem_types[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
         pMemoryRequirements->memoryRequirements.memoryTypeBits &= ~BITFIELD_BIT(i);
   }

   vk_foreach_struct(sType, ext, pMemoryRequirements->pNext) {`);
  change('vulkan/kk_image.c',
    '   pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_types;',
    `   if (!(image->vk.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) ||\n       image->vk.external_handle_types) {
      for (uint32_t i = 0; i < pdev->mem_type_count; i++) {
         if (pdev->mem_types[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
            memory_types &= ~BITFIELD_BIT(i);
      }
   }

   pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_types;`);
  change('bridge/mtl_heap.h', '/* Allocation from heap */',
    'mtl_texture *mtl_new_memoryless_texture(mtl_device *, const struct kk_image_layout *);\n\n' +
    '/* Allocation from heap */');
  change('bridge/mtl_heap.m', '/* Allocation from heap */',
    `mtl_texture *
mtl_new_memoryless_texture(mtl_device *device, const struct kk_image_layout *layout)
{
   @autoreleasepool {
      MTLTextureDescriptor *descriptor = [mtl_new_texture_descriptor(layout) autorelease];
      descriptor.storageMode = MTLStorageModeMemoryless;
      descriptor.usage = MTLTextureUsageRenderTarget;
      return [(id<MTLDevice>)device newTextureWithDescriptor:descriptor];
   }
}

/* Allocation from heap */`);
  change('vulkan/kk_image.h', '   mtl_texture *mtl_handle_array;',
    '   mtl_texture *mtl_handle_array;\n   bool memoryless;\n   mtl_texture *private_handle;');
  change('vulkan/kk_image.h', 'void kk_image_set_label(struct kk_image *image, const char *label);',
    `struct kk_cmd_buffer;
struct kk_image_view;
VkResult kk_transient_prepare_rendering(struct kk_cmd_buffer *, const VkRenderingInfo *);
mtl_texture *kk_transient_render_texture(const struct kk_image_view *);
VkResult kk_image_plane_require_private(struct kk_device *, struct kk_image_plane *);
void kk_image_set_label(struct kk_image *image, const char *label);`);
  change('vulkan/kk_image.h', '#define kk_foreach_slice',
    'mtl_texture *kk_image_plane_render_base(const struct kk_image_plane *);\n' +
    'bool kk_image_plane_is_committed(const struct kk_image_plane *);\n\n' +
    '#define kk_foreach_slice');
  change('vulkan/kk_image.h', '      return (mtl_resource *)image->planes[plane].mtl_handle;',
    '      return (mtl_resource *)kk_image_plane_render_base(&image->planes[plane]);');
  change('vulkan/kk_image.c', '   assert(plane->layout.linear || mem->bo->mtl_handle);',
    `   if (mem->lazy) {
      simple_mtx_lock(&mem->transient_mutex);
      mem->image_bind_count++;
      const bool tile_image = image->plane_count == 1 &&
         image->vk.image_type == VK_IMAGE_TYPE_2D && image->vk.mip_levels == 1 &&
         !(vk_image_usage(&image->vk, image->vk.aspects) & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) &&
         image->vk.create_flags == 0 && !plane->layout.linear;
      if (tile_image && mem->image_bind_count == 1 && !mem->bo)
         plane->mtl_handle = mtl_new_memoryless_texture(dev->mtl_handle, &plane->layout);
      plane->memoryless = plane->mtl_handle != NULL;
      VkResult result = plane->memoryless ? VK_SUCCESS :
         kk_device_memory_commit_private(dev, mem);
      simple_mtx_unlock(&mem->transient_mutex);
      if (result != VK_SUCCESS)
         return result;
   }
   assert(plane->memoryless || plane->layout.linear || mem->bo->mtl_handle);`);
  change('vulkan/kk_image.c',
    '   plane->mtl_handle = kk_image_plane_create_texture(plane, &plane->layout, 0u);',
    '   if (!plane->memoryless)\n' +
    '      plane->mtl_handle = kk_image_plane_create_texture(plane, &plane->layout, 0u);');
  change('vulkan/kk_image.c', '   plane->addr = mem->bo->gpu + *offset_B;',
    '   plane->addr = mem->bo ? mem->bo->gpu + *offset_B : 0;');
  change('vulkan/kk_image.c', '      mtl_release(plane->mtl_handle);',
    '      mtl_release(plane->mtl_handle);\n' +
    '   if (plane->private_handle)\n' +
    '      mtl_release(plane->private_handle);');
  change('vulkan/kk_image_view.h', '      mtl_texture *mtl_handle_render;',
    '      mtl_texture *mtl_handle_render;\n      mtl_texture *transient_private;');
  change('vulkan/kk_image_view.c',
    '      mtl_texture *mtl_handle = image->planes[image_plane].mtl_handle;',
    `      if (plane->memoryless && (kk_image_plane_is_committed(plane) ||
          (view->vk.usage & (VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)))) {
         VkResult result = kk_image_plane_require_private(dev, plane);
         if (result != VK_SUCCESS)
            return result;
      }
      mtl_texture *mtl_handle = kk_image_plane_render_base(plane);`);
  change('vulkan/kk_image_view.c', '      if (view->planes[plane].mtl_handle_render)',
    '      if (view->planes[plane].transient_private)\n' +
    '         mtl_release(view->planes[plane].transient_private);\n\n' +
    '      if (view->planes[plane].mtl_handle_render)');
  edit('src/kosmickrisp/vulkan/kk_image.c', s => s + '\n#include "kk_transient.h"\n');
  change('vulkan/kk_cmd_buffer.h', '   bool deferred_render_chain;',
    '   bool deferred_render_chain;\n   bool transient_safe;');
  change('vulkan/kk_cmd_buffer.h', 'bool kk_whole_pass_needs_submit(struct kk_cmd_buffer *cmd);',
    'bool kk_whole_pass_transient_safe(struct kk_device *, struct vk_command_buffer **, uint32_t);\n' +
    'bool kk_whole_pass_needs_submit(struct kk_cmd_buffer *cmd);');
  change('vulkan/kk_cmd_buffer.c', '      if (!cmd->enqueue_commands && !cmd->deferred_render_chain)\n         vk_cmd_queue_execute',
    '      struct vk_command_buffer *source = &cmd->vk;\n' +
    '      cmd->transient_safe = kk_whole_pass_transient_safe(kk_cmd_buffer_device(cmd),\n' +
    '                                                         &source, 1);\n' +
    '      if (!cmd->enqueue_commands && !cmd->deferred_render_chain)\n         vk_cmd_queue_execute');
  // Non-dispatchable memory/texture decisions happen before BeginRendering.
  // Attachment descriptors select the same backing for every restart.
  change('vulkan/kk_cmd_draw.c', '      descriptor, iview->planes[0].mtl_handle_render);',
    '      descriptor, kk_transient_render_texture(iview));');
  change('vulkan/kk_cmd_draw.c', '         descriptor, info->resolve_iview->planes[0].mtl_handle_render);',
    '         descriptor, kk_transient_render_texture(info->resolve_iview));');
  change('vulkan/kk_queue.c', '   for (*consumed = 0; *consumed < count; (*consumed)++) {',
    '   rerecord->transient_safe = kk_whole_pass_transient_safe(dev, commands, count);\n' +
    '   for (*consumed = 0; *consumed < count; (*consumed)++) {');

  change('vulkan/kk_image.h', '   bool wsi_opaque_alpha;',
    '   bool wsi_opaque_alpha;\n   VkImageUsageFlags2KHR application_usage;');
  change('vulkan/kk_image.c', '   vk_image_init(&dev->vk, &image->vk, pCreateInfo);',
    '   vk_image_init(&dev->vk, &image->vk, pCreateInfo);\n' +
    '   image->application_usage = vk_image_usage_flags(pCreateInfo);');
  change('vulkan/kk_image_view.c', '   vk_image_view_init(&dev->vk, &view->vk, pCreateInfo);',
    `   vk_image_view_init(&dev->vk, &view->vk, pCreateInfo);
   if (image->planes[0].memoryless &&
       !(pCreateInfo->flags & VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA) &&
       !vk_find_struct_const(pCreateInfo->pNext, IMAGE_VIEW_USAGE_CREATE_INFO))
      view->vk.usage &= image->application_usage;`);
  change('vulkan/kk_device.h', '   PFN_vkCmdBeginRendering whole_pass_begin_rendering;',
    '   bool has_lazy_memory;\n   PFN_vkCmdBeginRendering whole_pass_begin_rendering;');
  change('vulkan/kk_device_memory.c',
    '   mem->lazy = properties & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;',
    '   mem->lazy = properties & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;\n' +
    '   if (mem->lazy)\n      p_atomic_set(&dev->has_lazy_memory, true);');

  // Prepare backend attachments after implicit rendering selects its images.
  change('vulkan/kk_cmd_draw.c',
    '   struct kk_rendering_state *render = &cmd->state.gfx.render;\n\n' +
    '   memset(render, 0, sizeof(*render));',
    '   struct kk_rendering_state *render = &cmd->state.gfx.render;\n\n' +
    '   VkResult result = kk_transient_prepare_rendering(cmd, pRenderingInfo);\n' +
    '   if (result != VK_SUCCESS) {\n' +
    '      vk_command_buffer_set_error(&cmd->vk, result);\n      return;\n   }\n\n' +
    '   memset(render, 0, sizeof(*render));');
  // WSI present callbacks attach drawables directly to original command
  // buffers, outside the replay IR. Retain each unique source drawable on the
  // actual internal commit buffer; reset releases only these retained copies.
  change('vulkan/kk_cmd_buffer.h', '   void *drawable;',
    '   void *drawable;\n   struct util_dynarray submit_drawables;');
  change('vulkan/kk_cmd_buffer.c', '   cmd->ts_stage_map = UTIL_DYNARRAY_INIT;',
    '   cmd->ts_stage_map = UTIL_DYNARRAY_INIT;\n   cmd->submit_drawables = UTIL_DYNARRAY_INIT;');
  change('vulkan/kk_cmd_buffer.c', '   util_dynarray_clear(&cmd->ts_stage_map);\n}',
    '   util_dynarray_clear(&cmd->ts_stage_map);\n' +
    '   util_dynarray_foreach(&cmd->submit_drawables, void *, drawable)\n' +
    '      mtl_release(*drawable);\n   util_dynarray_clear(&cmd->submit_drawables);\n}');
  change('vulkan/kk_cmd_buffer.c', '   util_dynarray_fini(&cmd->ts_stage_map);',
    '   util_dynarray_fini(&cmd->ts_stage_map);\n   util_dynarray_fini(&cmd->submit_drawables);');
  change('vulkan/kk_queue.c', '      vk_cmd_queue_execute(&source->vk.cmd_queue, rerecord_handle,',
    `      if (source->drawable) {
         bool retained = false;
         util_dynarray_foreach(&rerecord->submit_drawables, void *, drawable)
            retained |= *drawable == source->drawable;
         if (!retained) {
            void **drawable = util_dynarray_grow(
               &rerecord->submit_drawables, void *, 1);
            if (!drawable) {
               result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
               goto release;
            }
            *drawable = mtl_retain(source->drawable);
         }
      }
      vk_cmd_queue_execute(&source->vk.cmd_queue, rerecord_handle,`);
  change('vulkan/kk_queue.c',
    `   if (cmd->drawable)
      mtl_command_queue_wait_for_drawable(queue->mtl_handle, cmd->drawable);`,
    `   const uint32_t retained_count = util_dynarray_num_elements(
      &cmd->submit_drawables, void *);
   void **drawables = util_dynarray_begin(&cmd->submit_drawables);
   bool primary_retained = false;
   for (uint32_t i = 0; i < retained_count; i++)
      primary_retained |= drawables[i] == cmd->drawable;
   const uint32_t count = retained_count +
      (cmd->drawable != NULL && !primary_retained);
   for (uint32_t i = 0; i < count; i++) {
      void *drawable = i < retained_count ? drawables[i] : cmd->drawable;
      mtl_command_queue_wait_for_drawable(queue->mtl_handle, drawable);
   }`);
  change('vulkan/kk_queue.c',
    `   if (cmd->drawable) {
      mtl_command_queue_signal_drawable(queue->mtl_handle, cmd->drawable);
      mtl_release(cmd->drawable);
      cmd->drawable = NULL;
   }`,
    `   for (uint32_t i = 0; i < count; i++) {
      void *drawable = i < retained_count ? drawables[i] : cmd->drawable;
      mtl_command_queue_signal_drawable(queue->mtl_handle, drawable);
   }
   for (uint32_t i = 0; i < retained_count; i++)
      mtl_release(drawables[i]);
   util_dynarray_clear(&cmd->submit_drawables);
   if (cmd->drawable) {
      mtl_release(cmd->drawable);
      cmd->drawable = NULL;
   }`);

  change('vulkan/kk_queue.c', '   struct vk_command_buffer *vk_cmd = NULL;',
    '   struct vk_command_buffer *vk_cmd = NULL;\n   *committed = false;');
  // Sparse selects the completion callback before this physical commit.
  change('vulkan/kk_queue.c',
    '   kk_queue_commit(queue, rerecord, callback, commit);',
    '   kk_queue_commit(queue, rerecord, callback, commit);\n   *committed = true;');

}

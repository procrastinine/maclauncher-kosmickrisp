// SPDX-License-Identifier: MIT
// Owning POSIX memory handles backed by one shared mapping, without a broker.
const header = String.raw`/* SPDX-License-Identifier: MIT */
#ifndef KK_EXTERNAL_MEMORY_FD_H
#define KK_EXTERNAL_MEMORY_FD_H

#include "kk_format.h"
#include "vk_format.h"
#include "util/format/u_format.h"

static inline bool
kk_fd_image_supported(VkFormat format, VkImageType type, VkImageTiling tiling,
                      VkImageUsageFlags2KHR usage, VkImageCreateFlags2KHR flags)
{
   const VkImageCreateFlags2KHR allowed = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
      VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_ALIAS_BIT;
   if (type != VK_IMAGE_TYPE_2D ||
       (tiling != VK_IMAGE_TILING_OPTIMAL && tiling != VK_IMAGE_TILING_LINEAR) ||
       (flags & ~allowed) || (usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) ||
       vk_format_is_compressed(format) || vk_format_is_depth_or_stencil(format) ||
       vk_format_get_ycbcr_info(format) != NULL ||
       vk_format_get_plane_count(format) != 1)
      return false;

   enum pipe_format pf = vk_format_to_pipe_format(format);
   if (pf == PIPE_FORMAT_NONE || kk_format_is_rgb32(pf) ||
       !util_is_power_of_two_nonzero(util_format_get_blocksize(pf)))
      return false;

   /* Native ordinary/packed color formats; no emulated byte representation. */
   const struct kk_va_format *native = kk_get_va_format(pf);
   return native && native->is_native;
}

#endif /* KK_EXTERNAL_MEMORY_FD_H */
`;

const memory = String.raw`
/* OPAQUE_FD is an owning capability for this allocation, not a host pointer.
 * All supported Darwin CPUs are little-endian. The v1 header consists only
 * of fixed-width fields, with no native pointers or compiler-dependent enums.
 */
#define KK_FD_PAYLOAD_OFFSET UINT64_C(65536)
struct kk_fd_memory_header {
   uint64_t magic, version, allocation_size, payload_size, memory_type;
   uint8_t device_uuid[VK_UUID_SIZE], driver_uuid[VK_UUID_SIZE];
   uint64_t dedicated[12];
};
_Static_assert(sizeof(struct kk_fd_memory_header) < KK_FD_PAYLOAD_OFFSET,
               "External memory header must fit before the payload");

const VkExternalMemoryProperties kk_fd_mem_props = {
   .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT,
   .exportFromImportedHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
};

static void
kk_fd_dedicated_description(const VkMemoryAllocateInfo *info, uint64_t out[12])
{
   const VkMemoryDedicatedAllocateInfo *dedicated =
      vk_find_struct_const(info->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   if (!dedicated)
      return;
   if (dedicated->image) {
      VK_FROM_HANDLE(kk_image, image, dedicated->image);
      const struct vk_image *v = &image->vk;
      out[0] = 1;
      out[1] = v->format;
      out[2] = v->extent.width;
      out[3] = v->extent.height;
      out[4] = v->extent.depth;
      out[5] = v->mip_levels;
      out[6] = v->array_layers;
      out[7] = v->samples;
      out[8] = v->tiling;
      out[9] = v->usage;
      out[10] = v->create_flags;
      out[11] = v->stencil_usage;
   } else if (dedicated->buffer) {
      VK_FROM_HANDLE(kk_buffer, buffer, dedicated->buffer);
      out[0] = 2;
      out[1] = buffer->vk.size;
      out[2] = buffer->vk.usage;
      out[3] = buffer->vk.create_flags;
   }
}

static VkResult
kk_allocate_fd_memory(struct kk_device *dev, const VkMemoryAllocateInfo *info,
                      const VkImportMemoryFdInfoKHR *import,
                      struct kk_device_memory *mem)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);
   uint64_t page_size;
   os_get_page_size(&page_size);
   if (!info->allocationSize || info->allocationSize > pdev->info.max_buffer_size ||
       KK_FD_PAYLOAD_OFFSET % page_size)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   const uint64_t payload_size = align64(info->allocationSize, page_size);
   if (payload_size > SIZE_MAX - KK_FD_PAYLOAD_OFFSET)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   const size_t mapping_size = KK_FD_PAYLOAD_OFFSET + payload_size;
   const bool importing = import && import->handleType;
   struct kk_fd_memory_header expected = {
      .magic = UINT64_C(0x3144464d454d4b4b),
      .version = 1,
      .allocation_size = info->allocationSize,
      .payload_size = payload_size,
      .memory_type = info->memoryTypeIndex,
   };
   memcpy(expected.device_uuid, pdev->vk.properties.deviceUUID, VK_UUID_SIZE);
   memcpy(expected.driver_uuid, pdev->vk.properties.driverUUID, VK_UUID_SIZE);
   kk_fd_dedicated_description(info, expected.dedicated);

   int fd = -1;
   if (importing) {
      if (import->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
          import->fd < 0)
         return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      struct stat stat;
      if (fstat(import->fd, &stat) || stat.st_size < 0 ||
          (uint64_t)stat.st_size != mapping_size)
         return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      /* Until the complete import succeeds the caller retains its descriptor. */
      fd = fcntl(import->fd, F_DUPFD_CLOEXEC, 0);
   } else {
      char name[32];
      for (unsigned attempt = 0; attempt < 16; ++attempt) {
         snprintf(name, sizeof(name), "/kk-%08x%08x", arc4random(), arc4random());
         fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
         if (fd >= 0 || errno != EEXIST)
            break;
      }
      if (fd >= 0) {
         if (shm_unlink(name) || fcntl(fd, F_SETFD, FD_CLOEXEC) ||
             ftruncate(fd, mapping_size)) {
            close(fd);
            return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         }
      }
   }
   if (fd < 0)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   void *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
   if (mapping == MAP_FAILED) {
      close(fd);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   if (importing) {
      if (memcmp(mapping, &expected, sizeof(expected))) {
         munmap(mapping, mapping_size);
         close(fd);
         return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      }
   } else {
      memcpy(mapping, &expected, sizeof(expected));
      /* A fresh shm object is zero-filled, including the entire payload.
       * Imported contents are never overwritten or reinitialized. */
   }

   struct kk_bo *bo = CALLOC_STRUCT(kk_bo);
   if (!bo) {
      munmap(mapping, mapping_size);
      close(fd);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   bo->map = mtl_new_buffer_with_memory_fd(dev->mtl_handle, fd, mapping,
                                           mapping_size, KK_FD_PAYLOAD_OFFSET,
                                           payload_size);
   if (!bo->map) {
      FREE(bo);
      munmap(mapping, mapping_size);
      close(fd);
      return vk_error(dev, importing ? VK_ERROR_INVALID_EXTERNAL_HANDLE :
                                      VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   /* The native buffer now owns its FD and mapping through final release.
    * Exported descriptors own additional kernel references independently. */
   bo->size_B = payload_size;
   bo->gpu = mtl_buffer_get_gpu_address(bo->map);
   if (pdev->mem_types[info->memoryTypeIndex].propertyFlags &
       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      bo->cpu = mtl_get_contents(bo->map);
   kk_device_add_buffer_to_residency_set(dev, bo->map);
   mem->bo = bo;
   if (importing)
      close(import->fd);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *info, int *fd)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(kk_device_memory, mem, info->memory);
   if (info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
       !(mem->vk.export_handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   *fd = mtl_buffer_export_memory_fd(mem->bo->map);
   if (*fd < 0)
      return vk_error(dev, errno == EMFILE || errno == ENFILE ?
                            VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetMemoryFdPropertiesKHR(VkDevice device,
                            VkExternalMemoryHandleTypeFlagBits handleType,
                            int fd, VkMemoryFdPropertiesKHR *properties)
{
   /* OPAQUE_FD is expressly excluded from this query by VUID-00674.
    * No other memory-FD type is advertised by this driver. */
   return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}
`;

const native = String.raw`
/* Only explicitly external buffers have associated FD metadata. Ordinary
 * resources gain no fields, allocations, destructor or per-draw lookup. */
static char kk_memory_fd_key;

mtl_buffer *
mtl_new_buffer_with_memory_fd(mtl_device *device, int fd, void *mapping,
                               uint64_t mapping_size, uint64_t payload_offset,
                               uint64_t payload_size)
{
   @autoreleasepool {
      id<MTLDevice> dev = (id<MTLDevice>)device;
      id<MTLBuffer> buffer = [dev newBufferWithBytesNoCopy:(char *)mapping + payload_offset
         length:payload_size options:MTLResourceStorageModeShared |
                                       MTLResourceHazardTrackingModeUntracked
         deallocator:^(void *pointer, NSUInteger length) {
            munmap(mapping, mapping_size);
            close(fd);
         }];
      if (buffer)
         objc_setAssociatedObject(buffer, &kk_memory_fd_key, @(fd),
                                   OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      return (mtl_buffer *)buffer;
   }
}

int
mtl_buffer_export_memory_fd(mtl_buffer *buffer)
{
   @autoreleasepool {
      NSNumber *value = objc_getAssociatedObject((id)buffer, &kk_memory_fd_key);
      if (!value) {
         errno = EINVAL;
         return -1;
      }
      return fcntl(value.intValue, F_DUPFD_CLOEXEC, 0);
   }
}
`;

export function apply(context) {
 const {edit}=context;
 context.edits.set('src/kosmickrisp/vulkan/kk_external_memory_fd.h',header);
 edit('src/kosmickrisp/vulkan/kk_physical_device.c',s=>s.replace(
  '      .KHR_external_memory = true,',
  '      .KHR_external_memory = true,\n      .KHR_external_memory_fd = true,'));
 edit('src/kosmickrisp/vulkan/kk_device_memory.h',s=>s.replace(
  'extern const VkExternalMemoryProperties kk_host_mem_props;',
  'extern const VkExternalMemoryProperties kk_host_mem_props;\nextern const VkExternalMemoryProperties kk_fd_mem_props;'));
 edit('src/kosmickrisp/bridge/mtl_buffer.h',s=>s.replace(
  '/* Allocation from buffer */',String.raw`/* FD and mapping ownership transfers only on a successful native allocation. */
mtl_buffer *mtl_new_buffer_with_memory_fd(mtl_device *device, int fd, void *mapping,
   uint64_t mapping_size, uint64_t payload_offset, uint64_t payload_size);
int mtl_buffer_export_memory_fd(mtl_buffer *buffer);

/* Allocation from buffer */`));
 edit('src/kosmickrisp/bridge/mtl_buffer.m',s=>s.replace(
  '#include "mtl_buffer.h"',String.raw`#include "mtl_buffer.h"
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>`)+native);
 edit('src/kosmickrisp/bridge/stubs/mtl_buffer.c',s=>String(s)+String.raw`
mtl_buffer *
mtl_new_buffer_with_memory_fd(mtl_device *device, int fd, void *mapping,
   uint64_t mapping_size, uint64_t payload_offset, uint64_t payload_size)
{
   return NULL;
}
int mtl_buffer_export_memory_fd(mtl_buffer *buffer) { return -1; }
`);
 edit('src/kosmickrisp/vulkan/kk_device_memory.c',s=>s
  .replace('#include "kk_device_memory.h"',String.raw`#include "kk_device_memory.h"
#include "kk_buffer.h"
#include "kk_image.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>`)
  .replace('/* Supports mtlheap only */',memory+'\n/* Supports mtlheap only */')
  .replace('   bool import_metal = metal_info && metal_info->handleType;',String.raw`   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
   if ((fd_info && fd_info->handleType) ||
       (mem->vk.export_handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)) {
      if ((metal_info && metal_info->handleType) ||
          (host_info && host_info->handleType) ||
          (mem->vk.export_handle_types & ~VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)) {
         result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto fail_alloc;
      }
      result = kk_allocate_fd_memory(dev, pAllocateInfo, fd_info, mem);
      if (result != VK_SUCCESS)
         goto fail_alloc;
      *pMem = kk_device_memory_to_handle(mem);
      return VK_SUCCESS;
   }

   bool import_metal = metal_info && metal_info->handleType;`));
 edit('src/kosmickrisp/vulkan/kk_buffer.c',s=>s.replace(
  '   switch (pExternalBufferInfo->handleType) {',String.raw`   switch (pExternalBufferInfo->handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
      pExternalBufferProperties->externalMemoryProperties = kk_fd_mem_props;
      return;`));
 edit('src/kosmickrisp/vulkan/kk_image.c',s=>s
  .replace('#include "kk_image.h"','#include "kk_image.h"\n#include "kk_external_memory_fd.h"')
  .replace('      switch (external_info->handleType) {',String.raw`      switch (external_info->handleType) {
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
         if (!kk_fd_image_supported(pImageFormatInfo->format,
               pImageFormatInfo->type, pImageFormatInfo->tiling, all_usage,
               create_flags))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
         maxArraySize = maxMipLevels = 1;
         sampleCounts = VK_SAMPLE_COUNT_1_BIT;
         ext_mem_props = &kk_fd_mem_props;
         break;`)
  .replace('   vk_image_init(&dev->vk, &image->vk, pCreateInfo);',String.raw`   vk_image_init(&dev->vk, &image->vk, pCreateInfo);
   if ((image->vk.external_handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) &&
       (!kk_fd_image_supported(image->vk.format, image->vk.image_type,
                              image->vk.tiling, image->vk.usage,
                              image->vk.create_flags) ||
        image->vk.array_layers != 1 || image->vk.mip_levels != 1 ||
        image->vk.samples != VK_SAMPLE_COUNT_1_BIT)) {
      vk_image_finish(&image->vk);
      return vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }`)
  .replace('      .maxResourceSize = UINT32_MAX, /* TODO */',String.raw`      .maxResourceSize = external_info &&
         external_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
            ? MIN2((uint64_t)UINT32_MAX, pdev->info.max_buffer_size)
            : UINT32_MAX, /* TODO: ordinary upstream resource limit */`));
 edit('src/kosmickrisp/vulkan/kk_image_layout.c',s=>s
  .replace('#include "kk_image_layout.h"','#include "kk_image_layout.h"\n#include "kk_external_memory_fd.h"')
  .replace('   layout->linear = image->tiling != VK_IMAGE_TILING_OPTIMAL;',String.raw`   const bool external_fd = image->external_handle_types &
                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
   layout->linear = image->tiling != VK_IMAGE_TILING_OPTIMAL || external_fd;`)
  .replace('   layout->usage = vk_image_usage_flags_to_mtl_texture_usage(',String.raw`   if (external_fd)
      layout->optimized_layout = false;
   layout->usage = vk_image_usage_flags_to_mtl_texture_usage(`));
}

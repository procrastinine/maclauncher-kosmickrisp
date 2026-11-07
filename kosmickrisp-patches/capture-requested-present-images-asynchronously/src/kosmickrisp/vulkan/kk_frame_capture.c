/* SPDX-License-Identifier: MIT */

#include "kk_frame_capture.h"
#include "kk_device.h"
#include "kk_entrypoints.h"
#include "kk_instance.h"
#include "kk_physical_device.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"
#include "util/format/u_format.h"
#include "util/os_time.h"
#include "vk_format.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

struct kk_capture_job {
   VkBuffer buffer;
   VkDeviceMemory memory;
   VkCommandPool pool;
   VkCommandBuffer command;
   VkSemaphore ready;
   VkFence copied, retired;
   void *map;
   bool coherent, retirement_submitted;
   uint32_t width, height, stride;
   VkFormat format;
   VkResult present_result;
   uint64_t frame, time;
   struct stat request;
};

struct kk_frame_capture {
   struct kk_device *dev;
   PFN_vkQueuePresentKHR present;
   PFN_vkCreateSwapchainKHR create_swapchain;
   PFN_vkDestroyDevice destroy_device;
   char *directory, *request_path;
   pthread_t worker;
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   bool stop, busy;
   uint64_t frame;
   struct kk_capture_job *job, *orphan;
};

static void
kk_capture_free(struct kk_frame_capture *capture, struct kk_capture_job *job)
{
   struct kk_device *dev = capture->dev;
   const struct vk_device_dispatch_table *d = &dev->vk.dispatch_table;
   VkDevice device = kk_device_to_handle(dev);
   if (job->map)
      d->UnmapMemory(device, job->memory);
   if (job->pool)
      d->DestroyCommandPool(device, job->pool, NULL);
   if (job->buffer)
      d->DestroyBuffer(device, job->buffer, NULL);
   if (job->memory)
      d->FreeMemory(device, job->memory, NULL);
   if (job->ready)
      d->DestroySemaphore(device, job->ready, NULL);
   if (job->copied)
      d->DestroyFence(device, job->copied, NULL);
   if (job->retired)
      d->DestroyFence(device, job->retired, NULL);
   free(job);
}

static bool
kk_capture_write(struct kk_frame_capture *capture, struct kk_capture_job *job)
{
   char *image_path = NULL, *info_path = NULL;
   uint8_t *row = malloc((size_t)job->width * 4);
   FILE *image = NULL, *info = NULL;
   bool ok = false;
   if (!row || asprintf(&image_path, "%s/frame-%" PRIu64 ".ppm",
                        capture->directory, job->frame) < 0 ||
       asprintf(&info_path, "%s/frame-%" PRIu64 ".txt",
                capture->directory, job->frame) < 0)
      goto out;
   image = fopen(image_path, "wb");
   if (!image || fprintf(image, "P6\n%u %u\n255\n", job->width, job->height) < 0)
      goto out;
   /* Preserve display-encoded bytes; the unpacker otherwise linearizes sRGB. */
   enum pipe_format format = util_format_linear(vk_format_to_pipe_format(job->format));
   for (uint32_t y = 0; y < job->height; y++) {
      util_format_unpack_rgba_8unorm_rect(format, row, job->width * 4,
         (uint8_t *)job->map + (size_t)y * job->stride, job->stride,
         job->width, 1);
      /* PPM is RGB, with no padding or alpha. Compact each converted row. */
      for (uint32_t x = 0; x < job->width; x++) {
         row[x * 3] = row[x * 4];
         row[x * 3 + 1] = row[x * 4 + 1];
         row[x * 3 + 2] = row[x * 4 + 2];
      }
      if (fwrite(row, 3, job->width, image) != job->width)
         goto out;
   }
   if (fclose(image) != 0) {
      image = NULL;
      goto out;
   }
   image = NULL;
   info = fopen(info_path, "w");
   if (!info || fprintf(info, "time_ns=%" PRIu64 " size=%ux%u frame=%" PRIu64
                        " format=%u layer=0\n", job->time, job->width,
                        job->height, job->frame, job->format) < 0)
      goto out;
   ok = fclose(info) == 0;
   info = NULL;
out:
   if (image)
      fclose(image);
   if (info)
      fclose(info);
   if (!ok) {
      if (image_path)
         unlink(image_path);
      if (info_path)
         unlink(info_path);
      mesa_logw("kk: frame capture write failed: %s", capture->directory);
   }
   free(image_path);
   free(info_path);
   free(row);
   return ok;
}

static void *
kk_capture_worker(void *data)
{
   struct kk_frame_capture *capture = data;
   struct kk_device *dev = capture->dev;
   VkDevice device = kk_device_to_handle(dev);
   const struct vk_device_dispatch_table *d = &dev->vk.dispatch_table;
   pthread_mutex_lock(&capture->mutex);
   for (;;) {
      while (!capture->job && !capture->stop)
         pthread_cond_wait(&capture->cond, &capture->mutex);
      struct kk_capture_job *job = capture->job;
      if (!job)
         break;
      capture->job = NULL;
      pthread_mutex_unlock(&capture->mutex);

      /* The retirement fence also protects the semaphore normal WSI consumed.
       * Only this worker waits; rendering and later presents remain asynchronous. */
      VkFence fence = job->retirement_submitted ? job->retired : job->copied;
      VkResult wait_result = d->WaitForFences(device, 1, &fence, true, UINT64_MAX);
      VkResult result = wait_result;
      if (result == VK_SUCCESS && !job->coherent) {
         const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = job->memory, .size = VK_WHOLE_SIZE,
         };
         result = d->InvalidateMappedMemoryRanges(device, 1, &range);
      }
      if (result == VK_SUCCESS &&
          (job->present_result == VK_SUCCESS || job->present_result == VK_SUBOPTIMAL_KHR) &&
          kk_capture_write(capture, job)) {
         /* Leave a replaced request intact for the next present. */
         struct stat current;
         if (stat(capture->request_path, &current) == 0 &&
             current.st_ino == job->request.st_ino &&
             current.st_size == job->request.st_size &&
             current.st_mtime == job->request.st_mtime)
            unlink(capture->request_path);
      } else if (result != VK_SUCCESS) {
         mesa_logw("kk: frame capture completion failed: %d", result);
      }

      bool retired = job->retirement_submitted &&
         (wait_result == VK_SUCCESS || wait_result == VK_ERROR_DEVICE_LOST);
      if (retired)
         kk_capture_free(capture, job);
      pthread_mutex_lock(&capture->mutex);
      if (!retired) {
         /* A failed retirement submit cannot prove the WSI wait has retired.
          * Keep its objects until device destruction rather than freeing early. */
         capture->orphan = job;
      } else {
         capture->busy = false;
      }
   }
   pthread_mutex_unlock(&capture->mutex);
   return NULL;
}

static struct kk_capture_job *
kk_capture_prepare(struct kk_frame_capture *capture, struct vk_queue *queue,
                   struct wsi_swapchain *chain, uint32_t index,
                   uint64_t frame, const struct stat *request)
{
   struct kk_device *dev = capture->dev;
   struct kk_physical_device *pdev = kk_device_physical(dev);
   const struct vk_device_dispatch_table *d = &dev->vk.dispatch_table;
   VkDevice device = kk_device_to_handle(dev);
   struct kk_capture_job *job = calloc(1, sizeof(*job));
   if (!job)
      return NULL;
   job->format = chain->image_info.create.format;
   job->width = chain->image_info.create.extent.width;
   job->height = chain->image_info.create.extent.height;
   job->frame = frame;
   job->time = os_time_get_nano();
   job->request = *request;
   uint32_t texel_size = vk_format_get_blocksize(job->format);
   if (!job->width || !job->height || vk_format_is_compressed(job->format) ||
       vk_format_aspects(job->format) != VK_IMAGE_ASPECT_COLOR_BIT ||
       (chain->create_flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) ||
       (queue->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) ||
       texel_size == 0 || job->width > UINT32_MAX / texel_size - 255 ||
       job->width > UINT32_MAX / 4 ||
       !util_format_unpack_description(vk_format_to_pipe_format(job->format))->unpack_rgba_8unorm)
      goto fail;
   job->stride = ALIGN_POT(job->width, 256) * texel_size;
   VkDeviceSize bytes = (VkDeviceSize)job->stride * job->height;
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (d->CreateBuffer(device, &buffer_info, NULL, &job->buffer) != VK_SUCCESS)
      goto fail;
   VkMemoryRequirements req;
   d->GetBufferMemoryRequirements(device, job->buffer, &req);
   uint32_t memory_type = UINT32_MAX;
   for (uint32_t i = 0; i < pdev->mem_type_count; i++) {
      VkMemoryPropertyFlags flags = pdev->mem_types[i].propertyFlags;
      if ((req.memoryTypeBits & BITFIELD_BIT(i)) &&
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         memory_type = i;
         job->coherent = flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
         if (job->coherent)
            break;
      }
   }
   if (memory_type == UINT32_MAX)
      goto fail;
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size, .memoryTypeIndex = memory_type,
   };
   if (d->AllocateMemory(device, &alloc_info, NULL, &job->memory) != VK_SUCCESS ||
       d->BindBufferMemory(device, job->buffer, job->memory, 0) != VK_SUCCESS ||
       d->MapMemory(device, job->memory, 0, VK_WHOLE_SIZE, 0, &job->map) != VK_SUCCESS)
      goto fail;
   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = queue->queue_family_index,
   };
   if (d->CreateCommandPool(device, &pool_info, NULL, &job->pool) != VK_SUCCESS)
      goto fail;
   const VkCommandBufferAllocateInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = job->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   if (d->AllocateCommandBuffers(device, &command_info, &job->command) != VK_SUCCESS)
      goto fail;
   const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   if (d->BeginCommandBuffer(job->command, &begin) != VK_SUCCESS)
      goto fail;
   const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = chain->get_wsi_image(chain, index)->image,
      .subresourceRange = range,
   };
   VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier,
   };
   d->CmdPipelineBarrier2(job->command, &dependency);
   const VkBufferImageCopy region = {
      .bufferRowLength = job->stride / texel_size,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {job->width, job->height, 1},
   };
   d->CmdCopyImageToBuffer(job->command, barrier.image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, job->buffer, 1, &region);
   barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
   barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
   barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
   barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
   barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   const VkBufferMemoryBarrier2 host_barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = job->buffer, .size = VK_WHOLE_SIZE,
   };
   dependency.bufferMemoryBarrierCount = 1;
   dependency.pBufferMemoryBarriers = &host_barrier;
   d->CmdPipelineBarrier2(job->command, &dependency);
   if (d->EndCommandBuffer(job->command) != VK_SUCCESS)
      goto fail;
   const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (d->CreateSemaphore(device, &semaphore, NULL, &job->ready) != VK_SUCCESS ||
       d->CreateFence(device, &fence, NULL, &job->copied) != VK_SUCCESS ||
       d->CreateFence(device, &fence, NULL, &job->retired) != VK_SUCCESS)
      goto fail;
   return job;
fail:
   kk_capture_free(capture, job);
   return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_capture_QueuePresentKHR(VkQueue handle, const VkPresentInfoKHR *present)
{
   VK_FROM_HANDLE(vk_queue, queue, handle);
   struct kk_device *dev = container_of(queue->base.device, struct kk_device, vk);
   struct kk_frame_capture *capture = dev->frame_capture;
   struct stat request;
   pthread_mutex_lock(&capture->mutex);
   uint64_t frame = ++capture->frame;
   bool requested = !capture->busy && present->swapchainCount &&
      stat(capture->request_path, &request) == 0 && S_ISREG(request.st_mode);
   if (requested)
      capture->busy = true;
   pthread_mutex_unlock(&capture->mutex);
   if (!requested)
      return capture->present(handle, present);

   VK_FROM_HANDLE(wsi_swapchain, chain, present->pSwapchains[0]);
   VkResult *owned_results = present->pResults ? NULL :
      malloc(sizeof(*owned_results) * present->swapchainCount);
   struct kk_capture_job *job = kk_capture_prepare(capture, queue, chain,
      present->pImageIndices[0], frame, &request);
   const struct vk_device_dispatch_table *d = &dev->vk.dispatch_table;
   VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
   if (job && (present->pResults || owned_results)) {
      VkPipelineStageFlags *stages = calloc(present->waitSemaphoreCount, sizeof(*stages));
      if (stages || present->waitSemaphoreCount == 0) {
         for (uint32_t i = 0; i < present->waitSemaphoreCount; i++)
            stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
         const VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = present->waitSemaphoreCount,
            .pWaitSemaphores = present->pWaitSemaphores,
            .pWaitDstStageMask = stages,
            .commandBufferCount = 1, .pCommandBuffers = &job->command,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &job->ready,
         };
         result = d->QueueSubmit(handle, 1, &submit, job->copied);
         free(stages);
      }
   }
   if (result != VK_SUCCESS) {
      free(owned_results);
      if (job)
         kk_capture_free(capture, job);
      pthread_mutex_lock(&capture->mutex);
      capture->busy = false;
      pthread_mutex_unlock(&capture->mutex);
      mesa_logw("kk: frame capture preparation failed: %d", result);
      return capture->present(handle, present);
   }

   VkPresentInfoKHR captured_present = *present;
   captured_present.waitSemaphoreCount = 1;
   captured_present.pWaitSemaphores = &job->ready;
   captured_present.pResults = present->pResults ? present->pResults : owned_results;
   for (uint32_t i = 0; i < present->swapchainCount; i++)
      captured_present.pResults[i] = VK_ERROR_UNKNOWN;
   result = capture->present(handle, &captured_present);
   job->present_result = captured_present.pResults[0];
   free(owned_results);
   /* Retire WSI's wait before destroying its semaphore on the worker. */
   job->retirement_submitted =
      d->QueueSubmit(handle, 0, NULL, job->retired) == VK_SUCCESS;
   pthread_mutex_lock(&capture->mutex);
   capture->job = job;
   pthread_cond_signal(&capture->cond);
   pthread_mutex_unlock(&capture->mutex);
   return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_capture_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                              const VkAllocationCallbacks *alloc, VkSwapchainKHR *chain)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VkSwapchainCreateInfoKHR capture_info = *info;
   capture_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   return dev->frame_capture->create_swapchain(device, &capture_info, alloc, chain);
}

static VKAPI_ATTR void VKAPI_CALL
kk_capture_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   if (!dev)
      return;
   struct kk_frame_capture *capture = dev->frame_capture;
   PFN_vkDestroyDevice destroy_device = capture->destroy_device;
   pthread_mutex_lock(&capture->mutex);
   capture->stop = true;
   pthread_cond_signal(&capture->cond);
   pthread_mutex_unlock(&capture->mutex);
   pthread_join(capture->worker, NULL);
   if (capture->orphan) {
      dev->vk.dispatch_table.DeviceWaitIdle(device);
      kk_capture_free(capture, capture->orphan);
   }
   pthread_cond_destroy(&capture->cond);
   pthread_mutex_destroy(&capture->mutex);
   free(capture->request_path);
   free(capture->directory);
   free(capture);
   dev->frame_capture = NULL;
   destroy_device(device, alloc);
}

void
kk_frame_capture_init(struct kk_device *dev)
{
   const char *directory = getenv("KK_CAPTURE_DIR");
   if (!directory || !directory[0])
      return;
   struct kk_frame_capture *capture = calloc(1, sizeof(*capture));
   if (!capture)
      return;
   capture->dev = dev;
   /* Compose with already-installed diagnostics rather than bypassing them. */
   capture->present = dev->vk.dispatch_table.QueuePresentKHR;
   capture->create_swapchain = dev->vk.dispatch_table.CreateSwapchainKHR;
   capture->destroy_device = dev->vk.dispatch_table.DestroyDevice;
   capture->directory = strdup(directory);
   if (!capture->directory || asprintf(&capture->request_path, "%s/request", directory) < 0)
      goto fail;
   if (mkdir(directory, 0700) != 0 && errno != EEXIST)
      goto fail;
   if (pthread_mutex_init(&capture->mutex, NULL) != 0)
      goto fail;
   if (pthread_cond_init(&capture->cond, NULL) != 0)
      goto fail_mutex;
   if (pthread_create(&capture->worker, NULL, kk_capture_worker, capture) != 0)
      goto fail_cond;
   dev->frame_capture = capture;
   dev->vk.dispatch_table.QueuePresentKHR = kk_capture_QueuePresentKHR;
   dev->vk.dispatch_table.CreateSwapchainKHR = kk_capture_CreateSwapchainKHR;
   dev->vk.dispatch_table.DestroyDevice = kk_capture_DestroyDevice;
   dev->exposed_dispatch_table.QueuePresentKHR = kk_capture_QueuePresentKHR;
   dev->exposed_dispatch_table.CreateSwapchainKHR = kk_capture_CreateSwapchainKHR;
   dev->exposed_dispatch_table.DestroyDevice = kk_capture_DestroyDevice;
   return;
fail_cond:
   pthread_cond_destroy(&capture->cond);
fail_mutex:
   pthread_mutex_destroy(&capture->mutex);
fail:
   mesa_logw("kk: could not enable frame capture: %s", directory);
   free(capture->request_path);
   free(capture->directory);
   free(capture);
}

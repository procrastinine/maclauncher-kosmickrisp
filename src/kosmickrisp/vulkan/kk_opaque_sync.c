/* SPDX-License-Identifier: MIT */

#include "kk_opaque_sync.h"
#include "kk_debug.h"
#include "kk_device.h"
#include "kk_interlock_submit.h"
#include "kk_queue.h"
#include "kk_sync.h"
#include "vk_common_entrypoints.h"
#include "vk_alloc.h"
#include "vk_semaphore.h"
#include "util/hash_table.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "kosmickrisp/bridge/mtl_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* A payload has no vk_device lifetime. Exported FDs can outlive the exporting
 * device. Device-local loss registrations are owned by the individual aliases.
 * Completion and availability are the real shared event and its committed
 * pending value, never a CPU proxy for GPU completion. */
struct kk_opaque_payload {
   struct kk_sync_timeline native;
   uint64_t device_id;
   uint64_t next_signal;
   uint64_t next_wait;
   unsigned references;
   bool timeline;
};

struct kk_opaque_sync {
   struct vk_sync base;
   struct kk_opaque_payload *payload;
   struct kk_sync_timeline *loss_sync;
   uint64_t point; /* A moved binary wait owns this immutable generation. */
};

/* An ordinary semaphore may receive a permanent import in place. */
static_assert(sizeof(struct kk_opaque_sync) <= sizeof(struct kk_sync_timeline),
              "opaque imports must fit ordinary permanent semaphore storage");

struct kk_opaque_key {
   uint64_t dev;
   uint64_t ino;
};

struct kk_opaque_fd {
   struct kk_opaque_key key;
   struct kk_opaque_payload *payload;
   int witness;
};

static simple_mtx_t registry_mutex = SIMPLE_MTX_INITIALIZER;
static struct hash_table *registry;
static const struct vk_sync_type opaque_type;

static uint32_t
key_hash(const void *key)
{
   return _mesa_hash_data(key, sizeof(struct kk_opaque_key));
}

static bool
key_equal(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(struct kk_opaque_key)) == 0;
}

static struct kk_opaque_payload *
payload_ref(struct kk_opaque_payload *payload)
{
   p_atomic_inc(&payload->references);
   return payload;
}

static void
payload_unref(struct kk_opaque_payload *payload)
{
   if (!payload || !p_atomic_dec_zero(&payload->references))
      return;
   mtl_release(payload->native.mtl_handle);
   mtx_destroy(&payload->native.publish_mutex);
   free(payload);
}

/* The witness was opened independently before unlink. It does not retain the
 * client's flock/open-file description. dup/SCM_RIGHTS retain that description
 * and its shared lock. Retire closed leases lazily on used-feature operations;
 * no thread, timer or callback runs for unused applications or idle handles. */
static void
registry_reap_locked(void)
{
   if (!registry)
      return;
   hash_table_foreach(registry, entry) {
      struct kk_opaque_fd *record = entry->data;
      if (flock(record->witness, LOCK_EX | LOCK_NB) != 0)
         continue;
      _mesa_hash_table_remove(registry, entry);
      close(record->witness);
      payload_unref(record->payload);
      free(record);
   }
   if (registry->entries == 0) {
      _mesa_hash_table_destroy(registry, NULL);
      registry = NULL;
   }
}

static struct kk_opaque_sync *
to_opaque(struct vk_sync *sync)
{
   assert(sync->type == &opaque_type);
   return container_of(sync, struct kk_opaque_sync, base);
}

static void
alias_finish(struct vk_device *device, struct kk_opaque_sync *sync)
{
   if (sync->loss_sync) {
      struct kk_device *dev = container_of(device, struct kk_device, vk);
      kk_interlock_sync_finish(dev, sync->loss_sync);
      free(sync->loss_sync);
      sync->loss_sync = NULL;
   }
   payload_unref(sync->payload);
   sync->payload = NULL;
}

static VkResult
alias_attach(struct vk_device *device, struct kk_opaque_sync *sync,
             struct kk_opaque_payload *payload)
{
   struct kk_device *dev = container_of(device, struct kk_device, vk);
   struct kk_sync_timeline *loss_sync = NULL;
   if (dev->interlock_registry) {
      loss_sync = calloc(1, sizeof(*loss_sync));
      if (!loss_sync)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      loss_sync->mtl_handle = payload->native.mtl_handle;
      kk_interlock_sync_init(dev, loss_sync);
   }
   alias_finish(device, sync);
   sync->payload = payload_ref(payload);
   sync->loss_sync = loss_sync;
   sync->point = 0;
   return VK_SUCCESS;
}

static VkResult
opaque_init(struct vk_device *device, struct vk_sync *base,
            uint64_t initial_value)
{
   /* Move destinations and temporary imports acquire their payload afterwards.
    * In particular, consuming a binary wait never creates a spare Metal event. */
   if (!(base->flags & VK_SYNC_IS_SHAREABLE))
      return VK_SUCCESS;

   struct kk_device *dev = container_of(device, struct kk_device, vk);
   struct kk_opaque_payload *payload = calloc(1, sizeof(*payload));
   if (!payload)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (mtx_init(&payload->native.publish_mutex, mtx_plain) != thrd_success) {
      free(payload);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   payload->native.mtl_handle = mtl_new_shared_event(dev->mtl_handle);
   if (!payload->native.mtl_handle) {
      mtx_destroy(&payload->native.publish_mutex);
      free(payload);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   payload->references = 1;
   payload->timeline = base->flags & VK_SYNC_IS_TIMELINE;
   payload->device_id = mtl_device_get_registry_id(dev->mtl_handle);
   payload->native.base.type = &kk_sync_type;
   payload->native.base.flags = VK_SYNC_IS_TIMELINE;
   payload->native.pending_value = initial_value;
   payload->next_signal = initial_value ? 2 : 1;
   payload->next_wait = 1;
   mtl_shared_event_set_signaled_value(payload->native.mtl_handle, initial_value);
   VkResult result = alias_attach(device, to_opaque(base), payload);
   payload_unref(payload);
   return result;
}

static void
opaque_finish(struct vk_device *device, struct vk_sync *base)
{
   alias_finish(device, to_opaque(base));
   simple_mtx_lock(&registry_mutex);
   registry_reap_locked();
   simple_mtx_unlock(&registry_mutex);
}

static uint64_t
binary_point(struct kk_opaque_sync *sync, bool consume)
{
   if (sync->point)
      return sync->point;
   struct kk_opaque_payload *payload = sync->payload;
   mtx_lock(&payload->native.publish_mutex);
   uint64_t point = payload->next_wait;
   if (consume)
      payload->next_wait++;
   mtx_unlock(&payload->native.publish_mutex);
   return point;
}

static uint64_t
binary_signal_point(struct kk_opaque_sync *sync)
{
   struct kk_opaque_payload *payload = sync->payload;
   mtx_lock(&payload->native.publish_mutex);
   uint64_t point = payload->next_signal++;
   mtx_unlock(&payload->native.publish_mutex);
   return point;
}

static VkResult
opaque_signal(struct vk_device *device, struct vk_sync *base, uint64_t value)
{
   struct kk_opaque_sync *sync = to_opaque(base);
   if (!(base->flags & VK_SYNC_IS_TIMELINE))
      value = binary_signal_point(sync);
   return vk_sync_signal(device, &sync->payload->native.base, value);
}

static VkResult
opaque_get_value(struct vk_device *device, struct vk_sync *base, uint64_t *value)
{
   return vk_sync_get_value(device, &to_opaque(base)->payload->native.base, value);
}

static VkResult
opaque_wait(struct vk_device *device, struct vk_sync *base, uint64_t value,
            enum vk_sync_wait_flags flags, uint64_t timeout)
{
   struct kk_opaque_sync *sync = to_opaque(base);
   if (!(base->flags & VK_SYNC_IS_TIMELINE))
      value = binary_point(sync, false);
   return vk_sync_wait(device, &sync->payload->native.base, value, flags, timeout);
}

static VkResult
opaque_reset(struct vk_device *device, struct vk_sync *base)
{
   binary_point(to_opaque(base), true);
   return VK_SUCCESS;
}

static VkResult
opaque_move(struct vk_device *device, struct vk_sync *dst_base,
            struct vk_sync *src_base)
{
   struct kk_opaque_sync *dst = to_opaque(dst_base);
   struct kk_opaque_sync *src = to_opaque(src_base);
   VkResult result = alias_attach(device, dst, src->payload);
   if (result != VK_SUCCESS)
      return result;
   dst->point = binary_point(src, true);
   return VK_SUCCESS;
}

static VkResult
opaque_import(struct vk_device *device, struct vk_sync *base, int fd)
{
   struct stat st;
   if (fd < 0 || fstat(fd, &st) != 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   const struct kk_opaque_key key = {st.st_dev, st.st_ino};
   struct kk_device *dev = container_of(device, struct kk_device, vk);
   struct kk_opaque_payload *payload = NULL;
   simple_mtx_lock(&registry_mutex);
   struct hash_entry *entry = registry ? _mesa_hash_table_search(registry, &key) : NULL;
   if (entry) {
      struct kk_opaque_fd *record = entry->data;
      bool timeline = base->flags & VK_SYNC_IS_TIMELINE;
      if (timeline == record->payload->timeline &&
          record->payload->device_id == mtl_device_get_registry_id(dev->mtl_handle))
         payload = payload_ref(record->payload);
   }
   simple_mtx_unlock(&registry_mutex);
   if (!payload)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   VkResult result = alias_attach(device, to_opaque(base), payload);
   payload_unref(payload);
   /* The Vulkan entrypoint consumes fd only after this transaction succeeds. */
   return result;
}

static VkResult
opaque_export(struct vk_device *device, struct vk_sync *base, int *out_fd)
{
   struct kk_opaque_sync *sync = to_opaque(base);
   struct kk_opaque_fd *record = calloc(1, sizeof(*record));
   if (!record)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   char name[] = "/tmp/kk-event-XXXXXX";
   int fd = mkostemp(name, O_CLOEXEC);
   if (fd < 0) {
      free(record);
      return VK_ERROR_TOO_MANY_OBJECTS;
   }
   int witness = open(name, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
   unlink(name);
   struct stat st;
   if (witness < 0 ||
       fstat(fd, &st) != 0 || flock(fd, LOCK_SH | LOCK_NB) != 0) {
      if (witness >= 0) close(witness);
      close(fd);
      free(record);
      return VK_ERROR_TOO_MANY_OBJECTS;
   }
   record->key = (struct kk_opaque_key){st.st_dev, st.st_ino};
   record->witness = witness;
   record->payload = payload_ref(sync->payload);
   simple_mtx_lock(&registry_mutex);
   registry_reap_locked();
   if (!registry)
      registry = _mesa_hash_table_create(NULL, key_hash, key_equal);
   struct hash_entry *entry = registry ?
      _mesa_hash_table_insert(registry, &record->key, record) : NULL;
   simple_mtx_unlock(&registry_mutex);
   if (!entry) {
      close(witness);
      close(fd);
      payload_unref(record->payload);
      free(record);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   *out_fd = fd;
   return VK_SUCCESS;
}

static const struct vk_sync_type opaque_type = {
   .size = sizeof(struct kk_opaque_sync),
   .features = VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_TIMELINE |
               VK_SYNC_FEATURE_GPU_WAIT | VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_SIGNAL | VK_SYNC_FEATURE_CPU_RESET |
               VK_SYNC_FEATURE_WAIT_PENDING | VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL,
   .init = opaque_init,
   .finish = opaque_finish,
   .signal = opaque_signal,
   .get_value = opaque_get_value,
   .reset = opaque_reset,
   .move = opaque_move,
   .wait = opaque_wait,
   .import_opaque_fd = opaque_import,
   .export_opaque_fd = opaque_export,
};

static VkResult
opaque_submit(struct vk_queue *queue, struct vk_queue_submit *submit)
{
   /* Convert once in the existing retained packet. A deferred resume sees
    * native points, so it cannot consume/reserve another binary generation. */
   for (uint32_t i = 0; i < submit->wait_count; i++) {
      struct vk_sync_wait *wait = &submit->waits[i];
      if (wait->sync->type != &opaque_type)
         continue;
      struct kk_opaque_sync *sync = to_opaque(wait->sync);
      if (!(sync->base.flags & VK_SYNC_IS_TIMELINE))
         wait->wait_value = binary_point(sync, true);
      wait->sync = &sync->payload->native.base;
   }
   for (uint32_t i = 0; i < submit->signal_count; i++) {
      struct vk_sync_signal *signal = &submit->signals[i];
      if (signal->sync->type != &opaque_type)
         continue;
      struct kk_opaque_sync *sync = to_opaque(signal->sync);
      if (!(sync->base.flags & VK_SYNC_IS_TIMELINE))
         signal->signal_value = binary_signal_point(sync);
      signal->sync = &sync->payload->native.base;
   }
   struct kk_queue *kk_queue = container_of(queue, struct kk_queue, vk);
   return kk_queue->native_submit(queue, submit);
}

static void
opaque_activate(struct kk_device *dev)
{
   /* Vulkan allows creation/import concurrently with submission on another
    * queue. Callback readers use a relaxed atomic pointer load: the ordinary
    * path still performs one load/call and no feature check or sync scan. */
   for (uint32_t i = 0; i < dev->queue_count; i++)
      __atomic_store_n(&dev->queues[i].vk.driver_submit, opaque_submit,
                       __ATOMIC_RELAXED);
}

static VKAPI_ATTR VkResult VKAPI_CALL
opaque_create_semaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
                        const VkAllocationCallbacks *alloc, VkSemaphore *out)
{
   const VkExportSemaphoreCreateInfo *export =
      vk_find_struct_const(info->pNext, EXPORT_SEMAPHORE_CREATE_INFO);
   if (!export || !(export->handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT))
      return vk_common_CreateSemaphore(device, info, alloc, out);
   if (export->handleTypes != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   VK_FROM_HANDLE(kk_device, dev, device);
   const VkSemaphoreTypeCreateInfo *type =
      vk_find_struct_const(info->pNext, SEMAPHORE_TYPE_CREATE_INFO);
   bool timeline = type && type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
   size_t size = offsetof(struct vk_semaphore, permanent) + sizeof(struct kk_opaque_sync);
   struct vk_semaphore *semaphore = vk_object_zalloc(&dev->vk, alloc, size,
                                                     VK_OBJECT_TYPE_SEMAPHORE);
   if (!semaphore)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   semaphore->type = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY;
   VkResult result = vk_sync_init(&dev->vk, &semaphore->permanent, &opaque_type,
      VK_SYNC_IS_SHAREABLE | (timeline ? VK_SYNC_IS_TIMELINE : 0),
      timeline ? type->initialValue : 0);
   if (result != VK_SUCCESS) {
      vk_object_free(&dev->vk, alloc, semaphore);
      return result;
   }
   opaque_activate(dev);
   *out = vk_semaphore_to_handle(semaphore);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
opaque_import_semaphore(VkDevice device, const VkImportSemaphoreFdInfoKHR *info)
{
   if (info->handleType != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT)
      return vk_common_ImportSemaphoreFdKHR(device, info);
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(vk_semaphore, semaphore, info->semaphore);
   if ((info->flags & ~VK_SEMAPHORE_IMPORT_TEMPORARY_BIT) ||
       ((info->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT) &&
        semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   struct vk_sync *candidate;
   VkResult result = vk_sync_create(&dev->vk, &opaque_type,
      semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE ? VK_SYNC_IS_TIMELINE : 0,
      0, &candidate);
   if (result != VK_SUCCESS)
      return result;
   result = vk_sync_import_opaque_fd(&dev->vk, candidate, info->fd);
   if (result != VK_SUCCESS) {
      vk_sync_destroy(&dev->vk, candidate);
      return result;
   }
   vk_semaphore_reset_temporary(&dev->vk, semaphore);
   if (info->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT) {
      semaphore->temporary = candidate;
   } else {
      vk_sync_finish(&dev->vk, &semaphore->permanent);
      memcpy(&semaphore->permanent, candidate, sizeof(struct kk_opaque_sync));
      vk_free(&dev->vk.alloc, candidate);
   }
   opaque_activate(dev);
   close(info->fd);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
opaque_get_semaphore_fd(VkDevice device, const VkSemaphoreGetFdInfoKHR *info, int *fd)
{
   if (info->handleType != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT)
      return vk_common_GetSemaphoreFdKHR(device, info, fd);
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(vk_semaphore, semaphore, info->semaphore);
   struct vk_sync *sync = vk_semaphore_get_active_sync(semaphore);
   if (sync->type != &opaque_type || !(sync->flags & VK_SYNC_IS_SHAREABLE))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   return vk_sync_export_opaque_fd(&dev->vk, sync, fd);
}

void
kk_opaque_sync_init_device(struct kk_device *dev)
{
   if (!KK_EXPERIMENTAL(OPAQUE_SYNC))
      return;
   dev->vk.dispatch_table.CreateSemaphore = opaque_create_semaphore;
   dev->vk.dispatch_table.ImportSemaphoreFdKHR = opaque_import_semaphore;
   dev->vk.dispatch_table.GetSemaphoreFdKHR = opaque_get_semaphore_fd;
   dev->exposed_dispatch_table.CreateSemaphore = opaque_create_semaphore;
   dev->exposed_dispatch_table.ImportSemaphoreFdKHR = opaque_import_semaphore;
   dev->exposed_dispatch_table.GetSemaphoreFdKHR = opaque_get_semaphore_fd;
}

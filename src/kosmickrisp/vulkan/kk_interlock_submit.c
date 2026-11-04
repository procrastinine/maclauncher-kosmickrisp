/* SPDX-License-Identifier: MIT */
#include "kk_interlock_submit.h"
#include "kk_scene.h"
#include "kk_depth_bounds.h"
#include "kk_interlock_heap.h"
#include "kk_device.h"
#include "kk_queue.h"
#include "kk_cmd_buffer.h"
#include "kk_sync.h"
#include "vk_alloc.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "c11/threads.h"
#include "util/u_atomic.h"
#include <stdatomic.h>
#include <string.h>

struct kk_interlock_registry {
   mtx_t mutex;
   struct list_head queues;
   struct list_head syncs;
   bool closing;
};

struct kk_interlock_queue {
   struct list_head link;
   struct kk_queue *queue;
   struct mtl_interlock_context *native;
   cnd_t drained;
   uint32_t packets;
   bool closing;
};

VkResult
kk_interlock_submission_init(struct kk_device *dev)
{
   if (!dev->vk.enabled_features.fragmentShaderPixelInterlock &&
       !dev->vk.enabled_features.meshShader && !kk_depth_bounds_emulated(dev))
      return VK_SUCCESS;
   struct kk_interlock_registry *r = vk_zalloc(&dev->vk.alloc, sizeof(*r), 8,
                                               VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!r)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (mtx_init(&r->mutex, mtx_plain) != thrd_success) {
      vk_free(&dev->vk.alloc, r);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   list_inithead(&r->queues);
   list_inithead(&r->syncs);
   dev->interlock_registry = r;
   return VK_SUCCESS;
}

VkResult
kk_interlock_submission_lost(struct kk_device *dev)
{
   vk_device_set_lost(&dev->vk, "Fragment interlock submission failed");
   struct kk_interlock_registry *r = dev->interlock_registry;
   if (!r)
      return VK_ERROR_DEVICE_LOST;
   mtx_lock(&r->mutex);
   list_for_each_entry(struct kk_sync_timeline, sync, &r->syncs, interlock_link)
      mtl_shared_event_set_signaled_value(sync->mtl_handle, UINT64_MAX);
   list_for_each_entry(struct kk_interlock_queue, q, &r->queues, link)
      mtl_interlock_context_cancel(q->native);
   mtx_unlock(&r->mutex);
   return VK_ERROR_DEVICE_LOST;
}

static void
kk_interlock_native_lost(void *data, const char *message)
{
   kk_interlock_submission_lost(data);
}

void
kk_interlock_sync_init(struct kk_device *dev, struct kk_sync_timeline *sync)
{
   struct kk_interlock_registry *r = dev->interlock_registry;
   sync->interlock_registered = false;
   if (!r)
      return;
   mtx_lock(&r->mutex);
   list_addtail(&sync->interlock_link, &r->syncs);
   sync->interlock_registered = true;
   /* A concurrent loss must not leave a newly created event unsignalled. */
   if (vk_device_is_lost(&dev->vk))
      mtl_shared_event_set_signaled_value(sync->mtl_handle, UINT64_MAX);
   mtx_unlock(&r->mutex);
}

void
kk_interlock_sync_finish(struct kk_device *dev, struct kk_sync_timeline *sync)
{
   if (!sync->interlock_registered)
      return;
   struct kk_interlock_registry *r = dev->interlock_registry;
   mtx_lock(&r->mutex);
   list_del(&sync->interlock_link);
   sync->interlock_registered = false;
   mtx_unlock(&r->mutex);
}

/* Registration precedes capture. Success transfers one off-thread resume or
 * cancellation; synchronous failure leaves callback ownership with caller. */

struct kk_interlock_packet {
   atomic_uint references;
   atomic_uint native_pending;
   atomic_bool terminal;
   atomic_bool completion_claimed;
   atomic_int error;
   _Atomic(mtl_feedback_handler_callback) completed;
   void *callback_data;
   struct kk_scene_submission *scene_submission;
   struct kk_device *device;
   struct kk_queue *queue;
   struct kk_interlock_queue *owner;
   struct mtl_interlock_context *context;
   uint32_t count;
   struct mtl_interlock_segment *segments;
   mtl_command_buffer **tail;
   uint32_t tail_count;
   mtl_argument_table *argument_table;
   void **drawables;
   uint32_t drawable_count;
   atomic_bool drawables_waited;
   atomic_bool drawables_signalled;
};

struct kk_interlock_stage {
   atomic_uint references;
   struct kk_interlock_packet *packet;
   struct vk_sync *ready;
   uint32_t index;
};

static VkResult
kk_interlock_retain_drawables(struct kk_interlock_packet *p,
   const struct kk_cmd_buffer *cmd)
{
   uint32_t retained = util_dynarray_num_elements(&cmd->submit_drawables, void *);
   void **drawables = util_dynarray_begin(&cmd->submit_drawables);
   bool primary_retained = false;
   for (uint32_t i = 0; i < retained; ++i)
      primary_retained |= drawables[i] == cmd->drawable;
   uint32_t count = retained + (cmd->drawable && !primary_retained);
   if (!count)
      return VK_SUCCESS;
   p->drawables = vk_alloc(&p->device->vk.alloc,
      (size_t)count * sizeof(*p->drawables), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!p->drawables)
      return VK_ERROR_DEVICE_LOST;
   for (uint32_t i = 0; i < count; ++i)
      p->drawables[i] = mtl_retain(i < retained ? drawables[i] : cmd->drawable);
   p->drawable_count = count;
   return VK_SUCCESS;
}

static void
kk_interlock_wait_drawables(struct kk_interlock_packet *p)
{
   if (!p->drawable_count)
      return;
   for (uint32_t i = 0; i < p->drawable_count; ++i)
      mtl_command_queue_wait_for_drawable(p->queue->mtl_handle, p->drawables[i]);
   atomic_store_explicit(&p->drawables_waited, true, memory_order_release);
}

static void
kk_interlock_signal_drawables(struct kk_interlock_packet *p)
{
   if (!atomic_load_explicit(&p->drawables_waited, memory_order_acquire) ||
       atomic_exchange(&p->drawables_signalled, true))
      return;
   for (uint32_t i = 0; i < p->drawable_count; ++i)
      mtl_command_queue_signal_drawable(p->queue->mtl_handle, p->drawables[i]);
}

static void
kk_interlock_release_command_drawables(struct kk_cmd_buffer *cmd)
{
   util_dynarray_foreach(&cmd->submit_drawables, void *, drawable)
      mtl_release(*drawable);
   util_dynarray_clear(&cmd->submit_drawables);
   if (cmd->drawable) {
      mtl_release(cmd->drawable);
      cmd->drawable = NULL;
   }
}

static void
kk_interlock_packet_ref(struct kk_interlock_packet *p)
{
   atomic_fetch_add_explicit(&p->references, 1, memory_order_relaxed);
}

static void
kk_interlock_packet_unref(struct kk_interlock_packet *p)
{
   if (atomic_fetch_sub_explicit(&p->references, 1, memory_order_acq_rel) != 1)
      return;
   kk_scene_submission_release(p->scene_submission);
   struct kk_device *dev = p->device;
   struct kk_interlock_registry *registry = dev->interlock_registry;
   struct kk_interlock_queue *owner = p->owner;
   /* Never invoke application callbacks from a destructor: the last starter
    * reference may be released while the re-record caller holds queue->mutex. */
   assert(atomic_load(&p->native_pending) == 0);
   for (uint32_t i = 0; i < p->count; i++) {
      if (p->segments[i].release)
         p->segments[i].release(p->segments[i].data);
      mtl_release(p->segments[i].capture);
   }
   for (uint32_t i = 0; i < p->tail_count; ++i)
      mtl_release(p->tail[i]);
   vk_free(&dev->vk.alloc, p->tail);
   mtl_release(p->argument_table);
   for (uint32_t i = 0; i < p->drawable_count; ++i)
      mtl_release(p->drawables[i]);
   vk_free(&dev->vk.alloc, p->drawables);
   vk_free(&dev->vk.alloc, p->segments);
   vk_free(&dev->vk.alloc, p);
   if (owner) {
      /* This is the last operation: dropping the count permits device/meta
       * teardown. No application/native callback runs with this lock held. */
      mtx_lock(&registry->mutex);
      assert(owner->packets > 0);
      if (--owner->packets == 0)
         cnd_broadcast(&owner->drained);
      mtx_unlock(&registry->mutex);
   }
}

static void
kk_interlock_packet_error(struct kk_interlock_packet *p, int error)
{
   int none = MTL_COMMAND_QUEUE_ERROR_NONE;
   atomic_compare_exchange_strong(&p->error, &none,
      error ? error : MTL_COMMAND_QUEUE_ERROR_INTERNAL);
}

/* The bridge's existing completion_queue/group defers callback delivery;
 * it is not a submission scheduler. The extra packet reference survives a
 * starter-thread final release while the re-record mutex is still held. */
static void
kk_interlock_packet_application_completed(struct mtl_feedback_data *feedback)
{
   struct kk_interlock_packet *p = feedback->user_data;
   /* On cancellation, finish acquired drawables only after native work has
    * drained. Normal completion already signalled them behind the tail. */
   kk_interlock_signal_drawables(p);
   mtl_feedback_handler_callback callback = atomic_exchange(&p->completed, NULL);
   if (callback) {
      struct mtl_feedback_data application = *feedback;
      application.user_data = p->callback_data;
      callback(&application);
   }
   kk_interlock_packet_unref(p); /* async app-completion reference */
}

static void
kk_interlock_packet_complete(struct kk_interlock_packet *p)
{
   if (!atomic_load_explicit(&p->terminal, memory_order_acquire) ||
       atomic_load_explicit(&p->native_pending, memory_order_acquire) != 0)
      return;
   bool unclaimed = false;
   if (!atomic_compare_exchange_strong(&p->completion_claimed, &unclaimed, true))
      return;
   if (!atomic_load(&p->completed))
      return; /* A synchronous failure never transferred callback/data. */
   kk_interlock_packet_ref(p);
   mtl_interlock_complete_async(p->context,
      kk_interlock_packet_application_completed, p, atomic_load(&p->error));
}

static void
kk_interlock_stage_ref(struct kk_interlock_stage *s)
{
   atomic_fetch_add_explicit(&s->references, 1, memory_order_relaxed);
}

static void
kk_interlock_stage_unref(struct kk_interlock_stage *s)
{
   if (atomic_fetch_sub_explicit(&s->references, 1, memory_order_acq_rel) != 1)
      return;
   struct kk_interlock_packet *p = s->packet;
   vk_sync_destroy(&p->device->vk, s->ready);
   vk_free(&p->device->vk.alloc, s);
   kk_interlock_packet_unref(p);
}

static void
kk_interlock_stage_native_completed(struct mtl_feedback_data *feedback)
{
   struct kk_interlock_stage *s = feedback->user_data;
   struct kk_interlock_packet *p = s->packet;
   if (feedback->error)
      kk_interlock_packet_error(p, feedback->error);
   /* Fresh private timeline was registered at zero before native commit.
    * This callback occurs only after every capture/auxiliary GPU completion. */
   if (!vk_device_is_lost(&p->device->vk) && !atomic_load(&p->terminal)) {
      VkResult signaled = vk_sync_signal(&p->device->vk, s->ready, 1);
      if (signaled != VK_SUCCESS) {
         kk_interlock_packet_error(p, MTL_COMMAND_QUEUE_ERROR_INTERNAL);
         kk_interlock_submission_lost(p->device);
      }
   }
   /* Visible loss/cancel already owns the shared cancelled resume. Do not
    * lower a point that the canonical loss path released with UINT64_MAX. */
   /* Cancelled resume may already have returned; its reference alone must
    * never free ready/data or run the app callback before this native drain. */
   atomic_fetch_sub_explicit(&p->native_pending, 1, memory_order_acq_rel);
   kk_interlock_packet_complete(p);
   kk_interlock_stage_unref(s); /* native-completion reference */
}

static void
kk_interlock_packet_tail_completed(struct mtl_feedback_data *feedback)
{
   struct kk_interlock_packet *p = feedback->user_data;
   if (feedback->error)
      kk_interlock_packet_error(p, feedback->error);
   atomic_fetch_sub_explicit(&p->native_pending, 1, memory_order_acq_rel);
   atomic_store_explicit(&p->terminal, true, memory_order_release);
   kk_interlock_packet_complete(p);
   kk_interlock_packet_unref(p); /* actual tail-completion reference */
}

static VkResult kk_interlock_packet_start_stage(struct kk_interlock_packet *, uint32_t);

static VkResult
kk_interlock_stage_resume(struct kk_queue *queue, void *data, bool cancelled)
{
   struct kk_interlock_stage *s = data;
   struct kk_interlock_packet *p = s->packet;
   assert(queue == p->queue);
   VkResult result = VK_SUCCESS;
   if (cancelled || vk_device_is_lost(&p->device->vk) || atomic_load(&p->error)) {
      kk_interlock_packet_error(p, MTL_COMMAND_QUEUE_ERROR_INTERNAL);
      atomic_store_explicit(&p->terminal, true, memory_order_release);
      result = VK_ERROR_DEVICE_LOST;
   } else if (s->index + 1 < p->count) {
      result = kk_interlock_packet_start_stage(p, s->index + 1);
      if (result != VK_SUCCESS) {
         kk_interlock_packet_error(p, MTL_COMMAND_QUEUE_ERROR_INTERNAL);
         atomic_store_explicit(&p->terminal, true, memory_order_release);
      }
   } else {
      /* No private event wait, FIFO, or app signal/fence handling. The common
       * owner resumes its original submit cursor after this tail commit. */
      mtl_commit_options *options = mtl_new_commit_options();
      if (!options) {
         kk_interlock_packet_error(p, MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY);
         atomic_store_explicit(&p->terminal, true, memory_order_release);
         result = VK_ERROR_DEVICE_LOST;
      } else {
         kk_interlock_packet_ref(p);
         atomic_fetch_add(&p->native_pending, 1);
         mtl_commit_options_add_feedback_handler(options,
            kk_interlock_packet_tail_completed, p);
         mtl_command_queue_commit(queue->mtl_handle, p->tail, p->tail_count, options);
         kk_scene_submission_committed(p->scene_submission, queue);
         kk_interlock_signal_drawables(p);
         mtl_release(options);
      }
   }
   kk_interlock_packet_complete(p);
   kk_interlock_stage_unref(s); /* one transferred shared-resume reference */
   return result;
}

static VkResult
kk_interlock_packet_start_stage(struct kk_interlock_packet *p, uint32_t index)
{
   struct kk_device *dev = p->device;
   struct kk_interlock_stage *s = vk_zalloc(&dev->vk.alloc, sizeof(*s), 8,
                                          VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!s)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   atomic_init(&s->references, 1); /* local launch reference */
   s->packet = p;
   s->index = index;
   VkResult result = vk_sync_create(&dev->vk, &kk_sync_type,
                                    VK_SYNC_IS_TIMELINE, 0, &s->ready);
   if (result != VK_SUCCESS) {
      vk_free(&dev->vk.alloc, s);
      return result;
   }
   kk_interlock_packet_ref(p); /* stage lifetime */
   kk_interlock_stage_ref(s); /* native callback reservation */
   kk_interlock_stage_ref(s); /* shared resume reservation */
   atomic_fetch_add(&p->native_pending, 1);
   /* Owner-approved preregistration before native GPU commit.
    * Cancellation may race even this starter. Reserved native/launch refs
    * keep ready, recorded data and app ownership alive until start resolves.
    * A failure here has NO GPU work, so caller callback/data stay owned. */
   result = kk_queue_defer(p->queue, s->ready, kk_interlock_stage_resume, s);
   if (result != VK_SUCCESS) {
      atomic_fetch_sub(&p->native_pending, 1);
      kk_interlock_stage_unref(s); /* no resume transferred */
      kk_interlock_stage_unref(s); /* no native completion transferred */
      kk_interlock_stage_unref(s); /* local launch */
      return result;
   }
   /* From this point SUCCESS must be returned: exactly one shared resume
    * owns the app completion, including native-start failure/cancellation.
    * C packet/context preparation and packet snapshots preceded the
    * registration. run_segment false must commit nothing/transfer no callback;
    * any partially committed native path must return true and drain its one
    * completion callback even if capture/auxiliary encoding later fails. */
   if (index == 0 && !atomic_load(&p->terminal) && !vk_device_is_lost(&dev->vk))
      kk_interlock_wait_drawables(p);
   struct kk_interlock_segment *recorded = p->segments[index].data;
   bool native_accepted = !atomic_load(&p->terminal) &&
      !vk_device_is_lost(&dev->vk) && mtl_interlock_run_segment_commands(p->context,
         &p->segments[index], recorded->commands, recorded->command_count,
         kk_interlock_stage_native_completed, s);
   if (!native_accepted) {
      kk_interlock_packet_error(p, MTL_COMMAND_QUEUE_ERROR_INTERNAL);
      atomic_fetch_sub(&p->native_pending, 1);
      kk_interlock_stage_unref(s); /* no native completion transferred */
      /* Common owner supplies the one off-thread cancelled resume. Never
       * invoke the application completion on this re-record caller thread. */
      kk_interlock_submission_lost(dev);
   }
   /* If cancellation already consumed resume before native-start resolved,
    * this starter may be the final drain. Completion still only dispatches
    * asynchronously through the bridge; it never invokes app code inline. */
   kk_interlock_packet_complete(p);
   kk_interlock_stage_unref(s); /* local launch reference */
   return VK_SUCCESS;
}

VkResult
kk_interlock_commit(struct kk_queue *queue, struct kk_cmd_buffer *cmd,
                    mtl_feedback_handler_callback callback, void *data)
{
   struct kk_device *dev = kk_queue_device(queue);
   struct kk_interlock_registry *r = dev->interlock_registry;
   if (!r || vk_device_is_lost(&dev->vk))
      return VK_ERROR_DEVICE_LOST;
   uint32_t count = 0;
   for (struct kk_interlock_segment *s = cmd->interlock_segments; s; s = s->next)
      ++count;
   if (!count)
      return VK_ERROR_DEVICE_LOST;
   struct kk_interlock_packet *p = vk_zalloc(&dev->vk.alloc, sizeof(*p), 8,
                                            VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!p)
      return VK_ERROR_DEVICE_LOST;
   p->segments = vk_alloc(&dev->vk.alloc, (size_t)count * sizeof(*p->segments),
                          8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!p->segments) {
      vk_free(&dev->vk.alloc, p);
      return VK_ERROR_DEVICE_LOST;
   }
   atomic_init(&p->references, 1); /* caller/launch owner, never invokes callback */
   atomic_init(&p->native_pending, 0);
   atomic_init(&p->terminal, false);
   atomic_init(&p->completion_claimed, false);
   atomic_init(&p->error, MTL_COMMAND_QUEUE_ERROR_NONE);
   atomic_init(&p->completed, callback);
   atomic_init(&p->drawables_waited, false);
   atomic_init(&p->drawables_signalled, false);
   p->callback_data = data;
   p->scene_submission = kk_scene_submission_acquire(callback, data);
   p->device = dev;
   p->queue = queue;
   p->count = count;
   uint32_t i = 0;
   for (struct kk_interlock_segment *s = cmd->interlock_segments; s; s = s->next) {
      p->segments[i] = s->native;
      p->segments[i].capture = mtl_retain(s->native.capture);
      if (p->segments[i].retain)
         p->segments[i].retain(p->segments[i].data);
      ++i;
   }
   uint32_t preceding = util_dynarray_num_elements(&cmd->mesh_command_buffers,
      mtl_command_buffer *);
   p->tail = vk_alloc(&dev->vk.alloc, (size_t)(preceding + 1) * sizeof(*p->tail),
      8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!p->tail) {
      atomic_store(&p->completed, NULL);
      kk_interlock_packet_unref(p);
      return VK_ERROR_DEVICE_LOST;
   }
   mtl_command_buffer **commands = util_dynarray_begin(&cmd->mesh_command_buffers);
   for (uint32_t j = 0; j <= preceding; ++j)
      p->tail[j] = mtl_retain(j < preceding ? commands[j] : cmd->metal.cmd_buf);
   p->tail_count = preceding + 1;
   p->argument_table = mtl_retain(cmd->argument_table);
   if (kk_interlock_retain_drawables(p, cmd) != VK_SUCCESS) {
      atomic_store(&p->completed, NULL);
      kk_interlock_packet_unref(p);
      return VK_ERROR_DEVICE_LOST;
   }
   /* Context lookup/creation preserves the existing registry mechanism. */
   mtx_lock(&r->mutex);
   struct kk_interlock_queue *q = r->closing ? NULL : queue->interlock_submission;
   if (!r->closing && !q) {
      q = vk_zalloc(&dev->vk.alloc, sizeof(*q), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (q) {
         q->queue = queue;
         if (cnd_init(&q->drained) != thrd_success) {
            vk_free(&dev->vk.alloc, q);
            q = NULL;
         } else {
            q->native = mtl_interlock_context_create(dev->mtl_handle,
               queue->mtl_handle, dev->residency_set.handle,
               kk_interlock_native_lost, dev);
            if (!q->native) {
               cnd_destroy(&q->drained);
               vk_free(&dev->vk.alloc, q);
               q = NULL;
            } else {
               queue->interlock_submission = q;
               list_addtail(&q->link, &r->queues);
            }
         }
      }
   }
   if (q && q->closing)
      q = NULL;
   if (q) {
      assert(q->packets < UINT32_MAX);
      ++q->packets;
      p->owner = q;
      p->context = q->native;
   }
   mtx_unlock(&r->mutex);
   VkResult result = q ? kk_interlock_packet_start_stage(p, 0) : VK_ERROR_DEVICE_LOST;
   if (result != VK_SUCCESS) {
      /* Registration/context/allocation failure preceded all native work.
       * No app callback/data ownership transferred; no pool/upload ownership
       * can be stranded behind an already committed native callback. */
      assert(atomic_load(&p->native_pending) == 0);
      atomic_store(&p->completed, NULL);
      p->callback_data = NULL;
      atomic_store_explicit(&p->terminal, true, memory_order_release);
   } else {
      /* Registration owns the packet now. Its independent retains cover the
       * first wait, every continuation and the final drawable signal. */
      kk_interlock_release_command_drawables(cmd);
   }
   kk_interlock_packet_unref(p);
   /* The caller unwinds its re-record ownership before marking loss. */
   return result == VK_SUCCESS ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

void
kk_interlock_submission_drain(struct kk_device *dev)
{
   struct kk_interlock_registry *r = dev->interlock_registry;
   if (!r)
      return;
   /* Common Mesa workers have already been joined or cancelled.
    * A native group alone can be zero between C stages or before native start.
    * Every C packet is counted from pre-registration through final release. */
   mtx_lock(&r->mutex);
   r->closing = true;
   mtx_unlock(&r->mutex);
   if (vk_device_is_lost(&dev->vk))
      kk_interlock_submission_lost(dev);
   mtx_lock(&r->mutex);
   list_for_each_entry(struct kk_interlock_queue, q, &r->queues, link) {
      q->closing = true;
      while (q->packets)
         cnd_wait(&q->drained, &r->mutex);
   }
   mtx_unlock(&r->mutex);
   /* Native callback epilogues may still leave their group after releasing
    * the final C packet; callbacks run without the registry mutex. */
   list_for_each_entry(struct kk_interlock_queue, q, &r->queues, link)
      mtl_interlock_context_drain(q->native);
}

void
kk_interlock_queue_finish(struct kk_queue *queue)
{
   /* The common worker is joined/cancelled before this hook, with all shared
    * meta/device state and the re-record mutex/pool still alive. */
   struct kk_interlock_queue *q = queue->interlock_submission;
   if (!q)
      return;
   struct kk_device *dev = kk_queue_device(queue);
   struct kk_interlock_registry *r = dev->interlock_registry;
   mtx_lock(&r->mutex);
   q->closing = true;
   while (q->packets)
      cnd_wait(&q->drained, &r->mutex);
   mtx_unlock(&r->mutex);
   mtl_interlock_context_finish(q->native);
   mtx_lock(&r->mutex);
   list_del(&q->link);
   queue->interlock_submission = NULL;
   mtx_unlock(&r->mutex);
   cnd_destroy(&q->drained);
   vk_free(&dev->vk.alloc, q);
}

void
kk_interlock_submission_finish(struct kk_device *dev)
{
   struct kk_interlock_registry *r = dev->interlock_registry;
   if (!r)
      return;
   assert(list_is_empty(&r->queues));
   assert(list_is_empty(&r->syncs));
   mtx_destroy(&r->mutex);
   vk_free(&dev->vk.alloc, r);
   dev->interlock_registry = NULL;
}

static void
kk_interlock_segment_retain(void *data)
{
   struct kk_interlock_segment *s = data;
   p_atomic_inc(&s->references);
}

static void
kk_interlock_segment_release(void *data)
{
   struct kk_interlock_segment *s = data;
   if (!p_atomic_dec_zero(&s->references))
      return;
   if (s->finish)
      s->finish(s->device, s->data);
   kk_interlock_heap_unref(s->heap);
   vk_free(&s->device->vk.alloc, s->commands);
   vk_free(&s->device->vk.alloc, s);
}

static bool
kk_interlock_segment_encode(void *data, mtl_command_queue *queue,
   mtl_feedback_handler_callback completed, void *callback_data)
{
   struct kk_interlock_segment *s = data;
   return s->encode(s->data, queue, completed, callback_data);
}

void
kk_interlock_segments_finish(struct kk_cmd_buffer *cmd)
{
   while (cmd->interlock_segments) {
      struct kk_interlock_segment *s = cmd->interlock_segments;
      cmd->interlock_segments = s->next;
      for (uint32_t i = 0; i < s->command_count; ++i)
         kk_cmd_pool_return_cmd_buf(kk_cmd_buffer_pool(cmd), s->commands[i]);
      kk_interlock_segment_release(s);
   }
}

VkResult
kk_interlock_split(struct kk_cmd_buffer *cmd, void *data,
                     mtl_interlock_encode_callback encode,
                     void (*finish)(struct kk_device *, void *))
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);
   struct kk_interlock_segment *segment = vk_zalloc(&dev->vk.alloc,
      sizeof(*segment), 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!segment)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   uint32_t preceding = util_dynarray_num_elements(&cmd->mesh_command_buffers,
      mtl_command_buffer *);
   segment->commands = vk_alloc(&dev->vk.alloc,
      (size_t)(preceding + 1) * sizeof(*segment->commands), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!segment->commands) {
      vk_free(&dev->vk.alloc, segment);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   mtl_command_buffer *tail = kk_cmd_pool_get_cmd_buf(pool);
   if (!tail) {
      vk_free(&dev->vk.alloc, segment->commands);
      vk_free(&dev->vk.alloc, segment);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   segment->native = (struct mtl_interlock_segment){
      .capture = cmd->metal.cmd_buf, .data = segment,
      .encode = kk_interlock_segment_encode,
      .retain = kk_interlock_segment_retain,
      .release = kk_interlock_segment_release,
   };
   segment->device = dev;
   segment->references = 1;
   segment->data = data;
   segment->encode = encode;
   segment->finish = finish;
   segment->heap = kk_interlock_heap_ref(cmd->interlock_heap_active ?
      cmd->interlock_heap : NULL);
   if (cmd->metal.render)
      kk_apply_attachment_store_ops(cmd, true);
   /* cs_end publishes every encoder, including deferred post-render writes. */
   cs_end(cmd);
   kk_cmd_buffer_dirty_all_gfx(cmd);
   cmd->state.gfx.need_to_start_render_pass = true;
   mtl_end_command_buffer(cmd->metal.cmd_buf);
   if (preceding)
      memcpy(segment->commands, util_dynarray_begin(&cmd->mesh_command_buffers),
         (size_t)preceding * sizeof(*segment->commands));
   segment->commands[preceding] = cmd->metal.cmd_buf;
   segment->command_count = preceding + 1;
   /* Transfer only the ended command buffers at this boundary. Allocators
    * retain the command buffer's normal lifetime and reset path. */
   util_dynarray_clear(&cmd->mesh_command_buffers);
   if (cmd->interlock_segments) {
      cmd->interlock_segments->tail->next = segment;
      cmd->interlock_segments->tail = segment;
   } else {
      segment->tail = segment;
      cmd->interlock_segments = segment;
   }
   cmd->metal.cmd_buf = tail;
   mtl_begin_command_buffer(tail, cmd->metal.allocator);
   return VK_SUCCESS;
}

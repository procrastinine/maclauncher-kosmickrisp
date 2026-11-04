/* SPDX-License-Identifier: MIT */
/* Temporal frames that VKD3D-Proton's NGX core records as a kk_temporal_frame_info on a vkCmdPipelineBarrier2,
 * forwarded in command order to MacLauncher's graphics pipeline. The pipeline's ordered scene work (effects and
 * frame generation) records classic Metal commands, so each frame runs on a classic command buffer between two
 * of the Vulkan command buffer's native MTL4 parts: the queue signals an event after the earlier parts, the
 * classic command buffer waits for it, encodes the frame and signals, and the later parts wait for that.
 * KosmicKrisp's images are untracked; the pipeline writes a tracked image of the feature, copied into the
 * frame's output at the end once it holds a processed frame. Nothing here encodes MetalFX itself. Metal
 * objects are created synchronously. */
#include "kk_temporal.h"
#include "kk_cmd_buffer.h"
#include "kk_device.h"
#include "kk_dispatch_cmd.h"
#include "kk_format.h"
#include "kk_image.h"
#include "kk_queue.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_command_queue.h"
#include "scene/temporal-frame.h"
#include "util/log.h"
#include "util/simple_mtx.h"
#include "util/u_call_once.h"
#include "util/u_atomic.h"
#include "vk_cmd_queue.h"
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <dlfcn.h>
#include <math.h>
#include <time.h>

struct kk_temporal_pipeline {
   int (*process)(const RSTemporalFrame *, RSTemporalResult *);
   uint32_t (*method)(void);
   void (*release)(void *owner);
};

/* One per NGX feature: the pipeline's history key, and the tracked image it writes. */
struct kk_temporal_owner {
   uint32_t references;
   uint64_t feature_id;
   id<MTLTexture> output;
   double previous_time;
   bool output_valid; /* see kk_temporal_tracked_valid */
   bool reported;
};

struct kk_temporal {
   simple_mtx_t mutex;
   struct util_dynarray owners; /* struct kk_temporal_owner *, one reference each */
};

struct kk_temporal_record {
   uint32_t part; /* the first native part after the frame */
   uint32_t feature_type;
   struct kk_temporal_owner *owner;
   RSTemporalFrame frame; /* values; the images are in textures */
   id<MTLTexture> textures[KK_TEMPORAL_IMAGES];
};

/* The pipeline, when its library is loaded in this process; resolved once. */
static struct kk_temporal_pipeline kk_temporal_symbols;
static bool kk_temporal_loaded;

static void
kk_temporal_resolve(void)
{
   void *process = dlsym(RTLD_DEFAULT, "rsProcessTemporalFrame");
   Dl_info info;
   if (!process || !dladdr(process, &info) || !info.dli_fname)
      return;
   /* Keep that exact library: its frame history outlives any later unload request. */
   void *library = dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE | RTLD_NOLOAD);
   if (!library)
      return;
   kk_temporal_symbols.process = dlsym(library, "rsProcessTemporalFrame");
   kk_temporal_symbols.method = dlsym(library, "rsGetTemporalMethod");
   kk_temporal_symbols.release = dlsym(library, "rsReleaseTemporalOwner");
   kk_temporal_loaded = kk_temporal_symbols.process && kk_temporal_symbols.method &&
                        kk_temporal_symbols.release;
   mesa_logi("Temporal frames: graphics pipeline %s (%s)", kk_temporal_loaded ? "loaded" : "incomplete",
             info.dli_fname);
}

static const struct kk_temporal_pipeline *
kk_temporal_pipeline(void)
{
   static util_once_flag once = UTIL_ONCE_FLAG_INIT;
   util_call_once(&once, kk_temporal_resolve);
   return kk_temporal_loaded ? &kk_temporal_symbols : NULL;
}

const char *
kk_temporal_driver_info(void *mtl_device)
{
   if (!kk_temporal_pipeline())
      return "";
   /* The pipeline maps DLSS to MetalFX unless another method is selected. */
   bool temporal = false;
   @autoreleasepool {
      if (dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY | RTLD_LOCAL)) {
         Class descriptor = NSClassFromString(@"MTLFXTemporalScalerDescriptor"); /* MetalFX is not linked */
         temporal = descriptor && [descriptor supportsDevice:(id<MTLDevice>)mtl_device];
      }
   }
   return temporal ? " temporal-sr" : "";
}

static const struct kk_temporal_frame_info *
kk_temporal_find(const VkDependencyInfo *dep)
{
   for (const VkBaseInStructure *s = dep->pNext; s; s = s->pNext) {
      if ((uint32_t)s->sType == KK_STRUCTURE_TYPE_TEMPORAL_FRAME_INFO)
         return (const struct kk_temporal_frame_info *)s;
   }
   return NULL;
}

void
kk_temporal_copy_enqueued(struct kk_cmd_buffer *cmd, const VkDependencyInfo *dep)
{
   const struct kk_temporal_frame_info *info = kk_temporal_find(dep);
   if (!info || cmd->record_dispatch != &kk_cmd_enqueue_dispatch ||
       list_is_empty(&cmd->vk.cmd_queue.cmds))
      return;
   struct vk_cmd_queue_entry *entry =
      list_last_entry(&cmd->vk.cmd_queue.cmds, struct vk_cmd_queue_entry, cmd_link);
   if (entry->type != VK_CMD_PIPELINE_BARRIER2)
      return;
   /* The enqueued copy still points at the caller's structure. */
   VkDependencyInfo *copied = (VkDependencyInfo *)entry->u.pipeline_barrier2.dependency_info;
   struct kk_temporal_frame_info *copy = linear_alloc_child(cmd->vk.cmd_queue.ctx, sizeof(*copy));
   uint64_t *ids = linear_alloc_child(cmd->vk.cmd_queue.ctx,
                                      MAX2(info->release_feature_id_count, 1) * sizeof(*ids));
   if (!copy || !ids) {
      copied->pNext = NULL;
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   *copy = *info;
   copy->pNext = NULL;
   if (info->release_feature_id_count)
      memcpy(ids, info->release_feature_ids, info->release_feature_id_count * sizeof(*ids));
   copy->release_feature_ids = ids;
   copied->pNext = copy;
}

static void
kk_temporal_owner_unref(struct kk_temporal_owner *owner)
{
   if (p_atomic_dec_return(&owner->references))
      return;
   kk_temporal_pipeline()->release(owner);
   [owner->output release];
   free(owner);
}

static struct kk_temporal *
kk_temporal_device(struct kk_device *dev)
{
   struct kk_temporal *t = p_atomic_read(&dev->temporal);
   if (t)
      return t;
   t = calloc(1, sizeof(*t));
   if (!t)
      return NULL;
   simple_mtx_init(&t->mutex, mtx_plain);
   t->owners = UTIL_DYNARRAY_INIT;
   struct kk_temporal *existing = p_atomic_cmpxchg(&dev->temporal, NULL, t);
   if (!existing)
      return t;
   simple_mtx_destroy(&t->mutex);
   free(t);
   return existing;
}

/* Returns a new reference to the feature's owner. */
static struct kk_temporal_owner *
kk_temporal_owner(struct kk_temporal *t, const struct kk_temporal_frame_info *info)
{
   struct kk_temporal_owner *found = NULL;
   simple_mtx_lock(&t->mutex);
   for (uint32_t i = 0; i < info->release_feature_id_count; i++) {
      util_dynarray_foreach(&t->owners, struct kk_temporal_owner *, owner) {
         if ((*owner)->feature_id != info->release_feature_ids[i])
            continue;
         kk_temporal_owner_unref(*owner);
         *owner = util_dynarray_pop(&t->owners, struct kk_temporal_owner *);
         break;
      }
   }
   util_dynarray_foreach(&t->owners, struct kk_temporal_owner *, owner) {
      if ((*owner)->feature_id == info->feature_id)
         found = *owner;
   }
   if (!found && (found = calloc(1, sizeof(*found)))) {
      found->references = 1;
      found->feature_id = info->feature_id;
      util_dynarray_append_typed(&t->owners, struct kk_temporal_owner *, found);
   }
   if (found)
      p_atomic_inc(&found->references);
   simple_mtx_unlock(&t->mutex);
   return found;
}

void
kk_temporal_finish(struct kk_device *dev)
{
   struct kk_temporal *t = dev->temporal;
   if (!t)
      return;
   util_dynarray_foreach(&t->owners, struct kk_temporal_owner *, owner)
      kk_temporal_owner_unref(*owner);
   util_dynarray_fini(&t->owners);
   simple_mtx_destroy(&t->mutex);
   free(t);
   dev->temporal = NULL;
}

static void
kk_temporal_record_free(struct kk_temporal_record *r)
{
   for (unsigned i = 0; i < KK_TEMPORAL_IMAGES; i++)
      [r->textures[i] release];
   if (r->owner)
      kk_temporal_owner_unref(r->owner);
   free(r);
}

void
kk_temporal_cmd_reset(struct kk_cmd_buffer *cmd)
{
   util_dynarray_foreach(&cmd->temporal_records, struct kk_temporal_record *, r)
      kk_temporal_record_free(*r);
   util_dynarray_clear(&cmd->temporal_records);
}

static uint32_t
kk_temporal_flags(uint32_t flags)
{
   uint32_t result = 0;
   result |= flags & KK_TEMPORAL_RESET ? RS_TEMPORAL_RESET : 0;
   result |= flags & KK_TEMPORAL_DEPTH_REVERSED ? RS_TEMPORAL_DEPTH_REVERSED : 0;
   result |= flags & KK_TEMPORAL_HDR ? RS_TEMPORAL_HDR : 0;
   result |= flags & KK_TEMPORAL_AUTO_EXPOSURE ? RS_TEMPORAL_AUTO_EXPOSURE : 0;
   result |= flags & KK_TEMPORAL_MOTION_JITTERED ? RS_TEMPORAL_JITTERED_MOTION : 0;
   result |= flags & KK_TEMPORAL_MOTION_DISPLAY_RES ? RS_TEMPORAL_OUTPUT_MOTION : 0;
   return result;
}

/* The camera of RSTemporalCamera: positive planes, a finite far plane beyond the near one unless
 * infinite (far 0), and a vertical field of view in (0, pi). */
static uint32_t
kk_temporal_camera_flags(const struct kk_temporal_frame_info *info)
{
   if (!(info->flags & KK_TEMPORAL_CAMERA_VALID) || !isfinite(info->near_plane) || info->near_plane <= 0.0f ||
       !isfinite(info->vertical_fov) || info->vertical_fov <= 0.0f || info->vertical_fov >= (float)M_PI)
      return 0;
   if (info->flags & KK_TEMPORAL_DEPTH_INFINITE)
      return info->far_plane == 0.0f ? RS_TEMPORAL_CAMERA_VALID | RS_TEMPORAL_DEPTH_INFINITE : 0;
   return isfinite(info->far_plane) && info->far_plane > info->near_plane ? RS_TEMPORAL_CAMERA_VALID : 0;
}

void
kk_temporal_record(struct kk_cmd_buffer *cmd, const VkDependencyInfo *dep)
{
   const struct kk_temporal_frame_info *info = kk_temporal_find(dep);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   if (!info || !kk_temporal_pipeline())
      return;
   if (!cmd->metal.cmd_buf || cmd->interlock_segments || cmd->state.gfx.render_pass_descriptor ||
       cmd->whole_pass.suspended || !info->frame_id || info->feature_type != KK_TEMPORAL_SUPER_RESOLUTION) {
      mesa_logw("Temporal frame %llu of feature %llu not forwarded: %s", (unsigned long long)info->frame_id,
                (unsigned long long)info->feature_id,
                info->feature_type != KK_TEMPORAL_SUPER_RESOLUTION ? "only Super Resolution is forwarded"
                : !info->frame_id                                  ? "no frame identity"
                                                                   : "recorded inside rendering or an interlock segment");
      return;
   }
   struct kk_temporal *t = kk_temporal_device(dev);
   struct kk_temporal_record *r = t ? calloc(1, sizeof(*r)) : NULL;
   if (!r) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   @autoreleasepool {
      for (unsigned i = 0; i < KK_TEMPORAL_IMAGES; i++) {
         if (info->images[i] == VK_NULL_HANDLE)
            continue;
         VK_FROM_HANDLE(kk_image, image, info->images[i]);
         id<MTLTexture> texture = (id<MTLTexture>)image->planes[0].mtl_handle;
         const MTLPixelFormat format = info->formats[i] != VK_FORMAT_UNDEFINED
            ? (MTLPixelFormat)vk_format_to_mtl_pixel_format(info->formats[i]) : texture.pixelFormat;
         /* Integer-typed images are read as the float format of their bits. */
         if (texture && format != texture.pixelFormat)
            texture = (texture.usage & MTLTextureUsagePixelFormatView)
               ? [texture newTextureViewWithPixelFormat:format] : nil;
         else
            texture = [texture retain];
         r->textures[i] = texture;
      }
   }
   id<MTLTexture> color = r->textures[KK_TEMPORAL_COLOR], depth = r->textures[KK_TEMPORAL_DEPTH];
   id<MTLTexture> motion = r->textures[KK_TEMPORAL_MOTION], output = r->textures[KK_TEMPORAL_OUTPUT];
   if (!color || !depth || !motion || !output) {
      mesa_logw("Temporal frame %llu not forwarded: color, depth, motion and output images are required",
                (unsigned long long)info->frame_id);
      kk_temporal_record_free(r);
      return;
   }

   r->feature_type = info->feature_type;
   r->owner = kk_temporal_owner(t, info);
   if (!r->owner) {
      kk_temporal_record_free(r);
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   RSTemporalFrame *f = &r->frame;
   f->size = sizeof(*f);
   f->version = 1;
   f->owner = r->owner;
   f->color = color;
   f->depth = depth;
   f->motion = motion;
   f->exposure = r->textures[KK_TEMPORAL_EXPOSURE];
   f->frameId = info->frame_id;
   f->flags = kk_temporal_flags(info->flags) | kk_temporal_camera_flags(info);
   f->renderWidth = info->render_width ? info->render_width : color.width - info->color_x;
   f->renderHeight = info->render_height ? info->render_height : color.height - info->color_y;
   f->outputWidth = info->output_width ? info->output_width : output.width - info->output_x;
   f->outputHeight = info->output_height ? info->output_height : output.height - info->output_y;
   /* Dynamic resolution stays within the allocated color and depth images. */
   f->maxRenderWidth = MIN2(color.width - info->color_x, depth.width - info->depth_x);
   f->maxRenderHeight = MIN2(color.height - info->color_y, depth.height - info->depth_y);
   f->colorX = info->color_x;
   f->colorY = info->color_y;
   f->depthX = info->depth_x;
   f->depthY = info->depth_y;
   f->motionX = info->motion_x;
   f->motionY = info->motion_y;
   f->outputX = info->output_x;
   f->outputY = info->output_y;
   f->jitterX = info->jitter_offset_x;
   f->jitterY = info->jitter_offset_y;
   f->motionScaleX = info->motion_vector_scale_x;
   f->motionScaleY = info->motion_vector_scale_y;
   /* DLSS's pre-exposure 0 means none. */
   f->preExposure = info->pre_exposure > 0.0f ? info->pre_exposure : 1.0f;
   f->sharpness = info->sharpness;
   f->frameTimeMs = info->frame_time_ms;
   if (f->flags & RS_TEMPORAL_CAMERA_VALID) {
      f->nearPlane = info->near_plane;
      f->farPlane = info->far_plane;
      f->verticalFov = info->vertical_fov;
   }

   /* The frame runs between this part and the next. */
   if (!kk_mesh_split_command_buffer(cmd)) {
      kk_temporal_record_free(r);
      return;
   }
   r->part = util_dynarray_num_elements(&cmd->mesh_command_buffers, mtl_command_buffer *);
   util_dynarray_append_typed(&cmd->temporal_records, struct kk_temporal_record *, r);
}

static double
kk_temporal_now(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return now.tv_sec + now.tv_nsec * 1e-9;
}

/* The feature's tracked image at the frame's output extent and format; *reallocated when it is new. */
static id<MTLTexture>
kk_temporal_owner_output(struct kk_temporal_owner *owner, id<MTLTexture> output, uint32_t width,
                         uint32_t height, bool *reallocated)
{
   *reallocated = false;
   id<MTLTexture> current = owner->output;
   if (current && current.width == width && current.height == height &&
       current.pixelFormat == output.pixelFormat)
      return current;
   MTLTextureDescriptor *descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:output.pixelFormat width:width height:height
                                                     mipmapped:NO];
   descriptor.storageMode = MTLStorageModePrivate;
   descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
   descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
   id<MTLTexture> made = [output.device newTextureWithDescriptor:descriptor];
   if (made) {
      [owner->output release];
      owner->output = made;
      *reallocated = true;
   }
   return made;
}

static void
kk_temporal_encode(const struct kk_temporal_pipeline *pipeline, struct kk_temporal_record *r,
                   id<MTLCommandBuffer> commands)
{
   struct kk_temporal_owner *owner = r->owner;
   id<MTLTexture> output = r->textures[KK_TEMPORAL_OUTPUT];
   RSTemporalFrame f = r->frame;
   bool reallocated;
   id<MTLTexture> tracked = kk_temporal_owner_output(owner, output, f.outputWidth, f.outputHeight, &reallocated);
   if (!tracked)
      return;
   f.commandBuffer = commands;
   f.commandBufferKind = 0;
   f.output = tracked;
   f.outputX = f.outputY = 0;
   const double now = kk_temporal_now();
   if (!(f.frameTimeMs > 0.0f)) {
      const double elapsed = owner->previous_time > 0.0 ? (now - owner->previous_time) * 1000.0 : 1000.0 / 60.0;
      f.frameTimeMs = (float)CLAMP(elapsed, 0.001, 1000.0);
   }
   owner->previous_time = now;

   /* The selected method, or MetalFX, which is what DLSS maps to. */
   const uint32_t mapped = RS_TEMPORAL_METALFX;
   const uint32_t selected = pipeline->method();
   f.method = selected ? selected : mapped;
   RSTemporalResult result = {.size = sizeof(result), .version = 1};
   int status = pipeline->process(&f, &result);
   if ((status != RS_TEMPORAL_OK || !result.encoded) && status != RS_TEMPORAL_BUSY && f.method != mapped) {
      if (!owner->reported)
         mesa_logw("Temporal frames of feature %llu: method %u unavailable (%s); using MetalFX",
                   (unsigned long long)owner->feature_id, f.method, result.reason);
      f.method = mapped;
      result = (RSTemporalResult){.size = sizeof(result), .version = 1};
      status = pipeline->process(&f, &result);
   }
   if (!owner->reported) {
      mesa_logi("Temporal frames of feature %llu: method %u, %ux%u to %ux%u, status %d encoded %u%s%s",
                (unsigned long long)owner->feature_id, f.method, f.renderWidth, f.renderHeight, f.outputWidth,
                f.outputHeight, status, result.encoded, result.reason[0] ? ": " : "", result.reason);
      owner->reported = status == RS_TEMPORAL_OK && result.encoded;
   }

   /* Busy or failed frames keep the feature's previous image; without one, the game's output is left as
    * it is. */
   owner->output_valid = kk_temporal_tracked_valid(owner->output_valid, reallocated,
                                                   status == RS_TEMPORAL_OK && result.encoded);
   if (!owner->output_valid)
      return;
   id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
   [blit copyFromTexture:tracked sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(f.outputWidth, f.outputHeight, 1) toTexture:output destinationSlice:0
        destinationLevel:0 destinationOrigin:MTLOriginMake(r->frame.outputX, r->frame.outputY, 0)];
   [blit endEncoding];
}

uint32_t
kk_temporal_commit_prefix(struct kk_queue *queue, struct kk_cmd_buffer *cmd)
{
   const uint32_t count = util_dynarray_num_elements(&cmd->temporal_records, struct kk_temporal_record *);
   if (!count)
      return 0;
   struct kk_device *dev = kk_queue_device(queue);
   const struct kk_temporal_pipeline *pipeline = kk_temporal_pipeline();
   uint32_t first = 0;
   @autoreleasepool {
      id<MTLDevice> device = (id<MTLDevice>)dev->mtl_handle;
      if (!queue->temporal_queue) {
         id<MTLCommandQueue> made = [device newCommandQueue];
         if (@available(macOS 15.0, *))
            [made addResidencySet:(id<MTLResidencySet>)dev->residency_set.handle];
         queue->temporal_queue = (void *)made;
         queue->temporal_event = (void *)[device newSharedEvent];
      }
      id<MTLCommandQueue> classic = (id<MTLCommandQueue>)queue->temporal_queue;
      id<MTLSharedEvent> event = (id<MTLSharedEvent>)queue->temporal_event;
      if (!classic || !event)
         return 0;
      util_dynarray_foreach(&cmd->temporal_records, struct kk_temporal_record *, record) {
         struct kk_temporal_record *r = *record;
         if (r->part > first) {
            mtl_commit_options *options = mtl_new_commit_options();
            mtl_command_queue_commit(queue->mtl_handle, cmd->native_submit.buffers + first, r->part - first,
                                     options);
            mtl_release(options);
            first = r->part;
         }
         const uint64_t ready = ++queue->temporal_value;
         mtl_signal_event(queue->mtl_handle, (mtl_event *)event, ready);
         id<MTLCommandBuffer> commands = [classic commandBuffer];
         [commands encodeWaitForEvent:event value:ready];
         kk_temporal_encode(pipeline, r, commands);
         const uint64_t done = ++queue->temporal_value;
         [commands encodeSignalEvent:event value:done];
         [commands commit];
         mtl_wait_for_event(queue->mtl_handle, (mtl_event *)event, done);
      }
   }
   return first;
}

void
kk_temporal_queue_finish(struct kk_queue *queue)
{
   [(id)queue->temporal_queue release];
   [(id)queue->temporal_event release];
   queue->temporal_queue = NULL;
   queue->temporal_event = NULL;
}

/* SPDX-License-Identifier: MIT */
#include "kk_scene.h"
#include "kk_scene_lifetime.h"
#include "kk_cmd_buffer.h"
#include "kk_device.h"
#include "kk_queue.h"
#include "kk_image.h"
#include "vk_cmd_queue.h"
#include "kosmickrisp/bridge/mtl_encoder.h"
#include "kosmickrisp/bridge/mtl_residency_set.h"
#include "kosmickrisp/bridge/mtl_sync.h"
#include "kosmickrisp/bridge/mtl_command_queue.h"
#include "scene/image-color.h"
#include "scene/recorded-scene.h"
#include "scene/generation-provider.h"
#include <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>

struct kk_scene_record {
   struct kk_scene_record *next;
   unsigned references;
   struct kk_scene_record *resolved_owner;
   struct kk_scene_record *generation_resources;
   void (*submitted)(void *, uint64_t, const RSTemporalResult *, uint32_t, void *, uint64_t);
   RSVulkanSceneOperationV1 operation;
   struct RSImageColorDescription color;
   RSSceneAovs aovs;
   RSSceneMatrices matrices;
   id<MTLTexture> textures[RS_VULKAN_SCENE_IMAGE_COUNT];
   id heaps[RS_VULKAN_SCENE_IMAGE_COUNT], buffers[RS_VULKAN_SCENE_IMAGE_COUNT];
   id device;
   mtl_residency_set *residency;
};
struct kk_scene_flight {
   struct kk_scene_flight *next, *storage_next;
   struct kk_scene_lifetime life;
   unsigned submitted, storage_closed;
   mtx_t storage_mutex;
   struct kk_scene_record *record;
   RSTemporalResult result;
   id queue, commands, allocator, residency;
   NSMutableArray *prefix_storage;
};
struct kk_scene_submission {
   unsigned references;
   mtx_t mutex;
   cnd_t publication;
   bool needs_event, notifying, notified, gpu_done, delivered, suppress_callback;
   struct mtl_feedback_data feedback;
   char error_message[512];
   id queue, event;
   NSMutableArray *prefix_storage;
   struct kk_scene_flight *flights;
   mtl_feedback_handler_callback callback;
   void *data;
};

static void record_ref(struct kk_scene_record *r)
{ __atomic_add_fetch(&r->references, 1, __ATOMIC_RELAXED); }
static void record_unref(struct kk_scene_record *r)
{
   if (__atomic_sub_fetch(&r->references, 1, __ATOMIC_ACQ_REL)) return;
   if (r->resolved_owner) record_unref(r->resolved_owner);
   else {
      if (r->generation_resources) record_unref(r->generation_resources);
      for (unsigned i = 0; i < RS_VULKAN_SCENE_IMAGE_COUNT; ++i) {
         [r->textures[i] release]; [r->heaps[i] release]; [r->buffers[i] release];
      }
      [r->device release]; [(id)r->residency release];
   }
   if (r->operation.callbacks.release)
      r->operation.callbacks.release(r->operation.callbacks.storage);
   free(r);
}
static void flight_ref(void *data)
{ kk_scene_lifetime_retain(&((struct kk_scene_flight *)data)->life); }
/* Native storage may retire at a legal command reset before delayed CPU
 * completion, while common still retains stable result/output ownership. */
static void flight_close_storage(struct kk_scene_flight *f)
{
   mtx_lock(&f->storage_mutex);
   if (!f->storage_closed) {
      [f->commands release]; f->commands = nil;
      [f->allocator release]; f->allocator = nil;
      [f->prefix_storage release]; f->prefix_storage = nil;
      f->storage_closed = 1;
   }
   mtx_unlock(&f->storage_mutex);
}
static void flight_unref(void *data)
{
   struct kk_scene_flight *f = data;
   if (!kk_scene_lifetime_release(&f->life)) return;
   flight_close_storage(f);
   mtx_destroy(&f->storage_mutex);
   [f->queue release]; [f->residency release]; record_unref(f->record); free(f);
}
static void flight_submitted(struct kk_scene_flight *f, bool accepted, void *event, uint64_t value)
{
   if (!f->record->submitted || __atomic_exchange_n(&f->submitted, 1, __ATOMIC_ACQ_REL)) return;
   @try { f->record->submitted(f->record->operation.callbacks.storage,
      f->record->operation.frame.frameId, &f->result, accepted, event, value); }
   @catch (NSException *exception) { (void)exception; }
}
static void flight_finish(struct kk_scene_flight *f, uint32_t completion)
{
   if (!kk_scene_lifetime_finish(&f->life)) return;
   flight_close_storage(f);
   if (completion != RS_VULKAN_SCENE_GPU_COMPLETED) {
      f->result.status = completion == RS_VULKAN_SCENE_GPU_FAILED ?
         RS_TEMPORAL_FAILED : RS_TEMPORAL_UNAVAILABLE;
      f->result.encoded = 0;
   }
   flight_submitted(f, false, NULL, 0);
   RSVulkanSceneCallbacksV1 *cb = &f->record->operation.callbacks;
   @try { cb->completed(cb->storage, f->record->operation.frame.frameId,
                        completion, &f->result); }
   @catch (NSException *exception) { (void)exception; }
}
static void finish_list(struct kk_scene_flight *f, uint32_t completion)
{
   while (f) { struct kk_scene_flight *next = f->next;
      flight_finish(f, completion); flight_unref(f); f = next; }
}
void kk_scene_reset(struct kk_cmd_buffer *cmd)
{
   struct kk_scene_flight *storage = cmd->scene_storage_flights;
   cmd->scene_storage_flights = NULL;
   while (storage) {
      struct kk_scene_flight *next = storage->storage_next;
      flight_close_storage(storage); flight_unref(storage); storage = next;
   }
   struct kk_scene_flight *flights = cmd->scene_flights;
   kk_scene_submission_release(cmd->scene_reserved_submission);
   cmd->scene_reserved_submission = NULL;
   cmd->scene_flights = NULL; cmd->scene_queue = NULL;
   finish_list(flights, RS_VULKAN_SCENE_ABANDONED);
   struct kk_scene_record *r = cmd->scene_records; cmd->scene_records = NULL;
   while (r) { struct kk_scene_record *next = r->next; record_unref(r); r = next; }
}

static void before(void *scope, void *encoder, uint32_t kind)
{
   (void)scope;
   if (kind == RS_VULKAN_SCENE_COMPUTE_ENCODER)
      mtl_barrier_after_queue_stages(encoder, MTL_STAGE_ALL, MTL_STAGE_ALL);
}
static void after(void *scope, void *encoder, uint32_t kind)
{
   (void)scope;
   if (kind == RS_VULKAN_SCENE_COMPUTE_ENCODER)
      mtl_barrier_after_stages(encoder, MTL_STAGE_ALL, MTL_STAGE_ALL);
}
static void resolved_encoding(struct kk_scene_record *r, RSVulkanSceneEncodingV2 *encoding, RSSceneAovs *aovs)
{
   memset(encoding, 0, sizeof(*encoding));
   encoding->base.size = sizeof(*encoding); encoding->base.version = 2;
   encoding->base.frame = r->operation.frame;
   encoding->base.frame.commandBufferKind = 1;

   void **fields[] = { &encoding->base.frame.color, &encoding->base.frame.depth,
      &encoding->base.frame.motion, &encoding->base.frame.output,
      &encoding->base.frame.exposure, &encoding->base.frame.reactive,
      &encoding->base.frame.transparency };
   for (unsigned i=0; i<RS_VULKAN_SCENE_AOV_FIRST; ++i) *fields[i] = r->textures[i];
   *aovs = r->aovs;
   for (unsigned i=0; i<RS_SCENE_AOV_COUNT; ++i)
      aovs->images[i].texture = r->textures[RS_VULKAN_SCENE_AOV_FIRST+i];
   encoding->base.color = r->operation.color;
   encoding->base.aovs = r->operation.aovs ? aovs : NULL;
   encoding->base.matrices = r->operation.matrices;
   encoding->base.sourceTimeSeconds = r->operation.sourceTimeSeconds;

   encoding->base.images = r->operation.images;
   encoding->base.imageMask = r->operation.imageMask;
}

static void scene_execute(VkCommandBuffer handle, void *data)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, handle);
   struct kk_scene_record *r = data;
   struct kk_scene_flight *f = calloc(1, sizeof(*f));
   if (!f || mtx_init(&f->storage_mutex, mtx_plain) != thrd_success) {
      free(f); vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY); return;
   }
   kk_scene_lifetime_init(&f->life); record_ref(r); f->record = r;
   f->result = (RSTemporalResult){ .size = sizeof(f->result), .version = 1,
      .status = RS_TEMPORAL_UNAVAILABLE, .method = r->operation.frame.method };
   f->next = cmd->scene_flights; cmd->scene_flights = f;
   f->storage_next = cmd->scene_storage_flights;
   cmd->scene_storage_flights = f; flight_ref(f);
   if (!cmd->scene_queue || cmd->state.gfx.render_pass_descriptor || cmd->whole_pass.suspended) {
      strncpy(f->result.reason, "Scene encoding needs a submitting queue outside rendering", sizeof(f->result.reason)-1);
      return;
   }
   cs_end(cmd);
   f->queue = [(id)cmd->scene_queue->mtl_handle retain];
   f->commands = [(id)cmd->metal.cmd_buf retain];
   f->allocator = [(id)cmd->metal.allocator retain];
   f->residency = [(id)r->residency retain];
   [(id<MTL4CommandBuffer>)cmd->metal.cmd_buf useResidencySet:f->residency];
   if (r->generation_resources)
      [(id<MTL4CommandBuffer>)cmd->metal.cmd_buf useResidencySet:(id)r->generation_resources->residency];
   RSVulkanSceneEncodingV2 encoding; RSSceneAovs aovs;
   resolved_encoding(r, &encoding, &aovs);
   encoding.base.frame.commandBuffer = cmd->metal.cmd_buf;
   encoding.base.dependencies = (RSVulkanSceneDependenciesV1){cmd, before, after};
   encoding.commandQueue = cmd->scene_queue->mtl_handle;
   encoding.flightStorage = f; encoding.retainFlight = flight_ref;
   encoding.releaseFlight = flight_unref;
   @try {
      int accepted = r->operation.callbacks.encode(r->operation.callbacks.storage,
                                                   &encoding.base, &f->result);
      if (!accepted || f->result.status != RS_TEMPORAL_OK || !f->result.encoded) {
         f->result.encoded = 0;
         if (f->result.status == RS_TEMPORAL_OK) f->result.status = RS_TEMPORAL_UNAVAILABLE;
      }
   } @catch (NSException *exception) {
      (void)exception; f->result.status = RS_TEMPORAL_FAILED; f->result.encoded = 0;
   }
   mtl_compute_encoder *barrier = cs_get_compute(cmd);
   mtl_barrier_after_queue_stages(barrier, MTL_STAGE_ALL, MTL_STAGE_ALL);
   /* Callback-owned encoders must already be ended. The next ordinary driver
    * encoder waits on all preceding queue stages using its existing path. */
}

void kk_scene_recording_failed(struct kk_cmd_buffer *cmd)
{
   kk_scene_submission_release(cmd->scene_reserved_submission);
   cmd->scene_reserved_submission = NULL;
   struct kk_scene_flight *flights = cmd->scene_flights;
   cmd->scene_flights = NULL;
   finish_list(flights, RS_VULKAN_SCENE_GPU_FAILED);
}

static VkResult resolve(struct kk_device *dev, struct kk_scene_record *r,
                       unsigned slot, const RSVulkanSceneImageV1 *binding)
{
   VK_FROM_HANDLE(kk_image, image, binding->image);
   if (!image || image->vk.base.device != &dev->vk)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (image->vk.image_type != VK_IMAGE_TYPE_2D || image->vk.samples != VK_SAMPLE_COUNT_1_BIT ||
       !binding->aspect || (binding->aspect & (binding->aspect-1)) ||
       (binding->aspect & ~image->vk.aspects) || binding->mipLevel >= image->vk.mip_levels ||
       binding->arrayLayer >= image->vk.array_layers || image->sparse || image->writable_msaa)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   unsigned plane = kk_image_aspects_to_plane(image, binding->aspect);
   if (plane >= image->plane_count || image->planes[plane].memoryless ||
       !image->planes[plane].mem || !image->planes[plane].mem->bo)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   id<MTLTexture> texture = (id)kk_image_plane_render_base(&image->planes[plane]);
   if (!texture || texture.device != (id)dev->mtl_handle) return VK_ERROR_INITIALIZATION_FAILED;
   if (texture.framebufferOnly ||
       (texture.textureType != MTLTextureType2D && texture.textureType != MTLTextureType2DArray))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   MTLPixelFormat format = texture.pixelFormat;
   if (binding->aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
      if (format == MTLPixelFormatDepth32Float_Stencil8) format = MTLPixelFormatX32_Stencil8;
      else return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   r->textures[slot] = [texture newTextureViewWithPixelFormat:format
      textureType:MTLTextureType2D levels:NSMakeRange(binding->mipLevel,1)
      slices:NSMakeRange(binding->arrayLayer,1)];
   if (!r->textures[slot]) return VK_ERROR_FEATURE_NOT_PRESENT;
   r->heaps[slot] = [(id)image->planes[plane].mem->bo->mtl_handle retain];
   r->buffers[slot] = [(id)image->planes[plane].mem->bo->map retain];
   if (r->heaps[slot]) mtl_residency_set_add_allocation(r->residency, (void *)r->heaps[slot]);
   if (r->buffers[slot]) mtl_residency_set_add_allocation(r->residency, (void *)r->buffers[slot]);
   return VK_SUCCESS;
}

static VkResult validate_metadata(const RSVulkanSceneOperationV1 *input, uint32_t *mask)
{
   if (input->frame.size != sizeof(input->frame) || input->frame.version != 1 ||
       !input->frame.owner || !input->frame.frameId || input->frame.commandBuffer ||
       input->frame.color || input->frame.depth || input->frame.motion || input->frame.output ||
       input->frame.exposure || input->frame.reactive || input->frame.transparency)
      return VK_ERROR_INITIALIZATION_FAILED;
   if ((input->color && (input->color->size != sizeof(*input->color) || input->color->version != 1)) ||
       (input->aovs && (input->aovs->size != sizeof(*input->aovs) || input->aovs->version != 1 || input->aovs->frameId != input->frame.frameId)) ||
       (input->matrices && (input->matrices->size != sizeof(*input->matrices) || input->matrices->version != 1 || input->matrices->frameId != input->frame.frameId)))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   *mask = input->imageMask ? input->imageMask :
      (1u << RS_VULKAN_SCENE_COLOR) | (1u << RS_VULKAN_SCENE_OUTPUT);
   if (*mask >> RS_VULKAN_SCENE_IMAGE_COUNT ||
       !(*mask & (1u << RS_VULKAN_SCENE_COLOR)) || !(*mask & (1u << RS_VULKAN_SCENE_OUTPUT)) ||
       ((*mask >> RS_VULKAN_SCENE_AOV_FIRST) && !input->aovs)) return VK_ERROR_FEATURE_NOT_PRESENT;
   if (input->aovs) for (unsigned i=0; i<RS_SCENE_AOV_COUNT; ++i)
      if (input->aovs->images[i].texture) return VK_ERROR_INITIALIZATION_FAILED;
   return VK_SUCCESS;
}

static VkResult resolve_record(struct kk_device *dev, const RSVulkanSceneOperationV1 *input,
                               uint32_t mask, bool callbacks, struct kk_scene_record **output)
{
   struct kk_scene_record *r = calloc(1, sizeof(*r));
   if (!r) return VK_ERROR_OUT_OF_HOST_MEMORY;
   r->references = 1; r->operation = *input; r->operation.imageMask = mask;
   if (callbacks && input->version == 3) r->submitted = ((const RSVulkanSceneOperationV3 *)input)->submitted;
   if (callbacks) input->callbacks.retain(input->callbacks.storage);
   else memset(&r->operation.callbacks, 0, sizeof(r->operation.callbacks));
   r->device = [(id)dev->mtl_handle retain];
   if (input->color) { r->color = *input->color; r->operation.color = &r->color; }
   if (input->aovs) { r->aovs = *input->aovs; r->operation.aovs = &r->aovs; }
   if (input->matrices) { r->matrices = *input->matrices; r->operation.matrices = &r->matrices; }
   @try {
      r->residency = mtl_new_residency_set((void *)r->device);
      if (!r->residency) { record_unref(r); return VK_ERROR_OUT_OF_HOST_MEMORY; }
      for (unsigned i=0; i<RS_VULKAN_SCENE_IMAGE_COUNT; ++i) if (mask & (1u<<i)) {
         VkResult result = resolve(dev, r, i, &input->images[i]);
         if (result != VK_SUCCESS) { record_unref(r); return result; }
      }
      mtl_residency_set_commit(r->residency);
   } @catch (NSException *exception) { (void)exception; record_unref(r); return VK_ERROR_UNKNOWN; }
   *output = r; return VK_SUCCESS;
}


struct kk_scene_prepared {
   unsigned references;
   struct kk_scene_record *resolved;
   RSRecordedSceneConsumerV3 consumer;
};
static void prepared_retain(void *data)
{ __atomic_add_fetch(&((struct kk_scene_prepared *)data)->references, 1, __ATOMIC_RELAXED); }
static void prepared_release(void *data)
{
   struct kk_scene_prepared *p = data;
   if (__atomic_sub_fetch(&p->references, 1, __ATOMIC_ACQ_REL)) return;
   p->consumer.callbacks.base.release(p->consumer.callbacks.base.storage);
   record_unref(p->resolved); free(p);
}
static int prepared_encode(void *data, const RSVulkanSceneEncodingV1 *encoding, RSTemporalResult *result)
{ struct kk_scene_prepared *p = data; return p->consumer.callbacks.base.encode(p->consumer.callbacks.base.storage, encoding, result); }
static void prepared_completed(void *data, uint64_t frame, uint32_t completion, const RSTemporalResult *result)
{ struct kk_scene_prepared *p = data; p->consumer.callbacks.base.completed(p->consumer.callbacks.base.storage, frame, completion, result); }
static void prepared_submitted(void *data, uint64_t frame, const RSTemporalResult *result, uint32_t accepted, void *event, uint64_t value)
{ struct kk_scene_prepared *p = data; p->consumer.callbacks.submitted(p->consumer.callbacks.base.storage, frame, result, accepted, event, value); }
static bool is_prepared(const RSVulkanSceneOperationV1 *input)
{
   return input->version == 3 && input->size == sizeof(RSVulkanSceneOperationV3) &&
      input->callbacks.storage && input->callbacks.retain == prepared_retain &&
      input->callbacks.release == prepared_release && input->callbacks.encode == prepared_encode &&
      input->callbacks.completed == prepared_completed &&
      ((const RSVulkanSceneOperationV3 *)input)->submitted == prepared_submitted;
}
static bool prepared_matches(struct kk_scene_prepared *p, struct kk_device *dev,
                             const RSVulkanSceneOperationV1 *input, uint32_t mask)
{
   const RSVulkanSceneOperationV1 *saved = &p->resolved->operation;
   if (p->resolved->device != (id)dev->mtl_handle || mask != saved->imageMask) return false;
   RSTemporalFrame frame = input->frame;
   if (frame.owner != saved->frame.owner && frame.owner != p->consumer.owner) return false;
   frame.owner = saved->frame.owner;
   if (memcmp(&frame, &saved->frame, sizeof(frame))) return false;
   for (unsigned i=0;i<RS_VULKAN_SCENE_IMAGE_COUNT;i++) if (mask & (1u<<i)) {
      const RSVulkanSceneImageV1 *a=&input->images[i], *b=&saved->images[i];
      if (a->image != b->image || a->aspect != b->aspect || a->mipLevel != b->mipLevel || a->arrayLayer != b->arrayLayer) return false;
   }
   if (!!input->color != !!saved->color || !!input->aovs != !!saved->aovs || !!input->matrices != !!saved->matrices) return false;
   return (!input->color || !memcmp(input->color,saved->color,sizeof(*input->color))) &&
      (!input->aovs || !memcmp(input->aovs,saved->aovs,sizeof(*input->aovs))) &&
      (!input->matrices || !memcmp(input->matrices,saved->matrices,sizeof(*input->matrices))) &&
      input->sourceTimeSeconds == saved->sourceTimeSeconds;
}
static VkResult resolved_record_for_callbacks(struct kk_device *dev,
   const RSVulkanSceneOperationV1 *input, uint32_t mask, struct kk_scene_record **output)
{
   if (!is_prepared(input)) return resolve_record(dev,input,mask,true,output);
   struct kk_scene_prepared *p=input->callbacks.storage;
   if (!prepared_matches(p,dev,input,mask)) return VK_ERROR_INITIALIZATION_FAILED;
   struct kk_scene_record *r=calloc(1,sizeof(*r));
   if (!r) return VK_ERROR_OUT_OF_HOST_MEMORY;
   *r=*p->resolved; r->next=NULL; r->references=1; r->resolved_owner=p->resolved;
   record_ref(r->resolved_owner); r->operation.frame=input->frame;
   r->operation.frame.owner=p->consumer.owner;
   r->operation.callbacks=input->callbacks; r->submitted=prepared_submitted;
   r->operation.callbacks.retain(r->operation.callbacks.storage);
   *output=r; return VK_SUCCESS;
}
static int prepare_reject(RSTemporalResult *result, uint32_t status, const char *reason)
{
   if (result && result->size==sizeof(*result) && result->version==1) {
      result->status=status; result->encoded=0;
      snprintf(result->reason,sizeof(result->reason),"%s",reason);
   }
   return 0;
}
static bool same_binding(const RSVulkanSceneImageV1 *a, const RSVulkanSceneImageV1 *b)
{ return a->image==b->image && a->aspect==b->aspect && a->mipLevel==b->mipLevel && a->arrayLayer==b->arrayLayer; }
static VkResult resolve_generation(struct kk_device *dev, struct kk_scene_record *resolved,
   const RSVulkanSceneOperationV1 *input, const struct RSGenerationSubmission *metadata,
   const RSVulkanSceneImageV1 *targets, const RSVulkanSceneEncodingV2 *encoding,
   struct RSGenerationSubmission *output)
{
   if (!metadata || metadata->size!=sizeof(*metadata) || metadata->version!=1 ||
       metadata->commandBuffer || metadata->color || metadata->scene || !targets ||
       !metadata->count || metadata->count>4)
      return VK_ERROR_INITIALIZATION_FAILED;
   for(unsigned i=0;i<4;i++) if(metadata->outputs[i]) return VK_ERROR_INITIALIZATION_FAILED;
   if(!same_binding(&targets[0],&input->images[RS_VULKAN_SCENE_OUTPUT])) return VK_ERROR_INITIALIZATION_FAILED;
   for(unsigned i=0;i<metadata->count;i++) {
      if(same_binding(&targets[i],&input->images[RS_VULKAN_SCENE_COLOR])) return VK_ERROR_INITIALIZATION_FAILED;
      for(unsigned j=0;j<i;j++) if(same_binding(&targets[i],&targets[j])) return VK_ERROR_INITIALIZATION_FAILED;
   }
   if(metadata->count>1) {
      RSVulkanSceneOperationV1 extra=*input;
      for(unsigned i=1;i<metadata->count;i++) extra.images[i]=targets[i];
      VkResult result=resolve_record(dev,&extra,((1u<<metadata->count)-1u)&~1u,false,&resolved->generation_resources);
      if(result!=VK_SUCCESS) return result;
   }
   *output=*metadata; output->color=encoding->base.frame.color; output->scene=&encoding->base.frame;
   output->outputs[0]=encoding->base.frame.output;
   for(unsigned i=1;i<metadata->count;i++) output->outputs[i]=resolved->generation_resources->textures[i];
   return VK_SUCCESS;
}

/* Shape-only Create query: no image resolution, commands, or admission lease. */
PUBLIC int rsVulkanQuerySceneFeature3(VkDevice device, void *feature_owner,
   uint32_t feature, const RSTemporalFrame *shape,
   RSQueryRecordedSceneFeature3 query, RSTemporalResult *result)
{
   if (!result || result->size!=sizeof(*result) || result->version!=1)
      return 0;
   *result=(RSTemporalResult){.size=sizeof(*result),.version=1,.status=RS_TEMPORAL_UNAVAILABLE};
   if (!device || !feature_owner || !query || !shape ||
       shape->size!=sizeof(*shape) || shape->version!=1 ||
       shape->owner!=feature_owner || shape->commandBufferKind!=1 ||
       !shape->renderWidth || !shape->renderHeight || !shape->outputWidth || !shape->outputHeight ||
       shape->commandBuffer || shape->color || shape->depth || shape->motion || shape->output ||
       shape->exposure || shape->reactive || shape->transparency ||
       !rsTemporalFrameOptionsValid(shape) || feature<1 || feature>3)
      return prepare_reject(result,RS_TEMPORAL_INVALID,"Invalid command-free native feature shape query");
   result->method=shape->method;
   VK_FROM_HANDLE(kk_device,dev,device);
   id actual_device=[(id)dev->mtl_handle retain];
   if (!actual_device)
      return prepare_reject(result,RS_TEMPORAL_FAILED,"The actual native device is unavailable");
   int supported=0;
   @try { supported=query(feature_owner,feature,actual_device,shape,result); }
   @catch(NSException *exception) {
      (void)exception;
      prepare_reject(result,RS_TEMPORAL_FAILED,"Native feature shape query failed");
   }
   [actual_device release];
   if (result->encoded)
      return prepare_reject(result,RS_TEMPORAL_FAILED,"A feature shape query cannot encode commands");
   return supported && result->status==RS_TEMPORAL_OK;
}

PUBLIC int rsVulkanPrepareSceneOperation3(VkDevice device, const RSVulkanSceneOperationV1 *input,
   void *feature_owner, uint32_t feature,
   const struct RSGenerationSubmission *generation, const RSVulkanSceneImageV1 *generation_outputs,
   RSPrepareRecordedSceneOperation3 prepare,
   RSRecordedSceneConsumerV3 *output, RSTemporalResult *result)
{
   if (!output || output->size!=sizeof(*output) || output->version!=3 ||
       !result || result->size!=sizeof(*result) || result->version!=1)
      return prepare_reject(result,RS_TEMPORAL_INVALID,"Invalid native admission output layout");
   *output=(RSRecordedSceneConsumerV3){.size=sizeof(*output),.version=3};
   *result=(RSTemporalResult){.size=sizeof(*result),.version=1,.status=RS_TEMPORAL_UNAVAILABLE};
   if (!device || !input || input->version!=3 || input->size!=sizeof(RSVulkanSceneOperationV3) ||
       input->flags & ~RS_VULKAN_SCENE_ORIGINAL_OUTPUT_RETAINED || !feature_owner || !prepare || feature<1 || feature>3)
      return prepare_reject(result,RS_TEMPORAL_INVALID,"Invalid native preparation request");
   result->method=input->frame.method;
   VK_FROM_HANDLE(kk_device,dev,device);
   uint32_t mask; VkResult status=validate_metadata(input,&mask);
   if (status!=VK_SUCCESS) return prepare_reject(result,RS_TEMPORAL_INVALID,"Invalid scene metadata or native image bindings");
   struct kk_scene_record *resolved=NULL;
   status=resolve_record(dev,input,mask,false,&resolved);
   if (status!=VK_SUCCESS) return prepare_reject(result,status==VK_ERROR_FEATURE_NOT_PRESENT?RS_TEMPORAL_UNAVAILABLE:RS_TEMPORAL_FAILED,"The actual native image/subresource could not be prepared");
   RSVulkanSceneEncodingV2 encoding; RSSceneAovs aovs;
   resolved_encoding(resolved,&encoding,&aovs); /* actual views, no commands/queue/flight */
   struct RSGenerationSubmission native_generation;
   const struct RSGenerationSubmission *actual_generation=NULL;
   if(feature==RS_RECORDED_GENERATION) {
      status=resolve_generation(dev,resolved,input,generation,generation_outputs,&encoding,&native_generation);
      if(status!=VK_SUCCESS) { record_unref(resolved); return prepare_reject(result,status==VK_ERROR_FEATURE_NOT_PRESENT?RS_TEMPORAL_UNAVAILABLE:status==VK_ERROR_INITIALIZATION_FAILED?RS_TEMPORAL_INVALID:RS_TEMPORAL_FAILED,"Invalid actual generation output bindings/submission"); }
      actual_generation=&native_generation;
   } else if(generation || generation_outputs) {
      record_unref(resolved); return prepare_reject(result,RS_TEMPORAL_INVALID,"Generation metadata belongs only to generation preparation");
   }
   RSRecordedSceneConsumerV3 admitted={.size=sizeof(admitted),.version=3};
   int ready=0;
   @try { ready=prepare(feature_owner,feature,resolved->device,&encoding.base,actual_generation,&admitted,result); }
   @catch(NSException *exception) { (void)exception; prepare_reject(result,RS_TEMPORAL_FAILED,"Native preparation failed"); }
   RSVulkanSceneCallbacksV1 *cb=&admitted.callbacks.base;
   bool valid=admitted.size==sizeof(admitted) && admitted.version==3 && admitted.owner &&
      cb->size==sizeof(admitted.callbacks) && cb->version==3 && cb->storage && cb->retain && cb->release &&
      cb->encode && cb->completed && admitted.callbacks.submitted;
   if (!ready || result->status!=RS_TEMPORAL_OK || result->encoded || !valid) {
      if (ready && cb->release && cb->storage) cb->release(cb->storage);
      record_unref(resolved);
      if (ready && (result->encoded || !valid)) return prepare_reject(result,RS_TEMPORAL_FAILED,"Preparation did not return a valid command-free owned admission");
      result->encoded=0;
      if (result->status==RS_TEMPORAL_OK) return prepare_reject(result,RS_TEMPORAL_UNAVAILABLE,"Native preparation did not admit an operation");
      return 0;
   }
   struct kk_scene_prepared *p=calloc(1,sizeof(*p));
   if (!p) { cb->release(cb->storage); record_unref(resolved); return prepare_reject(result,RS_TEMPORAL_FAILED,"Native admission allocation failed"); }
   p->references=1;p->resolved=resolved;p->consumer=admitted;
   *output=admitted;output->callbacks.base.storage=p;
   output->callbacks.base.retain=prepared_retain;output->callbacks.base.release=prepared_release;
   output->callbacks.base.encode=prepared_encode;output->callbacks.base.completed=prepared_completed;
   output->callbacks.submitted=prepared_submitted;
   return 1;
}

PUBLIC uint32_t rsVulkanSceneOperationAPI(void) { return 3; }
PUBLIC uint32_t rsVulkanSceneStagingAPI(void) { return 0; }
PUBLIC VkResult rsVulkanRecordSceneOperation(VkCommandBuffer handle,
                                             const RSVulkanSceneOperationV1 *input)
{
   if (!handle || !input) return VK_ERROR_INITIALIZATION_FAILED;
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, handle);
   bool api3 = input->version == 3 && input->size == sizeof(RSVulkanSceneOperationV3);
   bool api2 = input->version == 1 && input->size == sizeof(*input);
   if (cmd->vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING || (!api2 && !api3) ||
       (input->flags != RS_VULKAN_SCENE_ORIGINAL_OUTPUT_RETAINED &&
        !(api3 && input->flags==0 && is_prepared(input))) ||
       input->frame.size != sizeof(input->frame) || input->frame.version != 1 ||
       !input->frame.owner || !input->frame.frameId || input->frame.commandBuffer ||
       input->frame.color || input->frame.depth || input->frame.motion || input->frame.output ||
       input->frame.exposure || input->frame.reactive || input->frame.transparency)
      return VK_ERROR_INITIALIZATION_FAILED;
   const RSVulkanSceneCallbacksV1 *cb = &input->callbacks;
   if ((api3 ? (cb->size != sizeof(RSVulkanSceneCallbacksV3) || cb->version != 3 ||
                !((const RSVulkanSceneOperationV3 *)input)->submitted) :
               (cb->size != sizeof(*cb) || cb->version != 2)) || !cb->retain || !cb->release ||
       !cb->encode || !cb->completed) return VK_ERROR_FEATURE_NOT_PRESENT;
   uint32_t mask;
   VkResult metadata = validate_metadata(input, &mask);
   if (metadata != VK_SUCCESS) return metadata;
   struct kk_scene_record *r;
   VkResult result = resolved_record_for_callbacks(kk_cmd_buffer_device(cmd), input, mask, &r);
   if (result != VK_SUCCESS) return result;
   struct vk_cmd_queue_entry *entry = linear_zalloc_child(cmd->vk.cmd_queue.ctx, sizeof(*entry));
   if (!entry) { record_unref(r); return VK_ERROR_OUT_OF_HOST_MEMORY; }
   result = kk_cmd_buffer_defer_native_recording(cmd);
   if (result != VK_SUCCESS) { record_unref(r); return result; }
   entry->type = VK_CMD_DRIVER; entry->u.driver.data = r; entry->u.driver.execute = scene_execute;
   r->next = cmd->scene_records; cmd->scene_records = r;
   list_addtail(&entry->cmd_link, &cmd->vk.cmd_queue.cmds);
   return VK_SUCCESS; /* Record success alone is never output admission. */
}

static void submission_ref(struct kk_scene_submission *s)
{ if (s) __atomic_add_fetch(&s->references, 1, __ATOMIC_RELAXED); }
void kk_scene_submission_release(struct kk_scene_submission *s)
{
   if (!s || __atomic_sub_fetch(&s->references, 1, __ATOMIC_ACQ_REL)) return;
   [s->prefix_storage release]; [(id)s->event release]; [(id)s->queue release]; cnd_destroy(&s->publication); mtx_destroy(&s->mutex); free(s);
}
struct kk_scene_submission *kk_scene_submission_acquire(mtl_feedback_handler_callback callback, void *data)
{
   if (callback != kk_scene_submission_feedback) return NULL;
   submission_ref(data); return data;
}
static void submission_deliver(struct kk_scene_submission *s)
{
   mtx_lock(&s->mutex);
   if (!s->notified || !s->gpu_done || s->delivered) { mtx_unlock(&s->mutex); return; }
   s->delivered = true;
   struct kk_scene_flight *flights = s->flights; s->flights = NULL;
   struct mtl_feedback_data feedback = s->feedback; feedback.user_data = s->data;
   bool suppress = s->suppress_callback;
   mtx_unlock(&s->mutex);
   finish_list(flights, feedback.error == MTL_COMMAND_QUEUE_ERROR_NONE ?
      RS_VULKAN_SCENE_GPU_COMPLETED : RS_VULKAN_SCENE_GPU_FAILED);
   [s->prefix_storage release]; s->prefix_storage = nil;
   if (!suppress) s->callback(&feedback);
   kk_scene_submission_release(s); /* native-feedback reservation */
}
static void submission_notify(struct kk_scene_submission *s, struct kk_queue *queue, bool accepted)
{
   submission_ref(s); /* event and payload survive fast concurrent feedback */
   mtx_lock(&s->mutex);
   if (s->notified || s->notifying) { mtx_unlock(&s->mutex); kk_scene_submission_release(s); return; }
   s->notifying = true;
   mtx_unlock(&s->mutex);
   if (accepted && s->needs_event) {
      @try { mtl_signal_event((void *)s->queue, (void *)s->event, 1); }
      @catch (NSException *exception) {
         (void)exception; accepted = false;
         if (queue) vk_device_set_lost(&kk_queue_device(queue)->vk, "Scene event publication failed");
      }
   }
   for (struct kk_scene_flight *f=s->flights; f; f=f->next)
      flight_submitted(f, accepted, accepted ? s->event : NULL, accepted ? 1 : 0);
   mtx_lock(&s->mutex); s->notified = true; s->notifying = false;
   cnd_broadcast(&s->publication); mtx_unlock(&s->mutex);
   kk_scene_submission_release(s);
}
void kk_scene_submission_committed(struct kk_scene_submission *s, struct kk_queue *queue)
{ if (s) submission_notify(s, queue, true); }
static VkResult submission_create(struct kk_queue *queue, bool event,
   struct kk_scene_submission **output)
{
   *output = NULL;
   struct kk_scene_submission *s = calloc(1, sizeof(*s));
   if (!s || mtx_init(&s->mutex, mtx_plain) != thrd_success) {
      free(s); return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   if (cnd_init(&s->publication) != thrd_success) {
      mtx_destroy(&s->mutex); free(s); return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   s->references = 1;
   s->queue = [(id)queue->mtl_handle retain];
   s->needs_event = event;
   if (event) {
      @try { s->event = (id)mtl_new_shared_event(kk_queue_device(queue)->mtl_handle); }
      @catch (NSException *exception) { (void)exception; }
      if (!s->event) { kk_scene_submission_release(s); return VK_ERROR_OUT_OF_HOST_MEMORY; }
   } else s->notified = true;
   *output = s;
   return VK_SUCCESS;
}
VkResult kk_scene_submission_reserve(struct kk_queue *queue, struct kk_cmd_buffer *cmd)
{
   assert(!cmd->scene_reserved_submission);
   /* Reserve the existing flight wrapper/event before appending any suffix
    * commands. A retry after allocation failure still owns its intact prefix. */
   VkResult result = submission_create(queue, true, &cmd->scene_reserved_submission);
   if (result != VK_SUCCESS) return result;
   struct kk_scene_submission *s = cmd->scene_reserved_submission;
   @try {
      s->prefix_storage = [[NSMutableArray alloc] init];
      if (!s->prefix_storage) @throw [NSException exceptionWithName:@"SceneStorage" reason:nil userInfo:nil];
      util_dynarray_foreach(&cmd->mesh_command_buffers, mtl_command_buffer *, part)
         [s->prefix_storage addObject:(id)*part];
      util_dynarray_foreach(&cmd->mesh_allocators, mtl_command_allocator *, allocator)
         [s->prefix_storage addObject:(id)*allocator];
   } @catch (NSException *exception) {
      (void)exception; kk_scene_submission_release(s);
      cmd->scene_reserved_submission = NULL;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   return VK_SUCCESS;
}
VkResult kk_scene_submission_begin(struct kk_queue *queue, struct kk_cmd_buffer *cmd,
   mtl_feedback_handler_callback callback, void *data,
   struct kk_scene_submission **submission)
{
   *submission = NULL;
   struct kk_scene_submission *s = cmd->scene_reserved_submission;
   cmd->scene_reserved_submission = NULL;
   if (!cmd->scene_flights) { kk_scene_submission_release(s); return VK_SUCCESS; }
   if (!s) {
      bool event = false;
      for (struct kk_scene_flight *f=cmd->scene_flights; f; f=f->next)
         event |= f->record->submitted != NULL;
      VkResult result = submission_create(queue, event, &s);
      if (result != VK_SUCCESS) { kk_scene_recording_failed(cmd); return result; }
   }
   assert(s->queue == (id)queue->mtl_handle);
   submission_ref(s); /* eventual native feedback, besides caller handoff */
   s->callback = callback; s->data = data; s->flights = cmd->scene_flights;
   if (s->prefix_storage) {
      s->flights->prefix_storage = s->prefix_storage;
      s->prefix_storage = nil;
   }
   cmd->scene_flights = NULL; *submission = s; return VK_SUCCESS;
}
void kk_scene_submission_feedback(struct mtl_feedback_data *data)
{
   struct kk_scene_submission *s = data->user_data;
   submission_ref(s);
   mtx_lock(&s->mutex);
   s->feedback = *data;
   strncpy(s->error_message, data->error_message ? data->error_message : "", sizeof(s->error_message)-1);
   s->feedback.error_message = s->error_message; s->gpu_done = true;
   mtx_unlock(&s->mutex);
   if (data->error != MTL_COMMAND_QUEUE_ERROR_NONE) submission_notify(s, NULL, false);
   /* Only native feedback delivers completion. Fast feedback may wait for the
    * CPU's post-commit event enqueue; vkQueueSubmit never waits here, and its
    * re-record mutex can never be re-entered by an inline completion. */
   mtx_lock(&s->mutex);
   while (!s->notified) cnd_wait(&s->publication, &s->mutex);
   mtx_unlock(&s->mutex);
   submission_deliver(s);
   kk_scene_submission_release(s);
}
void kk_scene_submission_failed(struct kk_scene_submission *s)
{
   if (!s) return;
   mtx_lock(&s->mutex); s->suppress_callback = true; mtx_unlock(&s->mutex);
   struct mtl_feedback_data failed = { .user_data = s, .error = MTL_COMMAND_QUEUE_ERROR_INTERNAL };
   kk_scene_submission_feedback(&failed);
}

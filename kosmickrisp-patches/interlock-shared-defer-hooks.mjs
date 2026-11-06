// SPDX-License-Identifier: MIT
// Consumer-owned hooks. host-deferred-submit must have applied first.
const queueHeader='src/kosmickrisp/vulkan/kk_queue.h';
const implementation='src/kosmickrisp/vulkan/kk_queue_deferred.h';
const prototype=`VkResult kk_queue_defer(struct kk_queue *queue, struct vk_sync *ready,
   VkResult (*resume)(struct kk_queue *, void *, bool), void *data);`;
export const sharedQueueFinish=`   vk_queue_finish(&queue->vk);
   if (queue->deferred.resume)
      (void)kk_queue_resume_deferred(queue, true);`;
export const waitComment='   /* There may be in-flight command buffers, wait for them to finish. */';
export const earlyQueues=`   /* Host continuations may still encode using shared meta resources.
    * Join/cancel submit workers and drain their native callbacks first. */
   while (dev->queue_count > 0)
      kk_queue_finish(dev, &dev->queues[--dev->queue_count]);`;
export const metaComment='   /* Meta first since it may destroy Vulkan objects */';
function replaceOnce(source,from,to){
 const text=String(source),at=text.indexOf(from);
 if(at<0||text.indexOf(from,at+from.length)>=0)throw Error('fragment-interlock-envelope: missing or ambiguous host-deferred-submit teardown seam');
 return text.slice(0,at)+to+text.slice(at+from.length);
}
export function requireHostDeferred({edits}){
 // has(), rather than get(), proves this module was applied earlier in the
 // same ordered adaptation. A leftover source/build header is no substitute.
 if(!edits.has(implementation)||!String(edits.get(queueHeader)??'').includes(prototype))throw Error('fragment-interlock-envelope requires host-deferred-submit to apply first');
}
export function placeInterlockDeviceDrain(source){
 const from=earlyQueues+'\n\n'+metaComment;
 return replaceOnce(source,from,earlyQueues+`\n\n   /* Common workers and per-queue providers are finished; close the retained
    * interlock registry before destroying shared meta/query/heap state. */
   kk_interlock_submission_drain(dev);
`+metaComment);
}
export function placeInterlockQueueDrain(source){
 const from=sharedQueueFinish+'\n\n'+waitComment;
 return replaceOnce(source,from,sharedQueueFinish+`\n\n   /* The common worker and its one cancel callback are finished. Drain all
    * interlock C packets/native callbacks while pools and mutex remain live. */
   kk_interlock_queue_finish(queue);
`+waitComment);
}

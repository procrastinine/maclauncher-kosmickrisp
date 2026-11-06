// SPDX-License-Identifier: MIT
export const sourceFiles = ['kosmickrisp-patches/interlock-sparse-submit.mjs'];

function replace(source, before, after) {
  if (!source.includes(before))
    throw Error('Missing Sparse commit-result anchor: ' + before.split('\n')[0]);
  return source.replace(before, after);
}

/* The common commit helper returns VkResult. Callback ownership transfers only
 * on success; synchronous failure unwinds the accounting retained for feedback. */
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_sparse_queue.h', s => {
    s = replace(s, 'static void\nkk_sparse_commit_first(',
      'static VkResult\nkk_sparse_commit_first(');
    s = replace(s, `   if (!kk_sparse_residency_used(dev)) {
      kk_queue_commit(queue, cmd, commit_callback, dev);
      return;
   }`, `   if (!kk_sparse_residency_used(dev))
      return kk_queue_commit(queue, cmd, commit_callback, dev);`);
    return replace(s, `   kk_queue_commit(queue, cmd, kk_sparse_first_commit_callback, queue);
   mtx_unlock(&queue->mutex);
}`, `   VkResult result = kk_queue_commit(queue, cmd, kk_sparse_first_commit_callback, queue);
   if (result != VK_SUCCESS) {
      kk_sparse_submission_end(dev);
      assert(queue->commits_in_flight > 0u);
      if (--queue->commits_in_flight == 0u)
         cnd_broadcast(&queue->cond);
   }
   mtx_unlock(&queue->mutex);
   return result;
}`);
  });

  edit('src/kosmickrisp/vulkan/kk_queue.c', s => {
    if (!s.includes('static VkResult\nkk_queue_commit('))
      throw Error('Sparse commit adapter requires the common commit-result hook');
    s = replace(s, `   queue->commits_in_flight++;
   mtl_feedback_handler_callback callback = kk_sparse_submission_begin(dev) ?
      kk_sparse_rerecord_commit_callback : rerecord_commit_callback;
   kk_queue_commit(queue, rerecord, callback, commit);
   *committed = true;`, `   queue->commits_in_flight++;
   const bool sparse_tracked = kk_sparse_submission_begin(dev);
   mtl_feedback_handler_callback callback = sparse_tracked ?
      kk_sparse_rerecord_commit_callback : rerecord_commit_callback;
   result = kk_queue_commit(queue, rerecord, callback, commit);
   if (result != VK_SUCCESS) {
      if (sparse_tracked)
         kk_sparse_submission_end(dev);
      assert(queue->commits_in_flight > 0u);
      if (--queue->commits_in_flight == 0u)
         cnd_broadcast(&queue->cond);
      vk_free(&dev->vk.alloc, commit);
      goto release;
   }
   *committed = true;`);
    return replace(s, '         kk_sparse_commit_first(queue, cmd_buffer);',
      `         VkResult result = kk_sparse_commit_first(queue, cmd_buffer);
         if (result != VK_SUCCESS)
            return result == VK_ERROR_DEVICE_LOST ?
               kk_interlock_submission_lost(dev) : result;`);
  });
}

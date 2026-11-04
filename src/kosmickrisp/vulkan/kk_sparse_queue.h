/* SPDX-License-Identifier: MIT
 * Included after the common queue commit helper. */
static void
kk_sparse_rerecord_commit_callback(struct mtl_feedback_data *data)
{
   struct kk_commit_data *commit = data->user_data;
   kk_sparse_submission_end(kk_queue_device(commit->queue));
   rerecord_commit_callback(data);
}

static void
kk_sparse_first_commit_callback(struct mtl_feedback_data *data)
{
   struct kk_queue *queue = data->user_data;
   struct kk_device *dev = kk_queue_device(queue);
   check_device_lost(dev, data);
   kk_sparse_submission_end(dev);
   /* First-use command buffers also hold sparse range versions. Include
    * their feedback in the queue's existing teardown completion accounting. */
   mtx_lock(&queue->mutex);
   assert(queue->commits_in_flight);
   if (--queue->commits_in_flight == 0) {
      kk_ResetCommandPool(kk_device_to_handle(dev),
         kk_cmd_pool_to_handle(queue->cmd_pool), 0);
      cnd_broadcast(&queue->cond);
   }
   mtx_unlock(&queue->mutex);
}

static VkResult
kk_sparse_commit_first(struct kk_queue *queue, struct kk_cmd_buffer *cmd)
{
   struct kk_device *dev = kk_queue_device(queue);
   if (!cmd->uses_sparse_buffers)
      return kk_queue_commit(queue, cmd, commit_callback, dev);
   mtx_lock(&queue->mutex);
   queue->commits_in_flight++;
   bool tracked = kk_sparse_submission_begin(dev);
   assert(tracked); (void)tracked;
   VkResult result = kk_queue_commit(queue, cmd, kk_sparse_first_commit_callback, queue);
   if (result != VK_SUCCESS) {
      kk_sparse_submission_end(dev);
      assert(queue->commits_in_flight > 0u);
      if (--queue->commits_in_flight == 0u)
         cnd_broadcast(&queue->cond);
   }
   mtx_unlock(&queue->mutex);
   return result;
}

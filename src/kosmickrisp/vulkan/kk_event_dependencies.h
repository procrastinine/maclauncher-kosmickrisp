/*
 * Adaptation to Mesa KosmicKrisp's event dependency handling.
 * Original source copyright notices remain in kk_event.c.
 * SPDX-License-Identifier: MIT
 */
#ifndef KK_EVENT_DEPENDENCIES_H
#define KK_EVENT_DEPENDENCIES_H

static inline void
kk_event_wait_dependencies(VkCommandBuffer command_buffer, uint32_t event_count,
                           const VkDependencyInfo *dependencies)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, command_buffer);
   if (cmd->metal.compute) {
      /* Every compute dependency uses the same full dispatch/blit barrier.
       * All events are waited at this point, so one such barrier covers
       * their combined scopes. Do not turn an empty wait into a barrier. */
      for (uint32_t i = 0; i < event_count; ++i) {
         if (!kk_dependency_info_is_empty(&dependencies[i])) {
            kk_CmdPipelineBarrier2(command_buffer, &dependencies[i]);
            break;
         }
      }
      return;
   }

   /* Encoder ordering covers the device signal; memory dependencies still
    * need the full barrier path, including attachment stores and render-pass
    * splits. Waiting leaves the event's set/reset status unchanged. */
   for (uint32_t i = 0; i < event_count; ++i)
      kk_CmdPipelineBarrier2(command_buffer, &dependencies[i]);
}

#endif

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
   /* Encoder ordering covers the device signal; memory dependencies still
    * need the full barrier path, including attachment stores and render-pass
    * splits. Waiting leaves the event's set/reset status unchanged. */
   for (uint32_t i = 0; i < event_count; ++i)
      kk_CmdPipelineBarrier2(command_buffer, &dependencies[i]);
}

#endif

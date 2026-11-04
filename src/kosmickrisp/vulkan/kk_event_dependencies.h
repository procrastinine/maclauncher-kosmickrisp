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
   /* The shared path batches compute scopes and retains each graphics
    * dependency's attachment-store and encoder-split decision. */
   kk_cmd_pipeline_barriers(cmd, event_count, dependencies);
}

#endif

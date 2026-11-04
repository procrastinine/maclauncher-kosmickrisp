/* SPDX-License-Identifier: MIT */
#ifndef KK_STORED_DEPTH_BOUNDS_H
#define KK_STORED_DEPTH_BOUNDS_H
#include "kk_device.h"
#include "kk_physical_device.h"
#include "vk_graphics_state.h"
#include "kk_depth_bounds_types.h"

struct kk_interlock_envelope;
struct kk_rendering_state;
void kk_depth_bounds_set_transaction_inputs(struct kk_interlock_envelope *,
   const struct vk_input_attachment_location_state *, const struct kk_rendering_state *);
/* Bounds support follows the cached native family capability. Shared DS
 * transactions serialize command state independently of feature exposure. */
static inline bool
kk_depth_bounds_native(const struct kk_physical_device *pdev)
{
   return pdev->info.gpu_apple_family >= 10;
}

/* Also accepts snapshot->ds and snapshot->tested_aspects. Copies state into
 * the existing immutable draw packet, with no references to mutable command
 * state and no new root/descriptor ABI. A missing aspect cannot be tested. */
VkImageAspectFlags kk_depth_bounds_write_aspects(const struct vk_depth_stencil_state *,
   VkImageAspectFlags tested, VkImageAspectFlags read_only);
void kk_depth_bounds_set_transaction_state(struct kk_interlock_envelope *env,
   const struct vk_depth_stencil_state *ds, VkImageAspectFlags tested_aspects);
#endif

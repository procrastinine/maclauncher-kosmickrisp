/* SPDX-License-Identifier: MIT */
#ifndef KK_STORED_DEPTH_BOUNDS_H
#define KK_STORED_DEPTH_BOUNDS_H
#include "kk_device.h"
#include "kk_physical_device.h"
#include "vk_graphics_state.h"
#include "kk_depth_bias_control.h"

struct kk_interlock_envelope;
void kk_depth_bounds_init_dispatch(struct kk_device *dev);
struct kk_cmd_buffer;
struct kk_mesh_retained_batch;
struct kk_depth_stencil_snapshot;
struct kk_rendering_state;
void kk_depth_bounds_set_transaction_inputs(struct kk_interlock_envelope *,
   const struct vk_input_attachment_location_state *, const struct kk_rendering_state *);
VkResult kk_depth_bounds_record_mesh_batch(struct kk_cmd_buffer *,
   const struct kk_mesh_retained_batch *, void *);
/* Internal fragment stage table context. Original application per-draw state
 * keeps its original address, independently of the GPU job's storage key. */
#include "kk_depth_bounds_types.h"
VkResult kk_depth_bounds_mesh_capture_pipeline(struct kk_cmd_buffer *,
   const struct kk_mesh_retained_batch *, mtl_render_pipeline_state **);
VkResult kk_depth_bounds_mesh_capture_job(struct kk_cmd_buffer *,
   const struct kk_mesh_retained_batch *, const struct kk_depth_stencil_snapshot *,
   uint64_t env, uint64_t job, mtl_argument_table *fragment_table);

/* The public feature may include emulation later; native selectors must remain
 * tied to the physical operation, rather than to that public feature bit. */
static inline bool
kk_depth_bounds_native(const struct kk_physical_device *pdev)
{
   return pdev->info.gpu_apple_family >= 10;
}

static inline bool
kk_depth_bounds_emulated(const struct kk_device *dev)
{
   return dev->vk.enabled_features.depthBounds &&
      !kk_depth_bounds_native(kk_device_physical(dev));
}

static inline bool
kk_mesh_depth_bias_device(const struct kk_device *dev)
{
   return dev->vk.enabled_features.meshShader &&
      dev->vk.enabled_features.depthBiasControl &&
      (dev->vk.enabled_features.leastRepresentableValueForceUnormRepresentation ||
       dev->vk.enabled_features.floatRepresentation);
}

/* A pipeline that enables the test statically or makes the enable dynamic
 * selects the transaction. Pipeline hashing uses the same selection. */
static inline bool
kk_depth_bounds_selected(const struct kk_physical_device *pdev,
                         const struct vk_features *enabled,
                         const struct vk_graphics_pipeline_state *state)
{
   if (kk_mesh_depth_bias_pipeline(enabled->depthBiasControl,
          enabled->leastRepresentableValueForceUnormRepresentation ||
          enabled->floatRepresentation, state))
      return true;
   return enabled->depthBounds && !kk_depth_bounds_native(pdev) && state &&
      ((state->ds && state->ds->depth.bounds_test.enable) ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_ENABLE));
}

/* A dynamic enable also compiles the ordinary pipeline. Draws bind it while
 * the test is off and bind the transaction pipeline while it is on. */
static inline bool
kk_depth_bounds_dynamic_enable(const struct kk_device *dev,
                               const struct vk_graphics_pipeline_state *state)
{
   if (kk_mesh_depth_bias_pipeline(dev->vk.enabled_features.depthBiasControl,
          dev->vk.enabled_features.leastRepresentableValueForceUnormRepresentation ||
          dev->vk.enabled_features.floatRepresentation, state))
      return kk_depth_bias_control_flags(true, true, state) &
         (KK_DEPTH_BIAS_DYNAMIC_ENABLE | KK_DEPTH_BIAS_DYNAMIC_FACTORS);
   return state && kk_depth_bounds_emulated(dev) &&
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_ENABLE);
}
void kk_depth_bounds_route(struct kk_cmd_buffer *cmd);

static inline bool
kk_depth_bounds_pipeline(const struct kk_device *dev,
                          const struct vk_graphics_pipeline_state *state)
{
   return kk_depth_bounds_selected(kk_device_physical(dev),
                                   &dev->vk.enabled_features, state);
}

/* Also accepts snapshot->ds and snapshot->tested_aspects. Copies state into
 * the existing immutable draw packet, with no references to mutable command
 * state and no new root/descriptor ABI. A missing aspect cannot be tested. */
VkImageAspectFlags kk_depth_bounds_write_aspects(const struct vk_depth_stencil_state *,
   VkImageAspectFlags tested, VkImageAspectFlags read_only);
void kk_depth_bounds_set_transaction_state(struct kk_interlock_envelope *env,
   const struct vk_depth_stencil_state *ds, VkImageAspectFlags tested_aspects);
#endif

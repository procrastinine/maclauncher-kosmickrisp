/* SPDX-License-Identifier: MIT */
#ifndef KK_TEMPORAL_H
#define KK_TEMPORAL_H
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

struct kk_cmd_buffer;
struct kk_device;
struct kk_queue;

/* One temporal frame (DLSS Super Resolution or Ray Reconstruction inputs), in the pNext chain of a
 * vkCmdPipelineBarrier2 recorded by VKD3D-Proton's NGX core. KosmicKrisp forwards it, in command order,
 * to MacLauncher's graphics pipeline (rsProcessTemporalFrame) when that pipeline is loaded in the
 * process; the pipeline runs the selected upscaler and any effects or frame generation. driverInfo
 * names "temporal-sr" only then. */
#define KK_STRUCTURE_TYPE_TEMPORAL_FRAME_INFO 1000999101u

enum { KK_TEMPORAL_SUPER_RESOLUTION = 1, KK_TEMPORAL_RAY_RECONSTRUCTION = 2 };
enum { KK_TEMPORAL_COLOR, KK_TEMPORAL_DEPTH, KK_TEMPORAL_MOTION, KK_TEMPORAL_OUTPUT, KK_TEMPORAL_EXPOSURE,
       KK_TEMPORAL_DIFFUSE_ALBEDO, KK_TEMPORAL_SPECULAR_ALBEDO, KK_TEMPORAL_NORMALS, KK_TEMPORAL_ROUGHNESS,
       KK_TEMPORAL_SPECULAR_HIT_DISTANCE, KK_TEMPORAL_HUDLESS, KK_TEMPORAL_UI, KK_TEMPORAL_IMAGES };
enum {
   KK_TEMPORAL_AUTO_EXPOSURE = 0x1,
   KK_TEMPORAL_RESET = 0x2,
   KK_TEMPORAL_DEPTH_REVERSED = 0x4,
   KK_TEMPORAL_MOTION_DISPLAY_RES = 0x8, /* motion vectors cover the output extent */
   KK_TEMPORAL_MOTION_JITTERED = 0x10,   /* motion vectors include the sample jitter */
   KK_TEMPORAL_HDR = 0x20,               /* linear color */
   KK_TEMPORAL_CAMERA_VALID = 0x40,      /* near_plane, far_plane and vertical_fov are the frame's camera */
   KK_TEMPORAL_ROUGHNESS_NORMAL_ALPHA = 0x100, /* explicit producer packed-roughness mode */
   KK_TEMPORAL_DEPTH_INFINITE = 0x80,    /* infinite far plane; far_plane is 0 */
};

/* Optional, in a frame's pNext chain: its guide masks (VkTemporalFrameMasksInfoMVK of VKD3D-Proton's
 * vk_temporal_frame.h). Reactive is one channel from 0 to 1, where 1 relies on the current frame instead of
 * history; transparency is FidelityFX's transparency-and-composition mask. Each is read at its offset over the
 * render extent. */
#define KK_STRUCTURE_TYPE_TEMPORAL_FRAME_MASKS_INFO 1000999103u

enum { KK_TEMPORAL_MASK_REACTIVE, KK_TEMPORAL_MASK_TRANSPARENCY, KK_TEMPORAL_MASKS };

struct kk_temporal_frame_masks_info {
   VkStructureType sType;
   const void *pNext;
   VkImage images[KK_TEMPORAL_MASKS];
   VkFormat formats[KK_TEMPORAL_MASKS]; /* VK_FORMAT_UNDEFINED: the image's own */
   uint32_t x[KK_TEMPORAL_MASKS], y[KK_TEMPORAL_MASKS];
};

struct kk_temporal_frame_info {
   VkStructureType sType;
   const void *pNext;
   uint64_t feature_id; /* the temporal history: one per NGX feature */
   uint64_t frame_id;   /* > 0, new for every evaluation of the feature */
   uint32_t feature_type;
   uint32_t flags;
   VkImage images[KK_TEMPORAL_IMAGES];
   VkFormat formats[KK_TEMPORAL_IMAGES]; /* VK_FORMAT_UNDEFINED: the image's own */
   /* Regions: render_width/height of color, depth and motion at their offsets (motion at the output
    * extent with KK_TEMPORAL_MOTION_DISPLAY_RES); output_width/height of output at its offset.
    * 0: the image's extent. */
   uint32_t color_x, color_y, depth_x, depth_y, motion_x, motion_y, output_x, output_y;
   uint32_t render_width, render_height, output_width, output_height;
   float motion_vector_scale_x, motion_vector_scale_y;
   float jitter_offset_x, jitter_offset_y;
   float pre_exposure; /* 0: none */
   float sharpness;
   float frame_time_ms; /* 0: measured between submissions */
   float near_plane, far_plane, vertical_fov; /* positive view distances; radians */
   float world_to_view[16]; /* row-major, of row vectors (D3D's) */
   float view_to_clip[16];
   uint32_t release_feature_id_count;
   const uint64_t *release_feature_ids;
};

/* Whether a feature's tracked image holds a frame: a new allocation (first use, resize or format change)
 * holds none, a successful process into it makes it hold one, and a busy or failed frame keeps the previous
 * state. The game's output receives the tracked image only while it holds a frame. */
static inline bool
kk_temporal_tracked_valid(bool valid, bool reallocated, bool processed)
{
   return processed || (valid && !reallocated);
}

/* driverInfo words: " temporal-sr" when the graphics pipeline and MetalFX's temporal scaler are
 * available, or "". */
const char *kk_temporal_driver_info(void *mtl_device);
/* Recording: keeps the frame of the barrier just enqueued past the caller's memory. */
void kk_temporal_copy_enqueued(struct kk_cmd_buffer *cmd, const VkDependencyInfo *dep);
/* Recording: resolves the frame's images and ends the native part, so the frame runs between parts. */
void kk_temporal_record(struct kk_cmd_buffer *cmd, const VkDependencyInfo *dep);
/* Submission: commits the native parts before the last recorded frame, each frame on a classic
 * command buffer between them, and returns the index of the first part still to commit. */
uint32_t kk_temporal_commit_prefix(struct kk_queue *queue, struct kk_cmd_buffer *cmd);
void kk_temporal_cmd_reset(struct kk_cmd_buffer *cmd);
void kk_temporal_queue_finish(struct kk_queue *queue);
void kk_temporal_finish(struct kk_device *dev);

#endif /* KK_TEMPORAL_H */

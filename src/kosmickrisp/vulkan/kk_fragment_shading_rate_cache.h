/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_CACHE_H
#define KK_FRAGMENT_SHADING_RATE_CACHE_H
#include "util/mesa-blake3.h"
#include "util/simple_mtx.h"
#include "kosmickrisp/bridge/mtl_types.h"
#include <vulkan/vulkan_core.h>
struct kk_device;
struct kk_shader;
struct mtl_vrs_pipelines;
struct kk_vrs_device {
   simple_mtx_t compile_mutex;
};
struct kk_vrs_source;
struct kk_vrs_artifact;
enum kk_vrs_fragment_kind {
   KK_VRS_COARSE_BODY,
   KK_VRS_FINE_REPLAY,
   KK_VRS_FRAGMENT_KIND_COUNT,
};
struct msl_compile_data;
struct nir_shader;
struct kk_per_vertex_map;
struct vk_graphics_pipeline_state;
struct blob;
struct blob_reader;

/* Canonical byte encodings, not dumps of C structs. The NIR is the actual
 * linked callable application and its once-lowered fine epilogue. ABI/options include
 * lowering level, robustness, compiler/math requirements and source ABI. */
struct kk_vrs_source_data {
   const void *nir, *interface, *epilogue, *options, *abi;
   uint32_t nir_size, interface_size, epilogue_size, options_size, abi_size;
   uint32_t result_stride;
   uint32_t uses_per_draw;
   uint32_t early_fragment_tests;
};
VkResult kk_vrs_device_init(struct kk_device *dev);
void kk_vrs_device_finish(struct kk_device *dev);
struct kk_vrs_source *kk_vrs_source_create(struct kk_device *dev,
   const blake3_hash fine_key, const struct kk_vrs_source_data *data);
void kk_vrs_source_destroy(struct kk_device *dev, struct kk_vrs_source *source);
struct kk_vrs_source *kk_vrs_source_capture(struct kk_device *dev,
   struct nir_shader *nir, const struct kk_per_vertex_map *map,
   const struct vk_graphics_pipeline_state *state, const struct msl_compile_data *fine);
size_t kk_vrs_source_size(const struct kk_vrs_source *source);
/* Transfer one exact finished retained-reader sibling; shader lifetime owns both. */
void kk_vrs_source_attach_captured(struct kk_vrs_source *, struct kk_vrs_source *);
struct kk_vrs_source *kk_vrs_source_captured(const struct kk_vrs_source *);
uint32_t kk_vrs_source_result_stride(const struct kk_vrs_source *source);
void kk_vrs_source_serialize(const struct kk_vrs_source *source, struct blob *blob);
VkResult kk_vrs_source_deserialize(struct kk_device *dev,
   struct blob_reader *blob, struct kk_vrs_source **out);
const struct kk_vrs_source_data *kk_vrs_source_data(const struct kk_vrs_source *source);
/* The compiler callback uses common kk_compile_msl_options and resolves its
 * version with common kk_msl_language_version. This cache never selects a
 * separate MSL language or changes the producer's native function. */
typedef VkResult (*kk_vrs_build_fragment)(struct kk_device *dev,
   const struct kk_vrs_source_data *source, enum kk_vrs_fragment_kind kind,
   struct msl_compile_data *out,
   enum mtl_language_version *version, enum mtl_math_mode *math,
   enum mtl_math_floating_point_functions *functions, bool *framebuffer_reads);
VkResult kk_vrs_artifact_get(struct kk_device *dev, struct kk_vrs_source *source,
   enum kk_vrs_fragment_kind kind, kk_vrs_build_fragment build,
   struct kk_vrs_artifact **out);
/* Compile the actual once-lowered linked source through the common compiler.
 * Called lazily by an effective coarse draw, never ordinary pipeline creation. */
VkResult kk_vrs_fragment_get(struct kk_device *dev, struct kk_vrs_source *source,
   enum kk_vrs_fragment_kind kind, struct kk_vrs_artifact **out);
/* Producer and fragment are the actual bound pair. The command/attachment
 * owner holds their normal lifetime and owns the resulting native PSOs. */
VkResult kk_vrs_native_pipelines_create(struct kk_device *dev,
   const struct kk_shader *producer, const struct kk_shader *fragment,
   struct mtl_vrs_pipelines **out);
/* Retained-reader form. The descriptor is the actual linked raster reader,
 * borrowed read-only through creation. Native PSOs retain its functions; the
 * caller owns those PSOs through recorded execution. No producer clone or
 * interface remapping is performed here. */
VkResult kk_vrs_native_pipelines_create_for_raster(struct kk_device *dev,
   mtl_render_pipeline_descriptor *raster_descriptor,
   const struct kk_shader *fragment, struct mtl_vrs_pipelines **out);
VkResult kk_vrs_captured_pipelines_create(struct kk_device *,
   mtl_render_pipeline_descriptor *, const struct kk_shader *,
   struct mtl_vrs_pipelines **out);
void kk_vrs_artifact_unref(struct kk_device *dev, struct kk_vrs_artifact *artifact);
mtl_function_descriptor *kk_vrs_artifact_function(const struct kk_vrs_artifact *artifact);
bool kk_vrs_artifact_framebuffer_reads(const struct kk_vrs_artifact *artifact);
/* Returns a referenced artifact after dropping the compilation mutex. Native
 * PSOs belong to the original producer's attachment cache, outside this lock. */
#endif

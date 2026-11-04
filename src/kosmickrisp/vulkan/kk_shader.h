/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_SHADER_H
#define KK_SHADER_H 1

#include "kk_device_memory.h"
#include "kk_private.h"
#include "kk_compile_options.h"
#include <limits.h>

#include "kosmickrisp/bridge/mtl_format.h"

#include "poly/nir/poly_nir.h"
#include "kk_poly_raster_topology.h"

#include "vk_pipeline_cache.h"
#include "kk_fragment_shading_rate_cache.h"

#include "vk_shader.h"
#include "util/u_call_once.h"

/* Feature enumerators are bit indices, assigned once by the compiler.
 * New shader-affecting features belong here without explicit values. */
enum kk_feature_bit {
   KK_FEAT_CUSTOM_BORDER,
   KK_FEAT_NULL_DESCRIPTOR,
   KK_FEAT_IMAGE_VIEW_MIN_LOD,
   KK_FEAT_PIPELINE_STATISTICS,
   KK_FEAT_SAMPLER_MINMAX,
   KK_FEAT_CONSERVATIVE_RASTER,
   KK_FEAT_NON_SEAMLESS_CUBE,
   KK_FEAT_SPARSE_BUFFER,
   KK_FEAT_SPARSE_REPLAY,
   KK_FEAT_MESH_QUERIES,
   KK_FEAT_POLY_LAYER_REPLAY,
   KK_FEAT_FRAGMENT_SHADING_RATE,
   KK_FEAT_PIXEL_ENVELOPE,
   KK_FEAT_DGC_REPLAY,
   KK_FEAT_STANDARD_3D,
   KK_FEAT_SLICED_3D,
   KK_FEAT_LAYERED_BLOCK_VIEWS,
   KK_FEAT_IMAGE_ARRAY_ALIAS,
   KK_FEAT_SPARSE_IMAGE_QUERY,
   KK_FEAT_WRITABLE_MSAA,
   KK_FEAT_GEOMETRY_REPORTS,
   KK_FEAT_DEPTH_BOUNDS,
   KK_FEAT_COUNT,
};

typedef uint32_t kk_feature_key;
static_assert(KK_FEAT_COUNT <= sizeof(kk_feature_key) * CHAR_BIT,
              "shader feature bits fit the shader-cache key");

/* MSL query specialization and its serialized application-use metadata. */
#define KK_STATISTICS_SHADER_ABI 3u

#include "kosmickrisp/libkk/kk_mesh_types.h"
#include "kk_mesh_pipeline_api.h"

enum kk_pipeline_kind {
   KK_PIPELINE_MAIN,
   KK_PIPELINE_POLYGON,
   KK_PIPELINE_CONSERVATIVE,
   KK_PIPELINE_MESH_VIRTUAL,
   KK_PIPELINE_MESH_SLICED,
   KK_PIPELINE_MESH_VIRTUAL_SLICED,
   KK_PIPELINE_MESH_CAPTURE,
   KK_PIPELINE_MESH_READER,
   KK_PIPELINE_KIND_COUNT,
};

struct kk_cmd_buffer;
struct kk_sparse_shader_state;

struct kk_tess_info {
   enum tess_primitive_mode mode : 8;
   enum gl_tess_spacing spacing  : 8;
   bool points;
   bool ccw;
};
static_assert(sizeof(struct kk_tess_info) == 4, "packed");

static struct kk_tess_info
kk_tess_info_merge(struct kk_tess_info a, struct kk_tess_info b)
{
   static_assert(TESS_PRIMITIVE_UNSPECIFIED == 0, "zero state");
   static_assert(TESS_SPACING_UNSPECIFIED == 0, "zero state");

   /* Just merge by OR'ing the raw bits */
   uint32_t x, y;
   memcpy(&x, &a, sizeof(x));
   memcpy(&y, &b, sizeof(y));

   x |= y;

   struct kk_tess_info out;
   memcpy(&out, &x, sizeof(out));
   return out;
}

struct kk_mesh_shader_info {
   struct mtl_size local_size;
   struct mtl_size task_local_size;
   uint32_t payload_size;
   uint32_t task_payload_size;
   uint32_t task_grid_bound;
   enum mesa_prim primitive;
   bool has_task;
   bool flat_heap;
   bool projected_basis;
   uint32_t max_primitives;
   uint32_t gs_max_objects; /* Bounded internal GS object grid, zero for API Mesh. */
   bool queries;
   bool sliced;
   bool retained_capture, retained_reader;
   uint32_t max_vertices;
   uint32_t retained_strides[2];
   struct kk_mesh_retained_field retained_fields[KK_MESH_RETAINED_FIELD_COUNT];
   uint32_t view_mask;
};

/* Serialized info may change within padding without changing sizeof. */
#define KK_SHADER_INFO_CACHE_VERSION 5u

struct kk_shader_info {
   bool interlock_envelope;
   mesa_shader_stage stage;
   bool uses_per_draw_data;
   bool uses_sampler_emulation;
   uint8_t sparse_modes; /* Native code compiled for these residency classes. */
   uint8_t framebuffer_read_mask;
   uint32_t push_size;
   bool descriptor_heap;
   bool dgc_replay;
   bool dgc_interlock_guard;
   bool indirect_bindable;
   bool has_geometry;
   bool geometry_statistics;
   bool api_geometry;
   bool polygon_point;
   bool polygon_native;
   bool poly_layer_replay;
   bool conservative;
   bool conservative_full_coverage;
   uint8_t depth_bias_control;
   uint64_t conservative_canonical_outputs;
   uint64_t conservative_canonical_components[2];
   struct poly_gs_capture_info polygon;
   struct poly_gs_info geometry;
   uint8_t gs_aux_mask;

   /* Required for fragment shader cull distance discards. */
   uint8_t num_cull_distances;

   /* Outputs a VS, or a TES followed by a GS, writes for the next stage */
   uint64_t outputs_written;

   union {
      /* Vertex shader is the pipeline, store all relevant data here. */
      struct {
         /* Required to know which extra states does the pipeline contain for
          * serialization. */
         uint32_t additional_stages_bits;

         uint32_t tess_local_thread_size;

         /* Data needed to start render pass and bind pipeline. */
         uint32_t attribs_read;
         bool dynamic_vi;
         bool dynamic_multisample;
         bool dynamic_rasterization_samples;
         uint32_t sample_count;

         /* Data needed for serialization. */
         enum mtl_primitive_topology_class topology;
         enum mtl_primitive_topology_class polygon_topology;
         enum mtl_pixel_format rt_formats[MAX_DRAW_BUFFERS];
         enum mtl_pixel_format d_format;
         enum mtl_pixel_format s_format;
         uint32_t view_mask;
         VkCompareOp depth_compare_op;
         struct {
            uint8_t depth_fail;
            uint8_t fail;
            uint8_t pass;
            uint8_t compare;
            uint8_t compare_mask;
            uint8_t write_mask;
         } stencil_front, stencil_back;
         uint8_t color_attachment_count;
         bool has_ms;
         bool has_ds;
         bool has_depth_write;
         bool has_stencil_test;
      } vs;

      struct {
         uint64_t tcs_per_vertex_outputs;
         uint32_t tcs_output_stride;
         uint8_t tcs_output_patch_size;
         uint8_t tcs_nr_patch_outputs;

         struct kk_tess_info info;
      } tess;

      struct poly_gs_info gs;

      struct {
         bool uses_flat_varyings;
         bool line_geometry_stipple;
         bool emulates_per_vertex_inputs;
         bool per_vertex_uses_heap;
         uint64_t flat_outputs;
         uint64_t noperspective_outputs;
      } fs;

      struct {
         struct mtl_size local_size;
      } cs;
   };
};

/* Threadgroup size the VS, TES and GS compute passes are compiled for and
 * dispatched with */
#define KK_POLY_WG_SIZE 64u

/* Metal handles for binding. */
struct kk_pipeline_handles {
   union {
      struct {
         mtl_render_pipeline_state *render;
         mtl_render_pipeline_state *polygon_render;
         mtl_render_pipeline_state *conservative_render;
         mtl_compute_pipeline_state *polygon_capture;
         mtl_compute_pipeline_state *polygon_xfb;
         /* VS, TCS, TES and the GS compute pass at most run before the render
          * pipeline. */
         mtl_compute_pipeline_state *pre_render[4];
         mtl_compute_pipeline_state *gs_aux[2];
         util_once_flag gs_aux_once[2];
         VkResult gs_aux_result[2];
         mtl_depth_stencil_state *ds_handle;
         uint32_t pre_render_count;
      } gfx;
      mtl_compute_pipeline_state *cs;
      struct {
         mtl_compute_pipeline_state *handle; /* aliases cs */
         struct kk_async_compute *pending;
      } compute;
   };
};

static_assert(sizeof(((struct kk_pipeline_handles *)0)->compute) <=
              sizeof(((struct kk_pipeline_handles *)0)->gfx),
              "pending compute state fits the existing native-handle union");

struct msl_compile_data {
   char *code;
   char *entrypoint_name;
   bool requires_msl41;
   bool uses_matrix_coordinates;
   bool uses_atomic64_locks;
   bool uses_pipeline_statistics;
   bool uses_sampler_emulation;
   bool requires_sampler_emulation;
};

static inline enum mtl_language_version
kk_msl_language_version(const struct msl_compile_data *data,
                        enum mtl_language_version supported)
{
   assert(!data->requires_msl41 || supported >= MTL_LANGUAGE_VERSION_4_1);
   return data->requires_msl41 ? MTL_LANGUAGE_VERSION_4_1
                              : MTL_LANGUAGE_VERSION_4_0;
}

#define KK_EMBEDDED_SAMPLER_BINDING_SCHEMA 1u
struct vk_sampler_state;

/* Runtime-only Mesh auxiliaries. Never serialized. Native monolithic Mesh
 * needs none of this storage until an auxiliary pipeline is requested. */
struct kk_mesh_shader_runtime {
   struct kk_pipeline_handles *virtual;
   struct kk_pipeline_handles *sliced[2];
   struct kk_pipeline_handles *retained[2];
   struct kk_mesh_fragment_program fragment;
};

struct kk_mesh_shader_state {
   struct kk_mesh_shader_info info;
   struct kk_mesh_shader_runtime *runtime;
};

struct kk_shader {
   struct vk_shader vk;
   struct kk_interlock_shader *interlock;
   struct kk_interlock_pipeline *interlock_pipeline;
   /* Canonical states may be serialized; these CPU/GPU pointers never are. */
   bool cache_uses_embedded_samplers;
   uint32_t embedded_sampler_count;
   struct vk_sampler_state *embedded_sampler_states;
   uint64_t embedded_sampler_table;
   struct kk_sparse_shader_state *sparse;
   struct kk_shader *sparse_base; /* Weak: base owns every immutable variant. */
   /* Immutable vk.stage selects vertex, fragment, compute or Task/Mesh state. */
   union {
      struct kk_shader *gs_mesh; /* Vertex-owned internal Mesh alternative. */
      struct kk_vrs_source *vrs_source;
      mtl_compute_pipeline_state *dgc_compute;
      struct kk_mesh_shader_state *mesh;
   };

   struct kk_attachment_pipelines *attachment_pipelines;
   struct kk_lazy_polygon *lazy_polygon;
   struct kk_pipeline_handles pipeline;
   struct kk_shader_info info;
   struct msl_compile_data msl_data[MESA_ALL_SHADER_STAGES];
   /* Compute pass of a GS. Its rasterization VS is in
    * msl_data[MESA_SHADER_GEOMETRY]. */
   struct msl_compile_data gs_main;
   struct msl_compile_data polygon[6];
   struct msl_compile_data polygon_native;
   struct msl_compile_data polygon_native_fs;
   struct msl_compile_data gs_aux[2];
};

static_assert(_Alignof(struct kk_shader) >= _Alignof(struct kk_mesh_shader_state),
              "inline Mesh metadata remains aligned");

static inline bool
kk_shader_stage_has_mesh_info(mesa_shader_stage stage)
{
   return stage == MESA_SHADER_TASK || stage == MESA_SHADER_MESH;
}

static inline size_t
kk_shader_size(mesa_shader_stage stage)
{
   return sizeof(struct kk_shader) +
      (kk_shader_stage_has_mesh_info(stage) ? sizeof(struct kk_mesh_shader_state) : 0);
}

static inline void
kk_shader_init_mesh_state(struct kk_shader *shader, mesa_shader_stage stage)
{
   shader->mesh = kk_shader_stage_has_mesh_info(stage)
      ? (void *)(shader + 1) : NULL;
}

static inline const struct kk_mesh_fragment_program *
kk_mesh_fragment(const struct kk_shader *shader)
{
   const struct kk_mesh_shader_runtime *runtime = shader->mesh
      ? p_atomic_read(&shader->mesh->runtime) : NULL;
   return runtime && runtime->fragment.attach ? &runtime->fragment : NULL;
}

VK_DEFINE_NONDISP_HANDLE_CASTS(kk_shader, vk.base, VkShaderEXT,
                               VK_OBJECT_TYPE_SHADER_EXT);

VkResult kk_mesh_get_pipeline(struct kk_device *dev, struct kk_shader *shader,
   bool force_virtual, struct kk_pipeline_handles **out);

VkResult kk_mesh_get_sliced_pipeline(struct kk_device *dev, struct kk_shader *shader,
   bool virtual, struct kk_pipeline_handles **out);
void kk_mesh_release_pipeline(struct kk_device *dev, struct kk_shader *shader,
   struct kk_pipeline_handles *pipe, unsigned kind);

VkResult kk_sampler_compute_pipeline(struct kk_device *device,
   struct kk_shader *shader, mtl_compute_pipeline_state *base,
   mtl_compute_pipeline_state **out);
mtl_compute_pipeline_state *kk_cmd_sampler_compute_pipeline_used(
   struct kk_cmd_buffer *cmd, struct kk_shader *shader,
   mtl_compute_pipeline_state *base);
static inline mtl_compute_pipeline_state *
kk_cmd_sampler_compute_pipeline(struct kk_cmd_buffer *cmd,
   struct kk_shader *shader, mtl_compute_pipeline_state *base)
{
   return shader->info.uses_sampler_emulation ?
      kk_cmd_sampler_compute_pipeline_used(cmd, shader, base) : base;
}
bool kk_attachment_sampler_emulation_changed(struct kk_cmd_buffer *cmd,
   struct kk_shader *shader);
bool kk_attachment_sampler_emulation_value(struct kk_device *device,
   const struct kk_shader *shader);

extern const struct vk_device_shader_ops kk_device_shader_ops;
void kk_sparse_shader_finish(struct kk_shader *shader);
struct kk_shader *kk_sparse_select_shader(struct kk_cmd_buffer *cmd, struct kk_shader *shader);
void kk_sparse_refresh_shader(struct kk_cmd_buffer *cmd, struct kk_cmd_buffer *state,
                              mesa_shader_stage stage);

static inline nir_address_format
kk_buffer_addr_format(VkPipelineRobustnessBufferBehaviorEXT robustness)
{
   switch (robustness) {
   case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED:
      return nir_address_format_64bit_global_32bit_offset;
   case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS:
   case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2:
      return nir_address_format_64bit_bounded_global;
   default:
      UNREACHABLE("Invalid robust buffer access behavior");
   }
}

bool kk_nir_lower_sampler_helpers(nir_shader *nir, uint32_t set_layout_count,
   struct vk_descriptor_set_layout *const *set_layouts, bool minmax, bool custom);

bool kk_nir_lower_non_seamless_cube(nir_shader *nir,
      uint32_t set_layout_count,
      struct vk_descriptor_set_layout *const *set_layouts);

VkResult kk_shader_embedded_samplers_init(struct kk_device *dev,
   struct kk_shader *shader, uint32_t count, const void *state_bytes);
void kk_shader_embedded_samplers_finish(struct kk_device *dev,
   struct kk_shader *shader);

bool kk_nir_prepare_descriptor_heaps(nir_shader *nir, uint32_t embedded_count, const uint64_t *embedded, bool sparse_buffers);
bool kk_nir_finish_sparse_heap_buffers(nir_shader *nir);

bool kk_nir_prepare_layered_bc_cubes(nir_shader *nir,
   const struct vk_pipeline_robustness_state *rs, uint32_t set_layout_count,
   struct vk_descriptor_set_layout *const *set_layouts, bool descriptor_heap);
bool kk_nir_prepare_layered_bc(nir_shader *nir,
   const struct vk_pipeline_robustness_state *rs, uint32_t set_layout_count,
   struct vk_descriptor_set_layout *const *set_layouts, bool descriptor_heap);

bool
kk_nir_lower_descriptors(nir_shader *nir, kk_feature_key features,
                         const struct vk_pipeline_robustness_state *rs,
                         uint32_t set_layout_count,
                         struct vk_descriptor_set_layout *const *set_layouts);
bool kk_nir_lower_descriptors_for_binding_model(nir_shader *nir, kk_feature_key features,
   const struct vk_pipeline_robustness_state *rs, uint32_t set_layout_count,
   struct vk_descriptor_set_layout *const *set_layouts, bool raw_texel);

/* KK_WORKAROUND_18 */
bool kk_nir_add_device_barrier_workaround(nir_shader *nir);

bool kk_nir_lower_poly(struct nir_shader *nir, bool statistics);
bool kk_nir_lower_null_images(nir_shader *nir);
bool kk_nir_lower_sparse(nir_shader *nir);
bool kk_nir_lower_standard_3d_samples(nir_shader *,uint32_t,struct vk_descriptor_set_layout *const *);
bool kk_nir_lower_sparse_instruction(nir_builder *b, nir_instr *instr,
   nir_def *logical_resident, nir_def *zero_one_mask);
bool kk_nir_lower_sparse_border(nir_shader *nir);
bool kk_nir_lower_sparse_sample_views(nir_shader *nir, uint32_t count, struct vk_descriptor_set_layout *const *layouts, bool minmax);
bool kk_nir_lower_sparse_buffer_io(nir_shader *nir,
   const struct vk_pipeline_robustness_state *rs, uint32_t count,
   struct vk_descriptor_set_layout *const *layouts);
nir_def *kk_nir_sparse_buffer_resident(nir_builder *b, nir_def *metadata, nir_def *coord);
nir_def *kk_nir_sparse_view_address(nir_builder *b, nir_def *address, nir_def *query_record, nir_def **lod, bool storage, bool query_array);

bool kk_nir_lower_textures(nir_shader *nir);

bool kk_nir_lower_vs_multiview(nir_shader *nir, uint32_t view_mask);
bool kk_nir_lower_fs_multiview(nir_shader *nir, uint32_t view_mask);

/* Indicates that the sampler should be overridden to clamp to 0 instead of 1 */
#define KK_TEXTURE_FLAG_CLAMP_TO_0 (1 << 0)

bool kk_nir_lower_custom_border(nir_shader *nir);
bool kk_nir_lower_image_view_min_lod(nir_shader *nir);

VkResult kk_compile_nir_shader(struct kk_device *dev, nir_shader *nir,
                               const VkAllocationCallbacks *alloc,
                               struct kk_shader **shader_out);

VkResult kk_shader_ensure_polygon(struct kk_device *dev, struct kk_shader *shader);
VkResult kk_shader_ensure_geometry_reports(struct kk_device *dev,
   struct kk_shader *shader, unsigned mask);
VkResult kk_shader_get_statistics_pre_gs(struct kk_device *dev,
   struct kk_shader *shader, mtl_compute_pipeline_state **pipeline);

mtl_render_pipeline_state *kk_attachment_pipeline(struct kk_cmd_buffer *cmd,
   struct kk_shader *shader, unsigned kind);
mtl_depth_stencil_state *kk_attachment_depth_stencil(struct kk_cmd_buffer *cmd,
   struct kk_shader *shader);

void kk_cmd_bind_compute_shader(struct kk_cmd_buffer *cmd,
                                struct kk_shader *shader);

#endif /* KK_SHADER_H */

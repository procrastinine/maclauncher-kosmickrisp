/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_TYPES_H
#define KK_MESH_TYPES_H

/* One bounded payload arena, reused after each batch's mesh dispatch. */
/* Optional retained raster output, indexed by original physical group and
 * primitive slot. The existing slot5 parameter layout is unchanged:
 * capture/reader-only copies use indirect_count as this cache's GPU address;
 * reader-only copies use predicate_inverted as logical Layer. Prepare/filter
 * kernels always consume the original, unmodified parameters. Optional
 * reader-only records/indirect_grid/max_draw_count select a GPU primitive job
 * and validate its original grid/counts; records=0 retains batch replay. */
/* Working-set target, never a logical-dispatch or shader-output limit. */
#define KK_MESH_RETAINED_BYTES (32u * 1024u * 1024u)
struct kk_mesh_retained_count {
   uint32_t vertices, primitives;
   uint32_t reserved[2];
};

/* Job IDs name original storage slots, never shader-written PrimitiveID. */
struct kk_mesh_retained_job {
   uint32_t physical_group, original_primitive_slot;
};
static inline bool
kk_mesh_retained_job_group_valid(uint32_t group, uint32_t x, uint32_t y,
                                 uint32_t z, uint32_t capacity)
{
   if (!x || !y || !z || group >= capacity) return false;
   uint64_t plane = (uint64_t)x * y;
   if (plane > capacity || z > capacity / plane) return false;
   return group < plane * z;
}
static inline bool
kk_mesh_retained_job_slot_valid(uint32_t slot, uint32_t vertices,
   uint32_t primitives, uint32_t max_vertices, uint32_t max_primitives)
{
   return vertices <= max_vertices && primitives <= max_primitives && slot < primitives;
}

#define KK_MESH_RETAINED_ID_FIELD 0u
#define KK_MESH_RETAINED_RATE_FIELD 1u
#define KK_MESH_RETAINED_LAYER_FIELD 2u
#define KK_MESH_RETAINED_VIEW_FIELD 3u
#define KK_MESH_RETAINED_FIELD_COUNT 4u
#define KK_MESH_RETAINED_FIELD_ABSENT 0u
#define KK_MESH_RETAINED_FIELD_VERTEX 1u
#define KK_MESH_RETAINED_FIELD_PRIMITIVE 2u
#define KK_MESH_RETAINED_FIELD_PER_DRAW 3u
#define KK_MESH_RETAINED_FIELD_CONSTANT 4u
/* Four serialized scalar words. type is the actual emitted nir_alu_type.
 * Offsets are relative to the named vertex/primitive/per-draw record; for a
 * constant source, offset is its value. ABSENT is never replaced by a job ID. */
struct kk_mesh_retained_field {
   uint32_t source, offset, type, components;
};

#define KK_MESH_BATCH_SIZE 256u
#define KK_MESH_MAX_GROUPS 4194304u
#define KK_MESH_NATIVE_GROUPS 1024u
#define KK_MESH_MAX_DIMENSION 65535u

struct kk_mesh_task_record {
   uint32_t grid[3];
   uint32_t prefix_end;
};

struct kk_mesh_batch {
   uint32_t task_dispatch[3];
   uint32_t task_count;
   uint32_t mesh_dispatch[3];
   uint32_t mesh_count;
   uint32_t input_grid[3];
   uint32_t pad;
};

/* The same layout is included in C, OpenCL and generated MSL. The shared
 * argument-table declaration owns the mesh parameter binding. */
struct kk_mesh_params {
   uint64_t records;
   uint64_t payloads;
   uint64_t batch;
   uint64_t indirect_grid;
   uint64_t indirect_count;
   uint64_t predicate;
   uint64_t task_statistic;
   uint64_t mesh_statistic;
   uint64_t primitive_statistic;
   uint32_t task_base;
   uint32_t payload_stride;
   uint32_t draw_id;
   uint32_t max_draw_count;
   uint32_t predicate_inverted;
   uint32_t pad;
};

#endif

/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_ABI_H
#define KK_RT_ABI_H
#ifdef __METAL_VERSION__
#include <metal_stdlib>
using namespace metal;
#define KK_RT_ABI_U64 ulong
#define KK_RT_ABI_U32 uint
#define KK_RT_ABI_ASSERT static_assert
#else
#include <stdint.h>
#include <stddef.h>
#define KK_RT_ABI_U64 uint64_t
#define KK_RT_ABI_U32 uint32_t
#define KK_RT_ABI_ASSERT _Static_assert
#endif
/* SPIR-V RayFlagsKHR values shared by C and Metal control. */
enum kk_rt_ray_flag {
   KK_RT_RAY_TERMINATE_ON_FIRST_HIT = 0x4,
   KK_RT_RAY_SKIP_CLOSEST_HIT = 0x8,
};
enum kk_rt_action {
   KK_RT_RETURN,
   KK_RT_TRACE,
   KK_RT_CALLABLE,
   KK_RT_REPORT,
   KK_RT_IGNORE,
   KK_RT_TERMINATE,
   KK_RT_CANDIDATE,
   KK_RT_REPORT_DONE,
   KK_RT_TRAVERSAL_DONE,
};
struct kk_rt_cps_context {
   KK_RT_ABI_U64 root;
   KK_RT_ABI_U64 stack_base;
   KK_RT_ABI_U64 frame_base;
   KK_RT_ABI_U64 stack_size;
   KK_RT_ABI_U64 payload;
   KK_RT_ABI_U64 hit_attributes;
   KK_RT_ABI_U64 shader_record;
   KK_RT_ABI_U32 action;
   KK_RT_ABI_U32 resume;
   KK_RT_ABI_U32 frame_size;
   KK_RT_ABI_U32 callable_index;
   float report_t;
   KK_RT_ABI_U32 report_kind;
   KK_RT_ABI_U32 report_accepted;
   KK_RT_ABI_U32 reserved;
   KK_RT_ABI_U64 acceleration_structure;
   KK_RT_ABI_U32 flags;
   KK_RT_ABI_U32 cull_mask;
   KK_RT_ABI_U32 sbt_offset;
   KK_RT_ABI_U32 sbt_stride;
   KK_RT_ABI_U32 miss_index;
   KK_RT_ABI_U32 reserved2;
   float origin[3];
   float tmin;
   float direction[3];
   float tmax;
   KK_RT_ABI_U64 sampler_table;
   KK_RT_ABI_U32 launch_id[3];
   KK_RT_ABI_U32 launch_size[3];
   KK_RT_ABI_U32 lane;
   KK_RT_ABI_U32 subgroup_size;
   float object_origin[3];
   float object_direction[3];
   float object_to_world[12];
   float world_to_object[12];
   KK_RT_ABI_U32 primitive_id;
   KK_RT_ABI_U32 geometry_id;
   KK_RT_ABI_U32 instance_id;
   KK_RT_ABI_U32 instance_custom_index;
   KK_RT_ABI_U32 hit_kind;
   KK_RT_ABI_U32 reserved3;
   KK_RT_ABI_U64 embedded_sampler_table;
   KK_RT_ABI_U64 dispatch;
   KK_RT_ABI_U32 query_event;
   KK_RT_ABI_U32 candidate_type;
   KK_RT_ABI_U32 candidate_opaque;
   KK_RT_ABI_U32 query_accept;
   float candidate_t;
   float candidate_bary[2];
   KK_RT_ABI_U32 candidate_sbt;
   KK_RT_ABI_U32 committed_type;
   KK_RT_ABI_U32 caller_shader;
   KK_RT_ABI_U32 hit_group;
   KK_RT_ABI_U32 terminate;
   KK_RT_ABI_U64 report_attributes;
   KK_RT_ABI_U64 child_scope;
   float triangle_vertices[9];
   KK_RT_ABI_U32 vertex_pad;
};
KK_RT_ABI_ASSERT(__builtin_offsetof(struct kk_rt_cps_context, action) == 56, "CPS action offset");
KK_RT_ABI_ASSERT(__builtin_offsetof(struct kk_rt_cps_context, acceleration_structure) == 88, "CPS trace arguments");
KK_RT_ABI_ASSERT(__builtin_offsetof(struct kk_rt_cps_context, origin) == 120, "CPS origin offset");
KK_RT_ABI_ASSERT(__builtin_offsetof(struct kk_rt_cps_context, embedded_sampler_table) == 336, "per-program sampler context offset");
KK_RT_ABI_ASSERT(sizeof(struct kk_rt_cps_context) == 456, "CPS scalar ABI");

#undef KK_RT_ABI_U64
#undef KK_RT_ABI_U32
#undef KK_RT_ABI_ASSERT
#endif

/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_DISPATCH_H
#define KK_RT_DISPATCH_H
#ifndef KK_RT_STACK_H
#include "kk_rt_stack.h"
#endif
#ifndef KK_RT_SBT_H
#include "kk_rt_sbt.h"
#endif
#ifdef __METAL_VERSION__
#define KK_RT_U64 ulong
#define KK_RT_U32 uint
#define KK_RT_ASSERT static_assert
#define KK_RT_TABLE visible_function_table<void(device uchar *)>
#else
#define KK_RT_U64 uint64_t
#define KK_RT_U32 uint32_t
#define KK_RT_ASSERT _Static_assert
#define KK_RT_TABLE uint64_t
#endif

enum kk_rt_function_kind {KK_RT_FUNCTION_APPLICATION,KK_RT_FUNCTION_CONTROL};
enum kk_rt_frame_role {KK_RT_ROLE_APPLICATION,KK_RT_ROLE_TRAVERSAL,KK_RT_ROLE_INTERSECTION,KK_RT_ROLE_ANY_HIT,KK_RT_ROLE_CLOSEST_MISS,KK_RT_ROLE_CALLABLE};
struct kk_rt_function_info {KK_RT_U32 kind,shader;};
struct kk_rt_program_info {KK_RT_U32 first,count;KK_RT_U64 scratch_size,attribute_size,embedded_sampler_table;};
/* Matches Vulkan's GPU-written VkTraceRaysIndirectCommand2KHR. */
struct kk_rt_indirect2 {
   KK_RT_U64 raygen_address,raygen_size;
   KK_RT_U64 miss_address,miss_size,miss_stride;
   KK_RT_U64 hit_address,hit_size,hit_stride;
   KK_RT_U64 callable_address,callable_size,callable_stride;
   KK_RT_U32 width,height,depth,pad;
};
/* First failed logical launch, retained until command-buffer retirement. */
struct kk_rt_error_report {
   KK_RT_U32 claimed,error,action,shader,hit_group,pad;
   KK_RT_U64 launch,stack_used,stack_size;
};
KK_RT_ASSERT(sizeof(struct kk_rt_error_report)==48,"RT error report scalar ABI");
struct kk_rt_dispatch_root {
   KK_RT_TABLE functions;
   KK_RT_U64 function_info,program_info,groups;
   KK_RT_U64 root,sampler_table,lanes,stack_base,stack_stride,indirect_dimensions,indirect2;
   KK_RT_U64 error_report;
   struct kk_rt_sbt_region raygen,miss,hit,callable;
   KK_RT_U32 dimensions[3],physical_count;
   KK_RT_U32 function_count,program_count,group_count,control_pc;
   KK_RT_U32 traversal_program,max_trace_depth;
   KK_RT_U64 attribute_size;
};
KK_RT_ASSERT(sizeof(struct kk_rt_function_info)==8,"VFT metadata ABI");
KK_RT_ASSERT(sizeof(struct kk_rt_program_info)==32,"program scalar ABI");
KK_RT_ASSERT(sizeof(struct kk_rt_indirect2)==104,"Vulkan indirect2 ABI");
KK_RT_ASSERT(__builtin_offsetof(struct kk_rt_indirect2,width)==88,"indirect2 dimension offset");
KK_RT_ASSERT(sizeof(struct kk_rt_dispatch_root)==240,"RT root scalar ABI");
#ifndef __METAL_VERSION__
char *kk_rt_dispatcher_source(void *mem_ctx);
char *kk_rt_control_source(void *mem_ctx);
#endif
#undef KK_RT_U64
#undef KK_RT_U32
#undef KK_RT_ASSERT
#undef KK_RT_TABLE
#endif

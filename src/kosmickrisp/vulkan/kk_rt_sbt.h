/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_SBT_H
#define KK_RT_SBT_H
#ifndef KK_RT_STACK_H
#include "kk_rt_stack.h"
#endif
#ifdef __METAL_VERSION__
#define KK_RT_DEVICE device
#define KK_RT_THREAD thread
#define KK_RT_U64 ulong
#define KK_RT_U32 uint
#define KK_RT_ASSERT static_assert
#else
#define KK_RT_DEVICE
#define KK_RT_THREAD
#define KK_RT_U64 uint64_t
#define KK_RT_U32 uint32_t
#define KK_RT_ASSERT _Static_assert
#endif

/* Device-scoped identity, never a native function index or serialized pointer.
 * The common RT backend interns complete group keys with equality checking;
 * importing a group retains its token while every executable owns its map. */
struct kk_rt_group_token {
   KK_RT_U64 device_namespace[2];
   KK_RT_U64 group;
   KK_RT_U32 type;
   KK_RT_U32 abi;
};
struct kk_rt_group_entry {
   struct kk_rt_group_token token;
   KK_RT_U32 general;
   KK_RT_U32 closest_hit;
   KK_RT_U32 any_hit;
   KK_RT_U32 intersection;
};
struct kk_rt_sbt_region {
   KK_RT_U64 address;
   KK_RT_U64 stride;
   KK_RT_U64 size;
};
enum kk_rt_sbt_result {
   KK_RT_SBT_INVALID,
   KK_RT_SBT_NULL,
   KK_RT_SBT_FOUND,
};
KK_RT_ASSERT(sizeof(struct kk_rt_group_token) == 32, "Vulkan group handle width");
KK_RT_ASSERT(sizeof(struct kk_rt_group_entry) == 48, "GPU group map scalar ABI");
KK_RT_ASSERT(sizeof(struct kk_rt_sbt_region) == 24, "Vulkan SBT region scalar ABI");

static inline bool
kk_rt_sbt_record_address(struct kk_rt_sbt_region region, KK_RT_U64 index,
                          KK_RT_THREAD KK_RT_U64 *address)
{
   KK_RT_U64 offset, end;
   /* A zero stride selects the same record for every index. */
   if (!region.address || (region.address & 63) || (region.stride & 31) ||
       !kk_rt_u64_mul(index, region.stride, &offset) ||
       !kk_rt_u64_add(offset, sizeof(struct kk_rt_group_token), &end) ||
       end > region.size ||
       !kk_rt_u64_add(region.address, region.size, &end))
      return false;
   return kk_rt_u64_add(region.address, offset, address);
}

static inline bool
kk_rt_sbt_hit_index(KK_RT_U32 instance_offset, KK_RT_U32 geometry,
                     KK_RT_U32 ray_offset, KK_RT_U32 ray_stride,
                     KK_RT_THREAD KK_RT_U64 *index)
{
   KK_RT_U64 geometry_offset, base;
   /* TraceRay's offset and stride consume their low four bits, matching the
    * common portable RT path. Instance contribution consumes24 bits. Every
    * multiplication and addition still takes place at full address width. */
   return kk_rt_u64_mul(geometry, ray_stride & 15u, &geometry_offset) &&
          kk_rt_u64_add(instance_offset & 0xffffffu, ray_offset & 15u, &base) &&
          kk_rt_u64_add(base, geometry_offset, index);
}

static inline enum kk_rt_sbt_result
kk_rt_sbt_resolve(struct kk_rt_sbt_region region, KK_RT_U64 index,
                   KK_RT_DEVICE const struct kk_rt_group_entry *groups,
                   KK_RT_U32 group_count,
                   KK_RT_THREAD KK_RT_U32 *group_index,
                   KK_RT_THREAD KK_RT_U64 *shader_record)
{
   *group_index = KK_RT_STOP;
   *shader_record = 0;
   if (!region.size)
      return KK_RT_SBT_NULL;
   KK_RT_U64 address;
   if (!kk_rt_sbt_record_address(region, index, &address))
      return KK_RT_SBT_INVALID;
   /* Null/empty paths return before any pointer conversion or device load. */
   KK_RT_DEVICE const volatile struct kk_rt_group_token *token =
      (KK_RT_DEVICE const volatile struct kk_rt_group_token *)address;
   if (!(token->device_namespace[0] | token->device_namespace[1] | token->group |
         token->type | token->abi))
      return KK_RT_SBT_NULL;
   /* The pipeline-owned map is sorted by unique device-scoped group ID. */
   KK_RT_U32 first = 0, count = group_count;
   while (count) {
      KK_RT_U32 step = count / 2, candidate = first + step;
      KK_RT_DEVICE const struct kk_rt_group_entry *entry =
         (KK_RT_DEVICE const struct kk_rt_group_entry *)
            ((KK_RT_DEVICE const unsigned char *)groups +
             (KK_RT_U64)candidate * sizeof(struct kk_rt_group_entry));
      KK_RT_U64 group = entry->token.group;
      if (group < token->group) {
         first = candidate + 1;
         count -= step + 1;
      } else {
         count = step;
      }
   }
   if (first == group_count)
      return KK_RT_SBT_INVALID;
   KK_RT_DEVICE const struct kk_rt_group_token *found =
      &((KK_RT_DEVICE const struct kk_rt_group_entry *)
         ((KK_RT_DEVICE const unsigned char *)groups +
          (KK_RT_U64)first * sizeof(struct kk_rt_group_entry)))->token;
   if (found->group != token->group || found->type != token->type || found->abi != token->abi ||
       found->device_namespace[0] != token->device_namespace[0] ||
       found->device_namespace[1] != token->device_namespace[1])
      return KK_RT_SBT_INVALID;
   *group_index = first;
   /* record_address checked the complete handle lies inside the full-width
    * region. This points immediately after the32-byte handle, as Vulkan asks. */
   *shader_record = address + sizeof(struct kk_rt_group_token);
   return KK_RT_SBT_FOUND;
}

#undef KK_RT_DEVICE
#undef KK_RT_THREAD
#undef KK_RT_U64
#undef KK_RT_U32
#undef KK_RT_ASSERT
#endif

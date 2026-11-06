// SPDX-License-Identifier: MIT
struct kk_mesh_sysvals {
   uint3 group_id;
   uint3 group_count;
   uint3 local_id;
   uint local_index;
   uint subgroup_id;
   uint subgroup_count;
   uint subgroup_lane;
   uint subgroup_size;
};

inline mem_flags kk_payload_flags(const object_data uchar *p)
{
   return mem_flags::mem_threadgroup;
}
inline mem_flags kk_payload_flags(const device uchar *p)
{
   return mem_flags::mem_device;
}

template<typename T> inline T
kk_payload_load(const object_data uchar *p, uint offset)
{
   return *((const object_data T *)(p + offset));
}
template<typename T> inline T
kk_payload_load(const device uchar *p, uint offset)
{
   return *((const device T *)(p + offset));
}
template<typename T> inline void
kk_payload_store(object_data uchar *p, uint offset, T value)
{
   *((object_data T *)(p + offset)) = value;
}
template<typename T> inline void
kk_payload_store(device uchar *p, uint offset, T value)
{
   *((device T *)(p + offset)) = value;
}

inline void kk_payload_barrier(object_data uchar *p, mem_flags flags)
{
   threadgroup_barrier(flags | mem_flags::mem_threadgroup);
}
inline void kk_payload_barrier(device uchar *p, mem_flags flags)
{
   threadgroup_barrier(flags | mem_flags::mem_device);
}

inline void kk_emit_mesh(mesh_grid_properties grid, uint3 count)
{
   grid.set_threadgroups_per_grid(count);
}
inline void kk_emit_mesh(device kk_mesh_task_record *record, uint3 count)
{
   record->grid[0] = count.x;
   record->grid[1] = count.y;
   record->grid[2] = count.z;
}

inline uint3 kk_mesh_unflatten(uint i, uint3 count)
{
   return uint3(i % count.x, (i / count.x) % count.y,
                i / (count.x * count.y));
}

inline uint kk_mesh_parent(uint index, device const kk_mesh_task_record *r,
                           uint count)
{
   uint lo = 0, hi = count;
   while (lo < hi) {
      uint mid = lo + (hi - lo) / 2;
      if (r[mid].prefix_end <= index)
         lo = mid + 1;
      else
         hi = mid;
   }
   return lo;
}

inline void kk_mesh_stat(uint64_t address, uint value)
{
   if (!address)
      return;
   device atomic_uint *counter = (device atomic_uint *)address;
   uint old = atomic_fetch_add_explicit(counter, value, memory_order_relaxed);
   if (old + value < old)
      atomic_fetch_add_explicit(counter + 1, 1u, memory_order_relaxed);
}

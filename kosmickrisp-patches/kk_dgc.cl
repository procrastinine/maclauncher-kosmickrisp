/* SPDX-License-Identifier: MIT */
#include "compiler/libcl/libcl_vk.h"
#include "poly/geometry.h"

/* Poly consumes a known 32-bit index binding. Resolve generated index address,
 * size and type on the GPU into a reserved prefix of the ordinary Poly heap.
 * Later Poly allocations retain that prefix until this sequence has rasterized. */
KERNEL(1024)
libkk_dgc_indices(global struct poly_heap *heap, constant uint *in_draw,
                 global uint *out_draw, constant uint *binding,
                 global uint *floor, uint restart, uint restart_index)
{
   uint count = in_draw[1] ? in_draw[0] : 0;
   if (cl_local_id.x == 0) {
      if ((uint64_t)count * 4 + 16 >= heap->size)
         abort();
      uint reserved = count ? align(count * 4, 16) + 16 : 0;
      *floor = reserved;
      heap->bottom = reserved;
      for (uint i = 0; i < 5; ++i)
         out_draw[i] = in_draw[i];
      out_draw[0] = count;
      out_draw[2] = 0;
   }
   barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);
   uint64_t address = (uint64_t)binding[0] | ((uint64_t)binding[1] << 32);
   uint size = binding[2], range = binding[3];
   uint mask = size == 4 ? ~0u : (size == 2 ? 0xffffu : 0xffu);
   global uint *out = (global uint *)heap->base;
   for (uint i = cl_local_id.x; i < count; i += 1024) {
      uint64_t offset = ((uint64_t)in_draw[2] + i) * size;
      uint value = 0;
      if (address && size && offset + size <= range) {
         global uchar *src = (global uchar *)(address + offset);
         value = size == 4 ? *(global uint *)src :
                 size == 2 ? *(global ushort *)src : *src;
      }
      if (restart && value == (restart_index & mask))
         value = restart_index;
      out[i] = value;
   }
}

KERNEL(1)
libkk_dgc_reserve_heap(global struct poly_heap *heap, constant uint *floor)
{
   heap->bottom = *floor;
}

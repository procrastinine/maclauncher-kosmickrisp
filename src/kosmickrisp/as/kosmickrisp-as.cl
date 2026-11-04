/*
 * Copyright 2026 procrastinine
 * Copyright 2022 Bas Nieuwenhuizen
 * SPDX-License-Identifier: MIT
 *
 * The radix-tree topology is adapted from Mesa's common
 * src/vulkan/runtime/bvh/lbvh_main.comp (4f071f891d63396c799b267172c19ba6b0301682).
 * Leaves use their unique, already sorted ordinal as a radix key. This preserves
 * the common LBVH topology algorithm without requiring spatial sorting for
 * correctness. Every byte offset and dispatch product is 64-bit.
 */
#include "compiler/libcl/libcl.h"
#include "kosmickrisp-as-types.h"

static inline uint64_t
kk_as_id(void)
{
   return (uint64_t)cl_global_id.x + (uint64_t)cl_global_size.x *
      ((uint64_t)cl_global_id.y + (uint64_t)cl_global_size.y * cl_global_id.z);
}

static inline global struct kk_as_node *
kk_as_node_at(global struct kk_as_header *h, uint64_t id)
{
   return (global struct kk_as_node *)((global uchar *)h + sizeof(*h) +
                                      id * (uint64_t)sizeof(struct kk_as_node));
}

static inline uint64_t
kk_as_node_offset(uint64_t id)
{
   return sizeof(struct kk_as_header) + id * (uint64_t)sizeof(struct kk_as_node);
}

static inline global struct kk_as_node *
kk_as_node_ref(global struct kk_as_header *h, uint64_t offset)
{
   return (global struct kk_as_node *)((global uchar *)h + offset);
}

KERNEL(64)
kk_as_initialize(global struct kk_as_header *dst,
                  constant struct kk_as_header *layout,
                  global const struct kk_as_input *inputs)
{
   uint64_t id = kk_as_id();
   if (id == 0)
      *dst = *layout;
   if (id < layout->node_count) {
      global struct kk_as_node *n = kk_as_node_at(dst, id);
      *n = (struct kk_as_node){0};
      for (uint axis = 0; axis < 3; ++axis) {
         n->min[axis] = INFINITY;
         n->max[axis] = -INFINITY;
      }
      n->type = KK_AS_NODE_INACTIVE;
   }
   if (id < layout->primitive_count) {
      global struct kk_as_payload *p = (global struct kk_as_payload *)
         ((global uchar *)dst + layout->payload_offset);
      p[id] = (struct kk_as_payload){0};
   }
   if (id < layout->geometry_count) {
      global struct kk_as_geometry *g = (global struct kk_as_geometry *)
         ((global uchar *)dst + layout->geometry_offset);
      g[id] = (struct kk_as_geometry){inputs[id].first_primitive,
         inputs[id].primitive_count, inputs[id].type, inputs[id].flags, inputs[id].micromap};
   }
}

static inline float
kk_as_snorm(int value, int maximum)
{
   return fmax(-1.0f, (float)value / (float)maximum);
}

static inline float3
kk_as_vertex(global const uchar *address, uint format)
{
   global const ushort *u16 = (global const ushort *)address;
   global const short *s16 = (global const short *)address;
   global const char *s8 = (global const char *)address;
   switch (format) {
   case KK_AS_R32G32_SFLOAT: {
      global const float *v = (global const float *)address;
      return (float3)(v[0], v[1], 0);
   }
   case KK_AS_R32G32B32_SFLOAT:
      return vload3(0, (global const float *)address);
   case KK_AS_R16G16_SFLOAT:
      return (float3)(vload_half(0, (global const half *)address),
                      vload_half(1, (global const half *)address), 0);
   case KK_AS_R16G16B16A16_SFLOAT:
      return vload_half3(0, (global const half *)address);
   case KK_AS_R16G16_SNORM:
      return (float3)(kk_as_snorm(s16[0], 32767), kk_as_snorm(s16[1], 32767), 0);
   case KK_AS_R16G16B16A16_SNORM:
      return (float3)(kk_as_snorm(s16[0], 32767), kk_as_snorm(s16[1], 32767),
                      kk_as_snorm(s16[2], 32767));
   case KK_AS_R16G16_UNORM:
      return (float3)((float)u16[0] / 65535, (float)u16[1] / 65535, 0);
   case KK_AS_R16G16B16A16_UNORM:
      return convert_float3(vload3(0, u16)) / 65535;
   case KK_AS_A2B10G10R10_UNORM: {
      uint v = *(global const uint *)address;
      return (float3)(v & 1023, (v >> 10) & 1023, (v >> 20) & 1023) / 1023;
   }
   case KK_AS_R8G8_UNORM:
      return (float3)((float)address[0] / 255, (float)address[1] / 255, 0);
   case KK_AS_R8G8B8A8_UNORM:
      return convert_float3(vload3(0, address)) / 255;
   case KK_AS_R8G8_SNORM:
      return (float3)(kk_as_snorm(s8[0], 127), kk_as_snorm(s8[1], 127), 0);
   case KK_AS_R8G8B8A8_SNORM:
      return (float3)(kk_as_snorm(s8[0], 127), kk_as_snorm(s8[1], 127),
                      kk_as_snorm(s8[2], 127));
   }
   return (float3)(NAN);
}

static inline float3
kk_as_transform(global const float *m, float3 v)
{
   return (float3)(dot(vload3(0, m), v) + m[3],
                   dot(vload3(0, m + 4), v) + m[7],
                   dot(vload3(0, m + 8), v) + m[11]);
}

static inline void
kk_as_inverse(global float *out, global const float *m)
{
   float a = m[0], b = m[1], c = m[2];
   float d = m[4], e = m[5], f = m[6];
   float g = m[8], h = m[9], i = m[10];
   float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
   out[0] = (e * i - f * h) / det;
   out[1] = (c * h - b * i) / det;
   out[2] = (b * f - c * e) / det;
   out[4] = (f * g - d * i) / det;
   out[5] = (a * i - c * g) / det;
   out[6] = (c * d - a * f) / det;
   out[8] = (d * h - e * g) / det;
   out[9] = (b * g - a * h) / det;
   out[10] = (a * e - b * d) / det;
   for (uint row = 0; row < 3; ++row)
      out[4 * row + 3] = -(out[4 * row] * m[3] + out[4 * row + 1] * m[7] +
                             out[4 * row + 2] * m[11]);
}

KERNEL(64)
kk_as_convert_leaves(global struct kk_as_header *h,
                     global const struct kk_as_input *inputs)
{
   uint64_t id = kk_as_id();
   if (id >= h->primitive_count)
      return;

   /* Empty geometries may share a prefix. upper_bound selects the first
    * nonempty range containing this primitive. */
   uint lo = 0, hi = h->geometry_count;
   while (lo < hi) {
      uint mid = lo + (hi - lo) / 2;
      if (inputs[mid].first_primitive + inputs[mid].primitive_count <= id)
         lo = mid + 1;
      else
         hi = mid;
   }
   struct kk_as_input in = inputs[lo];
   uint64_t primitive = id - in.first_primitive;
   global struct kk_as_node *n = kk_as_node_at(h, h->primitive_count - 1 + id);
   global struct kk_as_payload *payload = (global struct kk_as_payload *)
      ((global uchar *)h + h->payload_offset + id * (uint64_t)sizeof(*payload));
   n->child0_or_payload = h->payload_offset + id * (uint64_t)sizeof(*payload);
   n->geometry_id = lo;
   n->primitive_id = primitive;
   n->flags = in.flags;

   if (in.type == KK_AS_NODE_TRIANGLE) {
      float3 bounds_min = (float3)(INFINITY), bounds_max = (float3)(-INFINITY);
      for (uint v = 0; v < 3; ++v) {
         uint64_t vertex = primitive * 3 + v;
         if (in.index_size) {
            global const uchar *indices = (global const uchar *)(uintptr_t)
               (in.indices + in.primitive_offset + vertex * in.index_size);
            vertex = in.index_size == 4 ? *(global const uint *)indices :
                     in.index_size == 2 ? *(global const ushort *)indices : *indices;
         }
         vertex += in.first_vertex;
         uint64_t address = in.data + vertex * in.stride +
                            (in.index_size ? 0 : in.primitive_offset);
         float3 p = kk_as_vertex((global const uchar *)(uintptr_t)address, in.format);
         /* A NaN x component in any vertex makes the triangle inactive. */
         if (isnan(p.x))
            return;
         if (in.transform)
            p = kk_as_transform((global const float *)(uintptr_t)in.transform, p);
         vstore3(p, v, payload->data);
         bounds_min = fmin(bounds_min, p);
         bounds_max = fmax(bounds_max, p);
      }
      vstore3(bounds_min, 0, n->min);
      vstore3(bounds_max, 0, n->max);
      n->type = KK_AS_NODE_TRIANGLE;
      /* Opacity micromap reference: the micromap triangle index (base added
       * to non-negative indices; -1..-4 are special indices) and the
       * micromap's canonical address. Index size 1 means no index buffer:
       * the triangle's own index is used. */
      if (in.micromap_index_size) {
         int index = (int)primitive;
         if (in.micromap_index_size > 1) {
            global const uchar *entry = (global const uchar *)(uintptr_t)
               (in.micromap_indices + primitive * in.micromap_index_stride);
            index = in.micromap_index_size == 4 ? *(global const int *)entry :
                                                  (int)*(global const short *)entry;
         }
         if (index >= 0)
            index += (int)in.micromap_base;
         if (index >= -4 && (index < 0 || in.micromap)) {
            ((global uint *)payload->data)[9] = (uint)index;
            *(global uint64_t *)(payload->data + 10) = in.micromap;
            n->flags |= KK_AS_NODE_FLAG_OPACITY_MICROMAP;
         }
      }
   } else if (in.type == KK_AS_NODE_AABB) {
      global const float *aabb = (global const float *)(uintptr_t)
         (in.data + in.primitive_offset + primitive * in.stride);
      if (isnan(aabb[0]))
         return;
      vstore3(vload3(0, aabb), 0, n->min);
      vstore3(vload3(1, aabb), 0, n->max);
      n->type = KK_AS_NODE_AABB;
   } else {
      uint64_t address = in.data + in.primitive_offset + primitive * (in.array_of_pointers ? ((uint64_t)8) : ((uint64_t)64));
      if (in.array_of_pointers)
         address = *(global const uint64_t *)(uintptr_t)address;
      if (!address)
         return;
      global const struct kk_as_instance_record *instance =
         (global const struct kk_as_instance_record *)(uintptr_t)address;
      uint64_t blas = instance->reference;
      n->instance_id = primitive;
      n->custom_index_and_mask = instance->custom_index_and_mask;
      n->sbt_offset_and_flags = instance->sbt_offset_and_flags;
      n->child1_or_blas = blas;
      /* Preserve every input pointer for serialization, including null and
       * masked instances. A null address is never dereferenced. */
      if (!blas || !(n->custom_index_and_mask >> 24))
         return;
      global struct kk_as_header *child = (global struct kk_as_header *)(uintptr_t)blas;
      if (!child->root_offset)
         return;
      for (uint j = 0; j < 12; ++j)
         payload->data[j] = instance->transform[j];
      kk_as_inverse(payload->data + 12, payload->data);
      global struct kk_as_node *root = kk_as_node_ref(child, child->root_offset);
      float3 bounds_min = (float3)(INFINITY), bounds_max = (float3)(-INFINITY);
      for (uint corner = 0; corner < 8; ++corner) {
         float3 v = (float3)(corner & 1 ? root->max[0] : root->min[0],
                             corner & 2 ? root->max[1] : root->min[1],
                             corner & 4 ? root->max[2] : root->min[2]);
         v = kk_as_transform(payload->data, v);
         bounds_min = fmin(bounds_min, v);
         bounds_max = fmax(bounds_max, v);
      }
      vstore3(bounds_min, 0, n->min);
      vstore3(bounds_max, 0, n->max);
      n->type = KK_AS_NODE_INSTANCE;
   }
}

/* One invocation packs the micromap triangle records: each triangle's states
 * take the next free range of the packed data, in triangle order. A record
 * outside the sizes given at build-size time keeps no states. */
KERNEL(1)
kk_as_micromap_records(global struct kk_as_header *dst,
                       constant struct kk_as_header *layout,
                       global const struct kk_as_input *input)
{
   *dst = *layout;
   global struct kk_as_micromap_triangle *records = (global struct kk_as_micromap_triangle *)
      ((global uchar *)dst + layout->payload_offset);
   uint64_t capacity = layout->canonical_size - layout->geometry_offset, cursor = 0;
   for (uint64_t i = 0; i < layout->primitive_count; ++i) {
      global const uchar *src = (global const uchar *)(uintptr_t)
         (input->indices + i * input->stride);
      uint level = *(global const ushort *)(src + 4);
      uint format = *(global const ushort *)(src + 6);
      uint64_t bytes = level <= KK_AS_MAX_MICROMAP_LEVEL && (format == 1 || format == 2) ?
         kk_as_micromap_triangle_bytes(level, format) : 0;
      if (!bytes || bytes > capacity - cursor) {
         records[i] = (struct kk_as_micromap_triangle){(uint)cursor, 0, 0};
         continue;
      }
      records[i] = (struct kk_as_micromap_triangle){(uint)cursor, (ushort)level, (ushort)format};
      cursor += bytes;
   }
}

/* One invocation per micromap triangle copies its states from the
 * application's data offset to the packed range. */
KERNEL(64)
kk_as_micromap_copy(global struct kk_as_header *dst,
                    global const struct kk_as_input *input)
{
   uint64_t id = kk_as_id();
   if (id >= dst->primitive_count)
      return;
   global const struct kk_as_micromap_triangle *record =
      (global const struct kk_as_micromap_triangle *)((global uchar *)dst + dst->payload_offset) + id;
   if (!record->format)
      return;
   global const uchar *triangle = (global const uchar *)(uintptr_t)
      (input->indices + id * input->stride);
   global const uchar *src = (global const uchar *)(uintptr_t)
      (input->data + *(global const uint *)triangle);
   global uchar *out = (global uchar *)dst + dst->geometry_offset + record->data_offset;
   uint64_t bytes = kk_as_micromap_triangle_bytes(record->subdivision_level, record->format);
   for (uint64_t i = 0; i < bytes; ++i)
      out[i] = src[i];
}

static inline int
kk_as_lcp(long i, long j, uint64_t count)
{
   if (j < 0 || (uint64_t)j >= count)
      return -1;
   return clz((uint)i ^ (uint)j);
}

KERNEL(64)
kk_as_radix_tree(global struct kk_as_header *h)
{
   uint64_t count = h->primitive_count, index = kk_as_id();
   if (count <= 1 || index >= count - 1)
      return;
   long id = index;
   int left_lcp = kk_as_lcp(id, id - 1, count);
   int right_lcp = kk_as_lcp(id, id + 1, count);
   long dir = right_lcp > left_lcp ? 1 : -1;
   int lcp_min = min(left_lcp, right_lcp);
   long lmax = 2;
   while (kk_as_lcp(id, id + dir * lmax, count) > lcp_min)
      lmax *= 2;
   long length = 0;
   for (long t = lmax / 2; t >= 1; t /= 2) {
      if (kk_as_lcp(id, id + (length + t) * dir, count) > lcp_min)
         length += t;
   }
   long other_end = id + length * dir;
   int lcp_node = kk_as_lcp(id, other_end, count);
   long child_range = 0;
   for (long diff = 2; diff < 2 * length; diff *= 2) {
      long t = length / diff + (length % diff != 0);
      if (kk_as_lcp(id, id + (child_range + t) * dir, count) > lcp_node)
         child_range += t;
   }
   long left = id + child_range * dir + min(dir, 0l), right = left + 1;
   uint64_t left_id = min(id, other_end) == left ? count - 1 + left : left;
   uint64_t right_id = max(id, other_end) == right ? count - 1 + right : right;
   global struct kk_as_node *node = kk_as_node_at(h, index);
   node->type = KK_AS_NODE_INTERNAL;
   node->child0_or_payload = kk_as_node_offset(left_id);
   node->child1_or_blas = kk_as_node_offset(right_id);
   node->prefix_length = lcp_node;
   kk_as_node_at(h, left_id)->parent = kk_as_node_offset(index);
   kk_as_node_at(h, right_id)->parent = kk_as_node_offset(index);
}

/* Separate dispatches are barriers between prefix levels. This uses no
 * inter-workgroup spinlock and no cross-workgroup publication assumptions. */
KERNEL(64)
kk_as_reduce_bounds(global struct kk_as_header *h, uint level)
{
   uint64_t id = kk_as_id();
   if (h->primitive_count <= 1 || id >= h->primitive_count - 1)
      return;
   global struct kk_as_node *node = kk_as_node_at(h, id);
   if (node->prefix_length != level)
      return;
   global struct kk_as_node *left = kk_as_node_ref(h, node->child0_or_payload);
   global struct kk_as_node *right = kk_as_node_ref(h, node->child1_or_blas);
   for (uint axis = 0; axis < 3; ++axis) {
      node->min[axis] = fmin(left->min[axis], right->min[axis]);
      node->max[axis] = fmax(left->max[axis], right->max[axis]);
   }
}

KERNEL(64)
kk_as_thread_successors(global struct kk_as_header *h)
{
   uint64_t id = kk_as_id();
   if (id >= h->node_count)
      return;
   global uint64_t *words = (global uint64_t *)h;
   uint64_t own = kk_as_node_offset(id), cursor = own, next = 0;
   uint64_t parent = words[(cursor + offsetof(struct kk_as_node, parent)) / 8];
   while (parent) {
      if (words[(parent + offsetof(struct kk_as_node, child0_or_payload)) / 8] == cursor) {
         next = words[(parent + offsetof(struct kk_as_node, child1_or_blas)) / 8];
         break;
      }
      cursor = parent;
      parent = words[(parent + offsetof(struct kk_as_node, parent)) / 8];
   }
   words[(own + offsetof(struct kk_as_node, escape)) / 8] = next;
}

KERNEL(64)
kk_as_copy(global uchar *dst, global const struct kk_as_header *src)
{
   uint64_t id = kk_as_id(), size = src->canonical_size;
   if (id < size / 8)
      ((global uint64_t *)dst)[id] = ((global const uint64_t *)src)[id];
}

static inline uint64_t
kk_as_serial_align(uint64_t references)
{
   return (KK_AS_SERIAL_PREFIX_SIZE + references + ((uint64_t)15)) & ~((uint64_t)15);
}

/* Bytes between the prefix and the canonical copy in a serialized AS: one
 * pointer per instance for a top level; for a bottom level the block list
 * that the marker announces, each block 16 bytes plus its handles. */
static inline uint64_t
kk_as_serial_references(global const uchar *src)
{
   global const struct kk_as_serial_header *prefix =
      (global const struct kk_as_serial_header *)src;
   uint64_t count = prefix->bottom_level_pointer_count;
   if ((count >> 32) != 0xffffffffu)
      return count * ((uint64_t)8);
   uint64_t bytes = 0;
   for (uint32_t block = 0; block < (uint32_t)count; ++block) {
      uint64_t handles = *(global const uint64_t *)
         (src + KK_AS_SERIAL_PREFIX_SIZE + bytes + 8);
      bytes += KK_AS_SERIAL_BLOCK_SIZE + handles * ((uint64_t)8);
   }
   return bytes;
}

static inline uint64_t
kk_as_serial_payload(global const uchar *src)
{
   return kk_as_serial_align(kk_as_serial_references(src));
}

static inline global struct kk_as_geometry *
kk_as_geometries(global struct kk_as_header *h)
{
   return (global struct kk_as_geometry *)((global uchar *)h + h->geometry_offset);
}

/* The opacity micromap handles of a serialized bottom level (its only
 * block), in the order of the geometries that reference a micromap. */
#define KK_AS_SERIAL_MICROMAP_HANDLES (KK_AS_SERIAL_PREFIX_SIZE + KK_AS_SERIAL_BLOCK_SIZE)

KERNEL(64)
kk_as_serialize(global uchar *dst, global struct kk_as_header *src,
                constant uchar *driver_uuid, constant uchar *format_uuid)
{
   uint64_t id = kk_as_id();
   global struct kk_as_serial_header *prefix = (global struct kk_as_serial_header *)dst;
   if (id == 0) {
      for (uint i = 0; i < 16; ++i) {
         prefix->driver_uuid[i] = driver_uuid[i];
         prefix->format_uuid[i] = format_uuid[i];
      }
      prefix->serialization_size = src->serialization_size;
      prefix->deserialization_size = src->canonical_size;
      prefix->bottom_level_pointer_count = src->type == KK_AS_TYPE_BOTTOM_LEVEL ?
         KK_AS_SERIAL_BLAS_BLOCKS | (src->micromap_count ? 1u : 0u) :
         src->bottom_level_pointer_count;
      if (src->type == KK_AS_TYPE_BOTTOM_LEVEL && src->micromap_count) {
         global uint *block = (global uint *)(dst + KK_AS_SERIAL_PREFIX_SIZE);
         block[0] = KK_AS_SERIAL_BLOCK_OPACITY_MICROMAP;
         block[1] = 0;
         *(global uint64_t *)(block + 2) = src->micromap_count;
         global uint64_t *handles = (global uint64_t *)(dst + KK_AS_SERIAL_MICROMAP_HANDLES);
         global struct kk_as_geometry *g = kk_as_geometries(src);
         uint32_t k = 0;
         for (uint32_t i = 0; i < src->geometry_count && k < src->micromap_count; ++i) {
            if (g[i].micromap)
               handles[k++] = g[i].micromap;
         }
      }
   }
   if (id < src->instance_count) {
      global struct kk_as_node *instance = kk_as_node_ref(src,
         src->instance_offset + id * (uint64_t)sizeof(struct kk_as_node));
      ((global uint64_t *)(dst + KK_AS_SERIAL_PREFIX_SIZE))[id] = instance->child1_or_blas;
   }
   uint64_t references = src->type == KK_AS_TYPE_BOTTOM_LEVEL ?
      (src->micromap_count ? KK_AS_SERIAL_BLOCK_SIZE + src->micromap_count * ((uint64_t)8) : 0) :
      src->bottom_level_pointer_count * ((uint64_t)8);
   uint64_t offset = kk_as_serial_align(references);
   if (id < src->canonical_size / 8)
      ((global uint64_t *)(dst + offset))[id] = ((global const uint64_t *)src)[id];
   /* Deterministic padding contains no process addresses or uninitialized data. */
   uint64_t padding = KK_AS_SERIAL_PREFIX_SIZE + references;
   if (id == 0 && offset != padding)
      *(global uint64_t *)(dst + padding) = 0;
}

KERNEL(64)
kk_as_deserialize(global struct kk_as_header *dst, global const uchar *src)
{
   uint64_t id = kk_as_id();
   uint64_t offset = kk_as_serial_payload(src);
   global const struct kk_as_header *h = (global const struct kk_as_header *)(src + offset);
   /* UUID compatibility is queried by the application before a legal command.
    * The embedded ABI is nevertheless checked before using its layout. */
   if (h->magic != KK_AS_MAGIC || h->abi_version != KK_AS_ABI_VERSION)
      return;
   if (id < h->canonical_size / 8)
      ((global uint64_t *)dst)[id] = ((global const uint64_t *)h)[id];
}

/* Must follow the payload copy in a separate dispatch, so copying a leaf cannot
 * race with application-patched BLAS pointers being installed in that leaf. */
KERNEL(64)
kk_as_patch_deserialized_instances(global struct kk_as_header *dst,
                                   global const uchar *src)
{
   uint64_t id = kk_as_id();
   if (id < dst->instance_count) {
      global struct kk_as_node *node = kk_as_node_ref(dst,
         dst->instance_offset + id * (uint64_t)sizeof(struct kk_as_node));
      node->child1_or_blas = ((global const uint64_t *)(src + KK_AS_SERIAL_PREFIX_SIZE))[id];
   }
   /* A bottom level's leaves take the application-patched micromap handle of
    * their geometry: the k-th handle belongs to the k-th geometry with a
    * micromap. The geometry records still hold the serialized addresses here
    * and are updated by the next dispatch. */
   if (dst->type == KK_AS_TYPE_BOTTOM_LEVEL && dst->micromap_count && id < dst->node_count) {
      global struct kk_as_node *node = kk_as_node_at(dst, id);
      if (node->type != KK_AS_NODE_TRIANGLE || !(node->flags & KK_AS_NODE_FLAG_OPACITY_MICROMAP))
         return;
      global struct kk_as_geometry *g = kk_as_geometries(dst);
      uint32_t geometry = node->geometry_id, k = 0;
      if (geometry >= dst->geometry_count || !g[geometry].micromap)
         return;
      for (uint32_t i = 0; i < geometry; ++i)
         k += g[i].micromap != 0;
      global uchar *payload = (global uchar *)dst + node->child0_or_payload;
      *(global uint64_t *)(payload + 10 * sizeof(float)) =
         k < dst->micromap_count ?
         ((global const uint64_t *)(src + KK_AS_SERIAL_MICROMAP_HANDLES))[k] : 0;
   }
}

/* Follows kk_as_patch_deserialized_instances: the geometry records take the
 * new micromap handles, so a later serialization lists them. */
KERNEL(1)
kk_as_patch_deserialized_micromaps(global struct kk_as_header *dst,
                                   global const uchar *src)
{
   if (dst->type != KK_AS_TYPE_BOTTOM_LEVEL || !dst->micromap_count)
      return;
   global struct kk_as_geometry *g = kk_as_geometries(dst);
   global const uint64_t *handles =
      (global const uint64_t *)(src + KK_AS_SERIAL_MICROMAP_HANDLES);
   uint32_t k = 0;
   for (uint32_t i = 0; i < dst->geometry_count && k < dst->micromap_count; ++i) {
      if (g[i].micromap)
         g[i].micromap = handles[k++];
   }
}

KERNEL(1)
kk_as_write_property(global const struct kk_as_header *src, global uint64_t *dst, uint query)
{
   /* Compacted-size, serialization-size, bottom-level-count, canonical-size. */
   *dst = query == 1 ? src->serialization_size :
          query == 2 ? src->bottom_level_pointer_count : src->canonical_size;
}

/* Metal's existing indirect-local grid schema: threads[3], group[3]. */
KERNEL(1)
kk_as_prepare_transfer(global uint *grid, global const uchar *src, uint serialized)
{
   global const struct kk_as_header *h = (global const struct kk_as_header *)src;
   if (serialized)
      h = (global const struct kk_as_header *)(src + kk_as_serial_payload(src));
   uint64_t count = h->canonical_size / 8;
   uint width = count < 65536 ? (uint)count : 65536;
   uint64_t rows = width ? count / width + (count % width != 0) : 1;
   uint height = (uint)min(rows, (uint64_t)65535);
   uint64_t plane = (uint64_t)width * height;
   uint depth = plane ? (uint)(count / plane + (count % plane != 0)) : 1;
   grid[0] = width;
   grid[1] = height;
   grid[2] = depth;
   grid[3] = 64;
   grid[4] = 1;
   grid[5] = 1;
}

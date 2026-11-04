/* SPDX-License-Identifier: MIT */
#ifndef KK_AS_TYPES_H
#define KK_AS_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* This ABI contains addresses only where Vulkan itself supplies device addresses.
 * All references within an AS are relative to its first byte. */
#define KK_AS_MAGIC 0x53414b4bu
#define KK_AS_ABI_VERSION 1u
#define KK_AS_MAX_PRIMITIVES ((((uint64_t)1) << 29) - 1)
#define KK_AS_MAX_GEOMETRIES ((((uint64_t)1) << 24) - 1)
#define KK_AS_MAX_INSTANCES ((((uint64_t)1) << 24) - 1)
#define KK_AS_ALIGNMENT ((uint64_t)16)
#define KK_AS_SCRATCH_ALIGNMENT ((uint64_t)16)
#define KK_AS_SERIAL_PREFIX_SIZE ((uint64_t)56)
#define KK_AS_SERIAL_BLAS_BLOCKS ((uint64_t)0xffffffffu << 32)
/* A serialized block: type (VK_ACCELERATION_STRUCTURE_SERIALIZED_BLOCK_TYPE_*),
 * a reserved word and a 64-bit handle count, then the handles. */
#define KK_AS_SERIAL_BLOCK_SIZE ((uint64_t)16)
#define KK_AS_SERIAL_BLOCK_OPACITY_MICROMAP 0u
#define KK_AS_MAX_DISPATCH_INVOCATIONS (UINT64_C(65536) * 65535 * UINT32_MAX)

/* Bottom-level builds sort their leaves along a Morton curve. The sort keys,
 * a leaf staging copy and the centroid bounds follow the canonical
 * representation in scratch. */
static inline uint64_t
kk_as_sort_size(uint64_t primitives)
{
   uint64_t size = 1;
   while (size < primitives)
      size <<= 1;
   return size;
}

#define KK_AS_SORT_BOUNDS_SIZE ((uint64_t)32)

#define KK_AS_TYPE_TOP_LEVEL 0u
#define KK_AS_TYPE_BOTTOM_LEVEL 1u
#define KK_AS_TYPE_OPACITY_MICROMAP 2u
#define KK_AS_NODE_INTERNAL 0u
#define KK_AS_NODE_TRIANGLE 1u
#define KK_AS_NODE_AABB 2u
#define KK_AS_NODE_INSTANCE 3u
#define KK_AS_NODE_INACTIVE 4u
#define KK_AS_STATUS_COMPLETE 1u
/* Leaf flag beside the geometry flags: the triangle payload holds an opacity
 * micromap reference (payload floats 9..11). */
#define KK_AS_NODE_FLAG_OPACITY_MICROMAP (1u << 16)
#define KK_AS_MAX_MICROMAP_LEVEL 12u

/* Explicit scalar arrays avoid host/OpenCL float3 alignment differences. */
struct __attribute__((aligned(16))) kk_as_header {
   uint32_t magic;
   uint16_t abi_version;
   uint8_t type;
   uint8_t status;
   uint32_t build_flags;
   /* Bottom level: geometries with an attached opacity micromap. */
   uint32_t micromap_count;
   uint64_t canonical_size;
   uint64_t serialization_size;
   uint64_t root_offset;
   uint64_t geometry_offset;
   uint64_t instance_offset;
   uint64_t payload_offset;
   uint64_t node_count;
   uint64_t primitive_count;
   uint32_t geometry_count;
   uint32_t instance_count;
   uint64_t bottom_level_pointer_count;
   uint64_t generation;
   uint64_t reserved_native_resource_id;
   uint64_t reserved_native_generation;
   uint64_t reserved1;
};

struct __attribute__((aligned(16))) kk_as_node {
   float min[3];
   uint32_t type;
   float max[3];
   uint32_t flags;
   uint64_t child0_or_payload;
   uint64_t child1_or_blas;
   uint64_t escape;
   uint64_t parent;
   uint32_t geometry_id;
   uint32_t primitive_id;
   uint32_t instance_id;
   uint32_t custom_index_and_mask;
   uint32_t sbt_offset_and_flags;
   uint32_t prefix_length;
   uint64_t reserved;
};

/* micromap: the attached opacity micromap's address (0 for none), which
 * serialization lists as the BLAS's micromap handle. */
struct kk_as_geometry {
   uint64_t first_primitive;
   uint64_t primitive_count;
   uint32_t type;
   uint32_t flags;
   uint64_t micromap;
};

/* Triangles use the first nine floats; instances use both row-major 3x4
 * transforms. An AABB's authoritative converted bounds are in its leaf. */
struct __attribute__((aligned(16))) kk_as_payload {
   float data[24];
};

/* A canonical opacity micromap: the header, then one record per micromap
 * triangle at payload_offset, then the packed state bytes at geometry_offset.
 * data_offset is relative to the packed bytes. */
struct kk_as_micromap_triangle {
   uint32_t data_offset;
   uint16_t subdivision_level;
   uint16_t format;
};

/* Bytes of one micromap triangle's states: 4^level states of 1 bit
 * (VK_OPACITY_MICROMAP_FORMAT_2_STATE_KHR = 1) or 2 bits (4-state = 2). */
static inline uint64_t
kk_as_micromap_triangle_bytes(uint32_t level, uint32_t format)
{
   return ((((uint64_t)1) << (2 * level)) * (format == 2 ? 2 : 1) + 7) / 8;
}

struct kk_as_serial_header {
   uint8_t driver_uuid[16];
   uint8_t format_uuid[16];
   uint64_t serialization_size;
   uint64_t deserialization_size;
   uint64_t bottom_level_pointer_count;
};

/* Immutable API parameters, uploaded for a build. These are not persisted in an
 * AS: converted vertices/transforms are snapshotted into canonical payloads. */
struct kk_as_input {
   uint64_t first_primitive;
   uint64_t primitive_count;
   uint64_t data;
   uint64_t indices;
   uint64_t transform;
   uint64_t stride;
   uint64_t primitive_offset;
   uint32_t first_vertex;
   uint32_t type;
   uint32_t format;
   uint32_t index_size;
   uint32_t flags;
   uint32_t array_of_pointers;
   /* Triangles: the attached opacity micromap (canonical address or 0), its
    * index buffer, stride and index size (0 for none) and base triangle. A
    * micromap build uses data, indices (triangle array) and stride. */
   uint64_t micromap;
   uint64_t micromap_indices;
   uint64_t micromap_index_stride;
   uint32_t micromap_index_size;
   uint32_t micromap_base;
};

/* GPU input ABI of VkAccelerationStructureInstanceKHR without C bitfields. */
struct kk_as_instance_record {
   float transform[12];
   uint32_t custom_index_and_mask;
   uint32_t sbt_offset_and_flags;
   uint64_t reference;
};
_Static_assert(sizeof(struct kk_as_instance_record) == 64, "Vulkan instance ABI");
_Static_assert(offsetof(struct kk_as_instance_record, custom_index_and_mask) == 48, "instance mask ABI");
_Static_assert(offsetof(struct kk_as_instance_record, sbt_offset_and_flags) == 52, "instance SBT ABI");
_Static_assert(offsetof(struct kk_as_instance_record, reference) == 56, "instance address ABI");

/* Dense converter IDs, independent of VkFormat numeric values. */
enum kk_as_vertex_format {
   KK_AS_R32G32_SFLOAT,
   KK_AS_R32G32B32_SFLOAT,
   KK_AS_R16G16_SFLOAT,
   KK_AS_R16G16_SNORM,
   KK_AS_R16G16B16A16_SFLOAT,
   KK_AS_R16G16B16A16_SNORM,
   KK_AS_R16G16B16A16_UNORM,
   KK_AS_R16G16_UNORM,
   KK_AS_A2B10G10R10_UNORM,
   KK_AS_R8G8B8A8_UNORM,
   KK_AS_R8G8_UNORM,
   KK_AS_R8G8B8A8_SNORM,
   KK_AS_R8G8_SNORM,
};

_Static_assert(__alignof__(struct kk_as_header) == 16, "AS base alignment ABI");
_Static_assert(sizeof(struct kk_as_header) == 128, "AS header ABI");
_Static_assert(offsetof(struct kk_as_header, canonical_size) == 16, "AS size ABI");
_Static_assert(offsetof(struct kk_as_header, root_offset) == 32, "AS root ABI");
_Static_assert(offsetof(struct kk_as_header, payload_offset) == 56, "AS payload ABI");
_Static_assert(offsetof(struct kk_as_header, node_count) == 64, "AS count ABI");
_Static_assert(offsetof(struct kk_as_header, geometry_count) == 80, "AS geometry ABI");
_Static_assert(offsetof(struct kk_as_header, bottom_level_pointer_count) == 88, "AS references ABI");
_Static_assert(offsetof(struct kk_as_header, generation) == 96, "AS generation ABI");
_Static_assert(offsetof(struct kk_as_header, reserved_native_resource_id) == 104, "AS reserved ABI");
_Static_assert(sizeof(struct kk_as_node) == 96, "AS node ABI");
_Static_assert(offsetof(struct kk_as_node, child0_or_payload) == 32, "AS child ABI");
_Static_assert(offsetof(struct kk_as_node, escape) == 48, "AS successor ABI");
_Static_assert(offsetof(struct kk_as_node, geometry_id) == 64, "AS primitive ABI");
_Static_assert(sizeof(struct kk_as_geometry) == 32, "AS geometry ABI");
_Static_assert(sizeof(struct kk_as_payload) == 96, "AS payload ABI");
_Static_assert(sizeof(struct kk_as_serial_header) == KK_AS_SERIAL_PREFIX_SIZE, "AS serialization ABI");
_Static_assert(sizeof(struct kk_as_input) == 112, "AS input ABI");
_Static_assert(sizeof(struct kk_as_micromap_triangle) == 8, "micromap triangle ABI");
#endif

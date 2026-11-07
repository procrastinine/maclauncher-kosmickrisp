/* SPDX-License-Identifier: MIT */
#include "kk_device.h"
#include "kk_query_pool.h"
#include "kk_bo.h"
#include "kk_cmd_buffer.h"
#include "kk_entrypoints.h"
#include "vk_acceleration_structure.h"
#include "vk_util.h"
#include "vk_alloc.h"
#include "vk_physical_device.h"
#include "vk_pipeline.h"
#include "kosmickrisp/as/kosmickrisp-as-layout.h"
#include "kosmickrisp/bridge/mtl_bridge.h"

/* The existing precompiler schema, with an independent program-set namespace. */
#define libkk_program kk_as_program
#define LIBKK_NUM_PROGRAMS KK_AS_NUM_PROGRAMS
#define libkk_AppleSilicon kk_as_AppleSilicon
#include "kosmickrisp/as/libkk_shaders.h"
#undef libkk_program
#undef LIBKK_NUM_PROGRAMS
#undef libkk_AppleSilicon

/* A real private vk_pipeline object, owned by the existing meta cache. No
 * additional device fields, allocator, lock or unused-device work is required. */
struct kk_as_meta_pipeline {
   struct vk_pipeline vk;
   struct kk_precompiled_shader shader;
};

static void
kk_as_meta_pipeline_destroy(struct vk_device *dev, struct vk_pipeline *base,
                             const VkAllocationCallbacks *alloc)
{
   struct kk_as_meta_pipeline *pipeline = container_of(base, struct kk_as_meta_pipeline, vk);
   mtl_release(pipeline->shader.pipeline);
   vk_pipeline_free(dev, alloc, base);
}

static const struct vk_pipeline_ops kk_as_meta_pipeline_ops = {
   .destroy = kk_as_meta_pipeline_destroy,
};

static const struct kk_precompiled_shader *
kk_as_get_program(struct kk_cmd_buffer *cmd, enum kk_as_program program)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   const struct {
      enum vk_meta_object_key_type type;
      char tag[8];
      uint32_t abi;
      uint32_t program;
   } key = {VK_META_OBJECT_KEY_DRIVER_OFFSET, "KKASMETA", KK_AS_ABI_VERSION, program};
   struct vk_pipeline *base = vk_pipeline_from_handle(
      vk_meta_lookup_pipeline(&dev->meta, &key, sizeof(key)));
   if (!base) {
      struct kk_as_meta_pipeline *pipeline = vk_pipeline_zalloc(&dev->vk,
         &kk_as_meta_pipeline_ops, VK_PIPELINE_BIND_POINT_COMPUTE, 0, NULL, sizeof(*pipeline));
      if (!pipeline) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
      pipeline->vk.stages = VK_SHADER_STAGE_COMPUTE_BIT;
      const struct kk_precompiled_info *info = (const void *)kk_as_AppleSilicon[program];
      uint32_t threads = info->workgroup_size[0] * info->workgroup_size[1] * info->workgroup_size[2];
      VkResult result = kk_build_precompiled_shader(dev, kk_as_AppleSilicon[program], threads, &pipeline->shader);
      if (result != VK_SUCCESS) {
         vk_pipeline_free(&dev->vk, NULL, &pipeline->vk);
         vk_command_buffer_set_error(&cmd->vk, result);
         return NULL;
      }
      VkPipeline handle = vk_pipeline_to_handle(&pipeline->vk);
      handle = (VkPipeline)vk_meta_cache_object(&dev->vk, &dev->meta, &key, sizeof(key),
                                                VK_OBJECT_TYPE_PIPELINE, (uint64_t)handle);
      base = vk_pipeline_from_handle(handle);
   }
   return &container_of(base, struct kk_as_meta_pipeline, vk)->shader;
}

static bool
kk_as_dispatch(struct kk_cmd_buffer *cmd, struct kk_grid grid,
                enum kk_as_program program, void *args, size_t size)
{
   const struct kk_precompiled_shader *shader = kk_as_get_program(cmd, program);
   if (!shader)
      return false;
   kk_dispatch_precompiled_shader(cmd, grid, false, shader, args, size);
   return true;
}

static struct kk_grid
kk_as_direct_grid(uint64_t count)
{
   uint32_t grid[3];
   bool fits = kk_as_grid(count, grid);
   assert(fits);
   return kk_grid_3d(grid[0], grid[1], grid[2]);
}

static bool
kk_as_info_layout(const VkAccelerationStructureBuildGeometryInfoKHR *info,
                   const uint32_t *max_counts,
                   const VkAccelerationStructureBuildRangeInfoKHR *ranges,
                   struct kk_as_layout *layout)
{
   uint64_t primitives = 0;
   for (uint32_t g = 0; g < info->geometryCount; ++g) {
      if (!kk_as_u64_add(primitives, ranges ? ranges[g].primitiveCount : max_counts[g], &primitives))
         return false;
   }
   return kk_as_compute_layout(info->type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR ?
                                 KK_AS_TYPE_TOP_LEVEL : KK_AS_TYPE_BOTTOM_LEVEL,
                                info->flags, info->geometryCount, primitives, layout);
}

VKAPI_ATTR void VKAPI_CALL
kk_GetAccelerationStructureBuildSizesKHR(VkDevice device,
   VkAccelerationStructureBuildTypeKHR buildType,
   const VkAccelerationStructureBuildGeometryInfoKHR *pBuildInfo,
   const uint32_t *pMaxPrimitiveCounts,
   VkAccelerationStructureBuildSizesInfoKHR *pSizeInfo)
{
   struct kk_as_layout layout;
   if (!kk_as_info_layout(pBuildInfo, pMaxPrimitiveCounts, NULL, &layout)) {
      /* Outside the exposed count limits. Never wrap into a small allocation. */
      pSizeInfo->accelerationStructureSize = UINT64_MAX;
      pSizeInfo->buildScratchSize = UINT64_MAX;
      pSizeInfo->updateScratchSize = UINT64_MAX;
      return;
   }
   pSizeInfo->accelerationStructureSize = layout.header.canonical_size;
   pSizeInfo->buildScratchSize = layout.scratch_size;
   pSizeInfo->updateScratchSize = layout.scratch_size;
}

static uint32_t
kk_as_convert_format(VkFormat format)
{
   switch (format) {
#define KK_AS_FORMAT(vk, as) case VK_FORMAT_##vk: return KK_AS_##as
   KK_AS_FORMAT(R32G32_SFLOAT, R32G32_SFLOAT);
   KK_AS_FORMAT(R32G32B32_SFLOAT, R32G32B32_SFLOAT);
   KK_AS_FORMAT(R16G16_SFLOAT, R16G16_SFLOAT);
   KK_AS_FORMAT(R16G16_SNORM, R16G16_SNORM);
   KK_AS_FORMAT(R16G16B16A16_SFLOAT, R16G16B16A16_SFLOAT);
   KK_AS_FORMAT(R16G16B16A16_SNORM, R16G16B16A16_SNORM);
   KK_AS_FORMAT(R16G16B16A16_UNORM, R16G16B16A16_UNORM);
   KK_AS_FORMAT(R16G16_UNORM, R16G16_UNORM);
   KK_AS_FORMAT(A2B10G10R10_UNORM_PACK32, A2B10G10R10_UNORM);
   KK_AS_FORMAT(R8G8B8A8_UNORM, R8G8B8A8_UNORM);
   KK_AS_FORMAT(R8G8_UNORM, R8G8_UNORM);
   KK_AS_FORMAT(R8G8B8A8_SNORM, R8G8B8A8_SNORM);
   KK_AS_FORMAT(R8G8_SNORM, R8G8_SNORM);
#undef KK_AS_FORMAT
   default: UNREACHABLE("unsupported AS vertex format");
   }
}

static bool
kk_as_make_input(const VkAccelerationStructureGeometryKHR *geometry,
                  const VkAccelerationStructureBuildRangeInfoKHR *range,
                  uint64_t first, struct kk_as_input *out)
{
   *out = (struct kk_as_input){.first_primitive = first,
      .primitive_count = range->primitiveCount, .primitive_offset = range->primitiveOffset,
      .flags = geometry->flags};
   switch (geometry->geometryType) {
   case VK_GEOMETRY_TYPE_TRIANGLES_KHR: {
      const VkAccelerationStructureGeometryTrianglesDataKHR *t = &geometry->geometry.triangles;
      out->type = KK_AS_NODE_TRIANGLE;
      out->data = t->vertexData.deviceAddress;
      out->indices = t->indexData.deviceAddress;
      out->stride = t->vertexStride;
      out->first_vertex = range->firstVertex;
      out->format = kk_as_convert_format(t->vertexFormat);
      out->index_size = t->indexType == VK_INDEX_TYPE_NONE_KHR ? 0 :
                        t->indexType == VK_INDEX_TYPE_UINT32 ? 4 :
                        t->indexType == VK_INDEX_TYPE_UINT16 ? 2 : 1;
      if (t->transformData.deviceAddress &&
          !kk_as_u64_add(t->transformData.deviceAddress, range->transformOffset, &out->transform))
         return false;
      break;
   }
   case VK_GEOMETRY_TYPE_AABBS_KHR:
      out->type = KK_AS_NODE_AABB;
      out->data = geometry->geometry.aabbs.data.deviceAddress;
      out->stride = geometry->geometry.aabbs.stride;
      break;
   case VK_GEOMETRY_TYPE_INSTANCES_KHR:
      out->type = KK_AS_NODE_INSTANCE;
      out->data = geometry->geometry.instances.data.deviceAddress;
      out->array_of_pointers = geometry->geometry.instances.arrayOfPointers;
      break;
   default: UNREACHABLE("unsupported AS geometry");
   }
   return true;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBuildAccelerationStructuresKHR(VkCommandBuffer commandBuffer, uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   for (uint32_t b = 0; b < infoCount; ++b) {
      const VkAccelerationStructureBuildGeometryInfoKHR *info = &pInfos[b];
      struct kk_as_layout layout;
      if (!kk_as_info_layout(info, NULL, ppBuildRangeInfos[b], &layout)) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         return;
      }
      VK_FROM_HANDLE(vk_acceleration_structure, dst, info->dstAccelerationStructure);
      uint64_t dst_address = vk_acceleration_structure_get_va(dst);
      if (!kk_as_range_fits(0, layout.header.canonical_size, dst->size) ||
          !kk_as_address_range_fits(dst_address, layout.header.canonical_size) ||
          !kk_as_address_range_fits(info->scratchData.deviceAddress, layout.scratch_size)) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         return;
      }
      uint64_t input_bytes;
      if (!kk_as_u64_mul(info->geometryCount, sizeof(struct kk_as_input), &input_bytes) ||
          input_bytes > UINT32_MAX) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return;
      }
      struct kk_ptr input = kk_pool_alloc(cmd, MAX2(input_bytes, 8), 8);
      struct kk_ptr header = kk_pool_upload(cmd, &layout.header, sizeof(layout.header), 16);
      if (!input.gpu || !header.gpu)
         return;
      uint64_t first = 0;
      for (uint32_t g = 0; g < info->geometryCount; ++g) {
         const VkAccelerationStructureGeometryKHR *geometry = info->pGeometries ?
            &info->pGeometries[g] : info->ppGeometries[g];
         if (!kk_as_make_input(geometry, &ppBuildRangeInfos[b][g], first,
                               &((struct kk_as_input *)input.cpu)[g])) {
            vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            return;
         }
         first += ppBuildRangeInfos[b][g].primitiveCount;
      }
      uint64_t scratch = info->scratchData.deviceAddress;
      struct kk_as_initialize_args init = {scratch, header.gpu, input.gpu};
      kk_as_dispatch(cmd, kk_as_direct_grid(MAX3(layout.header.node_count, info->geometryCount, 1)),
                       KK_AS_INITIALIZE, &init, sizeof(init));
      if (first) {
         struct kk_as_convert_leaves_args leaves = {scratch, input.gpu};
         kk_as_dispatch(cmd, kk_as_direct_grid(first), KK_AS_CONVERT_LEAVES, &leaves, sizeof(leaves));
         struct kk_as_radix_tree_args tree = {scratch};
         kk_as_dispatch(cmd, kk_as_direct_grid(first), KK_AS_RADIX_TREE, &tree, sizeof(tree));
         for (int level = 31; level >= 0; --level) {
            struct kk_as_reduce_bounds_args bounds = {scratch, level};
            kk_as_dispatch(cmd, kk_as_direct_grid(first), KK_AS_REDUCE_BOUNDS, &bounds, sizeof(bounds));
         }
         struct kk_as_thread_successors_args successors = {scratch};
         kk_as_dispatch(cmd, kk_as_direct_grid(layout.header.node_count), KK_AS_THREAD_SUCCESSORS,
                          &successors, sizeof(successors));
      }
      struct kk_as_copy_args copy = {dst_address, scratch};
      kk_as_dispatch(cmd, kk_as_direct_grid(layout.header.canonical_size / 8), KK_AS_COPY, &copy, sizeof(copy));
   }
}

static struct kk_grid
kk_as_transfer_grid(struct kk_cmd_buffer *cmd, uint64_t source, bool serialized)
{
   struct kk_ptr indirect = kk_pool_alloc(cmd, 24, 8);
   if (!indirect.gpu)
      return kk_grid_1d(0);
   struct kk_as_prepare_transfer_args args = {indirect.gpu, source, serialized};
   kk_as_dispatch(cmd, kk_grid_1d(1), KK_AS_PREPARE_TRANSFER, &args, sizeof(args));
   return kk_grid_indirect_local(indirect.gpu);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdCopyAccelerationStructureKHR(VkCommandBuffer commandBuffer,
   const VkCopyAccelerationStructureInfoKHR *pInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(vk_acceleration_structure, src, pInfo->src);
   VK_FROM_HANDLE(vk_acceleration_structure, dst, pInfo->dst);
   uint64_t source = vk_acceleration_structure_get_va(src);
   struct kk_grid grid = kk_as_transfer_grid(cmd, source, false);
   struct kk_as_copy_args args = {vk_acceleration_structure_get_va(dst), source};
   kk_as_dispatch(cmd, grid, KK_AS_COPY, &args, sizeof(args));
}

/* Stable format identity. The other UUID is the actual logical device's driver
 * identity, so compatibility cannot accidentally cross a different build. */
static const uint8_t kk_as_format_uuid[16] = {
   0x4b, 0x4b, 0x41, 0x53, 0x01, 0x00, 0x80, 0x00,
   0x60, 0x00, 0x60, 0x00, 0xa7, 0x1d, 0xf0, 0x65,
};

VKAPI_ATTR void VKAPI_CALL
kk_GetDeviceAccelerationStructureCompatibilityKHR(VkDevice device,
   const VkAccelerationStructureVersionInfoKHR *pVersionInfo,
   VkAccelerationStructureCompatibilityKHR *pCompatibility)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   bool compatible =
      memcmp(pVersionInfo->pVersionData, dev->vk.physical->properties.driverUUID, 16) == 0 &&
      memcmp(pVersionInfo->pVersionData + 16, kk_as_format_uuid, 16) == 0;
   *pCompatibility = compatible ? VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR :
                                 VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdCopyAccelerationStructureToMemoryKHR(VkCommandBuffer commandBuffer,
   const VkCopyAccelerationStructureToMemoryInfoKHR *pInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(vk_acceleration_structure, src, pInfo->src);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   uint64_t source = vk_acceleration_structure_get_va(src);
   struct kk_grid grid = kk_as_transfer_grid(cmd, source, false);
   struct kk_ptr driver = kk_pool_upload(cmd, dev->vk.physical->properties.driverUUID, 16, 8);
   struct kk_ptr format = kk_pool_upload(cmd, kk_as_format_uuid, 16, 8);
   if (!driver.gpu || !format.gpu)
      return;
   struct kk_as_serialize_args args = {pInfo->dst.deviceAddress, source, driver.gpu, format.gpu};
   kk_as_dispatch(cmd, grid, KK_AS_SERIALIZE, &args, sizeof(args));
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdCopyMemoryToAccelerationStructureKHR(VkCommandBuffer commandBuffer,
   const VkCopyMemoryToAccelerationStructureInfoKHR *pInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(vk_acceleration_structure, dst, pInfo->dst);
   uint64_t destination = vk_acceleration_structure_get_va(dst);
   struct kk_grid grid = kk_as_transfer_grid(cmd, pInfo->src.deviceAddress, true);
   struct kk_as_deserialize_args copy = {destination, pInfo->src.deviceAddress};
   kk_as_dispatch(cmd, grid, KK_AS_DESERIALIZE, &copy, sizeof(copy));
   struct kk_as_patch_deserialized_instances_args patch = {destination, pInfo->src.deviceAddress};
   kk_as_dispatch(cmd, grid, KK_AS_PATCH_DESERIALIZED_INSTANCES, &patch, sizeof(patch));
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdWriteAccelerationStructuresPropertiesKHR(VkCommandBuffer commandBuffer,
   uint32_t accelerationStructureCount, const VkAccelerationStructureKHR *pAccelerationStructures,
   VkQueryType queryType, VkQueryPool queryPool, uint32_t firstQuery)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(kk_query_pool, pool, queryPool);
   uint32_t property;
   switch (queryType) {
   case VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR: property = 0; break;
   case VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR: property = 1; break;
   case VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_BOTTOM_LEVEL_POINTERS_KHR: property = 2; break;
   case VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SIZE_KHR: property = 3; break;
   default: UNREACHABLE("unsupported AS property query");
   }
   for (uint32_t i = 0; i < accelerationStructureCount; ++i) {
      VK_FROM_HANDLE(vk_acceleration_structure, source, pAccelerationStructures[i]);
      uint32_t query = firstQuery + i;
      struct kk_as_write_property_args args = {
         vk_acceleration_structure_get_va(source),
         pool->bo->gpu + pool->query_start + (uint64_t)query * pool->query_stride,
         property,
      };
      if (!kk_as_dispatch(cmd, kk_grid_1d(1), KK_AS_WRITE_PROPERTY, &args, sizeof(args)))
         return;
      kk_cmd_write(cmd, (struct libkk_imm_write){
         pool->bo->gpu + (uint64_t)query * sizeof(uint32_t), 1,
      });
   }
}

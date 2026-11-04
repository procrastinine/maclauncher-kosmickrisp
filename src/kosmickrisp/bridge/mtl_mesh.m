/* SPDX-License-Identifier: MIT */
#include "mtl_compiler.h"
#include "mtl_encoder.h"
#include "kosmickrisp/vulkan/kk_mesh_pipeline_api.h"
#include <Metal/Metal.h>

mtl_render_pipeline_descriptor *
mtl_new_mesh_pipeline_descriptor(mtl_function_descriptor *mesh,
                                 mtl_function_descriptor *object,
                                 uint32_t mesh_threads, uint32_t object_threads,
                                 uint32_t payload_bytes, uint32_t mesh_groups)
{
   @autoreleasepool {
      MTL4MeshRenderPipelineDescriptor *desc = [MTL4MeshRenderPipelineDescriptor new];
      desc.meshFunctionDescriptor = (MTL4FunctionDescriptor *)mesh;
      desc.maxTotalThreadsPerMeshThreadgroup = mesh_threads;
      if (object) {
         desc.objectFunctionDescriptor = (MTL4FunctionDescriptor *)object;
         desc.maxTotalThreadsPerObjectThreadgroup = object_threads;
         /* Apple8 needs a complete payload allocation unit for each object
          * workgroup. A four-byte reservation silently loses child groups. */
         uint32_t aligned_payload = (payload_bytes + 15u) & ~15u;
         desc.payloadMemoryLength = aligned_payload ? aligned_payload : 16u;
         desc.maxTotalThreadgroupsPerMeshGrid = mesh_groups ? mesh_groups : 1;
      }
      return (mtl_render_pipeline_descriptor *)desc;
   }
}

void
mtl_draw_mesh_threadgroups(mtl_render_encoder *encoder, struct mtl_size groups,
                           struct mtl_size object_size, struct mtl_size mesh_size)
{
   @autoreleasepool {
      id<MTL4RenderCommandEncoder> enc = (id<MTL4RenderCommandEncoder>)encoder;
      [enc drawMeshThreadgroups:MTLSizeMake(groups.x, groups.y, groups.z)
         threadsPerObjectThreadgroup:MTLSizeMake(object_size.x, object_size.y, object_size.z)
           threadsPerMeshThreadgroup:MTLSizeMake(mesh_size.x, mesh_size.y, mesh_size.z)];
   }
}

void
mtl_draw_mesh_threadgroups_indirect(mtl_render_encoder *encoder, uint64_t address,
                                    struct mtl_size object_size,
                                    struct mtl_size mesh_size)
{
   @autoreleasepool {
      id<MTL4RenderCommandEncoder> enc = (id<MTL4RenderCommandEncoder>)encoder;
      [enc drawMeshThreadgroupsWithIndirectBuffer:address
         threadsPerObjectThreadgroup:MTLSizeMake(object_size.x, object_size.y, object_size.z)
           threadsPerMeshThreadgroup:MTLSizeMake(mesh_size.x, mesh_size.y, mesh_size.z)];
   }
}

/* Preserve the linked Mesh reader, threadgroup shape, topology and attachment
 * metadata. Only the supplied capture function replaces the fragment stage. */
mtl_render_pipeline_descriptor *
kk_mesh_retained_fragment_descriptor(const struct kk_mesh_retained_batch *batch,
                                     mtl_function_descriptor *fragment)
{
   if (!batch || !batch->raster_descriptor || !fragment)
      return NULL;
   @autoreleasepool {
      MTL4MeshRenderPipelineDescriptor *reader =
         (MTL4MeshRenderPipelineDescriptor *)batch->raster_descriptor;
      if (!reader.meshFunctionDescriptor || reader.objectFunctionDescriptor)
         return NULL;
      return mtl_copy_render_pipeline_descriptor_with_fragment(reader, fragment);
   }
}

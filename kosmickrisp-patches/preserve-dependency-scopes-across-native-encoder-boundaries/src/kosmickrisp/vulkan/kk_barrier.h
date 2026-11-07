/* SPDX-License-Identifier: MIT */
#ifndef KK_BARRIER_H
#define KK_BARRIER_H

/* Native stages available in an encoder, including internal implementations
 * of Vulkan graphics and transfer commands. No per-draw hazard bookkeeping. */
#define KK_RENDER_STAGES                                                       \
   (MTL_STAGE_VERTEX | MTL_STAGE_FRAGMENT | MTL_STAGE_TILE |                   \
    MTL_STAGE_OBJECTS | MTL_STAGE_MESH)
#define KK_COMPUTE_STAGES                                                      \
   (MTL_STAGE_DISPATCH | MTL_STAGE_BLIT | MTL_STAGE_ACCELERATION_STRUCTURE)

struct kk_barrier_scope {
   enum mtl_stages src, dst;
   enum mtl_visibility_options visibility;
};

static inline enum mtl_stages
kk_barrier_stages(VkPipelineStageFlags2 stages, bool src)
{
   const VkPipelineStageFlags2 top = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
   const VkPipelineStageFlags2 bottom = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
   enum mtl_stages native = 0;
   if (stages & (VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT |
                 VK_PIPELINE_STAGE_2_HOST_BIT | (src ? bottom : top)))
      return MTL_STAGE_ALL;
   stages &= ~(top | bottom);

#define MAP(vk, mtl)                                                           \
   do {                                                                        \
      if (stages & (vk)) {                                                     \
         native |= (mtl);                                                      \
         stages &= ~(vk);                                                      \
      }                                                                        \
   } while (0)
   MAP(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
          VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT,
       KK_RENDER_STAGES | MTL_STAGE_DISPATCH);
   MAP(VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
          VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
          VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
          VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
          VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
          VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
          VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
          VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT |
          VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
       MTL_STAGE_VERTEX | MTL_STAGE_OBJECTS | MTL_STAGE_MESH |
          MTL_STAGE_DISPATCH);
   MAP(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
       MTL_STAGE_FRAGMENT | MTL_STAGE_DISPATCH);
   MAP(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
       MTL_STAGE_FRAGMENT);
   MAP(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
       MTL_STAGE_FRAGMENT | MTL_STAGE_TILE);
   MAP(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, MTL_STAGE_DISPATCH);
   /* Meta blits/clears/resolves can use a render or compute pipeline, while
    * native copies execute on the compute encoder's BLIT stage. */
   MAP(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
          VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
          VK_PIPELINE_STAGE_2_CLEAR_BIT,
       KK_RENDER_STAGES | MTL_STAGE_DISPATCH | MTL_STAGE_BLIT);
#undef MAP
   /* Preserve newly introduced extension scopes until a narrower native
    * implementation is known; never discard a valid GPU stage as zero. */
   return stages ? MTL_STAGE_ALL : native;
}

static inline void
kk_barrier_scope_add(struct kk_barrier_scope *scope, VkPipelineStageFlags2 src,
                     VkPipelineStageFlags2 dst, VkAccessFlags2 src_access,
                     VkAccessFlags2 dst_access)
{
   scope->src |= kk_barrier_stages(src, true);
   scope->dst |= kk_barrier_stages(dst, false);
   /* Vulkan resources may alias without a device feature. Keep alias
    * visibility for explicit memory dependencies; internal handoffs between
    * the same native resources use Device visibility instead. */
   if (src_access || dst_access)
      scope->visibility = MTL_VISIBILITY_RESOURCE_ALIAS;
}

static inline void
kk_barrier_scope_add_dependency(struct kk_barrier_scope *scope,
                                const VkDependencyInfo *dep)
{
#define ADD(b)                                                                 \
   kk_barrier_scope_add(scope, (b)->srcStageMask, (b)->dstStageMask,           \
                        (b)->srcAccessMask, (b)->dstAccessMask)
   for (uint32_t i = 0; i < dep->memoryBarrierCount; ++i)
      ADD(&dep->pMemoryBarriers[i]);
   for (uint32_t i = 0; i < dep->bufferMemoryBarrierCount; ++i)
      ADD(&dep->pBufferMemoryBarriers[i]);
   for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; ++i) {
      const VkImageMemoryBarrier2 *b = &dep->pImageMemoryBarriers[i];
      ADD(b);
      if (b->oldLayout != b->newLayout)
         scope->visibility = MTL_VISIBILITY_RESOURCE_ALIAS;
   }
   vk_foreach_struct_const(sType, ext, dep->pNext) {
      if (sType == VK_STRUCTURE_TYPE_MEMORY_RANGE_BARRIERS_INFO_KHR) {
         const VkMemoryRangeBarriersInfoKHR *ranges = (const void *)ext;
         for (uint32_t i = 0; i < ranges->memoryRangeBarrierCount; ++i)
            ADD(&ranges->pMemoryRangeBarriers[i]);
      } else {
         scope->src = scope->dst = MTL_STAGE_ALL;
         scope->visibility = MTL_VISIBILITY_RESOURCE_ALIAS;
      }
   }
#undef ADD
}

#endif

/* SPDX-License-Identifier: MIT */
/* Canonical allocations are real internal VkImage/VkDeviceMemory planes.
 * The shared stateless constructor only retains or creates typed aliases;
 * this module owns block-coordinate materialization and its lifetime. */
static void kk_standard_3d_page_finish(struct kk_device *,struct kk_standard_3d_page *);

static struct kk_standard_3d_page *
kk_standard_3d_page_new(struct kk_device *dev,VkExtent3D shape,
   const struct kk_image_layout *source)
{
   struct kk_standard_3d_page *page=calloc(1,sizeof(*page));
   if(!page)return NULL;
   simple_mtx_init(&page->mutex,mtx_plain);page->shape=shape;
   const VkImageCreateInfo create={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_ALIAS_BIT,
      .imageType=shape.depth==1?VK_IMAGE_TYPE_2D:VK_IMAGE_TYPE_3D,.format=vk_format_from_pipe_format(source->format.pipe),
      .extent=shape,.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
      .tiling=VK_IMAGE_TILING_OPTIMAL,
      .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
   VkDevice device=kk_device_to_handle(dev);
   VkResult result=kk_CreateImage(device,&create,NULL,&page->image);
   if(result!=VK_SUCCESS)goto fail;
   VK_FROM_HANDLE(kk_image,image,page->image);
   struct kk_image_plane *plane=&image->planes[0];
   plane->layout.optimized_layout=false;
   plane->layout.usage=MTL_TEXTURE_USAGE_SHADER_READ|MTL_TEXTURE_USAGE_PIXEL_FORMAT_VIEW;
   if(!util_format_is_compressed(source->format.pipe))plane->layout.usage|=MTL_TEXTURE_USAGE_SHADER_WRITE|(source->usage&MTL_TEXTURE_USAGE_SHADER_ATOMIC);
   /* Requirements must describe the final unoptimized native descriptor,
    * after its usage and alias layout have been selected. */
   mtl_heap_texture_size_and_align_with_descriptor(dev->mtl_handle,&plane->layout,
      &plane->layout.size_B,&plane->layout.align_B);
   const VkImageMemoryRequirementsInfo2 req_info={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=page->image};
   VkMemoryRequirements2 requirements={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
   kk_GetImageMemoryRequirements2(device,&req_info,&requirements);
   VkMemoryRequirements req=requirements.memoryRequirements;
   struct kk_physical_device *pdev=kk_device_physical(dev);
   uint32_t type=UINT32_MAX;
   for(unsigned i=0;i<pdev->mem_type_count;i++)
      if((req.memoryTypeBits&BITFIELD_BIT(i))&&
         (pdev->mem_types[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)&&
         !(pdev->mem_types[i].propertyFlags&VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) {type=i;break;}
   if(type==UINT32_MAX)goto fail;
   const VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize=req.size,.memoryTypeIndex=type};
   result=kk_AllocateMemory(device,&alloc,NULL,&page->memory);
   if(result!=VK_SUCCESS)goto fail;
   const VkBindImageMemoryInfo bind={.sType=VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
      .image=page->image,.memory=page->memory,.memoryOffset=0};
   result=kk_BindImageMemory2(device,1,&bind);
   if(result!=VK_SUCCESS)goto fail;
   return page;
fail:
   kk_standard_3d_page_finish(dev,page);return NULL;
}

static struct kk_standard_3d_page *
kk_standard_3d_physical_page(struct kk_device *dev,VkDeviceMemory memory,
   uint64_t offset,VkExtent3D shape,const struct kk_image_layout *source)
{
   VK_FROM_HANDLE(kk_device_memory,mem,memory);
   simple_mtx_lock(&mem->transient_mutex);
   struct kk_standard_3d_page *page=mem->standard_3d;
   while(page&&(page->offset!=offset||memcmp(&page->shape,&shape,sizeof(shape))))page=page->next;
   if(!page) {
      page=kk_standard_3d_page_new(dev,shape,source);
      if(page){page->offset=offset;page->next=mem->standard_3d;mem->standard_3d=page;}
   }
   simple_mtx_unlock(&mem->transient_mutex);return page;
}

static mtl_texture *
kk_standard_3d_physical_target(struct kk_device *dev,
   struct kk_standard_3d_page *page,const struct kk_view_layout *view,
   const struct kk_image_layout *source)
{
   simple_mtx_lock(&page->mutex);
   struct kk_standard_3d_target *target=page->targets;
   while(target&&(target->layout.format.pipe!=view->format.pipe||
      target->layout.view_type!=view->view_type||
      memcmp(&target->layout.swizzle,&view->swizzle,sizeof(view->swizzle))))target=target->next;
   if(!target) {
      target=calloc(1,sizeof(*target));
      if(target) {
         VK_FROM_HANDLE(kk_image,image,page->image);
         struct kk_image_plane *plane=&image->planes[0];
         struct kk_standard_3d_format *format=page->formats;
         while(format&&format->format!=view->format.mtl)format=format->next;
         if(!format) {
            format=calloc(1,sizeof(*format));
            if(format) {
               struct kk_image_layout layout=plane->layout;
               layout.format.pipe=view->format.pipe;layout.format.mtl=view->format.mtl;
               if(util_format_is_compressed(source->format.pipe)&&!util_format_is_compressed(view->format.pipe)) {
                  layout.width_px=util_format_get_nblocksx(source->format.pipe,page->shape.width);
                  layout.height_px=util_format_get_nblocksy(source->format.pipe,page->shape.height);
                  layout.depth_px=util_format_get_nblocksz(source->format.pipe,page->shape.depth);
               }
               if(!util_format_is_compressed(view->format.pipe))layout.usage|=MTL_TEXTURE_USAGE_SHADER_WRITE;
               /* A typed alias keeps texture atomics only for a format that
                * supports them; Metal rejects the usage for other formats. */
               const struct kk_va_format *va=kk_get_va_format(view->format.pipe);
               if(!va||!va->atomic)layout.usage&=~MTL_TEXTURE_USAGE_SHADER_ATOMIC;
               mtl_texture *canonical=view->format.mtl==plane->layout.format.mtl?plane->mtl_handle:NULL;
               VkResult result=kk_layered_bc_native_alias_init(dev,plane,&layout,0,canonical,NULL,NULL,&format->native);
               if(result!=VK_SUCCESS){free(format);format=NULL;}
               else {
                  format->format=view->format.mtl;kk_sparse_residency(dev,format->native.base,true);
                  format->next=page->formats;page->formats=format;
               }
            }
         }
         struct kk_view_layout canonical=*view;
         canonical.base_level=canonical.base_array_layer=0;
         canonical.num_levels=canonical.array_len=1;canonical.min_lod_clamp=0;
         VkResult result=format?kk_layered_bc_native_alias_init(dev,NULL,NULL,0,
            format->native.base,&canonical,NULL,&target->native):VK_ERROR_OUT_OF_DEVICE_MEMORY;
         if(result!=VK_SUCCESS){free(target);target=NULL;}
         else {
            target->layout=*view;kk_sparse_residency(dev,target->native.sampled,true);
            target->next=page->targets;page->targets=target;
         }
      }
   }
   simple_mtx_unlock(&page->mutex);return target?target->native.sampled:NULL;
}

static void
kk_standard_3d_page_finish(struct kk_device *dev,struct kk_standard_3d_page *page)
{
   struct kk_standard_3d_target *target=page->targets;
   while(target){struct kk_standard_3d_target *next=target->next;
      kk_sparse_residency(dev,target->native.sampled,false);
      kk_layered_bc_native_alias_finish(&target->native);free(target);target=next;}
   struct kk_standard_3d_format *format=page->formats;
   while(format){struct kk_standard_3d_format *next=format->next;
      kk_sparse_residency(dev,format->native.base,false);
      kk_layered_bc_native_alias_finish(&format->native);free(format);format=next;}
   if(page->image)kk_DestroyImage(kk_device_to_handle(dev),page->image,NULL);
   if(page->memory)kk_FreeMemory(kk_device_to_handle(dev),page->memory,NULL);
   simple_mtx_destroy(&page->mutex);free(page);
}

void
kk_standard_3d_memory_finish(struct kk_device *dev,struct kk_device_memory *mem)
{
   struct kk_standard_3d_page *page=mem->standard_3d;
   while(page){struct kk_standard_3d_page *next=page->next;kk_standard_3d_page_finish(dev,page);page=next;}
   mem->standard_3d=NULL;
}

/* A raw meta view needs only a virtual native query/zero fallback. Its actual
 * mapped reads and writes use the canonical real-page constructor above.
 * This object has the view's block extent and no physical residency. */
VkResult
kk_standard_3d_raw_view(struct kk_device *dev,struct kk_image_view *view,
   const struct kk_view_layout *view_layout,mtl_texture **out)
{
   struct kk_image *image=container_of(view->vk.image,struct kk_image,vk);
   struct kk_image_layout layout=image->planes[0].layout;
   layout.width_px=view->vk.extent.width;layout.height_px=view->vk.extent.height;
   layout.depth_px=view->vk.extent.depth;layout.levels=layout.layers=1;
   layout.format.pipe=view_layout->format.pipe;layout.format.mtl=view_layout->format.mtl;
   layout.usage=MTL_TEXTURE_USAGE_SHADER_READ|MTL_TEXTURE_USAGE_SHADER_WRITE|MTL_TEXTURE_USAGE_PIXEL_FORMAT_VIEW;
   *out=mtl_new_sparse_texture(dev->mtl_handle,&layout);
   return *out?VK_SUCCESS:vk_error(dev,VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

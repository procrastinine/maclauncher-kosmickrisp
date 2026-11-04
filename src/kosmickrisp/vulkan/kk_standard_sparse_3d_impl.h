/* SPDX-License-Identifier: MIT */
/* Included after the existing sparse mapping batch helpers. Native backing
 * and logical residency deliberately have separate reference counts. */
static VkExtent3D
kk_standard_3d_shape(enum pipe_format format)
{
   VkExtent3D shape = {0};
   switch (util_format_get_blocksize(format)) {
   case 1: shape = (VkExtent3D){64,32,32}; break;
   case 2: shape = (VkExtent3D){32,32,32}; break;
   case 4: shape = (VkExtent3D){32,32,16}; break;
   case 8: shape = (VkExtent3D){32,16,16}; break;
   case 16: shape = (VkExtent3D){16,16,16}; break;
   }
   shape.width *= util_format_get_blockwidth(format);
   shape.height *= util_format_get_blockheight(format);
   shape.depth *= util_format_get_blockdepth(format);
   return shape;
}

static VkResult
kk_standard_3d_image_initialize(struct kk_device *dev,struct kk_image *image)
{
   struct kk_sparse_image *s = image->sparse;
   struct kk_image_plane *plane = &image->planes[0];
   struct kk_standard_3d_image *state = calloc(1,sizeof(*state));
   if (!state) return vk_error(dev,VK_ERROR_OUT_OF_HOST_MEMORY);
   state->references=1;
   simple_mtx_init(&state->mutex,mtx_plain);
   s->standard_3d = state;
   state->two_d = image->vk.image_type != VK_IMAGE_TYPE_3D;
   state->layers = state->two_d ? image->vk.array_layers : 1;
   struct mtl_size native = mtl_sparse_granularity(dev->mtl_handle,
      plane->layout.format.mtl,MTL_TEXTURE_TYPE_3D);
   /* Native tier 1 2D storage is never accessed: tails, partial tiles and
    * non-square residency are wrong there. Data lives in canonical pages. */
   if (state->two_d) native = (struct mtl_size){0};
   state->native_granularity = (VkExtent3D){native.x,native.y,native.z};
   state->native_first_tail = state->two_d ? image->vk.mip_levels : MIN2(image->vk.mip_levels,
      mtl_texture_first_mipmap_in_tail(plane->mtl_handle));
   state->native_tail_pages = state->native_first_tail < image->vk.mip_levels ?
      DIV_ROUND_UP(mtl_sparse_tail_size(plane->mtl_handle),KK_SPARSE_PAGE_SIZE) : 0;
   uint64_t native_pages = 0;
   for (unsigned l=0;!state->two_d&&l<state->native_first_tail;l++) {
      state->native_level_page[l] = native_pages;
      native_pages += (uint64_t)DIV_ROUND_UP(u_minify(image->vk.extent.width,l),native.x) *
         DIV_ROUND_UP(u_minify(image->vk.extent.height,l),native.y) *
         DIV_ROUND_UP(u_minify(image->vk.extent.depth,l),native.z);
   }
   state->native_pages = native_pages;
   state->native = calloc(MAX2(native_pages,1),sizeof(*state->native));
   if (!state->native) return vk_error(dev,VK_ERROR_OUT_OF_HOST_MEMORY);
   if (!state->two_d)
      s->granularity = kk_standard_3d_shape(vk_format_to_pipe_format(image->vk.format));
   s->tail_first_lod = image->vk.mip_levels;
   /* A 2D image reports the tail of a sampled-only sparse image of the same
    * shape, so images aliasing one memory range agree on tail start and size
    * whatever their usage. */
   uint32_t shader_read_first = UINT32_MAX;
   uint64_t shader_read_tail = 0;
   if (state->two_d)
      mtl_sparse_shader_read_tail(dev->mtl_handle, &plane->layout,
                                  &shader_read_first, &shader_read_tail);
   uint64_t pages = 0;
   for (unsigned l=0;l<image->vk.mip_levels;l++) {
      VkExtent3D size = {u_minify(image->vk.extent.width,l),
         u_minify(image->vk.extent.height,l),u_minify(image->vk.extent.depth,l)};
      uint64_t bytes = (uint64_t)util_format_get_nblocksx(plane->layout.format.pipe,size.width) *
         util_format_get_nblocksy(plane->layout.format.pipe,size.height) *
         util_format_get_nblocksz(plane->layout.format.pipe,size.depth) *
         util_format_get_blocksize(plane->layout.format.pipe);
      if (state->two_d ? l >= shader_read_first : bytes < KK_SPARSE_PAGE_SIZE) {
         s->tail_first_lod=l;
         break;
      }
      s->level_page[l] = pages;
      VkExtent3D t = kk_sparse_level_tiles(image,l);
      pages += (uint64_t)t.width*t.height*t.depth;
   }
   s->level_page[s->tail_first_lod] = pages;
   uint64_t tail_bytes = 0;
   const unsigned element = util_format_get_blocksize(plane->layout.format.pipe);
   for (unsigned l=s->tail_first_lod;l<image->vk.mip_levels;l++)
      tail_bytes += (uint64_t)util_format_get_nblocksx(plane->layout.format.pipe,u_minify(image->vk.extent.width,l)) *
         util_format_get_nblocksy(plane->layout.format.pipe,u_minify(image->vk.extent.height,l)) *
         util_format_get_nblocksz(plane->layout.format.pipe,u_minify(image->vk.extent.depth,l)) * element;
   s->tail_pages = DIV_ROUND_UP(tail_bytes,KK_SPARSE_PAGE_SIZE);
   if (state->two_d && s->tail_first_lod < image->vk.mip_levels)
      s->tail_pages = MAX2(s->tail_pages,DIV_ROUND_UP(shader_read_tail,KK_SPARSE_PAGE_SIZE));
   s->layer_pages = pages+s->tail_pages;
   state->tile_count = s->layer_pages*state->layers;
   state->dirty_first = UINT64_MAX;
   uint64_t entries_offset = align64(sizeof(struct kk_standard_3d_table),32);
   state->table = mtl_sparse_new_residency_buffer(dev->mtl_handle,
      entries_offset+state->tile_count*sizeof(struct kk_standard_3d_tile),false);
   state->tiles = calloc(MAX2(state->tile_count,1),sizeof(*state->tiles));
   state->memory = calloc(MAX2(state->tile_count,1),sizeof(*state->memory));
   state->memory_offset = calloc(MAX2(state->tile_count,1),sizeof(*state->memory_offset));
   state->pages = calloc(MAX2(state->tile_count,1),sizeof(*state->pages));
   if (!state->table||!state->tiles||!state->memory||!state->memory_offset||!state->pages)
      return vk_error(dev,VK_ERROR_OUT_OF_HOST_MEMORY);
   struct kk_standard_3d_table *table = mtl_get_contents(state->table);
   *table = (struct kk_standard_3d_table){
      .shape={s->granularity.width,s->granularity.height,s->granularity.depth},
      .first_tail=s->tail_first_lod,
      .extent={image->vk.extent.width,image->vk.extent.height,image->vk.extent.depth},
      .levels=image->vk.mip_levels,.element_bytes=element,
      .block={util_format_get_blockwidth(plane->layout.format.pipe),
         util_format_get_blockheight(plane->layout.format.pipe),
         util_format_get_blockdepth(plane->layout.format.pipe)},
      .entries=mtl_buffer_get_gpu_address(state->table)+entries_offset,
      .layer_pages=s->layer_pages,.layers=state->layers,
   };
   for (unsigned l=0;l<=s->tail_first_lod;l++)table->level_page[l]=s->level_page[l];
   uint64_t byte_offset=0;
   for (unsigned l=s->tail_first_lod;l<image->vk.mip_levels;l++) {
      table->tail_offset_B[l]=byte_offset;
      byte_offset+=(uint64_t)util_format_get_nblocksx(plane->layout.format.pipe,u_minify(image->vk.extent.width,l))*
         util_format_get_nblocksy(plane->layout.format.pipe,u_minify(image->vk.extent.height,l))*
         util_format_get_nblocksz(plane->layout.format.pipe,u_minify(image->vk.extent.depth,l))*element;
   }
   plane->layout.size_B=state->tile_count*KK_SPARSE_PAGE_SIZE;
   plane->layout.align_B=KK_SPARSE_PAGE_SIZE;
   kk_sparse_residency(dev,state->table,true);
   return VK_SUCCESS;
}

static void
kk_standard_3d_image_unref(struct kk_device *dev,struct kk_standard_3d_image *s)
{
   if(!p_atomic_dec_zero(&s->references))return;
   if(s->table){kk_sparse_residency(dev,s->table,false);mtl_release(s->table);}
   for(uint64_t i=0;s->native&&i<s->native_pages;i++)if(s->native[i].heap){
      kk_sparse_residency(dev,s->native[i].heap,false);mtl_release(s->native[i].heap);
   }
   if(s->native_tail_heap){kk_sparse_residency(dev,s->native_tail_heap,false);mtl_release(s->native_tail_heap);}
   assert(!s->views);
   simple_mtx_destroy(&s->mutex);
   free(s->native);free(s->tiles);free(s->memory);free(s->memory_offset);free(s->pages);free(s);
}

/* Internal meta views belong to command-buffer retirement, which may follow
 * destruction of an idle API image. Their table ownership is independent of
 * the image object and never reads view->vk.image during destruction. */
static void
kk_standard_3d_image_finish(struct kk_device *dev,struct kk_image *image)
{
   struct kk_standard_3d_image *s=image->sparse?image->sparse->standard_3d:NULL;
   if(!s)return;
   image->sparse->standard_3d=NULL;
   kk_standard_3d_image_unref(dev,s);
}

static uint32_t
kk_standard_3d_query_stride(const struct kk_image_view *view)
{
   const struct kk_image *image=container_of(view->vk.image,struct kk_image,vk);
   return image->sparse&&image->sparse->standard_3d?
      sizeof(struct kk_standard_3d_query):sizeof(struct kk_sparse_query_record);
}

static uint32_t
kk_standard_3d_query_bytes(const struct kk_image *image)
{
   return image->sparse&&image->sparse->standard_3d?
      2*(sizeof(struct kk_standard_3d_query)+sizeof(struct kk_standard_3d_view)):
      2*sizeof(struct kk_sparse_query_record);
}

/* A 2D page target carries the plain 2D handle and, in the array slot, a
 * one-layer 2D array handle, as ordinary 2D storage descriptors do. */
static bool
kk_standard_3d_target_entry(struct kk_device *dev,struct kk_standard_3d_image *s,
   struct kk_standard_3d_page *page,const struct kk_view_layout *layout,
   const struct kk_image_layout *source,struct kk_storage_image_descriptor *entry)
{
   const bool raw=util_format_is_compressed(source->format.pipe)&&!util_format_is_compressed(layout->format.pipe);
   if(s->two_d){
      struct kk_view_layout plain=*layout,array=*layout;
      plain.view_type=VK_IMAGE_VIEW_TYPE_2D;array.view_type=VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      mtl_texture *p=kk_standard_3d_physical_target(dev,page,&plain,source);
      mtl_texture *a=p?kk_standard_3d_physical_target(dev,page,&array,source):NULL;
      if(!a)return false;
      *entry=(struct kk_storage_image_descriptor){.image_gpu_resource_id=mtl_texture_get_gpu_resource_id(p)};
      entry->image_array_gpu_resource_id=mtl_texture_get_gpu_resource_id(a);
      return true;
   }
   mtl_texture *target=kk_standard_3d_physical_target(dev,page,layout,source);
   if(!target)return false;
   *entry=(struct kk_storage_image_descriptor){
      .image_gpu_resource_id=mtl_texture_get_gpu_resource_id(target),
      .texel_count=raw?util_format_get_nblocksz(source->format.pipe,page->shape.depth):page->shape.depth,
   };
   return true;
}

static VkResult
kk_standard_3d_query_initialize(struct kk_image_view *view,bool storage,
                                 const struct kk_view_layout *layout)
{
   struct kk_image *image=container_of(view->vk.image,struct kk_image,vk);
   if(!image->sparse||!image->sparse->standard_3d)return VK_SUCCESS;
   struct kk_standard_3d_image *s=image->sparse->standard_3d;
   struct kk_device *dev=(struct kk_device *)image->vk.base.device;
   simple_mtx_lock(&s->mutex);
   struct kk_standard_3d_view_state *state=view->sparse->standard_3d;
   if(!state){
      state=calloc(1,sizeof(*state));
      if(!state){simple_mtx_unlock(&s->mutex);return vk_error(dev,VK_ERROR_OUT_OF_HOST_MEMORY);}
      state->view=view;state->image_state=s;p_atomic_inc(&s->references);
      state->next=s->views;s->views=state;
      view->sparse->standard_3d=state;
   }
   state->layout[storage]=*layout;
   state->targets[storage]=mtl_sparse_new_residency_buffer(dev->mtl_handle,
      MAX2(s->tile_count,1)*sizeof(struct kk_storage_image_descriptor),false);
   if(!state->targets[storage]){simple_mtx_unlock(&s->mutex);return vk_error(dev,VK_ERROR_OUT_OF_DEVICE_MEMORY);}
   kk_sparse_residency(dev,state->targets[storage],true);
   struct kk_storage_image_descriptor *targets=mtl_get_contents(state->targets[storage]);
   enum pipe_format parent_format=image->planes[0].layout.format.pipe;
   const bool raw=util_format_is_compressed(parent_format)&&!util_format_is_compressed(layout->format.pipe);
   for(uint64_t i=0;i<s->tile_count;i++) {
      struct kk_standard_3d_page *page=s->pages[i];
      if(!page)continue;
      if(!kk_standard_3d_target_entry(dev,s,page,layout,&image->planes[0].layout,&targets[i])){
         simple_mtx_unlock(&s->mutex);return vk_error(dev,VK_ERROR_OUT_OF_DEVICE_MEMORY);}
   }
   char *cpu=mtl_get_contents(view->sparse->records);
   struct kk_standard_3d_query *query=(void *)(cpu+storage*sizeof(*query));
   uint64_t extra=2*sizeof(*query)+storage*sizeof(struct kk_standard_3d_view);
   struct kk_standard_3d_view *logical=(void *)(cpu+extra);
   logical->image_table=mtl_buffer_get_gpu_address(image->sparse->standard_3d->table);
   logical->targets=mtl_buffer_get_gpu_address(state->targets[storage]);
   logical->coord_scale[0]=raw?util_format_get_blockwidth(parent_format):1;
   logical->coord_scale[1]=raw?util_format_get_blockheight(parent_format):1;
   logical->coord_scale[2]=raw?util_format_get_blockdepth(parent_format):1;
   const struct util_format_description *format=util_format_description(layout->format.pipe);
   for(unsigned i=0;i<4;i++) {
      unsigned swizzle=layout->swizzle.channels[i];
      if(swizzle==PIPE_SWIZZLE_1 ||
         (swizzle<4 && format->swizzle[swizzle]==PIPE_SWIZZLE_1))
         logical->zero_one_mask|=1u<<i;
   }
   logical->base_layer=s->two_d?view->vk.base_array_layer:0;
   query->prefix.kind=s->two_d?KK_QUERY_RECORD_KIND_STANDARD_2D:KK_QUERY_RECORD_KIND_STANDARD_3D;
   query->logical_view=mtl_buffer_get_gpu_address(view->sparse->records)+extra;
   simple_mtx_unlock(&s->mutex);
   return VK_SUCCESS;
}

static void
kk_standard_3d_query_finish(struct kk_device *dev,struct kk_image_view *view)
{
   struct kk_standard_3d_view_state *state=view->sparse->standard_3d;
   if(!state)return;
   struct kk_standard_3d_image *s=state->image_state;
   simple_mtx_lock(&s->mutex);
   struct kk_standard_3d_view_state **at=&s->views;
   while(*at!=state)at=&(*at)->next;
   *at=state->next;
   simple_mtx_unlock(&s->mutex);
   for(unsigned i=0;i<2;i++)if(state->targets[i]){
      kk_sparse_residency(dev,state->targets[i],false);mtl_release(state->targets[i]);
   }
   free(state);view->sparse->standard_3d=NULL;
   kk_standard_3d_image_unref(dev,s);
}

static mtl_heap *
kk_standard_3d_new_heap(struct kk_device *dev,uint64_t pages)
{
   mtl_heap *heap=mtl_new_heap(dev->mtl_handle,pages*KK_SPARSE_PAGE_SIZE,
      MTL_RESOURCE_STORAGE_MODE_PRIVATE|MTL_RESOURCE_TRACKING_MODE_UNTRACKED);
   if(heap)kk_sparse_residency(dev,heap,true);
   return heap;
}

#include "kk_standard_sparse_3d_physical.h"

static bool
kk_standard_3d_tail_reference(struct kk_queue *queue,struct kk_image *image,bool add)
{
   struct kk_standard_3d_image *s=image->sparse->standard_3d;
   if(!s->native_tail_pages)return false;
   uint32_t previous=s->native_tail_references;
   if(add)s->native_tail_references++;
   else{assert(previous);s->native_tail_references--;}
   if((previous!=0)==(s->native_tail_references!=0))return true;
   struct kk_device *dev=kk_queue_device(queue);
   if(add&&!s->native_tail_heap){
      s->native_tail_heap=kk_standard_3d_new_heap(dev,s->native_tail_pages);
      if(!s->native_tail_heap){s->native_tail_references=previous;return false;}
   }
   struct mtl_sparse_texture_mapping op={.mode=add?0:1,
      .origin={0},.size={s->native_tail_pages,1,1},
      .level=s->native_first_tail,.layer=0,.offset=0};
   mtl_sparse_map_texture_batch(queue->mtl_handle,image->planes[0].mtl_handle,
      add?s->native_tail_heap:NULL,&op,1);
   return true;
}

static bool
kk_standard_3d_native_reference(struct kk_queue *queue,struct kk_image *image,
   uint32_t level,const uint32_t origin[3],bool add)
{
   struct kk_standard_3d_image *s=image->sparse->standard_3d;
   if(level>=s->native_first_tail)return kk_standard_3d_tail_reference(queue,image,add);
   const VkExtent3D *g=&s->native_granularity,*logical=&image->sparse->granularity;
   uint32_t extent[3]={u_minify(image->vk.extent.width,level),
      u_minify(image->vk.extent.height,level),u_minify(image->vk.extent.depth,level)};
   uint32_t shape[3]={logical->width,logical->height,logical->depth};
   uint32_t native[3]={g->width,g->height,g->depth},first[3],last[3];
   for(unsigned i=0;i<3;i++){first[i]=origin[i]/native[i];last[i]=DIV_ROUND_UP(MIN2(origin[i]+shape[i],extent[i]),native[i]);}
   uint32_t nx=DIV_ROUND_UP(extent[0],native[0]),ny=DIV_ROUND_UP(extent[1],native[1]);
   for(uint32_t z=first[2];z<last[2];z++)for(uint32_t y=first[1];y<last[1];y++)for(uint32_t x=first[0];x<last[0];x++){
      uint64_t index=s->native_level_page[level]+x+(uint64_t)nx*(y+(uint64_t)ny*z);
      assert(index<s->native_pages);
      struct kk_standard_3d_backing *page=&s->native[index];
      uint32_t previous=page->references;
      if(add)page->references++;else{assert(previous);page->references--;}
      if((previous!=0)==(page->references!=0))continue;
      if(add&&!page->heap){page->heap=kk_standard_3d_new_heap(kk_queue_device(queue),1);if(!page->heap){page->references=previous;return false;}}
      struct mtl_sparse_texture_mapping op={.mode=add?0:1,
         .origin={x,y,z},.size={1,1,1},.level=level,.layer=0,.offset=0};
      mtl_sparse_map_texture_batch(queue->mtl_handle,image->planes[0].mtl_handle,
         add?page->heap:NULL,&op,1);
   }
   return true;
}

/* A logical packed tail can start before or after the native packed tail.
 * Keep every native cell intersecting its mip levels backed; no native layout
 * boundary is used to infer logical residency. */
static bool
kk_standard_3d_logical_tail_reference(struct kk_queue *queue,struct kk_image *image,bool add)
{
   struct kk_standard_3d_image *s=image->sparse->standard_3d;
   VkExtent3D shape=image->sparse->granularity;
   for(unsigned l=image->sparse->tail_first_lod;l<s->native_first_tail;l++) {
      VkExtent3D extent={u_minify(image->vk.extent.width,l),
         u_minify(image->vk.extent.height,l),u_minify(image->vk.extent.depth,l)};
      for(uint32_t z=0;z<extent.depth;z+=shape.depth)
         for(uint32_t y=0;y<extent.height;y+=shape.height)
            for(uint32_t x=0;x<extent.width;x+=shape.width) {
               const uint32_t origin[3]={x,y,z};
               if(!kk_standard_3d_native_reference(queue,image,l,origin,add))return false;
            }
   }
   return !s->native_tail_pages||kk_standard_3d_tail_reference(queue,image,add);
}

static bool
kk_standard_3d_publish_targets(struct kk_device *dev,struct util_dynarray *copies,
   struct kk_standard_3d_image *s,uint64_t first,uint64_t count)
{
   if(!count)return true;
   for(struct kk_standard_3d_view_state *view=s->views;view;view=view->next)
      for(unsigned storage=0;storage<2;storage++)if(view->targets[storage]) {
         const uint64_t stride=sizeof(struct kk_storage_image_descriptor);
         mtl_buffer *values=mtl_sparse_new_residency_buffer(dev->mtl_handle,count*stride,false);
         if(!values)return false;
         struct kk_storage_image_descriptor *targets=mtl_get_contents(values);
         struct kk_image *image=container_of(view->view->vk.image,struct kk_image,vk);
         for(uint64_t n=0;n<count;n++) {
            uint64_t index=first+n;
            struct kk_standard_3d_page *page=s->pages[index];
            if(!page)continue;
            if(!kk_standard_3d_target_entry(dev,s,page,&view->layout[storage],&image->planes[0].layout,&targets[n])){
               mtl_release(values);return false;}
         }
         struct mtl_sparse_residency_copy *copy=util_dynarray_grow(copies,struct mtl_sparse_residency_copy,1);
         if(!copy){mtl_release(values);return false;}
         *copy=(struct mtl_sparse_residency_copy){.source=values,.destination=view->targets[storage],
            .destination_offset=first*stride,.size=count*stride};
         kk_sparse_residency(dev,values,true);
      }
   return true;
}

static bool
kk_standard_3d_publish(struct kk_device *dev,struct util_dynarray *copies,
   struct kk_standard_3d_image *s,uint64_t first,uint64_t count)
{
   if(!count)return true;
   uint64_t bytes=count*sizeof(struct kk_standard_3d_tile);
   mtl_buffer *staging=mtl_sparse_new_residency_buffer(dev->mtl_handle,bytes,false);
   if(!staging)return false;
   memcpy(mtl_get_contents(staging),s->tiles+first,bytes);
   struct mtl_sparse_residency_copy *copy=util_dynarray_grow(copies,struct mtl_sparse_residency_copy,1);
   if(!copy){mtl_release(staging);return false;}
   *copy=(struct mtl_sparse_residency_copy){.source=staging,.destination=s->table,
      .destination_offset=align64(sizeof(struct kk_standard_3d_table),32)+first*sizeof(struct kk_standard_3d_tile),.size=bytes};
   kk_sparse_residency(dev,staging,true);
   return kk_standard_3d_publish_targets(dev,copies,s,first,count);
}

/* One submission publishes each image once, from its final tile state. The
 * GPU copies of one submission are unordered, so separate publishes of
 * overlapping ranges (a 2D array interleaves each layer's tail between the
 * regular tiles of consecutive layers) could leave an earlier snapshot. */
static void
kk_standard_3d_mark_dirty(struct kk_standard_3d_image *s,uint64_t first,uint64_t last)
{
   if(first>=last)return;
   s->dirty_first=MIN2(s->dirty_first,first);s->dirty_last=MAX2(s->dirty_last,last);
}

static bool
kk_standard_3d_publish_dirty(struct kk_device *dev,struct util_dynarray *copies,
   struct kk_standard_3d_image *s)
{
   if(s->dirty_last<=s->dirty_first)return true;
   bool ok=kk_standard_3d_publish(dev,copies,s,s->dirty_first,s->dirty_last-s->dirty_first);
   s->dirty_first=UINT64_MAX;s->dirty_last=0;
   return ok;
}

static bool
kk_standard_3d_bind_tile(struct kk_queue *queue,struct kk_image *image,
   uint64_t tile,uint32_t level,const uint32_t origin[3],VkDeviceMemory memory,uint64_t offset)
{
   struct kk_standard_3d_image *s=image->sparse->standard_3d;
   bool add=memory!=VK_NULL_HANDLE,regular=level<image->sparse->tail_first_lod;
   /* Packed and regular cells share the actual bound memory page. Tail mip
    * offsets are decoded by the shader, never by an image-private texture. */
   struct kk_standard_3d_page *page=add?
      kk_standard_3d_physical_page(kk_queue_device(queue),memory,offset,
         image->sparse->granularity,&image->planes[0].layout):NULL;
   if(add&&!page)return false;
   if(!s->two_d&&(s->tiles[tile].resident!=0)!=add){
      bool ok=regular?kk_standard_3d_native_reference(queue,image,level,origin,add):
         kk_standard_3d_logical_tail_reference(queue,image,add);
      if(!ok)return false;
   }
   s->pages[tile]=page;
   s->tiles[tile]=(struct kk_standard_3d_tile){.target_id=add?1:0,
      .origin={origin[0],origin[1],origin[2]},.level=level,.resident=add};
   s->memory[tile]=memory;s->memory_offset[tile]=offset;
   return true;
}

static bool
kk_standard_3d_bind_images(struct kk_queue *queue,struct kk_sparse_texture_batch *batch,
   struct util_dynarray *copies,struct kk_image *image,uint32_t count,
   const VkSparseImageMemoryBind *binds)
{
   struct kk_standard_3d_image *s=image->sparse->standard_3d;
   const VkExtent3D *g=&image->sparse->granularity;
   uint64_t first=s->tile_count,last=0;
   kk_sparse_flush_textures(batch);
   for(unsigned i=0;i<count;i++){
      const VkSparseImageMemoryBind *b=&binds[i];uint32_t level=b->subresource.mipLevel;
      assert(level<image->sparse->tail_first_lod);
      VkExtent3D t=kk_sparse_level_tiles(image,level);
      uint32_t begin[3]={b->offset.x/g->width,b->offset.y/g->height,b->offset.z/g->depth};
      uint32_t dims[3]={DIV_ROUND_UP(b->extent.width,g->width),DIV_ROUND_UP(b->extent.height,g->height),DIV_ROUND_UP(b->extent.depth,g->depth)};
      uint64_t physical=0;
      for(uint32_t z=0;z<dims[2];z++)for(uint32_t y=0;y<dims[1];y++)for(uint32_t x=0;x<dims[0];x++,physical++){
         uint32_t origin[3]={(begin[0]+x)*g->width,(begin[1]+y)*g->height,(begin[2]+z)*g->depth};
         uint64_t tile=image->sparse->layer_pages*b->subresource.arrayLayer+
            image->sparse->level_page[level]+begin[0]+x+
            (uint64_t)t.width*(begin[1]+y+(uint64_t)t.height*(begin[2]+z));
         assert(tile<s->tile_count);
         if(!kk_standard_3d_bind_tile(queue,image,tile,level,origin,b->memory,
            b->memoryOffset+physical*KK_SPARSE_PAGE_SIZE))return false;
         first=MIN2(first,tile);last=MAX2(last,tile+1);
      }
   }
   kk_standard_3d_mark_dirty(s,first,last);
   return true;
}

static bool
kk_standard_3d_bind_opaque(struct kk_queue *queue,struct kk_sparse_texture_batch *batch,
   struct util_dynarray *copies,struct kk_image *image,uint32_t count,
   const VkSparseMemoryBind *binds)
{
   struct kk_standard_3d_image *s=image->sparse->standard_3d;
   uint64_t first=s->tile_count,last=0;
   kk_sparse_flush_textures(batch);
   for(unsigned i=0;i<count;i++){
      const VkSparseMemoryBind *b=&binds[i];
      uint64_t start=b->resourceOffset/KK_SPARSE_PAGE_SIZE,n=DIV_ROUND_UP(b->size,KK_SPARSE_PAGE_SIZE);
      assert(start+n<=s->tile_count);
      for(uint64_t j=0;j<n;j++){
         uint64_t tile=start+j,page=tile%image->sparse->layer_pages;
         uint32_t level=image->sparse->tail_first_lod,origin[3]={0};
         if(page<image->sparse->level_page[level]){
            level=0;
            while(level+1<image->sparse->tail_first_lod&&
               image->sparse->level_page[level+1]<=page)level++;
            VkExtent3D t=kk_sparse_level_tiles(image,level),g=image->sparse->granularity;
            uint64_t local=page-image->sparse->level_page[level];
            origin[0]=(local%t.width)*g.width;
            origin[1]=((local/t.width)%t.height)*g.height;
            origin[2]=(local/((uint64_t)t.width*t.height))*g.depth;
         }
         if(!kk_standard_3d_bind_tile(queue,image,tile,level,origin,b->memory,
            b->memoryOffset+j*KK_SPARSE_PAGE_SIZE))return false;
         first=MIN2(first,tile);last=MAX2(last,tile+1);
      }
   }
   kk_standard_3d_mark_dirty(s,first,last);
   return true;
}

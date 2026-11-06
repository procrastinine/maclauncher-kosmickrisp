// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import {apply as applySampling} from './standard-sparse-3d-sampling.mjs';
export const id = 'standard-sparse-3d';
export const sourceFiles = ['standard-sparse-3d-layout.h','standard-sparse-3d.h',
 'standard-sparse-3d-impl.h','standard-sparse-3d-nir.h',
 'standard-sparse-3d-physical.h','standard-sparse-3d-meta.h',
 'standard-sparse-3d-clear.h','standard-sparse-3d-blit.h'].map(s=>'kosmickrisp-patches/'+s).concat([
 'kosmickrisp-patches/standard-sparse-3d-sampling.mjs',
 'kosmickrisp-patches/standard-sparse-3d-sample.h']);
export function apply({edit,edits}) {
 for(const file of sourceFiles.filter(f=>f.endsWith('.h')&&!f.endsWith('-sample.h'))) {
  const from=file.slice(file.lastIndexOf('/')+1);
  edits.set('src/kosmickrisp/vulkan/'+from.replaceAll('-','_').replace('standard_', 'kk_standard_'),
   fs.readFileSync(new URL('./'+from,import.meta.url),'utf8'));
 }
 edit('src/kosmickrisp/vulkan/kk_physical_device.c',s=>s.replace(
  '      .sparseResidencyStandard3DBlockShape = false,',
  '      .sparseResidencyStandard3DBlockShape = pdev->info.placement_sparse,'));
 edit('src/kosmickrisp/vulkan/kk_sparse.h',s=>s.replace(
  'struct kk_sparse_image {','struct kk_standard_3d_image;\nstruct kk_sparse_image {\n   struct kk_standard_3d_image *standard_3d;')
  .replace('struct kk_device;',
   'struct kk_device_memory;\nstruct kk_device;\nvoid kk_standard_3d_memory_finish(struct kk_device *,struct kk_device_memory *);'));
 edit('src/kosmickrisp/vulkan/kk_sparse.h',s=>s.replace(
  'struct kk_queue;',
  'struct kk_image_view;\nstruct kk_view_layout;\nVkResult kk_standard_3d_raw_view(struct kk_device *,struct kk_image_view *,const struct kk_view_layout *,mtl_texture **);\nstruct kk_queue;'));
 edit('src/kosmickrisp/vulkan/kk_image.c',s=>s
  .replace('      } else {\n         image->vk.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;',
   '      } else if (!(image->vk.image_type == VK_IMAGE_TYPE_3D &&\n                   (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT))) {\n         image->vk.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;')
  .replace('   if (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)\n      return kk_sparse_image_init(dev, image, vk_image_usage_flags(pCreateInfo));',
   '   if (image->vk.image_type == VK_IMAGE_TYPE_3D &&\n       (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) &&\n       !(image->application_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))\n      image->planes[0].layout.usage &= ~MTL_TEXTURE_USAGE_RENDER_TARGET;\n   if (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)\n      return kk_sparse_image_init(dev, image, vk_image_usage_flags(pCreateInfo));'));
 edit('src/kosmickrisp/vulkan/kk_image_view.c',s=>s.replace(
  '         uint64_t subres_offset_B =',  '         if (image->sparse && image->sparse->standard_3d) {\n            VkResult result=kk_standard_3d_raw_view(dev,view,&view_layout,&mtl_handle);\n            if(result!=VK_SUCCESS){kk_image_view_finish(dev,view);return result;}\n            view->planes[view_plane].mtl_handle_subres=mtl_handle;\n            view_layout.base_level=view_layout.base_array_layer=0;\n            view_layout.num_levels=view_layout.array_len=1;\n         } else {\n         uint64_t subres_offset_B =')
  .replace('         view_layout.array_len = subres_layout.layers;\n      } else if',
   '         view_layout.array_len = subres_layout.layers;\n         }\n      } else if'));
 edit('src/kosmickrisp/vulkan/kk_device_memory.h',s=>s.replace(
  '   struct kk_bo *bo;',
  '   struct kk_bo *bo;\n   struct kk_standard_3d_page *standard_3d;'));
 edit('src/kosmickrisp/vulkan/kk_device_memory.c',s=>s
  .replace('#include "kk_device_memory.h"','#include "kk_device_memory.h"\n#include "kk_sparse.h"')
  .replace('   kk_device_memory_set_priority(dev, mem, 0.0f);',
   '   kk_standard_3d_memory_finish(dev, mem);\n   kk_device_memory_set_priority(dev, mem, 0.0f);'));
 edit('src/kosmickrisp/vulkan/kk_sparse.h',s=>s.replace(
  'struct kk_queue;', `struct kk_queue;
struct kk_cmd_buffer;
bool kk_standard_3d_copy_buffer_to_image(struct kk_cmd_buffer *,const VkCopyBufferToImageInfo2 *);
bool kk_standard_3d_copy_image_to_buffer(struct kk_cmd_buffer *,const VkCopyImageToBufferInfo2 *);
bool kk_standard_3d_copy_image(struct kk_cmd_buffer *,const VkCopyImageInfo2 *);`));
 edit('src/kosmickrisp/vulkan/kk_sparse.h',s=>s.replace(
  'struct kk_cmd_buffer;',
  'struct kk_cmd_buffer;\nbool kk_standard_3d_clear(struct kk_cmd_buffer *,struct kk_image *,VkImageLayout,const VkClearColorValue *,uint32_t,const VkImageSubresourceRange *);'));
 edit('src/kosmickrisp/vulkan/kk_cmd_clear.c',s=>s
  .replace('#include "kk_image.h"','#include "kk_image.h"\n#include "kk_sparse.h"')
  .replace('   VkClearValue clear_value = {\n      .color = *pColor,',
   '   if (kk_standard_3d_clear(cmd, image, imageLayout, pColor, rangeCount, pRanges)) return;\n\n   VkClearValue clear_value = {\n      .color = *pColor,'));
 edit('src/kosmickrisp/vulkan/kk_cmd_meta.c',s=>s.replace(
  '#include "kk-writable-msaa-meta.h"',
  '#include "kk-writable-msaa-meta.h"\n#include "kk_standard_sparse_3d_meta.h"'));
 edit('src/kosmickrisp/vulkan/kk_cmd_copy.c',s=>s
  .replace('#include "kk_bo.h"','#include "kk_bo.h"\n#include "kk_sparse.h"')
  .replace('   VK_FROM_HANDLE(kk_image, image, pCopyBufferToImageInfo->dstImage);',
   '   VK_FROM_HANDLE(kk_image, image, pCopyBufferToImageInfo->dstImage);\n   if (kk_standard_3d_copy_buffer_to_image(cmd, pCopyBufferToImageInfo)) return;')
  .replace('   VK_FROM_HANDLE(kk_buffer, buffer, pCopyImageToBufferInfo->dstBuffer);',
   '   VK_FROM_HANDLE(kk_buffer, buffer, pCopyImageToBufferInfo->dstBuffer);\n   if (kk_standard_3d_copy_image_to_buffer(cmd, pCopyImageToBufferInfo)) return;')
  .replace('   if (kk_writable_msaa_copy_image(cmd, pCopyImageInfo))',
   '   if (kk_standard_3d_copy_image(cmd, pCopyImageInfo)) return;\n   if (kk_writable_msaa_copy_image(cmd, pCopyImageInfo))'));
 edit('src/kosmickrisp/vulkan/kk_sparse.c',s=>s
  .replace('#include "util/u_memory.h"','#include "util/u_memory.h"\n#include "kk_standard_sparse_3d.h"')
  .replace('   unsigned bytes = util_format_get_blocksize(pipe);',`   if (type == VK_IMAGE_TYPE_3D) {
      VkExtent3D shape = kk_standard_3d_shape(pipe);
      if (shape.width) {
         *props = (VkSparseImageFormatProperties){
            .aspectMask = vk_format_aspects(format), .imageGranularity = shape,
         };
         return true;
      }
   }
   unsigned bytes = util_format_get_blocksize(pipe);`)
  .replace('   image->sparse->granularity = props.imageGranularity;\n   image->sparse->aspect_mask = props.aspectMask;\n   image->sparse->format_flags = props.flags;',`   image->sparse->aspect_mask = props.aspectMask;
   image->sparse->format_flags = props.flags;
   if (image->vk.image_type == VK_IMAGE_TYPE_3D &&
       (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT)) {
      VkResult result = kk_standard_3d_image_initialize(dev, image);
      if (result != VK_SUCCESS) {
         kk_standard_3d_image_finish(dev, image);
         mtl_release(plane->mtl_handle); plane->mtl_handle = NULL;
         free(image->sparse); image->sparse = NULL;
         return result;
      }
      kk_sparse_residency(dev, plane->mtl_handle, true);
      p_atomic_set(&dev->sparse_residency_used, 1);
      return VK_SUCCESS;
   }
   /* Opaque binding-only images retain their native granularity. */
   if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
      struct mtl_size native = mtl_sparse_granularity(dev->mtl_handle,
         plane->layout.format.mtl, MTL_TEXTURE_TYPE_3D);
      props.imageGranularity = (VkExtent3D){native.x, native.y, native.z};
   }
   image->sparse->granularity = props.imageGranularity;`)
  .replace('   free(image->sparse); image->sparse = NULL;\n}',
   '   kk_standard_3d_image_finish(dev, image);\n   free(image->sparse); image->sparse = NULL;\n}')
  .replace('       (storage && !view->vk.base_mip_level))',
   '       (storage && !view->vk.base_mip_level && !image->sparse->standard_3d))')
  .replace('         2 * sizeof(struct kk_sparse_query_record), false);',
   '         kk_standard_3d_query_bytes(image), false);')
  .replace('   struct kk_sparse_query_record *records = mtl_get_contents(view->sparse->records);',
   `   struct kk_sparse_query_record *record = (void *)((char *)
      mtl_get_contents(view->sparse->records) + storage * kk_standard_3d_query_stride(view));`)
  .replaceAll('records[storage]', '(*record)')
  .replace('   query_layout.num_levels = image->vk.mip_levels;',
   '   bool logical_raw=image->sparse->standard_3d && util_format_is_compressed(image->planes[0].layout.format.pipe) && !util_format_is_compressed(layout->format.pipe);\n   query_layout.num_levels = logical_raw ? view->vk.level_count : image->vk.mip_levels;')
  .replace('         unsigned view_size = MAX2(extent >> view->vk.base_mip_level, 1);',
   '         unsigned view_size = logical_raw ? (i==0?view->vk.extent.width:i==1?view->vk.extent.height:view->vk.extent.depth) : MAX2(extent >> view->vk.base_mip_level, 1);')
  .replace('   if (storage) {\n      /* The sliced-3D lowering',
   '   VkResult logical = kk_standard_3d_query_initialize(view, storage, layout);\n   if (logical != VK_SUCCESS) return logical;\n   if (storage) {\n      /* The sliced-3D lowering')
  .replace('struct kk_sparse_view_state {',
   'struct kk_sparse_view_state {\n   struct kk_standard_3d_view_state *standard_3d;')
  .replace('   for (unsigned i = 0; i < 2; ++i) {\n      mtl_release(view->sparse->query[i]);',
   '   kk_standard_3d_query_finish(dev, view);\n   for (unsigned i = 0; i < 2; ++i) {\n      mtl_release(view->sparse->query[i]);')
  .replace('      (struct kk_sparse_query_record *)mtl_get_contents(view->sparse->records) + storage;',
   '      (void *)((char *)mtl_get_contents(view->sparse->records) + storage * kk_standard_3d_query_stride(view));')
  .replace('      (struct kk_sparse_query_record *)mtl_get_contents(view->sparse->records) + 1;',
   '      (void *)((char *)mtl_get_contents(view->sparse->records) + kk_standard_3d_query_stride(view));')
  .replace('      storage * sizeof(struct kk_sparse_query_record) : 0;',
   '      storage * kk_standard_3d_query_stride(view) : 0;')
  .replace('      (char *)mtl_get_contents(view->sparse->records) + sizeof(*record));',
   '      (char *)mtl_get_contents(view->sparse->records) + kk_standard_3d_query_stride(view));')
  .replace('static bool\nkk_sparse_opaque_bind(',
   '#include "kk_standard_sparse_3d_impl.h"\n\nstatic bool\nkk_sparse_opaque_bind(')
  .replace('      for (unsigned j = 0; j < info->bindCount; ++j)\n         if (!kk_sparse_opaque_bind',
   `      if (image->sparse->standard_3d) {
         simple_mtx_lock(&image->sparse->standard_3d->mutex);
         bool ok = kk_standard_3d_bind_opaque(queue, &batch, &copies, image, info->bindCount, info->pBinds);
         simple_mtx_unlock(&image->sparse->standard_3d->mutex);
         if (!ok) {
            result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY); goto finish;
         }
         continue;
      }
      for (unsigned j = 0; j < info->bindCount; ++j)
         if (!kk_sparse_opaque_bind`)
  .replace('      const VkExtent3D *g = &image->sparse->granularity;\n      for (unsigned j',
   `      if (image->sparse->standard_3d) {
         simple_mtx_lock(&image->sparse->standard_3d->mutex);
         bool ok = kk_standard_3d_bind_images(queue, &batch, &copies, image, info->bindCount, info->pBinds);
         simple_mtx_unlock(&image->sparse->standard_3d->mutex);
         if (!ok) {
            result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY); goto finish;
         }
         continue;
      }
      const VkExtent3D *g = &image->sparse->granularity;
      for (unsigned j`)
  .replace('   return dev->vk.enabled_features.sparseResidencyBuffer &&',
   '   return (dev->vk.enabled_features.sparseResidencyBuffer ||\n           dev->vk.enabled_features.sparseResidencyImage3D) &&'));
 edit('src/kosmickrisp/vulkan/kk_shader.c',s=>s.replace(
  'if (feats->sparseResidencyBuffer) key |= BITFIELD_BIT(KK_FEAT_SPARSE_REPLAY);',
  'if (feats->sparseResidencyBuffer || feats->sparseResidencyImage3D) key |= BITFIELD_BIT(KK_FEAT_SPARSE_REPLAY);'));
 edit('src/kosmickrisp/vulkan/kk_sparse_pipeline.h',s=>s.replace(
  'if (features->sparseResidencyBuffer) {',
  'if (features->sparseResidencyBuffer || features->sparseResidencyImage3D) {'));
 edit('src/kosmickrisp/vulkan/kk_shader.h',s=>s.replace(
  'kk_nir_lower_descriptors(nir_shader *nir,',
  'kk_nir_lower_descriptors(nir_shader *nir, bool standard_3d,'));
 edit('src/kosmickrisp/vulkan/kk_shader.c',s=>s
  .replace('NIR_PASS(_, nir, kk_nir_lower_descriptors, rs, set_layout_count,',
   'NIR_PASS(_, nir, kk_nir_lower_descriptors, (features & BITFIELD_BIT(KK_FEAT_SPARSE_BUFFER)) != 0, rs, set_layout_count,')
  .replaceAll('kk_nir_lower_descriptors, robustness, 0, NULL)',
   'kk_nir_lower_descriptors, false, robustness, 0, NULL)')
  .replace('kk_nir_lower_descriptors, vertex_robustness, 0, NULL)',
   'kk_nir_lower_descriptors, false, vertex_robustness, 0, NULL)'));
 edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c',s=>s
  .replace('   bool robust_msaa_defaults;', '   bool standard_3d;\n   bool robust_msaa_defaults;')
  .replace('kk_nir_lower_descriptors(nir_shader *nir,',
   'kk_nir_lower_descriptors(nir_shader *nir, bool standard_3d,')
  .replace('   struct lower_descriptors_ctx ctx = kk_descriptor_context(rs, set_layout_count, set_layouts);\n\n   /* First lower everything',
   '   struct lower_descriptors_ctx ctx = kk_descriptor_context(rs, set_layout_count, set_layouts);\n   ctx.standard_3d = standard_3d;\n\n   /* First lower everything')
  .replace('static bool\nlower_image_intrin(',
   '#include "kk_standard_sparse_3d_nir.h"\n\nstatic bool\nlower_image_intrin(')
  .replace('   if (nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_BUF &&\n       (intr->intrinsic == nir_intrinsic_bindless_image_load',
   '   if (kk_standard_3d_lower_image(b, intr, deref, type, resource_addr, ctx)) return true;\n\n   if (nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_BUF &&\n       (intr->intrinsic == nir_intrinsic_bindless_image_load')
  .replace('   if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF && tex->op == nir_texop_txf) {',
   '   if (kk_standard_3d_lower_fetch(b, tex, texture, plane_offset_B, standard_resource_address, ctx)) return true;\n\n   if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF && tex->op == nir_texop_txf) {')
  .replace('   {\n      unsigned offs = (cube_face)',
   '   nir_def *standard_resource_address = NULL;\n   {\n      unsigned offs = (cube_face)')
  .replace('      nir_tex_instr_add_src(tex, nir_tex_src_texture_handle, handle);',
   '      nir_tex_instr_add_src(tex, nir_tex_src_texture_handle, handle);\n      standard_resource_address = resource_addr;'));
 edit('src/vulkan/runtime/vk_meta.h',s=>s.replace('struct vk_image;',
  'struct vk_image;\nstruct nir_shader;\nstruct vk_meta_device;\nstruct nir_shader *vk_meta_build_color_blit_shader(VkImageType,VkFormat);\nVkResult vk_meta_get_color_blit_sampler(struct vk_device *,struct vk_meta_device *,VkFilter,VkSampler *);'));
 edit('src/vulkan/runtime/vk_meta_blit_resolve.c',s=>s
  .replace('static VkResult\nget_blit_pipeline_layout(',   'nir_shader *\nvk_meta_build_color_blit_shader(VkImageType type,VkFormat format)\n{\n   const struct vk_meta_blit_key key={.dim=type==VK_IMAGE_TYPE_3D?GLSL_SAMPLER_DIM_3D:type==VK_IMAGE_TYPE_1D?GLSL_SAMPLER_DIM_1D:GLSL_SAMPLER_DIM_2D,\n      .src_samples=VK_SAMPLE_COUNT_1_BIT,.dst_format=format,.aspects=VK_IMAGE_ASPECT_COLOR_BIT};\n   return build_blit_shader(&key);\n}\n\nstatic VkResult\nget_blit_pipeline_layout(')
  .replace('static void\ndo_blit(',   'VkResult\nvk_meta_get_color_blit_sampler(struct vk_device *device,struct vk_meta_device *meta,VkFilter filter,VkSampler *sampler)\n{\n   return get_blit_sampler(device,meta,filter,sampler);\n}\n\nstatic void\ndo_blit('));
 edit('src/kosmickrisp/vulkan/kk_sparse.h',s=>s.replace('struct kk_cmd_buffer;',
  'struct kk_cmd_buffer;\nbool kk_standard_3d_blit(struct kk_cmd_buffer *,const VkBlitImageInfo2 *);'));
 edit('src/kosmickrisp/vulkan/kk_cmd_meta.c',s=>s.replace(
  '   struct kk_meta_save save;\n   kk_meta_begin(cmd, &save, VK_PIPELINE_BIND_POINT_GRAPHICS,\n                 "meta:vkCmdBlitImage2");',
  '   if (kk_standard_3d_blit(cmd,pBlitImageInfo))return;\n   struct kk_meta_save save;\n   kk_meta_begin(cmd, &save, VK_PIPELINE_BIND_POINT_GRAPHICS,\n                 "meta:vkCmdBlitImage2");'));
 // Keep one sparse color/status writer. Logical per-tap predicates enter this
 // existing lowering directly, without reserving sampler backend arguments.
 edit('src/kosmickrisp/vulkan/kk_shader.h',s=>s.replace(
  'bool kk_nir_lower_sparse(nir_shader *nir);',
  'bool kk_nir_lower_sparse(nir_shader *nir);\nbool kk_nir_lower_sparse_instruction(nir_builder *b, nir_instr *instr,\n   nir_def *logical_resident, nir_def *zero_one_mask);'));
 edit('src/kosmickrisp/vulkan/kk_nir_lower_sparse.c',s=>s
  .replace('static bool\nlower_sparse(nir_builder *b, nir_instr *instr, UNUSED void *data)',
   'bool\nkk_nir_lower_sparse_instruction(nir_builder *b, nir_instr *instr,\n   nir_def *logical_resident, nir_def *zero_one_mask)')
  .replace('   unsigned colors = --def->num_components;\n   nir_def *parts[5];\n   for (unsigned i = 0; i < colors; ++i)\n      parts[i] = nir_channel(b, def, i);',
   `   unsigned colors = --def->num_components;
   nir_def *parts[5];
   nir_alu_type type = instr->type == nir_instr_type_tex ?
      nir_instr_as_tex(instr)->dest_type :
      nir_intrinsic_dest_type(nir_instr_as_intrinsic(instr));
   for (unsigned i = 0; i < colors; ++i) {
      parts[i] = nir_channel(b, def, i);
      if (logical_resident) {
         nir_def *one = nir_alu_type_get_base_type(type) == nir_type_float ?
            nir_imm_floatN_t(b, 1, def->bit_size) :
            nir_imm_intN_t(b, 1, def->bit_size);
         nir_def *zero = nir_imm_intN_t(b, 0, def->bit_size);
         nir_def *unmapped = nir_bcsel(b,
            nir_test_mask(b, zero_one_mask, 1u << i), one, zero);
         parts[i] = nir_bcsel(b, logical_resident, parts[i], unmapped);
      }
   }`)
  .replace('   if (!resident) resident = nir_is_sparse_resident_zink(b, def);',
   '   if (!resident) resident = nir_is_sparse_resident_zink(b, def);\n   if (logical_resident) resident = nir_iand(b, resident, logical_resident);')
  .replace('bool\nkk_nir_lower_sparse(nir_shader *nir)',
   `static bool
lower_sparse(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   return kk_nir_lower_sparse_instruction(b, instr, NULL, NULL);
}

bool
kk_nir_lower_sparse(nir_shader *nir)`));
 applySampling({edit,edits});
}

// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import {apply as applyNativeSamplers} from './descriptor-heap-samplers.mjs';
export const id = 'descriptor-heap';
export const sourceFiles = [
  'kosmickrisp-patches/descriptor-heap-samplers.mjs',
  'kosmickrisp-patches/descriptor-heap-nir.h',
  'kosmickrisp-patches/descriptor-heap-api.h',
];
export function apply({edit: editSource, edits}) {
  const edit = (file, apply) => editSource('src/kosmickrisp/vulkan/' + file, apply);
  for (const [from, to] of [['descriptor-heap-nir.h', 'kk_descriptor_heap_nir.h'],
                            ['descriptor-heap-api.h', 'kk_descriptor_heap_api.h']])
    edits.set('src/kosmickrisp/vulkan/' + to,
      fs.readFileSync(new URL('./' + from, import.meta.url), 'utf8'));
  edit('kk_cmd_buffer.h', s => s
    .replace('   uint64_t sets[KK_MAX_SETS];', `   union {
      uint64_t sets[KK_MAX_SETS];
      struct { uint64_t heap_resource, heap_sampler; };
   };`)
    .replace('   struct kk_root_descriptor_table root;',
      '   struct kk_root_descriptor_table root;\n   uint64_t heap_resource, heap_sampler;\n   bool descriptor_heap;'));
  edit('kk_cmd_buffer.h', s => s
    .replace('#include <stdio.h>', '#include <stdio.h>\n#include <string.h>')
    .replace('   struct kk_push_descriptor_set *push[KK_MAX_SETS];\n};',
      `   struct kk_push_descriptor_set *push[KK_MAX_SETS];
};

static inline void
kk_descriptor_push_update(struct kk_descriptor_state *desc, uint32_t offset,
                           const void *data, size_t size)
{
   assert(offset <= KK_MAX_PUSH_SIZE && size <= KK_MAX_PUSH_SIZE - offset);
   if (size && memcmp(desc->root.push + offset, data, size)) {
      memcpy(desc->root.push + offset, data, size);
      desc->root_dirty = true;
   }
}`));
  edit('kk_cmd_buffer.c', s => s.replace(
    '   memcpy(desc->root.push + info->offset, info->pValues, info->size);\n   desc->root_dirty = true;',
    '   kk_descriptor_push_update(desc, info->offset, info->pValues, info->size);'));
  edit('kk_cmd_buffer.h', s => s.replace(
    '   struct kk_push_descriptor_set *push[KK_MAX_SETS];\n};',
    `   struct kk_push_descriptor_set *push[KK_MAX_SETS];
};

/* Set bindings stay in CPU state. Heap aliases belong only to a snapshot. */
static inline void
kk_descriptor_root_apply_heap(const struct kk_descriptor_state *desc,
                              struct kk_root_descriptor_table *snapshot)
{
   if (desc->descriptor_heap) {
      snapshot->heap_resource = desc->heap_resource;
      snapshot->heap_sampler = desc->heap_sampler;
   }
}`));
  edit('kk_cmd_buffer.c', s => s
    .replace(`uint64_t
kk_upload_descriptor_root_data(struct kk_cmd_buffer *cmd,
   const struct kk_root_descriptor_table *root, uint32_t size)
{
   struct kk_ptr ptr = kk_pool_upload(cmd, root, size, 8u);
   if (unlikely(!ptr.gpu)) return 0u;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   ((struct kk_root_descriptor_table *)ptr.cpu)->addr =
      dev->atomic64_locks ? dev->atomic64_locks->gpu : ptr.gpu;
   return ptr.gpu;
}`, `static struct kk_ptr
kk_upload_descriptor_root_ptr(struct kk_cmd_buffer *cmd,
   const struct kk_root_descriptor_table *root, uint32_t size)
{
   struct kk_ptr ptr = kk_pool_upload(cmd, root, size, 8u);
   if (unlikely(!ptr.gpu)) return (struct kk_ptr){0};
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   ((struct kk_root_descriptor_table *)ptr.cpu)->addr =
      dev->atomic64_locks ? dev->atomic64_locks->gpu : ptr.gpu;
   return ptr;
}

uint64_t
kk_upload_descriptor_root_data(struct kk_cmd_buffer *cmd,
   const struct kk_root_descriptor_table *root, uint32_t size)
{
   return kk_upload_descriptor_root_ptr(cmd, root, size).gpu;
}`)
    .replace(`   uint64_t address = kk_upload_descriptor_root_data(cmd, root, size);
   if (!address) return 0u;
   root->addr = address;
   desc->root_dirty = false;
   return address;`, `   struct kk_ptr ptr = kk_upload_descriptor_root_ptr(cmd, root, size);
   if (!ptr.gpu) return 0u;
   kk_descriptor_root_apply_heap(desc, ptr.cpu);
   root->addr = ptr.gpu;
   desc->root_dirty = false;
   return ptr.gpu;`));
  edit('kk_cmd_dgc.c', s => s.replace(
    '   p.base_root = kk_upload_descriptor_root_data(cmd, &root, sizeof(root));',
    `   kk_descriptor_root_apply_heap(&descriptors, &root);
   p.base_root = kk_upload_descriptor_root_data(cmd, &root, sizeof(root));`));
  edit('kk_shader.h', s => s.replace('   uint32_t push_size;',
    '   uint32_t push_size;\n   bool descriptor_heap;'));
  edit('kk_shader.c', s => s
    .replace('   for (uint32_t i = 0; i < total_shaders; ++i)\n      shaders[i]->info.push_size = push_size;',
      '   for (uint32_t i = 0; i < total_shaders; ++i) {\n      shaders[i]->info.push_size = push_size;\n      shaders[i]->info.descriptor_heap = infos[0].flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;\n   }')
    .replace('   desc->root_dirty |= push_size > desc->push_size;',
      '   bool heap = shader && shader->info.descriptor_heap;\n   desc->root_dirty |= heap != desc->descriptor_heap || push_size > desc->push_size;\n   desc->descriptor_heap = heap;')
    .replace('   desc->root_dirty |= shader->info.push_size > desc->push_size;',
      '   desc->root_dirty |= shader->info.descriptor_heap != desc->descriptor_heap || shader->info.push_size > desc->push_size;\n   desc->descriptor_heap = shader->info.descriptor_heap;'));

  edit('kk_device.h', s => s
    .replace('struct kk_bo;', 'struct kk_heap_sampler;\nstruct kk_heap_metadata_page;\nstruct kk_heap_border;\nstruct kk_buffer;\nstruct kk_bo;')
    .replace('   struct vk_device vk;', `   struct vk_device vk;
   simple_mtx_t descriptor_heap_lock;
   struct kk_heap_sampler *heap_samplers;
   struct kk_heap_metadata_page *heap_metadata_pages;
   struct kk_heap_border *heap_borders;
   struct kk_buffer *heap_buffers;`));
  edit('kk_image.h', s => s.replace('struct kk_image {', 'struct kk_heap_image_view;\n\nstruct kk_image {').replace('   struct vk_image vk;', '   struct vk_image vk;\n   struct kk_heap_image_view *heap_views;'));
  edit('kk_buffer.h', s => s.replace('struct kk_buffer {', 'struct kk_heap_buffer_view;\n\nstruct kk_buffer {').replace('   struct vk_buffer vk;', '   struct vk_buffer vk;\n   struct kk_heap_buffer_view *heap_views;\n   struct kk_buffer *heap_next;\n   bool heap_registered;'));
  edit('kk_descriptor_set.h', s => s.replace('#endif', `struct vk_sampler_state;
struct kk_image;
struct kk_buffer;
void kk_descriptor_heap_init(struct kk_device *dev);
void kk_descriptor_heap_finish(struct kk_device *dev);
void kk_descriptor_heap_image_finish(struct kk_device *dev, struct kk_image *image);
void kk_descriptor_heap_buffer_bind(struct kk_device *dev, struct kk_buffer *buffer);
void kk_descriptor_heap_buffer_finish(struct kk_device *dev, struct kk_buffer *buffer);
VkResult kk_heap_sampler_state(struct kk_device *dev,
   const struct vk_sampler_state *state, uint64_t *gpu_address);

#endif`));
  edit('kk_buffer.h', s => s
    .replace('#include "vk_buffer.h"', '#include "vk_buffer.h"\n#include "kk_descriptor_types.h"')
    .replace('static inline uint64_t\nkk_buffer_mtl_offset', `/* One bounded-buffer representation serves sets, dynamic sets and heaps.
 * The final word is reserved for the shared residency record; zero denotes
 * an ordinary or null buffer. */
static inline struct kk_buffer_address
kk_buffer_descriptor(const struct kk_buffer *buffer, struct kk_addr_range addr_range)
{
   (void)buffer;
   assert(addr_range.range <= UINT32_MAX);
   return (struct kk_buffer_address){
      .base_addr = addr_range.addr,
      .size = addr_range.range,
      .zero = 0,
   };
}

static inline uint64_t
kk_buffer_mtl_offset`));
  edit('kk_descriptor_set.c', s => s
    .replace(`   const struct kk_buffer_address desc = {
      .base_addr = addr_range.addr,
      .size = addr_range.range,
   };`, '   const struct kk_buffer_address desc = kk_buffer_descriptor(buffer, addr_range);')
    .replace(`   *desc = (struct kk_buffer_address){
      .base_addr = addr_range.addr,
      .size = addr_range.range,
   };`, '   *desc = kk_buffer_descriptor(buffer, addr_range);'));
  edit('kk_descriptor_set.c', s => s.replace(
    `         desc[plane].image_min_lod_uint16 = min_lod;
      }
   }

   if (descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER ||`,
    `         desc[plane].image_min_lod_uint16 = min_lod;
      }
      if (view->plane_count > 1) {
         /* Multi-planar images own both auxiliary words. Cube and sparse
          * images retain their words in the single-plane path. */
         assert(!(view->vk.image->create_flags &
                  (VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT |
                   VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                   VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                   VK_IMAGE_CREATE_SPARSE_ALIASED_BIT)));
         assert(desc[0].aux_resource_id == 0 && desc[0].aux_word == 0);
         desc[0].aux_resource_id = desc[1].image_gpu_resource_id;
         if (view->plane_count > 2)
            desc[0].aux_word = desc[2].image_gpu_resource_id;
      }
   }

   if (descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER ||`));
  edit('kk_descriptor_set.c', s => s + '\n#include "kk_descriptor_heap_api.h"\n');
  edit('kk_device.c', s => s
    .replace('#include "kk_device.h"', '#include "kk_device.h"\n#include "kk_descriptor_set.h"')
    .replace('   dev->vk.shader_ops = &kk_device_shader_ops;',
      '   kk_descriptor_heap_init(dev);\n   dev->vk.shader_ops = &kk_device_shader_ops;')
    .replace('fail_init:\n', 'fail_init:\n   kk_descriptor_heap_finish(dev);\n')
    .replace('   kk_query_table_finish(dev, &dev->occlusion_queries);\n   kk_destroy_sampler_heap',
      '   kk_query_table_finish(dev, &dev->occlusion_queries);\n   kk_descriptor_heap_finish(dev);\n   kk_destroy_sampler_heap'));
  edit('kk_image.c', s => s
    .replace('#include "kk_image.h"', '#include "kk_image.h"\n#include "kk_descriptor_set.h"')
    .replace('   for (uint8_t plane = 0; plane < image->plane_count; plane++) {\n      kk_image_plane_finish',
      '   kk_descriptor_heap_image_finish(dev, image);\n   for (uint8_t plane = 0; plane < image->plane_count; plane++) {\n      kk_image_plane_finish'));
  edit('kk_image.c', s => s.replace(
    `   const struct vk_format_ycbcr_info *ycbcr_info =
      vk_format_get_ycbcr_info(pImageFormatInfo->format);`,
    `   const struct vk_format_ycbcr_info *ycbcr_info =
      vk_format_get_ycbcr_info(pImageFormatInfo->format);
   if (ycbcr_info && ycbcr_info->n_planes > 1 &&
       (pImageFormatInfo->flags & (VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT |
                                  VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                                  VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                                  VK_IMAGE_CREATE_SPARSE_ALIASED_BIT)))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;`));
  edit('kk_buffer.c', s => s
    .replace('#include "kk_buffer.h"', '#include "kk_buffer.h"\n#include "kk_descriptor_set.h"')
    .replace('   *pBuffer = kk_buffer_to_handle(buffer);', '   kk_descriptor_heap_buffer_bind(dev, buffer);\n   *pBuffer = kk_buffer_to_handle(buffer);')
    .replace('   if (buffer->metal.handle)\n      mtl_release',
      '   kk_descriptor_heap_buffer_finish(dev, buffer);\n   if (buffer->metal.handle)\n      mtl_release')
    .replace('   mtl_get_contents(buffer->metal.handle);',
      '   mtl_get_contents(buffer->metal.handle);\n   kk_descriptor_heap_buffer_bind(dev, buffer);'));
  edit('kk_cmd_buffer.c', s => s.replace('   mtl_begin_command_buffer(cmd->metal.cmd_buf, cmd->metal.allocator);', `   mtl_begin_command_buffer(cmd->metal.cmd_buf, cmd->metal.allocator);
   if (cmd->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY && pBeginInfo->pInheritanceInfo) {
      const VkCommandBufferInheritanceDescriptorHeapInfoEXT *heap = vk_find_struct_const(
         pBeginInfo->pInheritanceInfo->pNext, COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT);
      if (heap) {
         if (heap->pSamplerHeapBindInfo)
            vk_enqueue_cmd_bind_sampler_heap_ext(&cmd->vk.cmd_queue, heap->pSamplerHeapBindInfo);
         if (heap->pResourceHeapBindInfo)
            vk_enqueue_cmd_bind_resource_heap_ext(&cmd->vk.cmd_queue, heap->pResourceHeapBindInfo);
      }
   }`));
  edit('kk_nir_lower_descriptors.c', s => s
    .replace('   uint32_t set, binding;\n   nir_def *index;\n   get_resource_deref_binding(b, deref, &set, &binding, &index);\n\n   const struct kk_descriptor_set_binding_layout *binding_layout =',
      `   if (deref->deref_type == nir_deref_type_cast)
      return nir_iadd_imm(b, deref->parent.ssa, offset_B);
   uint32_t set, binding;
   nir_def *index;
   get_resource_deref_binding(b, deref, &set, &binding, &index);

   const struct kk_descriptor_set_binding_layout *binding_layout =`)
    .replace('   uint32_t set, binding;\n   nir_def *index;\n   get_resource_deref_binding(b, deref, &set, &binding, &index);\n   return load_descriptor',
      `   if (deref->deref_type == nir_deref_type_cast)
      return nir_load_global_constant(b, num_components, bit_size,
         nir_iadd_imm(b, deref->parent.ssa, offset_B),
         .align_mul = MIN2(bit_size / 8, 4));
   uint32_t set, binding;
   nir_def *index;
   get_resource_deref_binding(b, deref, &set, &binding, &index);
   return load_descriptor`)
    .replace('   nir_variable *var = nir_deref_instr_get_variable(deref);\n   nir_def *handle =',
      '   nir_variable *var = nir_deref_instr_get_variable(deref);\n   enum gl_access_qualifier access = var ? var->data.access : nir_intrinsic_access(intr);\n   nir_def *handle =')
    .replace('.access = nir_intrinsic_access(intr) | var->data.access,',
      '.access = nir_intrinsic_access(intr) | access,')
    .replace('.flags = msl_convert_access_flag(var->data.access));',
      '.flags = msl_convert_access_flag(access));')
    .replace('   nir_rewrite_image_intrinsic(intr, handle, nir_image_intrinsic_type_bindless);', '   if (!var) nir_src_rewrite(&intr->src[0], nir_imm_int(b, 0));\n   nir_rewrite_image_intrinsic(intr, handle, nir_image_intrinsic_type_bindless);')
    .replace('static bool\nlower_poly(',
      '#include "kk_descriptor_heap_nir.h"\n\nstatic bool\nlower_poly('));
  edit('kk_nir_lower_descriptors.c', s => s
    .replace('      plane * sizeof(struct kk_sampled_image_descriptor);', `      plane * sizeof(struct kk_sampled_image_descriptor);
   bool heap_image = nir_src_as_deref(nir_src_for_ssa(texture))->deref_type == nir_deref_type_cast;
   uint64_t image_plane_offset_B = heap_image ? 0 : plane_offset_B;
   uint64_t image_id_offset_B = heap_image && plane
      ? (plane == 1 ? offsetof(struct kk_sampled_image_descriptor, aux_resource_id)
                    : offsetof(struct kk_sampled_image_descriptor, aux_word))
      : plane_offset_B;`)
    .replace('b, 1, 16, nir_src_as_deref(nir_src_for_ssa(texture)),\n         plane_offset_B + offs, ctx);',
      'b, 1, 16, nir_src_as_deref(nir_src_for_ssa(texture)),\n         image_plane_offset_B + offs, ctx);')
    .replace('b, 1, 64, nir_src_as_deref(nir_src_for_ssa(texture)),\n         plane_offset_B + offs, ctx);',
      'b, 1, 64, nir_src_as_deref(nir_src_for_ssa(texture)),\n         image_id_offset_B + offs, ctx);'));
  edit('kk_sampler_minmax.h', s => s.replace(
    `   const struct kk_descriptor_set_binding_layout *binding =
      get_binding_layout(var->data.descriptor_set, var->data.binding, ctx);
   if (binding->immutable_samplers) {`,
    `   const struct kk_descriptor_set_binding_layout *binding = var ?
      get_binding_layout(var->data.descriptor_set, var->data.binding, ctx) : NULL;
   if (binding && binding->immutable_samplers) {`));
  edit('kk-sampler-helpers-nir.h', s => s
    .replace('   if (!binding->immutable_samplers)',
      '   if (!binding || !binding->immutable_samplers)')
    .replace(`   const struct kk_descriptor_set_binding_layout *binding = get_binding_layout(
      var->data.descriptor_set, var->data.binding, &ctx->descriptors);`,
      `   const struct kk_descriptor_set_binding_layout *binding = var ? get_binding_layout(
      var->data.descriptor_set, var->data.binding, &ctx->descriptors) : NULL;`)
    .replace('   if (!known && binding->immutable_samplers && binding->array_size) {',
      '   if (!known && binding && binding->immutable_samplers && binding->array_size) {'));
  edit('kk_shader.h', s => s.replace('bool\nkk_nir_lower_descriptors(',
    'bool kk_nir_prepare_descriptor_heaps(nir_shader *nir, uint32_t embedded_count, const uint64_t *embedded);\n\nbool\nkk_nir_lower_descriptors('));
  edit('kk_shader.c', s => s
    .replace('   nir_shader *nir_shaders[shader_count + 2u];', '   uint32_t heap_push_size = 0;\n   nir_shader *nir_shaders[shader_count + 2u];')
    .replace('      nir_shader *nir = info->nir;', `      nir_shader *nir = info->nir;
      if (info->flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT) {
         nir_foreach_variable_with_modes(var, nir, nir_var_mem_push_const)
            heap_push_size = MAX2(heap_push_size, MIN2(glsl_get_explicit_size(var->type, false), KK_MAX_PUSH_SIZE));
         nir_foreach_function_impl(impl, nir) {
            nir_foreach_block(block, impl) {
               nir_foreach_instr(instr, block) {
                  if (instr->type != nir_instr_type_intrinsic) continue;
                  nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
                  if (load->intrinsic != nir_intrinsic_load_push_constant) continue;
                  uint32_t end = nir_intrinsic_base(load) + nir_intrinsic_range(load);
                  if (nir_src_is_const(load->src[0]))
                     end = MAX2(end, nir_intrinsic_base(load) + nir_src_as_uint(load->src[0]) +
                                load->def.num_components * load->def.bit_size / 8);
                  heap_push_size = MAX2(heap_push_size, MIN2(end, KK_MAX_PUSH_SIZE));
               }
            }
         }
      }`)
    .replace('   struct vk_descriptor_set_layout *const *set_layouts;\n};',
      '   struct vk_descriptor_set_layout *const *set_layouts;\n   const struct vk_shader_compile_info *info;\n};')
    .replace('   assert(set < state->set_layout_count);',
      `   if (set == VK_NIR_YCBCR_SET_IMMUTABLE_SAMPLERS) {
      assert(state->info && binding < state->info->embedded_sampler_count);
      const struct vk_sampler_state *sampler = &state->info->embedded_samplers[binding];
      return sampler->has_ycbcr_conversion ? &sampler->ycbcr_conversion : NULL;
   }
   assert(set < state->set_layout_count);`)
    .replace('             kk_feature_key features)\n{\n   struct kk_physical_device *pdev = kk_device_physical(dev);',
      '             kk_feature_key features,\n             const struct vk_shader_compile_info *compile_info,\n             const uint64_t *embedded)\n{\n   struct kk_physical_device *pdev = kk_device_physical(dev);')
    .replace('      .set_layouts = set_layouts,\n   };',
      '      .set_layouts = set_layouts,\n      .info = compile_info,\n   };')
    .replace('   /* Common msl texture lowering needs to happen after ycbcr lowering and',
      `   if (compile_info && (compile_info->flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT))
      NIR_PASS(_, nir, kk_nir_prepare_descriptor_heaps,
               compile_info->embedded_sampler_count, embedded);

   /* Common msl texture lowering needs to happen after ycbcr lowering and`)
    .replace('0u, NULL, state, features);', '0u, NULL, state, features, NULL, NULL);')
    .replace('      kk_lower_nir(dev, nir, emulated_stage, info->robustness,',
      `      uint64_t *embedded = info->embedded_sampler_count ? calloc(info->embedded_sampler_count, sizeof(uint64_t)) : NULL;
      if (info->embedded_sampler_count && !embedded)
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      for (uint32_t e = 0; e < info->embedded_sampler_count; ++e) {
         result = kk_heap_sampler_state(dev, &info->embedded_samplers[e], &embedded[e]);
         if (result != VK_SUCCESS) { free(embedded); return result; }
      }
      kk_lower_nir(dev, nir, emulated_stage, info->robustness,`)
    .replace('info->set_layout_count, info->set_layouts, state, features);',
      'info->set_layout_count, info->set_layouts, state, features, info, embedded);\n      free(embedded);')
    .replace('      for (uint32_t r = 0; r < info->push_constant_range_count; ++r) {',
      '      push_size = MAX2(push_size, heap_push_size);\n      for (uint32_t r = 0; r < info->push_constant_range_count; ++r) {'));
  edit('kk_sampler.c', s => s.replace('dev->vk.enabled_features.customBorderColors ||', 'dev->vk.enabled_features.customBorderColors || dev->vk.enabled_features.descriptorHeap ||'));
  edit('kk_shader.c', s => {
    const start = s.indexOf('   const struct lower_ycbcr_state ycbcr_state = {', s.indexOf('kk_lower_nir('));
    const end = s.indexOf('   /* Common msl texture lowering', start);
    if (start < 0 || end < 0) throw Error('missing heap preparation ordering anchors');
    const block = s.slice(start, end);
    return s.replace(block, '').replace('   /* Massage IO related variables to please Metal */', block + '   /* Massage IO related variables to please Metal */');
  });
  edit('kk_nir_lower_descriptors.c', s => s.replace(
    '   nir_rewrite_image_intrinsic(intr, handle, nir_image_intrinsic_type_bindless);',
    '   if (intr->intrinsic == nir_intrinsic_image_deref_levels) {\n      intr->intrinsic = nir_intrinsic_bindless_image_levels;\n      nir_src_rewrite(&intr->src[0], handle);\n   } else {\n      nir_rewrite_image_intrinsic(intr, handle, nir_image_intrinsic_type_bindless);\n   }'));
  edit('kk_physical_device.c', s => s
    .replace('#include "kk_physical_device.h"', '#include "kk_physical_device.h"\n#include "kk_descriptor_types.h"\n#include "kk_descriptor_set_layout.h"')
    .replace('   *properties = (struct vk_properties){', `   uint32_t image_stride = 0, buffer_stride = 0, stride, alignment;
   const VkDescriptorType image_types[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
   for (unsigned i = 0; i < ARRAY_SIZE(image_types); ++i) {
      kk_descriptor_stride_align_for_type(image_types[i], NULL, &stride, &alignment);
      image_stride = MAX2(image_stride, stride);
   }
   kk_descriptor_stride_align_for_type(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &stride, &alignment);
   buffer_stride = stride;
   kk_descriptor_stride_align_for_type(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &stride, &alignment);
   buffer_stride = MAX2(buffer_stride, stride);
   *properties = (struct vk_properties){`)
    .replace('      .EXT_descriptor_indexing = true,',
      '      .EXT_descriptor_heap = true,\n      .EXT_descriptor_indexing = true,')
    .replace('      .descriptorIndexing = true,',
      '      .descriptorHeap = true,\n      .descriptorHeapCaptureReplay = false,\n      .descriptorIndexing = true,')
    .replace('      .maxPushConstantsSize = KK_MAX_PUSH_SIZE,',
      `      .maxPushConstantsSize = KK_MAX_PUSH_SIZE,
      .samplerHeapAlignment = 8,
      .resourceHeapAlignment = 8,
      .maxSamplerHeapSize = UINT32_MAX,
      .maxResourceHeapSize = UINT32_MAX,
      .samplerDescriptorSize = sizeof(uint64_t),
      .imageDescriptorSize = image_stride,
      .bufferDescriptorSize = buffer_stride,
      .samplerDescriptorAlignment = 8,
      .imageDescriptorAlignment = 8,
      .bufferDescriptorAlignment = 8,
      .maxPushDataSize = KK_MAX_PUSH_SIZE,
      .maxDescriptorHeapEmbeddedSamplers = 2048,
      .samplerYcbcrConversionCount = 1,`));
  /* Metal captures resource IDs while encoding. Heap descriptor writes may
   * create native resources after Vulkan command recording has finished. */
  edit('kk_cmd_buffer.h', s => s.replace('   bool one_time_submit;',
    '   bool enqueue_commands;\n   bool one_time_submit;'));
  edit('kk_cmd_buffer.c', s => s
    .replace('   cmd->one_time_submit = false;',
      '   cmd->one_time_submit = false;\n   cmd->enqueue_commands = false;')
    .replace(`   cmd->one_time_submit =
      pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;`,
      `   cmd->enqueue_commands = cmd->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
      kk_cmd_buffer_device(cmd)->vk.enabled_features.descriptorHeap;
   cmd->one_time_submit = !cmd->enqueue_commands &&
      (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);`));
  editSource('src/kosmickrisp/util/kk_dispatch_cmd_gen.py', s => s
    .replaceAll('if (cmd->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) {',
                'if (cmd->enqueue_commands) {')
    .replaceAll('vk_cmd_enqueue_unless_primary_device_entrypoints.',
                'vk_cmd_enqueue_device_entrypoints.'));
  edit('kk_queue.c', s => {
    const wait = `      if (cmd_buffer->drawable) {
         mtl_command_queue_wait_for_drawable(queue->mtl_handle,
                                             cmd_buffer->drawable);
      }

`;
    const signal = `      if (cmd_buffer->drawable) {
         mtl_command_queue_signal_drawable(queue->mtl_handle,
                                           cmd_buffer->drawable);
         mtl_release(cmd_buffer->drawable);
         cmd_buffer->drawable = NULL;
      }
`;
    return s.replace(wait, '').replace(signal, '')
      .replace('   assert(kk_cmd_buffer_has_work(cmd));',
        `   assert(kk_cmd_buffer_has_work(cmd));
   if (cmd->drawable)
      mtl_command_queue_wait_for_drawable(queue->mtl_handle, cmd->drawable);`)
      .replace('   mtl_release(options);', `   mtl_release(options);
   if (cmd->drawable) {
      mtl_command_queue_signal_drawable(queue->mtl_handle, cmd->drawable);
      mtl_release(cmd->drawable);
      cmd->drawable = NULL;
   }`)
      .replace('      if (cmd_buffer->submitted) {',
        '      if (cmd_buffer->submitted || cmd_buffer->enqueue_commands) {');
  });
  /* Repeated push-constant commands may retain the same pipeline layout.
   * Its last queue reference already covers those commands' lifetime. */
  editSource('src/vulkan/util/vk_cmd_queue_gen.py', s => s.replace(
    '   VK_FROM_HANDLE(vk_pipeline_layout, vklayout, layout);\n   vk_pipeline_layout_ref(vklayout);',
    '   VK_FROM_HANDLE(vk_pipeline_layout, vklayout, layout);\n   if (vklayout && queue->pipeline_layouts.size &&\n       *util_dynarray_top_ptr(&queue->pipeline_layouts, void *) == vklayout)\n      return;\n   vk_pipeline_layout_ref(vklayout);'));
  /* backend1 carries opaque packed metadata; backend2 carries numeric LOD. */
  editSource('src/kosmickrisp/compiler/msl_type_inference.c', s => s
    .replace('      case nir_tex_src_ms_index:\n         set_type(types, src, TYPE_UINT);',
      '      case nir_tex_src_ms_index:\n      case nir_tex_src_backend1:\n         set_type(types, src, TYPE_UINT);')
    .replace('      case nir_tex_src_ddx:\n      case nir_tex_src_ddy:\n         set_type(types, src, TYPE_FLOAT);',
      '      case nir_tex_src_ddx:\n      case nir_tex_src_ddy:\n      case nir_tex_src_backend2:\n         set_type(types, src, TYPE_FLOAT);'));
  applyNativeSamplers({edit: editSource});
}

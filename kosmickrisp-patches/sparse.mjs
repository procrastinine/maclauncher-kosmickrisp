import fs from 'node:fs';
import path from 'node:path';

/** Placement sparse buffers and textures share the existing Metal queue. */
export const id = 'sparse';
export const sourceFiles = [
  'kosmickrisp-patches/sparse/kosmickrisp-sparse.c',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-nir.c',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-buffer-nir.h',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-queue.h',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-pipeline.h',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-samplers.h',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-sample-nir.h',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-msl.h',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse.h',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-layout.h',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-metal.m',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-metal.h',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-origins.json',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-notices/LICENSE.Apache-2.0',
  'kosmickrisp-patches/sparse/kosmickrisp-sparse-notices/NOTICE',
];
export function apply({base: sourceRoot, edits}) {
  const root = path.join(import.meta.dirname, 'sparse');
  const read = name => edits.get(name) ?? fs.readFileSync(path.join(sourceRoot, name), 'utf8');
  const edit = (name, apply) => edits.set(name, apply(read(name)));
  const owned = name => fs.readFileSync(path.join(root, name), 'utf8');
  const prefix = 'src/kosmickrisp/';
  const c = (name, apply) => edit(prefix + name, apply);
  for (const [target, input] of [
    ['vulkan/kk_sparse.c', 'kosmickrisp-sparse.c'],
    ['vulkan/kk_sparse.h', 'kosmickrisp-sparse.h'],
    ['compiler/kk_sparse_layout.h', 'kosmickrisp-sparse-layout.h'],
    ['bridge/mtl_sparse.h', 'kosmickrisp-sparse-metal.h'],
    ['bridge/mtl_sparse_impl.h', 'kosmickrisp-sparse-metal.m'],
    ['vulkan/kk_nir_lower_sparse.c', 'kosmickrisp-sparse-nir.c'],
    ['compiler/msl_sparse.h', 'kosmickrisp-sparse-msl.h'],
    ['vulkan/kk_sparse_buffer_nir.h', 'kosmickrisp-sparse-buffer-nir.h'],
    ['vulkan/kk_sparse_queue.h', 'kosmickrisp-sparse-queue.h'],
    ['vulkan/kk_sparse_pipeline.h', 'kosmickrisp-sparse-pipeline.h'],
    ['vulkan/kk_sparse_samplers.h', 'kosmickrisp-sparse-samplers.h'],
    ['vulkan/kk_sparse_sample_nir.h', 'kosmickrisp-sparse-sample-nir.h'],
  ]) edits.set(prefix + target, owned(input));
  c('vulkan/meson.build', s => s.replace("  'kk_queue.c',", "  'kk_queue.c',\n  'kk_sparse.c',\n  'kk_nir_lower_sparse.c',"));
  c('vulkan/kk_shader.h', s => s.replace('bool kk_nir_lower_null_images(nir_shader *nir);',
    'bool kk_nir_lower_null_images(nir_shader *nir);\nbool kk_nir_lower_sparse(nir_shader *nir);\nnir_def *kk_nir_sparse_buffer_resident(nir_builder *b, nir_def *metadata, nir_def *coord);\nnir_def *kk_nir_sparse_view_address(nir_builder *b, nir_def *address, nir_def *query_record, nir_def **lod, bool storage, bool query_array);'));
  c('vulkan/kk_shader.c', s => s.replace('      NIR_PASS(_, nir, kk_nir_lower_null_images);',
    '      NIR_PASS(_, nir, kk_nir_lower_null_images);\n\n   NIR_PASS(_, nir, kk_nir_lower_sparse);'));
  c('compiler/msl_type_inference.c', s => s.replace('   case nir_intrinsic_load_front_face:',
    '   case nir_intrinsic_is_sparse_resident_zink:\n   case nir_intrinsic_load_front_face:'));
  c('compiler/nir_to_msl.c', s => s
    .replace('static void\nintrinsic_to_msl(', '#include "msl_sparse.h"\n\nstatic void\nintrinsic_to_msl(')
    .replace(`   if (instrinsic_needs_dest_type(instr)) {
      P_IND(ctx, "t%d = ", instr->def.index);
   }`, `   if (instrinsic_needs_dest_type(instr)) {
      if (msl_def_has_sparse_residency(&instr->def)) {
         P_IND(ctx, "auto sparse%d = ", instr->def.index);
      } else {
         P_IND(ctx, "t%d = ", instr->def.index);
      }
   }`)
    .replace('   case nir_intrinsic_load_subgroup_size:', `   case nir_intrinsic_is_sparse_resident_zink:
      P(ctx, "sparse%d.resident();\\n", instr->src[0].ssa->index);
      break;
   case nir_intrinsic_load_subgroup_size:`)
    .replace(`   case nir_intrinsic_bindless_image_load:
      src_to_msl(ctx, &instr->src[0]);
      P(ctx, ".read(");`, `   case nir_intrinsic_bindless_image_load: {
      bool sparse = msl_def_has_sparse_residency(&instr->def);
      src_to_msl(ctx, &instr->src[0]);
      P(ctx, sparse ? ".sparse_read(" : ".read(");`)
    .replace(`      P(ctx, ").");
      for (uint32_t i = 0u; i < instr->def.num_components; ++i) {`, `      if (sparse) {
         P(ctx, ");\\n");
         P_IND(ctx, "t%d = sparse%d.value().", instr->def.index, instr->def.index);
      } else {
         P(ctx, ").");
      }
      for (uint32_t i = 0u; i < instr->def.num_components; ++i) {`)
    .replace(`      P(ctx, ";\\n");
      break;
   case nir_intrinsic_bindless_image_store:`, `      P(ctx, ";\\n");
      break;
   }
   case nir_intrinsic_bindless_image_store:`)
    .replace(`   P_IND(ctx, "t%d = ", tex->def.index);`, `   bool sparse = msl_def_has_sparse_residency(&tex->def);
   if (sparse) {
      P_IND(ctx, "auto sparse%d = ", tex->def.index);
   } else {
      P_IND(ctx, "t%d = ", tex->def.index);
   }`)
    .replace('P(ctx, ".sample_compare(");', 'P(ctx, sparse ? ".sparse_sample_compare(" : ".sample_compare(");')
    .replace('P(ctx, ".sample(");', 'P(ctx, sparse ? ".sparse_sample(" : ".sample(");')
    .replace(`   case nir_texop_txf: {
      src_to_msl(ctx, texhandle);
      P(ctx, ".read(");`, `   case nir_texop_txf: {
      src_to_msl(ctx, texhandle);
      P(ctx, sparse ? ".sparse_read(" : ".read(");`)
    .replace('P(ctx, ".gather_compare(");', 'P(ctx, sparse ? ".sparse_gather_compare(" : ".gather_compare(");')
    .replace('P(ctx, ".gather(");', 'P(ctx, sparse ? ".sparse_gather(" : ".gather(");')
    .replace(`      assert(!"Unsupported texture op");
   }
}`, `      assert(!"Unsupported texture op");
   }
   if (sparse) {
      P_IND(ctx, "t%d = sparse%d.value();\\n", tex->def.index, tex->def.index);
   }
}`));
  c('bridge/mtl_bridge.h', s => s.replace('#include "mtl_texture.h"', '#include "mtl_texture.h"\n#include "mtl_sparse.h"'));
  c('bridge/mtl_heap.m', s => s
    .replace('      descriptor.sparsePageSize = MTLSparsePageSize16;', `      descriptor.sparsePageSize = MTLSparsePageSize16;
      if (descriptor.storageMode == MTLStorageModePrivate &&
          size >= 65536 && size % 65536 == 0 &&
          mtl_device_supports_placement_sparse(device))
         descriptor.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;`)
    .replace('#include "mtl_heap.h"', '#include "mtl_heap.h"\n#include "mtl_sparse.h"') + '\n#include "mtl_sparse_impl.h"\n');
  c('vulkan/kk_physical_device.h', s => s.replace('   uint32_t gpu_apple_family;', '   uint32_t gpu_apple_family;\n   bool placement_sparse;'));
  c('vulkan/kk_physical_device.c', s => s
    .replace('   pdev->info.gpu_apple_family =', '   pdev->info.placement_sparse = mtl_device_supports_placement_sparse(pdev->mtl_dev_handle);\n   pdev->info.gpu_apple_family =')
    .replace('      .shaderResourceMinLod = true,', `      .shaderResourceMinLod = true,
      .sparseBinding = pdev->info.placement_sparse,
      .sparseResidencyBuffer = pdev->info.placement_sparse,
      .sparseResidencyImage2D = pdev->info.placement_sparse,
      .sparseResidencyImage3D = pdev->info.placement_sparse,
      .sparseResidencyAliased = pdev->info.placement_sparse,
      .shaderResourceResidency = pdev->info.placement_sparse,
`)
    .replace('.sparseResidencyNonResidentStrict = false', '.sparseResidencyNonResidentStrict = pdev->info.placement_sparse')
    .replace('.sparseResidencyStandard2DBlockShape = false', '.sparseResidencyStandard2DBlockShape = pdev->info.placement_sparse')
    .replace('   assert(pdev->queue_family_count <= ARRAY_SIZE(pdev->queue_families));', `   if (pdev->info.placement_sparse) {
      for (unsigned i = 0; i < pdev->queue_family_count; ++i)
         if (pdev->queue_families[i].queue_flags &
             (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
            pdev->queue_families[i].queue_flags |= VK_QUEUE_SPARSE_BINDING_BIT;
   }
   assert(pdev->queue_family_count <= ARRAY_SIZE(pdev->queue_families));`));
  c('vulkan/kk_buffer.c', s => s
    .replace('#include "kk_buffer.h"', '#include "kk_buffer.h"\n#include "kk_sparse.h"')
    .replace('   uint64_t alignment;', `   if (create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)
      return KK_SPARSE_PAGE_SIZE;
   uint64_t alignment;`)
    .replace('.memoryTypeBits = BITFIELD_MASK(pdev->mem_type_count),', `.memoryTypeBits = (pInfo->pCreateInfo->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) ?
         kk_sparse_memory_type_bits(pdev) : BITFIELD_MASK(pdev->mem_type_count),`)
    .replace('   *pBuffer = kk_buffer_to_handle(buffer);', `   if (pCreateInfo->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
      VkResult result = kk_sparse_buffer_init(dev, buffer);
      if (result != VK_SUCCESS) {
         vk_buffer_destroy(&dev->vk, pAllocator, &buffer->vk);
         return result;
      }
   }
   *pBuffer = kk_buffer_to_handle(buffer);`)
    .replace('   if (buffer->metal.handle)\n      mtl_release(buffer->metal.handle);', `   kk_sparse_buffer_finish(dev, buffer);
   if (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)
      kk_device_remove_buffer_from_residency_set(dev, buffer->metal.handle);
   if (buffer->metal.handle)
      mtl_release(buffer->metal.handle);`));
  c('vulkan/kk_buffer.h', s => s.replace('   struct vk_buffer vk;',
    '   struct vk_buffer vk;\n   mtl_buffer *sparse_residency;\n   uint32_t sparse_record;'));
  c('vulkan/kk_descriptor_types.h', s => s.replace(
    '   uint32_t zero; /* Must be zero! */',
    '   uint32_t zero; /* Sparse record in descriptor storage; zero in the lowered address. */'));
  c('vulkan/kk_descriptor_set_layout.c', s => s
    .replace('#include "kk_descriptor_set_layout.h"', '#include "kk_descriptor_set_layout.h"\n#include "kk_sparse.h"')
    .replace(`   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:`, `   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      *stride = *alignment = sizeof(struct kk_storage_image_descriptor) +
                            sizeof(struct kk_sparse_view_descriptor);
      break;
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:`));
  c('vulkan/kk_descriptor_set.c', s => {
    s = s.replace('#include "kk_buffer_view.h"', '#include "kk_buffer_view.h"\n#include "kk_sparse.h"');
    const start = s.indexOf('static void\nwrite_buffer_view_desc(');
    const end = s.indexOf('\nstatic void', start + 1);
    if (start < 0 || end < 0) throw new Error('kk_descriptor_set.c: buffer-view writer anchor missing');
    let fn = s.slice(start, end);
    for (const anchor of ['struct kk_storage_image_descriptor desc = {};',
      'desc.image_gpu_resource_id', 'desc.texel_offset', 'desc.texel_count',
      '      desc.texel_count = view->vk.elements;'])
      if (!fn.includes(anchor)) throw new Error('kk_descriptor_set.c: buffer-view writer missing ' + anchor);
    fn = fn.replace('struct kk_storage_image_descriptor desc = {};',
      'struct {\n      struct kk_storage_image_descriptor image;\n      struct kk_sparse_view_descriptor sparse;\n   } desc = {};');
    fn = fn.replaceAll('desc.image_gpu_resource_id', 'desc.image.image_gpu_resource_id')
      .replaceAll('desc.texel_offset', 'desc.image.texel_offset')
      .replaceAll('desc.texel_count', 'desc.image.texel_count')
      .replace('      desc.image.texel_count = view->vk.elements;',
        '      desc.image.texel_count = view->vk.elements;\n      desc.sparse = kk_sparse_buffer_view_descriptor(view);');
    s = s.slice(0, start) + fn + s.slice(end);
    s = s.replace('      plane_count = view->plane_count;',
      '      plane_count = view->plane_count;\n      const uint64_t sparse_query = kk_sparse_image_view_query_address(view, false);');
    s = s.replace('      if (view->plane_count > 1) {',
      '      if (sparse_query) desc[0].image_min_lod_uint16 |= KK_IMAGE_QUERY_RECORD_TAG;\n      if (view->plane_count > 1) {');
    s = s.replace('         float min_lod = MAX2(view->vk.min_lod - view->vk.base_mip_level, 0.0);',
      '         if (sparse_query) {\n            assert(view->plane_count == 1);\n            assert(desc[plane].aux_word == 0);\n            desc[plane].aux_word = sparse_query;\n         }\n         float min_lod = MAX2(view->vk.min_lod - view->vk.base_mip_level, 0.0);');
    return s.replace('      kk_sliced_3d_descriptor(&desc, &view->vk);',
      '      kk_sliced_3d_descriptor(&desc, &view->vk);\n      kk_sparse_storage_descriptor(&desc, view);');
  });
  c('vulkan/kk_device.h', s => s.replace(
    '   /* Index of this hardware sampler in the hardware sampler heap */\n   uint16_t index;',
    '   /* Index of this hardware sampler in the hardware sampler heap */\n   uint16_t index;\n   bool sparse_query_source;'));
  c('vulkan/kk_image_view.h', s => s
    .replace('struct kk_device;', 'struct kk_device;\nstruct kk_sparse_view_state;')
    .replace('   struct vk_image_view vk;', '   struct vk_image_view vk;\n   struct kk_sparse_view_state *sparse;'));
  c('vulkan/kk_sliced_3d.h', s => s
    .replace('#include <stddef.h>', '#include <stddef.h>\n#include "kk_sparse.h"')
    .replace(`   nir_def *range = nir_build_load_global_constant(b, 2, 32, address,
      .align_mul = 8, .access = ACCESS_CAN_SPECULATE);`,
      `   nir_def *range = nir_build_load_global_constant(b, 2, 32, address,
      .align_mul = 8, .access = ACCESS_CAN_SPECULATE);
   range = kk_nir_storage_sparse_slice_range(b, range);`));
  c('vulkan/kk_image_view.c', s => s
    .replace('#include "kk_image.h"', '#include "kk_image.h"\n#include "kk_sparse.h"')
    .replace('      if (view->vk.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {', `      if (view->vk.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
         VkResult result = kk_sparse_image_view_query(dev, view, mtl_handle, &view_layout, false);
         if (result != VK_SUCCESS) { kk_image_view_finish(dev, view); return result; }`)
    .replace('         view->planes[view_plane].mtl_handle_storage =', `         VkResult result = kk_sparse_image_view_query(dev, view, mtl_handle, &view_layout, true);
         if (result != VK_SUCCESS) { kk_image_view_finish(dev, view); return result; }
         view->planes[view_plane].mtl_handle_storage =`)
    .replace('   vk_image_view_finish(&view->vk);', '   kk_sparse_image_view_finish(dev, view);\n   vk_image_view_finish(&view->vk);'));
  c('vulkan/kk_nir_lower_descriptors.c', s => s.replace(
    '         image_plane_offset_B + offs, ctx);\n\n      nir_def_replace(&tex->def, min);',
    '         image_plane_offset_B + offs, ctx);\n      if (tex->dest_type == nir_type_uint16)\n         min = nir_iand_imm(b, min, KK_IMAGE_LOD_VALUE_MASK);\n\n      nir_def_replace(&tex->def, min);'));
  c('vulkan/kk_nir_lower_descriptors.c', s => {
    const begin = s.indexOf('static bool\nlower_tex(');
    const end = s.indexOf('\nstatic bool\ntry_lower_descriptors_instr(', begin);
    if (begin < 0 || end < 0) throw new Error('sparse: texture descriptor function anchor missing');
    let tex = s.slice(begin, end);
    const vector = '      nir_def *rgb[4] = {nir_channel(b, &first->def, 0)};';
    const component = '         rgb[c] = nir_channel(b, &component->def, 0);';
    if (tex.split(vector).length !== 2 || tex.split(component).length !== 2)
      throw new Error('sparse: unique RGB32 texture anchors required');
    tex = tex.replace(vector,
      '      nir_def *rgb[5] = {nir_channel(b, &first->def, 0)};\n      if (tex->is_sparse) rgb[4] = nir_channel(b, &first->def, 4);')
      .replace(component,
      '         rgb[c] = nir_channel(b, &component->def, 0);\n         if (tex->is_sparse) rgb[4] = nir_ior(b, rgb[4], nir_channel(b, &component->def, 4));');
    return s.slice(0, begin) + tex + s.slice(end);
  });
  c('vulkan/kk_nir_lower_descriptors.c', s => s
    .replace('#include "kk_shader.h"', '#include "kk_shader.h"\n#include "kk_sparse.h"')
    .replace('   nir_alu_type type;\n   if (nir_intrinsic_has_atomic_op(intr)) {', `   bool sparse_array_dimension = nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_1D ||
                                 nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_2D;
   nir_def *sparse_descriptor = NULL;
   if (sparse_array_dimension) {
      sparse_descriptor = load_resource_deref_desc(b, 2, 64, deref,
         offsetof(struct kk_storage_image_descriptor, image_gpu_resource_id), ctx);
      nir_def *descriptor_address = load_resource_addr(b, 1, 64, deref,
         offsetof(struct kk_storage_image_descriptor, image_gpu_resource_id), ctx);
      resource_addr = kk_nir_storage_sparse_handle_address(b, descriptor_address,
         sparse_descriptor, nir_intrinsic_image_array(intr));
   }
   if (intr->intrinsic == nir_intrinsic_image_deref_sparse_load &&
       nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_BUF &&
       nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_MS) {
      nir_def *query = load_resource_deref_desc(b, 1, 64, deref,
         offsetof(struct kk_storage_image_descriptor, texel_offset), ctx);
      query = sparse_array_dimension ?
         kk_nir_storage_sparse_descriptor_query(b, sparse_descriptor) :
         kk_nir_storage_sparse_query_decode(b, query);
      nir_def *lod = intr->src[3].ssa;
      resource_addr = kk_nir_sparse_view_address(b, resource_addr, query, &lod, true,
         sparse_array_dimension && nir_intrinsic_image_array(intr));
      nir_src_rewrite(&intr->src[3], lod);
   }

   nir_alu_type type;
   if (nir_intrinsic_has_atomic_op(intr)) {`)
    .replace(`      nir_def *handle = NULL;
      if (tex->is_shadow) {`, `      if (tex->is_sparse && tex->op == nir_texop_txf &&
          tex->sampler_dim != GLSL_SAMPLER_DIM_BUF &&
          tex->sampler_dim != GLSL_SAMPLER_DIM_MS) {
         nir_def *query = load_resource_deref_desc(b, 1, 64,
            nir_src_as_deref(nir_src_for_ssa(texture)),
            plane_offset_B + offsetof(struct kk_sampled_image_descriptor, aux_word), ctx);
         int lod_source = nir_tex_instr_src_index(tex, nir_tex_src_lod);
         nir_def *lod = lod_source >= 0 ? tex->src[lod_source].src.ssa : nir_imm_int(b, 0);
         resource_addr = kk_nir_sparse_view_address(b, resource_addr, query, &lod, false,
            tex->is_array && (tex->sampler_dim == GLSL_SAMPLER_DIM_1D ||
                              tex->sampler_dim == GLSL_SAMPLER_DIM_2D));
         if (lod_source >= 0) nir_src_rewrite(&tex->src[lod_source].src, lod);
         else nir_tex_instr_add_src(tex, nir_tex_src_lod, lod);
      }
      nir_def *handle = NULL;
      if (tex->is_shadow) {`)
    .replace(`         nir_src_rewrite(&intr->src[1],
                        kk_texel_buffer_coord(b, intr->src[1].ssa, metadata));`, `      {
         nir_def *coord = kk_texel_buffer_coord(b, intr->src[1].ssa, metadata);
         nir_src_rewrite(&intr->src[1], coord);
         if (intr->intrinsic == nir_intrinsic_image_deref_sparse_load) {
            nir_def *residency = load_resource_deref_desc(b, 4, 32, deref,
               sizeof(struct kk_storage_image_descriptor), ctx);
            nir_src_rewrite(&intr->src[2], nir_b2i32(b, kk_nir_sparse_buffer_resident(b, residency, coord)));
         }
      }`)
    .replace(`      if (coord_src >= 0)
         nir_src_rewrite(&tex->src[coord_src].src,
                        kk_texel_buffer_coord(b, tex->src[coord_src].src.ssa,
                                              metadata));`, `      if (coord_src >= 0) {
         nir_def *coord = kk_texel_buffer_coord(b, tex->src[coord_src].src.ssa, metadata);
         nir_src_rewrite(&tex->src[coord_src].src, coord);
         if (tex->is_sparse) {
            nir_def *residency = load_resource_deref_desc(b, 4, 32,
               nir_src_as_deref(nir_src_for_ssa(texture)),
               sizeof(struct kk_storage_image_descriptor), ctx);
            nir_tex_instr_add_src(tex, nir_tex_src_backend1,
               nir_b2i32(b, kk_nir_sparse_buffer_resident(b, residency, coord)));
         }
      }`));
  c('vulkan/kk_image.h', s => s
    .replace('#include "kk_image_layout.h"', '#include "kk_image_layout.h"\n#include "kk_sparse.h"')
    .replace('   struct vk_image vk;', '   struct vk_image vk;\n   struct kk_sparse_image *sparse;'));
  c('vulkan/kk_buffer_view.h', s => s.replace('   mtl_texture *mtl_texel_buffer_handle;',
    '   mtl_texture *mtl_texel_buffer_handle;\n   void *sparse_view_pool;'));
  c('vulkan/kk_buffer_view.c', s => s
    .replace('   view->mtl_texel_buffer_handle = mtl_new_texture_with_descriptor_linear(', `   if (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
      view->sparse_view_pool = mtl_new_sparse_buffer_view(buffer->metal.handle, &layout,
         kk_buffer_mtl_offset(buffer, view->vk.offset - prefix), &view->texel_buffer_gpu_id);
   } else
   view->mtl_texel_buffer_handle = mtl_new_texture_with_descriptor_linear(`)
    .replace('   if (!view->mtl_texel_buffer_handle) {', '   if (!view->mtl_texel_buffer_handle && !view->sparse_view_pool) {')
    .replace('   view->texel_buffer_gpu_id =\n      mtl_texture_get_gpu_resource_id(view->mtl_texel_buffer_handle);', `   if (!view->sparse_view_pool)
      view->texel_buffer_gpu_id = mtl_texture_get_gpu_resource_id(view->mtl_texel_buffer_handle);`)
    .replace('   mtl_release(view->mtl_texel_buffer_handle);', '   if (view->sparse_view_pool) mtl_release(view->sparse_view_pool);\n   mtl_release(view->mtl_texel_buffer_handle);'));
  c('vulkan/kk_image_layout.c', s => s.replace(
    '   /* Can only optimize if tiling is optimal */', `   if (flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)
      return false;
   /* Can only optimize if tiling is optimal */`));
  c('vulkan/kk_image.c', s => {
    s = s.replace('      return 2048;', '      return KK_STORAGE_MAX_3D_DIMENSION;');
    s = s.replace('   // TODO hope for the best?', `   if (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)
      memory_types = kk_sparse_memory_type_bits(pdev);`);
    s = s.replace(`   /* Don't support sparse residency */
   if ((create_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;`, `   if (create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) {
      VkSparseImageFormatProperties props;
      if (!kk_sparse_image_format_properties(pdev, pImageFormatInfo->format,
          pImageFormatInfo->type, VK_SAMPLE_COUNT_1_BIT, all_usage,
          pImageFormatInfo->tiling, create_flags, &props))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }`);
    s = s.replace('   VkSampleCountFlags sampleCounts = VK_SAMPLE_COUNT_1_BIT;', '   VkSampleCountFlags sampleCounts = VK_SAMPLE_COUNT_1_BIT;');
    s = s.replace('   /* From the Vulkan 1.2.199 spec:', `   if (create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)
      sampleCounts = VK_SAMPLE_COUNT_1_BIT;

   /* From the Vulkan 1.2.199 spec:`);
    s = s.replace(`   *pPropertyCount = 0;
   return;`, `   VK_FROM_HANDLE(kk_physical_device, pdev, physicalDevice);
   VkSparseImageFormatProperties props;
   if (!kk_sparse_format_properties(pdev, pFormatInfo->format, pFormatInfo->type,
       pFormatInfo->samples, pFormatInfo->usage, pFormatInfo->tiling, &props)) {
      *pPropertyCount = 0;
      return;
   }
   if (!pProperties) { *pPropertyCount = 1; return; }
   if (!*pPropertyCount) return;
   *pPropertyCount = 1;
   pProperties[0].properties = props;`);
    s = s.replace(`   return VK_SUCCESS;
}

static void
kk_image_plane_size_align_B`, `   if (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)
      return kk_sparse_image_init(dev, image, vk_image_usage_flags(pCreateInfo));
   return VK_SUCCESS;
}

static void
kk_image_plane_size_align_B`);
    s = s.replace(`   for (uint8_t plane = 0; plane < image->plane_count; plane++) {
      kk_image_plane_finish`, `   kk_sparse_image_finish(dev, image);
   for (uint8_t plane = 0; plane < image->plane_count; plane++) {
      kk_image_plane_finish`);
    s = s.replace('   *pSparseMemoryRequirementCount = 0u;', `   VK_FROM_HANDLE(kk_image, image, pInfo->image);
   kk_sparse_image_requirements(image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);`);
    s = s.replace('   *pSparseMemoryRequirementCount = 0u;', `   VK_FROM_HANDLE(kk_device, dev, device);
   struct kk_image image = {0};
   VkResult result = kk_image_init(dev, &image, pInfo->pCreateInfo);
   if (result != VK_SUCCESS) { *pSparseMemoryRequirementCount = 0; return; }
   kk_sparse_image_requirements(&image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
   kk_image_finish(dev, &image, NULL);`);
    return s;
  });
  c('vulkan/kk_device.h', s => s.replace('   struct vk_device vk;',
    '   struct vk_device vk;\n   struct kk_sparse_device_state *sparse;'));
  c('vulkan/kk_device.c', s => s
    .replace('#include "kk_device.h"', '#include "kk_device.h"\n#include "kk_sparse.h"')
    .replace('   if (pdev->settings.gpu_capture_enabled) {', `   result = kk_sparse_device_init(dev);
   if (result != VK_SUCCESS) {
      if (dev->atomic64_locks) kk_destroy_bo(dev, dev->atomic64_locks);
      kk_device_finish_lib(dev);
      goto fail_sampler_heap;
   }

   if (pdev->settings.gpu_capture_enabled) {`)
    .replace('   /* Release the residency set last once all BOs are released. */',
      '   kk_sparse_device_finish(dev);\n\n   /* Release the residency set last once all BOs are released. */'));
  c('vulkan/kk_cmd_buffer.h', s => s.replace('   /* Client push constants: upload only the bound pipeline range. */',
    '   uint64_t sparse_state;\n\n   /* Client push constants: upload only the bound pipeline range. */'));
  c('vulkan/kk_cmd_buffer.c', s => s
    .replace('#include "kk_cmd_buffer.h"', '#include "kk_cmd_buffer.h"\n#include "kk_sparse.h"')
    .replace(`   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   ((struct kk_root_descriptor_table *)ptr.cpu)->addr =`,
      `   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   ((struct kk_root_descriptor_table *)ptr.cpu)->sparse_state = kk_sparse_device_address(dev);
   ((struct kk_root_descriptor_table *)ptr.cpu)->addr =`));
  c('vulkan/kk_buffer.h', s => s.replace('      .zero = 0,',
    '      .zero = buffer ? buffer->sparse_record : 0,'));
  c('vulkan/kk_shader.h', s => s
    .replace('   KK_FEAT_COUNT,', '   KK_FEAT_SPARSE_BUFFER,\n   KK_FEAT_SPARSE_REPLAY,\n   KK_FEAT_COUNT,')
    .replace('bool kk_nir_lower_sparse(nir_shader *nir);', `bool kk_nir_lower_sparse(nir_shader *nir);
bool kk_nir_lower_sparse_buffer_io(nir_shader *nir,
   const struct vk_pipeline_robustness_state *rs, uint32_t count,
   struct vk_descriptor_set_layout *const *layouts);`));
  c('vulkan/kk_shader.c', s => s
    .replace('   if (feats->nullDescriptor)', `   /* Cache entries with a replay input cannot alias entries without it.
    * This bit changes serialization only; the guard bit is selected on use. */
   if (feats->sparseResidencyBuffer) key |= BITFIELD_BIT(KK_FEAT_SPARSE_REPLAY);
   if (feats->nullDescriptor)`)
    .replace('   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,', `   if (features & BITFIELD_BIT(KK_FEAT_SPARSE_BUFFER)) {
      NIR_PASS(_, nir, kk_nir_lower_sparse_buffer_io, rs, set_layout_count, set_layouts);
      NIR_PASS(_, nir, kk_nir_finish_sparse_heap_buffers);
   }

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,`));
  c('vulkan/kk_shader.h', s => s
    .replace('struct kk_cmd_buffer;', 'struct kk_cmd_buffer;\nstruct kk_sparse_shader_state;')
    .replace('   struct vk_shader vk;', '   struct vk_shader vk;\n   struct kk_sparse_shader_state *sparse;')
    .replace('extern const struct vk_device_shader_ops kk_device_shader_ops;', `extern const struct vk_device_shader_ops kk_device_shader_ops;
void kk_sparse_shader_finish(struct kk_shader *shader);
struct kk_shader *kk_sparse_select_shader(struct kk_cmd_buffer *cmd, struct kk_shader *shader);
void kk_sparse_refresh_shader(struct kk_cmd_buffer *cmd, mesa_shader_stage stage);`));
  c('vulkan/kk_shader.c', s => s
    .replace('   kk_attachment_pipelines_finish(shader);', '   kk_sparse_shader_finish(shader);\n   kk_attachment_pipelines_finish(shader);')
    .replace(`static VkResult
kk_compile_shaders(struct vk_device *device, uint32_t shader_count,`, `#include "kk_sparse_pipeline.h"

static VkResult
kk_compile_shaders_native(struct vk_device *device, uint32_t shader_count,`)
    .replace(`                   struct vk_shader **shaders_out)
{
   VkResult result = VK_SUCCESS;`, `                   struct vk_shader **shaders_out, bool sparse_guarded)
{
   VkResult result = VK_SUCCESS;`)
    .replace('   kk_feature_key features = kk_make_feature_key(enabled_features);', `   kk_feature_key features = kk_make_feature_key(enabled_features);
   if (sparse_guarded) features |= BITFIELD_BIT(KK_FEAT_SPARSE_BUFFER);`)
    .replace('kk_shader_serialize(struct vk_device *vk_dev,', 'kk_shader_serialize_native(struct vk_device *vk_dev,')
    .replace('kk_deserialize_shader(struct vk_device *vk_dev,', 'kk_deserialize_shader_native(struct vk_device *vk_dev,')
    .replace(`   struct kk_descriptor_state *desc = &cmd->state.cs.descriptors;
   /* Meta operations`, `   shader = kk_sparse_select_shader(cmd, shader);
   struct kk_descriptor_state *desc = &cmd->state.cs.descriptors;
   /* Meta operations`)
    .replace(`   cmd->state.shaders[stage] = shader;
   cmd->state.dirty_shaders`, `   if (stage == MESA_SHADER_VERTEX) shader = kk_sparse_select_shader(cmd, shader);
   cmd->state.shaders[stage] = shader;
   cmd->state.dirty_shaders`)
    .replace('static VkResult\nkk_shader_get_executable_properties(', `void
kk_sparse_refresh_shader(struct kk_cmd_buffer *cmd, mesa_shader_stage stage)
{
   if (!kk_sparse_residency_used(kk_cmd_buffer_device(cmd))) return;
   struct kk_shader *shader = cmd->state.shaders[stage];
   struct kk_shader *selected = kk_sparse_select_shader(cmd, shader);
   if (selected == shader) return;
   if (stage == MESA_SHADER_VERTEX) kk_cmd_bind_graphics_shader(cmd, stage, selected);
   else kk_cmd_bind_compute_shader(cmd, selected);
}

static VkResult
kk_shader_get_executable_properties(`));
  c('vulkan/kk_device.h', s => s.replace('   struct kk_sparse_device_state *sparse;',
    '   struct kk_sparse_device_state *sparse;\n   uint32_t sparse_residency_used;\n   struct kk_sparse_sample_row *sparse_sample_rows;'));
  c('vulkan/kk_device.c', s => s
    .replace(`VkResult
kk_sampler_heap_add(struct kk_device *dev,`, `static void kk_sampler_heap_remove_locked(struct kk_device *dev,
   struct kk_sampler_heap *h, struct kk_rc_sampler *rc);

VkResult
kk_sampler_heap_add(struct kk_device *dev,`)
    .replace(`   VkResult result = kk_sampler_heap_add_locked(dev, h, desc, out, true);
   simple_mtx_unlock(&h->lock);`, `   VkResult result = kk_sampler_heap_add_locked(dev, h, desc, out, true);
   if (result == VK_SUCCESS) {
      result = kk_sparse_sampler_added(dev, *out);
      if (result != VK_SUCCESS) kk_sampler_heap_remove_locked(dev, h, *out);
   }
   simple_mtx_unlock(&h->lock);`)
    .replace(`   VkResult result = kk_sampler_heap_add_locked(dev, h, desc, out, false);
   simple_mtx_unlock(&h->lock);`, `   VkResult result = kk_sampler_heap_add_locked(dev, h, desc, out, false);
   if (result == VK_SUCCESS) {
      result = kk_sparse_sampler_added(dev, *out);
      if (result != VK_SUCCESS) kk_sampler_heap_remove_locked(dev, h, *out);
   }
   simple_mtx_unlock(&h->lock);`)
    .replace(`   if (rc->refcount == 0) {
      mtl_release(rc->handle);`, `   if (rc->refcount == 0) {
      kk_sparse_sampler_removed(dev, rc);
      mtl_release(rc->handle);`)
    .replace('   kk_query_table_finish(dev, &h->table);',
      '   kk_sparse_sample_rows_finish(dev);\n   kk_query_table_finish(dev, &h->table);'));
  c('vulkan/kk_cmd_buffer.h', s => s.replace('   bool one_time_submit;',
    '   bool one_time_submit;\n   bool sparse_guarded;'));
  c('vulkan/kk_cmd_buffer.c', s => s.replace(`   cmd->one_time_submit = !cmd->enqueue_commands &&
      (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);`, `   struct kk_device *sparse_dev = kk_cmd_buffer_device(cmd);
   cmd->sparse_guarded = kk_sparse_residency_used(sparse_dev);
   cmd->one_time_submit = !cmd->enqueue_commands &&
      (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) &&
      (!sparse_dev->vk.enabled_features.sparseResidencyBuffer || cmd->sparse_guarded);`));
  c('vulkan/kk_queue.c', s => s.replace('      if (cmd_buffer->submitted || cmd_buffer->enqueue_commands) {',
    '      if (cmd_buffer->submitted || cmd_buffer->enqueue_commands || (!cmd_buffer->sparse_guarded && kk_sparse_residency_used(dev))) {'));
  c('vulkan/kk_cmd_dispatch.c', s => s.replace(`kk_flush_compute_state(struct kk_cmd_buffer *cmd)
{`, `kk_flush_compute_state(struct kk_cmd_buffer *cmd)
{
   kk_sparse_refresh_shader(cmd, MESA_SHADER_COMPUTE);`));
  c('vulkan/kk_cmd_draw.c', s => s.replace(`kk_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{`, `kk_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   kk_sparse_refresh_shader(cmd, MESA_SHADER_VERTEX);`));
  c('vulkan/kk_cmd_dgc.c', s => s
    .replace(`   bool compute = l->stages & VK_SHADER_STAGE_COMPUTE_BIT;
   struct kk_dgc_params p = kk_dgc_parameters(state, info);`,
      `   bool compute = l->stages & VK_SHADER_STAGE_COMPUTE_BIT;
   kk_sparse_refresh_shader(state, compute ? MESA_SHADER_COMPUTE : MESA_SHADER_VERTEX);
   struct kk_dgc_params p = kk_dgc_parameters(state, info);`)
    .replace(`   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   if (!p.base_root || !p.base_draw || (p.patch_count && !p.patches) ||`,
      `   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   kk_sparse_refresh_shader(cmd, compute ? MESA_SHADER_COMPUTE : MESA_SHADER_VERTEX);
   if (!p.base_root || !p.base_draw || (p.patch_count && !p.patches) ||`));
  c('vulkan/kk_shader.h', s => s.replace('bool kk_nir_lower_sparse(nir_shader *nir);',
    'bool kk_nir_lower_sparse(nir_shader *nir);\nbool kk_nir_lower_sparse_border(nir_shader *nir);\nbool kk_nir_lower_sparse_sample_views(nir_shader *nir, uint32_t count, struct vk_descriptor_set_layout *const *layouts, bool minmax);'));
  c('vulkan/kk_shader.c', s => s.replace(`   NIR_PASS(_, nir, kk_nir_lower_sampler_helpers, set_layout_count, set_layouts,
      (features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX)) && pdev->info.gpu_apple_family < 10,
      features & BITFIELD_BIT(KK_FEAT_CUSTOM_BORDER));`,
    `   NIR_PASS(_, nir, kk_nir_lower_sampler_helpers, set_layout_count, set_layouts,
      (features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX)) && pdev->info.gpu_apple_family < 10,
      features & BITFIELD_BIT(KK_FEAT_CUSTOM_BORDER));
   NIR_PASS(_, nir, kk_nir_lower_sparse_sample_views, set_layout_count, set_layouts,
      (features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX)) && pdev->info.gpu_apple_family < 10);`));
  c('vulkan/kk_nir_lower_custom_border.c', s => s
    .replace('   nir_def *clamp_to_1 = &nir_instr_as_tex(orig)->def;',
      '   nir_def *clamp_to_1 = &nir_instr_as_tex(orig)->def;\n   nir_def *source_1 = clamp_to_1;')
    .replace('      nir_def *clamp_to_0 = &tex_0->def;',
      `      nir_def *clamp_to_0 = &tex_0->def;
      nir_def *sparse_status = NULL;
      if (tex->is_sparse) {
         sparse_status = nir_ior(b, nir_channel(b, clamp_to_0, 4), nir_channel(b, source_1, 4));
         clamp_to_0 = nir_trim_vector(b, clamp_to_0, 4);
      }
      nir_def *clamp_to_1 = tex->is_sparse ? nir_trim_vector(b, source_1, 4) : source_1;`)
    .replace(`   }
   nir_pop_if(b, NULL);`,
      `      if (tex->is_sparse)
         replaced = nir_vec5(b, nir_channel(b, replaced, 0), nir_channel(b, replaced, 1),
            nir_channel(b, replaced, 2), nir_channel(b, replaced, 3), sparse_status);
   }
   nir_pop_if(b, NULL);`)
    .replace(`   return nir_shader_tex_pass(nir, lower, nir_metadata_none, NULL);
}`, `   return nir_shader_tex_pass(nir, lower, nir_metadata_none, NULL);
}

static bool
kk_lower_sparse_border_tex(nir_builder *b, nir_tex_instr *tex, void *data)
{
   return tex->is_sparse && lower(b, tex, data);
}

bool
kk_nir_lower_sparse_border(nir_shader *nir)
{
   return nir_shader_tex_pass(nir, kk_lower_sparse_border_tex, nir_metadata_none, NULL);
}`));
  c('vulkan/kk_nir_lower_descriptors.c', s => s.replace('#include "kk_sampler_minmax.h"',
    '#include "kk_sampler_minmax.h"\n#include "kk_sparse_sample_nir.h"'));
  c('vulkan/kk_sampler_minmax.h', s => s.replace(`      if (probe->src[i].src_type != nir_tex_src_texture_deref &&
          probe->src[i].src_type != nir_tex_src_sampler_deref &&
          probe->src[i].src_type != nir_tex_src_coord)`, `      if (probe->src[i].src_type != nir_tex_src_texture_deref &&
          probe->src[i].src_type != nir_tex_src_sampler_deref &&
          probe->src[i].src_type != nir_tex_src_texture_handle &&
          probe->src[i].src_type != nir_tex_src_sampler_handle &&
          probe->src[i].src_type != nir_tex_src_coord)`));
  c('vulkan/kk_nir_lower_descriptors.c', s => s
    .replace(`      assert(num_components == 4 && bit_size == 32);
      nir_def *desc = load_root(b, 4, 32, root_desc_offset, 16);`, `      nir_def *desc = load_root(b, num_components, bit_size,
         nir_iadd_imm(b, root_desc_offset, offset_B), offset_B ? 4 : 16);
      if (offset_B) return desc;
      assert(num_components == 4 && bit_size == 32);`)
    .replace(`      if (binding_layout->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
          binding_layout->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {`, `      if (offset_B == 0 && (binding_layout->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                            binding_layout->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) {`)
    .replace('static bool\nlower_ssbo_descriptor(', '#include "kk_sparse_buffer_nir.h"\n\nstatic bool\nlower_ssbo_descriptor(')
    .replace(`   nir_def_rewrite_uses(&intrin->def, desc);

   return true;
}

#include "kk_sparse_buffer_nir.h"`, `   /* The stored fourth word is a residency index, never a byte offset. */
   desc = nir_vector_insert_imm(b, desc, nir_imm_int(b, 0), 3);
   nir_def_rewrite_uses(&intrin->def, desc);

   return true;
}

#include "kk_sparse_buffer_nir.h"`));
  c('vulkan/kk_shader.h', s => s.replace(
    'bool kk_nir_prepare_descriptor_heaps(nir_shader *nir, uint32_t embedded_count, const uint64_t *embedded);',
    'bool kk_nir_prepare_descriptor_heaps(nir_shader *nir, uint32_t embedded_count, const uint64_t *embedded, bool sparse_buffers);\nbool kk_nir_finish_sparse_heap_buffers(nir_shader *nir);'));
  c('vulkan/kk_shader.c', s => s.replace(
    '               compile_info->embedded_sampler_count, embedded);',
    '               compile_info->embedded_sampler_count, embedded, features & BITFIELD_BIT(KK_FEAT_SPARSE_BUFFER));'));
  c('vulkan/kk_descriptor_heap_nir.h', s => s
    .replace('   uint32_t embedded_count;', '   uint32_t embedded_count;\n   bool sparse_buffers;')
    .replace(`   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic == nir_intrinsic_load_heap_descriptor ||`,
      `   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   bool buffer = intr->intrinsic == nir_intrinsic_load_heap_descriptor &&
      (nir_intrinsic_resource_type(intr) & (nir_resource_type_uniform_buffer |
       nir_resource_type_read_only_storage_buffer | nir_resource_type_read_write_storage_buffer));
   /* Keep the descriptor identity until sparse IO consumes its record index. */
   if (buffer && ctx->sparse_buffers) return false;
   if (intr->intrinsic == nir_intrinsic_load_heap_descriptor ||`)
    .replace('      nir_def_replace(&intr->def, value);',
      `      if (buffer) {
         assert(value->num_components == 4 && value->bit_size == 32);
         value = nir_vector_insert_imm(b, value, nir_imm_int(b, 0), 3);
      }
      nir_def_replace(&intr->def, value);`)
    .replace(`                                 const uint64_t *embedded)
{
   const struct kk_heap_nir_context ctx = {embedded, embedded_count};`,
      `                                 const uint64_t *embedded, bool sparse_buffers)
{
   const struct kk_heap_nir_context ctx = {embedded, embedded_count, sparse_buffers};`)
    .replace(`                                       nir_metadata_control_flow, (void *)&ctx);
}`, `                                       nir_metadata_control_flow, (void *)&ctx);
}

bool
kk_nir_finish_sparse_heap_buffers(nir_shader *nir)
{
   const struct kk_heap_nir_context ctx = {0};
   return nir_shader_instructions_pass(nir, kk_prepare_heap_instr,
                                       nir_metadata_control_flow, (void *)&ctx);
}`));
  c('vulkan/kk_queue.c', s => s
    .replace('#include "kk_sync.h"', '#include "kk_sync.h"\n#include "kk_sparse.h"')
    .replace('   for (uint32_t i = 0; i < submit->command_buffer_count; ++i) {', `   if (vk_queue_submit_has_bind(submit)) {
      VkResult result = kk_queue_bind_sparse(queue, submit);
      if (result != VK_SUCCESS) return result;
   }
   for (uint32_t i = 0; i < submit->command_buffer_count; ++i) {`)
    .replace('static VkResult\nrerecord_and_commit_cmd_buffer(',
      '#include "kk_sparse_queue.h"\n\nstatic VkResult\nrerecord_and_commit_cmd_buffer(')
    .replace('         kk_queue_commit(queue, cmd_buffer, commit_callback, dev);',
      '         kk_sparse_commit_first(queue, cmd_buffer);')
    .replace(`   queue->commits_in_flight++;
   kk_queue_commit(queue, rerecord, rerecord_commit_callback, commit);`, `   queue->commits_in_flight++;
   mtl_feedback_handler_callback callback = kk_sparse_submission_begin(dev) ?
      kk_sparse_rerecord_commit_callback : rerecord_commit_callback;
   kk_queue_commit(queue, rerecord, callback, commit);`));
}

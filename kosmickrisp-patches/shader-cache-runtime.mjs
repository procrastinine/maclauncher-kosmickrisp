// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'shader-cache-runtime';
export const sourceFiles = ['kosmickrisp-patches/shader-cache-bindings.h'];
export function apply({edit, edits, read}) {
  const root = 'src/kosmickrisp/vulkan/';
  if (!read(root + 'kk_shader.h').includes('#define KK_EMBEDDED_SAMPLER_BINDING_SCHEMA 1u') ||
      !read(root + 'kk_descriptor_heap_nir.h').includes('kk_root_descriptor_offset(heap_embedded_samplers)'))
    throw Error('Shader cache runtime requires embedded-sampler-bindings schema1');
  edits.set(root + 'kk_shader_cache_bindings.h',
    fs.readFileSync(new URL('./shader-cache-bindings.h', import.meta.url), 'utf8'));
  edit(root + 'kk_shader.h', s => s.replace('   uint32_t embedded_sampler_count;',
    '   bool cache_uses_embedded_samplers;\n   uint32_t embedded_sampler_count;'));
  edit(root + 'kk_shader.c', s => s
    .replace('   gather_shader_info(shader, nir, state);',
      '   shader->cache_uses_embedded_samplers = nir->info.uses_embedded_samplers;\n   gather_shader_info(shader, nir, state);')
    .replace('static bool\nkk_shader_serialize_native(',
      '#include "kk_shader_cache_bindings.h"\n\nstatic bool\nkk_shader_serialize_native(')
    .replace('   blob_write_bytes(blob, &shader->info, sizeof(shader->info));',
      '   if (!kk_shader_cache_write_bindings(shader, blob))\n      return false;\n   blob_write_bytes(blob, &shader->info, sizeof(shader->info));')
    .replace('   struct kk_shader_info info;\n   blob_copy_bytes(blob, &info, sizeof(info));',
      `   struct kk_shader_cache_bindings bindings;
   VkResult result = kk_shader_cache_read_bindings(dev, blob, &bindings);
   if (result != VK_SUCCESS)
      return result;
   struct kk_shader_info info;
   blob_copy_bytes(blob, &info, sizeof(info));`)
    .replace('   VkResult result = kk_msl_deserialize(blob, &shader->msl_data[info.stage]);',
      `   result = kk_shader_cache_restore_bindings(dev, shader, &bindings);
   if (result != VK_SUCCESS)
      goto fail;
   result = kk_msl_deserialize(blob, &shader->msl_data[info.stage]);`));
}

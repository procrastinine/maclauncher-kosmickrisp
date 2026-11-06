// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

/** Extend push data through the existing root table and bindless descriptors. */
export const id = 'push-data';
export function apply({base: sourceRoot, edits}) {
  const edit = (file, apply) => {
    const name = 'src/kosmickrisp/vulkan/' + file;
    edits.set(name, apply(edits.get(name) ?? fs.readFileSync(path.join(sourceRoot, name), 'utf8')));
  };

  // MoltenVK 5189f8de reports 4 KiB constants and per-stage resource limits.
  // Kosmic uses GPU addresses rather than Metal's direct buffer binding slots.
  edit('kk_private.h', s => s
    .replace('#define KK_MAX_PUSH_SIZE                 256', '#define KK_MAX_PUSH_SIZE                 4096')
    .replace('#define KK_MAX_PUSH_DESCRIPTORS          32', '#define KK_MAX_PUSH_DESCRIPTORS          256'));

  // Keep fixed state before the variable-sized tail. All shader accesses use
  // offsetof, including those in the Poly compute stages.
  edit('kk_cmd_buffer.h', s => s
    .replace('   /* Client push constants */\n   uint8_t push[KK_MAX_PUSH_SIZE];\n\n', '')
    .replace('   uint8_t set_dynamic_buffer_start[KK_MAX_SETS];',
      '   uint8_t set_dynamic_buffer_start[KK_MAX_SETS];\n\n   /* Client push constants: upload only the bound pipeline range. */\n   uint8_t push[KK_MAX_PUSH_SIZE];')
    .replace('   bool root_dirty;', '   bool root_dirty;\n   uint32_t push_size;'));

  edit('kk_shader.h', s => s.replace('   bool uses_per_draw_data;',
    '   bool uses_per_draw_data;\n   uint32_t push_size;'));
  edit('kk_shader.c', s => s
    .replace('   /* Compile pipeline:\n', `   /* Retain the stage-filtered layout extent in serialized shader info.
    * Graphics stages share one root, including stages lowered through Poly. */
   uint32_t push_size = 0;
   for (uint32_t i = 0; i < shader_count; ++i) {
      const struct vk_shader_compile_info *info = &infos[i];
      for (uint32_t r = 0; r < info->push_constant_range_count; ++r) {
         const VkPushConstantRange *range = &info->push_constant_ranges[r];
         if (range->stageFlags & mesa_to_vk_shader_stage(info->stage))
            push_size = MAX2(push_size, range->offset + range->size);
      }
   }
   for (uint32_t i = 0; i < total_shaders; ++i)
      shaders[i]->info.push_size = push_size;

   /* Compile pipeline:
`)
    .replace('   cmd->state.shaders[MESA_SHADER_COMPUTE] = shader;', `   struct kk_descriptor_state *desc = &cmd->state.cs.descriptors;
   /* Meta operations can restore an unbound compute stage. */
   uint32_t push_size = shader ? shader->info.push_size : 0;
   desc->root_dirty |= push_size > desc->push_size;
   desc->push_size = push_size;
   cmd->state.shaders[MESA_SHADER_COMPUTE] = shader;`)
    .replace('   bool requires_dynamic_depth_stencil =', `   struct kk_descriptor_state *desc = &cmd->state.gfx.descriptors;
   desc->root_dirty |= shader->info.push_size > desc->push_size;
   desc->push_size = shader->info.push_size;

   bool requires_dynamic_depth_stencil =`));

  edit('kk_cmd_buffer.c', s => s
    .replace('   struct kk_ptr root_ptr = kk_pool_alloc(cmd, sizeof(*root), 8u);',
      '   uint32_t size = offsetof(struct kk_root_descriptor_table, push) + desc->push_size;\n   struct kk_ptr root_ptr = kk_pool_alloc(cmd, size, 8u);')
    .replace('   memcpy(root_ptr.cpu, root, sizeof(*root));',
      '   memcpy(root_ptr.cpu, root, size);')
    .replace(`   if (unlikely(desc->push[set] == NULL)) {
      size_t size = sizeof(*desc->push[set]);
      desc->push[set] = vk_zalloc(&cmd->vk.pool->alloc, size, 8,
                                  VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (unlikely(desc->push[set] == NULL)) {`, `   uint32_t required = set_layout->non_variable_descriptor_buffer_size;
   if (unlikely(desc->push[set] == NULL || desc->push[set]->capacity < required)) {
      uint32_t previous = desc->push[set] ? desc->push[set]->capacity : 0;
      size_t size = sizeof(*desc->push[set]) + required;
      struct kk_push_descriptor_set *push = vk_realloc(
         &cmd->vk.pool->alloc, desc->push[set], size, 8,
         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (unlikely(push == NULL)) {`)
    .replace(`         return NULL;
      }
   }

   /* Pushing descriptors`, `         return NULL;
      }
      memset(push->data + previous, 0, required - previous);
      push->capacity = required;
      desc->push[set] = push;
   }

   /* Pushing descriptors`)
    .replace(`      struct kk_ptr push_gpu = kk_pool_upload(
         cmd, push_set->data, sizeof(push_set->data), KK_MIN_UBO_ALIGNMENT);`,
      `      uint32_t size = push_set->layout->non_variable_descriptor_buffer_size;
      struct kk_ptr push_gpu = kk_pool_upload(
         cmd, push_set->data, size, KK_MIN_UBO_ALIGNMENT);`)
    .replace('      desc->set_sizes[set_idx] = sizeof(push_set->data);',
      '      desc->set_sizes[set_idx] = size;'));

  edit('kk_descriptor_set.h', s => s.replace(`   uint8_t data[KK_PUSH_DESCRIPTOR_SET_SIZE];
   struct kk_descriptor_set_layout *layout;`, `   uint32_t capacity;
   struct kk_descriptor_set_layout *layout;
   uint8_t data[];`));
  edit('kk_descriptor_set.c', s => s
    .replace('layout->non_variable_descriptor_buffer_size < sizeof(push_set->data)',
      'layout->non_variable_descriptor_buffer_size <= push_set->capacity')
    .replaceAll('.size = sizeof(push_set->data),',
      '.size = layout->non_variable_descriptor_buffer_size,'));
}

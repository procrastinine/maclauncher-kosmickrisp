// SPDX-License-Identifier: MIT
// Rebinding unchanged descriptor roots preserves the existing uploaded table.
export const id = 'descriptor-bind-dirty';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', (source, replace) => {
    const start = source.indexOf('static void\nkk_bind_descriptor_sets(');
    const end = source.indexOf('\nVKAPI_ATTR void VKAPI_CALL\nkk_CmdBindDescriptorSets2KHR(', start);
    if (start < 0 || end < 0) throw Error('descriptor-bind-dirty: descriptor binding function anchor missing');
    const original = source.slice(start, end);
    let changed = original;
    const change = (from, to) => {
      if (!changed.includes(from)) throw Error('descriptor-bind-dirty: descriptor binding body anchor missing');
      changed = changed.replace(from, to);
    };
    change("   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, info->layout);",
      "   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, info->layout);\n\n   /* A repeated ordinary bind without dynamic descriptors changes neither\n    * root addresses nor layout-derived offset slots. Preserve pending dirty\n    * state and push descriptors exactly as on the general path. */\n   if (info->descriptorSetCount == 1 && info->dynamicOffsetCount == 0 &&\n       pipeline_layout->set_layouts[info->firstSet] != NULL &&\n       vk_to_kk_descriptor_set_layout(\n          pipeline_layout->set_layouts[info->firstSet])->vk.dynamic_descriptor_count == 0) {\n      VK_FROM_HANDLE(kk_descriptor_set, set, info->pDescriptorSets[0]);\n      if (set != NULL && desc->sets[info->firstSet] == set)\n         return;\n   }\n");
    change('   uint32_t next_dyn_offset = 0;',
      '   bool root_changed = false;\n   uint32_t next_dyn_offset = 0;');
    change(`      if (desc->sets[s] != set) {
         if (set != NULL) {`, `      if (desc->sets[s] != set) {
         const uint64_t addr = set ? set->addr : 0;
         const uint32_t size = set ? set->size : 0;
         root_changed |= desc->root.sets[s] != addr ||
                         desc->set_sizes[s] != size;
         if (set != NULL) {`);
    change('               desc->root.dynamic_buffers[dyn_buffer_start + j] = addr;',
      `               struct kk_buffer_address *previous =
                  &desc->root.dynamic_buffers[dyn_buffer_start + j];
               if (previous->base_addr != addr.base_addr ||
                   previous->size != addr.size || previous->zero != addr.zero) {
                  *previous = addr;
                  root_changed = true;
               }`);
    change('   desc->root_dirty = true;',
      `   /* Dynamic offsets are compared in their layout-derived slots, so
    * incompatible layouts still update every affected root address. Preserve
    * dirty state from pushes, pipeline changes and earlier binds. */
   desc->root_dirty |= root_changed;`);
    return replace(original, changed);
  });
}

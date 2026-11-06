// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

// Empty-view handling follows Willie Abrams's Mesa MR !45933, MIT:
// https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/45933
// head 33262b67d839200af23d176b87091c68054f2626 (2026-11-05).
// This complements the logical-offset adaptation; it adds no feature bit.
export const id = 'empty-texel-views';
export function apply({base: sourceRoot, edits}) {
  const edit = (file, apply) => {
    const name = 'src/kosmickrisp/vulkan/' + file;
    edits.set(name, apply(edits.get(name) ?? fs.readFileSync(path.join(sourceRoot, name), 'utf8')));
  };
  edit('kk_buffer_view.c', s => s.replace(
    '   enum pipe_format p_format = vk_format_to_pipe_format(view->vk.format);',
    `   /* VK_WHOLE_SIZE can leave less than one texel after the offset. The
    * valid empty view has a null descriptor; Metal rejects width zero. */
   if (view->vk.elements == 0) {
      *pBufferView = kk_buffer_view_to_handle(view);
      return VK_SUCCESS;
   }

   enum pipe_format p_format = vk_format_to_pipe_format(view->vk.format);`));
  edit('kk_descriptor_set.c', s => s.replace(
    '      assert(view->mtl_texel_buffer_handle);\n      assert(view->texel_buffer_gpu_id);',
    '      /* An empty view has no native texture. */\n      assert(view->texel_buffer_gpu_id || view->vk.elements == 0);'));
}

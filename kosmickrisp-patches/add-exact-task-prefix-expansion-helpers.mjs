// SPDX-License-Identifier: MIT
// Add exact Task-prefix expansion helpers
import fs from 'node:fs';
export const id = "add-exact-task-prefix-expansion-helpers";
export const sourceFiles = [
  "kosmickrisp-patches/add-exact-task-prefix-expansion-helpers/src/kosmickrisp/vulkan/kk_mesh_expand.h",
  "kosmickrisp-patches/add-exact-task-prefix-expansion-helpers/src/kosmickrisp/vulkan/kk_mesh_expand_bridge.h",
  "kosmickrisp-patches/add-exact-task-prefix-expansion-helpers/src/kosmickrisp/vulkan/kk_mesh_expand_shader.h"
];
const added = file => fs.readFileSync(new URL("./add-exact-task-prefix-expansion-helpers/" + file, import.meta.url), 'utf8');
export function apply({edit, edits}) {
  edit("src/kosmickrisp/libkk/kk_mesh.cl", source => source
    .replace("   out[1] = active ? grid[1] : 1;\n   out[2] = active ? grid[2] : 1;\n}\n",
      () => "   out[1] = active ? grid[1] : 1;\n   out[2] = active ? grid[2] : 1;\n}\n\nKERNEL(1)\nlibkk_mesh_prefix_expand(global struct kk_mesh_params *p,\n   global uint32_t *grids, global uint32_t *range, uint32_t span)\n{\n   kk_mesh_prefix_expand(p, grids, range, span);\n}\n"));
  edit("src/kosmickrisp/libkk/kk_mesh_kernels.h", source => source
    .replace("   }\n}\n\n#endif\n",
      () => "   }\n}\n\n/* Prefix and expand only real output; unused capacity is never traversed. */\nstatic inline void\nkk_mesh_prefix_expand(KK_MESH_GLOBAL struct kk_mesh_params *p,\n   KK_MESH_GLOBAL uint32_t *grids, KK_MESH_GLOBAL uint32_t *range,\n   uint32_t span)\n{\n   kk_mesh_prefix_batch(p);\n   KK_MESH_GLOBAL struct kk_mesh_batch *batch =\n      (KK_MESH_GLOBAL struct kk_mesh_batch *)p->batch;\n   uint32_t total = batch->mesh_count;\n   uint32_t needed = total / span + (total % span != 0);\n   range[0] = 0;\n   range[1] = needed;\n   batch->pad = needed > 1;\n   if (!needed) grids[0] = 0, grids[1] = grids[2] = 1;\n   for (uint32_t i = 0; i < needed; ++i) {\n      uint32_t left = total - i * span;\n      kk_mesh_slice_grid(grids + 3u * i, left < span ? left : span);\n   }\n}\n\n#endif\n"));
  edit("src/kosmickrisp/vulkan/kk_cmd_buffer.h", source => source
    .replace("   bool leased;\n};\n\nstruct kk_cmd_buffer {\n   struct vk_command_buffer vk;\n   struct kk_queue *scene_queue;\n",
      () => "   bool leased;\n};\n\n#include \"kk_mesh_expand.h\"\n\nstruct kk_cmd_buffer {\n   struct vk_command_buffer vk;\n   struct kk_queue *scene_queue;\n")
    .replace("   struct util_dynarray mesh_command_buffers;\n   struct util_dynarray mesh_allocators;\n   uint32_t mesh_batches_in_part, mesh_draws_in_part;\n   /* Nonzero only in a submit-owned, bounded indirect Mesh continuation. */\n   struct {\n      bool active;\n      uint32_t groups, base, draw, maximum;\n",
      () => "   struct util_dynarray mesh_command_buffers;\n   struct util_dynarray mesh_allocators;\n   uint32_t mesh_batches_in_part, mesh_draws_in_part;\n   /* Nonzero only in a submit-owned, bounded Task/Mesh continuation. */\n   struct {\n      bool active;\n      uint32_t groups, base, draw, maximum;\n")
    .replace("   struct {\n      bool active;\n      uint32_t groups, base, draw, maximum;\n   } mesh_chunk;\n   /* Pending timestamp resolves (struct kk_ts_resolve), flushed at cs_end. */\n   bool writable_msaa_meta;\n",
      () => "   struct {\n      bool active;\n      uint32_t groups, base, draw, maximum;\n      struct kk_mesh_exact_batch exact;\n   } mesh_chunk;\n   /* Pending timestamp resolves (struct kk_ts_resolve), flushed at cs_end. */\n   bool writable_msaa_meta;\n"));
  edit("src/kosmickrisp/vulkan/kk_dgc.h", source => source
    .replace("#include <stdint.h>\n#include <vulkan/vulkan_core.h>\n#include \"kosmickrisp/libkk/kk_mesh_types.h\"\n\n#define KK_DGC_PIPELINE_COUNT 4096u\n#define KK_DGC_PIPELINE_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)\n",
      () => "#include <stdint.h>\n#include <vulkan/vulkan_core.h>\n#include \"kosmickrisp/libkk/kk_mesh_types.h\"\n#include \"kk_mesh_expand.h\"\n\n#define KK_DGC_PIPELINE_COUNT 4096u\n#define KK_DGC_PIPELINE_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)\n"));
  edit("src/kosmickrisp/vulkan/kk_dgc.m", source => source
    .replace("}\n\n#include \"kk_dgc_mesh_bridge.h\"\n\n#include \"kk_dgc_pipeline_bridge.h\"\n",
      () => "}\n\n#include \"kk_dgc_mesh_bridge.h\"\n#include \"kk_mesh_expand_bridge.h\"\n\n#include \"kk_dgc_pipeline_bridge.h\"\n"));
  edit("src/kosmickrisp/vulkan/kk_dgc_mesh_bridge.h", source => source
    .replace("   mtl_library *library;\n   mtl_compute_pipeline_state *pipeline[2];\n   bool failed[2];\n};\n\nstatic char *\n",
      () => "   mtl_library *library;\n   mtl_compute_pipeline_state *pipeline[2];\n   bool failed[2];\n   mtl_library *expand_library;\n   mtl_compute_pipeline_state *expand_pipeline;\n   bool expand_failed;\n};\n\nstatic char *\n")
    .replace("{\n   if (!mesh) return;\n   for (unsigned i = 0; i < 2; ++i) mtl_release(mesh->pipeline[i]);\n   mtl_release(mesh->library);\n   free(mesh);\n}\n",
      () => "{\n   if (!mesh) return;\n   for (unsigned i = 0; i < 2; ++i) mtl_release(mesh->pipeline[i]);\n   mtl_release(mesh->expand_pipeline);\n   mtl_release(mesh->expand_library);\n   mtl_release(mesh->library);\n   free(mesh);\n}\n"));
  edits.set("src/kosmickrisp/vulkan/kk_mesh_expand.h", added("src/kosmickrisp/vulkan/kk_mesh_expand.h"));
  edits.set("src/kosmickrisp/vulkan/kk_mesh_expand_bridge.h", added("src/kosmickrisp/vulkan/kk_mesh_expand_bridge.h"));
  edits.set("src/kosmickrisp/vulkan/kk_mesh_expand_shader.h", added("src/kosmickrisp/vulkan/kk_mesh_expand_shader.h"));
}

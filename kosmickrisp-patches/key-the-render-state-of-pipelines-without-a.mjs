// SPDX-License-Identifier: MIT
// Key the render state of pipelines without a fragment stage
export const id = "key-the-render-state-of-pipelines-without-a";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_shader.c", source => source
    .replace("      _mesa_blake3_update(&blake3_ctx, &state->mv->view_mask,\n                          sizeof(state->mv->view_mask));\n\n   const uint32_t feature_key[] = {\n      (KK_SHADER_INFO_CACHE_VERSION << 16) | KK_FEAT_COUNT,\n      sizeof(struct kk_shader_info),\n",
      () => "      _mesa_blake3_update(&blake3_ctx, &state->mv->view_mask,\n                          sizeof(state->mv->view_mask));\n\n   /* Without a fragment stage the pipeline links an empty one, and its last\n    * pre-raster shader holds the native render pipeline and depth/stencil\n    * state built from the render pass, depth/stencil, blend and multisample\n    * state. Key that state as for an application fragment stage, so pipelines\n    * that share pre-raster code never share another pipeline's state. */\n   if (!(stages & VK_SHADER_STAGE_FRAGMENT_BIT) && state && state->rp) {\n      struct kk_fs_key key;\n      kk_populate_fs_key(&key, state);\n      _mesa_blake3_update(&blake3_ctx, &key, sizeof(key));\n   }\n\n   const uint32_t feature_key[] = {\n      (KK_SHADER_INFO_CACHE_VERSION << 16) | KK_FEAT_COUNT,\n      sizeof(struct kk_shader_info),\n"));
}

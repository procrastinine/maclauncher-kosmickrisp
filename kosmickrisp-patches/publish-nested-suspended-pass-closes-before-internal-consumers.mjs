// SPDX-License-Identifier: MIT
// Publish nested suspended-pass closes before internal consumers
export const id = "publish-nested-suspended-pass-closes-before-internal-consumers";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_cmd_buffer.c", source => source
    .replace("cs_end_internal(struct kk_cmd_buffer *cmd, bool ordered)\n{\n   assert(cmd);\n   kk_whole_pass_flush_suspended(cmd);\n\n   /* Render first as it may open a compute encoder for post render writes. */\n   bool ordered_compute = ordered;\n",
      () => "cs_end_internal(struct kk_cmd_buffer *cmd, bool ordered)\n{\n   assert(cmd);\n   const bool publish_suspended = ordered && cmd->whole_pass.suspended;\n   kk_whole_pass_flush_suspended(cmd);\n   if (publish_suspended) {\n      /* The saved EndRendering backend can recursively close the render pass\n       * and its meta resolves before this ordered close resumes. Those closed\n       * encoders can only publish through a later consumer now. */\n      cmd->pending_barrier.src |= cmd->closed_encoder_stages;\n      cmd->pending_barrier.dst = MTL_STAGE_ALL;\n      cmd->pending_barrier.visibility = MAX2(cmd->pending_barrier.visibility,\n                                            MTL_VISIBILITY_DEVICE);\n   }\n\n   /* Render first as it may open a compute encoder for post render writes. */\n   bool ordered_compute = ordered;\n")
    .replace("   if (!encoder) {\n      cmd->pending_barrier.src |= scope.src;\n      cmd->pending_barrier.dst |= scope.dst;\n      cmd->pending_barrier.visibility |= scope.visibility;\n      return;\n   }\n   const enum mtl_stages supported =\n",
      () => "   if (!encoder) {\n      cmd->pending_barrier.src |= scope.src;\n      cmd->pending_barrier.dst |= scope.dst;\n      cmd->pending_barrier.visibility =\n         MAX2(cmd->pending_barrier.visibility, scope.visibility);\n      return;\n   }\n   const enum mtl_stages supported =\n"));
}

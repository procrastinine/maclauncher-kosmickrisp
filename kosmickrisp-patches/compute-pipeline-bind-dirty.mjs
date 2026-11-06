// SPDX-License-Identifier: MIT
// Ordinary compute dispatches preserve pipeline binding until internal work.
export const id = 'compute-pipeline-bind-dirty';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', source => source.replace(
    'struct kk_compute_state {\n   struct kk_descriptor_state descriptors;',
    'struct kk_compute_state {\n   struct kk_descriptor_state descriptors;\n   mtl_compute_pipeline_state *bound_pipeline;').replace(
    'mtl_compute_encoder *cs_get_compute(struct kk_cmd_buffer *cmd);',
    'mtl_compute_encoder *cs_get_compute(struct kk_cmd_buffer *cmd);\nmtl_compute_encoder *cs_get_compute_for_dispatch(struct kk_cmd_buffer *cmd);'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', source => source.replace(
    'mtl_compute_encoder *\ncs_get_compute(struct kk_cmd_buffer *cmd)',
    'static mtl_compute_encoder *\ncs_get_compute_internal(struct kk_cmd_buffer *cmd, bool preserve_pipeline)').replace(
    '      cmd->metal.compute = mtl_new_compute_command_encoder(cmd->metal.cmd_buf);',
    '      cmd->state.cs.bound_pipeline = NULL;\n      cmd->metal.compute = mtl_new_compute_command_encoder(cmd->metal.cmd_buf);').replace(
    '   return cmd->metal.compute;\n}\n\nstatic void\nend_encoder',
    '   if (!preserve_pipeline)\n      cmd->state.cs.bound_pipeline = NULL;\n   return cmd->metal.compute;\n}\n\n' +
    'mtl_compute_encoder *\ncs_get_compute(struct kk_cmd_buffer *cmd)\n{\n' +
    '   /* Internal compute work may bind a different pipeline directly. */\n' +
    '   return cs_get_compute_internal(cmd, false);\n}\n\n' +
    'mtl_compute_encoder *\ncs_get_compute_for_dispatch(struct kk_cmd_buffer *cmd)\n{\n' +
    '   return cs_get_compute_internal(cmd, true);\n}\n\nstatic void\nend_encoder'));
  edit('src/kosmickrisp/vulkan/kk_cmd_dispatch.c', source => source.replaceAll(
    'cs_get_compute(cmd)', 'cs_get_compute_for_dispatch(cmd)').replace(
    '   mtl_compute_set_pipeline_state(\n      encoder, cmd->state.shaders[MESA_SHADER_COMPUTE]->pipeline.cs);',
    '   mtl_compute_pipeline_state *pipeline =\n      cmd->state.shaders[MESA_SHADER_COMPUTE]->pipeline.cs;\n' +
    '   if (cmd->state.cs.bound_pipeline != pipeline ||\n' +
    '       (cmd->state.dirty_shaders & BITFIELD_BIT(MESA_SHADER_COMPUTE))) {\n' +
    '      mtl_compute_set_pipeline_state(encoder, pipeline);\n' +
    '      cmd->state.cs.bound_pipeline = pipeline;\n' +
    '   }\n' +
    '   cmd->state.dirty_shaders &= ~BITFIELD_BIT(MESA_SHADER_COMPUTE);'));
}

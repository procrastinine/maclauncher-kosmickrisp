// SPDX-License-Identifier: MIT
// Keep constant records on retained Mesh fragment copies
export const id = "keep-constant-records-on-retained-mesh-fragment-copies";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/bridge/mtl_compiler.h", source => source
    .replace("mtl_render_pipeline_descriptor *mtl_sampler_emulation_render_descriptor(\n   mtl_render_pipeline_descriptor *descriptor, uint32_t stages,\n   uint32_t enabled_stages);\n\n/* Compute pipeline */\nmtl_compute_pipeline_state *\n",
      () => "mtl_render_pipeline_descriptor *mtl_sampler_emulation_render_descriptor(\n   mtl_render_pipeline_descriptor *descriptor, uint32_t stages,\n   uint32_t enabled_stages);\n/* Copies a render or Mesh pipeline descriptor with a new fragment function,\n * keeping the archive key's record of the other stages' constant values. */\nmtl_render_pipeline_descriptor *mtl_copy_render_pipeline_descriptor_with_fragment(\n   mtl_render_pipeline_descriptor *descriptor, mtl_function_descriptor *fragment);\n\n/* Compute pipeline */\nmtl_compute_pipeline_state *\n"));
  edit("src/kosmickrisp/bridge/mtl_compiler.m", source => source
    .replace("   }\n}\n\n/* Compute pipeline */\nmtl_compute_pipeline_state *\nmtl_new_compute_pipeline_state(mtl_compiler *compiler,\n",
      () => "   }\n}\n\nmtl_render_pipeline_descriptor *\nmtl_copy_render_pipeline_descriptor_with_fragment(mtl_render_pipeline_descriptor *descriptor,\n                                                  mtl_function_descriptor *fragment)\n{\n   @autoreleasepool {\n      id copy = [(id)descriptor copy];\n      if (!copy) return NULL;\n      kk_carry_constant_record(copy, (id)descriptor);\n      [(MTL4RenderPipelineDescriptor *)copy setFragmentFunctionDescriptor:\n         (MTL4FunctionDescriptor *)fragment];\n      kk_record_stage_constants(copy, @\"fragmentFunctionDescriptor\", (id)fragment, false);\n      return copy;\n   }\n}\n\n/* Compute pipeline */\nmtl_compute_pipeline_state *\nmtl_new_compute_pipeline_state(mtl_compiler *compiler,\n"));
  edit("src/kosmickrisp/bridge/mtl_mesh.m", source => source
    .replace("         (MTL4MeshRenderPipelineDescriptor *)batch->raster_descriptor;\n      if (!reader.meshFunctionDescriptor || reader.objectFunctionDescriptor)\n         return NULL;\n      MTL4MeshRenderPipelineDescriptor *descriptor = [reader copy];\n      descriptor.fragmentFunctionDescriptor = (MTL4FunctionDescriptor *)fragment;\n      return (mtl_render_pipeline_descriptor *)descriptor;\n   }\n}\n",
      () => "         (MTL4MeshRenderPipelineDescriptor *)batch->raster_descriptor;\n      if (!reader.meshFunctionDescriptor || reader.objectFunctionDescriptor)\n         return NULL;\n      return mtl_copy_render_pipeline_descriptor_with_fragment(reader, fragment);\n   }\n}\n"));
  edit("src/kosmickrisp/bridge/stubs/mtl_compiler.c", source => source
    .replace("   return NULL;\n}\n\n/* Compute pipeline */\n\nmtl_compute_pipeline_state *\n",
      () => "   return NULL;\n}\n\nmtl_render_pipeline_descriptor *\nmtl_copy_render_pipeline_descriptor_with_fragment(mtl_render_pipeline_descriptor *descriptor,\n                                                  mtl_function_descriptor *fragment)\n{\n   return NULL;\n}\n\n/* Compute pipeline */\n\nmtl_compute_pipeline_state *\n"));
}

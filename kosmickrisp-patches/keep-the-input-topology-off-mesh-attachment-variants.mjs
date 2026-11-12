// SPDX-License-Identifier: MIT
// Keep the input topology off mesh attachment variants
export const id = "keep-the-input-topology-off-mesh-attachment-variants";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/bridge/mtl_compiler.m", source => source
    .replace("   @autoreleasepool {\n      MTL4RenderPipelineDescriptor *copy = [(MTL4RenderPipelineDescriptor *)descriptor copy];\n      copy.rasterSampleCount = samples;\n      if (topology != MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED)\n         copy.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)topology;\n      if (framebuffer_reads) {\n         MTL4SpecializedFunctionDescriptor *fragment =\n",
      () => "   @autoreleasepool {\n      MTL4RenderPipelineDescriptor *copy = [(MTL4RenderPipelineDescriptor *)descriptor copy];\n      copy.rasterSampleCount = samples;\n      /* Mesh pipeline descriptors have no input primitive topology. */\n      if (topology != MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED &&\n          [copy isKindOfClass:[MTL4RenderPipelineDescriptor class]])\n         copy.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)topology;\n      if (framebuffer_reads) {\n         MTL4SpecializedFunctionDescriptor *fragment =\n"));
}

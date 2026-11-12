// SPDX-License-Identifier: MIT
// Hold Mesh multiview reporting for replay corrections
export const id = "hold-mesh-multiview-reporting-for-replay-corrections";
export function apply({edit, edits}) {
  edit("docs/drivers/kosmickrisp.rst", source => source
    .replace("KosmicKrisp is a Vulkan 1.4 driver for Apple GPUs on macOS. It is implemented\non top of Metal 4. iOS is not supported.\n\nMesh multiview\n**************\n\nTask and Mesh pipelines support up to 32 multiview views when the\n``mesh_shader`` entry is selected. Each set bit of the view mask executes\nwith its original ViewIndex and writes that index as the render-target layer.\nMesh draws use this per-view path without native vertex amplification.\nThe feature and its view-count limit follow the same mode and family selection\nas Mesh shading.\n\nSystem requirements\n*******************\n\n",
      () => "KosmicKrisp is a Vulkan 1.4 driver for Apple GPUs on macOS. It is implemented\non top of Metal 4. iOS is not supported.\n\nSystem requirements\n*******************\n\n"));
  edit("src/kosmickrisp/vulkan/kk_physical_device.c", source => source
    .replace("      .pipelineStatisticsQuery = true,\n      .taskShader = true,\n      .meshShader = true,\n      .multiviewMeshShader = true,\n      .meshShaderQueries = true,\n      .primitiveFragmentShadingRateMeshShader = true,\n      .computeDerivativeGroupLinear = true,\n",
      () => "      .pipelineStatisticsQuery = true,\n      .taskShader = true,\n      .meshShader = true,\n      .multiviewMeshShader = false,\n      .meshShaderQueries = true,\n      .primitiveFragmentShadingRateMeshShader = true,\n      .computeDerivativeGroupLinear = true,\n")
    .replace("      .maxMeshOutputVertices = 256,\n      .maxMeshOutputPrimitives = 512,\n      .maxMeshOutputLayers = 8,\n      .maxMeshMultiviewViewCount = KK_MAX_MULTIVIEW_VIEW_COUNT,\n      .meshOutputPerVertexGranularity = 1,\n      .meshOutputPerPrimitiveGranularity = 1,\n      .maxPreferredTaskWorkGroupInvocations = 32,\n",
      () => "      .maxMeshOutputVertices = 256,\n      .maxMeshOutputPrimitives = 512,\n      .maxMeshOutputLayers = 8,\n      .maxMeshMultiviewViewCount = 0,\n      .meshOutputPerVertexGranularity = 1,\n      .meshOutputPerPrimitiveGranularity = 1,\n      .maxPreferredTaskWorkGroupInvocations = 32,\n"));
}

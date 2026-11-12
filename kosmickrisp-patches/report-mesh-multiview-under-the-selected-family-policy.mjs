// SPDX-License-Identifier: MIT
// Report Mesh multiview under the selected family policy
export const id = "report-mesh-multiview-under-the-selected-family-policy";
export function apply({edit, edits}) {
  edit("docs/drivers/kosmickrisp.rst", source => source
    .replace("sized from the shader declaration. Prefix sums of the actual child grids map\nMesh groups back to their parent Task group.\n\nOn Apple9 and later, pipelines without captured flat outputs expand the real\nMesh count into native indirect commands and execute a GPU-written range.\nOther pipelines use indirect Mesh draws. A completed batch publishes its real\n",
      () => "sized from the shader declaration. Prefix sums of the actual child grids map\nMesh groups back to their parent Task group.\n\nMesh multiview executes each original view with its explicit layer and view\nindex, supporting up to 32 views. The feature and limit follow the selected\nMesh entry on each family. Attachment clears use explicit layer rectangles\nwith a single-view native pipeline.\n\nOn Apple9 and later, pipelines without captured flat outputs expand the real\nMesh count into native indirect commands and execute a GPU-written range.\nOther pipelines use indirect Mesh draws. A completed batch publishes its real\n"));
  edit("src/kosmickrisp/vulkan/kk_physical_device.c", source => source
    .replace("      .pipelineStatisticsQuery = true,\n      .taskShader = true,\n      .meshShader = true,\n      .multiviewMeshShader = false,\n      .meshShaderQueries = true,\n      .primitiveFragmentShadingRateMeshShader = true,\n      .computeDerivativeGroupLinear = true,\n",
      () => "      .pipelineStatisticsQuery = true,\n      .taskShader = true,\n      .meshShader = true,\n      .multiviewMeshShader = true,\n      .meshShaderQueries = true,\n      .primitiveFragmentShadingRateMeshShader = true,\n      .computeDerivativeGroupLinear = true,\n")
    .replace("      .maxMeshOutputVertices = 256,\n      .maxMeshOutputPrimitives = 512,\n      .maxMeshOutputLayers = 8,\n      .maxMeshMultiviewViewCount = 0,\n      .meshOutputPerVertexGranularity = 1,\n      .meshOutputPerPrimitiveGranularity = 1,\n      .maxPreferredTaskWorkGroupInvocations = 32,\n",
      () => "      .maxMeshOutputVertices = 256,\n      .maxMeshOutputPrimitives = 512,\n      .maxMeshOutputLayers = 8,\n      .maxMeshMultiviewViewCount = KK_MAX_MULTIVIEW_VIEW_COUNT,\n      .meshOutputPerVertexGranularity = 1,\n      .meshOutputPerPrimitiveGranularity = 1,\n      .maxPreferredTaskWorkGroupInvocations = 32,\n"));
}

// SPDX-License-Identifier: MIT
// Build the adapted Mesa tree directly and document the driver
import fs from 'node:fs';
export const id = "build-the-adapted-mesa-tree-directly-and-document";
export const sourceFiles = [
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/LICENSE",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/MESA-LICENSE.rst",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/README.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/ADAPTATIONS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/BUILDING.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/SOFT-FP64-LICENSE.txt",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/arm64.ini",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_ATOMIC_INT64.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_ATTACHMENT_ACCESS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_CLIP_DISTANCES.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_COMPILE_OPTIONS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_COMPUTE_DERIVATIVES.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_CONSERVATIVE_RASTERIZATION.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_CUBE_FILTERING.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_CULL_DISTANCE.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_DEPTH_BIAS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_DEPTH_STENCIL_STATE.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_DEVICE_ADDRESS_COMMANDS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_DEVICE_GENERATED_COMMANDS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_EXTENDED_FLAGS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_EXTERNAL_MEMORY.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_FORMATS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_FP64.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_GEOMETRY_BOUNDS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_GRAPHICS_STAGES.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_HELPER_WRITES.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_IMAGE_ALIASING.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_IMAGE_ATOMIC_INT64.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_IMAGE_VIEWS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_INDIRECT_TESSELLATION.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_INTERLOCK.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_LEGACY_RASTER.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_LONG_VECTORS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_MATRIX_TILES.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_MESH_SHADERS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_MSRTSS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_MULTISAMPLE.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_POLY_LAYER_REPLAY.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_QUERIES.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_QUEUES.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_REQUIRED_SUBGROUP_SIZE.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_SAMPLERS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_SAMPLER_MINMAX.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_SHADER_CACHE.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_SPARSE.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_TRANSIENT_ATTACHMENTS.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_VERTEX_INPUT.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/KOSMICKRISP_WRITABLE_MSAA.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-barycentric.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-descriptor-heaps.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-descriptor-layout.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-independent-interfaces.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-memory.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-mesh-pipeline-api.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-retained-raster.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-sampler-parameters.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-shader-cache.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-varyings.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/reference/kosmickrisp-zero-initialization.md",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/sparse-notices/LICENSE.Apache-2.0",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/sparse-notices/NOTICE",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/maclauncher/x86_64.ini",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/src/kosmickrisp/ci/kosmickrisp-fails.txt",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/src/kosmickrisp/ci/kosmickrisp-flakes.txt",
  "kosmickrisp-patches/build-the-adapted-mesa-tree-directly-and-document/src/kosmickrisp/ci/kosmickrisp-skips.txt"
];
const added = file => fs.readFileSync(new URL("./build-the-adapted-mesa-tree-directly-and-document/" + file, import.meta.url), 'utf8');
export function apply({edit, edits}) {
  edits.set("LICENSE", added("LICENSE"));
  edits.set("MESA-LICENSE.rst", added("MESA-LICENSE.rst"));
  edits.set("README.md", added("README.md"));
  edits.set("maclauncher/ADAPTATIONS.md", added("maclauncher/ADAPTATIONS.md"));
  edits.set("maclauncher/BUILDING.md", added("maclauncher/BUILDING.md"));
  edits.set("maclauncher/SOFT-FP64-LICENSE.txt", added("maclauncher/SOFT-FP64-LICENSE.txt"));
  edits.set("maclauncher/arm64.ini", added("maclauncher/arm64.ini"));
  edits.set("maclauncher/reference/KOSMICKRISP_ATOMIC_INT64.md", added("maclauncher/reference/KOSMICKRISP_ATOMIC_INT64.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_ATTACHMENT_ACCESS.md", added("maclauncher/reference/KOSMICKRISP_ATTACHMENT_ACCESS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_CLIP_DISTANCES.md", added("maclauncher/reference/KOSMICKRISP_CLIP_DISTANCES.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_COMPILE_OPTIONS.md", added("maclauncher/reference/KOSMICKRISP_COMPILE_OPTIONS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_COMPUTE_DERIVATIVES.md", added("maclauncher/reference/KOSMICKRISP_COMPUTE_DERIVATIVES.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_CONSERVATIVE_RASTERIZATION.md", added("maclauncher/reference/KOSMICKRISP_CONSERVATIVE_RASTERIZATION.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_CUBE_FILTERING.md", added("maclauncher/reference/KOSMICKRISP_CUBE_FILTERING.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_CULL_DISTANCE.md", added("maclauncher/reference/KOSMICKRISP_CULL_DISTANCE.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_DEPTH_BIAS.md", added("maclauncher/reference/KOSMICKRISP_DEPTH_BIAS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_DEPTH_STENCIL_STATE.md", added("maclauncher/reference/KOSMICKRISP_DEPTH_STENCIL_STATE.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_DEVICE_ADDRESS_COMMANDS.md", added("maclauncher/reference/KOSMICKRISP_DEVICE_ADDRESS_COMMANDS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_DEVICE_GENERATED_COMMANDS.md", added("maclauncher/reference/KOSMICKRISP_DEVICE_GENERATED_COMMANDS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_EXTENDED_FLAGS.md", added("maclauncher/reference/KOSMICKRISP_EXTENDED_FLAGS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_EXTERNAL_MEMORY.md", added("maclauncher/reference/KOSMICKRISP_EXTERNAL_MEMORY.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_FORMATS.md", added("maclauncher/reference/KOSMICKRISP_FORMATS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_FP64.md", added("maclauncher/reference/KOSMICKRISP_FP64.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_GEOMETRY_BOUNDS.md", added("maclauncher/reference/KOSMICKRISP_GEOMETRY_BOUNDS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_GRAPHICS_STAGES.md", added("maclauncher/reference/KOSMICKRISP_GRAPHICS_STAGES.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_HELPER_WRITES.md", added("maclauncher/reference/KOSMICKRISP_HELPER_WRITES.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_IMAGE_ALIASING.md", added("maclauncher/reference/KOSMICKRISP_IMAGE_ALIASING.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_IMAGE_ATOMIC_INT64.md", added("maclauncher/reference/KOSMICKRISP_IMAGE_ATOMIC_INT64.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_IMAGE_VIEWS.md", added("maclauncher/reference/KOSMICKRISP_IMAGE_VIEWS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_INDIRECT_TESSELLATION.md", added("maclauncher/reference/KOSMICKRISP_INDIRECT_TESSELLATION.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_INTERLOCK.md", added("maclauncher/reference/KOSMICKRISP_INTERLOCK.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_LEGACY_RASTER.md", added("maclauncher/reference/KOSMICKRISP_LEGACY_RASTER.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_LONG_VECTORS.md", added("maclauncher/reference/KOSMICKRISP_LONG_VECTORS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_MATRIX_TILES.md", added("maclauncher/reference/KOSMICKRISP_MATRIX_TILES.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_MESH_SHADERS.md", added("maclauncher/reference/KOSMICKRISP_MESH_SHADERS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_MSRTSS.md", added("maclauncher/reference/KOSMICKRISP_MSRTSS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_MULTISAMPLE.md", added("maclauncher/reference/KOSMICKRISP_MULTISAMPLE.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_POLY_LAYER_REPLAY.md", added("maclauncher/reference/KOSMICKRISP_POLY_LAYER_REPLAY.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_QUERIES.md", added("maclauncher/reference/KOSMICKRISP_QUERIES.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_QUEUES.md", added("maclauncher/reference/KOSMICKRISP_QUEUES.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_REQUIRED_SUBGROUP_SIZE.md", added("maclauncher/reference/KOSMICKRISP_REQUIRED_SUBGROUP_SIZE.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_SAMPLERS.md", added("maclauncher/reference/KOSMICKRISP_SAMPLERS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_SAMPLER_MINMAX.md", added("maclauncher/reference/KOSMICKRISP_SAMPLER_MINMAX.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_SHADER_CACHE.md", added("maclauncher/reference/KOSMICKRISP_SHADER_CACHE.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_SPARSE.md", added("maclauncher/reference/KOSMICKRISP_SPARSE.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_TRANSIENT_ATTACHMENTS.md", added("maclauncher/reference/KOSMICKRISP_TRANSIENT_ATTACHMENTS.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_VERTEX_INPUT.md", added("maclauncher/reference/KOSMICKRISP_VERTEX_INPUT.md"));
  edits.set("maclauncher/reference/KOSMICKRISP_WRITABLE_MSAA.md", added("maclauncher/reference/KOSMICKRISP_WRITABLE_MSAA.md"));
  edits.set("maclauncher/reference/kosmickrisp-barycentric.md", added("maclauncher/reference/kosmickrisp-barycentric.md"));
  edits.set("maclauncher/reference/kosmickrisp-descriptor-heaps.md", added("maclauncher/reference/kosmickrisp-descriptor-heaps.md"));
  edits.set("maclauncher/reference/kosmickrisp-descriptor-layout.md", added("maclauncher/reference/kosmickrisp-descriptor-layout.md"));
  edits.set("maclauncher/reference/kosmickrisp-independent-interfaces.md", added("maclauncher/reference/kosmickrisp-independent-interfaces.md"));
  edits.set("maclauncher/reference/kosmickrisp-memory.md", added("maclauncher/reference/kosmickrisp-memory.md"));
  edits.set("maclauncher/reference/kosmickrisp-mesh-pipeline-api.md", added("maclauncher/reference/kosmickrisp-mesh-pipeline-api.md"));
  edits.set("maclauncher/reference/kosmickrisp-retained-raster.md", added("maclauncher/reference/kosmickrisp-retained-raster.md"));
  edits.set("maclauncher/reference/kosmickrisp-sampler-parameters.md", added("maclauncher/reference/kosmickrisp-sampler-parameters.md"));
  edits.set("maclauncher/reference/kosmickrisp-shader-cache.md", added("maclauncher/reference/kosmickrisp-shader-cache.md"));
  edits.set("maclauncher/reference/kosmickrisp-varyings.md", added("maclauncher/reference/kosmickrisp-varyings.md"));
  edits.set("maclauncher/reference/kosmickrisp-zero-initialization.md", added("maclauncher/reference/kosmickrisp-zero-initialization.md"));
  edits.set("maclauncher/sparse-notices/LICENSE.Apache-2.0", added("maclauncher/sparse-notices/LICENSE.Apache-2.0"));
  edits.set("maclauncher/sparse-notices/NOTICE", added("maclauncher/sparse-notices/NOTICE"));
  edits.set("maclauncher/x86_64.ini", added("maclauncher/x86_64.ini"));
  edits.set("src/kosmickrisp/ci/kosmickrisp-fails.txt", added("src/kosmickrisp/ci/kosmickrisp-fails.txt"));
  edits.set("src/kosmickrisp/ci/kosmickrisp-flakes.txt", added("src/kosmickrisp/ci/kosmickrisp-flakes.txt"));
  edits.set("src/kosmickrisp/ci/kosmickrisp-skips.txt", added("src/kosmickrisp/ci/kosmickrisp-skips.txt"));
}

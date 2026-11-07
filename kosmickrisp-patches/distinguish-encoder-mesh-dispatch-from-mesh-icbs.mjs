// SPDX-License-Identifier: MIT
// Distinguish encoder mesh dispatch from mesh ICBs
export const id = "distinguish-encoder-mesh-dispatch-from-mesh-icbs";
export function apply({edit, edits}) {
  edit("docs/drivers/kosmickrisp/features.rst", source => source
    .replace("     - Native with emulated cases: object and mesh shaders run natively.\n       Task grids above the native bound run as compute batches, and\n       oversized outputs are sliced.\n     - Apple7 for mesh shading, Apple9 for indirect mesh arguments; native\n       on all families.\n     - ``mesh_shader``\n     - On\n     - D3D12 mesh shader tier 1 and feature level 12_2 are unavailable.\n",
      () => "     - Native with emulated cases: object and mesh shaders run natively.\n       Task grids above the native bound run as compute batches, and\n       oversized outputs are sliced.\n     - Apple7 for mesh shading. Apple's table lists Apple9 for indirect\n       mesh arguments and mesh draws in indirect command buffers. The\n       Metal 4 encoder-level indirect path also runs on M2; this does not\n       enable mesh draws in indirect command buffers.\n     - ``mesh_shader``\n     - On\n     - D3D12 mesh shader tier 1 and feature level 12_2 are unavailable.\n"));
}

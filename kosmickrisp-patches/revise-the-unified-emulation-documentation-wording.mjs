// SPDX-License-Identifier: MIT
// Revise the unified emulation documentation wording
export const id = "revise-the-unified-emulation-documentation-wording";
export function apply({edit, edits}) {
  edit("docs/drivers/kosmickrisp.rst", source => source
    .replace("``conformance,-VK_KHR_ray_query`` removes ray-query advertisement. These removals\napply after named entry selection. Unknown tokens produce one stderr line per\ntoken and are ignored. The setting changes capability reports; rendering and\nshader compilation do not parse it. Ray tracing remains outside release scope.\n\n.. include:: kosmickrisp/emulation.rst\n\n",
      () => "``conformance,-VK_KHR_ray_query`` removes ray-query advertisement. These removals\napply after named entry selection. Unknown tokens produce one stderr line per\ntoken and are ignored. The setting changes capability reports; rendering and\nshader compilation do not parse it. Ray tracing requires explicit selection.\n\n.. include:: kosmickrisp/emulation.rst\n\n"));
}

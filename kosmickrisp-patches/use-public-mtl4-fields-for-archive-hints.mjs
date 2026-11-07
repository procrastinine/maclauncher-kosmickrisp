// SPDX-License-Identifier: MIT
// Use public MTL4 fields for archive hints
export const id = "use-public-mtl4-fields-for-archive-hints";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/bridge/mtl_pipeline_archive.h", source => source
    .replace("      if (!vertex || !fragment) return nil;\n      [fields addObjectsFromArray:@[@\"render\", vertex, fragment,\n         @(render.rasterSampleCount), @(render.inputPrimitiveTopology),\n         @(render.rasterizationEnabled), @(render.maxVertexAmplificationCount),\n         @(render.depthAttachmentPixelFormat), @(render.stencilAttachmentPixelFormat)]];\n      for (unsigned i = 0; i < 8; ++i)\n         [fields addObject:@[@(render.colorAttachments[i].pixelFormat),\n                             @(render.colorAttachments[i].writeMask)]];\n",
      () => "      if (!vertex || !fragment) return nil;\n      [fields addObjectsFromArray:@[@\"render\", vertex, fragment,\n         @(render.rasterSampleCount), @(render.inputPrimitiveTopology),\n         @(render.rasterizationEnabled), @(render.maxVertexAmplificationCount)]];\n      /* MTL4 has no depth/stencil-format fields on this descriptor. This\n       * advisory key never replaces Metal's complete descriptor verification. */\n      for (unsigned i = 0; i < 8; ++i)\n         [fields addObject:@[@(render.colorAttachments[i].pixelFormat),\n                             @(render.colorAttachments[i].writeMask)]];\n"));
}

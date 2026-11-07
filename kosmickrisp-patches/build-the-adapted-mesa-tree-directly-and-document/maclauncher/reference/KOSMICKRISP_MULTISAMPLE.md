# Dynamic multisampling

`VK_EXT_extended_dynamic_state3` exposes
`extendedDynamicState3RasterizationSamples`, `extendedDynamicState3SampleMask`
and `extendedDynamicState3AlphaToCoverageEnable`.

`vkCmdSetRasterizationSamplesEXT` selects the raster sample count.
`vkCmdSetSampleMaskEXT` sets coverage bits for those samples.
`vkCmdSetAlphaToCoverageEnableEXT` controls conversion of fragment alpha to
sample coverage. The commands take effect on subsequent draws in the current
command buffer. Supported sample counts follow the device's framebuffer
sample-count properties.

Raster sample count shares the cached Metal pipeline variants used by attachment
formats and polygon rendering. Changing the count inside an attachmentless
rendering scope restarts the native encoder with that count. Sample mask and
alpha-to-coverage are draw-time root values consumed by the shared fragment
lowering. Static values use the same mask and coverage implementation.

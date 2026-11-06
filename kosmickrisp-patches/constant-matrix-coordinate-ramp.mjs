// SPDX-License-Identifier: MIT
// The cooperative-matrix coordinate defaults are identical for every recording.
export const id = 'constant-matrix-coordinate-ramp';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', source => source.replace(
    '   for (unsigned i = 0; i < 64; i++)\n' +
    '      cmd->state.cs.descriptors.root.cs.matrix_coordinates[i] = _mesa_float_to_half((float)i);',
    "   /* Exact binary16 encodings of the immutable coordinate ramp 0..63. */\n   static const uint16_t matrix_coordinates[64] = {\n      0x0000, 0x3c00, 0x4000, 0x4200, 0x4400, 0x4500, 0x4600, 0x4700,\n      0x4800, 0x4880, 0x4900, 0x4980, 0x4a00, 0x4a80, 0x4b00, 0x4b80,\n      0x4c00, 0x4c40, 0x4c80, 0x4cc0, 0x4d00, 0x4d40, 0x4d80, 0x4dc0,\n      0x4e00, 0x4e40, 0x4e80, 0x4ec0, 0x4f00, 0x4f40, 0x4f80, 0x4fc0,\n      0x5000, 0x5020, 0x5040, 0x5060, 0x5080, 0x50a0, 0x50c0, 0x50e0,\n      0x5100, 0x5120, 0x5140, 0x5160, 0x5180, 0x51a0, 0x51c0, 0x51e0,\n      0x5200, 0x5220, 0x5240, 0x5260, 0x5280, 0x52a0, 0x52c0, 0x52e0,\n      0x5300, 0x5320, 0x5340, 0x5360, 0x5380, 0x53a0, 0x53c0, 0x53e0,\n   };\n   STATIC_ASSERT(sizeof(matrix_coordinates) ==\n                 sizeof(cmd->state.cs.descriptors.root.cs.matrix_coordinates));\n   memcpy(cmd->state.cs.descriptors.root.cs.matrix_coordinates,\n          matrix_coordinates, sizeof(matrix_coordinates));"));
}

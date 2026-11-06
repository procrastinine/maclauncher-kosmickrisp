// SPDX-License-Identifier: MIT
// Vulkan direct dispatch counts already describe complete workgroups.
export const id = 'direct-compute-threadgroups';
export function apply({edit}) {
  edit('src/kosmickrisp/bridge/mtl_encoder.h', source => source.replace(
    'void mtl_dispatch_threadgroups_with_indirect_buffer(',
    'void mtl_dispatch_threadgroups(mtl_compute_encoder *encoder,\n' +
    '                               struct mtl_size groups,\n' +
    '                               struct mtl_size local_size);\n\n' +
    'void mtl_dispatch_threadgroups_with_indirect_buffer('));
  edit('src/kosmickrisp/bridge/mtl_encoder.m', source => source.replace(
    'void\nmtl_dispatch_threadgroups_with_indirect_buffer(',
    "void\nmtl_dispatch_threadgroups(mtl_compute_encoder *encoder,\n                          struct mtl_size groups, struct mtl_size local_size)\n{\n   @autoreleasepool {\n      id<MTL4ComputeCommandEncoder> enc = (id<MTL4ComputeCommandEncoder>)encoder;\n      MTLSize threadgroups = MTLSizeMake(groups.x, groups.y, groups.z);\n      MTLSize threads_per_threadgroup = MTLSizeMake(local_size.x, local_size.y,\n                                                    local_size.z);\n      [enc dispatchThreadgroups:threadgroups\n             threadsPerThreadgroup:threads_per_threadgroup];\n   }\n}\n\n" + 'void\nmtl_dispatch_threadgroups_with_indirect_buffer('));
  edit('src/kosmickrisp/vulkan/kk_cmd_dispatch.c', source => source.replace(
    '      struct mtl_size grid_size = {\n' +
    '         .x = groupCountX * local_size.x,\n' +
    '         .y = groupCountY * local_size.y,\n' +
    '         .z = groupCountZ * local_size.z,\n' +
    '      };',
    '      struct mtl_size groups = {groupCountX, groupCountY, groupCountZ};').replace(
    '      mtl_dispatch_threads(encoder, grid_size, local_size);',
    '      mtl_dispatch_threadgroups(encoder, groups, local_size);'));
}

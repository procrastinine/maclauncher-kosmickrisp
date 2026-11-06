// SPDX-License-Identifier: MIT
// Native barycentric declarations adapted from Mesa !45585 (Sascha Scandella).
export const id = 'fragment-shader-barycentric';

export function apply({edit}) {
  edit("src/kosmickrisp/bridge/mtl_device.h", s => {
    s = s.replace(`bool mtl_device_supports_sample_count(mtl_device *dev, uint32_t sample_count);\n`, `bool mtl_device_supports_sample_count(mtl_device *dev, uint32_t sample_count);\nbool mtl_device_supports_shader_barycentric_coordinates(mtl_device *dev);\n`);
    return s;
  });
   edit("src/kosmickrisp/bridge/mtl_device.m", s => {
    s = s.replace(`   }
}

struct mtl_size
mtl_device_max_threads_per_threadgroup(mtl_device *dev)
{
`, `   }
}

bool
mtl_device_supports_shader_barycentric_coordinates(mtl_device *dev)
{
   @autoreleasepool {
      id<MTLDevice> device = (id<MTLDevice>)dev;
      return [device supportsShaderBarycentricCoordinates];
   }
}

struct mtl_size
mtl_device_max_threads_per_threadgroup(mtl_device *dev)
{
`);
    return s;
  });
   edit("src/kosmickrisp/bridge/stubs/mtl_device.c", s => {
    s = s.replace(`   return false;
}

struct mtl_size
mtl_device_max_threads_per_threadgroup(mtl_device *dev)
{
`, `   return false;
}

bool
mtl_device_supports_shader_barycentric_coordinates(mtl_device *dev)
{
   return false;
}

struct mtl_size
mtl_device_max_threads_per_threadgroup(mtl_device *dev)
{
`);
    return s;
  });
   edit("src/kosmickrisp/compiler/msl_nir_lower_common.c", s => {
    s = s.replace(`
   nir_builder b = nir_builder_at(nir_before_impl(entrypoint));

   nir_def *barycentric = nir_load_barycentric_coord_pixel(
      &b, 32u, .interp_mode = INTERP_MODE_SMOOTH);
   BITSET_SET(nir->info.system_values_read,
              SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD);

   nir_def *all_negative =
      nir_flt_imm(&b, nir_channel(&b, barycentric, 0u), 0.0);
`, `
   nir_builder b = nir_builder_at(nir_before_impl(entrypoint));

   bool use_linear = BITSET_TEST(nir->info.system_values_read,
                                 SYSTEM_VALUE_BARYCENTRIC_LINEAR_COORD);
   nir_def *barycentric = nir_load_barycentric_coord_pixel(
      &b, 32u,
      .interp_mode =
         use_linear ? INTERP_MODE_NOPERSPECTIVE : INTERP_MODE_SMOOTH);
   BITSET_SET(nir->info.system_values_read,
              use_linear ? SYSTEM_VALUE_BARYCENTRIC_LINEAR_COORD
                         : SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD);

   nir_def *all_negative =
      nir_flt_imm(&b, nir_channel(&b, barycentric, 0u), 0.0);
`);
    return s;
  });
   edit("src/kosmickrisp/compiler/nir_to_msl.c", s => {
    s = s.replace(`   [SYSTEM_VALUE_SAMPLE_MASK_IN] = "uint gl_SampleMask [[sample_mask]]",
   [SYSTEM_VALUE_PRIMITIVE_ID] = "uint gl_PrimitiveID [[primitive_id]]",
   [SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD] =
      "float3 gl_BaryCoord [[barycentric_coord]]",
   [SYSTEM_VALUE_AMPLIFICATION_ID_KK] =
      "uint mtl_AmplificationID [[amplification_id]]",
   [SYSTEM_VALUE_FIRST_VERTEX] = "uint gl_FirstVertex [[base_vertex]]",
`, `   [SYSTEM_VALUE_SAMPLE_MASK_IN] = "uint gl_SampleMask [[sample_mask]]",
   [SYSTEM_VALUE_PRIMITIVE_ID] = "uint gl_PrimitiveID [[primitive_id]]",
   [SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD] =
      "float3 gl_BaryCoord [[barycentric_coord, center_perspective]]",
   [SYSTEM_VALUE_BARYCENTRIC_LINEAR_COORD] =
      "float3 gl_BaryCoordNoPersp [[barycentric_coord, center_no_perspective]]",
   [SYSTEM_VALUE_AMPLIFICATION_ID_KK] =
      "uint mtl_AmplificationID [[amplification_id]]",
   [SYSTEM_VALUE_FIRST_VERTEX] = "uint gl_FirstVertex [[base_vertex]]",
`);
    s = s.replace(`      P(ctx, "gl_PrimitiveID;\\n");
      break;
   case nir_intrinsic_load_barycentric_coord_pixel:
      P(ctx, "gl_BaryCoord;\\n");
      break;
   case nir_intrinsic_load_sample_pos:
      P(ctx, "get_sample_position(gl_SampleID);\\n");
`, `      P(ctx, "gl_PrimitiveID;\\n");
      break;
   case nir_intrinsic_load_barycentric_coord_pixel:
      if (nir_intrinsic_interp_mode(instr) == INTERP_MODE_NOPERSPECTIVE) {
         P(ctx, "gl_BaryCoordNoPersp;\\n");
      } else {
         P(ctx, "gl_BaryCoord;\\n");
      }
      break;
   case nir_intrinsic_load_sample_pos:
      P(ctx, "get_sample_position(gl_SampleID);\\n");
`);
    return s;
  });
   edit("src/kosmickrisp/vulkan/kk_physical_device.c", s => {
    s = s.replace(`      /* Optional extensions */
      .KHR_calibrated_timestamps = true,
      .KHR_deferred_host_operations = true,
      .KHR_maintenance7 = true,
      .KHR_maintenance8 = true,
      .KHR_maintenance9 = true,
`, `      /* Optional extensions */
      .KHR_calibrated_timestamps = true,
      .KHR_deferred_host_operations = true,
      .KHR_fragment_shader_barycentric =
         mtl_device_supports_shader_barycentric_coordinates(
            pdev->mtl_dev_handle),
      .KHR_maintenance7 = true,
      .KHR_maintenance8 = true,
      .KHR_maintenance9 = true,
`);
    s = s.replace(`      /* VK_EXT_mutable_descriptor_type */
      .mutableDescriptorType = true,

      /* VK_KHR_maintenance7 */
      .maintenance7 = true,

`, `      /* VK_EXT_mutable_descriptor_type */
      .mutableDescriptorType = true,

      /* VK_KHR_fragment_shader_barycentric */
      .fragmentShaderBarycentric =
         supported_extensions->KHR_fragment_shader_barycentric,

      /* VK_KHR_maintenance7 */
      .maintenance7 = true,

`);
    return s;
  });
   edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = s.replace(`                   SYSTEM_VALUE_BARYCENTRIC_LINEAR_CENTROID);
      BITSET_CLEAR(nir->info.system_values_read,
                   SYSTEM_VALUE_BARYCENTRIC_LINEAR_SAMPLE);
   }
}

`, `                   SYSTEM_VALUE_BARYCENTRIC_LINEAR_CENTROID);
      BITSET_CLEAR(nir->info.system_values_read,
                   SYSTEM_VALUE_BARYCENTRIC_LINEAR_SAMPLE);

      /* Perspective [[barycentric_coord]] returns wrong values unless
       * [[position]] is also declared. */
      if (BITSET_TEST(nir->info.system_values_read,
                      SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD))
         BITSET_SET(nir->info.system_values_read, SYSTEM_VALUE_FRAG_COORD);
   }
}

`);
    return s;
  });
}

/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_RETAINED_POINTS_NIR_H
#define KK_POLY_RETAINED_POINTS_NIR_H

/* Raster-only reader of the existing ordered polygon output. The shared
 * capture/clip/prefix/output kernels already retained one packet per surviving
 * point and the matching indirect draw count. There is no producer NIR here. */
static nir_shader *
kk_polygon_retained_point_shader(const struct poly_gs_capture_info *info,
                                  const nir_shader_compiler_options *options)
{
   assert(info->primitive_vertices == 1);
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options,
                                                 "Poly retained point reader");
   nir_shader *nir = b.shader;
   nir->info.io_lowered = true;
   nir->info.outputs_written = info->outputs;
   nir_def *id = nir_load_vertex_id(&b);
   nir_def *address = nir_iadd(&b,
      kk_polygon_load(&b, 1, 64, offsetof(struct poly_geometry_capture, output), 8),
      nir_u2u64(&b, nir_imul_imm(&b, id, (util_bitcount64(info->outputs) + 1) * 16)));
   unsigned packed = 0;
   u_foreach_bit64(slot, info->outputs) {
      nir_def *value = nir_load_global_constant(&b, 4, 32,
         nir_iadd_imm(&b, address, packed++ * 16), .align_mul = 16);
      value = nir_convert_to_bit_size(&b, value,
         nir_alu_type_get_base_type(info->output_types[slot]),
         nir_alu_type_get_type_size(info->output_types[slot]));
      nir_store_output(&b, nir_trim_vector(&b, value, info->output_components[slot]),
         nir_imm_int(&b, 0), .src_type = info->output_types[slot],
         .io_semantics.location = slot, .io_semantics.num_slots = 1);
   }
   return nir;
}
#endif

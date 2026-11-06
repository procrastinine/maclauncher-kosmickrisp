// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'conservative-degenerate-triangles';
export const sourceFiles = ['kosmickrisp-patches/conservative-canonical.h'];
export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_device_lib.c', s => s.replace(
    '         dev->mtl_compiler_handle, function, local_size_threads);',
    `         dev->mtl_compiler_handle, function,
         /* Preserve private temporaries in conservative preparation by
          * leaving this compiler limit at its default. Dispatch dimensions
          * still come from the precompiled kernel metadata. */
         i == LIBKK_CONSERVATIVE_PREPARE ? 0 : local_size_threads);`));
  edits.set('src/kosmickrisp/libkk/kk_conservative_canonical.h',
    fs.readFileSync(new URL('./conservative-canonical.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/libkk/kk_conservative.h', s => s.replace(
    '   uint64_t canonical_locations;',
    '   uint64_t canonical_locations;\n   uint64_t canonical_provoking_components[2];'));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s.replace(
    '   uint64_t conservative_canonical_outputs;',
    '   uint64_t conservative_canonical_outputs;\n   uint64_t conservative_canonical_components[2];'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
    '            .canonical_locations = cmd->state.shaders[MESA_SHADER_VERTEX]->info.conservative_canonical_outputs,',
    `            .canonical_locations = cmd->state.shaders[MESA_SHADER_VERTEX]->info.conservative_canonical_outputs,
            .canonical_provoking_components = {
               cmd->state.shaders[MESA_SHADER_VERTEX]->info.conservative_canonical_components[0],
               cmd->state.shaders[MESA_SHADER_VERTEX]->info.conservative_canonical_components[1],
            },`));
  edit('src/kosmickrisp/vulkan/kk_conservative_fs_link.h', s => s + `
/* Gather from IO-lowered FS before canonical lowering. Retain only the two
 * words; the final executable setter copies them without retaining NIR. */
#define KK_CONSERVATIVE_CANONICAL_COMPONENTS_API 1
void kk_conservative_gather_canonical_components(nir_shader *fragment,
                                                 uint64_t components[2]);
void kk_conservative_require_canonical_components(struct kk_shader *pipeline,
   uint64_t generic_locations, const uint64_t components[2]);
`);
  edit('src/kosmickrisp/libkk/kk_conservative.h', s => s.replace(
    '   float extra_size;\n};\n\nstruct kk_conservative_span',
    '   float extra_size;\n   uint32_t degenerate;\n};\n\nstruct kk_conservative_span'));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => {
    s = s.replace(`KERNEL(64)
libkk_polygon_clip(global struct poly_geometry_params *geometry,
                   global struct poly_heap *heap)`, `static void
polygon_clip_primitive(global struct poly_geometry_params *geometry,
                       global struct poly_heap *heap, uint id, bool defer_facing)`);
    s = s.replace('   uint id = cl_global_id.x;\n   global uint *counts',
      '   global uint *counts');
    s = s.replace('   if ((front && (p->cull_mode & 1)) || (!front && (p->cull_mode & 2)))',
      '   if (!defer_facing && ((front && (p->cull_mode & 1)) || (!front && (p->cull_mode & 2))))');
    s = s.replace('KERNEL(1024)\nlibkk_polygon_prefix_sum', `KERNEL(64)
libkk_polygon_clip(global struct poly_geometry_params *geometry,
                   global struct poly_heap *heap)
{
   polygon_clip_primitive(geometry, heap, cl_global_id.x, false);
}

KERNEL(64)
libkk_conservative_clip(global struct poly_geometry_params *geometry,
                        global struct poly_heap *heap)
{
   polygon_clip_primitive(geometry, heap, cl_global_id.x, true);
}

KERNEL(1024)
libkk_polygon_prefix_sum`);
    const begin = s.indexOf('   int exponent = p->depth_is_float ?');
    const end = s.indexOf('\n}\n\nKERNEL(64)\nlibkk_polygon_output', begin);
    if (begin < 0 || end < 0) throw Error('Missing depth-bias units helper body');
    const body = s.slice(begin, end);
    s = s.replace(body, '   return polygon_depth_bias_units(p, maximum_z, slope);');
    s = s.replace('static float\npolygon_depth_bias(', `static float
polygon_depth_bias_units(global struct poly_geometry_capture *p,
                          float maximum_z, float slope)
{
${body}
}

static float
polygon_depth_bias(`);
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', s => s.replace(
    '   libkk_polygon_clip(cmd, triangles, true, geometry_params, kk_heap(cmd));',
    `   if (gfx->conservative) {
      libkk_conservative_clip(cmd, triangles, true, geometry_params, kk_heap(cmd));
   } else {
      libkk_polygon_clip(cmd, triangles, true, geometry_params, kk_heap(cmd));
   }`));
  edit('src/kosmickrisp/libkk/kk_conservative.cl', s => s
    .replace('#include "kk_conservative.h"', '#include "kk_conservative.h"\n#include "kk_conservative_canonical.h"')
    .replace('/* DegenerateTrianglesRasterized=false: area on the reported 8-bit',
      '/* Area on the reported 8-bit')
    .replace('   float area = 0.0f;', '   long area = 0;')
    .replace('   float2 previous = (float2)(0.0f), first = (float2)(0.0f);',
      '   long2 previous = (long2)(0), first = (long2)(0);')
    .replace('      float2 fixed = rint(window * 256.0f);',
      '      long2 fixed = convert_long2(rint(window * 256.0f));')
    .replace('      float2 window = conservative_window(pos, vp, p->negative_one_to_one).xy;',
      '      float2 window = conservative_window(pos, vp, p->negative_one_to_one).xy;\n      if (!all(isfinite(window))) return;')
    .replace('   if (area == 0.0f || !isfinite(area))\n      return;', `   if (!isfinite(area))
      return;
   bool degenerate = area == 0;
   bool front = !degenerate && (saved & 1);
   if ((front && (p->cull_mode & 1)) || (!front && (p->cull_mode & 2)))
      return;
   if (degenerate) {
      lo = rint(lo * 256.0f) / 256.0f;
      hi = rint(hi * 256.0f) / 256.0f;
   }`)
    .replace('   if (!isfinite(area))\n      return;\n', '')
    .replace('   out->front = saved & 1;', '   out->front = front;\n   out->degenerate = degenerate;')
    .replace(`            values[corner * slots + location - VARYING_SLOT_VAR0] =
               p->outputs & (1ul << location) ? input[polygon_slot(p, location)] : (uint4)(0);`,
      `            uint index = location - VARYING_SLOT_VAR0;
            uint4 value = p->outputs & (1ul << location) ? input[polygon_slot(p, location)] : (uint4)(0);
            uint mask = (config->canonical_provoking_components[index / 16] >> ((index % 16) * 4)) & 15;
            if (degenerate && mask && (p->outputs & (1ul << location)))
               value = kk_conservative_provoking_components(value, prov[polygon_slot(p, location)], mask);
            values[corner * slots + index] = value;`)
    .replace('   out->bias = polygon_depth_bias(p, viewport, polygon, count);', `   out->bias = polygon_depth_bias(p, viewport, polygon, count);
   if (degenerate && p->depth_bias_enable) {
      float4 position = polygon_position(p, invocation, stream, vertices[provoking]);
      float depth = conservative_window(position, vp, p->negative_one_to_one).z;
      out->bias = polygon_depth_bias_units(p, fabs(depth), 0.0f);
   }`)
    .replace('      float2 edge = (float2)(a.y - b.y, b.x - a.x);', `      if (degenerate) {
         a = rint(a * 256.0f) / 256.0f;
         b = rint(b * 256.0f) / 256.0f;
      }
      float2 edge = (float2)(a.y - b.y, b.x - a.x);`));
  edit('src/kosmickrisp/vulkan/kk_conservative_nir.h', s => s
    .replace('   nir_def *sum = nir_fadd(b, nir_fadd(b, v[0], v[1]), v[2]);', `   nir_def *degenerate = nir_ine_imm(b, cr_load(b, packet, 1, 32, degenerate), 0);
   nir_def *provoking = cr_load(b, packet, 1, 32, provoking);
   nir_def *prov = nir_bcsel(b, nir_ieq_imm(b, provoking, 0), p[0],
      nir_bcsel(b, nir_ieq_imm(b, provoking, 1), p[1], p[2]));
   nir_def *sum = nir_fadd(b, nir_fadd(b, v[0], v[1]), v[2]);`)
    .replace('      *reciprocal_w = nir_fdiv(b, sum, den);',
      '      *reciprocal_w = nir_bcsel(b, degenerate, nir_frcp(b, nir_channel(b, prov, 3)), nir_fdiv(b, sum, den));')
    .replace('      *depth = nir_fadd(b, nir_fdiv(b, z, den), cr_load(b, packet, 1, 32, bias));',
      '      *depth = nir_fadd(b, nir_bcsel(b, degenerate, nir_fdiv(b, nir_channel(b, prov, 2), nir_channel(b, prov, 3)), nir_fdiv(b, z, den)), cr_load(b, packet, 1, 32, bias));')
    .replace('   return nir_vec3(b, v[0], v[1], v[2]);', `   for (unsigned i = 0; i < 3; ++i)
      v[i] = nir_bcsel(b, degenerate,
         nir_b2f32(b, nir_ieq_imm(b, provoking, i)), v[i]);
   return nir_vec3(b, v[0], v[1], v[2]);`)
    .replace(`         for (unsigned v = 0; v < 3; ++v)
            value = nir_fadd(b, value, nir_fmul(b, values[v],
               nir_f2fN(b, nir_channel(b, weights, v), intr->def.bit_size)));`,
      `         for (unsigned v = 0; v < 3; ++v)
            value = nir_fadd(b, value, nir_fmul(b, values[v],
               nir_f2fN(b, nir_channel(b, weights, v), intr->def.bit_size)));
         nir_def *provoking = cr_load(b, packet, 1, 32, provoking);
         nir_def *prov = nir_bcsel(b, nir_ieq_imm(b, provoking, 0), values[0],
            nir_bcsel(b, nir_ieq_imm(b, provoking, 1), values[1], values[2]));
         value = nir_bcsel(b, nir_ine_imm(b, cr_load(b, packet, 1, 32, degenerate), 0), prov, value);`));
  edit('src/kosmickrisp/vulkan/kk_conservative_fs_prolog.c', s => s
    .replace('     "   device const packed_float4 *h = (device const packed_float4 *)(p + %u);\\n"',
      `     "   if (*(device const uint *)(p+%u)) { uint v = *(device const uint *)(p+%u); return float3(v==0,v==1,v==2); }\\n"
     "   device const packed_float4 *h = (device const packed_float4 *)(p + %u);\\n"`)
    .replace('OFFSET(positions));', 'OFFSET(degenerate), OFFSET(provoking), OFFSET(positions));')
    .replace('         } else {\n            P("dot(float3(");', `         } else {
            P("(*(device const uint *)(kk_cr_p+%u) ? *(device const float *)(corners[*(device const uint *)(kk_cr_p+%u)]+%u) : ",
              OFFSET(degenerate), OFFSET(provoking), offset);
            P("dot(float3(");`)
    .replace('            P("),w)");', '            P("),w))");')
    .replace('#undef OFFSET', `void
kk_conservative_gather_canonical_components(nir_shader *fs, uint64_t components[2])
{
   components[0] = components[1] = 0;
   nir_foreach_block(block, nir_shader_get_entrypoint(fs)) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic) continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         bool flat = intr->intrinsic == nir_intrinsic_load_input;
         if (!flat && intr->intrinsic != nir_intrinsic_load_interpolated_input) continue;
         nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
         nir_src offset = intr->src[flat ? 0 : 1];
         unsigned base = sem.location + (nir_src_is_const(offset) ? nir_src_as_uint(offset) : 0);
         unsigned first = nir_intrinsic_component(intr);
         unsigned words = intr->def.num_components * DIV_ROUND_UP(intr->def.bit_size, 32);
         unsigned stride = DIV_ROUND_UP(first + words, 4);
         unsigned slots = nir_src_is_const(offset) ? stride : MAX2(sem.num_slots, stride);
         for (unsigned array_slot = 0; array_slot + stride <= slots; array_slot += stride) {
            for (unsigned word = 0; word < words; ++word) {
               unsigned location = base + array_slot + (first + word) / 4;
               if (location < VARYING_SLOT_VAR0 || location > VARYING_SLOT_VAR31) continue;
               unsigned index = location - VARYING_SLOT_VAR0;
               unsigned component = (first + word) % 4;
               components[index / 16] |= (uint64_t)1 << ((index % 16) * 4 + component);
            }
         }
      }
   }
}

void
kk_conservative_require_canonical_components(struct kk_shader *pipeline,
   uint64_t locations, const uint64_t components[2])
{
   kk_conservative_require_canonical(pipeline, locations);
   pipeline->info.conservative_canonical_components[0] = components ? components[0] : 0;
   pipeline->info.conservative_canonical_components[1] = components ? components[1] : 0;
}
#undef OFFSET`));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s.replace(
    '      .degenerateTrianglesRasterized = false,',
    '      .degenerateTrianglesRasterized = true,'));
}

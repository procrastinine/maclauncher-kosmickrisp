// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id='conservative-inner-coverage';
export const sourceFiles=['kosmickrisp-patches/conservative-inner.cl','kosmickrisp-patches/conservative-inner-nir.h','kosmickrisp-patches/conservative-inner-msl.h'];
export function apply({edit,edits}) {
 const owned=name=>fs.readFileSync(new URL('./'+name,import.meta.url),'utf8');
 edits.set('src/kosmickrisp/libkk/kk_conservative_inner.cl',owned('conservative-inner.cl'));
 edits.set('src/kosmickrisp/vulkan/kk_conservative_inner_nir.h',owned('conservative-inner-nir.h'));
 edits.set('src/kosmickrisp/vulkan/kk_conservative_inner_msl.h',owned('conservative-inner-msl.h'));
 edit('src/kosmickrisp/libkk/kk_conservative.h',s=>s
  .replace('   uint64_t canonical_provoking_components[2];','   uint64_t canonical_provoking_components[2];\n   uint32_t inner_coverage;')
  .replace('   uint32_t degenerate;','   uint32_t degenerate;\n   uint64_t inner_edges;')
  .replace('struct kk_conservative_span {','struct kk_conservative_inner_edge {\n   int64_t a, b, c;\n};\n\nstruct kk_conservative_span {'));
 edit('src/kosmickrisp/libkk/kk_conservative.cl',s=>s
  .replace('/* Intersect the square-expanded convex polygon', '#include "kk_conservative_inner.cl"\n\n/* Intersect the square-expanded convex polygon')
  .replace('   out->degenerate = degenerate;',`   out->degenerate = degenerate;
   out->inner_edges = config->inner_coverage && !degenerate ?
      kk_conservative_build_inner(heap, polygon, count, vp, p->negative_one_to_one) : 0;`));
 edit('src/kosmickrisp/vulkan/kk_shader.h',s=>s.replace('   bool conservative;', '   bool conservative;\n   bool conservative_full_coverage;'));
 edit('src/kosmickrisp/vulkan/kk_cmd_draw.c',s=>{
  // Follow the canonical packet's selected pre-raster executable. Mesh uses
  // gfx.shader; older projections select the merged vertex executable here.
  const anchor = /^ {12}\.canonical_locations = (cmd->state\.(?:shaders\[MESA_SHADER_VERTEX\]|gfx\.shader))->info\.conservative_canonical_outputs,$/gm;
  if ([...s.matchAll(anchor)].length !== 1)
   throw new Error('Expected one conservative canonical producer initializer');
  return s.replace(anchor,(line,producer)=>line+
   '\n            .inner_coverage = '+producer+'->info.conservative_full_coverage,');
 });
 edit('src/kosmickrisp/vulkan/kk_conservative_fs_link.h',s=>s
  .replace('   const struct kk_per_vertex_map *canonical;',`   const struct kk_per_vertex_map *canonical;
   /* Optional MSL int4 expression: logical pixel bounds (x0,y0,x1,y1).
    * NULL selects the 1x1 fragment only while coarse rates are unadvertised. */
   const char *logical_fragment_rectangle;`)
  + `
#define KK_CONSERVATIVE_INNER_COVERAGE_API 1
struct nir_builder;
nir_def *kk_conservative_evaluate_inner_coverage(struct nir_builder *builder,
   nir_def *packet, nir_def *logical_rectangle);
/* Request optional inner edges on the final executable only. */
void kk_conservative_require_inner_coverage(struct kk_shader *executable);
`);
 edit('src/kosmickrisp/vulkan/kk_conservative_nir.h',s=>s
  .replace('/* The input mask belongs to the original conservative primitive.', '#include "kk_conservative_inner_nir.h"\n\n/* The input mask belongs to the original conservative primitive.')
  .replace('   bool mask = intr->intrinsic == nir_intrinsic_load_sample_mask_in;', '   bool mask = intr->intrinsic == nir_intrinsic_load_sample_mask_in;\n   bool fully = intr->intrinsic == nir_intrinsic_load_fully_covered;')
  .replace('&& !position && !front && !mask)', '&& !position && !front && !mask && !fully)')
  .replace('   if (mask) {',`   if (fully) {
      nir_def *xy = nir_f2i32(b, nir_ffloor(b, nir_channels(b, nir_load_frag_coord(b), 3)));
      nir_def *rectangle = nir_vec4(b, nir_channel(b, xy, 0), nir_channel(b, xy, 1),
         nir_iadd_imm(b, nir_channel(b, xy, 0), 1),
         nir_iadd_imm(b, nir_channel(b, xy, 1), 1));
      value = kk_conservative_evaluate_inner_coverage(b, packet, rectangle);
   } else if (mask) {`));
 edit('src/kosmickrisp/vulkan/kk_shader.c',s=>s
  .replace('   gather_shader_info(shader, nir, state);',`   gather_shader_info(shader, nir, state);
   if (stage == MESA_SHADER_FRAGMENT) {
      shader->info.conservative_full_coverage =
         BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FULLY_COVERED);
      if (shader->info.conservative_full_coverage && !opts->independent_interface) {
         nir_shader_intrinsics_pass(nir, kk_conservative_disabled_inner,
                                    nir_metadata_control_flow, NULL);
         nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
      }
   }`)
  .replace('      if (stage == MESA_SHADER_FRAGMENT && info->conservative)\n         kk_msl_copy(&vs->polygon[5], &s->polygon[5]);',`      if (stage == MESA_SHADER_FRAGMENT && info->conservative)
         kk_msl_copy(&vs->polygon[5], &s->polygon[5]);
      if (stage == MESA_SHADER_FRAGMENT)
         info->conservative_full_coverage |= s->info.conservative_full_coverage;`));
 edit('src/kosmickrisp/vulkan/kk_conservative_fs_prolog.c',s=>s
  .replace('static const char *\nbuiltin(', '#include "kk_conservative_inner_msl.h"\n\nstatic const char *\nbuiltin(')
  .replace('   _mesa_string_buffer_append_len(text, prefix, input - prefix);',`   const char *fully = builtin(fs, SYSTEM_VALUE_FULLY_COVERED);
   if (fully) kk_conservative_emit_inner(text);
   _mesa_string_buffer_append_len(text, prefix, input - prefix);`)
  .replace('   _mesa_string_buffer_append_len(text, body+3, returning-body-3);',`   if (fully) {
      char *initializer = ralloc_asprintf(tmp, "bool %s = false;", fully);
      const char *found = strstr(body+3, initializer);
      if (!found || found >= returning) { ralloc_free(tmp); return VK_ERROR_UNKNOWN; }
      _mesa_string_buffer_append_len(text, body+3, found-body-3);
      P("bool %s = kk_cr_fully_covered(kk_cr_p,", fully);
      if (fs->logical_fragment_rectangle) P("%s", fs->logical_fragment_rectangle);
      else P("int4(int2(floor(%s.xy)),int2(floor(%s.xy))+1)", position, position);
      P(");");
      const char *after = found+strlen(initializer);
      _mesa_string_buffer_append_len(text, after, returning-after);
   } else {
      _mesa_string_buffer_append_len(text, body+3, returning-body-3);
   }`)
  .replace('void\nkk_conservative_require_canonical(',`void
kk_conservative_require_inner_coverage(struct kk_shader *pipeline)
{
   pipeline->info.conservative_full_coverage = true;
}

void
kk_conservative_require_canonical(`));
 // FullyCovered is evaluated from the original logical fragment rectangle.
 edit('src/kosmickrisp/vulkan/kk_physical_device.c',s=>s.replace('      .fullyCoveredFragmentShaderInputVariable = false,','      .fullyCoveredFragmentShaderInputVariable = true,'));
}

// SPDX-License-Identifier: MIT
export const id = 'native-vertex-input-layout';

export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    const old = `static void
kk_lower_vs_vbo(nir_shader *nir, const struct vk_graphics_pipeline_state *state,
                const struct vk_pipeline_robustness_state *rs)
{`;
    const replacement = `static bool
kk_native_vbo_input_slot(UNUSED nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_input)
      return false;
   const uint64_t mask = *(const uint64_t *)data;
   const unsigned location = nir_intrinsic_io_semantics(intr).location - VERT_ATTRIB_GENERIC0;
   assert(location < KK_MAX_ATTRIBS && (mask & BITFIELD64_BIT(location)));
   const unsigned slot = util_bitcount64(mask & BITFIELD64_MASK(location));
   if (nir_intrinsic_base(intr) == slot)
      return false;
   nir_intrinsic_set_base(intr, slot);
   return true;
}

static void
kk_lower_vs_vbo_mask(nir_shader *nir, const struct vk_graphics_pipeline_state *state,
                     const struct vk_pipeline_robustness_state *rs,
                     uint64_t attribs_read)
{`;
    if (!s.includes(old)) throw Error('native vertex input layout: VBO lowering definition');
    s = s.replace(old, replacement);
    const mask = '   uint64_t attribs_read = nir->info.inputs_read >> VERT_ATTRIB_GENERIC0;';
    if (!s.includes(mask)) throw Error('native vertex input layout: local attribute mask');
    s = s.replace(mask, `   uint64_t stage_inputs = nir->info.inputs_read >> VERT_ATTRIB_GENERIC0;
   assert((stage_inputs & ~attribs_read) == 0);
   /* Root entries are packed using the owning linked shader's mask. A native
    * alternative can use fewer inputs, but must keep those same root slots. */
   if (stage_inputs != attribs_read)
      nir_shader_intrinsics_pass(nir, kk_native_vbo_input_slot,
                                 nir_metadata_all, &attribs_read);`);
    const end = `   NIR_PASS(_, nir, kk_nir_lower_vbo,
            kk_vi_is_static(state) ? attributes : NULL, robustness2);
}`;
    if (!s.includes(end)) throw Error('native vertex input layout: ordinary VBO tail');
    s = s.replace(end, end + `

static void
kk_lower_vs_vbo(nir_shader *nir, const struct vk_graphics_pipeline_state *state,
                const struct vk_pipeline_robustness_state *rs)
{
   kk_lower_vs_vbo_mask(nir, state, rs,
      nir->info.inputs_read >> VERT_ATTRIB_GENERIC0);
}`);
    const early = `      kk_lower_vs_vbo(native_pair[0], state, vertex_robustness);
      NIR_PASS(_, native_pair[0], msl_nir_lower_instance_id);`;
    if (!s.includes(early)) throw Error('native vertex input layout: early clone VBO lowering');
    s = s.replace(early, `      /* VBO lowering waits until linking establishes the owning root mask. */`);
    const late = `      if (native_pair[0]) {
         NIR_PASS(_, native_pair[0], kk_nir_lower_polygon_scalar_outputs);`;
    if (!s.includes(late)) throw Error('native vertex input layout: native compilation');
    s = s.replace(late, `      if (native_pair[0]) {
         NIR_PASS(_, native_pair[0], nir_opt_dce);
         nir_shader_gather_info(native_pair[0], nir_shader_get_entrypoint(native_pair[0]));
         kk_lower_vs_vbo_mask(native_pair[0], state, vertex_robustness,
                              shaders[0]->info.vs.attribs_read);
         NIR_PASS(_, native_pair[0], msl_nir_lower_instance_id);
         NIR_PASS(_, native_pair[0], nir_opt_dce);
         NIR_PASS(_, native_pair[0], kk_nir_lower_descriptors, false,
                  vertex_robustness, 0, NULL);
         NIR_PASS(_, native_pair[0], kk_nir_lower_polygon_scalar_outputs);`);
    return s;
  });
}

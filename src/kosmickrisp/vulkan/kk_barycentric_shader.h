/* SPDX-License-Identifier: MIT
 * Barycentric pipeline lowering adapted from Mesa !45585, Sascha Scandella.
 */

static bool
kk_state_strip_fan_or_adjacency(const struct vk_graphics_pipeline_state *state)
{
   if (!state || !state->ia)
      return true;
   VkPrimitiveTopology topology = state->ia->primitive_topology;
   return BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY) ||
          topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
          topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
          topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
          topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY ||
          topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY ||
          topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
}

static bool
kk_state_provoking_last(const struct vk_graphics_pipeline_state *state)
{
   return !state || !state->rs ||
          BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX) ||
          state->rs->provoking_vertex ==
             VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
}

static bool
interpolates_off_center(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_interpolated_input)
      return false;
   nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
   if (bary->intrinsic != nir_intrinsic_load_barycentric_pixel)
      *(bool *)data = true;
   return false;
}

/* Metal merges triangles sharing vertices, which also affects inputs
 * evaluated at sample, centroid or offset positions on shared edges. */
static bool
fs_needs_unmerged_triangles(nir_shader *nir)
{
   /* Ordinary sample-qualified inputs retain the existing merge policy. */
   if (!BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD) &&
       !BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_LINEAR_COORD))
      return false;
   if (nir->info.fs.needs_full_quad_helper_invocations ||
       nir->info.fs.needs_coarse_quad_helper_invocations ||
       nir->info.fs.uses_sample_shading)
      return true;

   bool off_center = false;
   nir_shader_intrinsics_pass(nir, interpolates_off_center, nir_metadata_all,
                              &off_center);
   return off_center;
}

static void
kk_barycentric_shader_info(struct kk_shader *shader,
                           const struct kk_per_vertex_map *per_vertex_map)
{
   if (shader->info.stage == MESA_SHADER_FRAGMENT && per_vertex_map &&
       per_vertex_map->active) {
      /* The emulated per-vertex values and flat values are identical for all
       * vertices of a primitive, so they don't depend on the provoking
       * vertex. */
      uint64_t emulated = per_vertex_map->flat_locations;
      u_foreach_bit64(location, per_vertex_map->locations) {
         for (unsigned v = 0u; v < 3u; ++v)
            emulated |= BITFIELD64_BIT(per_vertex_map->slot[location][v]);
      }
      if (per_vertex_map->buffer_slot)
         emulated |= BITFIELD64_BIT(per_vertex_map->buffer_slot);
      shader->info.fs.emulates_per_vertex_inputs = true;
      shader->info.fs.per_vertex_uses_heap =
         per_vertex_map->buffer && per_vertex_map->locations;
      shader->info.fs.uses_flat_varyings = (shader->info.fs.flat_outputs & ~emulated) != 0;
   }

}

enum kk_barycentric_input_flags {
   KK_BARYCENTRIC_INPUT_PER_VERTEX = 1u << 0,
   KK_BARYCENTRIC_INPUT_FLAT = 1u << 1,
};

/* Capture the layout before IO lowering removes the input variables. */
static unsigned
kk_barycentric_input_flags(nir_shader *fs)
{
   unsigned flags = 0;
   nir_foreach_shader_in_variable(var, fs) {
      if (var->data.per_vertex)
         flags |= KK_BARYCENTRIC_INPUT_PER_VERTEX;
      else if (var->data.interpolation == INTERP_MODE_FLAT)
         flags |= KK_BARYCENTRIC_INPUT_FLAT;
   }
   return flags;
}

static bool
kk_barycentric_force_per_vertex(uint32_t shader_count,
                                struct vk_shader_compile_info *infos,
                                const struct vk_graphics_pipeline_state *state,
                                const struct vk_features *enabled_features,
                                unsigned input_flags)
{
   /* Barycentrics of strips, fans and adjacency primitives, including the
    * strips a GS outputs, need the emulated draw path so their vertex order
    * follows the provoking vertex mode like PerVertexKHR inputs. So do flat
    * inputs with last-vertex provoking: the unroll that emulates it rotates the
    * primitives, which reorders native barycentrics.
    */
   bool force_per_vertex = false;
   if (enabled_features->fragmentShaderBarycentric && shader_count && state && state->ia &&
       infos[shader_count - 1u].stage == MESA_SHADER_FRAGMENT) {
      nir_shader *fs = infos[shader_count - 1u].nir;
      force_per_vertex = input_flags & KK_BARYCENTRIC_INPUT_PER_VERTEX;
      bool flat_inputs = input_flags & KK_BARYCENTRIC_INPUT_FLAT;
      enum kk_barycentric_usage usage = kk_nir_barycentric_usage(fs);
      if (usage == KK_BARYCENTRIC_NONE)
         return force_per_vertex;
      /* A GS outputs points, line strips or triangle strips */
      nir_shader *gs = shader_count >= 2u && infos[shader_count - 2u].stage ==
                                                MESA_SHADER_GEOMETRY
                          ? infos[shader_count - 2u].nir
                          : NULL;
      bool reordered = gs ? gs->info.gs.output_primitive != MESA_PRIM_POINTS
                          : kk_state_strip_fan_or_adjacency(state);
      bool provoking_last = kk_state_provoking_last(state);
      force_per_vertex |=
         usage == KK_BARYCENTRIC_EXACT ||
         (usage == KK_BARYCENTRIC_PIXEL &&
          (reordered || (provoking_last && flat_inputs)));
   }

   return force_per_vertex;
}

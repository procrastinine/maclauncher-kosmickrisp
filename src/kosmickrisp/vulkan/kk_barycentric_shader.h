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

/* Native pre-raster values preserve triangle-list API corner order only on
 * an ordinary, linked VS/FS pair with static first-vertex provoking. Other
 * producers and independent stage interfaces keep the shared corner path. */
static bool
kk_barycentric_native_per_vertex(uint32_t count, nir_shader **stages,
                                 const struct vk_graphics_pipeline_state *state,
                                 struct msl_per_vertex_inputs *inputs)
{
   if (count != 2 || stages[0]->info.stage != MESA_SHADER_VERTEX ||
       stages[1]->info.stage != MESA_SHADER_FRAGMENT || !state || !state->ia ||
       state->ia->primitive_topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
       kk_state_strip_fan_or_adjacency(state) || kk_state_provoking_last(state) ||
       stages[0]->xfb_info)
      return false;

   memset(inputs, 0, sizeof(*inputs));
   uint64_t ordinary = 0;
   nir_foreach_function_impl(impl, stages[1]) nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         bool per_vertex = intr->intrinsic == nir_intrinsic_load_per_vertex_input ||
                           intr->intrinsic == nir_intrinsic_load_input_vertex;
         bool flat = intr->intrinsic == nir_intrinsic_load_input ||
                     intr->intrinsic == nir_intrinsic_load_per_primitive_input;
         if (!per_vertex && !flat &&
             intr->intrinsic != nir_intrinsic_load_interpolated_input)
            continue;
         unsigned src = flat ? 0 : 1;
         if (!nir_src_is_const(intr->src[src]))
            return false;
         unsigned location = nir_intrinsic_io_semantics(intr).location +
                             nir_src_as_uint(intr->src[src]);
         if (location < VARYING_SLOT_VAR0 || location > VARYING_SLOT_VAR31) {
            if (per_vertex)
               return false;
            continue;
         }
         if (!per_vertex) {
            ordinary |= BITFIELD64_BIT(location);
            continue;
         }
         unsigned slot = location - VARYING_SLOT_VAR0;
         nir_alu_type type = nir_intrinsic_dest_type(intr);
         if ((type != nir_type_float32 && type != nir_type_int32 &&
              type != nir_type_uint32 && type != nir_type_float16) ||
             (inputs->type[slot] && inputs->type[slot] != type))
            return false;
         inputs->locations |= BITFIELD64_BIT(location);
         inputs->type[slot] = type;
         inputs->components[slot] = MAX2(inputs->components[slot],
            nir_intrinsic_component(intr) + intr->num_components);
      }
   }
   /* A single Metal input cannot be both vertex_value<T> and an ordinary
    * interpolant/flat value. Keep mixed aliases on the existing transport. */
   if (!inputs->locations || (inputs->locations & ordinary))
      return false;
   uint64_t written = 0;
   nir_foreach_function_impl(impl, stages[0]) nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_store_output)
            continue;
         if (!nir_src_is_const(intr->src[1]))
            return false;
         unsigned location = nir_intrinsic_io_semantics(intr).location +
                             nir_src_as_uint(intr->src[1]);
         if (!(inputs->locations & BITFIELD64_BIT(location)))
            continue;
         unsigned slot = location - VARYING_SLOT_VAR0;
         if (nir_intrinsic_src_type(intr) != inputs->type[slot])
            return false;
         written |= BITFIELD64_BIT(location);
         /* Match the MSL output declaration, including sparse component
          * stores. Unwritten components retain Vulkan's undefined value. */
         unsigned width = nir_intrinsic_component(intr) +
                          util_last_bit(nir_intrinsic_write_mask(intr));
         inputs->components[slot] = MAX3(inputs->components[slot], width,
                                         intr->num_components);
      }
   }
   return written == inputs->locations;
}

enum kk_barycentric_input_flags {
   KK_BARYCENTRIC_INPUT_PER_VERTEX = 1u << 0,
   KK_BARYCENTRIC_INPUT_FLAT = 1u << 1,
   KK_BARYCENTRIC_INPUT_COORD = 1u << 2,
};

/* Capture the layout before IO lowering removes the input variables. */
static unsigned
kk_barycentric_input_flags(nir_shader *fs)
{
   unsigned flags = 0;
   /* This is the original guest interface. Fragment finishing can later add
    * native coordinates solely to prevent Metal triangle merging; those do
    * not request guest corner ordering or per-vertex shader replay. */
   if (BITSET_TEST(fs->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD) ||
       BITSET_TEST(fs->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_LINEAR_COORD))
      flags |= KK_BARYCENTRIC_INPUT_COORD;
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
      if (!(input_flags & KK_BARYCENTRIC_INPUT_COORD))
         return force_per_vertex;
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

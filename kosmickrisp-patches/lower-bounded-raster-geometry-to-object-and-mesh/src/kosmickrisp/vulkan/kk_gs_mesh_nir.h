/* SPDX-License-Identifier: MIT */
#ifndef KK_GS_MESH_NIR_H
#define KK_GS_MESH_NIR_H
#include "poly/cl/libpoly.h"

/* One object group owns one original primitive and instance. Each GS
 * invocation gets one child mesh, never a replay of the application body. */
struct kk_gs_mesh_layout {
   unsigned input_vertices, vertices, primitives, invocations, payload_bytes,
      max_objects;
   uint64_t outputs, primitive_outputs, flat_outputs;
   nir_alu_type type[NUM_TOTAL_VARYING_SLOTS];
   uint8_t components[NUM_TOTAL_VARYING_SLOTS];
   uint8_t basis[2];
   enum mesa_prim topology;
};

static bool
kk_gs_mesh_layout(nir_shader *vs, nir_shader *gs, nir_shader *fs,
                  unsigned family, const struct kk_per_vertex_map *map,
                  struct kk_gs_mesh_layout *l)
{
   /* Shader-written PrimitiveID uses the existing linked flat-varying
    * transport. An unresolved builtin remains with the compute producer. */
   if (family < 7 || vs->info.writes_memory || gs->info.writes_memory ||
       (gs->info.outputs_written & VARYING_BIT_PRIMITIVE_ID) ||
       gs->info.gs.vertices_in > 3 || !gs->info.gs.vertices_in ||
       gs->info.gs.active_stream_mask & ~1u || gs->info.gs.invocations > 32 ||
       !gs->info.gs.invocations || vs->info.ray_queries ||
       gs->info.ray_queries || gs->info.cull_distance_array_size ||
       vs->info.uses_wide_subgroup_intrinsics ||
       gs->info.uses_wide_subgroup_intrinsics ||
       vs->info.uses_control_barrier || gs->info.uses_control_barrier ||
       (map && (map->locations || map->buffer || map->canonical_heap)))
      return false;
   nir_shader *stages[2] = {vs, gs};
   for (unsigned i = 0; i < 2; i++)
      nir_foreach_function_impl(impl, stages[i])
         nir_foreach_block(block, impl)
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               if ((i == 0 && intr->intrinsic == nir_intrinsic_store_output &&
                    intr->src[0].ssa->bit_size != 32) ||
                   (i == 1 &&
                    intr->intrinsic == nir_intrinsic_load_per_vertex_input &&
                    intr->def.bit_size != 32))
                  return false;
               if (i == 0 && intr->intrinsic == nir_intrinsic_store_output &&
                   (!nir_src_is_const(intr->src[1]) ||
                    nir_intrinsic_io_semantics(intr).location +
                          nir_src_as_uint(intr->src[1]) >=
                       NUM_TOTAL_VARYING_SLOTS ||
                    nir_intrinsic_component(intr) + intr->num_components > 4))
                  return false;
               if (i == 1 &&
                   intr->intrinsic == nir_intrinsic_load_per_vertex_input &&
                   (!nir_src_is_const(intr->src[1]) ||
                    nir_intrinsic_io_semantics(intr).location +
                          nir_src_as_uint(intr->src[1]) >=
                       NUM_TOTAL_VARYING_SLOTS ||
                    nir_intrinsic_component(intr) + intr->def.num_components >
                       4))
                  return false;
               const char *name = nir_intrinsic_infos[intr->intrinsic].name;
               if (strstr(name, "subgroup") || strstr(name, "ballot") ||
                   strstr(name, "shuffle") || strstr(name, "vote") ||
                   strstr(name, "elect") || strstr(name, "read_invocation") ||
                   strstr(name, "read_first_invocation") ||
                   strstr(name, "clock") || strstr(name, "quad"))
                  return false;
            }
   unsigned vpp = gs->info.gs.output_primitive == MESA_PRIM_POINTS       ? 1
                  : gs->info.gs.output_primitive == MESA_PRIM_LINE_STRIP ? 2
                  : gs->info.gs.output_primitive == MESA_PRIM_TRIANGLE_STRIP
                     ? 3
                     : 0;
   if (!vpp || gs->info.gs.vertices_out < vpp || gs->info.gs.vertices_out > 256)
      return false;
   memset(l, 0, sizeof(*l));
   l->input_vertices = gs->info.gs.vertices_in;
   l->invocations = gs->info.gs.invocations;
   l->primitives = gs->info.gs.vertices_out - vpp + 1;
   l->vertices = l->primitives * vpp;
   l->topology = vpp == 1   ? MESA_PRIM_POINTS
                 : vpp == 2 ? MESA_PRIM_LINES
                            : MESA_PRIM_TRIANGLES;
   l->payload_bytes = 16 + l->input_vertices * NUM_TOTAL_VARYING_SLOTS * 16;
   l->flat_outputs = fs_flat_output_mask(fs);
   nir_foreach_function_impl(impl, gs)
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if ((in->intrinsic == nir_intrinsic_emit_vertex ||
                 in->intrinsic == nir_intrinsic_end_primitive) &&
                nir_intrinsic_stream_id(in))
               return false;
            if (in->intrinsic == nir_intrinsic_load_output &&
                (!nir_src_is_const(in->src[0]) ||
                 nir_intrinsic_io_semantics(in).location +
                       nir_src_as_uint(in->src[0]) >=
                    NUM_TOTAL_VARYING_SLOTS ||
                 nir_intrinsic_component(in) + in->def.num_components > 4))
               return false;
            if (in->intrinsic != nir_intrinsic_store_output)
               continue;
            if (!nir_src_is_const(in->src[1]) ||
                in->src[0].ssa->bit_size != 32 ||
                nir_alu_type_get_type_size(nir_intrinsic_src_type(in)) != 32)
               return false;
            unsigned slot = nir_intrinsic_io_semantics(in).location +
                            nir_src_as_uint(in->src[1]);
            if (slot >= NUM_TOTAL_VARYING_SLOTS || slot >= 64 ||
                nir_intrinsic_component(in) + in->num_components > 4)
               return false;
            if (l->components[slot] &&
                l->type[slot] != nir_intrinsic_src_type(in))
               return false;
            l->outputs |= BITFIELD64_BIT(slot);
            l->type[slot] = nir_intrinsic_src_type(in);
            l->components[slot] =
               MAX2(l->components[slot],
                    nir_intrinsic_component(in) + in->num_components);
         }
      }
   if (!(l->outputs & VARYING_BIT_POS) || l->components[VARYING_SLOT_POS] != 4)
      return false;
   /* Hardware rasterization supplies a unit point size when the application
    * does not write one. Include that field in the declared byte budget. */
   if (l->topology == MESA_PRIM_POINTS && !(l->outputs & VARYING_BIT_PSIZ)) {
      l->outputs |= VARYING_BIT_PSIZ;
      l->type[VARYING_SLOT_PSIZ] = nir_type_float32;
      l->components[VARYING_SLOT_PSIZ] = 1;
   }
   l->primitive_outputs =
      l->outputs &
      (VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT | VARYING_BIT_PRIMITIVE_ID);
   u_foreach_bit64(slot, l->primitive_outputs)
      l->type[slot] = nir_type_uint32;
   uint64_t float_builtins = VARYING_BIT_POS | VARYING_BIT_PSIZ |
                             VARYING_BIT_CLIP_DIST0 | VARYING_BIT_CLIP_DIST1;
   u_foreach_bit64(slot, l->outputs & float_builtins)
      l->type[slot] = nir_type_float32;
   if (map)
      for (unsigned i = 0; i < 2; i++)
         if (map->barycentric_slot[i]) {
            l->basis[i] = map->barycentric_slot[i];
            l->outputs |= BITFIELD64_BIT(l->basis[i]);
            l->type[l->basis[i]] = nir_type_float32;
            l->components[l->basis[i]] = 3;
         }
   /* Round each live vector up to 16 bytes, include primitive indices and
    * metadata, and apply the conservative Apple7 baseline byte budget. */
   unsigned clip_bytes = ALIGN_POT(4u * gs->info.clip_distance_array_size, 16u);
   uint64_t bytes =
      (uint64_t)l->vertices * clip_bytes +
      (uint64_t)l->vertices * 16 *
         util_bitcount64(l->outputs & ~l->primitive_outputs) +
      (uint64_t)l->primitives *
         (16 * util_bitcount64(l->primitive_outputs) + 16 + 4 * vpp);
   l->max_objects =
      MIN2(KK_MESH_MAX_GROUPS,
           UINT32_MAX / (l->payload_bytes + bytes * l->invocations));
   return l->vertices <= 256 && l->primitives <= 512 && bytes <= 16384 &&
          l->payload_bytes <= 16384 && l->invocations <= KK_MESH_NATIVE_GROUPS;
}

static nir_def *
kk_gs_mesh_param(nir_builder *b, unsigned bits, unsigned offset)
{
   nir_def *pd = nir_load_per_draw_ptr_kk(b, 1, 64);
   nir_def *address = nir_load_global_constant(
      b, 1, 64,
      nir_iadd_imm(b, pd, offsetof(struct kk_per_draw_data, vertex_params)),
      .align_mul = 8);
   return nir_load_global_constant(b, 1, bits, nir_iadd_imm(b, address, offset),
                                   .align_mul = bits / 8);
}

struct kk_gs_mesh_vs_lower {
   nir_variable *vertex;
   const struct kk_gs_mesh_layout *layout;
   enum mesa_prim topology;
};

static bool
kk_gs_mesh_vs_intrinsic(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   struct kk_gs_mesh_vs_lower *s = data;
   b->cursor = nir_before_instr(&in->instr);
   if (in->intrinsic == nir_intrinsic_store_output) {
      unsigned base = 16 + nir_intrinsic_io_semantics(in).location * 16 +
                      nir_intrinsic_component(in) * 4;
      nir_def *offset = nir_iadd(b,
                                 nir_imul_imm(b, nir_load_var(b, s->vertex),
                                              NUM_TOTAL_VARYING_SLOTS * 16),
                                 nir_imul_imm(b, in->src[1].ssa, 16));
      nir_store_task_payload(b, in->src[0].ssa, offset, .base = base,
                             .write_mask = nir_intrinsic_write_mask(in));
      nir_instr_remove(&in->instr);
      return true;
   }
   nir_def *value = NULL;
   switch (in->intrinsic) {
   case nir_intrinsic_load_vertex_id:
   case nir_intrinsic_load_vertex_id_zero_base: {
      nir_def *p = nir_channel(b, nir_load_workgroup_id(b), 0);
      nir_def *v = nir_load_var(b, s->vertex), *index;
      nir_def *topology = nir_imm_int(b, s->topology);
      if (s->topology == MESA_PRIM_POINTS)
         index = p;
      else if (u_reduced_prim(s->topology) == MESA_PRIM_LINES)
         index =
            poly_vertex_id_for_line_class(b, topology, p, v, nir_imm_int(b, 0));
      else
         index = poly_vertex_id_for_tri_class(
            b, topology, p, v, nir_ieq_imm(b, nir_load_provoking_last(b), 0));
      if (in->intrinsic == nir_intrinsic_load_vertex_id_zero_base)
         value = index;
      else {
         nir_def *size = kk_gs_mesh_param(
            b, 32, offsetof(struct kk_gs_mesh_draw, index_size));
         nir_def *first = kk_gs_mesh_param(
            b, 32, offsetof(struct kk_gs_mesh_draw, first_vertex));
         nir_def *base = kk_gs_mesh_param(
            b, 32, offsetof(struct kk_gs_mesh_draw, base_vertex));
         nir_if *indexed = nir_push_if(b, nir_ine_imm(b, size, 0));
         nir_def *address =
            kk_gs_mesh_param(b, 64, offsetof(struct kk_gs_mesh_draw, index));
         nir_def *range = kk_gs_mesh_param(
            b, 32, offsetof(struct kk_gs_mesh_draw, index_count));
         nir_def *el = nir_iadd(b, index, first);
         nir_def *got = nir_imm_int(b, 0);
         nir_if *bounded = nir_push_if(
            b, nir_iand(b, nir_uge(b, el, first), nir_ult(b, el, range)));
         nir_def *at =
            nir_iadd(b, address, nir_u2u64(b, nir_imul(b, el, size)));
         nir_if *byte = nir_push_if(b, nir_ieq_imm(b, size, 1));
         nir_def *u8 =
            nir_u2u32(b, nir_load_global_constant(b, 1, 8, at, .align_mul = 1));
         nir_push_else(b, byte);
         nir_if *half = nir_push_if(b, nir_ieq_imm(b, size, 2));
         nir_def *u16 = nir_u2u32(
            b, nir_load_global_constant(b, 1, 16, at, .align_mul = 1));
         nir_push_else(b, half);
         nir_def *u32 = nir_load_global_constant(b, 1, 32, at, .align_mul = 1);
         nir_pop_if(b, half);
         nir_def *wide = nir_if_phi(b, u16, u32);
         nir_pop_if(b, byte);
         nir_def *loaded = nir_if_phi(b, u8, wide);
         nir_pop_if(b, bounded);
         got = nir_if_phi(b, loaded, got);
         nir_def *biased = nir_iadd(b, got, base);
         nir_push_else(b, indexed);
         nir_def *linear = nir_iadd(b, index, first);
         nir_pop_if(b, indexed);
         value = nir_if_phi(b, biased, linear);
      }
      break;
   }
   case nir_intrinsic_load_instance_id:
      value = nir_channel(b, nir_load_workgroup_id(b), 1);
      break;
   case nir_intrinsic_load_base_instance:
      value = kk_gs_mesh_param(
         b, 32, offsetof(struct kk_gs_mesh_draw, first_instance));
      break;
   case nir_intrinsic_load_first_vertex:
      value =
         kk_gs_mesh_param(b, 32, offsetof(struct kk_gs_mesh_draw, base_vertex));
      break;
   default:
      return false;
   }
   nir_def_replace(&in->def, value);
   return true;
}

static void
kk_gs_mesh_lower_vs(nir_shader *vs, const struct kk_gs_mesh_layout *l,
                    enum mesa_prim topology)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(vs);
   struct kk_gs_mesh_vs_lower state = {.layout = l, .topology = topology};
   state.vertex =
      nir_local_variable_create(impl, glsl_uint_type(), "input_corner");
   nir_cf_list body;
   nir_cf_extract(&body, nir_before_impl(impl), nir_after_impl(impl));
   nir_builder b = nir_builder_at(nir_after_block(nir_start_block(impl)));
   nir_store_var(&b, state.vertex, nir_imm_int(&b, 0), 1);
   nir_loop *loop = nir_push_loop(&b);
   nir_break_if(
      &b, nir_uge_imm(&b, nir_load_var(&b, state.vertex), l->input_vertices));
   b.cursor = nir_cf_reinsert(&body, b.cursor);
   nir_store_var(&b, state.vertex,
                 nir_iadd_imm(&b, nir_load_var(&b, state.vertex), 1), 1);
   nir_pop_loop(&b, loop);
   nir_progress(true, impl, nir_metadata_none);
   nir_shader_intrinsics_pass(vs, kk_gs_mesh_vs_intrinsic, nir_metadata_none,
                              &state);
   b.cursor = nir_after_impl(impl);
   nir_store_task_payload(&b, nir_channel(&b, nir_load_workgroup_id(&b), 0),
                          nir_imm_int(&b, 0), .write_mask = 1);
   nir_launch_mesh_workgroups(&b, nir_imm_ivec3(&b, l->invocations, 1, 1));
   vs->info.stage = MESA_SHADER_TASK;
   memset(&vs->info.cs, 0, sizeof(vs->info.cs));
   vs->info.task_payload_size = l->payload_bytes;
   vs->info.workgroup_size[0] = vs->info.workgroup_size[1] =
      vs->info.workgroup_size[2] = 1;
   vs->info.outputs_written = vs->info.inputs_read = 0;
}

struct kk_gs_mesh_gs_lower {
   const struct kk_gs_mesh_layout *layout;
   nir_variable *current[NUM_TOTAL_VARYING_SLOTS],
      *history[NUM_TOTAL_VARYING_SLOTS];
   nir_variable *vertices, *primitives, *strip;
   bool viewport_transform;
};

/* Raster transforms apply to emitted snapshots, never to application GS
 * output variables which the body can read again. These are the same half-Z
 * and disabled-clip/clamp transforms as the ordinary hardware VS tail. */
static nir_def *
kk_gs_mesh_position(nir_builder *b, struct kk_gs_mesh_gs_lower *s,
                    nir_def *position, nir_def *provoking)
{
   nir_def *z = nir_channel(b, position, 2);
   nir_def *w = nir_channel(b, position, 3);
   nir_def *c = nir_load_clip_z_coeff(b);
   z = nir_ffma_weak(b, nir_fneg(b, z), c, nir_ffma_weak(b, w, c, z));
   position = nir_vector_insert_imm(b, position, z, 2);
   if (!s->viewport_transform)
      return position;
   nir_if *transform =
      nir_push_if(b, nir_load_is_viewport_z_transform_emulated_kk(b));
   nir_def *viewport = nir_imm_int(b, 0);
   if (s->history[VARYING_SLOT_VIEWPORT]) {
      nir_deref_instr *at = nir_build_deref_array(
         b, nir_build_deref_var(b, s->history[VARYING_SLOT_VIEWPORT]),
         provoking);
      viewport = nir_channel(b, nir_load_deref(b, at), 0);
   }
   nir_def *range = nir_load_viewport_z_range_kk(b, 2, 32, viewport);
   nir_def *min = nir_channel(b, range, 0);
   nir_def *scale = nir_fsub(b, nir_channel(b, range, 1), min);
   nir_def *mapped = nir_fadd(b, nir_fmul(b, z, scale), nir_fmul(b, w, min));
   nir_def *result = nir_vector_insert_imm(b, position, mapped, 2);
   nir_pop_if(b, transform);
   return nir_if_phi(b, result, position);
}

static bool
kk_gs_mesh_gs_intrinsic(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   struct kk_gs_mesh_gs_lower *s = data;
   const struct kk_gs_mesh_layout *l = s->layout;
   b->cursor = nir_before_instr(&in->instr);
   if (in->intrinsic == nir_intrinsic_load_per_vertex_input) {
      nir_def *offset = nir_iadd(
         b, nir_imul_imm(b, in->src[0].ssa, NUM_TOTAL_VARYING_SLOTS * 16),
         nir_imul_imm(b, in->src[1].ssa, 16));
      nir_def *value = nir_load_task_payload(
         b, in->def.num_components, in->def.bit_size, offset,
         .base = 16 + nir_intrinsic_io_semantics(in).location * 16 +
                 nir_intrinsic_component(in) * 4);
      nir_def_replace(&in->def, value);
      return true;
   }
   if (in->intrinsic == nir_intrinsic_load_primitive_id) {
      nir_def_replace(&in->def,
                      nir_load_task_payload(b, 1, 32, nir_imm_int(b, 0)));
      return true;
   }
   if (in->intrinsic == nir_intrinsic_load_invocation_id) {
      nir_def_replace(&in->def, nir_channel(b, nir_load_workgroup_id(b), 0));
      return true;
   }
   if (in->intrinsic == nir_intrinsic_load_output) {
      unsigned slot =
         nir_intrinsic_io_semantics(in).location + nir_src_as_uint(in->src[0]);
      if (!s->current[slot]) {
         nir_def_replace(
            &in->def, nir_undef(b, in->def.num_components, in->def.bit_size));
         return true;
      }
      nir_def *value = nir_load_var(b, s->current[slot]);
      value = nir_channels(
         b, value,
         (nir_component_mask_t)(BITFIELD_MASK(in->def.num_components)
                                << nir_intrinsic_component(in)));
      nir_def_replace(&in->def, value);
      return true;
   }
   if (in->intrinsic == nir_intrinsic_store_output) {
      unsigned slot =
         nir_intrinsic_io_semantics(in).location + nir_src_as_uint(in->src[1]);
      nir_def *old = nir_load_var(b, s->current[slot]);
      unsigned component = nir_intrinsic_component(in),
               mask = nir_intrinsic_write_mask(in);
      nir_def *v[4];
      for (unsigned c = 0; c < 4; c++)
         v[c] = c >= component && c < component + in->num_components &&
                      (mask & BITFIELD_BIT(c - component))
                   ? nir_channel(b, in->src[0].ssa, c - component)
                   : nir_channel(b, old, c);
      nir_store_var(b, s->current[slot], nir_vec(b, v, 4), 15);
      nir_instr_remove(&in->instr);
      return true;
   }
   if (in->intrinsic == nir_intrinsic_end_primitive) {
      nir_store_var(b, s->strip, nir_load_var(b, s->vertices), 1);
      nir_instr_remove(&in->instr);
      return true;
   }
   if (in->intrinsic != nir_intrinsic_emit_vertex)
      return false;
   unsigned vpp = mesa_vertices_per_prim(l->topology);
   nir_def *vertex = nir_load_var(b, s->vertices),
           *strip = nir_load_var(b, s->strip);
   u_foreach_bit64(slot, l->outputs) {
      if ((l->basis[0] && slot == l->basis[0]) ||
          (l->basis[1] && slot == l->basis[1]))
         continue;
      nir_store_deref(b,
                      nir_build_deref_array(
                         b, nir_build_deref_var(b, s->history[slot]), vertex),
                      nir_load_var(b, s->current[slot]), 15);
   }
   nir_def *total = nir_iadd_imm(b, vertex, 1);
   nir_store_var(b, s->vertices, total, 1);
   nir_if *complete =
      nir_push_if(b, nir_uge_imm(b, nir_isub(b, total, strip), vpp));
   nir_def *primitive = nir_load_var(b, s->primitives);
   nir_def *first = nir_iadd_imm(b, total, -(int)vpp);
   nir_def *last = nir_load_provoking_last(b);
   nir_def *provoking = nir_bcsel(b, nir_ine_imm(b, last, 0), vertex, first);
   for (unsigned corner = 0; corner < vpp; corner++) {
      nir_def *source = nir_iadd_imm(b, first, corner);
      if (vpp == 3)
         source = nir_iadd(b, strip,
                           poly_vertex_id_for_tri_class(
                              b, nir_imm_int(b, MESA_PRIM_TRIANGLE_STRIP),
                              nir_isub(b, first, strip), nir_imm_int(b, corner),
                              nir_ieq_imm(b, last, 0)));
      nir_def *output =
         nir_iadd_imm(b, nir_imul_imm(b, primitive, vpp), corner);
      u_foreach_bit64(slot, l->outputs & ~l->primitive_outputs) {
         if (slot == VARYING_SLOT_PSIZ && l->topology != MESA_PRIM_POINTS)
            continue;
         nir_def *value;
         if ((l->basis[0] && slot == l->basis[0]) ||
             (l->basis[1] && slot == l->basis[1]))
            value = nir_imm_vec3(b, corner == 0, corner == 1, corner == 2);
         else {
            nir_def *at =
               (l->flat_outputs & BITFIELD64_BIT(slot)) ? provoking : source;
            value = nir_load_deref(
               b, nir_build_deref_array(
                     b, nir_build_deref_var(b, s->history[slot]), at));
            value = nir_channels(
               b, value,
               (nir_component_mask_t)BITFIELD_MASK(l->components[slot]));
         }
         if (slot == VARYING_SLOT_POS)
            value = kk_gs_mesh_position(b, s, value, provoking);
         nir_store_per_vertex_output(
            b, value, output, nir_imm_int(b, 0),
            .io_semantics = {.location = slot, .num_slots = 1},
            .src_type = l->type[slot],
            .write_mask = BITFIELD_MASK(l->components[slot]));
      }
      nir_store_per_primitive_output(
         b, output, primitive, nir_imm_int(b, 0),
         .io_semantics = {.location = VARYING_SLOT_PRIMITIVE_INDICES,
                          .num_slots = 1},
         .component = corner, .src_type = nir_type_uint32, .write_mask = 1);
   }
   u_foreach_bit64(slot, l->primitive_outputs) {
      nir_def *value = nir_load_deref(
         b, nir_build_deref_array(b, nir_build_deref_var(b, s->history[slot]),
                                  provoking));
      nir_store_per_primitive_output(
         b, nir_channel(b, value, 0), primitive, nir_imm_int(b, 0),
         .io_semantics = {.location = slot, .num_slots = 1},
         .src_type = l->type[slot], .write_mask = 1);
   }
   nir_store_var(b, s->primitives, nir_iadd_imm(b, primitive, 1), 1);
   nir_pop_if(b, complete);
   nir_instr_remove(&in->instr);
   return true;
}

static void
kk_gs_mesh_lower_gs(nir_shader *gs, const struct kk_gs_mesh_layout *l,
                    const struct vk_graphics_pipeline_state *pipeline)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(gs);
   struct kk_gs_mesh_gs_lower state = {
      .layout = l,
      .viewport_transform =
         kk_is_possible_both_depth_clip_clamp(pipeline, false),
   };
   state.vertices =
      nir_local_variable_create(impl, glsl_uint_type(), "gs_vertices");
   state.primitives =
      nir_local_variable_create(impl, glsl_uint_type(), "gs_primitives");
   state.strip = nir_local_variable_create(impl, glsl_uint_type(), "gs_strip");
   nir_builder b = nir_builder_at(nir_before_impl(impl));
   nir_store_var(&b, state.vertices, nir_imm_int(&b, 0), 1);
   nir_store_var(&b, state.primitives, nir_imm_int(&b, 0), 1);
   nir_store_var(&b, state.strip, nir_imm_int(&b, 0), 1);
   u_foreach_bit64(slot, l->outputs) {
      if ((l->basis[0] && slot == l->basis[0]) ||
          (l->basis[1] && slot == l->basis[1]))
         continue;
      const struct glsl_type *type = glsl_vector_type(
         nir_get_glsl_base_type_for_nir_type(l->type[slot]), 4);
      state.current[slot] = nir_local_variable_create(impl, type, "gs_output");
      if (slot == VARYING_SLOT_PSIZ &&
          !(gs->info.outputs_written & VARYING_BIT_PSIZ))
         nir_store_var(&b, state.current[slot], nir_imm_vec4(&b, 1, 1, 1, 1),
                       15);
      state.history[slot] = nir_local_variable_create(
         impl, glsl_array_type(type, gs->info.gs.vertices_out, 0),
         "gs_emitted");
   }
   nir_shader_intrinsics_pass(gs, kk_gs_mesh_gs_intrinsic, nir_metadata_none,
                              &state);
   b.cursor = nir_after_impl(impl);
   nir_def *primitives = nir_load_var(&b, state.primitives);
   nir_set_vertex_and_primitive_count(
      &b, nir_imul_imm(&b, primitives, mesa_vertices_per_prim(l->topology)),
      primitives, nir_imm_int(&b, 0));
   gs->info.stage = MESA_SHADER_MESH;
   memset(&gs->info.mesh, 0, sizeof(gs->info.mesh));
   gs->info.mesh.primitive_type = l->topology;
   gs->info.mesh.max_vertices_out = l->vertices;
   gs->info.mesh.max_primitives_out = l->primitives;
   gs->info.per_primitive_outputs =
      l->primitive_outputs | VARYING_BIT_PRIMITIVE_INDICES;
   gs->info.task_payload_size = l->payload_bytes;
   gs->info.workgroup_size[0] = gs->info.workgroup_size[1] =
      gs->info.workgroup_size[2] = 1;
   /* These temporaries are introduced after the application's preprocessing.
    * Use the same scratch/indirect lowering for the emitted history arrays. */
   NIR_PASS(_, gs, nir_lower_vars_to_scratch, 32,
            glsl_get_natural_size_align_bytes, glsl_get_word_size_align_bytes);
   NIR_PASS(_, gs, nir_lower_indirect_derefs_to_if_else_trees,
            nir_var_function_temp, UINT32_MAX);
}
#endif

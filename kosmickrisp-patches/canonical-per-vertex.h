/* SPDX-License-Identifier: MIT */

/* The independent-stage ABI is owned here, not by its GPL consumer. */
#define KK_CANONICAL_VARYING_COUNT \
   (VARYING_SLOT_VAR31 - VARYING_SLOT_VAR0 + 1)

static uint64_t
kk_canonical_generic_mask(void)
{
   return BITFIELD64_MASK(VARYING_SLOT_VAR31 + 1) &
          ~BITFIELD64_MASK(VARYING_SLOT_VAR0);
}

static void
kk_canonical_map_init(struct kk_per_vertex_map *map)
{
   memset(map, 0, sizeof(*map));
   map->active = true;
   map->canonical_heap = true;
   map->buffer = true;
   map->buffer_slot = VARYING_SLOT_VAR0;
   map->barycentric_slot[0] = VARYING_SLOT_VAR0 + 1;
   map->barycentric_slot[1] = VARYING_SLOT_VAR0 + 2;
}

static void
kk_canonical_fs_map(nir_shader *fs, struct kk_per_vertex_map *map)
{
   uint64_t per_vertex = map->locations;
   kk_canonical_map_init(map);
   map->locations = per_vertex;

   /* Every ordinary generic input uses the heap too: independent stages
    * cannot reuse its native slot for both the input and the address/bases. */
   nir_foreach_block(block, nir_shader_get_entrypoint(fs)) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         bool flat = intr->intrinsic == nir_intrinsic_load_input;
         if (!flat && intr->intrinsic != nir_intrinsic_load_interpolated_input)
            continue;
         unsigned location = io_location(intr, flat ? 0 : 1);
         uint64_t bit = BITFIELD64_BIT(location);
         if (kk_canonical_generic_mask() & bit) {
            map->locations |= bit;
            map->buffered_locations |= bit;
         }
      }
   }
}

// SPDX-License-Identifier: MIT
// Give Layer replay readers a zero Position when none is written
export const id = "give-layer-replay-readers-a-zero-position-when";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_poly_layer_replay_nir.h", source => source
    .replace("         }\n      }\n   }\n   assert(position);\n   if (!layer) {\n      nir_builder zero = nir_builder_at(nir_after_instr(&position->instr));\n      nir_store_output(&zero, nir_imm_int(&zero, 0), nir_imm_int(&zero, 0),\n",
      () => "         }\n      }\n   }\n   if (!position) {\n      /* Position is undefined when the producer writes none. A zero\n       * Position (w = 0) rasterizes nothing, as in ordinary draws, and gives\n       * the Layer selection below a store to order against. */\n      nir_builder zero = layer ? nir_builder_at(nir_after_instr(&layer->instr)) :\n         nir_builder_at(nir_after_impl(nir_shader_get_entrypoint(nir)));\n      nir_store_output(&zero, nir_imm_vec4(&zero, 0.0f, 0.0f, 0.0f, 0.0f), nir_imm_int(&zero, 0),\n         .src_type = nir_type_float32, .io_semantics.location = VARYING_SLOT_POS,\n         .io_semantics.num_slots = 1);\n      position = nir_instr_as_intrinsic(zero.cursor.instr);\n      nir->info.outputs_written |= VARYING_BIT_POS;\n   }\n   if (!layer) {\n      nir_builder zero = nir_builder_at(nir_after_instr(&position->instr));\n      nir_store_output(&zero, nir_imm_int(&zero, 0), nir_imm_int(&zero, 0),\n"));
}

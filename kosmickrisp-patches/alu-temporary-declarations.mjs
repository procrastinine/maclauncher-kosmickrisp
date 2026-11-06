// SPDX-License-Identifier: MIT
/** ALU SSA definitions receive a complete value before any dominated use. */
export const id = 'alu-temporary-declarations';
export function apply({edit}) {
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s.replace(
    '            } else\n               P_IND(ctx, "%s t%u = %s(0);\\n", type, def->index, type);',
    '            } else if (instr->type == nir_instr_type_alu) {\n' +
    '               /* instr_to_msl assigns every ALU destination in full. */\n' +
    '               P_IND(ctx, "%s t%u;\\n", type, def->index);\n' +
    '            } else {\n' +
    '               /* Guarded intrinsic and texture reads retain their zero. */\n' +
    '               P_IND(ctx, "%s t%u = %s(0);\\n", type, def->index, type);\n' +
    '            }'));
}

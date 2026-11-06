// SPDX-License-Identifier: MIT
export const id = 'msl-language-version';

/** Select the source language from operations actually emitted by a library. */
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s.replace(
    '   char *entrypoint_name;\n};',
    `   char *entrypoint_name;
   bool requires_msl41;
};

static inline enum mtl_language_version
kk_msl_language_version(const struct msl_compile_data *data,
                        enum mtl_language_version supported)
{
   assert(!data->requires_msl41 || supported >= MTL_LANGUAGE_VERSION_4_1);
   return data->requires_msl41 ? MTL_LANGUAGE_VERSION_4_1
                              : MTL_LANGUAGE_VERSION_4_0;
}`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = s.replace('/* Consumes nir */\nstatic void\nkk_compile_msl_options',
      `static bool
kk_nir_requires_msl41(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_shared_atomic &&
                nir_intrinsic_atomic_op(intr) == nir_atomic_op_fadd)
               return true;
         }
      }
   }
   return false;
}

/* Consumes nir */
static void
kk_compile_msl_options`);
    s = s.replace('   data->code = nir_to_msl(nir, &translate_options);',
      '   data->requires_msl41 = kk_nir_requires_msl41(nir);\n' +
      '   data->code = nir_to_msl(nir, &translate_options);');
    for (const data of ['data', 'vs_data', 'fs_data'])
      s = s.replace(data + '->code, pdev->info.msl_version,',
        data + '->code, kk_msl_language_version(' + data + ', pdev->info.msl_version),');
    s = s.replace('   dst->entrypoint_name = ralloc_strdup(NULL, src->entrypoint_name);',
      '   dst->entrypoint_name = ralloc_strdup(NULL, src->entrypoint_name);\n' +
      '   dst->requires_msl41 = src->requires_msl41;');
    s = s.replace('   blob_write_uint32(blob, code_length);',
      '   blob_write_uint32(blob, code_length);\n' +
      '   blob_write_uint8(blob, data->requires_msl41);');
    s = s.replace('   const uint32_t code_length = blob_read_uint32(blob);',
      '   const uint32_t code_length = blob_read_uint32(blob);\n' +
      '   data->requires_msl41 = blob_read_uint8(blob) != 0;');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_mesh_pipeline.h', s => s.replace(
    'source ? source : data->code, pdev->info.msl_version,',
    'source ? source : data->code, kk_msl_language_version(data, pdev->info.msl_version),'));
  edit('src/kosmickrisp/vulkan/kk_device_lib.c', s => s.replace(
    '   struct kk_physical_device *pdev = kk_device_physical(dev);\n\n', '')
    .replace('dev->mtl_compiler_handle, msl, pdev->info.msl_version,',
      'dev->mtl_compiler_handle, msl, MTL_LANGUAGE_VERSION_4_0,'));
}

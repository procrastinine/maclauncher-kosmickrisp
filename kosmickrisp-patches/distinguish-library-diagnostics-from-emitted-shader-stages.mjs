// SPDX-License-Identifier: MIT
// Distinguish library diagnostics from emitted shader stages
export const id = "distinguish-library-diagnostics-from-emitted-shader-stages";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/bridge/mtl_compiler.m", source => source
    .replace("   if (KK_DEBUG(MSL)) {\n      blake3_hash hash; char hex[BLAKE3_HEX_LEN];\n      _mesa_blake3_compute(src, strlen(src), hash); _mesa_blake3_format(hex, hash);\n      mesa_logi(\"// KK_MSL blake3=%s stage=native_library bytes=%zu\\n%s\\n\", hex, strlen(src), src);\n   }\n   MTLCompileOptions *opts = [[MTLCompileOptions new] autorelease];\n   opts.languageVersion = (MTLLanguageVersion)msl_version;\n",
      () => "   if (KK_DEBUG(MSL)) {\n      blake3_hash hash; char hex[BLAKE3_HEX_LEN];\n      _mesa_blake3_compute(src, strlen(src), hash); _mesa_blake3_format(hex, hash);\n      mesa_logi(\"// KK_MSL_LIBRARY blake3=%s stage=native_library bytes=%zu\\n%s\\n\", hex, strlen(src), src);\n   }\n   MTLCompileOptions *opts = [[MTLCompileOptions new] autorelease];\n   opts.languageVersion = (MTLLanguageVersion)msl_version;\n"));
}

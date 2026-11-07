// SPDX-License-Identifier: MIT
// Omit duplicate native fallbacks in emulation-only sampler calls
export const id = "omit-duplicate-native-fallbacks-in-emulation-only-sampler";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/compiler/kk-sampler-helpers-msl.h", source => source
    .replace("   struct hash_entry *entry = _mesa_hash_table_search(ctx->sampler_helpers, &key);assert(entry);\n   struct kk_msl_sample_helper *helper = entry->data;\n   bool sparse = tex->backend_flags & KK_SAMPLE_SPARSE;\n   bool guarded = !sparse && tex->op != nir_texop_lod;\n   if (guarded) {\n      P_IND(ctx, \"if (kk_sampler_emulation_active) {\\n\");\n      ctx->indentlevel++;\n",
      () => "   struct hash_entry *entry = _mesa_hash_table_search(ctx->sampler_helpers, &key);assert(entry);\n   struct kk_msl_sample_helper *helper = entry->data;\n   bool sparse = tex->backend_flags & KK_SAMPLE_SPARSE;\n   /* Descriptor-aware lowering marks calls reached only by an emulating\n    * sampler. Its outer dispatch already tests FC10 and the bound metadata;\n    * immutable emulating samplers publish activation before pipeline use.\n    * Keep the one noinline helper boundary, without another native clone. */\n   bool guarded = !sparse && tex->op != nir_texop_lod &&\n                  !(tex->backend_flags & KK_SAMPLE_EMULATION_ONLY);\n   if (guarded) {\n      P_IND(ctx, \"if (kk_sampler_emulation_active) {\\n\");\n      ctx->indentlevel++;\n"));
}

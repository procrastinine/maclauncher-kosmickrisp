// SPDX-License-Identifier: MIT
// Skip unused sampler handles for texture queries
export const id = "skip-unused-sampler-handles-for-texture-queries";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c", source => source
    .replace("      nir_def_replace(&tex->def, result);\n   }\n\n   if (sampler != NULL) {\n      unsigned offs =\n         offsetof(struct kk_sampled_image_descriptor, sampler_index);\n\n",
      () => "      nir_def_replace(&tex->def, result);\n   }\n\n   /* Queries and integer fetches may retain a sampler dereference from a\n    * cloned sample, but do not consume a native sampler object. */\n   if (sampler != NULL && nir_tex_instr_need_sampler(tex)) {\n      unsigned offs =\n         offsetof(struct kk_sampled_image_descriptor, sampler_index);\n\n"));
}

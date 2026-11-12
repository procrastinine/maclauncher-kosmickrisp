// SPDX-License-Identifier: MIT
// Lower hit-object fields to word loads and stores
import fs from 'node:fs';
export const id = "lower-hit-object-fields-to-word-loads-and";
export const sourceFiles = [
  "kosmickrisp-patches/lower-hit-object-fields-to-word-loads-and/src/kosmickrisp/vulkan/kk_rt_hit_object.c"
];
const added = file => fs.readFileSync(new URL("./lower-hit-object-fields-to-word-loads-and/" + file, import.meta.url), 'utf8');
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_ray_tracing.h", source => source
    .replace("void kk_rt_begin_traversal(nir_builder *, const struct kk_rt_ray_traversal_args *);\nnir_def *kk_rt_build_ray_traversal(nir_builder *, const struct kk_rt_ray_traversal_args *);\nbool kk_nir_lower_ray_queries(nir_shader *shader);\n#endif\n",
      () => "void kk_rt_begin_traversal(nir_builder *, const struct kk_rt_ray_traversal_args *);\nnir_def *kk_rt_build_ray_traversal(nir_builder *, const struct kk_rt_ray_traversal_args *);\nbool kk_nir_lower_ray_queries(nir_shader *shader);\nbool kk_nir_lower_hit_objects(nir_shader *shader);\n#endif\n"));
  edits.set("src/kosmickrisp/vulkan/kk_rt_hit_object.c", added("src/kosmickrisp/vulkan/kk_rt_hit_object.c"));
  edit("src/kosmickrisp/vulkan/kk_shader.c", source => source
    .replace("   };\n   NIR_PASS(_, nir, nir_opt_access, &access_options);\n\n   if (nir->info.ray_queries)\n      NIR_PASS(_, nir, kk_nir_lower_ray_queries);\n\n",
      () => "   };\n   NIR_PASS(_, nir, nir_opt_access, &access_options);\n\n   if (nir->info.hit_objects)\n      NIR_PASS(_, nir, kk_nir_lower_hit_objects);\n   if (nir->info.ray_queries)\n      NIR_PASS(_, nir, kk_nir_lower_ray_queries);\n\n"));
  edit("src/kosmickrisp/vulkan/meson.build", source => source
    .replace("  'kk_ray_tracing.h',\n  'kk_ray_traversal.c',\n  'kk_ray_query.c',\n  'kk_as_object.c',\n  'kk_nir_lower_custom_border.c',\n  'kk_nir_lower_descriptors.c',\n",
      () => "  'kk_ray_tracing.h',\n  'kk_ray_traversal.c',\n  'kk_ray_query.c',\n  'kk_rt_hit_object.c',\n  'kk_as_object.c',\n  'kk_nir_lower_custom_border.c',\n  'kk_nir_lower_descriptors.c',\n"));
}

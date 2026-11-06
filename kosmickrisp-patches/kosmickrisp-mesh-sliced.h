/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_SLICED_H
#define KK_MESH_SLICED_H
#include <limits.h>
#include <string.h>
#include "kosmickrisp/libkk/kk_argument_table.h"

/* Derived variants leave the stored native translation unit byte-identical.
 * Only a draw requiring virtualization compiles one of these wrappers. */
static char *
kk_mesh_source_replace(void *mem, const char *source, const char *find,
                         const char *replacement)
{
   const char *at = strstr(source, find);
   if (!at || at - source > INT_MAX)
      return NULL;
   return ralloc_asprintf(mem, "%.*s%s%s", (int)(at - source), source,
                          replacement, at + strlen(find));
}

static char *
kk_mesh_virtual_sliced_source(void *mem, const char *source)
{
   return kk_mesh_source_replace(mem, source,
      "uint index = kk_group.x + kk_group.y * batch->mesh_dispatch[0];",
      "uint index = params.pad + kk_group.x + kk_group_count.x * "
      "(kk_group.y + kk_group_count.y * kk_group.z);");
}

static bool
kk_mesh_native_sliced_data(void *mem, const struct msl_compile_data *source,
                             bool task, struct msl_compile_data *out)
{
   if (!source || !source->code || !source->entrypoint_name)
      return false;
   const char *start = strstr(source->code, task
      ? "[[object, max_total_threads_per_threadgroup("
      : "[[mesh, max_total_threads_per_threadgroup(");
   const char *brace = start ? strchr(start, '{') : NULL;
   if (!brace)
      return false;
   const char *end = brace + 1;
   unsigned depth = 1;
   while (*end && depth) {
      if (*end == '{') ++depth;
      if (*end == '}') --depth;
      ++end;
   }
   if (depth || end - start > INT_MAX)
      return false;
   char *wrapper = ralloc_asprintf(mem, "%.*s", (int)(end - start), start);
   char *entry = ralloc_asprintf(mem, "%s_sliced", source->entrypoint_name);
   char *find = ralloc_asprintf(mem, "void %s(", source->entrypoint_name);
   char *replacement = ralloc_asprintf(mem, "void %s(", entry);
   wrapper = kk_mesh_source_replace(mem, wrapper, find, replacement);
   if (!wrapper)
      return false;

   /* Query-enabled native wrappers already have the parameter binding. */
   if (!strstr(wrapper, "constant kk_mesh_params &params")) {
      char *binding = ralloc_asprintf(mem,
         ", constant kk_mesh_params &params [[buffer(%u)]]) {",
         KK_ARGUMENT_MESH_PARAMETERS);
      wrapper = kk_mesh_source_replace(mem, wrapper, ") {", binding);
      if (!wrapper)
         return false;
   }
   const char *sys = "kk_mesh_sysvals sys = {kk_group, kk_group_count, kk_local, "
      "kk_index, kk_subgroup, kk_subgroups, kk_lane, kk_width};\n";
   const char *zero = task
      ? "if (kk_index == 0) kk_grid.set_threadgroups_per_grid(uint3(0,1,1));"
      : "if (kk_index == 0) kk_output.set_primitive_count(0);";
   char *adjust = ralloc_asprintf(mem,
      "%s"
      "    const device kk_mesh_batch *slice = (const device kk_mesh_batch *)params.batch;\n"
      "    uint local = kk_group.x + kk_group_count.x * (kk_group.y + kk_group_count.y * kk_group.z);\n"
      "    uint logical = params.task_base + local;\n"
      "    uint3 original = uint3(slice->input_grid[0],slice->input_grid[1],slice->input_grid[2]);\n"
      "    if (local >= params.payload_stride || ulong(logical) >= ulong(original.x) * original.y * original.z) {\n"
      "        threadgroup_barrier(mem_flags::mem_threadgroup);\n"
      "        %s\n"
      "        return;\n"
      "    }\n"
      "    sys.group_id = kk_mesh_unflatten(logical, original);\n"
      "    sys.group_count = original;\n", sys, zero);
   wrapper = kk_mesh_source_replace(mem, wrapper, sys, adjust);
   if (!wrapper)
      return false;
   *out = *source;
   out->entrypoint_name = entry;
   out->code = ralloc_asprintf(mem,
      "#define KK_MESH_VIRTUAL_ONLY 1\n%s\n%s\n", source->code, wrapper);
   return out->code != NULL;
}

#endif

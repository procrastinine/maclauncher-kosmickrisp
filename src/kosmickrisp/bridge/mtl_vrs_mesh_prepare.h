/* SPDX-License-Identifier: MIT */
#define KK_VRS_MESH_PREPARE_MSL \
   "/* SPDX-License-Identifier: MIT */\n" \
   "/* The Mesh-owned definitions/guards precede this source in the same library. */\n" \
   "inline uint kk_vrs_mesh_scalar(constant kk_vrs_mesh_prepare &p,\n" \
   "   const device uchar *group, uint slot, uint source, uint offset)\n" \
   "{\n" \
   "   if (source == KK_MESH_RETAINED_FIELD_ABSENT) return 0;\n" \
   "   if (source == KK_MESH_RETAINED_FIELD_CONSTANT) return offset;\n" \
   "   if (source == KK_MESH_RETAINED_FIELD_PER_DRAW)\n" \
   "      return *(const device uint *)((const device uchar *)p.per_draw + offset);\n" \
   "   if (source == KK_MESH_RETAINED_FIELD_PRIMITIVE)\n" \
   "      return *(const device uint *)(group + p.primitives + ulong(slot) * p.primitive_stride + offset);\n" \
   "   uint corner = p.provoking_last ? p.vertices_per_primitive - 1 : 0;\n" \
   "   uint index = ((const device uint *)(group + p.indices))[ulong(slot) * p.vertices_per_primitive + corner];\n" \
   "   if (index >= p.max_vertices) return 0;\n" \
   "   return *(const device uint *)(group + p.vertices + ulong(index) * p.vertex_stride + offset);\n" \
   "}\n" \
   "\n" \
   "/* One sequential GPU step over the bounded original retained batch. It\n" \
   " * never inspects a stale unused header or replaces an application ID. */\n" \
   "inline void kk_vrs_advance_mesh_job(constant kk_vrs_mesh_prepare &p)\n" \
   "{\n" \
   "   device kk_vrs_mesh_jobs_state &s = *(device kk_vrs_mesh_jobs_state *)p.job;\n" \
   "   s.job = {~0u, ~0u};\n" \
   "   if (s.finished) return;\n" \
   "   const device uint *grid = (const device uint *)p.grid;\n" \
   "   if (!grid[0] || !grid[1] || !grid[2]) { s.finished = 1; return; }\n" \
   "   if (!kk_mesh_retained_job_group_valid(0, grid[0], grid[1], grid[2], p.capacity)) {\n" \
   "      s.error = KK_VRS_JOBS_BAD_GRID; s.finished = 1; return;\n" \
   "   }\n" \
   "   while (kk_mesh_retained_job_group_valid(s.next_group, grid[0], grid[1], grid[2], p.capacity)) {\n" \
   "      if ((ulong(s.next_group) + 1) * p.group_stride > p.output_bytes) {\n" \
   "         s.error = KK_VRS_JOBS_BAD_SPAN; s.finished = 1; return;\n" \
   "      }\n" \
   "      const device uchar *group = (const device uchar *)p.outputs + ulong(s.next_group) * p.group_stride;\n" \
   "      const device kk_mesh_retained_count &count = *(const device kk_mesh_retained_count *)group;\n" \
   "      if (count.vertices > p.max_vertices || count.primitives > p.max_primitives) {\n" \
   "         s.error = KK_VRS_JOBS_BAD_COUNTS; s.finished = 1; return;\n" \
   "      }\n" \
   "      if (s.next_slot < count.primitives) {\n" \
   "         s.job = {s.next_group, s.next_slot++};\n" \
   "         return;\n" \
   "      }\n" \
   "      s.next_group++;\n" \
   "      s.next_slot = 0;\n" \
   "   }\n" \
   "   s.finished = 1;\n" \
   "}\n" \
   "\n" \
   "vertex void kk_vrs_prepare_mesh_job(uint i [[vertex_id]],\n" \
   "   constant kk_vrs_mesh_prepare &p [[buffer(KK_ARGUMENT_PER_DRAW)]])\n" \
   "{\n" \
   "   device kk_vrs_context &context = *(device kk_vrs_context *)p.context;\n" \
   "   if (i >= context.width * context.height) return;\n" \
   "   device kk_vrs_coverage *coverage = (device kk_vrs_coverage *)context.coverage;\n" \
   "   coverage[i] = {0, 0.0f, 0.0f, 0};\n" \
   "   ((device uint *)context.pixel_results)[i] = i;\n" \
   "   device kk_vrs_result *result = (device kk_vrs_result *)\n" \
   "      ((device uchar *)context.results + ulong(i) * context.result_stride);\n" \
   "   result->mask = 0;\n" \
   "   if (i == 0) {\n" \
   "      if (p.advance) kk_vrs_advance_mesh_job(p);\n" \
   "      /* Defaults for absent/invalid input are not storage-slot job IDs. */\n" \
   "      context.api_primitive = 0;\n" \
   "      context.primitive_rate = 0;\n" \
   "      const device kk_mesh_retained_job &job = *(const device kk_mesh_retained_job *)p.job;\n" \
   "      const device uint *grid = (const device uint *)p.grid;\n" \
   "      if (!kk_mesh_retained_job_group_valid(job.physical_group, grid[0], grid[1], grid[2], p.capacity))\n" \
   "         return;\n" \
   "      if ((ulong(job.physical_group) + 1) * p.group_stride > p.output_bytes)\n" \
   "         return;\n" \
   "      const device uchar *group = (const device uchar *)p.outputs + ulong(job.physical_group) * p.group_stride;\n" \
   "      const device kk_mesh_retained_count &count = *(const device kk_mesh_retained_count *)group;\n" \
   "      if (!kk_mesh_retained_job_slot_valid(job.original_primitive_slot, count.vertices,\n" \
   "            count.primitives, p.max_vertices, p.max_primitives)) return;\n" \
   "      context.api_primitive = kk_vrs_mesh_scalar(p, group, job.original_primitive_slot, p.id_source, p.id_offset);\n" \
   "      context.primitive_rate = kk_vrs_mesh_scalar(p, group, job.original_primitive_slot, p.rate_source, p.rate_offset);\n" \
   "   }\n" \
   "   return;\n" \
   "}\n" \
   ""

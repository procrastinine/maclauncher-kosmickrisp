/* SPDX-License-Identifier: MIT */
/* The Mesh-owned definitions/guards precede this source in the same library. */
inline uint kk_vrs_mesh_scalar(constant kk_vrs_mesh_prepare &p,
   const device uchar *group, uint slot, uint source, uint offset)
{
   if (source == KK_MESH_RETAINED_FIELD_ABSENT) return 0;
   if (source == KK_MESH_RETAINED_FIELD_CONSTANT) return offset;
   if (source == KK_MESH_RETAINED_FIELD_PER_DRAW)
      return *(const device uint *)((const device uchar *)p.per_draw + offset);
   if (source == KK_MESH_RETAINED_FIELD_PRIMITIVE)
      return *(const device uint *)(group + p.primitives + ulong(slot) * p.primitive_stride + offset);
   uint corner = p.provoking_last ? p.vertices_per_primitive - 1 : 0;
   uint index = ((const device uint *)(group + p.indices))[ulong(slot) * p.vertices_per_primitive + corner];
   if (index >= p.max_vertices) return 0;
   return *(const device uint *)(group + p.vertices + ulong(index) * p.vertex_stride + offset);
}

vertex void kk_vrs_prepare_mesh_job(uint i [[vertex_id]],
   constant kk_vrs_mesh_prepare &p [[buffer(KK_ARGUMENT_PER_DRAW)]])
{
   device kk_vrs_context &context = *(device kk_vrs_context *)p.context;
   if (i >= context.width * context.height) return;
   device kk_vrs_coverage *coverage = (device kk_vrs_coverage *)context.coverage;
   coverage[i] = {0, 0.0f, 0.0f, 0};
   ((device uint *)context.pixel_results)[i] = i;
   device kk_vrs_result *result = (device kk_vrs_result *)
      ((device uchar *)context.results + ulong(i) * context.result_stride);
   result->mask = 0;
   if (i == 0) {
      /* Defaults for absent/invalid input are not storage-slot job IDs. */
      context.api_primitive = 0;
      context.primitive_rate = 0;
      const device kk_mesh_retained_job &job = *(const device kk_mesh_retained_job *)p.job;
      const device uint *grid = (const device uint *)p.grid;
      if (!kk_mesh_retained_job_group_valid(job.physical_group, grid[0], grid[1], grid[2], p.capacity))
         return;
      if ((ulong(job.physical_group) + 1) * p.group_stride > p.output_bytes)
         return;
      const device uchar *group = (const device uchar *)p.outputs + ulong(job.physical_group) * p.group_stride;
      const device kk_mesh_retained_count &count = *(const device kk_mesh_retained_count *)group;
      if (!kk_mesh_retained_job_slot_valid(job.original_primitive_slot, count.vertices,
            count.primitives, p.max_vertices, p.max_primitives)) return;
      context.api_primitive = kk_vrs_mesh_scalar(p, group, job.original_primitive_slot, p.id_source, p.id_offset);
      context.primitive_rate = kk_vrs_mesh_scalar(p, group, job.original_primitive_slot, p.rate_source, p.rate_offset);
   }
   return;
}

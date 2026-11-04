/* SPDX-License-Identifier: MIT */
/* Native GS report programs share the shader's existing auxiliary slots.
 * Keep mandatory geometry/raster compilation independent of dynamic queries. */
#ifndef KK_GEOMETRY_REPORTS_H
#define KK_GEOMETRY_REPORTS_H

struct kk_geometry_report_build {
   struct kk_device *dev;
   struct kk_shader *shader;
   unsigned program;
};

static void
kk_compile_geometry_report_once(const void *data)
{
   const struct kk_geometry_report_build *build = data;
   unsigned i = build->program;
   struct kk_pipeline_handles *pipe = &build->shader->pipeline;
   if (i == 1 && build->shader->info.geometry_statistics &&
       build->shader->gs_aux[i].code) {
      VkResult result = kk_attachment_pipelines_create(build->dev, build->shader);
      if (result == VK_SUCCESS)
         result = kk_compile_compute_pipeline_mode(build->dev,
            &build->shader->gs_aux[i], 1, &pipe->gfx.gs_aux[i], false,
            &kk_statistics_pre_gs(build->shader)->function);
      pipe->gfx.gs_aux_result[i] = result;
      return;
   }
   pipe->gfx.gs_aux_result[i] = build->shader->gs_aux[i].code ?
      kk_compile_compute_pipeline(build->dev, &build->shader->gs_aux[i],
         i ? 1 : KK_POLY_WG_SIZE, &pipe->gfx.gs_aux[i]) :
      VK_ERROR_INVALID_SHADER_NV;
}

VkResult
kk_shader_ensure_geometry_reports(struct kk_device *dev,
   struct kk_shader *shader, unsigned mask)
{
   u_foreach_bit(i, mask & shader->info.gs_aux_mask) {
      const struct kk_geometry_report_build build = {dev, shader, i};
      util_call_once_data(&shader->pipeline.gfx.gs_aux_once[i],
                         kk_compile_geometry_report_once, &build);
      VkResult result = shader->pipeline.gfx.gs_aux_result[i];
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

#endif

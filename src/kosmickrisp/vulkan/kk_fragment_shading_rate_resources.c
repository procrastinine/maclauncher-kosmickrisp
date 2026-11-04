/* SPDX-License-Identifier: MIT */
#include "kk_fragment_shading_rate_resources.h"
#include "kk_shader.h"
#include "kk_mesh_pipeline_api.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_fragment_shading_rate.h"
#include "kosmickrisp/libkk/kk_argument_table.h"
#include <string.h>

VkResult
kk_vrs_cmd_prepare_native(struct kk_cmd_buffer *cmd,
   struct kk_shader *fragment, const struct kk_vrs_context *parameters,
   uint64_t original_per_draw, mtl_argument_table *original_table,
   mtl_argument_table *coarse_table, struct kk_vrs_prepared *out)
{
   if (!out)
      return VK_ERROR_UNKNOWN;
   *out = (struct kk_vrs_prepared){0};
   if (!cmd || !fragment || fragment != cmd->state.shaders[MESA_SHADER_FRAGMENT] ||
       !fragment->vrs_source || !parameters || !original_table || !coarse_table ||
       original_table == coarse_table)
      return VK_ERROR_UNKNOWN;
   const struct kk_rendering_state *render = &cmd->state.gfx.render;
   uint32_t samples = cmd->vk.dynamic_graphics_state.ms.rasterization_samples;
   if ((samples != 1 && samples != 2 && samples != 4) ||
       (parameters->reserved && parameters->reserved != samples) ||
       !kk_vrs_window_valid(parameters, &render->area) ||
       render->view_mask || render->layer_count != 1)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   const struct kk_vrs_source_data *source = kk_vrs_source_data(fragment->vrs_source);
   uint32_t stride = source ? source->result_stride : 0;
   if (stride < sizeof(struct kk_vrs_result) || stride % 16 ||
       (source->uses_per_draw && !original_per_draw))
      return VK_ERROR_INVALID_SHADER_NV;
   const uint32_t pixels = 32 * 32;
   const uint32_t coverage_offset = ALIGN_POT(sizeof(struct kk_vrs_context), 16);
   const uint32_t result_offset = coverage_offset + pixels * sizeof(struct kk_vrs_coverage);
   uint64_t map_offset = (uint64_t)result_offset + pixels * (uint64_t)stride;
   uint64_t bytes = map_offset + pixels * sizeof(uint32_t);
   if (bytes > 64u * 1024u * 1024u)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   struct kk_ptr allocation = kk_pool_alloc(cmd, bytes, 16);
   if (!allocation.cpu || !allocation.gpu)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   /* New storage only, before any of its GPU use is recorded. The continuation
    * scheduler must perform GPU clears between reused primitive jobs. */
   memset(allocation.cpu, 0, bytes);
   struct kk_vrs_context *context = allocation.cpu;
   *context = *parameters;
   context->coverage = allocation.gpu + coverage_offset;
   context->results = allocation.gpu + result_offset;
   context->pixel_results = allocation.gpu + map_offset;
   context->result_stride = stride;
   context->original_per_draw = original_per_draw;
   context->reserved = samples;
   context->sample_mask = cmd->vk.dynamic_graphics_state.ms.sample_mask;
   mtl_set_address(coarse_table, allocation.gpu, KK_ARGUMENT_PER_DRAW);
   *out = (struct kk_vrs_prepared){context, allocation.gpu, bytes};
   return VK_SUCCESS;
}

static bool
kk_vrs_mesh_field_valid(const struct kk_mesh_retained_field *field,
   const struct kk_mesh_retained_layout *layout)
{
   if (field->source == KK_MESH_RETAINED_FIELD_ABSENT ||
       field->source == KK_MESH_RETAINED_FIELD_CONSTANT)
      return true;
   if (field->source > KK_MESH_RETAINED_FIELD_CONSTANT || field->components != 1 ||
       nir_alu_type_get_type_size(field->type) != 32 ||
       (nir_alu_type_get_base_type(field->type) != nir_type_int &&
        nir_alu_type_get_base_type(field->type) != nir_type_uint))
      return false;
   uint32_t size = field->source == KK_MESH_RETAINED_FIELD_VERTEX ? layout->vertex_stride :
      field->source == KK_MESH_RETAINED_FIELD_PRIMITIVE ? layout->primitive_stride :
      sizeof(struct kk_per_draw_data);
   return !(field->offset % 4) && field->offset <= size && size - field->offset >= 4;
}

static VkResult
kk_vrs_cmd_prepare_mesh_job_state(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, uint64_t job,
   const struct mtl_vrs_pipelines *pipelines, mtl_argument_table *private_table,
   const struct kk_vrs_prepared *resources, bool advance)
{
   if (!cmd || !batch || !job || job % 4 || !pipelines || !private_table ||
       private_table == batch->mesh_table || !resources || !resources->context ||
       !resources->context_gpu || batch->encoder != cmd->metal.render ||
       batch->producer != cmd->state.gfx.shader || !batch->grid || !batch->outputs ||
       resources->context->original_per_draw != batch->per_draw_address ||
       resources->context->width != 32 || resources->context->height != 32)
      return VK_ERROR_UNKNOWN;
   const struct kk_mesh_retained_layout *layout = &batch->layout;
   const struct kk_mesh_retained_field *id = &layout->fields[KK_MESH_RETAINED_ID_FIELD];
   const struct kk_mesh_retained_field *rate = &layout->fields[KK_MESH_RETAINED_RATE_FIELD];
   if (!layout->vertices_per_primitive || !layout->group_stride ||
       !kk_vrs_mesh_field_valid(id, layout) || !kk_vrs_mesh_field_valid(rate, layout))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   const struct kk_vrs_mesh_prepare parameters = {
      .context = resources->context_gpu, .job = job,
      .outputs = batch->outputs, .output_bytes = batch->output_bytes,
      .grid = batch->grid, .per_draw = batch->per_draw_address,
      .capacity = batch->group_capacity, .group_stride = layout->group_stride,
      .vertices = layout->vertices, .vertex_stride = layout->vertex_stride,
      .primitives = layout->primitives, .primitive_stride = layout->primitive_stride,
      .indices = layout->indices, .vertices_per_primitive = layout->vertices_per_primitive,
      .max_vertices = layout->max_vertices, .max_primitives = layout->max_primitives,
      .provoking_last = cmd->vk.dynamic_graphics_state.rs.provoking_vertex ==
         VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT,
      .id_source = id->source, .id_offset = id->offset,
      .rate_source = rate->source, .rate_offset = rate->offset,
      .advance = advance,
   };
   struct kk_ptr upload = kk_pool_upload(cmd, &parameters, sizeof(parameters), 8);
   if (!upload.gpu)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (!mtl_vrs_prepare_mesh_job(batch->encoder, pipelines, private_table,
          upload.gpu, resources->context_gpu, 32 * 32))
      return VK_ERROR_UNKNOWN;
   mtl_render_set_argument_table(batch->encoder, cmd->argument_table, MTL_RENDER_STAGE_VERTEX);
   mtl_render_set_pipeline_state(batch->encoder, batch->raster_pipeline);
   return VK_SUCCESS;
}

VkResult
kk_vrs_cmd_prepare_mesh_job(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, uint64_t job,
   const struct mtl_vrs_pipelines *pipelines, mtl_argument_table *private_table,
   const struct kk_vrs_prepared *resources)
{
   return kk_vrs_cmd_prepare_mesh_job_state(cmd, batch, job,
      pipelines, private_table, resources, false);
}

VkResult
kk_vrs_cmd_begin_mesh_jobs(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, struct kk_vrs_mesh_jobs *out)
{
   if (!out) return VK_ERROR_UNKNOWN;
   *out = (struct kk_vrs_mesh_jobs){0};
   if (!cmd || !batch || batch->encoder != cmd->metal.render ||
       batch->producer != cmd->state.gfx.shader)
      return VK_ERROR_UNKNOWN;
   const struct kk_vrs_mesh_jobs_state initial = {.job = {UINT32_MAX, UINT32_MAX}};
   struct kk_ptr upload = kk_pool_upload(cmd, &initial, sizeof(initial), 8);
   if (!upload.gpu) return VK_ERROR_OUT_OF_HOST_MEMORY;
   uint64_t reader_parameters;
   VkResult result = kk_mesh_retained_raster_job_parameters(cmd, batch,
      upload.gpu, &reader_parameters);
   if (result != VK_SUCCESS)
      return result;
   *out = (struct kk_vrs_mesh_jobs){batch, upload.gpu, reader_parameters};
   return VK_SUCCESS;
}

VkResult
kk_vrs_cmd_prepare_next_mesh_job(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, const struct kk_vrs_mesh_jobs *jobs,
   const struct mtl_vrs_pipelines *pipelines, mtl_argument_table *private_table,
   const struct kk_vrs_prepared *resources)
{
   if (!jobs || jobs->batch != batch) return VK_ERROR_UNKNOWN;
   return kk_vrs_cmd_prepare_mesh_job_state(cmd, batch, jobs->gpu,
      pipelines, private_table, resources, true);
}

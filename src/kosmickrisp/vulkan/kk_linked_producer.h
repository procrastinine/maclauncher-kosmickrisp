/* SPDX-License-Identifier: MIT */
#ifndef KK_LINKED_PRODUCER_H
#define KK_LINKED_PRODUCER_H
#include "vk_graphics_state.h"
#include "vk_pipeline.h"
#include "nir.h"
#include "nir_serialize.h"
#include "util/blob.h"
#include "util/u_math.h"

/* Owned decode result. State pointers refer only to this value; NIR belongs to
 * mem_ctx. Ordinary VS/TCS native programs remain owned by their shaders. */
struct kk_linked_producer {
   nir_shader *nir;
   struct vk_graphics_pipeline_state state;
   struct vk_graphics_pipeline_all_state values;
   struct vk_pipeline_robustness_state robustness;
   uint64_t features;
   uint64_t attribs_read;
   bool pipeline_statistics, geometry_reports;
};

/* The caller passes the actual linked ordinary producer before native
 * position conversion, with resource and robustness lowering already done.
 * All encodings are explicit scalar values, never C padding or pointers. */
static inline void
kk_linked_producer_encode(struct blob *blob, nir_shader *nir,
   const struct vk_graphics_pipeline_state *state,
   const struct vk_pipeline_robustness_state *rs, uint64_t features, bool pipeline_statistics, bool geometry_reports)
{
   assert(nir->info.stage == MESA_SHADER_VERTEX || nir->info.stage == MESA_SHADER_TESS_EVAL);
   blob_write_uint32(blob, 1);
   blob_write_uint32(blob, nir->info.stage);
   blob_write_uint64(blob, features);
   blob_write_uint64(blob, nir->info.stage == MESA_SHADER_VERTEX
      ? nir->info.inputs_read >> VERT_ATTRIB_GENERIC0 : 0);
   blob_write_uint8(blob, pipeline_statistics);
   blob_write_uint8(blob, geometry_reports);
   blob_write_uint32(blob, state->shader_stages);
   blob_write_uint8(blob, state->mv != NULL);
   if (state->mv) blob_write_uint32(blob, state->mv->view_mask);
   for (unsigned i = 0; i < MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX; i++)
      blob_write_uint8(blob, !!BITSET_TEST(state->dynamic, i));
   blob_write_uint32(blob, rs->storage_buffers);
   blob_write_uint32(blob, rs->uniform_buffers);
   blob_write_uint32(blob, rs->vertex_inputs);
   blob_write_uint32(blob, rs->images);
   blob_write_uint32(blob, rs->null_uniform_buffer_descriptor);
   blob_write_uint32(blob, rs->null_storage_buffer_descriptor);
   blob_write_uint8(blob, state->vi != NULL);
   if (state->vi) {
      blob_write_uint32(blob, state->vi->bindings_valid);
      blob_write_uint32(blob, state->vi->attributes_valid);
      u_foreach_bit(i, state->vi->bindings_valid) {
         blob_write_uint32(blob, state->vi->bindings[i].input_rate);
         blob_write_uint32(blob, state->vi->bindings[i].divisor);
      }
      u_foreach_bit(i, state->vi->attributes_valid) {
         blob_write_uint32(blob, state->vi->attributes[i].binding);
         blob_write_uint32(blob, state->vi->attributes[i].format);
      }
   }
   blob_write_uint8(blob, state->ia != NULL);
   if (state->ia) {
      blob_write_uint32(blob, state->ia->primitive_topology);
   }
   blob_write_uint8(blob, state->rs != NULL);
   if (state->rs) {
      blob_write_uint32(blob, state->rs->depth_clamp_enable);
      blob_write_uint32(blob, state->rs->depth_clip_enable);
      blob_write_uint32(blob, state->rs->polygon_mode);
      blob_write_uint32(blob, state->rs->conservative_mode);
      blob_write_uint32(blob, state->rs->line.mode);
      blob_write_uint32(blob, state->rs->line.stipple.enable);
      blob_write_uint32(blob, fui(state->rs->line.width));
   }
   blob_write_uint8(blob, state->ms != NULL);
   if (state->ms) blob_write_uint32(blob, state->ms->rasterization_samples);
   nir_serialize(blob, nir, false);
}

static inline VkResult
kk_linked_producer_decode(const void *bytes, size_t size, void *mem_ctx,
   const struct nir_shader_compiler_options *options, struct kk_linked_producer *out)
{
   *out = (struct kk_linked_producer){0};
   struct blob_reader blob;
   blob_reader_init(&blob, bytes, size);
   if (blob_read_uint32(&blob) != 1) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   uint32_t stage = blob_read_uint32(&blob);
   if (stage != MESA_SHADER_VERTEX && stage != MESA_SHADER_TESS_EVAL)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   out->features = blob_read_uint64(&blob);
   out->attribs_read = blob_read_uint64(&blob);
   out->pipeline_statistics = blob_read_uint8(&blob);
   out->geometry_reports = blob_read_uint8(&blob);
   out->state.shader_stages = blob_read_uint32(&blob);
   if (blob_read_uint8(&blob)) {
      out->values.mv.view_mask = blob_read_uint32(&blob);
      out->state.mv = &out->values.mv;
   }
   for (unsigned i = 0; i < MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX; i++)
      if (blob_read_uint8(&blob)) BITSET_SET(out->state.dynamic, i);
   out->robustness.storage_buffers = blob_read_uint32(&blob);
   out->robustness.uniform_buffers = blob_read_uint32(&blob);
   out->robustness.vertex_inputs = blob_read_uint32(&blob);
   out->robustness.images = blob_read_uint32(&blob);
   out->robustness.null_uniform_buffer_descriptor = blob_read_uint32(&blob);
   out->robustness.null_storage_buffer_descriptor = blob_read_uint32(&blob);
   if (blob_read_uint8(&blob)) {
      out->state.vi = &out->values.vi;
      out->values.vi.bindings_valid = blob_read_uint32(&blob);
      out->values.vi.attributes_valid = blob_read_uint32(&blob);
      u_foreach_bit(i, out->values.vi.bindings_valid) {
         if (i >= MESA_VK_MAX_VERTEX_BINDINGS) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
         out->values.vi.bindings[i].input_rate = blob_read_uint32(&blob);
         out->values.vi.bindings[i].divisor = blob_read_uint32(&blob);
      }
      u_foreach_bit(i, out->values.vi.attributes_valid) {
         if (i >= MESA_VK_MAX_VERTEX_ATTRIBUTES) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
         out->values.vi.attributes[i].binding = blob_read_uint32(&blob);
         out->values.vi.attributes[i].format = blob_read_uint32(&blob);
      }
   }
   if (blob_read_uint8(&blob)) {
      out->state.ia = &out->values.ia;
      out->values.ia.primitive_topology = blob_read_uint32(&blob);
   }
   if (blob_read_uint8(&blob)) {
      out->state.rs = &out->values.rs;
      out->values.rs.depth_clamp_enable = blob_read_uint32(&blob);
      out->values.rs.depth_clip_enable = blob_read_uint32(&blob);
      out->values.rs.polygon_mode = blob_read_uint32(&blob);
      out->values.rs.conservative_mode = blob_read_uint32(&blob);
      out->values.rs.line.mode = blob_read_uint32(&blob);
      out->values.rs.line.stipple.enable = blob_read_uint32(&blob);
      out->values.rs.line.width = uif(blob_read_uint32(&blob));
   }
   if (blob_read_uint8(&blob)) {
      out->state.ms = &out->values.ms;
      out->values.ms.rasterization_samples = blob_read_uint32(&blob);
   }
   if (blob.overrun) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   out->nir = nir_deserialize(mem_ctx, options, &blob);
   if (!out->nir || blob.overrun || blob.current != blob.end || out->nir->info.stage != stage) {
      ralloc_free(out->nir);
      out->nir = NULL;
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   }
   return VK_SUCCESS;
}
#endif

/* SPDX-License-Identifier: MIT */
/* Private graphics-library implementation, included by kk_shader.c. */
struct kk_gpl_shader_state {
   struct msl_compile_data entry[2];
   mtl_function_descriptor *body;
   mtl_function_descriptor *fragment_body;
   mtl_function_descriptor *function[2];
   struct msl_compile_data fetch;
   mtl_function_descriptor *fetch_function;
   uint32_t attributes, integer_attributes;
};

static void
kk_gpl_shader_finish(struct kk_device *dev, struct kk_shader *shader,
                     const VkAllocationCallbacks *alloc)
{
   struct kk_gpl_shader_state *gpl = shader->gpl;
   if (!gpl) return;
   mtl_release(gpl->body);
   mtl_release(gpl->fragment_body);
   mtl_release(gpl->fetch_function);
   ralloc_free(gpl->fetch.code);
   ralloc_free(gpl->fetch.entrypoint_name);
   for (unsigned i = 0; i < ARRAY_SIZE(gpl->entry); ++i) {
      mtl_release(gpl->function[i]);
      ralloc_free(gpl->entry[i].code);
      ralloc_free(gpl->entry[i].entrypoint_name);
   }
   vk_free2(&dev->vk.alloc, alloc, gpl);
   shader->gpl = NULL;
}

static VkResult
kk_gpl_shader_allocate(struct kk_device *dev, struct kk_shader *shader,
                       const VkAllocationCallbacks *alloc)
{
   shader->gpl = vk_zalloc2(&dev->vk.alloc, alloc, sizeof(*shader->gpl), 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   return shader->gpl ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult
kk_gpl_link_executable(struct kk_device *dev, struct kk_shader *shader)
{
   struct kk_gpl_shader_state *gpl = shader->gpl;
   mtl_function_descriptor *vertex[] = {gpl->body, gpl->fetch_function};
   mtl_function_descriptor *fragment[] = {gpl->fragment_body};
   const struct kk_graphics_link_dependencies dependencies = {
      .vertex_count = gpl->fetch_function ? 2 : 1, .vertex = vertex,
      .fragment_count = 1, .fragment = fragment,
   };
   return kk_link_graphics_stages_with_libraries(dev, shader,
      &shader->msl_data[MESA_SHADER_VERTEX], &shader->msl_data[MESA_SHADER_FRAGMENT],
      gpl->function[0], gpl->function[1], 0, &dependencies);
}

static bool
kk_gpl_shader_serialize(const struct kk_shader *shader, struct blob *blob)
{
   const struct kk_gpl_shader_state *gpl = shader->gpl;
   if (!gpl) return false;
   blob_write_uint32(blob, 1); /* Native library payload version. */
   blob_write_uint32(blob, gpl->attributes);
   blob_write_uint32(blob, gpl->integer_attributes);
   unsigned count = shader->info.gpl_kind == KK_GPL_LIBRARY_FRAGMENT ? 1 : 2;
   for (unsigned i = 0; i < count; ++i)
      kk_msl_serialize(&gpl->entry[i], blob);
   if (shader->info.gpl_kind == KK_GPL_EXECUTABLE) {
      blob_write_uint8(blob, gpl->fetch.code != NULL);
      if (gpl->fetch.code) kk_msl_serialize(&gpl->fetch, blob);
   }
   return !blob->out_of_memory;
}

static VkResult
kk_gpl_shader_deserialize(struct kk_device *dev, struct kk_shader *shader,
   struct blob_reader *blob, const VkAllocationCallbacks *alloc)
{
   if (blob_read_uint32(blob) != 1)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   VkResult result = kk_gpl_shader_allocate(dev, shader, alloc);
   if (result != VK_SUCCESS) return result;
   struct kk_gpl_shader_state *gpl = shader->gpl;
   gpl->attributes = blob_read_uint32(blob);
   gpl->integer_attributes = blob_read_uint32(blob);
   if (blob->overrun || (gpl->integer_attributes & ~gpl->attributes) ||
       (shader->info.gpl_kind == KK_GPL_LIBRARY_FRAGMENT && gpl->attributes) ||
       (shader->info.gpl_kind != KK_GPL_LIBRARY_FRAGMENT &&
        gpl->attributes != shader->info.vs.attribs_read))
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   unsigned count = shader->info.gpl_kind == KK_GPL_LIBRARY_FRAGMENT ? 1 : 2;
   for (unsigned i = 0; i < count; ++i) {
      result = kk_msl_deserialize(blob, &gpl->entry[i]);
      if (result != VK_SUCCESS) return result;
   }
   if (shader->info.gpl_kind == KK_GPL_EXECUTABLE) {
      bool fetch = blob_read_uint8(blob);
      if (fetch != (gpl->attributes != 0) || blob->overrun)
         return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
      if (fetch) {
         result = kk_msl_deserialize(blob, &gpl->fetch);
         if (result != VK_SUCCESS) return result;
         result = kk_compile_graphics_stage(dev, &gpl->fetch, &gpl->fetch_function);
         if (result != VK_SUCCESS) return result;
      }
      result = kk_compile_graphics_stage(dev, &shader->msl_data[MESA_SHADER_FRAGMENT],
                                         &gpl->fragment_body);
      if (result != VK_SUCCESS) return result;
   }
   result = kk_compile_graphics_stage(dev, &shader->msl_data[shader->info.stage],
                                     &gpl->body);
   for (unsigned i = 0; result == VK_SUCCESS && i < count; ++i)
      result = kk_compile_graphics_module(dev, &gpl->entry[i], true, &gpl->function[i]);
   if (result == VK_SUCCESS && shader->info.gpl_kind == KK_GPL_EXECUTABLE)
      result = kk_gpl_link_executable(dev, shader);
   return result;
}

static VkResult
kk_gpl_source_library_shader(struct kk_device *dev, mesa_shader_stage stage,
   const VkAllocationCallbacks *alloc, struct kk_shader **out)
{
   struct kk_shader *shader = vk_shader_zalloc(&dev->vk, &kk_shader_ops, stage,
                                              alloc, kk_shader_size(stage));
   if (!shader) return VK_ERROR_OUT_OF_HOST_MEMORY;
   shader->info.stage = stage;
   shader->info.gpl_kind = KK_GPL_LIBRARY_SOURCE;
   kk_shader_init_mesh_state(shader, stage);
   *out = shader;
   return VK_SUCCESS;
}

static VkResult
kk_compile_graphics_library_shaders(struct vk_device *device, uint32_t count,
   struct vk_shader_compile_info *infos, const struct vk_graphics_pipeline_state *state,
   const struct vk_features *features, const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   struct kk_device *dev = container_of(device, struct kk_device, vk);
   memset(out, 0, count * sizeof(*out));
   for (unsigned i = 0; i < count; ++i) {
      struct kk_shader *shader = NULL;
      VkResult result = VK_PIPELINE_COMPILE_REQUIRED;
      /* Sparse recipes and coupled producer chains keep the common canonical
       * input. Their complete final state selects the existing joint compiler. */
      bool native = features && !features->sparseBinding &&
         !features->sparseResidencyBuffer && !features->sparseResidencyImage2D &&
         !features->sparseResidencyImage3D &&
         !(state->shader_stages & ~(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT));
      struct vk_graphics_pipeline_state part = *state;
      part.shader_stages = mesa_to_vk_shader_stage(infos[i].stage);
      if (native && infos[i].stage == MESA_SHADER_VERTEX) {
         struct kk_native_preraster_body body = {0};
         result = kk_compile_preraster_body(dev, &infos[i], &part, features, alloc, &body);
         if (result == VK_SUCCESS) {
            shader = body.body.shader;
            result = kk_gpl_shader_allocate(dev, shader, alloc);
            if (result == VK_SUCCESS) {
               shader->info.gpl_kind = KK_GPL_LIBRARY_PRERASTER;
               shader->gpl->attributes = body.attribute_mask;
               shader->gpl->integer_attributes = body.integer_attributes;
               shader->gpl->body = body.body.function;
               for (unsigned j = 0; j < ARRAY_SIZE(body.entry); ++j) {
                  shader->gpl->entry[j] = body.wrapper[j];
                  shader->gpl->function[j] = body.entry[j];
               }
            } else {
               kk_native_preraster_body_finish(dev, &body, alloc);
               shader = NULL;
            }
         }
      } else if (native && infos[i].stage == MESA_SHADER_FRAGMENT) {
         struct kk_native_fragment_body body = {0};
         result = kk_compile_fragment_body(dev, &infos[i], &part, features, alloc, &body);
         if (result == VK_SUCCESS) {
            shader = body.body.shader;
            result = kk_gpl_shader_allocate(dev, shader, alloc);
            if (result == VK_SUCCESS) {
               shader->info.gpl_kind = KK_GPL_LIBRARY_FRAGMENT;
               shader->gpl->body = body.body.function;
               shader->gpl->entry[0] = body.wrapper;
               shader->gpl->function[0] = body.entry;
            } else {
               kk_native_fragment_body_finish(dev, &body, alloc);
               shader = NULL;
            }
         }
      } else {
         ralloc_free(infos[i].nir);
      }
      /* Native module construction is an optimization. Keep real canonical
       * sources for final compilation when this interface cannot use it. */
      if (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_HOST_MEMORY &&
          result != VK_ERROR_DEVICE_LOST)
         result = kk_gpl_source_library_shader(dev, infos[i].stage, alloc, &shader);
      if (result != VK_SUCCESS) {
         for (unsigned j = 0; j < i; ++j) {
            kk_shader_destroy(device, container_of(out[j], struct kk_shader, vk), alloc);
            out[j] = NULL;
         }
         for (unsigned j = i + 1; j < count; ++j)
            ralloc_free(infos[j].nir);
         return result;
      }
      out[i] = &shader->vk;
   }
   return VK_SUCCESS;
}

/* Native assembly is optional; the retained complete compiler owns every
 * coupled state outside this boundary. No native body is mutated at link. */
static bool
kk_gpl_native_state(struct kk_device *dev, const struct vk_graphics_pipeline_state *state,
                     const struct vk_features *features, const struct kk_shader *vs,
                     const struct kk_shader *fs)
{
   if (!state || !features || !state->ia || !state->rs || !state->rp || !state->mv ||
       !state->ms || !state->cb || !vk_render_pass_state_has_attachment_info(state->rp) ||
       state->shader_stages != (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) ||
       state->mv->view_mask || state->rs->rasterizer_discard_enable ||
       features->sparseBinding || state->ms->rasterization_samples != 1 ||
       state->ms->sample_shading_enable || state->ms->alpha_to_coverage_enable ||
       state->ms->alpha_to_one_enable || !(state->ms->sample_mask & 1) ||
       state->ms->sample_locations_enable || state->cb->logic_op_enable ||
       vs->cache_uses_embedded_samplers || fs->cache_uses_embedded_samplers ||
       vs->embedded_sampler_count || fs->embedded_sampler_count ||
       kk_polygon_regular_possible(state,
          kk_depth_bias_control_flags(features->depthBiasControl,
             features->leastRepresentableValueForceUnormRepresentation ||
                features->floatRepresentation, state)) ||
       kk_conservative_possible(state) || kk_depth_bounds_pipeline(dev, state) ||
       kk_is_possible_both_depth_clip_clamp(state, true) || kk_line_effects_possible(state))
      return false;
   switch (state->ia->primitive_topology) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: break;
   default: return false;
   }
   /* These states use the existing encoder/root paths without shader changes.
    * Other dynamic states continue through the canonical compiler. */
   static const enum mesa_vk_dynamic_graphics_state allowed[] = {
      MESA_VK_DYNAMIC_VI, MESA_VK_DYNAMIC_VI_BINDINGS_VALID,
      MESA_VK_DYNAMIC_VI_BINDING_STRIDES, MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT,
      MESA_VK_DYNAMIC_VP_VIEWPORTS, MESA_VK_DYNAMIC_VP_SCISSOR_COUNT,
      MESA_VK_DYNAMIC_VP_SCISSORS, MESA_VK_DYNAMIC_RS_CULL_MODE,
      MESA_VK_DYNAMIC_RS_FRONT_FACE, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE,
      MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP,
      MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE, MESA_VK_DYNAMIC_DS_STENCIL_OP,
      MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK,
      MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS,
   };
   BITSET_DECLARE(unhandled, MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX);
   memcpy(unhandled, state->dynamic, sizeof(unhandled));
   for (unsigned i = 0; i < ARRAY_SIZE(allowed); ++i) BITSET_CLEAR(unhandled, allowed[i]);
   if (!BITSET_IS_EMPTY(unhandled)) return false;
   if (state->fsr && (state->fsr->fragment_size.width > 1 ||
                      state->fsr->fragment_size.height > 1)) return false;
   if (state->pipeline_flags &
       (VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
        VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT |
        VK_PIPELINE_CREATE_2_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
        VK_PIPELINE_CREATE_2_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)) return false;
   if (features->pipelineStatisticsQuery && !fs->msl_data[MESA_SHADER_FRAGMENT].uses_pipeline_statistics)
      return false;
   uint64_t colors = BITFIELD64_MASK(MAX_DRAW_BUFFERS) << FRAG_RESULT_DATA0;
   if (fs->info.outputs_written & ~colors) return false;
   for (unsigned i = 0; i < state->rp->color_attachment_count; ++i) {
      if (state->cal && state->cal->color_map[i] != i) return false;
      if (state->cb->attachments[i].blend_enable ||
          state->cb->attachments[i].write_mask != 15) return false;
   }
   uint64_t outputs = fs->info.outputs_written >> FRAG_RESULT_DATA0;
   u_foreach_bit64(color, outputs) {
      if (color >= state->rp->color_attachment_count) return false;
      VkFormat format = state->rp->color_attachment_formats[color];
      if (format == VK_FORMAT_UNDEFINED ||
          !kk_get_va_format(vk_format_to_pipe_format(format))->is_native) return false;
   }
   return true;
}

static bool
kk_gpl_copy_msl(struct msl_compile_data *dst, const struct msl_compile_data *src)
{
   kk_msl_copy(dst, src);
   return dst->code && dst->entrypoint_name;
}

static bool
kk_try_link_graphics_libraries(struct vk_device *device, uint32_t count,
   struct vk_shader_compile_info *infos, const struct vk_graphics_pipeline_state *state,
   const struct vk_features *features, const VkAllocationCallbacks *alloc,
   struct vk_shader **out, VkResult *result)
{
   if (count != 2 || infos[0].stage != MESA_SHADER_VERTEX ||
       infos[1].stage != MESA_SHADER_FRAGMENT || !infos[0].library_shader ||
       !infos[1].library_shader ||
       (infos[0].graphics_pipeline_flags & VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT) ||
       (infos[0].flags & (VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT |
                          VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT))) return false;
   const struct kk_shader *source_vs = container_of(infos[0].library_shader, struct kk_shader, vk);
   const struct kk_shader *source_fs = container_of(infos[1].library_shader, struct kk_shader, vk);
   struct kk_device *dev = container_of(device, struct kk_device, vk);
   if (source_vs->info.gpl_kind != KK_GPL_LIBRARY_PRERASTER ||
       source_fs->info.gpl_kind != KK_GPL_LIBRARY_FRAGMENT ||
       !kk_gpl_native_state(dev, state, features, source_vs, source_fs)) return false;
   struct kk_shader *shaders[2] = {0};
   memset(out, 0, count * sizeof(*out));
   *result = VK_ERROR_OUT_OF_HOST_MEMORY;
   for (unsigned i = 0; i < 2; ++i) {
      shaders[i] = vk_shader_zalloc(device, &kk_shader_ops, infos[i].stage,
                                    alloc, kk_shader_size(infos[i].stage));
      if (!shaders[i]) goto fail;
   }
   struct kk_shader *vs = shaders[0], *fs = shaders[1];
   vs->info = source_vs->info; vs->info.gpl_kind = KK_GPL_EXECUTABLE;
   fs->info = source_fs->info; fs->info.gpl_kind = KK_GPL_NONE;
   if (!kk_gpl_copy_msl(&vs->msl_data[MESA_SHADER_VERTEX], &source_vs->msl_data[MESA_SHADER_VERTEX]) ||
       !kk_gpl_copy_msl(&fs->msl_data[MESA_SHADER_FRAGMENT], &source_fs->msl_data[MESA_SHADER_FRAGMENT]))
      goto fail;
   if (kk_gpl_shader_allocate(dev, vs, alloc) != VK_SUCCESS) goto fail;
   struct kk_gpl_shader_state *gpl = vs->gpl;
   const struct kk_gpl_shader_state *pr = source_vs->gpl, *frag = source_fs->gpl;
   unsigned point = state->ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   gpl->attributes = pr->attributes; gpl->integer_attributes = pr->integer_attributes;
   gpl->body = mtl_retain(pr->body); gpl->fragment_body = mtl_retain(frag->body);
   gpl->function[0] = mtl_retain(pr->function[point]);
   gpl->function[1] = mtl_retain(frag->function[0]);
   if (!kk_gpl_copy_msl(&gpl->entry[0], &pr->entry[point]) ||
       !kk_gpl_copy_msl(&gpl->entry[1], &frag->entry[0])) goto fail;
   if (gpl->attributes) {
      struct kk_native_vertex_fetch fetch = {0};
      *result = kk_compile_dynamic_vertex_fetch(dev, state, gpl->attributes,
         gpl->integer_attributes, infos[0].robustness, &fetch);
      if (*result != VK_SUCCESS) goto fail;
      gpl->fetch = fetch.data; gpl->fetch_function = fetch.function;
   }
   gather_graphics_pipeline_create_info(state, shaders, 2);
   kk_shader_sampler_info(vs);
   if (!vs->msl_data[MESA_SHADER_FRAGMENT].code || !vs->msl_data[MESA_SHADER_FRAGMENT].entrypoint_name) {
      *result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail;
   }
   uint32_t push_size = MAX2(source_vs->info.push_size, source_fs->info.push_size);
   vs->info.push_size = fs->info.push_size = push_size;
   *result = kk_gpl_link_executable(dev, vs);
   if (*result != VK_SUCCESS) goto fail;
   for (unsigned i = 0; i < count; ++i) {
      ralloc_free(infos[i].nir);
      out[i] = &shaders[i]->vk;
   }
   return true;
fail:
   for (unsigned i = 0; i < ARRAY_SIZE(shaders); ++i)
      if (shaders[i]) kk_shader_destroy(device, shaders[i], alloc);
   if (*result == VK_ERROR_OUT_OF_HOST_MEMORY || *result == VK_ERROR_DEVICE_LOST) {
      for (unsigned i = 0; i < count; ++i) ralloc_free(infos[i].nir);
      return true;
   }
   /* The ordinary compiler still owns every canonical input on this path. */
   return false;
}

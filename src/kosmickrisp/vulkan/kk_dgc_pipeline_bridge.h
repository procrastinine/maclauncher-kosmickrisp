/* SPDX-License-Identifier: MIT */
/* Kept separate from ordinary DGC: Metal4 state handles are Apple10-only. */
struct kk_dgc_pipeline_metal {
   mtl_library *library;
   mtl_compute_pipeline_state *pipeline[2];
   bool failed[2];
};
static char *
kk_dgc_pipeline_source(void)
{
   const char *signature = "static void kk_dgc_expand_sequence(constant Params &p, uint i, bool enabled)";
   const char *start = strstr(kk_dgc_shader, signature);
   const char *end = start ? strstr(start, "/* Native commands consume") : NULL;
   if (!start || !end || strstr(start + strlen(signature), signature)) return NULL;
   return ralloc_asprintf(NULL,
      "%s#define KK_DGC_PIPELINE_ROW_SIZE %zu\n#define KK_DGC_PIPELINE_STATE_SIZE %zu\n"
      "%.*sstatic void kk_dgc_expand_thread(thread const Params &p, uint i, bool enabled)%.*s"
      "static void kk_dgc_expand_sequence(constant Params &p, uint i, bool enabled) { Params q = p; kk_dgc_expand_thread(q,i,enabled); }\n%s\n%s",
      KK_ARGUMENT_TABLE_MSL_PROLOG, sizeof(struct kk_dgc_pipeline_row), sizeof(struct kk_dgc_pipeline_state),
      (int)(start - kk_dgc_shader), kk_dgc_shader,
      (int)(end - start - strlen(signature)), start + strlen(signature),
      end, kk_dgc_pipeline_shader);
}
static mtl_compute_pipeline_state *
kk_dgc_pipeline_phase(struct kk_device *dev, bool native)
{
   struct kk_dgc_metal *dgc = dev->dgc;
   simple_mtx_lock(&dgc->pipeline_mutex);
   struct kk_dgc_pipeline_metal *state = dgc->switching;
   if (!state) dgc->switching = state = calloc(1, sizeof(*state));
   if (!state) { simple_mtx_unlock(&dgc->pipeline_mutex); return NULL; }
   unsigned phase = native;
   @try {
   if (!state->pipeline[phase] && !state->failed[phase]) {
      /* The joined public operation is already running on a KK worker. */
      char *source = state->library ? NULL : kk_dgc_pipeline_source();
      @try {
         if (!state->library && source)
            state->library = mtl_new_library(dev->mtl_compiler_handle, source,
               (enum mtl_language_version)MTLLanguageVersion4_0,
               MTL_MATH_MODE_SAFE, MTL_MATH_FLOATING_POINT_FUNCTIONS_PRECISE);
      } @finally { ralloc_free(source); }
      if (state->library) {
         mtl_function_descriptor *function = mtl_new_library_function_descriptor(
            state->library, native ? "kk_dgc_pipeline_commands" : "kk_dgc_pipeline_filter");
         if (function) {
            @try {
               state->pipeline[phase] = mtl_new_compute_pipeline_state(dev->mtl_compiler_handle, function, 32);
            } @finally { mtl_release(function); }
         }
      }
      state->failed[phase] = !state->pipeline[phase];
   }
   } @catch (NSException *error) {
      state->failed[phase] = true;
      fprintf(stderr, "DGC execution-set pipeline: %s\n", error.reason.UTF8String);
   }
   mtl_compute_pipeline_state *pipeline = state->pipeline[phase];
   simple_mtx_unlock(&dgc->pipeline_mutex);
   return pipeline;
}
static void
kk_dgc_pipeline_finish(struct kk_dgc_pipeline_metal *state)
{
   if (!state) return;
   for (unsigned i = 0; i < 2; ++i) mtl_release(state->pipeline[i]);
   mtl_release(state->library);
   free(state);
}
void
kk_dgc_pipeline_ids(struct kk_dgc_pipeline_row *row,
   mtl_render_pipeline_state *render, mtl_compute_pipeline_state *compute,
   mtl_depth_stencil_state *depth)
{
   row->render = render ? ((id<MTLRenderPipelineState>)render).gpuResourceID._impl : 0;
   row->compute = compute ? ((id<MTLComputePipelineState>)compute).gpuResourceID._impl : 0;
   row->depth = depth ? ((id<MTLDepthStencilState>)depth).gpuResourceID._impl : 0;
   row->has_depth = depth != NULL;
}
bool
kk_dgc_pipeline_encode(struct kk_cmd_buffer *cmd, mtl_compute_encoder *encoder,
   uint64_t parameters, uint64_t second, uint32_t count, bool native)
{
   mtl_compute_pipeline_state *pipeline = kk_dgc_pipeline_phase(kk_cmd_buffer_device(cmd), native);
   if (!pipeline) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INVALID_SHADER_NV);
      return false;
   }
   id<MTL4ArgumentTable> table = (id<MTL4ArgumentTable>)cmd->argument_table;
   [table setAddress:parameters atIndex:KK_ARGUMENT_ROOT];
   [table setAddress:second atIndex:KK_ARGUMENT_SAMPLERS];
   id<MTL4ComputeCommandEncoder> enc = (id<MTL4ComputeCommandEncoder>)encoder;
   [enc setArgumentTable:table];
   [enc setComputePipelineState:(id<MTLComputePipelineState>)pipeline];
   [enc dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];
   [enc barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch
      visibilityOptions:MTL4VisibilityOptionResourceAlias];
   [table setAddress:cmd->state.root_addr atIndex:KK_ARGUMENT_ROOT];
   [table setAddress:kk_cmd_buffer_device(cmd)->samplers.table.bo->gpu atIndex:KK_ARGUMENT_SAMPLERS];
   return true;
}

/* SPDX-License-Identifier: MIT */
/* Separate lazy library: ordinary DGC never parses Mesh ICB operations. */
struct kk_dgc_mesh_metal {
   mtl_library *library;
   mtl_compute_pipeline_state *pipeline[2];
   bool failed[2];
   mtl_library *expand_library;
   mtl_compute_pipeline_state *expand_pipeline;
   bool expand_failed;
};

static char *
kk_dgc_mesh_source(void)
{
   const char *words = "uint words = p.action == 2 ? 3 : (p.action == 1 ? 5 : 4);";
   const char *at = strstr(kk_dgc_shader, words);
   if (!at || strstr(at + strlen(words), words))
      return NULL;
   return ralloc_asprintf(NULL,
      "%s#define KK_DGC_MESH_WORDS %zu\n"
      "#define KK_DGC_MESH_STATE_SIZE %zu\n"
      "#define KK_DGC_MESH_PARAMETER_SIZE %zu\n"
      "#define KK_DGC_MESH_BATCH_SIZE %zu\n"
      "#define KK_DGC_MESH_METADATA_STRIDE %zu\n"
      "#define KK_DGC_MESH_INPUT_GRID %zu\n"
      "#define KK_DGC_MESH_DISPATCH %zu\n"
      "#define KK_DGC_MESH_COUNT %zu\n"
      "#define KK_DGC_MESH_BATCH %zu\n"
      "#define KK_DGC_MESH_INDIRECT_GRID %zu\n"
      "#define KK_DGC_MESH_INDIRECT_COUNT %zu\n"
      "#define KK_DGC_MESH_PREDICATE %zu\n"
      "#define KK_DGC_MESH_TASK_BASE %zu\n"
      "#define KK_DGC_MESH_PAYLOAD_STRIDE %zu\n"
      "#define KK_DGC_MESH_DRAW_ID %zu\n"
      "#define KK_DGC_MESH_MAX_DRAW_COUNT %zu\n"
      "#define KK_DGC_MESH_DIMENSION %u\n"
      "#define KK_DGC_MESH_MAX_GROUPS %u\n"
      "%.*suint words = 3;%s\n%s",
      KK_ARGUMENT_TABLE_MSL_PROLOG,
      sizeof(struct kk_mesh_params) / sizeof(uint64_t),
      sizeof(struct kk_dgc_mesh_params), sizeof(struct kk_mesh_params),
      sizeof(struct kk_mesh_batch),
      ALIGN_POT(sizeof(struct kk_mesh_params), 8) + sizeof(struct kk_mesh_batch),
      offsetof(struct kk_mesh_batch, input_grid),
      offsetof(struct kk_mesh_batch, mesh_dispatch),
      offsetof(struct kk_mesh_batch, mesh_count),
      offsetof(struct kk_mesh_params, batch),
      offsetof(struct kk_mesh_params, indirect_grid),
      offsetof(struct kk_mesh_params, indirect_count),
      offsetof(struct kk_mesh_params, predicate),
      offsetof(struct kk_mesh_params, task_base),
      offsetof(struct kk_mesh_params, payload_stride),
      offsetof(struct kk_mesh_params, draw_id),
      offsetof(struct kk_mesh_params, max_draw_count),
      KK_MESH_MAX_DIMENSION, KK_MESH_MAX_GROUPS,
      (int)(at - kk_dgc_shader), kk_dgc_shader, at + strlen(words),
      kk_dgc_mesh_shader);
}

struct kk_dgc_mesh_compile {
   struct kk_device *dev;
   const char *source;
   mtl_library *library;
   mtl_compute_pipeline_state *pipeline;
   unsigned phase;
   bool new_library, failed;
};
static void
kk_dgc_mesh_compile_execute(void *data)
{
   struct kk_dgc_mesh_compile *work = data;
   if (!work->library) {
      work->library = mtl_new_library(work->dev->mtl_compiler_handle,
         work->source, (enum mtl_language_version)MTLLanguageVersion3_1,
         MTL_MATH_MODE_SAFE, MTL_MATH_FLOATING_POINT_FUNCTIONS_PRECISE);
      work->new_library = work->library != NULL;
   }
   if (!work->library) return;
   mtl_function_descriptor *function = mtl_new_library_function_descriptor(
      work->library, work->phase ? "kk_dgc_mesh_commands" : "kk_dgc_expand");
   if (!function) return;
   @try {
      work->pipeline = mtl_new_compute_pipeline_state(
         work->dev->mtl_compiler_handle, function, 32);
   } @finally { mtl_release(function); }
}
static void
kk_dgc_mesh_compile_failed(void *data, const char *message)
{
   ((struct kk_dgc_mesh_compile *)data)->failed = true;
   fprintf(stderr, "DGC Mesh pipeline: %s\n", message);
}

static mtl_compute_pipeline_state *
kk_dgc_mesh_pipeline(struct kk_device *dev, bool native)
{
   struct kk_dgc_metal *dgc = dev->dgc;
   simple_mtx_lock(&dgc->pipeline_mutex);
   struct kk_dgc_mesh_metal *mesh = dgc->mesh;
   if (!mesh) {
      mesh = calloc(1, sizeof(*mesh));
      dgc->mesh = mesh;
   }
   if (!mesh) { simple_mtx_unlock(&dgc->pipeline_mutex); return NULL; }
   unsigned phase = native;
   if (!mesh->pipeline[phase] && !mesh->failed[phase]) {
      char *source = mesh->library ? NULL : kk_dgc_mesh_source();
      struct kk_dgc_mesh_compile work = {
         .dev = dev, .source = source, .library = mesh->library, .phase = phase,
      };
      if (mesh->library || source) {
         /* The public Mesh DGC command joined this KK-owned worker. Do not
          * enqueue another blocking job while holding the DGC phase mutex. */
         @try { kk_dgc_mesh_compile_execute(&work); }
         @catch (NSException *error) {
            kk_dgc_mesh_compile_failed(&work, error.reason.UTF8String);
         }
      } else work.failed = true;
      ralloc_free(source);
      if (!work.failed && work.pipeline) {
         mesh->library = work.library;
         mesh->pipeline[phase] = work.pipeline;
      } else {
         mtl_release(work.pipeline);
         if (work.new_library) mtl_release(work.library);
         mesh->failed[phase] = true;
      }
   }
   mtl_compute_pipeline_state *pipeline = mesh->pipeline[phase];
   simple_mtx_unlock(&dgc->pipeline_mutex);
   return pipeline;
}

uint32_t kk_dgc_mesh_group_limit(struct kk_device *dev)
{
   return dev->dgc->mesh_group_limit;
}

void *
kk_dgc_new_mesh_icb(struct kk_device *dev, uint32_t count,
   const struct kk_dgc_mesh_params *params)
{
   @autoreleasepool {
      MTLIndirectCommandBufferDescriptor *desc = [MTLIndirectCommandBufferDescriptor new];
      desc.commandTypes = MTLIndirectCommandTypeDrawMeshThreadgroups;
      desc.inheritPipelineState = YES;
      desc.inheritBuffers = NO;
      unsigned buffers = kk_argument_table_buffer_count((struct kk_argument_table_usage){
         .mesh = true, .poly_layer_replay = true,
         .linked_graphics = params->p.linked_vs_buffer || params->p.linked_fs_buffer,
         .interlock_guard = params->p.interlock_guard != 0,
      });
      desc.maxObjectBufferBindCount = buffers;
      desc.maxMeshBufferBindCount = buffers;
      desc.maxFragmentBufferBindCount = buffers;
      id<MTLIndirectCommandBuffer> icb = [(id<MTLDevice>)dev->mtl_handle
         newIndirectCommandBufferWithDescriptor:desc maxCommandCount:count
         options:MTLResourceStorageModePrivate];
      [desc release];
      return (void *)icb;
   }
}

bool
kk_dgc_mesh_encode(struct kk_cmd_buffer *cmd, mtl_compute_encoder *encoder,
   uint64_t parameters, uint32_t count, bool native)
{
   mtl_compute_pipeline_state *pipeline = kk_dgc_mesh_pipeline(
      kk_cmd_buffer_device(cmd), native);
   if (!pipeline) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INVALID_SHADER_NV);
      return false;
   }
   id<MTL4ArgumentTable> table = (id<MTL4ArgumentTable>)cmd->argument_table;
   [table setAddress:parameters atIndex:KK_ARGUMENT_ROOT];
   if (native) [table setAddress:parameters + sizeof(struct kk_dgc_mesh_params)
                        atIndex:KK_ARGUMENT_SAMPLERS];
   id<MTL4ComputeCommandEncoder> enc = (id<MTL4ComputeCommandEncoder>)encoder;
   [enc setArgumentTable:table];
   [enc setComputePipelineState:(id<MTLComputePipelineState>)pipeline];
   [enc dispatchThreads:MTLSizeMake(count, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
   [enc barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch
               visibilityOptions:MTL4VisibilityOptionResourceAlias];
   [table setAddress:cmd->state.root_addr atIndex:KK_ARGUMENT_ROOT];
   if (native) [table setAddress:kk_cmd_buffer_device(cmd)->samplers.table.bo->gpu
                        atIndex:KK_ARGUMENT_SAMPLERS];
   return true;
}

static void kk_dgc_mesh_finish(struct kk_dgc_mesh_metal *mesh)
{
   if (!mesh) return;
   for (unsigned i = 0; i < 2; ++i) mtl_release(mesh->pipeline[i]);
   mtl_release(mesh->expand_pipeline);
   mtl_release(mesh->expand_library);
   mtl_release(mesh->library);
   free(mesh);
}
struct kk_dgc_native_work { void *data; void (*execute)(void *); bool failed, completed; };
static void kk_dgc_native_work_execute(void *data)
{
   struct kk_dgc_native_work *work = data;
   work->execute(work->data);
   work->completed = true;
}
static void kk_dgc_native_work_failed(void *data, const char *message)
{
   ((struct kk_dgc_native_work *)data)->failed = true;
   fprintf(stderr, "DGC native command: %s\n", message);
}
static void kk_dgc_native_work_cleanup(void *data) { (void)data; }
bool kk_dgc_run_native_command(struct kk_device *dev, void *data,
   void (*execute)(void *))
{
   struct kk_dgc_native_work work = {data, execute, false, false};
   void *task = mtl_compiler_submit(dev->mtl_compiler_handle, &work,
      kk_dgc_native_work_execute, kk_dgc_native_work_failed,
      kk_dgc_native_work_cleanup);
   if (!task) return false;
   mtl_compiler_task_wait(task);
   mtl_release(task);
   return !work.failed && work.completed;
}

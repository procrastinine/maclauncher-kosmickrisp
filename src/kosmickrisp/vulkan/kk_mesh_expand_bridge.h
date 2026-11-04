/* SPDX-License-Identifier: MIT */
#include "kosmickrisp/compiler/kk_mesh_runtime.h"
#include "kk_mesh_expand_shader.h"
#include "kk_interlock_draw.h"

static mtl_compute_pipeline_state *
kk_mesh_expand_pipeline(struct kk_device *dev)
{
   if (!kk_dgc_init(dev)) return NULL;
   struct kk_dgc_metal *dgc = dev->dgc;
   simple_mtx_lock(&dgc->pipeline_mutex);
   if (!dgc->mesh) dgc->mesh = calloc(1, sizeof(*dgc->mesh));
   struct kk_dgc_mesh_metal *mesh = dgc->mesh;
   if (!mesh) { simple_mtx_unlock(&dgc->pipeline_mutex); return NULL; }
   if (!mesh->expand_pipeline && !mesh->expand_failed) {
      char *source = ralloc_asprintf(NULL,
         "%s#include <metal_stdlib>\n#include <metal_command_buffer>\nusing namespace metal;\n"
         "typedef uint uint32_t; typedef ulong uint64_t;\n"
         "#define KK_MESH_EXPAND_SIZE %zu\n%s\n%s",
         KK_ARGUMENT_TABLE_MSL_PROLOG, sizeof(struct kk_mesh_expand_params),
         kk_mesh_runtime_msl, kk_mesh_expand_shader);
      if (source) {
         @try {
            mesh->expand_library = mtl_new_library(dev->mtl_compiler_handle, source,
               (enum mtl_language_version)MTLLanguageVersion3_1,
               MTL_MATH_MODE_SAFE, MTL_MATH_FLOATING_POINT_FUNCTIONS_PRECISE);
            if (mesh->expand_library) {
               mtl_function_descriptor *fn = mtl_new_library_function_descriptor(
                  mesh->expand_library, "kk_mesh_expand_commands");
               if (fn) {
                  mesh->expand_pipeline = mtl_new_compute_pipeline_state(dev->mtl_compiler_handle, fn, 1);
                  mtl_release(fn);
               }
            }
         } @catch (NSException *error) {
            fprintf(stderr, "Mesh command expansion: %s\n", error.reason.UTF8String);
         }
      }
      ralloc_free(source);
      mesh->expand_failed = mesh->expand_pipeline == NULL;
   }
   mtl_compute_pipeline_state *pipeline = mesh->expand_pipeline;
   simple_mtx_unlock(&dgc->pipeline_mutex);
   return pipeline;
}

bool
kk_mesh_expand_native(struct kk_cmd_buffer *cmd, struct kk_mesh_expand_params *params,
   void **out_icb)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   mtl_compute_pipeline_state *pipeline = kk_mesh_expand_pipeline(dev);
   if (!pipeline) return false;
   struct kk_dgc_mesh_params description = {0};
   description.p.linked_vs_buffer = params->linked_vs;
   description.p.linked_fs_buffer = params->linked_fs;
   description.p.interlock_guard = params->guard;
   void *icb = kk_dgc_new_mesh_icb(dev, params->capacity, &description);
   if (!icb) return false;
   kk_device_add_buffer_to_residency_set(dev, icb);
   util_dynarray_append(&cmd->dgc_commands, icb);
   struct { struct kk_mesh_expand_params p; uint64_t id; } upload = {
      *params, kk_dgc_icb_id(icb),
   };
   uint64_t address = kk_pool_upload(cmd, &upload, sizeof(upload), 8).gpu;
   if (!address) return false;
   mtl_compute_encoder *encoder = cs_get_compute(cmd);
   mtl_argument_table *table = kk_new_command_argument_table(dev);
   if (!table) return false;
   mtl_set_address(table, address, KK_ARGUMENT_ROOT);
   mtl_set_address(table, address + sizeof(*params), KK_ARGUMENT_SAMPLERS);
   kk_cmd_bind_compute_arguments(cmd, encoder, table);
   mtl_compute_set_pipeline_state(encoder, pipeline);
   mtl_dispatch_threadgroups(encoder, (struct mtl_size){1,1,1}, (struct mtl_size){1,1,1});
   mtl_release(table);
   *out_icb = icb;
   return true;
}

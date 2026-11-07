/* SPDX-License-Identifier: MIT */
#include "nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#include "util/simple_mtx.h"
#include "kk_soft_fp64_library.h"
#include "kk_soft_fp64_spv.h"

/* Inlining clones each function into the destination shader. The cleaned
 * library is immutable, so concurrent compilations can share it. Keep a copy
 * of the compiler options: callers' option objects need not outlive a shader.
 */
struct kk_fp64_library {
   struct kk_fp64_library *next;
   nir_shader_compiler_options options;
   nir_shader *nir;
};

static simple_mtx_t library_mutex = SIMPLE_MTX_INITIALIZER;
static struct kk_fp64_library *libraries;
static util_once_flag library_once = UTIL_ONCE_FLAG_INIT;

static void
kk_soft_fp64_library_finish(void)
{
   while (libraries) {
      struct kk_fp64_library *entry = libraries;
      libraries = entry->next;
      ralloc_free(entry->nir);
      free(entry);
   }
   glsl_type_singleton_decref();
   simple_mtx_destroy(&library_mutex);
}

static void
kk_soft_fp64_library_init(void)
{
   /* The cache may outlive the last Vulkan instance. Retain its GLSL types
    * until the library is released, independently of instance lifetimes. */
   glsl_type_singleton_init_or_ref();
   atexit(kk_soft_fp64_library_finish);
}

static nir_shader *
kk_soft_fp64_library_create(const nir_shader_compiler_options *compiler_options)
{
   const struct spirv_capabilities capabilities = {
      .Addresses = true,
      .Float64 = true,
      .Int8 = true,
      .Int16 = true,
      .Int64 = true,
      .Shader = true,
   };
   const struct spirv_to_nir_options options = {
      .capabilities = &capabilities,
      .environment = NIR_SPIRV_VULKAN,
      .create_library = true,
   };
   nir_shader *library = spirv_to_nir(kk_soft_fp64_spv,
      sizeof(kk_soft_fp64_spv) / sizeof(kk_soft_fp64_spv[0]), NULL,
      MESA_SHADER_VERTEX, "main", &options, compiler_options);
   if (!library)
      return NULL;
   NIR_PASS(_, library, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, library, nir_lower_returns);
   NIR_PASS(_, library, nir_inline_functions);
   NIR_PASS(_, library, nir_lower_continue_constructs);

   /* Match glsl_float64_funcs_to_nir's cleanup before copying a function
    * into each operation. It reduces repeated work and basic blocks. */
   NIR_PASS(_, library, nir_opt_deref);
   NIR_PASS(_, library, nir_lower_vars_to_ssa);
   NIR_PASS(_, library, nir_remove_dead_variables, nir_var_function_temp, NULL);
   NIR_PASS(_, library, nir_opt_copy_prop);
   NIR_PASS(_, library, nir_opt_dce);
   NIR_PASS(_, library, nir_opt_cse);
   NIR_PASS(_, library, nir_opt_gcm, true);
   const nir_opt_peephole_select_options select = {.limit = 1};
   NIR_PASS(_, library, nir_opt_peephole_select, &select);
   NIR_PASS(_, library, nir_opt_dce);

   return library;
}

const nir_shader *
kk_soft_fp64_library(const nir_shader_compiler_options *options)
{
   util_call_once(&library_once, kk_soft_fp64_library_init);
   simple_mtx_lock(&library_mutex);
   struct kk_fp64_library *entry;
   for (entry = libraries; entry; entry = entry->next) {
      if (!memcmp(&entry->options, options, sizeof(*options)))
         break;
   }
   if (!entry) {
      entry = calloc(1, sizeof(*entry));
      if (entry) {
         memcpy(&entry->options, options, sizeof(*options));
         entry->nir = kk_soft_fp64_library_create(&entry->options);
         if (entry->nir) {
            entry->next = libraries;
            libraries = entry;
         } else {
            free(entry);
            entry = NULL;
         }
      }
   }
   const nir_shader *library = entry ? entry->nir : NULL;
   simple_mtx_unlock(&library_mutex);
   return library;
}

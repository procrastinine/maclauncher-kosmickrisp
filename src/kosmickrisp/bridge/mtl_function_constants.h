/* SPDX-License-Identifier: MIT */
/* Private pipeline-creation validation; no retained descriptor is modified. */
#include <Metal/MTLLibrary.h>
#include <Metal/MTL4MeshRenderPipeline.h>
#include <Metal/MTL4TileRenderPipeline.h>

static MTL4FunctionDescriptor *
kk_prepare_function_constants(MTL4FunctionDescriptor *descriptor, bool *valid)
{
   if (!descriptor)
      return nil;
   if ([descriptor isKindOfClass:[MTL4SpecializedFunctionDescriptor class]]) {
      MTL4SpecializedFunctionDescriptor *specialized = (id)descriptor;
      if (specialized.functionDescriptor && specialized.constantValues)
         return descriptor; /* Preserve the caller's exact value chain. */
      fprintf(stderr, "Metal function specialization is missing its source or values\n");
      *valid = false;
      return nil;
   }
   if (![descriptor isKindOfClass:[MTL4LibraryFunctionDescriptor class]]) {
      fprintf(stderr, "Cannot validate Metal function descriptor %s\n",
              NSStringFromClass(descriptor.class).UTF8String);
      *valid = false;
      return nil;
   }
   MTL4LibraryFunctionDescriptor *source = (id)descriptor;
   if (!source.library || !source.name) {
      fprintf(stderr, "Metal function descriptor is missing its library or name\n");
      *valid = false;
      return nil;
   }
   id<MTLFunction> function = [source.library newFunctionWithName:source.name];
   if (!function) {
      fprintf(stderr, "Metal function %s is unavailable\n", source.name.UTF8String);
      *valid = false;
      return nil;
   }
   NSDictionary<NSString *, MTLFunctionConstant *> *constants = function.functionConstantsDictionary;
   if (!constants.count) {
      [function release];
      return descriptor;
   }
   for (MTLFunctionConstant *constant in constants.allValues) {
      /* Color inputs depend on actual attachments, not an implicit default. */
      bool color = [constant.name hasPrefix:@"kk_color_enabled_"];
      if (constant.required || color) {
         fprintf(stderr, "Metal function %s requires an explicit value for %s\n",
                 source.name.UTF8String, constant.name.UTF8String);
         [function release];
         *valid = false;
         return nil;
      }
   }
   /* Empty values do not invent bindings: optional constants retain the
    * behavior explicitly expressed by is_function_constant_defined in MSL. */
   MTLFunctionConstantValues *values = [[MTLFunctionConstantValues new] autorelease];
   NSError *error = nil;
   id<MTLFunction> specialized = [source.library newFunctionWithName:source.name
      constantValues:values error:&error];
   bool ready = specialized && !specialized.functionConstantsDictionary.count;
   [function release];
   [specialized release];
   if (!ready) {
      fprintf(stderr, "Metal function %s specialization failed: %s\n",
              source.name.UTF8String,
              error.localizedDescription.UTF8String ?: "unresolved function constants");
      *valid = false;
      return nil;
   }
   MTL4SpecializedFunctionDescriptor *prepared =
      [[MTL4SpecializedFunctionDescriptor new] autorelease];
   prepared.functionDescriptor = descriptor;
   prepared.constantValues = values;
   return prepared;
}

static MTL4PipelineDescriptor *
kk_prepare_render_function_constants(MTL4PipelineDescriptor *descriptor)
{
   bool valid = true;
   MTL4PipelineDescriptor *prepared = descriptor;
#define KK_PREPARE_STAGE(type, field) do { \
   MTL4FunctionDescriptor *original = ((type *)descriptor).field; \
   MTL4FunctionDescriptor *function = kk_prepare_function_constants(original, &valid); \
   if (!valid) return nil; \
   if (function != original) { \
      if (prepared == descriptor) prepared = [[descriptor copy] autorelease]; \
      ((type *)prepared).field = function; \
   } \
} while (0)
   if ([descriptor isKindOfClass:[MTL4RenderPipelineDescriptor class]]) {
      KK_PREPARE_STAGE(MTL4RenderPipelineDescriptor, vertexFunctionDescriptor);
      KK_PREPARE_STAGE(MTL4RenderPipelineDescriptor, fragmentFunctionDescriptor);
   } else if ([descriptor isKindOfClass:[MTL4MeshRenderPipelineDescriptor class]]) {
      KK_PREPARE_STAGE(MTL4MeshRenderPipelineDescriptor, objectFunctionDescriptor);
      KK_PREPARE_STAGE(MTL4MeshRenderPipelineDescriptor, meshFunctionDescriptor);
      KK_PREPARE_STAGE(MTL4MeshRenderPipelineDescriptor, fragmentFunctionDescriptor);
   } else if ([descriptor isKindOfClass:[MTL4TileRenderPipelineDescriptor class]]) {
      KK_PREPARE_STAGE(MTL4TileRenderPipelineDescriptor, tileFunctionDescriptor);
   } else {
      fprintf(stderr, "Cannot validate Metal pipeline descriptor %s\n",
              NSStringFromClass(descriptor.class).UTF8String);
      return nil;
   }
#undef KK_PREPARE_STAGE
   return prepared;
}

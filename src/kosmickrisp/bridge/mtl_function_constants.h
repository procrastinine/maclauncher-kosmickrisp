/* SPDX-License-Identifier: MIT */
/* Private pipeline-creation validation; no retained descriptor is modified. */
#include <Metal/MTLLibrary.h>
#include <Metal/MTL4MeshRenderPipeline.h>
#include <Metal/MTL4TileRenderPipeline.h>

/* Every specialization owns a snapshot of all values over a named library
 * function. Copying the values preserves their native types and indices. */
static bool
kk_named_library_function(MTL4FunctionDescriptor *function)
{
   if (![function isKindOfClass:[MTL4LibraryFunctionDescriptor class]])
      return false;
   MTL4LibraryFunctionDescriptor *source = (id)function;
   return source.library && source.name.length;
}

/* Returns +1; callers may extend its private values before publishing it. */
static MTL4SpecializedFunctionDescriptor *
kk_new_function_specialization(MTL4FunctionDescriptor *function)
{
   MTLFunctionConstantValues *values = nil;
   MTL4FunctionDescriptor *previous_function = function;
   if ([function isKindOfClass:[MTL4SpecializedFunctionDescriptor class]]) {
      MTL4SpecializedFunctionDescriptor *previous = (id)function;
      function = previous.functionDescriptor;
      values = previous.constantValues;
      if (!values) return nil;
   }
   if (!kk_named_library_function(function)) return nil;
   MTL4SpecializedFunctionDescriptor *specialized =
      [MTL4SpecializedFunctionDescriptor new];
   kk_set_constant_record(specialized, kk_constant_record(values ? previous_function : nil));
   specialized.functionDescriptor = function;
   /* The SDK copy property gives each caller independent mutable storage. */
   specialized.constantValues = values ?: [[MTLFunctionConstantValues new] autorelease];
   if (!specialized.functionDescriptor || !specialized.constantValues) {
      [specialized release];
      return nil;
   }
   return specialized;
}

static MTL4FunctionDescriptor *
kk_prepare_function_constants(MTL4FunctionDescriptor *descriptor, bool *valid)
{
   if (!descriptor)
      return nil;
   if ([descriptor isKindOfClass:[MTL4SpecializedFunctionDescriptor class]]) {
      MTL4SpecializedFunctionDescriptor *specialized = (id)descriptor;
      if (kk_named_library_function(specialized.functionDescriptor) &&
          specialized.constantValues)
         return descriptor;
      fprintf(stderr, "Metal function specialization requires a named library source and values\n");
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
      [kk_new_function_specialization(descriptor) autorelease];
   if (!prepared) { *valid = false; return nil; }
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
      if (prepared == descriptor) { \
         prepared = [[descriptor copy] autorelease]; \
         kk_carry_constant_record(prepared, descriptor); \
      } \
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

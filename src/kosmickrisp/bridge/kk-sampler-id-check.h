/* SPDX-License-Identifier: MIT
 * CPU-only native sampler publication check. Included by mtl_sampler.m.
 */
#ifndef KK_SAMPLER_ID_CHECK_H
#define KK_SAMPLER_ID_CHECK_H
#include <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>

struct mtl_sampler_id_check {
   id<MTLArgumentEncoder> encoder;
   id<MTLBuffer> scratch;
};

struct mtl_sampler_id_check *
mtl_sampler_id_check_create(mtl_device *device)
{
   @autoreleasepool {
      struct mtl_sampler_id_check *check = calloc(1, sizeof(*check));
      if (!check)
         return NULL;
      MTLArgumentDescriptor *argument = [MTLArgumentDescriptor new];
      argument.dataType = MTLDataTypeSampler;
      argument.index = 0;
      argument.arrayLength = 1;
      argument.access = MTLBindingAccessReadOnly;
      check->encoder = [(id<MTLDevice>)device
         newArgumentEncoderWithArguments:@[argument]];
      [argument release];
      if (!check->encoder || check->encoder.encodedLength < sizeof(uint64_t)) {
         mtl_sampler_id_check_destroy(check);
         return NULL;
      }
      check->scratch = [(id<MTLDevice>)device
         newBufferWithLength:check->encoder.encodedLength
                     options:MTLResourceStorageModeShared];
      if (!check->scratch || !check->scratch.contents) {
         mtl_sampler_id_check_destroy(check);
         return NULL;
      }
      [check->encoder setArgumentBuffer:check->scratch offset:0];
      return check;
   }
}

void
mtl_sampler_id_check_destroy(struct mtl_sampler_id_check *check)
{
   @autoreleasepool {
      if (!check)
         return;
      [check->encoder release];
      [check->scratch release];
      free(check);
   }
}

bool
mtl_sampler_id_check_matches(struct mtl_sampler_id_check *check,
                            mtl_sampler *sampler)
{
   @autoreleasepool {
      id<MTLSamplerState> state = (id<MTLSamplerState>)sampler;
      uint64_t raw = state.gpuResourceID._impl;
      [check->encoder setSamplerState:state atIndex:0];
      uint64_t encoded;
      memcpy(&encoded, check->scratch.contents, sizeof(encoded));
      uint64_t after = state.gpuResourceID._impl;
      /* The scratch never executes on GPU and retains no sampled object. */
      [check->encoder setSamplerState:nil atIndex:0];
      return raw == encoded && after == encoded;
   }
}
#endif

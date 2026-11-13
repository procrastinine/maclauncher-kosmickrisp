// SPDX-License-Identifier: MIT
// Write the sampler LOD bias into sampled descriptors again
export const id = "write-the-sampler-lod-bias-into-sampled-descriptors";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_descriptor_set.c", source => source
    .replace("         desc[plane].sampler_reduction_mode = sampler->reduction_mode;\n         desc[plane].sampler_filter_bits = sampler->filter_bits;\n         desc[plane].sampler_address_modes = sampler->address_modes;\n         desc[plane].clamp_0_sampler_index_or_negative = -1;\n      }\n\n",
      () => "         desc[plane].sampler_reduction_mode = sampler->reduction_mode;\n         desc[plane].sampler_filter_bits = sampler->filter_bits;\n         desc[plane].sampler_address_modes = sampler->address_modes;\n         /* Byte 40 stays numeric on both encodings: the native Metal3 bias\n          * path and the metadata reader take the sampler bias from it. */\n         desc[plane].sampler_query_lod_bias_fp16 = sampler->query_lod_bias_fp16;\n         desc[plane].clamp_0_sampler_index_or_negative = -1;\n      }\n\n"));
}

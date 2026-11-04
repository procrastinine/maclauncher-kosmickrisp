/* SPDX-License-Identifier: MIT */
#ifndef KK_NIR_LOWER_FSR_H
#define KK_NIR_LOWER_FSR_H

#include "nir.h"

struct kk_fsr_lower_options {
   /* Read the pipeline rate and combiners from the root table. */
   bool dynamic_state;
   /* Otherwise: rate in bits 0-3, combiner ops in bits 4-6 and 8-10. */
   uint32_t static_state;
   /* Read the provoking vertex's PrimitiveShadingRateKHR. */
   bool primitive_rate;
   /* Read the rate attachment bound for the render pass. */
   bool attachment_rate;
   /* A partial dynamic sample mask selects 1x1. */
   bool dynamic_sample_mask;
};

bool kk_nir_fsr_supported(nir_shader *nir);
bool kk_nir_lower_fsr(nir_shader *nir, const struct kk_fsr_lower_options *opts);
bool kk_nir_lower_fsr_fine(nir_shader *nir);
bool kk_nir_lower_fsr_attachment(nir_shader *nir);

#endif

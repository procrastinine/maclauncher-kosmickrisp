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
   /* A mesh shader provides it per primitive. */
   bool primitive_rate_per_primitive;
   /* Read the rate attachment bound for the render pass. */
   bool attachment_rate;
   /* A partial dynamic sample mask selects 1x1. */
   bool dynamic_sample_mask;
   /* A static partial API mask requires 1x1 while retaining sample exchange. */
   bool force_fine;
   /* Rasterization may use more than one sample. */
   bool multisampled;
   /* Sample mask bits that cover every rasterization sample. */
   uint8_t full_sample_mask;
};

void kk_nir_fsr_prepare(nir_shader *nir);
bool kk_nir_fsr_supported(nir_shader *nir, bool force_fine);
bool kk_nir_lower_fsr(nir_shader *nir, const struct kk_fsr_lower_options *opts);
/* After descriptor lowering and primitive-ID forwarding: the attachment read
 * and the raster primitive ID. */
bool kk_nir_lower_fsr_late(nir_shader *nir);
bool kk_nir_lower_fsr_primitive(nir_shader *nir);
bool kk_nir_fsr_uses_slots(nir_shader *nir);
bool kk_nir_fsr_reads_root(nir_shader *nir);

#endif

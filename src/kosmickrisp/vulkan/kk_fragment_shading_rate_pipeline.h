/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_PIPELINE_H
#define KK_FRAGMENT_SHADING_RATE_PIPELINE_H
#include "kosmickrisp/compiler/kk_fragment_shading_rate.h"
/* Borrowed descriptor: caller must hold the owning shader through completion.
 * This does not select or advertise any Poly, mesh or GPL combination. */
mtl_render_pipeline_descriptor *
kk_vrs_native_pipeline_descriptor(const struct kk_shader *shader)
{
   return shader && shader->attachment_pipelines
      ? shader->attachment_pipelines->descriptor[0] : NULL;
}
#endif

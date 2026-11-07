/* SPDX-License-Identifier: MIT */
#ifndef KK_SAMPLER_EMULATION_H
#define KK_SAMPLER_EMULATION_H
#include "kk_device.h"
#include "util/u_atomic.h"

static inline bool
kk_sampler_emulation_active(const struct kk_device *dev)
{
   return p_atomic_read(&dev->sampler_emulation) != 0;
}
#endif

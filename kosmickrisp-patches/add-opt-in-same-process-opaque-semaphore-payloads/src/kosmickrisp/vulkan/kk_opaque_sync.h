/* SPDX-License-Identifier: MIT */
#ifndef KK_OPAQUE_SYNC_H
#define KK_OPAQUE_SYNC_H

struct kk_device;

/* Private opt-in only; external semaphore properties remain unchanged. */
void kk_opaque_sync_init_device(struct kk_device *dev);

#endif

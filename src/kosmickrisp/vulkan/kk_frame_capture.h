/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAME_CAPTURE_H
#define KK_FRAME_CAPTURE_H

struct kk_device;
struct kk_frame_capture;

/* Reads KK_CAPTURE_DIR once and installs dispatch only when requested. */
void kk_frame_capture_init(struct kk_device *dev);

#endif

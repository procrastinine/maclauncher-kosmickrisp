/* SPDX-License-Identifier: MIT */
#ifndef KK_EMULATION_H
#define KK_EMULATION_H
#include <stdbool.h>

struct vk_device_extension_table;
struct vk_features;
struct vk_properties;

/* Behavior entries: they change how the driver records commands, not what it
 * reports. */
struct kk_emulation_behavior {
   bool precise_timestamps;
};

/* Device creation only. Enables retain implemented support; disabling removes
 * every promise owned by the entry, including promoted/core query aliases. */
void kk_emulation_apply(const char *value, unsigned apple_family,
                        struct vk_device_extension_table *extensions,
                        struct vk_features *features,
                        struct vk_properties *properties,
                        struct kk_emulation_behavior *behavior);
#endif

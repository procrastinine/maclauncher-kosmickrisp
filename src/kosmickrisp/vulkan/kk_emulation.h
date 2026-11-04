/* SPDX-License-Identifier: MIT */
#ifndef KK_EMULATION_H
#define KK_EMULATION_H
struct vk_device_extension_table;
struct vk_features;
struct vk_properties;

/* Device creation only. Enables retain implemented support; disabling removes
 * every promise owned by the entry, including promoted/core query aliases. */
void kk_emulation_apply(const char *value, unsigned apple_family,
                        struct vk_device_extension_table *extensions,
                        struct vk_features *features,
                        struct vk_properties *properties);
#endif

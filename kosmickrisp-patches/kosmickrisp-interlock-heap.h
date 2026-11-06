/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_HEAP_H
#define KK_INTERLOCK_HEAP_H

#include <stdbool.h>
#include <vulkan/vulkan_core.h>

#define KK_POLY_HEAP_SIZE (128ull * 1024ull * 1024ull)

struct kk_device;
struct kk_cmd_buffer;
struct kk_bo;
struct kk_interlock_heap;

/* Only selected draws create a private Poly heap. A continuation may outlive
 * command-buffer reset, so recorded segments retain it independently. */
VkResult kk_interlock_heap_select(struct kk_cmd_buffer *cmd, bool selected);
struct kk_bo *kk_interlock_heap_bo(struct kk_interlock_heap *heap);
struct kk_interlock_heap *kk_interlock_heap_ref(struct kk_interlock_heap *heap);
void kk_interlock_heap_unref(struct kk_interlock_heap *heap);
void kk_interlock_heap_finish(struct kk_cmd_buffer *cmd);

#endif

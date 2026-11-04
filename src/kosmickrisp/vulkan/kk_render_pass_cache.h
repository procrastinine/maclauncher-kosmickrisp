/* SPDX-License-Identifier: MIT */
#ifndef KK_RENDER_PASS_CACHE_H
#define KK_RENDER_PASS_CACHE_H

#include "kk_cmd_buffer.h"
#include "kk_perf.h"
#include "kosmickrisp/bridge/mtl_bridge.h"

static inline struct kk_render_pass_cache *
kk_render_pass_cache_find(struct kk_cmd_buffer *cmd, mtl_render_pass_descriptor *desc)
{
   for (unsigned i = 0; i < KK_RENDER_PASS_CACHE_SIZE; ++i)
      if (desc && cmd->render_pass_cache[i].descriptor == desc)
         return &cmd->render_pass_cache[i];
   return NULL;
}

static inline mtl_render_pass_descriptor *
kk_render_pass_acquire(struct kk_cmd_buffer *cmd, uint8_t colors,
                       bool depth, bool stencil, mtl_buffer *visibility)
{
   struct kk_perf *perf = kk_cmd_buffer_device(cmd)->perf;
   for (unsigned i = 0; i < KK_RENDER_PASS_CACHE_SIZE; ++i) {
      struct kk_render_pass_cache *entry = &cmd->render_pass_cache[i];
      if (entry->leased)
         continue;
      if (!entry->descriptor) {
         entry->descriptor = mtl_new_render_pass_descriptor();
         if (!entry->descriptor)
            return NULL;
      } else {
         mtl_render_pass_descriptor_reuse(entry->descriptor, entry->color_mask,
            colors, entry->depth != NULL, depth, entry->stencil != NULL,
            stencil, visibility);
         if (unlikely(perf))
            kk_perf_add(perf, KK_PERF_DESCRIPTOR_REUSE, 0);
      }
      entry->leased = true;
      return entry->descriptor;
   }
   /* Nested meta/suspended passes retain their leased descriptors. Overflow
    * follows the existing owned native allocation/release path. */
   return mtl_new_render_pass_descriptor();
}

static inline void
kk_render_pass_release(struct kk_cmd_buffer *cmd, mtl_render_pass_descriptor *desc)
{
   struct kk_render_pass_cache *entry = kk_render_pass_cache_find(cmd, desc);
   if (entry)
      entry->leased = false;
   else
      mtl_release(desc);
}

static inline mtl_render_pass_attachment_descriptor *
kk_render_pass_color(struct kk_cmd_buffer *cmd, mtl_render_pass_descriptor *desc,
                      unsigned index)
{
   struct kk_render_pass_cache *entry = kk_render_pass_cache_find(cmd, desc);
   if (!entry)
      return mtl_render_pass_descriptor_get_color_attachment(desc, index);
   if (!entry->color[index]) {
      entry->color[index] = mtl_render_pass_descriptor_get_color_attachment(desc, index);
      entry->color_mask |= 1u << index;
   }
   return entry->color[index];
}

static inline mtl_render_pass_attachment_descriptor *
kk_render_pass_depth(struct kk_cmd_buffer *cmd, mtl_render_pass_descriptor *desc)
{
   struct kk_render_pass_cache *entry = kk_render_pass_cache_find(cmd, desc);
   if (!entry)
      return mtl_render_pass_descriptor_get_depth_attachment(desc);
   if (!entry->depth)
      entry->depth = mtl_render_pass_descriptor_get_depth_attachment(desc);
   return entry->depth;
}

static inline mtl_render_pass_attachment_descriptor *
kk_render_pass_stencil(struct kk_cmd_buffer *cmd, mtl_render_pass_descriptor *desc)
{
   struct kk_render_pass_cache *entry = kk_render_pass_cache_find(cmd, desc);
   if (!entry)
      return mtl_render_pass_descriptor_get_stencil_attachment(desc);
   if (!entry->stencil)
      entry->stencil = mtl_render_pass_descriptor_get_stencil_attachment(desc);
   return entry->stencil;
}

static inline void
kk_render_pass_cache_reset(struct kk_cmd_buffer *cmd)
{
   /* Recording has ended. Drop retained native textures before meta views and
    * command-owned images, but keep the descriptor and attachment objects. */
   mtl_render_pass_descriptor *active = cmd->state.gfx.render_pass_descriptor;
   if (active && !kk_render_pass_cache_find(cmd, active))
      mtl_release(active);
   cmd->state.gfx.render_pass_descriptor = NULL;
   for (unsigned i = 0; i < KK_RENDER_PASS_CACHE_SIZE; ++i) {
      struct kk_render_pass_cache *entry = &cmd->render_pass_cache[i];
      if (entry->descriptor)
         mtl_render_pass_descriptor_reuse(entry->descriptor, entry->color_mask,
            0, entry->depth != NULL, false, entry->stencil != NULL, false, NULL);
      entry->leased = false;
   }
   cmd->compute_bound_table = NULL;
}

static inline void
kk_render_pass_cache_finish(struct kk_cmd_buffer *cmd)
{
   for (unsigned i = 0; i < KK_RENDER_PASS_CACHE_SIZE; ++i)
      mtl_release(cmd->render_pass_cache[i].descriptor);
}

#endif

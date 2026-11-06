/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_LAYER_REPLAY_TYPES_H
#define KK_POLY_LAYER_REPLAY_TYPES_H

#include "kosmickrisp/libkk/kk_argument_table.h"
struct kk_cmd_buffer;
struct kk_draw_command;

/* Attachment binding preserves the same logical MRT/depth/resolve slice and
 * original clear/load/store state. It must not run user producer stages. */
typedef VkResult (*kk_poly_layer_bind)(struct kk_cmd_buffer *, void *, uint32_t);

struct kk_poly_layer_replay_state {
   kk_poly_layer_bind bind;
   void *data;
   uint32_t layer_count;
   uint64_t per_draw_address;
};

/* Optional immutable selector. MSL sees a nullable constant uint2 pointer:
 * x=logical_layer, y=enabled. Ordinary draws bind NULL and upload no selector. */
struct kk_poly_layer_selector {
   uint32_t logical_layer;
   uint32_t enabled;
};
static_assert(offsetof(struct kk_poly_layer_selector, logical_layer) == 0,
              "logical Layer is MSL uint2.x");
static_assert(offsetof(struct kk_poly_layer_selector, enabled) == 4,
              "selector enabled is MSL uint2.y");
static_assert(sizeof(struct kk_poly_layer_selector) == 8, "two selector words");
static_assert(KK_ARGUMENT_POLY_LAYER_REPLAY > KK_ARGUMENT_INTERLOCK_GUARD &&
              KK_ARGUMENT_POLY_LAYER_REPLAY > KK_ARGUMENT_MESH_PARAMETERS &&
              KK_ARGUMENT_POLY_LAYER_REPLAY > KK_ARGUMENT_FS_VFT_WRAPPER,
              "Layer selector does not alias existing entries");
static_assert(KK_ARGUMENT_POLY_LAYER_REPLAY < 31,
              "Layer selector fits Metal4's 31 buffer bindings");

/* Begin follows the resolved first kk_flush_gfx_state inside draw preparation,
 * before predicates, parameter uploads and producer stages.
 * End follows the original draw sequence. Data/callback live through end;
 * captured GPU outputs keep their normal same-command-execution lifetime. */
bool kk_poly_layer_replay_begin(struct kk_cmd_buffer *cmd, struct kk_draw_command *draw,
                               uint32_t layers,
                               kk_poly_layer_bind bind, void *data);
void kk_poly_layer_replay_end(struct kk_cmd_buffer *cmd);
#endif

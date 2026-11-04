/* SPDX-License-Identifier: MIT */
#ifndef KK_SCENE_LIFETIME_H
#define KK_SCENE_LIFETIME_H
#include <stdbool.h>
#include <stdint.h>
/* The command recording and every execution have separate ownership. The
 * caller holds a reference while finishing, including through user feedback. */
struct kk_scene_lifetime { unsigned references, finished; };
static inline void kk_scene_lifetime_init(struct kk_scene_lifetime *life)
{ life->references = 1; life->finished = 0; }
static inline void kk_scene_lifetime_retain(struct kk_scene_lifetime *life)
{ __atomic_add_fetch(&life->references, 1, __ATOMIC_RELAXED); }
static inline bool kk_scene_lifetime_release(struct kk_scene_lifetime *life)
{ return __atomic_sub_fetch(&life->references, 1, __ATOMIC_ACQ_REL) == 0; }
static inline bool kk_scene_lifetime_finish(struct kk_scene_lifetime *life)
{ return !__atomic_exchange_n(&life->finished, 1, __ATOMIC_ACQ_REL); }
#endif

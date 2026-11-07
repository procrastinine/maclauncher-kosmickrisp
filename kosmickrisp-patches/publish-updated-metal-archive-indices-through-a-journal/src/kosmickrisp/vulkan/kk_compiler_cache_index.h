/* SPDX-License-Identifier: MIT */
#ifndef KK_COMPILER_CACHE_INDEX_H
#define KK_COMPILER_CACHE_INDEX_H

#include "util/disk_cache.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Mesa content entries are immutable. Alternate two entries so replacing the
 * older slot never removes this writer's last complete published index. */
static inline void
kk_compiler_index_key(struct disk_cache *disk, const cache_key parent,
                      unsigned slot, cache_key key)
{
   struct {
      cache_key parent;
      char tag[sizeof("Metal4 index")];
      uint32_t slot;
   } input = {0};
   memcpy(input.parent, parent, sizeof(input.parent));
   memcpy(input.tag, "Metal4 index", sizeof(input.tag));
   input.slot = slot;
   disk_cache_compute_key(disk, &input, sizeof(input), key);
}

static inline void *
kk_compiler_index_load(struct disk_cache *disk, const cache_key parent,
                       uint64_t *sequence, size_t *size)
{
   void *best = NULL;
   size_t best_size = 0;
   *sequence = 0;
   *size = 0;
   for (unsigned slot = 0; slot < 2; ++slot) {
      cache_key key;
      kk_compiler_index_key(disk, parent, slot, key);
      size_t length = 0;
      void *record = disk_cache_get(disk, key, &length);
      uint64_t number = 0;
      if (record && length > sizeof(number))
         memcpy(&number, record, sizeof(number));
      if (number > *sequence && (number & 1) == slot) {
         free(best);
         best = record;
         best_size = length;
         *sequence = number;
      } else {
         free(record);
      }
   }
   if (best) {
      *size = best_size - sizeof(*sequence);
      memmove(best, (uint8_t *)best + sizeof(*sequence), *size);
   }
   return best;
}

static inline bool
kk_compiler_index_store(struct disk_cache *disk, const cache_key parent,
                        uint64_t *sequence, const void *bytes, size_t size)
{
   if (!size || size > SIZE_MAX - sizeof(uint64_t) || *sequence == UINT64_MAX)
      return false;
   uint64_t next = *sequence + 1;
   size_t length = sizeof(next) + size;
   uint8_t *record = malloc(length);
   if (!record)
      return false;
   memcpy(record, &next, sizeof(next));
   memcpy(record + sizeof(next), bytes, size);
   cache_key key;
   kk_compiler_index_key(disk, parent, next & 1, key);
   /* Remove only the older journal slot, not the current index or pieces.
    * Eviction and concurrent processes may still cause ordinary cache misses. */
   disk_cache_remove(disk, key);
   disk_cache_put(disk, key, record, length, NULL);
   disk_cache_wait_for_idle(disk);
   size_t stored_size = 0;
   void *stored = disk_cache_get(disk, key, &stored_size);
   bool complete = stored && stored_size == length &&
                   !memcmp(stored, record, length);
   free(stored);
   free(record);
   if (complete)
      *sequence = next;
   return complete;
}

#endif

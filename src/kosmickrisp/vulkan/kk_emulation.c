/* SPDX-License-Identifier: MIT */
#include "kk_emulation.h"
#include <vulkan/vulkan_core.h>
#include "vk_extensions.h"
#include "vk_physical_device_features.h"
#include "vk_physical_device_properties.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "kk_emulation_generated.h"

static bool
kk_emulation_token(const char *begin, size_t length, const char *name)
{
   return strlen(name) == length && memcmp(begin, name, length) == 0;
}

void
kk_emulation_apply(const char *value, unsigned apple_family,
                    struct vk_device_extension_table *extensions,
                    struct vk_features *features,
                    struct vk_properties *properties)
{
   const struct vk_device_extension_table before = *extensions;
   bool enabled[KK_EMULATION_COUNT];
   for (unsigned i = 0; i < KK_EMULATION_COUNT; ++i)
      enabled[i] = apple_family < 32 &&
         (kk_emulation_performance_families[i] & (1u << apple_family));
   bool hidden[VK_DEVICE_EXTENSION_COUNT] = {false};
   const char *cursor = value ? value : "";
   unsigned token_index = 0;
   while (*cursor) {
      const char *end = cursor + strcspn(cursor, ",");
      const char *begin = cursor, *trimmed = end;
      while (begin < trimmed && isspace((unsigned char)*begin)) ++begin;
      while (trimmed > begin && isspace((unsigned char)trimmed[-1])) --trimmed;
      size_t length = trimmed - begin;
      bool known = false;
      unsigned preset = 0;
      if (length == 6 && memcmp(begin, "apple", 5) == 0 &&
          begin[5] >= '7' && begin[5] <= '9')
         preset = begin[5] - '0';
      else if (length == 7 && memcmp(begin, "apple1", 6) == 0 &&
               begin[6] >= '0' && begin[6] <= '1')
         preset = 10 + begin[6] - '0';
      else if (length == 6 && memcmp(begin, "mode=", 5) == 0 &&
               begin[5] >= '7' && begin[5] <= '9')
         preset = begin[5] - '0';
      else if (length == 7 && memcmp(begin, "mode=1", 6) == 0 &&
               begin[6] >= '0' && begin[6] <= '1')
         preset = 10 + begin[6] - '0';
      if (token_index == 0 &&
          (kk_emulation_token(begin, length, "performance") ||
           kk_emulation_token(begin, length, "conformance"))) {
         bool all = kk_emulation_token(begin, length, "conformance");
         for (unsigned i = 0; i < KK_EMULATION_COUNT; ++i)
            enabled[i] = all || (apple_family < 32 &&
               (kk_emulation_performance_families[i] & (1u << apple_family)));
         known = true;
      } else if (preset) {
         for (unsigned i = 0; i < KK_EMULATION_COUNT; ++i) {
            if (kk_emulation_native_from[i] && kk_emulation_native_from[i] <= preset)
               enabled[i] = true;
         }
         known = true;
      } else if (length > 1 && (*begin == '+' || *begin == '-')) {
         for (unsigned i = 0; i < KK_EMULATION_COUNT; ++i) {
            if (kk_emulation_token(begin + 1, length - 1, kk_emulation_names[i])) {
               enabled[i] = *begin == '+';
               known = true;
               break;
            }
         }
         if (!known && *begin == '-') {
            for (unsigned i = 0; i < VK_DEVICE_EXTENSION_COUNT; ++i) {
               if (kk_emulation_token(begin + 1, length - 1,
                                      vk_device_extensions[i].extensionName)) {
                  hidden[i] = true;
                  known = true;
                  break;
               }
            }
         }
      }
      if (length && !known) {
         fputs("KosmicKrisp: unknown KK_EMULATION token '", stderr);
         size_t shown = length < 256 ? length : 256;
         for (size_t i = 0; i < shown; ++i)
            fputc(iscntrl((unsigned char)begin[i]) ? '?' : begin[i], stderr);
         if (shown < length) fputs("...", stderr);
         fputs("'\n", stderr);
      }
      ++token_index;
      cursor = *end ? end + 1 : end;
   }
   bool active[KK_EMULATION_COUNT];
   for (unsigned i = 0; i < KK_EMULATION_COUNT; ++i) {
      bool native = kk_emulation_native_from[i] &&
                    kk_emulation_native_from[i] <= apple_family;
      active[i] = enabled[i] || native;
   }
   for (unsigned i = 0; i < KK_EMULATION_COUNT; ++i) {
      if (!active[i])
         kk_emulation_disable(i, active, extensions, features, properties);
   }
   for (unsigned i = 0; i < VK_DEVICE_EXTENSION_COUNT; ++i) {
      if (hidden[i]) {
         extensions->extensions[i] = false;
         kk_emulation_hide_extension(vk_device_extensions[i].extensionName,
                                      features, properties);
      }
   }
   /* Remove only dependencies broken by this filter; retain the builder's
    * unrelated support and core-version alternatives. Device creation only. */
   kk_emulation_dependencies(&before, extensions, features, properties);
}

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
kk_emulation_apply(const char *value,
                    struct vk_device_extension_table *extensions,
                    struct vk_features *features,
                    struct vk_properties *properties)
{
   bool enabled[KK_EMULATION_COUNT];
   memcpy(enabled, kk_emulation_performance, sizeof(enabled));
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
      if (token_index == 0 &&
          (kk_emulation_token(begin, length, "performance") ||
           kk_emulation_token(begin, length, "conformance"))) {
         bool all = kk_emulation_token(begin, length, "conformance");
         for (unsigned i = 0; i < KK_EMULATION_COUNT; ++i)
            enabled[i] = all || kk_emulation_performance[i];
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
   for (unsigned i = 0; i < KK_EMULATION_COUNT; ++i) {
      if (!enabled[i]) kk_emulation_disable(i, extensions, features, properties);
   }
   for (unsigned i = 0; i < VK_DEVICE_EXTENSION_COUNT; ++i) {
      if (hidden[i]) {
         extensions->extensions[i] = false;
         kk_emulation_hide_extension(vk_device_extensions[i].extensionName,
                                      features, properties);
      }
   }
}

// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'sampler-capacity';
const headers = ['kk-sampler-cap-keys.h','kk-sampler-cap-native.h','kk-sampler-capacity.h'];
export const sourceFiles = [...headers, 'kk-sampler-id-check.h'].map(name => 'kosmickrisp-patches/' + name);
export function apply({edit, edits}) {
  for (const name of headers)
    edits.set('src/kosmickrisp/vulkan/' + name, fs.readFileSync(new URL(name, import.meta.url), 'utf8'));
  edits.set('src/kosmickrisp/bridge/kk-sampler-id-check.h', fs.readFileSync(new URL('kk-sampler-id-check.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/bridge/mtl_sampler.h', s => s.replace('#endif /* MTL_SAMPLER_H */', `/* Validate new raw IDs before they enter retained sampler tables. The caller
 * serializes this CPU-only scratch through its existing native cache mutex. */
struct mtl_sampler_id_check;
struct mtl_sampler_id_check *mtl_sampler_id_check_create(mtl_device *device);
void mtl_sampler_id_check_destroy(struct mtl_sampler_id_check *check);
bool mtl_sampler_id_check_matches(struct mtl_sampler_id_check *check,
                                  mtl_sampler *sampler);

#endif /* MTL_SAMPLER_H */`));
  edit('src/kosmickrisp/bridge/mtl_sampler.m', s => s.replace('#include <Metal/MTLSampler.h>', '#include <Metal/MTLSampler.h>\n#include "kk-sampler-id-check.h"'));
  edit('src/kosmickrisp/vulkan/kk_device.h', s => s
    .replace('struct kk_sampler_heap {', `struct kk_sampler_native_cache;
struct kk_sampler_heap {
   struct kk_sampler_native_cache *native_cache;
   uint32_t canonical_count;`)
    .replace('   uint32_t refcount;\n\n   /* Index', '   uint32_t refcount;\n   bool canonical;\n\n   /* Index'));
  edit('src/kosmickrisp/vulkan/kk_sampler.h', s => s
    .replace('#include "kk_device.h"', '#include "kk_device.h"\n#include "kosmickrisp/compiler/kk-sampler-params.h"')
    .replace('   bool native_only;', `   bool native_only;
   bool is_virtual;
   uint64_t params_gpu_address;
   uint16_t plane_filter_bits[2];`)
    .replace('#endif /* KK_SAMPLER_H */', `VkResult kk_sampler_cap_native_init(struct kk_device *dev);
void kk_sampler_cap_native_finish(struct kk_device *dev);
uint32_t kk_sampler_cap_native_keys(struct kk_device *dev,
   const struct mtl_sampler_packed **keys);
VkResult kk_sampler_cap_get_canonical(struct kk_device *dev,
   const struct mtl_sampler_packed *key, struct kk_rc_sampler **out, bool native_only);
VkResult kk_sampler_cap_finalize(struct kk_device *dev, struct kk_sampler *sampler,
   const VkSamplerCreateInfo *info, enum kk_sampler_params_lock_context lock_context);

#endif /* KK_SAMPLER_H */`));
  edit('src/kosmickrisp/vulkan/kk_device.c', s => {
    s = s.replace('#include "kk_device.h"', '#include "kk_device.h"\n#include "kk_sampler.h"\n#include "kk-sampler-cap-keys.h"');
    s = s.replace('static VkResult\nkk_init_sampler_heap(', `static VkResult kk_sampler_cap_reserve(struct kk_device *dev);
static void kk_destroy_sampler_heap(struct kk_device *dev, struct kk_sampler_heap *h);

static VkResult
kk_init_sampler_heap(`);
    s = s.replace('   simple_mtx_init(&h->lock, mtx_plain);\n   return VK_SUCCESS;', `   simple_mtx_init(&h->lock, mtx_plain);
   result = kk_sampler_cap_native_init(dev);
   if (result == VK_SUCCESS)
      result = kk_sampler_cap_reserve(dev);
   if (result != VK_SUCCESS)
      kk_destroy_sampler_heap(dev, h);
   return result;`);
    s = s.replace('   simple_mtx_destroy(&h->lock);', '   simple_mtx_destroy(&h->lock);\n   kk_sampler_cap_native_finish(dev);');
    s = s.replace('static VkResult\nkk_sampler_heap_add_locked(', `static VkResult
kk_sampler_cap_fallback_locked(struct kk_device *dev, struct kk_sampler_heap *h,
   const struct mtl_sampler_packed *requested, struct kk_rc_sampler **out, bool needs_index)
{
   struct mtl_sampler_packed key;
   kk_sampler_cap_key(&key, requested);
   struct hash_entry *entry = _mesa_hash_table_search(h->ht, &key);
   if (!entry)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct kk_rc_sampler *rc = entry->data;
   if (needs_index) {
      VkResult result = kk_sampler_require_index(dev, h, rc);
      if (result != VK_SUCCESS)
         return result;
   }
   ++rc->refcount;
   *out = rc;
   return VK_SUCCESS;
}

static VkResult
kk_sampler_heap_add_locked(`);
    s = s.replace('   struct hash_entry *ent = _mesa_hash_table_search(h->ht, &desc);',
      '   kk_sampler_cap_key_normalize(&desc, &desc);\n   struct hash_entry *ent = _mesa_hash_table_search(h->ht, &desc);');
    s = s.replace(`   if (!handle) {
      ralloc_free(rc);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }`, `   if (!handle) {
      ralloc_free(rc);
      if (h->canonical_count)
         return kk_sampler_cap_fallback_locked(dev, h, &desc, out, needs_index);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }`);
    s = s.replace('static void kk_sampler_heap_remove_locked(', `static VkResult
kk_sampler_cap_reserve(struct kk_device *dev)
{
   struct kk_sampler_heap *h = &dev->samplers;
   const struct mtl_sampler_packed *keys;
   uint32_t count = kk_sampler_cap_native_keys(dev, &keys);
   for (uint32_t i = 0; i < count; ++i) {
      struct kk_rc_sampler *rc;
      VkResult result = kk_sampler_heap_add_locked(dev, h, keys[i], &rc, true);
      if (result != VK_SUCCESS)
         return result;
      rc->canonical = true;
   }
   h->canonical_count = count;
   return VK_SUCCESS;
}

VkResult
kk_sampler_cap_get_canonical(struct kk_device *dev,
   const struct mtl_sampler_packed *requested, struct kk_rc_sampler **out, bool native_only)
{
   struct mtl_sampler_packed key;
   kk_sampler_cap_key(&key, requested);
   struct kk_sampler_heap *h = &dev->samplers;
   simple_mtx_lock(&h->lock);
   VkResult result = kk_sampler_cap_fallback_locked(dev, h, &key, out, !native_only);
   simple_mtx_unlock(&h->lock);
   return result;
}

static void kk_sampler_heap_remove_locked(`);
    s = s.replaceAll('   if (result == VK_SUCCESS) {\n      result = kk_sparse_sampler_added(dev, *out);',
      '   if (result == VK_SUCCESS && kk_sampler_cap_keys_equal(&desc, &(*out)->key)) {\n      result = kk_sparse_sampler_added(dev, *out);');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_sampler.c', s => s
    .replace('mtl_sampler *\nkk_sampler_create(', '#include "kk-sampler-cap-native.h"\n\nmtl_sampler *\nkk_sampler_create(')
    .replace(`   mtl_sampler_descriptor *desc = create_sampler_descriptor(dev, packed);
   mtl_sampler *sampler = mtl_new_sampler(dev->mtl_handle, desc);
   mtl_release(desc);
   return sampler;`, '   return kk_sampler_cap_native_create(dev, packed);')
    .replace('static VkResult\nkk_create_sampler(', '#include "kk-sampler-capacity.h"\n\nstatic VkResult\nkk_create_sampler(')
    .replace('(native_only ? kk_sampler_native_add : kk_sampler_heap_add)(dev, packed, &sampler->planes[0].hw)',
      'kk_sampler_cap_add_plane(dev, sampler, 0, packed, native_only)')
    .replaceAll('(native_only ? kk_sampler_native_add : kk_sampler_heap_add)(dev, packed, &sampler->planes[1].hw)',
      'kk_sampler_cap_add_plane(dev, sampler, 1, packed, native_only)'));
}

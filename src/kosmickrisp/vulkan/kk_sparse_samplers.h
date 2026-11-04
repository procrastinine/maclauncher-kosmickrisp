/* SPDX-License-Identifier: MIT
 * Included by kk_sparse.c under the existing sampler-heap mutex. No query
 * sampler is materialized: canonical point states are pinned by capacity.
 */
#ifndef KK_SPARSE_SAMPLERS_H
#define KK_SPARSE_SAMPLERS_H
#include "kosmickrisp/compiler/nir_to_msl.h"
#include "kosmickrisp/bridge/vk_to_mtl_map.h"
#include "kk-sampler-cap-keys.h"

struct kk_sparse_native_sampler {
   struct kk_sparse_native_sampler *next;
   struct kk_rc_sampler *source;
   mtl_buffer *record;
};
struct kk_sparse_sample_row {
   struct kk_sparse_sample_row *next;
   uint32_t base, levels;
   mtl_buffer *table;
   struct kk_sparse_native_sampler *native;
};

/* The same row serves native source metadata and bounded canonical taps.
 * word0[index] is a native params pointer; words2/3[address mode code] hold
 * black/white nearest normalized tap IDs. Virtual logical identity stays in
 * the existing immutable arena record, never in this physical-key index. */
static uint32_t
kk_sparse_physical_facts(const struct kk_rc_sampler *source)
{
   enum kk_sparse_physical_border border;
   switch (source->key.border_color) {
   case MTL_SAMPLER_BORDER_COLOR_TRANSPARENT_BLACK: border = KK_SPARSE_BORDER_TRANSPARENT_ZERO; break;
   case MTL_SAMPLER_BORDER_COLOR_OPAQUE_BLACK: border = KK_SPARSE_BORDER_OPAQUE_BLACK; break;
   case MTL_SAMPLER_BORDER_COLOR_OPAQUE_WHITE: border = KK_SPARSE_BORDER_ONE; break;
   default: UNREACHABLE("native sampler border enum");
   }
   enum kk_sparse_physical_reduction reduction;
   switch (source->key.reduction_mode) {
   case MTL_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE: reduction = KK_SPARSE_REDUCTION_WEIGHTED; break;
   case MTL_SAMPLER_REDUCTION_MODE_MINIMUM: reduction = KK_SPARSE_REDUCTION_MIN; break;
   case MTL_SAMPLER_REDUCTION_MODE_MAXIMUM: reduction = KK_SPARSE_REDUCTION_MAX; break;
   default: UNREACHABLE("native sampler reduction enum");
   }
   return kk_sparse_sampler_facts_encode(border, reduction);
}

static VkResult
kk_sparse_canonical_taps(struct kk_device *dev, struct kk_sparse_sample_row *row)
{
   uint64_t *ids = mtl_get_contents(row->table);
   for (unsigned u = 0; u <= VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; ++u)
   for (unsigned v = 0; v <= VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; ++v)
   for (unsigned w = 0; w <= VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; ++w)
   for (unsigned white = 0; white < 2; ++white) {
      struct mtl_sampler_packed key = {
         .mode_u = vk_sampler_address_mode_to_mtl_sampler_address_mode(u),
         .mode_v = vk_sampler_address_mode_to_mtl_sampler_address_mode(v),
         .mode_w = vk_sampler_address_mode_to_mtl_sampler_address_mode(w),
         .border_color = white ? MTL_SAMPLER_BORDER_COLOR_OPAQUE_WHITE :
                                 MTL_SAMPLER_BORDER_COLOR_TRANSPARENT_BLACK,
         .min_filter = MTL_SAMPLER_MIN_MAG_FILTER_NEAREST,
         .mag_filter = MTL_SAMPLER_MIN_MAG_FILTER_NEAREST,
         .mip_filter = MTL_SAMPLER_MIP_FILTER_NEAREST,
         .reduction_mode = MTL_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE,
         .compare_func = MTL_COMPARE_FUNCTION_ALWAYS,
         .max_lod = 1000.0f,
         .max_anisotropy = 1,
         .normalized_coordinates = true,
      };
      kk_sampler_cap_key(&key, &key);
      struct hash_entry *entry = _mesa_hash_table_search(dev->samplers.ht, &key);
      if (!entry || !((struct kk_rc_sampler *)entry->data)->canonical)
         return vk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                          "Canonical sparse point sampler is not reserved");
      struct kk_rc_sampler *tap = entry->data;
      unsigned mode = u | (v << 3) | (w << 6);
      assert(mode < 512 && mode < MSL_MAX_SAMPLERS);
      ids[KK_SPARSE_ROW_ENTRY_WORDS * mode + KK_SPARSE_ROW_BLACK_TAP_WORD + white] = mtl_sampler_get_gpu_resource_id(tap->handle);
   }
   return VK_SUCCESS;
}

static struct kk_sampler_params
kk_sparse_native_parameters(const struct kk_rc_sampler *source)
{
   const struct mtl_sampler_packed *key = &source->key;
   static_assert(MTL_COMPARE_FUNCTION_NEVER == VK_COMPARE_OP_NEVER, "compare ABI");
   static_assert(MTL_COMPARE_FUNCTION_ALWAYS == VK_COMPARE_OP_ALWAYS, "compare ABI");
   return (struct kk_sampler_params){
      .min_lod = key->min_lod, .max_lod = key->max_lod,
      .lod_bias = key->lod_bias, .max_anisotropy = key->max_anisotropy,
      .flags = (key->mip_filter == MTL_SAMPLER_MIP_FILTER_LINEAR ?
                KK_SAMPLER_PARAMS_MIPMAP_LINEAR : 0) |
               (key->max_anisotropy > 1 ? KK_SAMPLER_PARAMS_ANISOTROPY_ENABLE : 0),
      .compare_op = key->compare_func,
   };
}

static VkResult
kk_sparse_sample_row_add(struct kk_device *dev, struct kk_sparse_sample_row *row,
                         struct kk_rc_sampler *sampler)
{
   struct kk_sparse_native_sampler *native = row->native;
   for (; native; native = native->next) if (native->source == sampler) break;
   if (!native) {
      native = calloc(1, sizeof(*native));
      if (!native) return VK_ERROR_OUT_OF_HOST_MEMORY;
      native->record = mtl_sparse_new_residency_buffer(dev->mtl_handle,
         sizeof(struct kk_sparse_native_sampler_record), false);
      if (!native->record) { free(native); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
      native->source = sampler;
      native->next = row->native;
      struct kk_sparse_native_sampler_record *record = mtl_get_contents(native->record);
      *record = (struct kk_sparse_native_sampler_record){
         .next_address = row->native ? mtl_buffer_get_gpu_address(row->native->record) : 0,
         .source_id = mtl_sampler_get_gpu_resource_id(sampler->handle),
         .params = kk_sparse_native_parameters(sampler),
         .physical_facts = kk_sparse_physical_facts(sampler),
      };
      kk_sparse_residency(dev, native->record, true);
      row->native = native;
      *(uint64_t *)((char *)mtl_get_contents(row->table) + KK_SPARSE_NATIVE_SAMPLER_HEAD) =
         mtl_buffer_get_gpu_address(native->record);
   }
   if (sampler->index != UINT16_MAX) {
      assert(sampler->index < MSL_MAX_SAMPLERS);
      uint64_t *ids = mtl_get_contents(row->table);
      ids[KK_SPARSE_ROW_ENTRY_WORDS * sampler->index + KK_SPARSE_ROW_PHYSICAL_FACTS_WORD] = kk_sparse_physical_facts(sampler);
      ids[KK_SPARSE_ROW_ENTRY_WORDS * sampler->index + KK_SPARSE_ROW_NATIVE_PARAMS_WORD] = mtl_buffer_get_gpu_address(native->record) +
         offsetof(struct kk_sparse_native_sampler_record, params);
   }
   return VK_SUCCESS;
}

VkResult
kk_sparse_sampler_added(struct kk_device *dev, struct kk_rc_sampler *sampler)
{
   /* Reserve bypasses this callback. A different-key virtual fallback also
    * bypasses it; only actual exact-key native requests populate metadata. */
   sampler->sparse_query_source = true;
   for (struct kk_sparse_sample_row *row = dev->sparse_sample_rows; row; row = row->next) {
      VkResult result = kk_sparse_sample_row_add(dev, row, sampler);
      if (result != VK_SUCCESS) return result;
   }
   return VK_SUCCESS;
}

void
kk_sparse_sampler_removed(struct kk_device *dev, struct kk_rc_sampler *sampler)
{
   for (struct kk_sparse_sample_row *row = dev->sparse_sample_rows; row; row = row->next) {
      if (sampler->index != UINT16_MAX) {
         uint64_t *ids = mtl_get_contents(row->table);
         ids[KK_SPARSE_ROW_ENTRY_WORDS * sampler->index + KK_SPARSE_ROW_NATIVE_PARAMS_WORD] = 0;
         ids[KK_SPARSE_ROW_ENTRY_WORDS * sampler->index + KK_SPARSE_ROW_PHYSICAL_FACTS_WORD] = 0;
      }
      struct kk_sparse_native_sampler **link = &row->native;
      uint64_t *gpu_link = (void *)((char *)mtl_get_contents(row->table) + KK_SPARSE_NATIVE_SAMPLER_HEAD);
      while (*link && (*link)->source != sampler) {
         gpu_link = mtl_get_contents((*link)->record);
         link = &(*link)->next;
      }
      if (!*link) continue;
      struct kk_sparse_native_sampler *native = *link;
      *link = native->next;
      *gpu_link = native->next ? mtl_buffer_get_gpu_address(native->next->record) : 0;
      kk_sparse_residency(dev, native->record, false);
      mtl_release(native->record); free(native);
   }
}

static void
kk_sparse_sample_row_finish(struct kk_device *dev, struct kk_sparse_sample_row *row)
{
   while (row->native) {
      struct kk_sparse_native_sampler *native = row->native;
      row->native = native->next;
      kk_sparse_residency(dev, native->record, false);
      mtl_release(native->record); free(native);
   }
   kk_sparse_residency(dev, row->table, false);
   mtl_release(row->table); free(row);
}

void
kk_sparse_sample_rows_finish(struct kk_device *dev)
{
   while (dev->sparse_sample_rows) {
      struct kk_sparse_sample_row *row = dev->sparse_sample_rows;
      dev->sparse_sample_rows = row->next;
      kk_sparse_sample_row_finish(dev, row);
   }
}

static VkResult
kk_sparse_sample_row(struct kk_device *dev, uint32_t base, uint32_t levels, uint64_t *address)
{
   simple_mtx_lock(&dev->samplers.lock);
   struct kk_sparse_sample_row *row = dev->sparse_sample_rows;
   for (; row; row = row->next) if (row->base == base && row->levels == levels) break;
   VkResult result = VK_SUCCESS;
   if (!row) {
      row = calloc(1, sizeof(*row));
      if (!row) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto unlock; }
      row->base = base; row->levels = levels;
      row->table = mtl_sparse_new_residency_buffer(dev->mtl_handle, KK_SPARSE_NATIVE_SAMPLER_HEAD + 8, false);
      if (!row->table) { free(row); result = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto unlock; }
      kk_sparse_residency(dev, row->table, true);
      result = kk_sparse_canonical_taps(dev, row);
      if (result != VK_SUCCESS) { kk_sparse_sample_row_finish(dev, row); goto unlock; }
      hash_table_foreach(dev->samplers.ht, entry) {
         struct kk_rc_sampler *source = entry->data;
         if (source->index != UINT16_MAX) {
            assert(source->index < MSL_MAX_SAMPLERS);
            ((uint64_t *)mtl_get_contents(row->table))[
               KK_SPARSE_ROW_ENTRY_WORDS * source->index + KK_SPARSE_ROW_PHYSICAL_FACTS_WORD] =
               kk_sparse_physical_facts(source);
         }
         if (!source->sparse_query_source) continue;
         result = kk_sparse_sample_row_add(dev, row, source);
         if (result != VK_SUCCESS) { kk_sparse_sample_row_finish(dev, row); goto unlock; }
      }
      row->next = dev->sparse_sample_rows;
      dev->sparse_sample_rows = row;
   }
   *address = mtl_buffer_get_gpu_address(row->table);
unlock:
   simple_mtx_unlock(&dev->samplers.lock);
   return result;
}
#endif

// SPDX-License-Identifier: MIT
// Adapted from steamac patch 0080 (https://github.com/fxgl/steamac).
// Original author: steamac. Copyright 2026 FX GAMES FZ LLC and patch authors.
// steamac is Apache-2.0; its changes to Mesa follow Mesa MIT, per NOTICE.
// See steamac-NOTICE.txt, steamac-LICENSE.txt and steamac-MIT.txt.
// Escape NIR source object indices past 20 bits
export const id = "escape-nir-source-object-indices-past-20-bits";
export const sourceFiles = ['kosmickrisp-patches/steamac-NOTICE.txt', 'kosmickrisp-patches/steamac-LICENSE.txt', 'kosmickrisp-patches/steamac-MIT.txt'];
export function apply({edit, edits}) {
  edit("src/compiler/nir/nir_serialize.c", source => source
    .replace("\n#define NIR_SERIALIZE_FUNC_HAS_IMPL ((void *)(intptr_t)1)\n#define MAX_OBJECT_IDS              (1 << 20)\n\ntypedef struct {\n",
      () => "\n#define NIR_SERIALIZE_FUNC_HAS_IMPL ((void *)(intptr_t)1)\n\ntypedef struct {\n")
    .replace("{\n   uint32_t index = ctx->next_idx++;\n   assert(index != MAX_OBJECT_IDS);\n   _mesa_hash_table_insert(&ctx->remap_table, obj, (void *)(uintptr_t)index);\n}\n",
      () => "{\n   uint32_t index = ctx->next_idx++;\n   _mesa_hash_table_insert(&ctx->remap_table, obj, (void *)(uintptr_t)index);\n}\n")
    .replace("};\n\nstatic void\nwrite_src_full(write_ctx *ctx, const nir_src *src, union packed_src header)\n{\n   header.any.object_idx = write_lookup_object(ctx, src->ssa);\n   blob_write_uint32(ctx->blob, header.u32);\n}\n\n",
      () => "};\n\n/* A source's object index has 20 bits in its packed header. Larger shaders\n * have more objects: the largest 20-bit value means the index follows the\n * header as a separate uint32. Smaller indices are encoded as before. */\n#define SRC_OBJECT_IDX_SEPARATE ((1u << 20) - 1)\n\nstatic void\nwrite_src_full(write_ctx *ctx, const nir_src *src, union packed_src header)\n{\n   const uint32_t index = write_lookup_object(ctx, src->ssa);\n   header.any.object_idx = MIN2(index, SRC_OBJECT_IDX_SEPARATE);\n   blob_write_uint32(ctx->blob, header.u32);\n   if (index >= SRC_OBJECT_IDX_SEPARATE)\n      blob_write_uint32(ctx->blob, index);\n}\n\n")
    .replace("   header.u32 = blob_read_uint32(ctx->blob);\n\n   src->ssa = read_lookup_object(ctx, header.any.object_idx);\n   return header;\n}\n",
      () => "   header.u32 = blob_read_uint32(ctx->blob);\n\n   uint32_t index = header.any.object_idx;\n   if (index == SRC_OBJECT_IDX_SEPARATE)\n      index = blob_read_uint32(ctx->blob);\n   src->ssa = read_lookup_object(ctx, index);\n   return header;\n}\n"));
}

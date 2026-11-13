// SPDX-License-Identifier: MIT
// Count free memory in kernel pages on macOS
export const id = "count-free-memory-in-kernel-pages-on-macos";
export function apply({edit, edits}) {
  edit("docs/drivers/kosmickrisp.rst", source => source
    .replace("not measure compiler-service CPU usage or attribute asynchronous callback work\non other threads. Diagnostic logging, source hashing and timing add overhead.\n\nMesh dispatch\n*************\n\n",
      () => "not measure compiler-service CPU usage or attribute asynchronous callback work\non other threads. Diagnostic logging, source hashing and timing add overhead.\n\nMemory heap and budget\n**********************\n\nThe driver reports one device-local memory heap. Its size is the Metal device's\nrecommended maximum working set size. ``VK_EXT_memory_budget`` reports the Metal\ndevice's current allocated size as the heap's usage. The budget is that usage\nplus 90% of the system's free and inactive memory, limited to the heap size and\nrounded down to a whole MiB. Free and inactive memory is counted in kernel\npages, so an x86_64 process translated by Rosetta reports the same budget as an\narm64 process.\n\nMesh dispatch\n*************\n\n"));
  edit("src/util/os_misc.c", source => source
    .replace("      return false;\n   }\n\n   *size = ((uint64_t)vm_stats.free_count + (uint64_t)vm_stats.inactive_count) * PAGE_SIZE;\n   return true;\n#else\n   return false;\n",
      () => "      return false;\n   }\n\n   /* Host statistics count kernel pages. An x86_64 process translated by\n    * Rosetta has a 4 KiB PAGE_SIZE on a 16 KiB-page kernel, so PAGE_SIZE\n    * would report a quarter of the free memory. */\n   *size = ((uint64_t)vm_stats.free_count + (uint64_t)vm_stats.inactive_count) *\n           vm_kernel_page_size;\n   return true;\n#else\n   return false;\n"));
}

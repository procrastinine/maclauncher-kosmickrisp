// SPDX-License-Identifier: MIT
// Start predicted builds from an empty set under a new MSL salt
export const id = "start-predicted-builds-from-an-empty-set-under";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_async_compute.c", source => source
    .replace("   cache_key parent;\n   uint64_t sequence = 0;\n   if (disk) {\n      static const char tag[] = \"KosmicKrisp compute first binds 1\";\n      disk_cache_compute_key(disk, tag, sizeof(tag), parent);\n      size_t size = 0;\n      void *blob = kk_compiler_index_load(disk, parent, &sequence, &size);\n      unsigned count = size / sizeof(struct kk_bound_id);\n",
      () => "   cache_key parent;\n   uint64_t sequence = 0;\n   if (disk) {\n      /* KK_DEBUG_MSL_SALT starts a cold Metal cache; it also starts an empty set. */\n      struct { char tag[sizeof(\"KosmicKrisp compute first binds 1\")]; blake3_hash salt; } input = {\n         .tag = \"KosmicKrisp compute first binds 1\" };\n      const char *salt = getenv(\"KK_DEBUG_MSL_SALT\");\n      if (salt && *salt) _mesa_blake3_compute(salt, strlen(salt), input.salt);\n      disk_cache_compute_key(disk, &input, sizeof(input), parent);\n      size_t size = 0;\n      void *blob = kk_compiler_index_load(disk, parent, &sequence, &size);\n      unsigned count = size / sizeof(struct kk_bound_id);\n"));
}

/* SPDX-License-Identifier: MIT */
#ifndef KK_DRIVER_ARGUMENT_TABLE_H
#define KK_DRIVER_ARGUMENT_TABLE_H

/* Function constants 0..7 belong to framebuffer input presence. */
#define KK_FUNCTION_CONSTANT_PIPELINE_STATISTICS 8u
#define KK_FUNCTION_CONSTANT_ATOMIC64_LOCK_TABLE 9u
#define KK_FUNCTION_CONSTANT_SAMPLER_EMULATION 10u

/* Driver-owned buffer entries shared by native stages and command expansion.
 * VFT entries contain addresses of buffers holding table resource IDs. */
#define KK_ARGUMENT_ROOT             0u
#define KK_ARGUMENT_SAMPLERS         1u
#define KK_ARGUMENT_PER_DRAW         2u
#define KK_ARGUMENT_VS_VFT_WRAPPER   3u
#define KK_ARGUMENT_FS_VFT_WRAPPER   4u
#define KK_ARGUMENT_MESH_PARAMETERS  5u
#define KK_ARGUMENT_INTERLOCK_GUARD  6u
#define KK_ARGUMENT_POLY_LAYER_REPLAY (KK_ARGUMENT_INTERLOCK_GUARD + 1u)

#ifndef __METAL_VERSION__
#define KK_ARGUMENT_VALUE_STRING_(value) #value
#define KK_ARGUMENT_VALUE_STRING(value) KK_ARGUMENT_VALUE_STRING_(value)

/* Prepend to runtime-compiled MSL; values expand from the declarations above. */
#define KK_ARGUMENT_TABLE_MSL_PROLOG \
   "#define KK_ARGUMENT_ROOT " KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_ROOT) "\n" \
   "#define KK_ARGUMENT_SAMPLERS " KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_SAMPLERS) "\n" \
   "#define KK_ARGUMENT_PER_DRAW " KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_PER_DRAW) "\n" \
   "#define KK_ARGUMENT_VS_VFT_WRAPPER " KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_VS_VFT_WRAPPER) "\n" \
   "#define KK_ARGUMENT_FS_VFT_WRAPPER " KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_FS_VFT_WRAPPER) "\n" \
   "#define KK_ARGUMENT_MESH_PARAMETERS " KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_MESH_PARAMETERS) "\n" \
   "#define KK_ARGUMENT_INTERLOCK_GUARD " KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_INTERLOCK_GUARD) "\n" \
   "#define KK_ARGUMENT_POLY_LAYER_REPLAY " KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_POLY_LAYER_REPLAY) "\n"

struct kk_argument_table_usage {
   unsigned linked_graphics;
   unsigned mesh;
   unsigned interlock_guard;
   unsigned poly_layer_replay;
};

/* One capacity rule for ordinary tables and indirect-command descriptors. */
static inline unsigned
kk_argument_table_buffer_count(struct kk_argument_table_usage use)
{
   unsigned highest = KK_ARGUMENT_PER_DRAW;
   if (use.linked_graphics)
      highest = KK_ARGUMENT_FS_VFT_WRAPPER;
   if (use.mesh)
      highest = KK_ARGUMENT_MESH_PARAMETERS;
   if (use.interlock_guard)
      highest = KK_ARGUMENT_INTERLOCK_GUARD;
   if (use.poly_layer_replay)
      highest = KK_ARGUMENT_POLY_LAYER_REPLAY;
   return highest + 1u;
}

#endif /* !__METAL_VERSION__ */

#endif

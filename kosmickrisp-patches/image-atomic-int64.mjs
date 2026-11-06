// SPDX-License-Identifier: MIT
// Full-width image operations share the existing atomic-int64 lock table.
const imageMSL = String.raw`
template<typename T> static inline bool kk_image64_bounds(T t, uint c)
{ return c < t.get_width(); }
template<typename T> static inline bool kk_image64_bounds(T t, uint2 c)
{ return all(c < uint2(t.get_width(), t.get_height())); }
template<typename T> static inline bool kk_image64_bounds(T t, uint3 c)
{ return all(c < uint3(t.get_width(), t.get_height(), t.get_depth())); }
template<typename T, typename C, bool Array> struct kk_image64_texel;
template<typename T, typename C> struct kk_image64_texel<T, C, false> {
    T image; C coord; uint layer;
    bool valid() { return kk_image64_bounds(image, coord); }
    ulong read() { return image.read(coord).x; }
    void minimum(ulong v) { image.atomic_min(coord, ulong4(v)); }
    void maximum(ulong v) { image.atomic_max(coord, ulong4(v)); }
};
template<typename T, typename C> struct kk_image64_texel<T, C, true> {
    T image; C coord; uint layer;
    bool valid() { return kk_image64_bounds(image, coord) && layer < image.get_array_size(); }
    ulong read() { return image.read(coord, layer).x; }
    void minimum(ulong v) { image.atomic_min(coord, layer, ulong4(v)); }
    void maximum(ulong v) { image.atomic_max(coord, layer, ulong4(v)); }
};
template<typename Pixel> static inline ulong
kk_image64_update(Pixel pixel, uint2 coord, device atomic_uint *table,
                  uint op, ulong value, ulong compare, bool unused)
{
    if (!pixel.valid()) return 0ul;
    // Views have no x/y subrect offsets. Ignoring resource, mip and slice
    // makes aliases agree, with conservative collisions across resources.
    uint key = coord.x * 0x9e3779b1u ^ coord.y * 0x85ebca77u;
    device atomic_uint *lock = kk_atomic64_lock(ulong(key) << 3, table);
    ulong result = 0ul;
    bool done = false;
    while (!done) {
        uint expected = 0u;
        if (atomic_compare_exchange_weak_explicit(lock, &expected, 1u,
                memory_order_relaxed, memory_order_relaxed)) {
            atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_texture,
                                memory_order_seq_cst, thread_scope_device);
            if (unused && (op == 2u || op == 4u)) {
                if (op == 2u) pixel.minimum(value);
                else pixel.maximum(value);
            } else {
                result = pixel.read();
                ulong next = kk_atomic64_value(result, op, value, compare);
                // One native RMW publishes the complete value and depends on
                // the locked read, including loads and unchanged CAS results.
                if (next < result) pixel.minimum(next);
                else pixel.maximum(next);
            }
            atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_texture,
                                memory_order_seq_cst, thread_scope_device);
            atomic_store_explicit(lock, 0u, memory_order_relaxed);
            done = true;
        }
    }
    return result;
}
template<typename T, typename C> static inline ulong
kk_image_atomic64(T image, C coord, uint2 key, device atomic_uint *table,
                   uint op, ulong value, ulong compare, bool unused)
{
    return kk_image64_update(kk_image64_texel<T,C,false>{image,coord,0},
                             key,table,op,value,compare,unused);
}
template<typename T, typename C> static inline ulong
kk_image_atomic64_array(T image, C coord, uint layer, uint2 key,
                         device atomic_uint *table, uint op, ulong value,
                         ulong compare, bool unused)
{
    return kk_image64_update(kk_image64_texel<T,C,true>{image,coord,layer},
                             key,table,op,value,compare,unused);
}
`;

const imageCompiler = String.raw`
static inline bool
kk_is_image64(nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_bindless_image_load:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap:
      return intr->def.bit_size == 64;
   case nir_intrinsic_bindless_image_store:
      return intr->src[3].ssa->bit_size == 64;
   default: return false;
   }
}
static inline bool
kk_is_image_atomic64(nir_intrinsic_instr *intr)
{
   return kk_is_image64(intr) &&
      (nir_intrinsic_has_atomic_op(intr) ||
       (nir_intrinsic_access(intr) & ACCESS_ATOMIC));
}
static inline bool
kk_uses_image_atomic64(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir)
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block)
            if (instr->type == nir_instr_type_intrinsic &&
                kk_is_image_atomic64(nir_instr_as_intrinsic(instr)))
               return true;
   return false;
}
static inline bool
kk_emit_image64(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *intr)
{
   if (!kk_is_image64(intr)) return false;
   const bool store = intr->intrinsic == nir_intrinsic_bindless_image_store;
   const bool load = intr->intrinsic == nir_intrinsic_bindless_image_load;
   const bool swap = intr->intrinsic == nir_intrinsic_bindless_image_atomic_swap;
   const bool atomic = kk_is_image_atomic64(intr);
   const bool guarded = msl_interlock_side_effect(ctx, intr);
   if (guarded) { P_IND(ctx, "if (kk_rog_live) {\n"); ctx->indentlevel++; }
   if (!store) {
      if (swap) { P_IND(ctx, "%s t%d = ", msl_type_for_def(ctx->types, &intr->def), intr->def.index); }
      else { P_IND(ctx, "t%d = ", intr->def.index); }
      P(ctx, "as_type<%s>(", msl_type_for_def(ctx->types, &intr->def));
   } else { P_INDENT(ctx); }
   if (!atomic) {
      src_to_msl(ctx, &intr->src[0]);
      if (store) {
         P(ctx, ".write(as_type<ulong4>("); src_to_msl(ctx, &intr->src[3]);
         P(ctx, "), "); image_coord_swizzle(ctx, intr);
         if (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_BUF) {
            P(ctx, ", "); src_to_msl(ctx, &intr->src[4]);
         }
         P(ctx, ");\n");
      } else {
         P(ctx, ".read("); image_coord_swizzle(ctx, intr);
         if (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_BUF) {
            P(ctx, ", "); src_to_msl(ctx, &intr->src[3]);
         }
         P(ctx, ").");
         for (unsigned c = 0; c < intr->def.num_components; c++) P(ctx, "%c", "xyzw"[c]);
         P(ctx, ");\n");
      }
   } else {
      if (ctx->shader->info.stage == MESA_SHADER_FRAGMENT) {
         if (store) { P(ctx, "if (!simd_is_helper_thread()) "); }
         else { P(ctx, "simd_is_helper_thread() ? 0ul : "); }
      }
      const bool array = nir_intrinsic_image_array(intr);
      P(ctx, "kk_image_atomic64%s(", array ? "_array" : "");
      src_to_msl(ctx, &intr->src[0]); P(ctx, ", ");
      image_coord_swizzle(ctx, intr); P(ctx, ", uint2(");
      src_to_msl(ctx, &intr->src[1]);
      P(ctx, nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_1D ? ".x, 0u)" : ".xy)");
      const unsigned op = load ? 9 : store ? 8 : kk_atomic64_opcode(nir_intrinsic_atomic_op(intr));
      P(ctx, ", (device atomic_uint*)buf0.contents[0], %uu, ", op);
      if (load) { P(ctx, "0ul"); }
      else {
         P(ctx, "ulong("); src_to_msl(ctx, &intr->src[swap ? 4 : 3]);
         if (store && intr->src[3].ssa->num_components > 1) P(ctx, ".x");
         P(ctx, ")");
      }
      P(ctx, ", ");
      if (swap) { P(ctx, "ulong("); src_to_msl(ctx, &intr->src[3]); P(ctx, ")"); }
      else { P(ctx, "0ul"); }
      P(ctx, ", %s)", store || nir_def_is_unused(&intr->def) ? "true" : "false");
      if (!store) P(ctx, ")");
      P(ctx, ";\n");
   }
   if (guarded) { ctx->indentlevel--; P_IND(ctx, "}\n"); }
   return true;
}
`;

const bufferLowering = String.raw`
/* R64 texel buffers contain byte addresses, not Metal resource IDs. Keep
 * bounds and aliases in the same address-based path as buffer atomics. */
static bool
kk_lower_buffer64(nir_builder *b, nir_intrinsic_instr *intr,
                   nir_deref_instr *deref, const struct lower_descriptors_ctx *ctx)
{
   if (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_BUF) return false;
   const bool store = intr->intrinsic == nir_intrinsic_image_deref_store;
   const bool load = intr->intrinsic == nir_intrinsic_image_deref_load;
   const bool atomic = intr->intrinsic == nir_intrinsic_image_deref_atomic;
   const bool swap = intr->intrinsic == nir_intrinsic_image_deref_atomic_swap;
   if (!(store || load || atomic || swap) ||
       (store ? intr->src[3].ssa->bit_size : intr->def.bit_size) != 64) return false;
   nir_def *base = load_resource_deref_desc(b, 1, 64, deref, 0, ctx);
   nir_def *metadata = load_resource_deref_desc(b, 2, 32, deref,
      offsetof(struct kk_storage_image_descriptor, texel_offset), ctx);
   nir_def *x = nir_channel(b, intr->src[1].ssa, 0);
   nir_def *valid = nir_iand(b, nir_ine_imm(b, base, 0),
      nir_ult(b, x, nir_channel(b, metadata, 1)));
   nir_def *address = nir_iadd(b, base, nir_imul_imm(b, nir_u2u64(b, x), 8));
   enum gl_access_qualifier access = nir_intrinsic_access(intr);
   nir_def *result = NULL;
   nir_push_if(b, valid);
   if (store) {
      nir_store_global(b, nir_channel(b, intr->src[3].ssa, 0), address,
         .align_mul = 8, .write_mask = 1, .access = access);
   } else if (load) {
      nir_def *value = nir_load_global(b, 1, 64, address, .align_mul = 8, .access = access);
      nir_def *components[4] = {value, nir_imm_int64(b, 0), nir_imm_int64(b, 0), nir_imm_int64(b, 1)};
      result = nir_vec(b, components, intr->def.num_components);
   } else if (swap) {
      result = nir_global_atomic_swap(b, 64, address, intr->src[3].ssa,
         intr->src[4].ssa, .atomic_op = nir_intrinsic_atomic_op(intr), .access = access);
   } else {
      result = nir_global_atomic(b, 64, address, intr->src[3].ssa,
         .atomic_op = nir_intrinsic_atomic_op(intr), .access = access);
   }
   nir_push_else(b, NULL);
   nir_def *zero = store ? NULL : nir_imm_zero(b, intr->def.num_components, 64);
   nir_pop_if(b, NULL);
   if (store) nir_instr_remove(&intr->instr);
   else nir_def_replace(&intr->def, nir_if_phi(b, result, zero));
   return true;
}
`;

const opcodeSwitch = `      switch (nir_intrinsic_atomic_op(intr)) {
      case nir_atomic_op_iadd: op = 0; break;
      case nir_atomic_op_imin: op = 1; break;
      case nir_atomic_op_umin: op = 2; break;
      case nir_atomic_op_imax: op = 3; break;
      case nir_atomic_op_umax: op = 4; break;
      case nir_atomic_op_iand: op = 5; break;
      case nir_atomic_op_ior: op = 6; break;
      case nir_atomic_op_ixor: op = 7; break;
      case nir_atomic_op_xchg: op = 8; break;
      case nir_atomic_op_cmpxchg: op = 10; break;
      default: UNREACHABLE("Unsupported integer atomic");
      }`;
const opcodeFunction = `static inline unsigned
kk_atomic64_opcode(nir_atomic_op atomic_op)
{
   unsigned op;
${opcodeSwitch.replace('nir_intrinsic_atomic_op(intr)', 'atomic_op')}
   return op;
}
`;

export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .EXT_shader_atomic_float = true,', '      .EXT_shader_image_atomic_int64 = pdev->info.gpu_apple_family >= 8,\n      .EXT_shader_atomic_float = true,')
    .replace('      .shaderBufferInt64Atomics = true,', '      .shaderImageInt64Atomics = supported_extensions->EXT_shader_image_atomic_int64,\n      .shaderBufferInt64Atomics = true,'));
  edit('src/kosmickrisp/vulkan/kk_device.c', s => s.replace(
    '   if (dev->vk.enabled_features.shaderBufferInt64Atomics ||',
    '   if (dev->vk.enabled_features.shaderImageInt64Atomics ||\n       dev->vk.enabled_features.shaderBufferInt64Atomics ||'));
  edit('src/kosmickrisp/vulkan/kk_format.c', s => s.replace(
    '   // 64-bit formats\n', `   // Exact R64 storage uses RG32Uint backing and ulong shader elements.
   MTL_FMT(R64_UINT, R32G32_UINT, MTL_SWIZZLE_IDENTITY,
      MTL_FMT_IMAGE64, MTL_FMT_TB_NONE, true),
   MTL_FMT(R64_SINT, R32G32_UINT, MTL_SWIZZLE_IDENTITY,
      MTL_FMT_IMAGE64, MTL_FMT_TB_NONE, true),

   // 64-bit formats
`));
  edit('src/kosmickrisp/vulkan/kk_format.c', s => s.replace(
    '// Write, Color, Sparse', '#define MTL_FMT_IMAGE64 .bit_widths = 64, .write = 1u, .atomic = 1u\n\n// Write, Color, Sparse'));
  edit('src/kosmickrisp/vulkan/kk_image.c', s => s.replace(
    '   // Textures can at least be sampled', `   if (p_format == PIPE_FORMAT_R64_UINT || p_format == PIPE_FORMAT_R64_SINT) {
      /* Publication requires native ulong texture min/max. */
      if (pdev->info.gpu_apple_family < 8)
         return 0;
      return VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
             VK_FORMAT_FEATURE_2_STORAGE_IMAGE_ATOMIC_BIT |
             VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
             VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT |
             VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
             VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
   }

   // Textures can at least be sampled`));
  edit('src/kosmickrisp/vulkan/kk_image_view.c', s => s.replace(
    'bool requires_format_change = view->vk.format != image->vk.format;',
    'bool requires_format_change = view->vk.format != image->vk.format &&\n         view_layout.format.mtl != plane->layout.format.mtl;'));
  edit('src/kosmickrisp/vulkan/kk_buffer_view.c', s => s
    .replace('   if (kk_texel_buffer_native_format(p_format) != p_format)', `   if (p_format == PIPE_FORMAT_R64_UINT || p_format == PIPE_FORMAT_R64_SINT)
      return pdev->info.gpu_apple_family < 8 ? 0 :
             VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT |
             VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_ATOMIC_BIT |
             VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
             VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;

   if (kk_texel_buffer_native_format(p_format) != p_format)`)
    .replace('   enum pipe_format native_format = kk_texel_buffer_native_format(p_format);', `   if (p_format == PIPE_FORMAT_R64_UINT || p_format == PIPE_FORMAT_R64_SINT) {
      struct kk_buffer *buffer = container_of(view->vk.buffer, struct kk_buffer, vk);
      view->texel_buffer_gpu_id = vk_buffer_address(&buffer->vk, view->vk.offset);
      *pBufferView = kk_buffer_view_to_handle(view);
      return VK_SUCCESS;
   }
   enum pipe_format native_format = kk_texel_buffer_native_format(p_format);`));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s
    .replace('static bool\nlower_image_intrin(', bufferLowering + '\nstatic bool\nlower_image_intrin(')
    .replace('   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);\n\n   if (nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_BUF)',
      '   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);\n   if (kk_lower_buffer64(b, intr, deref, ctx)) return true;\n\n   if (nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_BUF)')
    .replace('.flags = msl_convert_access_flag(access));', '.flags = nir_alu_type_get_type_size(type) == 64 ? MSL_ACCESS_READ_WRITE :\n               msl_convert_access_flag(access));'));
  edit('src/kosmickrisp/compiler/kk_atomic_int64.h', s => s
    .replace('static inline bool\nkk_emit_atomic64(', opcodeFunction + imageCompiler + '\nstatic inline bool\nkk_emit_atomic64(')
    .replace(opcodeSwitch, '      op = kk_atomic64_opcode(nir_intrinsic_atomic_op(intr));'));
  edits.set('src/kosmickrisp/compiler/kk_image_atomic_int64_msl.h',
    'static const char kk_image_atomic64_msl[] = ' + JSON.stringify(imageMSL) + ';\n');
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('   case nir_type_int16:\n', '   case nir_type_int64:\n   case nir_type_uint64:\n      return "ulong";\n   case nir_type_int16:\n')
    .replace('#include "kk_atomic_int64.h"', '#include "kk_atomic_int64.h"\n#include "kk_image_atomic_int64_msl.h"')
    .replace('   if (kk_emit_atomic64(ctx, instr)) return;', '   if (kk_emit_image64(ctx, instr)) return;\n   if (kk_emit_atomic64(ctx, instr)) return;')
    .replace('   if (kk_uses_atomic64(shader)) P(&ctx, "%s", kk_atomic64_msl);',
      '   if (kk_uses_atomic64(shader) || kk_uses_image_atomic64(shader))\n      P(&ctx, "%s", kk_atomic64_msl);\n   if (kk_uses_image_atomic64(shader)) P(&ctx, "%s", kk_image_atomic64_msl);'));
}

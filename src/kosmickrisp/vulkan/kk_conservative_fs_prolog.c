/* Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT */
#include "kk_conservative_fs_link.h"
#include "kk_conservative.h"
#include "kk_shader.h"
#include "util/string_buffer.h"
#include "util/u_math.h"
#include <stddef.h>

/* This wraps compiler-owned entry-point source after GPL output composition.
 * The native callable and its ABI remain unchanged. Offsets always come from
 * the same packet declaration used by the expansion kernels. */
#define P(...) _mesa_string_buffer_printf(text, __VA_ARGS__)
#define OFFSET(member) ((unsigned)offsetof(struct kk_conservative_primitive, member))

static const char *
builtin(const struct kk_conservative_fs_link_info *fs, gl_system_value value)
{
   for (unsigned i = 0; i < fs->builtin_count; ++i)
      if (fs->builtins[i].semantic == value)
         return fs->builtins[i].argument;
   return NULL;
}

static const char *
scalar_type(nir_alu_type type)
{
   switch (type) {
   case nir_type_float16: return "half";
   case nir_type_float32: return "float";
   case nir_type_int8: return "char";
   case nir_type_int16: return "short";
   case nir_type_int32: return "int";
   case nir_type_uint8: return "uchar";
   case nir_type_uint16: return "ushort";
   case nir_type_uint32: return "uint";
   default: return NULL;
   }
}

/* Framebuffer-fetch inputs are native attachment values, not producer
 * varyings. Preserve those declarations and values through the wrapper. */
static void
native_color_inputs(struct _mesa_string_buffer *text, void *mem_ctx,
                    const struct kk_conservative_fs_link_info *fs, bool declare)
{
   char *tag = ralloc_asprintf(mem_ctx, "struct %s {", fs->stage_in_type);
   const char *cursor = strstr(fs->native_wrapper, tag);
   if (!cursor) return;
   cursor = strchr(cursor, '{')+1;
   const char *end = strstr(cursor, "};");
   if (!end) return;
   const char *next;
   while ((next = strchr(cursor, ';')) && next < end) {
      const char *color = strstr(cursor, "[[color(");
      if (color && color < next) {
         const char *attribute = strstr(cursor, " [[");
         if (!attribute || attribute > color) return;
         const char *name = attribute;
         while (name > cursor && name[-1] != ' ') --name;
         if (declare) {
            _mesa_string_buffer_append_len(text, cursor, next-cursor+1);
            P("\n");
         } else {
            P("   %s.%.*s = kk_original.%.*s;\n", fs->stage_in_argument,
              (int)(attribute-name), name, (int)(attribute-name), name);
         }
      }
      cursor = next+1;
   }
}

VkResult
kk_conservative_build_fs_prolog(void *mem_ctx,
   const struct kk_conservative_fs_link_info *fs,
   const struct poly_gs_capture_info *producer, char **out_wrapper)
{
   *out_wrapper = NULL;
   /* Invalid compiler metadata is a real compile error, never a rendering
    * fallback. All valid pipelines must supply a complete interface. */
   if (!fs || !fs->native_wrapper || !fs->stage_in_type ||
       !fs->stage_in_argument || !producer)
      return VK_ERROR_UNKNOWN;
   void *tmp = ralloc_context(NULL);
   struct _mesa_string_buffer *text = _mesa_string_buffer_create(tmp, 8192);
   const char *source = fs->native_wrapper;
   const char *entry = strstr(source, "kk_link_entrypoint(");
   const char *body = entry ? strstr(entry, ") {") : NULL;
   const char *returning = NULL;
   if (body)
      for (const char *p = body; (p = strstr(p, "return ")); p += 7)
         returning = p;
   const char *return_end = returning ? strchr(returning, ';') : NULL;
   char *parameter = ralloc_asprintf(tmp, "%s %s [[stage_in]]",
                                     fs->stage_in_type, fs->stage_in_argument);
   const char *input = entry ? strstr(entry, parameter) : NULL;
   if (!body || !input || input >= body || !return_end) {
      ralloc_free(tmp);
      return VK_ERROR_UNKNOWN;
   }

   /* Insert helpers before the entry's attributes, preserving their binding. */
   const char *entry_line = entry;
   while (entry_line > source && entry_line[-1] != '\n') --entry_line;
   const char *prefix = entry_line;
   const char *early = strstr(source, "[[early_fragment_tests]]\n");
   if (early && early < entry_line) prefix = early;
   _mesa_string_buffer_append_len(text, source, prefix - source);
   P("\nstruct KKConservativeIn { uint2 kk_packet [[user(vary_00)]] [[flat]];\n");
   native_color_inputs(text, tmp, fs, true);
   P("};\n"
     "static float3 kk_cr_weights(device const uchar *p, float2 xy, bool linear) {\n"
     "   device const packed_float4 *h = (device const packed_float4 *)(p + %u);\n"
     "   float3 x = float3(h[0].x,h[1].x,h[2].x) - xy.x*float3(h[0].w,h[1].w,h[2].w);\n"
     "   float3 y = float3(h[0].y,h[1].y,h[2].y) - xy.y*float3(h[0].w,h[1].w,h[2].w);\n"
     "   float3 v = cross(x,y);\n"
     "   if (linear) v *= float3(h[0].w,h[1].w,h[2].w);\n"
     "   return v / (v.x+v.y+v.z);\n}\n", OFFSET(positions));
   _mesa_string_buffer_append_len(text, prefix, input - prefix);
   P("KKConservativeIn kk_original [[stage_in]]");
   const char *position = builtin(fs, SYSTEM_VALUE_FRAG_COORD);
   const char *sample = builtin(fs, SYSTEM_VALUE_SAMPLE_ID);
   /* A callable core may have no sample-mask output. Native per-sample
    * coverage still implements the API mask without changing its ABI. */
   bool physical_sample = true;
   bool sample_inputs = false;
   for (unsigned i = 0; i < fs->input_count; ++i)
      sample_inputs |= fs->inputs[i].evaluation == KK_CONSERVATIVE_INPUT_SAMPLE_SMOOTH ||
                       fs->inputs[i].evaluation == KK_CONSERVATIVE_INPUT_SAMPLE_LINEAR;
   _mesa_string_buffer_append_len(text, input + strlen(parameter),
                                   body - input - strlen(parameter));
   if (!position) {
      P(", float4 kk_cr_position [[position]]");
      position = "kk_cr_position";
   }
   if (!sample && (physical_sample || sample_inputs)) {
      P(", uint kk_cr_sample [[sample_id]]");
      sample = "kk_cr_sample";
   }
   P(") {\n   device const uchar *kk_cr_p = (device const uchar *)"
     "(ulong(kk_original.kk_packet.x) | (ulong(kk_original.kk_packet.y)<<32));\n"
     "   uint kk_cr_n = *(device const uint *)(kk_cr_p+%u);\n"
     "   uint kk_cr_api = (~0u >> (32u-kk_cr_n)) & *(device const uint *)(kk_cr_p+%u);\n"
     "   uint kk_cr_mask = kk_cr_api;\n", OFFSET(samples), OFFSET(sample_mask));
   if (fs->sample_shading)
      P("   kk_cr_mask &= 1u << %s;\n", sample);
   if (physical_sample)
      P("   if (!(kk_cr_api & (1u << %s))) { discard_fragment(); return {}; }\n", sample);
   P("   if (!kk_cr_mask) { discard_fragment(); return {}; }\n"
     "   float2 kk_cr_center = floor(%s.xy)+0.5f;\n", position);
   P("   %s %s = {};\n", fs->stage_in_type, fs->stage_in_argument);
   native_color_inputs(text, tmp, fs, false);
   for (unsigned i = 0; i < fs->input_count; ++i) {
      const struct kk_conservative_fs_input *in = &fs->inputs[i];
      const char *target = fs->stage_in_argument;
      if (in->evaluation == KK_CONSERVATIVE_INPUT_CANONICAL_ADDRESS) {
         P("   %s.%s = as_type<uint2>(*(device const ulong *)(kk_cr_p+%u));\n",
           target, in->member, OFFSET(canonical));
         continue;
      }
      bool basis = in->evaluation == KK_CONSERVATIVE_INPUT_CANONICAL_SMOOTH_BASIS ||
                   in->evaluation == KK_CONSERVATIVE_INPUT_CANONICAL_LINEAR_BASIS;
      bool linear = in->evaluation == KK_CONSERVATIVE_INPUT_CENTER_LINEAR ||
                    in->evaluation == KK_CONSERVATIVE_INPUT_CENTROID_LINEAR ||
                    in->evaluation == KK_CONSERVATIVE_INPUT_SAMPLE_LINEAR ||
                    in->evaluation == KK_CONSERVATIVE_INPUT_CANONICAL_LINEAR_BASIS;
      bool at_sample = in->evaluation == KK_CONSERVATIVE_INPUT_SAMPLE_SMOOTH ||
                       in->evaluation == KK_CONSERVATIVE_INPUT_SAMPLE_LINEAR;
      P("   { float2 xy = kk_cr_center");
      if (at_sample) P(" + get_sample_position(%s) - 0.5f", sample);
      P("; float3 w = kk_cr_weights(kk_cr_p, xy, %s);\n", linear ? "true" : "false");
      if (basis) {
         const char *swizzle = in->components == 2 ? ".xy" : "";
         P("     %s.%s = w%s; }\n", target, in->member, swizzle);
         continue;
      }
      const char *type = scalar_type(in->type);
      if (!type || in->components < 1 || in->components > 4 ||
          in->component + in->components > 4 || in->location >= 64) {
         ralloc_free(tmp);
         return VK_ERROR_UNKNOWN;
      }
      unsigned packed = util_bitcount64(producer->outputs & BITFIELD64_MASK(in->location));
      P("     device const ulong *corners = (device const ulong *)(kk_cr_p+%u);\n",
        OFFSET(corners));
      P("     %s.%s = %s", target, in->member, type);
      if (in->components > 1) P("%u", in->components);
      P("(");
      for (unsigned c = 0; c < in->components; ++c) {
         if (c) P(",");
         unsigned offset = packed*16+(in->component+c)*4;
         const char *rawtype = nir_alu_type_get_base_type(in->type) == nir_type_float ?
            "float" : nir_alu_type_get_base_type(in->type) == nir_type_int ? "int" : "uint";
         if (in->evaluation == KK_CONSERVATIVE_INPUT_FLAT) {
            P("*(device const %s *)(corners[*(device const uint *)(kk_cr_p+%u)]+%u)",
              rawtype, OFFSET(provoking), offset);
         } else {
            P("dot(float3(");
            for (unsigned v = 0; v < 3; ++v)
               P("%s*(device const float *)(corners[%u]+%u)", v ? "," : "", v, offset);
            P("),w)");
         }
      }
      P("); }\n");
   }
   for (unsigned i = 0; i < fs->builtin_count; ++i) {
      const struct kk_conservative_fs_builtin *in = &fs->builtins[i];
      switch (in->semantic) {
      case SYSTEM_VALUE_SAMPLE_MASK_IN:
         P("   %s = kk_cr_mask;\n", in->argument);
         break;
      case SYSTEM_VALUE_FRONT_FACE:
         P("   %s = *(device const uint *)(kk_cr_p+%u) != 0;\n", in->argument, OFFSET(front));
         break;
      case SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD:
      case SYSTEM_VALUE_BARYCENTRIC_LINEAR_COORD:
         P("   %s = kk_cr_weights(kk_cr_p, kk_cr_center, %s);\n", in->argument,
           in->semantic == SYSTEM_VALUE_BARYCENTRIC_LINEAR_COORD ? "true" : "false");
         break;
      default: break;
      }
   }
   if (builtin(fs, SYSTEM_VALUE_FRAG_COORD)) {
      P("   { float2 xy = kk_cr_center");
      if (fs->sample_shading) P(" + get_sample_position(%s)-0.5f", sample);
      P("; device const packed_float4 *h = (device const packed_float4 *)(kk_cr_p+%u);\n"
        "     float3 w = kk_cr_weights(kk_cr_p,xy,false);\n"
        "     float den = dot(w,float3(h[0].w,h[1].w,h[2].w));\n"
        "     float z = dot(w,float3(h[0].z,h[1].z,h[2].z))/den"
        " + *(device const float *)(kk_cr_p+%u);\n"
        "     if (*(device const uint *)(kk_cr_p+%u)) {\n"
        "        float2 range = *(device const packed_float2 *)(kk_cr_p+%u);\n"
        "        z = clamp(z,min(range.x,range.y),max(range.x,range.y)); }\n"
        "     %s.zw = float2(z,1.0f/den); }\n", OFFSET(positions), OFFSET(bias),
        OFFSET(depth_clamp), OFFSET(viewport)+16, position);
   }
   _mesa_string_buffer_append_len(text, body+3, returning-body-3);
   P("auto kk_cr_out = ");
   _mesa_string_buffer_append_len(text, returning+7, return_end-returning-7);
   P(";\n");
   if (fs->sample_mask_output_member) {
      P("   kk_cr_out.%s &= kk_cr_api;\n", fs->sample_mask_output_member);
      if (fs->early_fragment_tests)
         P("   if (!(kk_cr_out.%s & (1u<<%s))) discard_fragment();\n",
           fs->sample_mask_output_member, sample);
   }
   P("   return kk_cr_out;");
   _mesa_string_buffer_append(text, return_end+1);
   *out_wrapper = ralloc_strdup(mem_ctx, text->buf);
   ralloc_free(tmp);
   return *out_wrapper ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

void
kk_conservative_require_canonical(struct kk_shader *pipeline, uint64_t locations)
{
   pipeline->info.conservative_canonical_outputs |= locations;
}
#undef OFFSET
#undef P

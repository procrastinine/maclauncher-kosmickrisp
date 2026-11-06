// SPDX-License-Identifier: MIT
import fs from 'node:fs';

// Mesa draft !45831, 83b7c659c4aebd67863bbe105c299b1b383454eb,
// by Nilton Perim Neto. Shared format decoding and bounded metadata uploads.
export const id = 'vertex-input-dynamic-state';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-vertex-fetch.h', 'kosmickrisp-patches/kosmickrisp-vertex-input.h'];

export function apply({edit, edits}) {
  const replace = (s, before, after) => {
    if (!s.includes(before)) throw Error('Missing vertex-input anchor: ' + before.slice(0, 100));
    return s.replace(before, after);
  };
  edits.set('src/kosmickrisp/vulkan/kk_vertex_fetch.h',
    fs.readFileSync(new URL('./kosmickrisp-vertex-fetch.h', import.meta.url), 'utf8'));
  edits.set('src/kosmickrisp/vulkan/kk_vertex_input.h',
    fs.readFileSync(new URL('./kosmickrisp-vertex-input.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => replace(s,
"         uint32_t attrib_clamps[KK_MAX_ATTRIBS];",
`         uint32_t attrib_clamps[KK_MAX_ATTRIBS];
         uint64_t vertex_input;`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => replace(s,
`      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK));
}

struct kk_vs_key {
   bool is_points;
   struct vk_vertex_input_state vi;
};

`,
`      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK));
}

static bool
kk_vi_is_static(const struct vk_graphics_pipeline_state *state)
{
   return state && state->vi &&
          !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_VI);
}

struct kk_vs_key {
   bool is_points;
   bool dynamic_vi;
   struct vk_vertex_input_state vi;
};

`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => replace(s,
`                   const struct vk_graphics_pipeline_state *state)
{
   memset(key, 0, sizeof(*key));
   key->is_points =
      (state->ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
   key->vi = *state->vi;
}

struct kk_fs_key {
`,
`                   const struct vk_graphics_pipeline_state *state)
{
   memset(key, 0, sizeof(*key));
   key->is_points = state->ia && state->ia->primitive_topology ==
                                    VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   key->dynamic_vi = !kk_vi_is_static(state);
   if (!key->dynamic_vi)
      key->vi = *state->vi;
}

struct kk_fs_key {
`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => replace(s,
`   /* Fold constant offset srcs for IO. */
   NIR_PASS(_, nir, nir_opt_constant_folding);

   struct kk_attribute attributes[KK_MAX_ATTRIBS] = {};
   uint64_t attribs_read = nir->info.inputs_read >> VERT_ATTRIB_GENERIC0;
   u_foreach_bit(i, state->vi->attributes_valid) {
`,
`   /* Fold constant offset srcs for IO. */
   NIR_PASS(_, nir, nir_opt_constant_folding);

   bool robustness2 =
      rs->vertex_inputs ==
      VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2;
   struct kk_attribute attributes[KK_MAX_ATTRIBS] = {};
   uint64_t attribs_read = nir->info.inputs_read >> VERT_ATTRIB_GENERIC0;
   u_foreach_bit(i, state->vi->attributes_valid) {
`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => replace(s,
`      attributes[slot].instanced =
         binding->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE;
   }
   bool robustness2 =
      rs->vertex_inputs ==
      VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2;
   NIR_PASS(_, nir, kk_nir_lower_vbo, attributes, robustness2);
}

`,
`      attributes[slot].instanced =
         binding->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE;
   }
   NIR_PASS(_, nir, kk_nir_lower_vbo, attributes, robustness2);
}

`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => replace(s,
`   if (nir->info.stage == MESA_SHADER_VERTEX) {
      nir_shader_intrinsics_pass(nir, gather_vs_inputs, nir_metadata_all,
                                 &shader->info.vs.attribs_read);
      shader->info.outputs_written = nir->info.outputs_written;
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* Some meta shaders like vk-meta-resolve will have depth_layout as NONE
`,
`   if (nir->info.stage == MESA_SHADER_VERTEX) {
      nir_shader_intrinsics_pass(nir, gather_vs_inputs, nir_metadata_all,
                                 &shader->info.vs.attribs_read);
      shader->info.vs.dynamic_vi = !kk_vi_is_static(state);
      shader->info.outputs_written = nir->info.outputs_written;
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* Some meta shaders like vk-meta-resolve will have depth_layout as NONE
`));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => replace(s,
`
         /* Data needed to start render pass and bind pipeline. */
         uint32_t attribs_read;
         uint32_t sample_count;

         /* Data needed for serialization. */
`,
`
         /* Data needed to start render pass and bind pipeline. */
         uint32_t attribs_read;
         bool dynamic_vi;
         uint32_t sample_count;

         /* Data needed for serialization. */
`));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => replace(s,
`      .EXT_swapchain_maintenance1 = true,
#endif
      .EXT_vertex_attribute_robustness = true,

      .GOOGLE_decorate_string = true,
      .GOOGLE_hlsl_functionality1 = true,
`,
`      .EXT_swapchain_maintenance1 = true,
#endif
      .EXT_vertex_attribute_robustness = true,
      .EXT_vertex_input_dynamic_state = true,

      .GOOGLE_decorate_string = true,
      .GOOGLE_hlsl_functionality1 = true,
`));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => replace(s,
`      /* VK_EXT_vertex_attribute_robustness */
      .vertexAttributeRobustness = true,

      /* EXT_ycbcr_2plane_444_formats */
      .ycbcr2plane444Formats = true,
   };
`,
`      /* VK_EXT_vertex_attribute_robustness */
      .vertexAttributeRobustness = true,

      /* VK_EXT_vertex_input_dynamic_state */
      .vertexInputDynamicState = true,

      /* EXT_ycbcr_2plane_444_formats */
      .ycbcr2plane444Formats = true,
   };
`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => replace(s,
"   u_foreach_bit(i, state->vi->attributes_valid) {",
"   u_foreach_bit(i, kk_vi_is_static(state) ? (state->vi->attributes_valid & attribs_read) : 0) {"));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => replace(s,
"   NIR_PASS(_, nir, kk_nir_lower_vbo, attributes, robustness2);",
`   NIR_PASS(_, nir, kk_nir_lower_vbo,
            kk_vi_is_static(state) ? attributes : NULL, robustness2);`));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_vbo.h', s => replace(s,
"bool kk_vbo_supports_format(enum pipe_format format);",
`bool kk_vbo_supports_format(enum pipe_format format);

enum kk_vbo_type {
   KK_VBO_TYPE_UNORM = 0,
   KK_VBO_TYPE_SNORM = 1,
   KK_VBO_TYPE_USCALED = 2,
   KK_VBO_TYPE_SSCALED = 3,
   KK_VBO_TYPE_UINT = 4,
   KK_VBO_TYPE_SINT = 5,
   KK_VBO_TYPE_SFLOAT = 6,
};

#define KK_VBO_FMT_SIZE_SHIFT    0
#define KK_VBO_FMT_SIZE_MASK     0x3
#define KK_VBO_FMT_COUNT_SHIFT   2
#define KK_VBO_FMT_COUNT_MASK    0x3
#define KK_VBO_FMT_TYPE_SHIFT    4
#define KK_VBO_FMT_TYPE_MASK     0x7
#define KK_VBO_FMT_RGB10A2       (1u << 7)
#define KK_VBO_FMT_RG11B10F      (1u << 8)
#define KK_VBO_FMT_INSTANCED     (1u << 9)
#define KK_VBO_FMT_VALID         (1u << 10)
#define KK_VBO_FMT_SWIZZLE_SHIFT 11
#define KK_VBO_FMT_SWIZZLE_MASK  0x7

uint32_t kk_vbo_encode_format(enum pipe_format format);

struct kk_vertex_fetch {
   uint32_t format;
   uint32_t divisor;
   uint32_t stride;
};

`));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_vbo.c', s => {
    const start = s.indexOf('static nir_def *\napply_swizzle_channel');
    if (start < 0) throw Error('Missing original vertex-fetch decoder');
    return s.slice(0, start) + '#include "kk_vertex_fetch.h"\n';
  });
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => replace(s,
`static void
kk_flush_dynamic_state(struct kk_cmd_buffer *cmd)`,
`#include "kk_vertex_input.h"

static void
kk_flush_dynamic_state(struct kk_cmd_buffer *cmd)`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => replace(s,
`   }

   if (IS_DIRTY(VI) || IS_DIRTY(VI_BINDINGS_VALID) ||
       IS_DIRTY(VI_BINDING_STRIDES) || gfx->dirty & KK_DIRTY_VB) {
      struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
      unsigned slot = 0;
      u_foreach_bit(i, vs->info.vs.attribs_read) {
         if (dyn->vi->attributes_valid & BITFIELD_BIT(i)) {
            struct vk_vertex_attribute_state attr = dyn->vi->attributes[i];
            struct kk_addr_range vb = gfx->vb.addr_range[attr.binding];

            desc->root.draw.attrib_clamps[slot] = kk_calculate_vbo_clamp(
               vb.addr, 0, vk_format_to_pipe_format(attr.format), vb.range,
               dyn->vi_binding_strides[attr.binding], attr.offset,
               &desc->root.draw.attrib_base[slot]);
            desc->root.draw.buffer_strides[attr.binding] =
               dyn->vi_binding_strides[attr.binding];
         }
         slot++;
      }
`,
`   }

   if (IS_DIRTY(VI) || IS_DIRTY(VI_BINDINGS_VALID) ||
       IS_DIRTY(VI_BINDING_STRIDES) || gfx->dirty & KK_DIRTY_VB ||
       IS_SHADER_DIRTY(VERTEX)) {
      if (!kk_flush_vertex_input(cmd))
         return;
`));
}

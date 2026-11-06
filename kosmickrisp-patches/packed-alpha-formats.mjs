// SPDX-License-Identifier: MIT
// Alpha-only attachments retain all logical color components through blending;
// the native A8 target selects alpha at the Metal boundary.
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_format.c', s => s
    .replace('MTL_FMT_NATIVE(A8_UNORM, MTL_FMT_FS(8), MTL_FMT_TB_NONE)',
             'MTL_FMT_NATIVE(A8_UNORM, MTL_FMT_ALL_NO_ATOMIC(8), MTL_FMT_TB_WR)')
    .replaceAll('MTL_SWIZZLE_BGRA, MTL_FMT_FS(16), MTL_FMT_TB_NONE, false)',
                'MTL_SWIZZLE_BGRA, MTL_FMT_FCBMRS(16), MTL_FMT_TB_NONE, false)')
    .replaceAll('MTL_FMT_FCBMRS(16)', 'MTL_FMT_ALL_NO_ATOMIC(16)')
    .replaceAll(/(MTL_FMT_ALL_NO_ATOMIC\(16\),\s*)MTL_FMT_TB_NONE/g, '$1MTL_FMT_TB_WR'));
  edit('src/kosmickrisp/bridge/mtl_texture.m', s => s.replace(
    '      MTLTextureType type = mtl_texture_view_type(layout->view_type, layout->sample_count_sa);',
    '      MTLTextureType type = tex.textureType == MTLTextureTypeTextureBuffer ?\n' +
    '         MTLTextureTypeTextureBuffer :\n' +
    '         mtl_texture_view_type(layout->view_type, layout->sample_count_sa);'));
  edit('src/kosmickrisp/bridge/mtl_buffer.m', s => s
    .replace('#include "mtl_buffer.h"', '#include "mtl_buffer.h"\n#include "mtl_texture.h"')
    .replace('static MTLTextureDescriptor *\nmtl_new_texture_descriptor',
      `static bool
mtl_buffer_needs_swizzle(const struct kk_image_layout *layout)
{
   return layout->type == MTL_TEXTURE_TYPE_TEXTURE_BUFFER &&
          (layout->swizzle.red != PIPE_SWIZZLE_X ||
           layout->swizzle.green != PIPE_SWIZZLE_Y ||
           layout->swizzle.blue != PIPE_SWIZZLE_Z ||
           layout->swizzle.alpha != PIPE_SWIZZLE_W);
}

static MTLTextureDescriptor *
mtl_new_texture_descriptor`)
    .replace('      descriptor.usage = (MTLTextureUsage)layout->usage;',
      `      descriptor.usage = (MTLTextureUsage)layout->usage;
      if (mtl_buffer_needs_swizzle(layout))
         descriptor.usage |= MTLTextureUsagePixelFormatView;`)
    .replace('      return texture;\n',
      `      if (texture && mtl_buffer_needs_swizzle(layout)) {
         struct kk_view_layout view = {
            .sample_count_sa = 1,
            .format = {.pipe = layout->format.pipe, .mtl = layout->format.mtl},
            .num_levels = 1,
            .array_len = 1,
            .swizzle = {.red = layout->swizzle.red, .green = layout->swizzle.green,
                        .blue = layout->swizzle.blue, .alpha = layout->swizzle.alpha},
         };
         mtl_texture *swizzled = mtl_new_texture_view_with(texture, &view);
         [texture release];
         return swizzled;
      }
      return texture;
`));
  edit('src/kosmickrisp/vulkan/kk_image.c', s => s.replace(
    `   /* We disable A8 format due to lower blend pass issues */
   if (va_format->color && tiling != VK_IMAGE_TILING_LINEAR &&
       vk_format != VK_FORMAT_A8_UNORM) {`,
    '   if (va_format->color && tiling != VK_IMAGE_TILING_LINEAR) {'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    `         vk_format_to_pipe_format(state->rp->color_attachment_formats[i]);
      rt->colormask = att->write_mask;`,
    `         vk_format_to_pipe_format(state->rp->color_attachment_formats[i]);
      /* Blend equations operate on logical RGBA. Trimming an A8 target to
       * one component would retain red and discard alpha. */
      if (rt->format == PIPE_FORMAT_A8_UNORM)
         rt->format = PIPE_FORMAT_R8G8B8A8_UNORM;
      rt->colormask = att->write_mask;`).replace(
    '               util_format_get_nr_components(format);',
    '               format == PIPE_FORMAT_A8_UNORM ? 4 :\n' +
    '               util_format_get_nr_components(format);'));
}

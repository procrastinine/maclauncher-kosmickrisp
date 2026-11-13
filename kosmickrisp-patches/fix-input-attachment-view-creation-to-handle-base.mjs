// SPDX-License-Identifier: MIT
// Fix input attachment view creation to handle base array/mip offsets
export const id = "fix-input-attachment-view-creation-to-handle-base";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_image_view.c", source => source
    .replace("          */\n         if (view->vk.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) {\n            VkImageViewType original_type = view_layout.view_type;\n            if (requires_type_change || requires_format_change) {\n               view_layout.view_type = requires_type_change\n                                          ? VK_IMAGE_VIEW_TYPE_2D_ARRAY\n                                          : original_type;\n",
      () => "          */\n         if (view->vk.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) {\n            VkImageViewType original_type = view_layout.view_type;\n            bool requires_subresource =\n               view_layout.base_array_layer != 0 || view_layout.base_level != 0;\n            if (requires_type_change || requires_format_change ||\n                requires_subresource) {\n               view_layout.view_type = requires_type_change\n                                          ? VK_IMAGE_VIEW_TYPE_2D_ARRAY\n                                          : original_type;\n"));
}

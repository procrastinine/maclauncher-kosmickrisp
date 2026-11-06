/* SPDX-License-Identifier: MIT */
static void
kk_attachment_device_features(const struct kk_physical_device *pdev,
                              struct vk_device_extension_table *extensions,
                              struct vk_features *features)
{
   /* Metal 4 Apple GPUs provide programmable-blending color inputs. There
    * is no corresponding native depth/stencil attachment input. */
   if (pdev->info.gpu_apple_family >= 7) {
      extensions->EXT_rasterization_order_attachment_access = true;
      features->rasterizationOrderColorAttachmentAccess = true;
   }
}

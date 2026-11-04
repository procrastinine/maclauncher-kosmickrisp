# Image views

MacLauncher KosmicKrisp supports `VK_EXT_image_view_min_lod` and
`VK_EXT_image_sliced_view_of_3d`.

When `minLod` is enabled, an image view can supply a fractional minimum mip
level through `VkImageViewMinLodCreateInfoEXT`. Shader lowering combines the
view clamp with explicit LOD or minimum LOD operands. Integer texel fetches
below the integer part of the view clamp return zero. The existing Metal texture and
sampler remain shared; the clamp travels in the sampled-image descriptor.

When `imageSlicedViewOf3D` is enabled, `VkImageViewSlicedCreateInfoEXT` selects
a range of depth slices within a single mip level of a 3D view. Storage-image
loads, stores and atomics translate their Z coordinate relative to
`sliceOffset`, and storage-image size queries report `sliceCount` as the depth.
`VK_REMAINING_3D_SLICES_EXT` selects the remaining slices at that mip level.
Sampled-image descriptors retain the full depth of the view's mip level.

The storage range shares the existing texel-buffer descriptor metadata words;
storage descriptors remain 16 bytes. Ordinary and sliced storage views use
the same coordinate lowering and original Metal 3D texture, without an additional image allocation,
copy or synchronization operation.

The Vulkan specification defines the [minimum LOD structure](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewMinLodCreateInfoEXT.html)
and [sliced-view structure](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewSlicedCreateInfoEXT.html).

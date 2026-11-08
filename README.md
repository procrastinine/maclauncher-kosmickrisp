# MacLauncher KosmicKrisp

MacLauncher KosmicKrisp is the Vulkan driver that [MacLauncher](https://github.com/procrastinine/maclauncher) uses on Apple Silicon Macs. It is built from [KosmicKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html), the Vulkan driver in [Mesa](https://mesa3d.org/) that LunarG develops on Metal 4, and adds the Vulkan features that [DXVK](https://github.com/doitsujin/dxvk), [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) and [ANGLE](https://chromium.googlesource.com/angle/angle) use. Upstream KosmicKrisp lacks some of them, so DXVK 2.x and 3.x and vkd3d-proton cannot create a device on it. Examples are transform feedback for Direct3D stream output, pixel interlock for rasterizer-ordered views, sparse resources for tiled resources and conservative rasterization.

MacLauncher uses this driver for Windows games in CrossOver and Sikarugir through DXVK and vkd3d-proton, for Android games through ANGLE's Vulkan renderer, and for AnyPS5 and Prosper. The [MoltenVK](https://github.com/KhronosGroup/MoltenVK) README states that Khronos has deprecated MoltenVK and recommends KosmicKrisp for Vulkan on Apple Silicon Macs, while MoltenVK remains the recommendation for Intel Macs and iOS.

## Requirements

The driver is built on Metal 4, so it requires macOS 26 or later on Apple Silicon. Metal 4 is available from Apple GPU family Apple7 (M1) onward, so every Mac that runs the driver is Apple7 or later. It does not run on Intel Macs or in virtual machines. The x86_64 build runs under Rosetta on an Apple Silicon Mac, for x86_64 Wine, AnyPS5 and Prosper.

## Conformance

Khronos has [announced](https://www.khronos.org/news/permalink/kosmickrisp-achieves-vulkan-1.4-conformance-on-apple-silicon) that upstream KosmicKrisp passed the Vulkan 1.4 conformance tests and is a conformant Vulkan 1.4 implementation on Apple Silicon. Its entry in the Khronos list of conformant products, submission 997, covers the A18 Pro and the M1 to M5 families on macOS 27.

This build adds features to that driver and has no conformance submission of its own, so it is not a Khronos conformant product. Its additions are tested with the Vulkan conformance test suite (CTS). For example, in the device-generated commands group of CTS 1.4.7.1, run with the x86_64 build on an M2 Max, every case that the build supports passes: 431 pass and none fail.

## Direct3D feature levels

DXVK and vkd3d-proton derive a game's highest Direct3D feature level from the driver's features:

| Feature level | Requirement |
| --- | --- |
| 12_0 | Tiled resources tier 2, which rests on Vulkan sparse residency, and typed UAV loads of additional formats; vkd3d-proton also needs resource binding tier 2 |
| 12_1 | Rasterizer-ordered views, through pixel interlock, and conservative rasterization tier 1 |
| 12_2 | vkd3d-proton only: shader model 6.5, ray tracing tier 1.1, variable shading rate tier 2, mesh shader tier 1, sampler feedback, tiled resources tier 3, conservative rasterization tier 3, resource binding tier 3 and further options |

With this driver, vkd3d-proton reports feature level 12_1 for Direct3D 12 and DXVK reports a maximum of 12_1 for Direct3D 11. Feature level 12_0 needs sparse residency, which the driver exposes where Metal supports placement sparse resources. Feature level 12_2 also needs variable shading rate tier 2, which vkd3d-proton implements through `VK_KHR_fragment_shading_rate`. The driver does not expose that extension, so vkd3d-proton reports at most 12_1 in both modes described below, including when ray tracing is selected. DXVK offers no level above 12_1.

## Performance and conformance modes

The driver emulates Vulkan capabilities that Metal does not provide directly, using shader lowering, compute passes or captured geometry. Each emulation is a named entry, and the `KK_EMULATION` environment variable selects which entries the device reports. It is read once, when the Vulkan physical device is created.

- `performance`, the default: an entry is off only when turning it off lowers no Direct3D shader model or feature level that DXVK or vkd3d-proton reports and does not stop either translator from creating its device. Ray tracing and fragment barycentrics are also off.
- `conformance`: every entry is on, including ray tracing.

For example, vkd3d-proton reports shader model 6.2 and later only when both 32-bit denormal controls are reported, so `denorm_preserve` and `denorm_flush` stay on in performance mode. Of the 35 entries, 23 are on in performance mode.

The value is a comma-separated list of tokens, applied from left to right:

| Token | Effect |
| --- | --- |
| `performance` or `conformance` | As the first token, selects the mode. Without either, performance mode applies. |
| `apple7` to `apple11`, or `mode=7` to `mode=11` | Chip preset: enables every entry that the named family runs natively. |
| `+name` | Enables the entry `name`. |
| `-name` | Disables the entry `name`, unless the running GPU provides it natively. |
| `-VK_extension_name` | Hides that device extension. |
| Any other token | Prints a warning and is ignored. |

An entry can name the first Apple GPU family on which Metal provides its behavior natively. On that family and later, the entry takes the native path and stays reported in every mode. In this version only `sampler_filter_minmax` declares one, Apple10, and performance mode already enables it. `apple10` and `apple11` therefore re-enable it after `-sampler_filter_minmax`, for example `performance,-sampler_filter_minmax,apple10`, while `apple7` to `apple9` enable nothing. A preset never adds capabilities that only the hardware provides, such as 64-bit image atomics.

More examples:

- `conformance,-ray_tracing` enables every entry except ray tracing.
- `performance,+float64` adds software 64-bit floating point to the defaults.
- `conformance,-VK_KHR_ray_query` enables every entry and hides `VK_KHR_ray_query`.

## Apple GPU families

Apple's [Metal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf) of October 23, 2026 assign the Mac chips to these families:

| Family | Mac chips |
| --- | --- |
| Apple7 | M1 series |
| Apple8 | M2 series |
| Apple9 | M3 and M4 series |
| Apple10 | M5 series |
| Apple11 | M6 series |

The driver reads the family once, when the Vulkan device is created, and takes a native Metal path where the family provides one:

- Apple8 (M2) and later report 64-bit integer image atomics (`VK_EXT_shader_image_atomic_int64`). Apple7 does not.
- Apple9 (M3) and later report task and mesh shader stages for device-generated commands, which use Metal indirect command buffers with mesh draws.
- Apple10 (M5) and later use Metal directly for sampler LOD bias and minimum and maximum sampler filtering, which earlier families compute in the shader. They also report depth bounds testing, and 1D, 2D and cube images reach 32,768 pixels instead of 16,384.

## Features

Compared with upstream KosmicKrisp at the same Mesa commit, this build adds the following. Features marked \* are reported in performance mode only when `KK_EMULATION` selects them.

**Rasterization and geometry**
- Transform feedback with geometry streams, and primitives-generated queries
- Geometry, tessellation and mesh shaders, and multiview together with geometry and tessellation shaders\*
- Fragment shader interlock, pixel and sample, for rasterizer-ordered views
- Conservative rasterization with overestimation
- Wireframe and point fill, and wide, smooth and stippled lines
- Depth bias control\*, cull distances and pipeline statistics queries

**Resources and memory**
- Sparse binding and sparse residency for buffers and 2D and 3D images, on Metal devices with placement sparse resources
- Descriptor heaps\*, device-generated commands and device address commands
- Multisampled storage images\*, and multisampled rendering to single-sampled images\*
- Dynamic rendering with unused attachments, sliced views of 3D images and non-seamless cube maps\*
- Image compression control, image alignment control and custom border colors
- Minimum and maximum sampler filtering on every Apple GPU
- External memory through file descriptors

**Shaders**
- 64-bit floating point\*, computed in software with IEEE binary64 rounding
- 64-bit integer atomics on buffers and shared memory, and on images on Apple8 and later
- Independent float controls for each bit width
- 8-bit floating point and cooperative matrices
- Compute shader derivatives, fragment barycentrics\* and quad control
- Subgroup operations in vertex, tessellation and geometry shaders

**Ray tracing**\*
- Acceleration structures, ray queries and ray tracing pipelines, built and traversed in compute passes without Metal ray tracing

**Presentation**
- Headless surfaces, incremental present and presentation timing

## Builds

MacLauncher Builds packages the driver for arm64 and for x86_64, each with a Khronos Vulkan loader built from source. The MacLauncher app downloads the package when a game first needs it.

## Licence

The adaptations are released under the MIT licence in [LICENSE](LICENSE). Mesa's licensing is described in [MESA-LICENSE.rst](MESA-LICENSE.rst). The software 64-bit floating-point code carries its own notice in [kosmickrisp-patches/kosmickrisp-soft-fp64-LICENSE.txt](kosmickrisp-patches/kosmickrisp-soft-fp64-LICENSE.txt).

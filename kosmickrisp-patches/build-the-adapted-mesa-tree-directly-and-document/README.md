# MacLauncher KosmicKrisp

MacLauncher KosmicKrisp is the Vulkan driver that [MacLauncher](https://github.com/procrastinine/maclauncher) uses on macOS. It is built from [KosmicKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html), the Vulkan driver in [Mesa](https://mesa3d.org/) that runs on Metal 4. Upstream KosmicKrisp is Khronos-conformant for Vulkan 1.4 on Apple Silicon. Khronos has deprecated MoltenVK, which implements only a non-conformant portability subset, and recommends KosmicKrisp on Apple Silicon Macs.

MacLauncher uses this driver by default for Windows games through [DXVK](https://github.com/doitsujin/dxvk) and [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton), for Android games, and for AnyPS5. Those translators depend on Vulkan features that upstream KosmicKrisp does not expose yet, so this build adds them on Metal. Each addition targets something a real consumer needs. Examples are transform feedback for Direct3D stream output, interlock for rasterizer-ordered views, sparse resources for tiled resources, and conservative rasterization for Direct3D 12 renderers. The build is based on the conformant upstream driver. It has no conformance submission of its own.

## Features

Compared with upstream KosmicKrisp at the same Mesa commit, this build adds the following.

**Rasterization and geometry**
- Transform feedback with geometry streams, and primitives-generated queries
- Geometry, tessellation and mesh shaders, including with multiview
- Fragment shader interlock, pixel and sample, for rasterizer-ordered views
- Fragment shading rate (variable rate shading)
- Conservative rasterization
- Wireframe and point fill, and wide, smooth and stippled lines
- Depth bias control, cull distances and pipeline statistics queries

**Resources and memory**
- Sparse binding and sparse residency for buffers and 2D and 3D images, on Metal devices with placement sparse resources
- Descriptor heaps, device-generated commands and device address commands
- Multisampled storage images, and multisampled rendering to single-sampled images
- Dynamic rendering with unused attachments, sliced views of 3D images and non-seamless cube maps
- Image compression control, image alignment control and custom border colors
- Minimum and maximum sampler filtering on every Apple GPU
- External memory through file descriptors

**Shaders**
- 64-bit floating point, computed in software with IEEE binary64 rounding
- 64-bit integer atomics on buffers and shared memory, and on images on Apple GPU family 8 and later
- Independent float controls for each bit width
- 8-bit floating point and cooperative matrices
- Compute shader derivatives, fragment barycentrics and quad control
- Subgroup operations in vertex, tessellation and geometry shaders

**Presentation**
- Headless surfaces, incremental present and presentation timing

## Builds

MacLauncher Builds packages the driver for Apple Silicon (arm64) and for Intel code (x86_64), which serves Windows games running in Wine under Rosetta. The MacLauncher app downloads the package when a game first needs it.

## Licence

The adaptations are released under the MIT licence in [LICENSE](LICENSE). Mesa's licensing is described in [MESA-LICENSE.rst](MESA-LICENSE.rst). The software 64-bit floating-point code carries its own notice in [maclauncher/SOFT-FP64-LICENSE.txt](maclauncher/SOFT-FP64-LICENSE.txt).

## Building

[Build configurations](maclauncher/BUILDING.md) describe the dependencies and both CPUs. [Adaptations](maclauncher/ADAPTATIONS.md) describes the Vulkan implementation.

```sh
meson setup build/arm64 --native-file maclauncher/arm64.ini
meson compile -C build/arm64 src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib
meson setup build/x86_64 --cross-file maclauncher/x86_64.ini
meson compile -C build/x86_64 src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib
```

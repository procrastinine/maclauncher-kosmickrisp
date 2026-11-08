# Building

The source is Mesa with the adaptations applied as commits. Building requires macOS 26, Xcode command line tools with Metal 4, Meson, Ninja, ccache, Python with Mako and PyYAML, pkg-config, glslang, SPIRV-Tools, zstd and expat. The native shader compilers also require LLVM with SPIR-V support and SPIRV-LLVM-Translator. LLVM tools must be on PATH, with LLVM_CONFIG selecting that installation. PKG_CONFIG_PATH selects the dependency installations. No JavaScript runtime is involved.

## Apple Silicon

`arm64.ini` selects a release build, macOS 26, the KosmicKrisp Vulkan driver and native shader compiler tools. The Objective-C compiler is `ccache clang`, resolved from PATH alongside LLVM.

```sh
export MESA_GIT_SHA1_OVERRIDE=4f071f891d63396c799b267172c19ba6b0301682
meson setup build/arm64 --native-file maclauncher/arm64.ini
meson compile -C build/arm64 src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib
```

The upstream revision override preserves the driver version string used by existing packages. The fork commit remains the source identity recorded by packaging.

## Intel code on Apple Silicon

`x86_64.ini` selects the x86_64 slice with Apple clang. Native shader compilers execute on the Apple Silicon build host and serialize NIR from this same source tree. The target build uses those tools without linking target LLVM. SPIRV-Tools, zstd and expat must have x86_64 slices. Their pkg-config directories replace the host directories for target dependency discovery.

```sh
export PATH="$PWD/build/arm64/src/compiler/clc:$PWD/build/arm64/src/compiler/spirv:$PWD/build/arm64/src/kosmickrisp/clc:$PATH"
export PKG_CONFIG_PATH="$TARGET_PKG_CONFIG_PATH"
export PKG_CONFIG_LIBDIR="$TARGET_PKG_CONFIG_PATH"
meson setup build/x86_64 --cross-file maclauncher/x86_64.ini
meson compile -C build/x86_64 src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib
```

`TARGET_PKG_CONFIG_PATH` is the list of target dependency pkg-config directories. The dylib is in `src/kosmickrisp/vulkan/` within each build directory. The complete option lists are in the two configuration files. The loader is built and packaged separately by MacLauncher Builds.

## Generated code

Meson generates `kk_soft_fp64_spv.h` from `src/compiler/glsl/float64.glsl` through Mesa's `glsl2spirv.py` and glslang with `-Olib`. The header is a build output. Native tools and target builds must use the same adapted NIR definitions. MacLauncher Builds verifies the generated headers, serializer inputs and compiler executable hashes before reusing native tools.

## Tests

Mesa unit tests use a separate native configuration. The production configurations disable test targets.

```sh
meson setup build/tests --native-file maclauncher/arm64.ini -Dbuild-tests=true
meson test -C build/tests --print-errorlogs
```

Vulkan conformance uses VK-GL-CTS and deqp-runner. Mesa selects deqp-runner 0.24.0 in its CI dependency configuration. The Vulkan CTS binary must match the test case list. The selected driver is supplied through a Vulkan ICD manifest. Expected failures, flakes and skips are maintained in `src/kosmickrisp/ci/`.

`ICD` names an absolute manifest path, `DEQP_VK` names the CTS executable, `CASELIST` names the selected case list, and `VULKAN_LOADER` names the packaged `libvulkan.1.dylib`. The CTS executable, loader and driver must have matching CPU slices. The explicit CTS loader argument avoids relying on a system Vulkan loader. One worker serializes the selected tests.

```sh
VK_DRIVER_FILES="$ICD" deqp-runner run \
  --deqp "$DEQP_VK" --caselist "$CASELIST" --output results --jobs 1 \
  --baseline src/kosmickrisp/ci/kosmickrisp-fails.txt \
  --flakes src/kosmickrisp/ci/kosmickrisp-flakes.txt \
  --skips src/kosmickrisp/ci/kosmickrisp-skips.txt \
  -- --deqp-vk-library-path="$VULKAN_LOADER"
```

The skip list excludes cases with recorded GPU hangs. Unsupported features remain CTS NotSupported results. A failure expectation records an incorrect result; it does not establish conformance.

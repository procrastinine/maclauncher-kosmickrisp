# Vulkan loader

The KosmicKrisp producer builds the pinned Khronos Vulkan-Loader with the declared Vulkan-Headers source. The runtime producer and native build command use the same buildKosmicLoader function in kosmickrisp-targets.mjs.

Each target has a separate CMake configuration. CMAKE_SYSTEM_NAME is Darwin, and CMAKE_SYSTEM_PROCESSOR and CMAKE_OSX_ARCHITECTURES identify the same CPU: arm64 or x86_64. This selects the target assembly used for unknown extension command dispatch, including commands newer than the loader's generated entry-point table.

A universal loader is assembled with lipo from one arm64 build and one x86_64 build. Headers are installed once. Single-CPU builds compile only their requested target; universal builds compile each target once. Mesa, shader compilers and other runtime dependencies are outside this loader build.


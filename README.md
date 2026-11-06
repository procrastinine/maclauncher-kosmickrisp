# MacLauncher KosmicKrisp

Anchor-checked Mesa adaptations and macOS build recipes for MacLauncher KosmicKrisp. Mesa is pinned by the sources/mesa gitlink. The ordered patch registry and patches.json describe the adaptations. Existing component notices accompany their sources; Mesa licensing is recorded in MESA-LICENSE.rst.

The buildKosmic function in kosmickrisp-build.mjs accepts a pinned Mesa checkout, an output work directory, Meson, Ninja, LLVM and dependency pkg-config paths. It produces the driver and native compiler metadata. buildKosmicLoader accepts pinned Vulkan-Loader and Vulkan-Headers checkouts with their revisions. MacLauncher Builds supplies those dependencies and packages the resulting runtime. Node.js 22 or newer, Xcode command-line tools and macOS 26 are required.

The command node generate-mesa-branch.mjs <mesa-checkout> generates mesa-with-adaptations in this repository from the Mesa gitlink and ordered catalog modules. This branch is generated and read-only: changes belong in the adaptation modules, followed by regeneration. Each adaptation becomes an ordinary commit over the pinned Mesa history. The generator uses local Git objects and never fetches a remote.

The command node --test tests/*.test.mjs runs the source tests.

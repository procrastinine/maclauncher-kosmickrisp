KosmicKrisp
###########

KosmicKrisp is a Vulkan conformant implementation for macOS on Apple Silicon
hardware. It is implemented on top of Metal 4, which requires macOS 26 and up.

No iOS support is present as of now. However, iOS was taken into consideration
during development to support A14 Bionic GPUs and upwards.

Building
********

The following build instructions assume Homebrew as the package manager to
install dependencies. Homebrew homepage https://brew.sh/
Homebrew install command line:

.. code-block:: sh

   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

Terminal restart is recommended after Homebrew installation.

Requirements
============

- Xcode and Xcode command line tools
- Homebrew packages
   - meson (1.9.1+, can also be installed as a Python package)
   - cmake
   - pkg-config
   - libclc
   - llvm (20.1.8+, lower versions may work but have not been tested)
   - spirv-llvm-translator
   - spirv-tools (static library preferred which is not included in the package, Vulkan macOS SDK does)

Users might already have llvm installed, run ``llvm-config --version`` and
check that the command works and prints at least ``20.1.8``. Otherwise install
llvm from homebrew and add it to the ``PATH`` (follow homebrew instructions).

- Python
- Python packages
   - mako
   - packaging
   - pyyaml
   - meson (1.9.1+, if not installed through Homebrew)

Since Homebrew manages the Python environment, it is encouraged to create a
Python virtual environment and install all packages in that environment. To
create a Python virtual environment (e.g. ``$HOME/venv_mesa``):

.. code-block:: sh

   python3 -m venv $HOME/venv_mesa

To enable a Python virtual environment:

.. code-block:: sh

   source $HOME/venv_mesa/bin/activate

Build instructions
==================

Out of tree build directory is recommended.

Once all requirements have been installed, the following command line can be
used to create a debug build:

.. code-block:: sh

   meson setup <path/to/mesa> --buildtype=debug -Dplatforms=macos -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static

Environment variables
*********************

KosmicKrisp specific environment variables:


- ``MESA_KK_DEBUG``: ``msl`` to log all generated Metal Shading Language (MSL) shaders. ``force_robustness`` to force robustness on all shaders.
- ``MESA_KK_GPU_CAPTURE``: Starts Metal capture at device create and ends it at device destroy. Set to ``1`` to activate.
- ``MESA_KK_GPU_CAPTURE_DIRECTORY``: Metal capture will be saved to the specified directory. Defaults to Xcode if no path is provided.
- ``MESA_KK_DISABLE_WORKAROUNDS``: Provide ``all`` to disable all workarounds. Otherwise, provide a comma separated list to disable wanted workarounds e.g. ``1,3,4`` to disable workaround 1, 3 and 4.
- ``MESA_KK_EXPERIMENTAL``: Enables advertising experimental features which may not provide full correctness or may negatively impact performance when enabled. Provide ``all`` to advertise all listed features.
  - ``custom_border`` to advertise ``VK_EXT_custom_border_color`` and ``VK_EXT_border_color_swizzle``.
  - ``image_view_min_lod`` to advertise ``VK_EXT_image_view_min_lod``.
  - ``optimize_e5r9g9b9`` to use allowGPUOptimizedContents with this format, which causes some CTS tests for vkCmdCopyImage() to fail

Slow but correct features
*************************

``KK_EMULATION`` controls software-backed capability advertisement. It is read
once at physical-device creation. The default is ``performance``, which hides
all registry entries. ``conformance`` retains all implemented capabilities.
An entry is enabled with ``+name`` and disabled with ``-name``; the last override
for an entry wins. For example, ``performance,+denorm_preserve`` enables only
preservation, and ``conformance,-ray_tracing`` excludes ray tracing. Enabling an
entry does not add unsupported hardware modes; FTZ16 and FTZ64 remain false.

``-VK_extension_name`` removes an extension and its associated feature and
property fields, including promoted query aliases. For example,
``conformance,-VK_KHR_ray_query`` removes ray-query advertisement. These removals
apply after named entry selection. Unknown tokens produce one stderr line per
token and are ignored. The setting changes capability reports; rendering and
shader compilation do not parse it. Ray tracing requires explicit selection.

.. include:: kosmickrisp/emulation.rst

Compile timing
**************

Built-in compute kernels compile when an operation first needs them. Each
kernel shares one completed pipeline within the Vulkan device. Native compile
failures propagate to command recording; a failed kernel is not dispatched.

``KK_PERF_TRACKING=1`` enables interval timing records. The output is newline
separated JSON on stderr, or in the file selected by ``KK_PERF_TRACKING_LOG``.
``KK_PERF_TRACKING_INTERVAL_MS`` and ``KK_PERF_TRACKING_INTERVAL_FRAMES``
select the reporting intervals; their defaults are 1000 ms and 60 frames.

With tracking enabled, ``KK_PERF_TRACKING_GPU_ENCODERS=1`` adds GPU encoder
boundary timestamps using Metal 4 counter heaps. Synchronous CPU resolution
runs in the existing submission feedback path after GPU completion. Render,
compute, virtual Mesh, internal meta and blit work is identified; opaque
MetalFX and other temporal scene callbacks are reported as named groups.
No timestamp heap or samples are created when the option is unset. Ordinary
draw and pipeline-bind calls retain their native encoder receiver.

Each interval reports the twenty largest ``gpu_encoders`` groups by total
GPU milliseconds, with source hashes, draw/dispatch/blit counts, target extent,
and sample count, total, mean and maximum milliseconds. ``gpu_pipelines``
accumulates inclusive encoder time for each source hash across passes and
reports the twenty largest totals. A mixed encoder's duration belongs to its
whole workload; membership totals are not independently measured shader times
and must not be summed across hashes. Encoder spans can overlap on the GPU.
Missing/failed samples, bounded-table overflow, truncated hash sets and unknown
pipeline/extent metadata are reported explicitly. Concurrent reuse of a sampled
command buffer makes its overlapping samples unavailable rather than assigning
ambiguous timestamps to a submission. Tracking retains up to 4,096 encoder
records per command buffer and 1,024 distinct interval aggregation keys.

``MESA_KK_DEBUG=msl`` identifies generated and final native-library inputs with
``KK_MSL`` headers containing their BLAKE3 hash and source byte length. The same
hashes appear in GPU records, including generated sibling and internal meta
sources. MetalFX's private shader source is unavailable and has no invented hash.

With tracking enabled, ``KK_PERF_TRACKING_COMPILES=1`` adds individual shader
compile callback records and generated MSL source sizes and BLAKE3 hashes.
Each source record carries its enclosing callback ID and shader stage. ID zero
indicates emission outside a tracked callback. A callback can compile several
linked stages. Binary deserialization callbacks have a separate origin label;
they can still perform Metal compilation. Source records cover newly generated
MSL, including generated helper stages, and do not dump shader contents.

Stage durations are nested elapsed wall times on the calling thread. They do
not measure compiler-service CPU usage or attribute asynchronous callback work
on other threads. Diagnostic logging, source hashing and timing add overhead.
Tracking is disabled by default.

Metal workarounds
*****************

Different workarounds are applied throughout the project to avoid issues such
as:

- Metal API and Vulkan API discrepancies
- Metal bugs
- MSL compiler bugs
- MSL compiler crashes

These workarounds can be found in:

.. toctree::
   :maxdepth: 1

   kosmickrisp/workarounds

Graphics library reuse
----------------------

Each cached compiler retains up to 128 graphics libraries and 8 MiB of
UTF-16 shader source. Reuse requires identical generated source, including
any diagnostic salt, and identical language version, math mode and floating
point function options. Concurrent identical requests share the synchronous
native compilation. Completed entries are evicted in insertion order; active
entries are retained until compilation finishes. Requests outside the bounds
compile without retention. Failed compilations are retried by later requests.

This in-memory reuse applies to vertex and fragment libraries. Function
constants and render pipeline state remain part of pipeline creation. Single
library compute compilation and persistent archive identities are unchanged.

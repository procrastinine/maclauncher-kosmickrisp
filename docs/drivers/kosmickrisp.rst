KosmicKrisp
###########

KosmicKrisp is a Vulkan 1.4 driver for Apple GPUs on macOS. It is implemented
on top of Metal 4. iOS is not supported.

System requirements
*******************

- macOS 26 or later, which provides Metal 4.
- An Apple GPU that supports the Metal 4 GPU family. The Metal 4 programming
  model is available from the Apple7 GPU family onward, which begins with the
  M1 series and the A14 Bionic.

The driver uses the first Metal device that supports the Metal 4 GPU family.
When no Metal device supports it, the driver reports no physical device.

Shaders compile as Metal Shading Language (MSL) 4.0 unless they require MSL
4.1, which is available on macOS 27 and later. ``shaderSharedFloat32AtomicAdd``
is reported only on macOS 27 and later.

Apple GPU families
******************

Native Metal capability differs by Apple GPU family. Apple's `Metal Feature Set
Tables <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>`__
(October 23, 2026) assign the families as follows:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Family
     - GPUs
   * - Apple7
     - M1 series, A14 Bionic
   * - Apple8
     - M2 series, A15 Bionic, A16 Bionic
   * - Apple9
     - M3 series, M4 series, A17 Pro, A18 series
   * - Apple10
     - M5 series, A19 series
   * - Apple11
     - M6 series

KosmicKrisp determines the family once, when the physical device is created,
as the highest Apple GPU family the Metal device supports. It uses a native
Metal path where that family provides one:

- Every supported device is Apple7 or later, so the capabilities gated on
  Apple7 (cooperative matrices, fragment shader interlock and
  rasterization-order color attachment access) do not vary by family.
- Apple8 and later report 64-bit integer image atomics
  (``VK_EXT_shader_image_atomic_int64``) and storage image and storage texel
  buffer support for ``R64_UINT`` and ``R64_SINT`` in every mode. Each image
  operation takes a hashed lock and publishes its result with a native 64-bit
  texture minimum or maximum. On Apple7 the same capabilities are the
  ``image_atomic64`` emulation entry, which is on in the default performance
  mode. Apple7 has no 64-bit texture atomics, so each operation reads and
  writes the two 32-bit words of the texel under the lock.
- Sparse binding and sparse residency are reported when the device supports
  placement sparse resources with 64 KiB sparse pages. On macOS 26.4 and later
  this follows Metal's placement sparse query. Earlier macOS 26 releases
  require Apple7 or later; Apple's Metal Feature Set Tables list placement
  sparse buffers and textures for every Apple7 macOS device and for all
  Apple8 and later GPUs.
- Apple10 and later:

  - report ``depthBounds`` and use Metal depth bounds testing. Earlier
    families report the feature through the ``depth_bounds`` emulation
    entry, which is off in the default performance mode.
  - apply ``mipLodBias`` in the Metal sampler. Earlier families apply the bias
    in the shader.
  - use the Metal sampler reduction mode for minimum and maximum filtering
    and report it for formats that support linear filtering. Earlier families
    compute the reduction in the shader and report it for every sampleable
    format.
  - report a maximum 1D, 2D and cube image dimension of 32768. Earlier
    families report 16384.

- On Apple7 and Apple8 with macOS 26, multisampled pipelines keep Metal's
  triangle merging for fragment shaders that need full-quad helper
  invocations or unmerged triangles for barycentrics, because the method that
  disables merging breaks derivatives in multisampled rendering on those
  GPUs. Edges shared by merged triangles can then differ from Vulkan
  rasterization rules.
- The active Metal workarounds also depend on the family and the macOS
  version; see :doc:`kosmickrisp/workarounds`.

.. include:: kosmickrisp/features.rst

Performance and conformance
***************************

KosmicKrisp emulates Vulkan capabilities that Metal does not provide directly,
using shader lowering, compute passes or captured geometry. Each emulation is
a named entry. ``KK_EMULATION`` selects which entries the device reports.

Two modes set the starting selection:

``performance``
   The default. Each entry uses its performance default.
``conformance``
   Every entry is enabled.

Performance defaults are selected for the running Apple GPU family. Native
paths and bounded compatibility work remain available; software ray traversal,
FP64 and coarse fragment shading require explicit selection. Mesh dispatch is
selected on every family: direct Mesh and bounded direct Task draws are
native, Apple7 and Apple8 prepare native indirect inputs before complete
render scopes, and Apple9 and later retain GPU indirect command expansion
where eligible. Buffer atomic64, denormal controls, raster ordering and
conservative rasterization retain their selected costs. Requested FP32
preservation uses native arithmetic and comparisons with subnormal guards;
cold repair handles true or possibly flushed subnormal results. Shaders that
request neither denormal mode emit no preservation guards.

Fragment barycentrics are selected on every family. Center-only coordinates
are native; exact or reordered coordinates and per-vertex inputs use corner
transport only in shaders that request them. Apple10 and Apple11 use native
pre-raster per-vertex inputs for eligible pipelines, and topology and replay
combinations outside that path retain corner transport. Hardware availability
alone does not select a feature: the ray-tracing implementation uses software
traversal even on GPUs with hardware ray tracing.

.. include:: kosmickrisp/emulation-performance-families.rst

For example, vkd3d-proton reports shader model 6.2 and later only when FP32
flush-to-zero, FP32 denormal preservation and independent denormal controls
are all reported. Disabling either denormal entry limits vkd3d-proton to
shader model 6.1, so both entries stay on in performance mode.

``KK_EMULATION`` is read once, when the physical device is created. Its value
is a comma-separated list of tokens. Whitespace around each token is ignored.

- ``performance`` or ``conformance`` as the first token selects the mode.
  Without either, performance mode applies.
- ``apple7`` through ``apple11``, or ``mode=7`` through ``mode=11``, add
  entries with a declared native-family threshold at or below that family to
  the performance defaults. Later named overrides apply to emulated entries.
- ``+name`` enables the entry ``name`` and ``-name`` disables it. Tokens apply
  in order, so the last token for an emulated entry takes effect. Entries native
  on the running device remain advertised; diagnostic extension hiding still
  applies.
- ``-VK_extension_name`` hides that device extension together with its
  feature and property fields, including their core-version query aliases.
  Extension hiding applies after entry selection.
- Any other token, including a mode name after the first position, prints one
  line to standard error and is ignored.

Examples:

- ``performance,-denorm_preserve`` keeps the performance defaults and disables
  denormal preservation.
- ``conformance,-ray_tracing`` enables every entry except ray tracing.
- ``conformance,-VK_KHR_ray_query`` enables every entry and hides
  ``VK_KHR_ray_query``.

Disabling an emulated entry removes the extensions, features, properties and
format features that the entry controls. Disabling an entry can lower the shader
model, feature level or interfaces available to a translator, as listed in the
table below. Enabling an entry keeps the support the device otherwise reports
and adds nothing beyond it; ``+denorm_flush`` leaves FP16 and FP64
flush-to-zero unsupported. Apart from the behavior entry
``precise_timestamps``, the variable changes only the reported capabilities;
command recording and shader compilation do not read it.
``precise_timestamps`` changes no reported capability and selects how
timestamps written outside a render pass are recorded (see
`Timestamp queries`_).

The table lists each entry's emulated behavior, cost, performance default,
the reason for that default, the effect of disabling it, and the token that
enables it in performance mode.

.. include:: kosmickrisp/emulation.rst

Family-dependent emulation
**************************

The `Apple GPU family table <#apple-gpu-families>`__ maps
Apple7 through Apple11 to GPU models. The `feature table above <#features-and-apple-gpu-families>`__ lists the
native family and implementation of individual features. Those references use
Apple's `Metal Feature Set Tables
<https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>`__
(October 23, 2026, pages 2 through 6).

A Metal capability can provide one component of a Vulkan feature without
providing the whole feature. Native barycentric coordinates, for example,
are separate from access to pre-raster per-vertex values. Metal ray-tracing
facilities do not imply that a Vulkan implementation uses them. Programming
model, operating-system and device capability requirements also apply.

The emulation registry's optional ``native_from`` field identifies the first
Apple GPU family with a native implementation path for that entry.
``null`` means no Apple family provides that native path. An absent
field makes no native-family claim. A grouped entry requires a native path for
all the behavior it controls before it has a native-family threshold. A native
path can keep driver work around the Metal feature it relies on: from Apple8,
``image_atomic64`` keeps its hashed lock and publishes each result with a
native 64-bit texture minimum or maximum.

On a device at or above an entry's threshold, that entry remains advertised in
every ``KK_EMULATION`` mode, including with a named disable token. The implemented native path may retain the driver work described in the registry. The performance default and named overrides
select emulation on earlier devices. A diagnostic ``-VK_extension_name`` can
still hide the extension and dependent interfaces. Thresholds never create
support that the physical-device builder does not provide.

.. include:: kosmickrisp/emulation-native-families.rst

Chip-generation presets
=======================

``KK_EMULATION=appleN`` selects performance mode and adds every entry whose
``native_from`` is at most N. Supported presets are ``apple7`` through
``apple11``. ``performance,apple9`` is equivalent to ``apple9``; ``mode=9`` is
a spelling alias. The family-to-GPU mapping is in the
`main family table <#apple-gpu-families>`__.

On an Apple8 device, ``apple9`` enables emulation for entries with thresholds
through Apple9. On an Apple10 device, those entries already use their native
paths, so that preset adds nothing to the native set. Entries with no threshold,
or with a threshold above the preset, retain their performance defaults.
Presets add capabilities and never hide a device's native capabilities.

Named overrides apply from left to right after a preset. For example,
``apple9,-name`` disables emulation of ``name`` on an earlier device, while
``apple9,-name,+name`` enables it again. An entry native on the running device
remains native and advertised. ``conformance`` enables every implemented entry;
a subsequent preset leaves that selection enabled. An out-of-range family
number produces the ordinary unknown-token warning and is ignored.

.. include:: kosmickrisp/emulation-presets.rst

Family test selector
====================

``MESA_KK_APPLE_FAMILY=N`` is a test selector. It is read once, when the
physical device is created. When N is at least 7 and below the family of the
running GPU, the driver uses family N for every decision it bases on the
family: native-family thresholds, format support, shader routes and
workarounds. Metal's runtime capability queries, such as placement sparse
support, keep the answers of the running GPU, and the GPU itself keeps its own
behavior. On an Apple8 device, ``MESA_KK_APPLE_FAMILY=7`` selects the Apple7
route of ``image_atomic64`` and reports that entry as emulated. While the
selector is active, pipeline caches, shader binaries and the disk cache use a
separate identity. Any other value prints one line to standard error and is
ignored. Without the variable the driver is unchanged.

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
   - meson (1.9.1 or later, also available as a Python package)
   - cmake
   - pkg-config
   - libclc
   - llvm (20.1.8 or later)
   - spirv-llvm-translator
   - spirv-tools (static library preferred; the Homebrew package does not
     include it, the Vulkan macOS SDK does)

An existing llvm installation can be checked with ``llvm-config --version``,
which must print at least ``20.1.8``. Otherwise, install llvm from Homebrew and
add it to the ``PATH`` as the Homebrew instructions describe.

- Python
- Python packages
   - mako
   - packaging
   - pyyaml
   - meson (1.9.1 or later, if not installed through Homebrew)

Since Homebrew manages the Python environment, a Python virtual environment
for these packages is recommended. To create a Python virtual environment
(e.g. ``$HOME/venv_mesa``):

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

Capability selection
====================

- ``KK_EMULATION``: selects emulated capabilities; see
  `Performance and conformance`_.
- ``MESA_KK_EXPERIMENTAL``: comma-separated list of experimental
  capabilities. When unset, ``custom_border`` and ``image_view_min_lod`` are
  enabled. A set value replaces that default; ``all`` enables every flag.

  - ``custom_border`` reports ``VK_EXT_custom_border_color`` and
    ``VK_EXT_border_color_swizzle``.
  - ``image_view_min_lod`` reports ``VK_EXT_image_view_min_lod``.
  - ``optimize_e5b9g9r9`` uses ``allowGPUOptimizedContents`` for
    ``E5B9G9R9`` images with transfer usage; some conformance tests for
    ``vkCmdCopyImage`` fail with it.
  - ``opaque_sync`` implements opaque file descriptor semaphores
    (``vkCreateSemaphore``, ``vkGetSemaphoreFdKHR`` and
    ``vkImportSemaphoreFdKHR``) with shared Metal events within the process.
    Reported external semaphore properties do not change.

- ``MESA_KK_DISABLE_WORKAROUNDS``: ``all`` disables all workarounds. Otherwise,
  a comma-separated list of workaround numbers to disable, e.g. ``1,3,4``.
  Workarounds are also disabled automatically on macOS versions and GPU
  families that do not need them. Workaround 2 stays active on Apple10 and
  later.

Diagnostics
===========

- ``MESA_KK_DEBUG``: comma-separated debug flags.

  - ``msl`` logs every generated MSL shader and every native library source
    with a ``KK_MSL`` header, and prints interlock and mesh dispatch traces to
    standard error.
  - ``force_robustness`` compiles every shader with all robustness behaviors
    enabled.
  - ``force_per_vertex_buffer`` passes the ``PerVertexKHR`` values of emulated
    shaders through memory instead of varying slots.

- ``MESA_KK_GPU_CAPTURE``: ``1`` starts a Metal GPU capture at device creation
  and ends it at device destruction. Internal driver operations carry debug
  labels in the capture.
- ``MESA_KK_GPU_CAPTURE_DIRECTORY``: writes the Metal GPU capture to
  ``<directory>/<process name>.gputrace`` when Metal supports GPU trace
  documents. Without it, the capture goes to Xcode.
- ``KK_CAPTURE_DIR``: enables frame capture on request. The directory is
  created at device creation. When a regular file named ``request`` exists in
  it at a present, the first swapchain image of that present is written as
  ``frame-N.ppm`` (8-bit RGB) with metadata in ``frame-N.txt``, where ``N`` is
  the device's present count. The request file is removed once the frame is
  written.
- ``KK_DEBUG_MSL_SALT``: a nonempty value prefixes every MSL source passed to
  the Metal compiler with a comment containing the base64-encoded value, and
  adds the value's hash to the shader cache identity. It is read once, at the
  first device creation.
- ``KK_DEBUG_EAGER_COMPUTE_TRANSLATION``: a value other than ``0`` translates
  every compute shader during pipeline creation; only the Metal library and
  pipeline compile in the deferred build. It is read at device creation.
- ``KK_DEBUG_FAIL_COMPUTE_TRANSLATION``: a nonzero workgroup width makes the
  translation of every compute shader with that workgroup width fail with
  ``VK_ERROR_OUT_OF_HOST_MEMORY``, to exercise translation failure handling.
  It is read at device creation.
- ``MESA_SHADER_CACHE_SHOW_STATS``: in addition to Mesa's shader cache
  statistics, prints one ``KK_SHADER_CACHE_BINDINGS`` line per shader written
  to the cache, with its source hash, embedded sampler count and whether its
  bindings could be serialized.
- ``KK_PERF_TRACKING`` and the ``KK_PERF_TRACKING_*`` variables: see
  `Performance tracking`_.

Performance tracking
********************

Tracking is disabled by default. The variables are read at device creation:

- ``KK_PERF_TRACKING``: any value other than ``0`` or ``false`` enables
  interval timing records, written as newline-separated JSON.
- ``KK_PERF_TRACKING_LOG``: appends the records to this file instead of
  standard error. If the file cannot be opened, an error is printed and
  records go to standard error.
- ``KK_PERF_TRACKING_INTERVAL_MS``: time interval between records in
  milliseconds. The default is 1000.
- ``KK_PERF_TRACKING_INTERVAL_FRAMES``: frame interval between records. The
  default is 60. A record is written when either interval elapses.
- ``KK_PERF_TRACKING_COMPILES``: any value other than ``0`` or ``false`` adds
  individual shader compile records.
- ``KK_PERF_TRACKING_SUBMITS``: ``1`` adds ``kk_submit_trace`` records for each
  queue submission and ``kk_sync_trace`` records for each timeline wait.
- ``KK_PERF_TRACKING_BARRIERS``: ``1`` adds ``kk_render_pass`` begin and end
  records with their attachments, and a ``kk_barrier`` record for each
  barrier.
- ``KK_PERF_TRACKING_GPU_ENCODERS``: ``1`` adds GPU encoder timing.

GPU encoder timing
==================

``KK_PERF_TRACKING_GPU_ENCODERS=1`` adds GPU encoder timing using Metal 4
counter heaps. Each timed encoder writes relaxed-granularity timestamps as its
first and last commands (a render encoder's last timestamp follows its
fragment stage), so its span covers the work the encoder executes. The fixed
GPU cost of starting and ending an encoder, tens of microseconds, lies outside
the spans and remains in the submission ranges of ``gpu_frames``; the
``gpu_encoder_scope`` field names this as ``encoder_work_spans``. Temporal
scene callbacks are timed by command buffer timestamps around their encoders.
Synchronous CPU resolution runs in the
existing submission feedback path after GPU completion. Render, compute, virtual
Mesh, internal meta and blit work is identified; opaque MetalFX and other
temporal scene callbacks are reported as named groups. Draws, dispatches, blits
and pipeline binds are attributed in the driver's encoder calls, which test one
flag when the option is unset; no timestamp heap or samples are created then.

A device times encoders through at most two timestamp heaps of 4,096 entries,
allocated on first use. Metal allows a process 32 live timestamp heaps of at
most 4,096 entries, which application timestamp query pools share (see
`Timestamp queries`_). Each timed encoder holds two entries until its record is
released, after its submission's feedback or the command buffer's next
recording. An encoder that starts while all 4,096 entry pairs are held, or
after Metal refused a heap, is reported as unavailable; recording never waits
for an entry.

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
sources. MetalFX's private shader source is unavailable and has no hash.

Compile records
===============

``KK_PERF_TRACKING_COMPILES`` adds individual shader compile callback records
and generated MSL source sizes and BLAKE3 hashes. Each source record carries
its enclosing callback ID and shader stage. ID zero indicates emission outside
a tracked callback. A callback can compile several linked stages. Binary
deserialization callbacks have a separate origin label; they can still
perform Metal compilation. Source records cover newly generated MSL, including
generated helper stages, and do not dump shader contents.

Each graphics or compute pipeline creation call also writes
``kk_pipeline_create`` begin and end records with its pipeline count. The
begin record carries the union of the create infos' pipeline creation flags
and whether any create info chains creation feedback; the end record carries
the result and wall time.

With compile records enabled, ``KK_PERF_TRACKING_BUILD_REASONS=1`` adds
native library and archive outcomes, exact archive search keys and stable
function-constant value digests. Each attributed device also identifies the
mapped driver image. These identify source-cache misses, native
archive misses and constant specializations as separate, overlapping facts.
Joined graphics-library workers retain the enclosing callback identity.

Deferred compute records also carry a build ID from enqueue to shader
destruction, the source hash and shader-cache key, first bind and first recorded
dispatch. Cancellation is distinct from successful completion. A
``completed-unused`` result means the shader was destroyed without a recorded
dispatch; it is assigned at destruction, since a completed shader can be used
later. Recorded dispatch does not measure GPU invocation counts.

Stage durations are nested elapsed wall times on the calling thread. They do
not measure compiler-service CPU usage or attribute asynchronous callback work
on other threads. Diagnostic logging, source hashing and timing add overhead.

Mesh dispatch
*************

Task shaders whose child grids exceed the native object-stage bound execute
as compute kernels. A batch retains at most 256 Task groups, with payload slots
sized from the shader declaration. Prefix sums of the actual child grids map
Mesh groups back to their parent Task group.

Mesh multiview executes each original view with its explicit layer and view
index, supporting up to 8 views (``maxMeshMultiviewViewCount``); other
multiview draws support up to 32 views. The feature and limit follow the selected
Mesh entry on each family. Attachment clears use explicit layer rectangles
with a single-view native pipeline.

On Apple9 and later, pipelines without captured flat outputs expand the real
Mesh count into native indirect commands and execute a GPU-written range.
Other pipelines use indirect Mesh draws. A completed batch publishes its real
overflow count before any additional slices are recorded. The continuation
retains the payload and records until every slice has consumed them, then
reuses storage for the next batch. At most 32 overflow draws are recorded at a
time. Indirect Task inputs are captured on the GPU in submission order;
command recording does not read application indirect buffers on the CPU.

Shader compilation
******************

Built-in compute kernels compile when an operation first needs them. Each
kernel shares one completed pipeline within the Vulkan device. Native compile
failures propagate to command recording; a failed kernel is not dispatched.

Compute pipeline creation
=========================

A single compute pipeline created without a native archive hit keeps its
compiler input and returns without translating it: the preprocessed NIR, the
descriptor set layouts and immutable sampler parameters by value, push
constant ranges, robustness, enabled features and sparse residency classes.
Its build, on the bounded compiler queue at background priority, translates
that input to MSL, including any sparse residency guards, then compiles the
Metal library and pipeline. Translation in the build uses the default
allocator; the application's allocation callbacks are used only during API
calls. A pipeline restored from a pipeline cache defers its build the same
way; one stored with its MSL compiles only the Metal library and pipeline.

The build stores the translated MSL and shader metadata in the Mesa disk cache
under a digest of the compiler input. A later build of the same input, in
this or a later process, restores them and compiles only the Metal library and
pipeline. In a pipeline cache, such a pipeline is serialized as its compiler
input and an XXH3-64 checksum of it, whether or not its build has run, so
``vkGetPipelineCacheData`` reports the same size before and after the build.
Pipeline cache data whose compiler input does not match its checksum is
rejected, and the pipeline compiles from SPIR-V. A pipeline restored from that
data in a creation that cannot defer, such as one with creation feedback,
restores the stored translation, or translates during creation and stores the
translation; it is serialized as its compiler input again. A pipeline whose build did not run in its process, for example
because the pipeline or device was destroyed first, has no stored translation
in a later process: binding it there before its build runs translates and
compiles it on the binding thread, unless it was predicted.

A translation failure in a build is recorded in the Mesa disk cache under a
checksum of the compiler input. Later creations of that input, fresh or from
a pipeline cache, translate during creation and return the error from
``vkCreateComputePipelines``. Builds do not call debug messengers: the
result of a failed build is reported when the binding thread joins it.

Once its native pipeline is built, such a pipeline releases its MSL. Later
binds, sampler emulation variants and pipeline cache data use the native
pipeline, the retained Metal function and the compiler input. When
device-generated commands first need an indirect-command pipeline for it, the
translation is restored from the Mesa disk cache, or translated again from
the compiler input, and released after that pipeline is built.

Binding the pipeline compiles it on the binding thread when its
compile has not started, or waits for the compile in progress and raises the
compiling thread to user-initiated priority for the wait; a translation or native
compile failure loses the device at first use. Destroying a pipeline does not wait.
When the last pipeline or pipeline cache reference to the shader is
destroyed, a compile that has not started is cancelled, and a compile in
progress is joined at user-initiated priority and its result discarded.
Device destruction does the same for every remaining compile. A cache-only
creation (``VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT``)
compiles nothing on a miss; a shader it restores from a cache defers its build
and the creation does not wait for it. Creation stays synchronous for
multi-pipeline calls, creation feedback, early-return failures, statistics and
internal-representation capture, indirect-bindable pipelines and sparse
variants selected at bind. Shaders with
descriptor-heap embedded samplers are translated during creation, and their
Metal compile is deferred unless they use 64-bit atomic locks. The first
deferred compute pipeline of a device allocates the device's 64-bit atomic
lock table, which builds of translated shaders that use 64-bit atomic locks
share.

Deferred builds wait while demand compiles run. A device records the first
bind of each deferred build by a digest of the build's source: the compiler
input of a pipeline translated in its build, otherwise its MSL. A thread of
the device stores the recorded digests in the Mesa disk cache, in the Metal
archive namespace, every two seconds and at device destruction, at most 4096
digests. Each store merges the newest stored set, so devices that an
application creates and destroys while another device runs add to one set
instead of replacing it. At device creation the thread loads the set. A
deferred build whose source is in it is predicted: it starts at once on two
utility-priority workers that demand compiles do not pause, so it is usually
ready before its first bind. Builds queued before the set loads move to those
workers when it loads, unless they have started. A new
``KK_DEBUG_MSL_SALT`` value starts an empty set.

Library reuse
=============

Each cached compiler retains up to 1024 Metal libraries and 64 MiB of shader
source characters. Reuse requires identical generated source, including any
diagnostic salt, and identical language version, math mode and floating point
function options. Concurrent identical requests share the synchronous native
compilation; a request waiting for a background-priority compile raises it to
user-initiated priority for the wait. Completed entries are evicted in
insertion order; active entries are retained until compilation finishes.
Requests outside the bounds compile without retention. Failed compilations
are retried by later requests.

This in-memory reuse applies to every library the driver compiles: vertex and
fragment pairs, compute, mesh, ray tracing and internal kernels. Function
constants and pipeline state remain part of pipeline creation. Persistent
archive identities do not use it.

Native pipeline archives
========================

Native pipeline archives share Mesa's disk-cache namespace and storage
budget. Checkpoint journals retain all referenced archive pieces. Up to eight
opened archive objects are retained in an in-memory least-recently-used set;
active requests retain their own archive and backing file.

Lookup hints combine shader-library source hashes and compiler options with
selected resolved compute, render, mesh or tile pipeline state. Each
checkpoint records the hints of the pipelines it captured. A hint can refer to
several pieces when pipelines differ only in state outside the hint, such as
function constant values. A lookup opens only the pieces recorded for its
hint. A request whose hint has no recorded piece, or whose descriptor has no
hint, uses normal compilation without opening an archive piece. Metal verifies
the complete native descriptor, including function constants, so a piece that
does not contain the exact pipeline is a miss. Unavailable archive payloads are
cache misses and use normal compilation.

A checkpoint stores and verifies its archive payload before publishing the
piece reference and captured lookup hints together in the two-slot journal.
The journal stores one row for each hint and piece pair. Journals with a single
row per hint, and legacy journals containing only piece references, remain
readable; pipelines without a recorded piece compile once and are recorded by
the next checkpoint.

A checkpoint holds the capture gate exclusively while Metal serializes the
captured pipelines. On macOS 27 and later, pipeline creation that finds the
gate held compiles on a second compiler without a serializer instead of
waiting. Each such descriptor is created again on the capturing compiler after
the checkpoint, where the native binaries are already compiled, so the next
checkpoint records it. Remaining descriptors are recorded before the final
checkpoint at device teardown. On earlier systems, pipeline creation waits for
the checkpoint.

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

Shader module identifiers
-------------------------

``VK_EXT_shader_module_identifier`` identifies SPIR-V modules with Mesa's
BLAKE3 algorithm. Module handles and inline module create information produce
the same identifier. Pipeline creation with identifiers uses existing pipeline
cache entries; a cache miss returns ``VK_PIPELINE_COMPILE_REQUIRED`` when
``VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`` is set.
Identifiers do not alter shader compilation or draw commands.

Graphics pipeline libraries
--------------------------

``VK_EXT_graphics_pipeline_library`` provides all four library parts,
independent pipeline layouts and independent interpolation decorations.
The ``graphics_pipeline_library`` registry entry selects this interface on
Apple7 through Apple11 in performance and conformance modes.

Ordinary one-sample vertex and color fragment libraries compile separate
Metal shader bodies. Final assembly reuses those bodies and creates the
vertex fetch helper and render pipeline state. Coupled raster state,
other graphics stage chains and link-time optimization use retained
canonical shader sources with the complete pipeline compiler. Pipeline
caches retain final executable combinations independently of their source
libraries. Original shader modules, layouts and library handles can be
destroyed after their consumers are created.

``graphicsPipelineLibraryIndependentInterpolationDecoration`` is true.
``graphicsPipelineLibraryFastLinking`` is false: final assembly of a new
combination still invokes Metal's render pipeline backend. Libraries can
move stage compilation ahead of drawing, while final render pipeline
creation can still compile native code.

Timestamp queries
-----------------

Timestamp query pools sample into Metal 4 timestamp counter heaps. Metal
allows a process 32 live timestamp heaps of at most 4,096 entries. A device
creates heaps of 4,096 entries as its pools need them and keeps them until
the device is destroyed. Each pool takes its queries from those heaps in units
of 32 and returns them when it is destroyed, so the live pools of a process
share up to 131,072 entries, less any other timestamp heaps in the process,
whatever their number and size. Pool creation fails with
``VK_ERROR_OUT_OF_DEVICE_MEMORY`` when no unit is free and Metal refuses
another heap, and writes one warning to standard error naming the query count,
the query type and what ran out.

Outside a render pass, ``vkCmdWriteTimestamp2`` writes in the current compute
encoder. Metal records only the first four relaxed timestamps of a compute
encoder, all with one value. By default, every timestamp written in one compute
encoder reports that encoder's first relaxed sample, which adds no GPU wait:
timestamps stay available, nonzero and non-decreasing, and durations measured
within one encoder are 0. The ``precise_timestamps`` entry, on in
``conformance`` mode and off by default in ``performance`` mode, makes each
such timestamp precise: it waits for the encoder's earlier work, about 7
microseconds of GPU time per timestamp, and records its own time.

Inside a render pass, Metal samples each render stage once per render encoder.
The first timestamp written at a stage takes that sample, and later timestamps
at the same stage in the encoder, from any pool, report it.

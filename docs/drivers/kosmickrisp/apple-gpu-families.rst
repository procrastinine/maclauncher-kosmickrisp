Family-dependent emulation
##########################

The `Apple GPU family table <../kosmickrisp.html#apple-gpu-families>`__ maps
Apple7 through Apple11 to GPU models. The :doc:`features` reference lists the
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
Apple GPU family with a complete native implementation for that entry.
``null`` means no Apple family provides that native implementation. An absent
field makes no native-family claim. A grouped entry requires a native path for
all the behavior it controls before it has a native-family threshold.

On a device at or above an entry's threshold, that entry remains advertised in
every ``KK_EMULATION`` mode, including with a named disable token. Its native
path has no emulation overhead. The performance default and named overrides
select emulation on earlier devices. A diagnostic ``-VK_extension_name`` can
still hide the extension and dependent interfaces. Thresholds never create
support that the physical-device builder does not provide.

.. include:: emulation-native-families.rst

Chip-generation presets
***********************

``KK_EMULATION=appleN`` selects performance mode and adds every entry whose
``native_from`` is at most N. Supported presets are ``apple7`` through
``apple11``. ``performance,apple9`` is equivalent to ``apple9``; ``mode=9`` is
a spelling alias. The family-to-GPU mapping is in the
`main family table <../kosmickrisp.html#apple-gpu-families>`__.

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

.. include:: emulation-presets.rst

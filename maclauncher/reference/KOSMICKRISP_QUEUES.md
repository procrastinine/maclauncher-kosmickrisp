# KosmicKrisp queue families

Separate Vulkan queues allow graphics, compute and copies to progress independently. MacLauncher KosmicKrisp provides four queue families:

| Family index | Operations | Queue count | Minimum image transfer granularity |
| --- | --- | ---: | --- |
| 0 | Graphics, compute and transfer | 2 | 1 × 1 × 1 |
| 1 | Compute and transfer | 1 | 1 × 1 × 1 |
| 2 | Transfer | 1 | 1 × 1 × 1 |
| 3 | Graphics, compute and transfer | 1 | 1 × 1 × 1 |

Each requested queue owns a separate Metal 4 command queue. Applications that request one queue create one application command queue; the second graphics queue and the dedicated queues are created only when requested. Binary and timeline semaphores order work between families through the existing Metal shared-event synchronization path. Buffer and image queue-family ownership transfers use the same resource storage and barrier implementation; image layouts do not require a second copy of an image.

Family 0 retains the general-purpose queue interface. Family 3 provides a second graphics-capable family for graphics ownership transfers and concurrent resources across families. The compute family excludes graphics, and the transfer family excludes both graphics and compute, allowing Vulkan consumers to select dedicated queues by their operation flags. Sparse-binding flags, when supplied by the sparse-resource adaptation, are additional capabilities of the corresponding families.

Image copies on the transfer family accept regions at the reported granularity. Format block sizes, buffer offsets, row pitches and the other Vulkan copy requirements still apply. All families expose 64 valid timestamp bits and medium global priority.

Metal presentation uses a separate internal queue, created when presentation requires it. That queue keeps drawable waits separate from application submissions.

The [extra-queue-families module](../../src/kosmickrisp/) supplies the family and device-queue changes; the [additional-graphics-family module](../../src/kosmickrisp/) appends family 3 and its device capacity. Queue submission and shared-event synchronization remain shared with Mesa.

Host continuations retain the remaining commands and application signals until a fresh private timeline point completes. Ordinary queues begin in immediate mode; Mesa starts a submission worker on demand when an unresolved dependency first requires it. Binary semaphore points are reserved when the application submits them, and cross-queue waits are issued to Metal only after their producer signals are committed. Deferred work drains before shared device resources are destroyed.

# Long shader vectors

MacLauncher KosmicKrisp supports `VK_EXT_shader_long_vector` with `longVector` enabled and `maxVectorComponents` equal to 1024. `SPV_EXT_long_vector` accepts vector lengths from 1 through 1024, including `OpTypeVectorIdEXT` lengths resolved from specialization constants.

Long vectors are represented as arrays of scalar components in the SPIR-V frontend. Arithmetic uses the existing scalar operation lowering, including conversions, floating-point controls and integer wrap decorations. Composite construction, static and dynamic component access, shuffles, bitcasts, selection, phi values, calls and returns use the same NIR memory and SSA machinery as other composite values. Reductions combine the scalar results. Subgroup operations preserve subgroup participation for each component.

Explicit buffer offsets and contiguous component strides remain intact. Long vectors can reside in the storage classes allowed by the SPIR-V extension, including Function, Private, Workgroup, Uniform, StorageBuffer, physical storage buffers and push constants. Vector-size specialization follows Vulkan's restrictions for explicitly laid-out storage classes. Numeric component widths require the corresponding existing shader and storage features. Graphics input/output interface restrictions remain those of Vulkan.

The adaptation does not add GPU dispatches, temporary device buffers, CPU transfers or draw-time resources. Compilation expands operations according to the number of vector components; shader code and compilation cost scale with the source operation count and vector length.

The pinned vkd3d-proton source consumes `longVector` and a limit of at least 1024 components as independent inputs to its Shader Model 6.9 eligibility check in [`device.c`](https://github.com/HansKristian-Work/vkd3d-proton/blob/87983f34c5cffa6691b7deb665d09d06376772ea/libs/vkd3d/device.c#L9981). Other inputs to that check are separate Vulkan contracts. Cooperative-matrix and float8 capabilities have their own feature and property requirements.

# Required compute subgroup size

MacLauncher KosmicKrisp accepts an explicit subgroup size of 32 for compute pipelines. The minimum and maximum subgroup sizes are both 32. Required-size pipelines and full-subgroup pipelines use Mesa's existing subgroup selection and Metal's fixed-width SIMD groups. Ordinary pipelines use the same compilation and dispatch path.

The `required-compute-subgroup-size` capability reports the compute stage in `requiredSubgroupSizeStages`. Other shader stages do not gain required-size support from this module. The adaptation adds no command allocations, uploads or per-dispatch work.

vkd3d-proton's experimental work-graph path additionally requires descriptor heaps, maximal reconvergence and the Vulkan memory model. The subgroup property alone does not enable work graphs.

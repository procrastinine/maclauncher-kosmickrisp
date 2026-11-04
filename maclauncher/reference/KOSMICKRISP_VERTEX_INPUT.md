# Vertex input

`VK_EXT_vertex_input_dynamic_state` supplies draw-time vertex bindings and
attributes through `vkCmdSetVertexInputEXT`. Layout changes use the bound vertex
shader without pipeline creation. Format conversion, channel ordering, instance
divisors and robust buffer bounds share the static vertex-fetch implementation.

The root descriptor carries one pointer to dynamic vertex metadata. Metadata
contains one format, divisor and stride record per vertex shader input and is
uploaded when vertex state changes. Static layouts fold format and divisor
metadata into the shader.

Vertex input root slots follow the owning linked shader’s live attribute mask.
Native raster alternatives lower vertex fetches after varying linking and use
that same mask, including when an alternative consumes only a subset of the
inputs. Static formats, dynamic metadata and robust bounds retain the same
compact slot for each original attribute location.

Format encoding and runtime fetch derive from Mesa draft
[!45831](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/45831), commit
`83b7c659c4aebd67863bbe105c299b1b383454eb`, by Nilton Perim Neto under the MIT
license. Mesa notices remain with the source inputs.

# Cull distances

`shaderCullDistance` provides eight cull distances, with a combined clip and cull distance limit of eight. A primitive is rejected when all of its vertices have a negative value in any one cull plane. A zero value keeps the primitive in that plane. Cull distances do not cut a primitive at the plane.

Vertex, tessellation and geometry output uses Mesa Poly's primitive capture and clipping pipeline. Rejection precedes rasterization and retains the original shader invocations, transform feedback and primitive identities. Input assembly resolves topology, indices, primitive restart and adjacency before the final primitive test. Points, line segments and triangles use one, two and three vertex values respectively. Mesh shaders test their indexed vertices and combine the result with explicit primitive culling through Metal's mesh primitive cull output.

Clip distances retain their clipping behavior. Fragment shader reads receive the interpolated original distances.

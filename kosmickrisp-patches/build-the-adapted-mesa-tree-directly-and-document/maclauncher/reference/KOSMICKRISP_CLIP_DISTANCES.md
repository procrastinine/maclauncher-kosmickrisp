# User clip distances

The physical device exposes eight clip distances, eight cull distances and a combined maximum of eight.

User clip distances are retained through ordinary and generated polygon rasterization vertex shaders. Triangle lists, strips and fans use the same clip planes with static or dynamic polygon state. Smooth and flat fragment inputs retain their interpolation and provoking-vertex behavior.

Clip and cull varying slots use scalar Metal interface members. Captured slot vectors are scalarized before Metal translation, including the generated vertex shader used by polygon modes and exact depth-bias paths. Poly clips the captured primitive before rasterization.

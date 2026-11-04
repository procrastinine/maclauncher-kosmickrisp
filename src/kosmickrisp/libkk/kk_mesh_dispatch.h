/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_DISPATCH_H
#define KK_MESH_DISPATCH_H

/* M2 Max: Metal 3/4 direct and indirect mesh/object grids execute 2^21
 * workgroups; 2^21+1 executes none. This is independent of tested 1/32/128
 * thread workgroup sizes. A full physical slice is 32768 x 64 x 1. */
#define KK_MESH_MAX_PHYSICAL_GROUPS 2097152u

#endif

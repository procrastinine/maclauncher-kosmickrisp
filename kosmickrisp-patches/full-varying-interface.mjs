// SPDX-License-Identifier: MIT
import {applyPerVertexIO, sourceFiles as transportFiles} from './per-vertex-io.mjs';

export const id = 'full-varying-interface';
export const sourceFiles = ['kosmickrisp-patches/per-vertex-io.mjs', ...transportFiles];

/** Pack only interfaces exceeding the native varying component budget. */
export function apply(context) {
  applyPerVertexIO(context);
}

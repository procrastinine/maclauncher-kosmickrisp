// SPDX-License-Identifier: MIT
// Add feature table by implementation and Apple GPU family
import fs from 'node:fs';
export const id = "add-feature-table-by-implementation-and-apple-gpu";
export const sourceFiles = [
  "kosmickrisp-patches/add-feature-table-by-implementation-and-apple-gpu/docs/drivers/kosmickrisp/features.rst"
];
const added = file => fs.readFileSync(new URL("./add-feature-table-by-implementation-and-apple-gpu/" + file, import.meta.url), 'utf8');
export function apply({edit, edits}) {
  edits.set("docs/drivers/kosmickrisp/features.rst", added("docs/drivers/kosmickrisp/features.rst"));
}

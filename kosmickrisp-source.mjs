import {createSourceEditor} from './kosmickrisp-patches/source-editor.mjs';
import {patches} from './kosmickrisp-patches/registry.mjs';

export function adaptedKosmicSources(sourceRoot) {
  const context = createSourceEditor(sourceRoot);
  for (const patch of patches) patch.apply(context);
  return Object.fromEntries(context.edits);
}

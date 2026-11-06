// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

/** Keep ordinary replacement semantics while requiring a matching anchor. */
class SourceText extends String {
  constructor(text, relative) {
    super(text);
    this.relative = relative;
  }

  check(pattern) {
    let matches;
    if (typeof pattern === 'string') {
      matches = this.includes(pattern);
    } else if (pattern instanceof RegExp) {
      const probe = new RegExp(pattern.source, pattern.flags);
      probe.lastIndex = pattern.global ? 0 : pattern.lastIndex;
      matches = probe.test(this.toString());
    } else {
      throw new TypeError(`${this.relative}: source anchor must be a string or RegExp`);
    }
    if (!matches)
      throw new Error(`${this.relative}: missing original anchor ${String(pattern).slice(0, 100)}`);
  }

  replace(pattern, replacement) {
    this.check(pattern);
    return new SourceText(super.replace(pattern, replacement), this.relative);
  }

  replaceAll(pattern, replacement) {
    this.check(pattern);
    return new SourceText(super.replaceAll(pattern, replacement), this.relative);
  }
}

/** Local helpers share checked original source and preceding edits. */
class SourceEdits extends Map {
  #sourceRoot;

  constructor(sourceRoot) {
    super();
    this.#sourceRoot = sourceRoot;
  }

  get(relative) {
    let text = super.get(relative);
    if (text === undefined) {
      try {
        text = fs.readFileSync(path.join(this.#sourceRoot, relative), 'utf8');
      } catch (error) {
        if (error.code === 'ENOENT') return undefined;
        throw error;
      }
    }
    return new SourceText(text, relative);
  }

  set(relative, text) {
    if (typeof text !== 'string' && !(text instanceof String))
      throw new TypeError(`${relative}: source edit must return text`);
    return super.set(relative, String(text));
  }
}

/** Read upstream once per edit and retain each preceding adaptation. */
export function createSourceEditor(sourceRoot) {
  const base = path.resolve(sourceRoot);
  const edits = new SourceEdits(base);
  const read = relative => edits.get(relative) ?? fs.readFileSync(path.join(base, relative), 'utf8');
  function edit(relative, apply) {
    const before = new SourceText(read(relative), relative);
    const replace = (from, to) => before.replace(from, to);
    edits.set(relative, apply(before, replace));
  }
  return {base, edits, read, edit};
}

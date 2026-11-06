// SPDX-License-Identifier: MIT
// Regenerate the production library from the registered Mesa source and the
// glslang revision selected by the registered shaderc dependency's DEPS file.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {adaptSoftFp64Library} from './kosmickrisp-patches/soft-fp64-library.mjs';

const [mesa, glslang, python, glslangRevision] = process.argv.slice(2);
if (!mesa || !glslang || !python || !/^[a-f0-9]{40}$/.test(glslangRevision ?? ''))
  throw Error('Use MESA_SOURCE GLSLANG_EXECUTABLE PYTHON_EXECUTABLE GLSLANG_REVISION');
const source = path.join(mesa, 'src/compiler/glsl/float64.glsl');
const original = fs.readFileSync(source, 'utf8');
const sha256 = crypto.createHash('sha256').update(original).digest('hex');
const adapted = adaptSoftFp64Library(original);
const adaptedSha256 = crypto.createHash('sha256').update(adapted).digest('hex');
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'kosmic-soft-fp64-'));
const name = 'kk_soft_fp64_spv.h';
try {
  fs.mkdirSync(path.join(scratch, 'input'));
  fs.writeFileSync(path.join(scratch, 'input/float64.glsl'), adapted);
  // Relative generated filenames keep machine paths out of the SPIR-V.
  execFileSync(python, [path.join(mesa, 'src/util/glsl2spirv.py'),
    path.join(scratch, 'input/float64.glsl'), name, glslang, '--vn', 'kk_soft_fp64_spv',
    '--glsl-version', '450', '-Olib'], {cwd: scratch, stdio: 'inherit'});
  const notice = original.slice(0, original.indexOf('*/') + 2);
  fs.writeFileSync(new URL('./kosmickrisp-patches/kosmickrisp-soft-fp64-spv.h', import.meta.url),
    `${notice}\n/* Generated from pinned Mesa src/compiler/glsl/float64.glsl, SHA-256 ${sha256}.\n` +
    ` * Owned RTE-corrected source SHA-256 ${adaptedSha256}.\n` +
    ` * glslang ${glslangRevision}; upstream glsl2spirv -Olib.\n */\n` +
    fs.readFileSync(path.join(scratch, name), 'utf8'));
} finally {
  fs.rmSync(scratch, {recursive: true, force: true});
}

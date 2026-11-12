import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import {execFileSync} from 'node:child_process';

const root=path.resolve(import.meta.dirname,'..');
const git=(...args)=>execFileSync('git',['-C',root,...args],{encoding:'utf8',maxBuffer:1<<28});

test('the producers read files of this repository, not files through a pinned checkout',async()=>{
 const index=git('ls-files','-s','-z').split('\0').filter(Boolean).map(row=>{const [header,file]=row.split('\t'),[mode,object]=header.split(' ');return {mode,object,file};});
 const gitlinks=index.filter(entry=>entry.mode==='160000').map(entry=>entry.file);
 for(const link of index.filter(entry=>entry.mode==='120000')){
  const target=path.posix.normalize(path.posix.join(path.posix.dirname(link.file),git('cat-file','blob',link.object)));
  assert.ok(!target.startsWith('../'),`${link.file} points outside the repository`);
  assert.equal(gitlinks.find(gitlink=>target===gitlink||target.startsWith(gitlink+'/')),undefined,`${link.file} reads ${target} through a pinned checkout`);
 }
 for(const module of ['kosmickrisp-build.mjs','kosmickrisp-targets.mjs','kosmickrisp-patches/soft-fp64.mjs'])
  assert.ok(Object.keys(await import(path.join(root,module))).length,module);
});

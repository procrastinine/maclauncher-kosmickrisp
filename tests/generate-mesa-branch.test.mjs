import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {generateMesaBranch} from '../generate-mesa-branch.mjs';
import {adaptedKosmicSources} from '../kosmickrisp-source.mjs';
import {patches} from '../kosmickrisp-patches/registry.mjs';
import {mesaRevision} from '../pins.mjs';
test('generated commits reproduce the ordered adaptations over the exact Mesa pin',()=>{
 const root=path.resolve(import.meta.dirname,'..'),source=process.env.KOSMIC_MESA_SOURCE||path.join(root,'sources/mesa');
 const git=(...args)=>execFileSync('git',['-C',root,...args],{encoding:'utf8',maxBuffer:Infinity});
 const head=generateMesaBranch(source);
 assert.equal(Number(git('rev-list','--count',mesaRevision()+'..'+head)),patches.length);
 for(const [file,content]of Object.entries(adaptedKosmicSources(source)))
  if(content===null)assert.throws(()=>git('cat-file','-e',head+':'+file),undefined,file);else assert.equal(git('show',head+':'+file),content,file);
 assert.equal(generateMesaBranch(source),head);
});

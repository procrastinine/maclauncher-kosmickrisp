// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {createSourceEditor} from './kosmickrisp-patches/source-editor.mjs';
import {patches} from './kosmickrisp-patches/registry.mjs';
import {mesaRevision} from './pins.mjs';
const root=import.meta.dirname;
const git=(cwd,...args)=>execFileSync('git',['-C',cwd,...args],{encoding:'utf8',maxBuffer:Infinity}).trim();
export function generateMesaBranch(source){
 const pin=mesaRevision(), ref='refs/heads/mesa-with-adaptations';
 if(git(source,'rev-parse','HEAD')!==pin)throw Error('Mesa checkout differs from its pin');
 if(git(source,'status','--porcelain'))throw Error('Mesa checkout must be clean');
 if(git(root,'worktree','list','--porcelain').includes('branch '+ref+'\n'))throw Error('Generated branch must not be checked out');
 let old='';try{old=git(root,'rev-parse','--verify','--quiet',ref+'^{commit}');}catch{} 
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'kosmic-mesa-'));
 try{
  git(temporary,'init','-q');git(temporary,'fetch','--quiet','--no-tags',source,pin);
  git(temporary,'checkout','--quiet','--detach',pin);
  git(temporary,'config','user.name','procrastinine');git(temporary,'config','user.email','24549552+procrastinine@users.noreply.github.com');
  const catalog=JSON.parse(fs.readFileSync(new URL('./kosmickrisp-patches/patches.json',import.meta.url)));
  const context=createSourceEditor(source);
  for(const patch of patches){
   const entry=Object.values(catalog.patches).find(entry=>entry.id===patch.id);
   if(!entry)throw Error('Catalog entry missing: '+patch.id);
   patch.apply(context);
   for(const [file,content]of context.edits){const target=path.resolve(temporary,file);if(!target.startsWith(temporary+path.sep))throw Error('Invalid adaptation path');fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,content);}
   git(temporary,'add','--all');
   for(const role of ['AUTHOR','COMMITTER'])if(!git(temporary,'var','GIT_'+role+'_IDENT').startsWith('procrastinine <24549552+procrastinine@users.noreply.github.com> '))throw Error('Commit identity differs');
   const date=git(source,'show','-s','--format=%cI',pin);
   execFileSync('git',['-C',temporary,'commit','--quiet','--allow-empty','-m',entry.title],{env:{...process.env,GIT_AUTHOR_DATE:date,GIT_COMMITTER_DATE:date}});
  }
  const head=git(temporary,'rev-parse','HEAD');git(root,'fetch','--quiet','--no-tags',temporary,head);
  git(root,'update-ref',ref,head,old||'0'.repeat(40));return head;
 }finally{fs.rmSync(temporary,{recursive:true,force:true});}
}
if(process.argv[1]===import.meta.filename)console.log(generateMesaBranch(path.resolve(process.argv[2]||path.join(root,'sources/mesa'))));

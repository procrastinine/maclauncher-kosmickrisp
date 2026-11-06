import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
// A package's licence texts live in <root>/licenses/, each distinct text once; licenses/components.txt names
// the text each component uses. Texts compare by a fingerprint of their bytes without whitespace, so the same
// licence in another layout is not stored again. A missing or unreadable source is skipped with a warning.
const fingerprint=bytes=>crypto.createHash('sha256').update(bytes.filter(byte=>byte>32)).digest('hex'),stored=new Map();
export function addLicense(root,component,source,licence=component,compatibilityRelative){
 try{
  const dir=path.join(root,'licenses'),index=path.join(dir,'components.txt'),text=fs.readFileSync(source),key=fingerprint(text);
  fs.mkdirSync(dir,{recursive:true});
  if(!stored.has(dir)||!fs.existsSync(index)){ // a new or recreated folder
   fs.appendFileSync(index,'');
   stored.set(dir,new Map(fs.readdirSync(dir,{withFileTypes:true}).filter(entry=>entry.isFile()&&entry.name!=='components.txt').map(entry=>[fingerprint(fs.readFileSync(path.join(dir,entry.name))),entry.name])));
  }
  const texts=stored.get(dir),base=String(licence).replace(/[^\w.+-]+/g,'-');
  let file=texts.get(key);
  if(!file){
   file=base+'.txt';
   for(let n=2;fs.existsSync(path.join(dir,file));n++)file=`${base}-${n}.txt`;
   fs.writeFileSync(path.join(dir,file),text);texts.set(key,file);
  }
  fs.appendFileSync(index,`${component}: ${file}\n`);
  if(compatibilityRelative){fs.writeFileSync(path.join(root,compatibilityRelative),text);return {sha256:crypto.createHash('sha256').update(text).digest('hex')};}
 }catch(error){try{console.log(`::warning::No licence text for ${component}: ${error.message}`);}catch{}}
}

/** Merge a producer's component index through the same package-level operation. */
export function includeLicenses(root,directory){
 try{if(path.resolve(root,'licenses')===path.resolve(directory))return;
 for(const row of fs.readFileSync(path.join(directory,'components.txt'),'utf8').split('\n').filter(Boolean)){
  const split=row.lastIndexOf(': '),file=row.slice(split+2);
  addLicense(root,row.slice(0,split),path.join(directory,file),file.replace(/\.txt$/,''));
 }}catch(error){try{console.log(`::warning::No licence texts from ${directory}: ${error.message}`);}catch{}}
}

/** Add the licence texts at the top of a source directory, including the files of a LICENSES folder. */
export function addLicenses(root,component,directory,immediateChildren=false){
 try{
  if(immediateChildren){
   for(const entry of fs.readdirSync(directory,{withFileTypes:true}).sort((a,b)=>a.name<b.name?-1:a.name>b.name?1:0))if(entry.isDirectory())addLicenses(root,entry.name,path.join(directory,entry.name));
   return;
  }
  const copy=file=>{try{
   if(fs.statSync(file).isDirectory())for(const name of fs.readdirSync(file).sort())copy(path.join(file,name));
   else addLicense(root,component,file);
  }catch(error){try{console.log(`::warning::No licence text from ${file}: ${error.message}`);}catch{}}};
  for(const name of fs.readdirSync(directory).sort().filter(name=>/^(?:licen[cs](?:e|ing)|copying|notice|copyright|third[_ .-]?party[_ .-]?(?:notices?|licen[cs]es?)|(?:[AL]?GPL|MPL|Apache|MIT|BSD|ISC|Zlib|OFL|UFL|BSL|CC0|Unlicense)(?:[._ -]|$))/i.test(name)))copy(path.join(directory,name));
 }catch(error){try{console.log(`::warning::No licence texts in ${directory}: ${error.message}`);}catch{}}
}

/** Read the crate and toolchain directories already available to this build. */
export function addCargoLicenses(root,component,directory,env=process.env){
 try{
  let lock='';try{lock=fs.readFileSync(path.join(directory,'Cargo.lock'),'utf8');}catch{}
  const cargo=env.CARGO_HOME||path.join(process.env.HOME,'.cargo');
  for(const block of lock.split('[[package]]').slice(1)){
   const name=/^name = "([^"]+)"/m.exec(block)?.[1],version=/^version = "([^"]+)"/m.exec(block)?.[1];
   if(name&&version)for(const source of fs.globSync(path.join(cargo,'registry/src/*',name+'-'+version)))addLicenses(root,name+' '+version,source);
  }
 }catch(error){try{console.log(`::warning::No crate licence texts for ${component}: ${error.message}`);}catch{}}
 try{
  const home=env.RUSTUP_HOME||path.join(process.env.HOME,'.rustup'),selected=env.RUSTUP_TOOLCHAIN;
  if(selected)for(const toolchain of fs.readdirSync(path.join(home,'toolchains')).sort().filter(name=>name===selected||name.startsWith(selected+'-')))addLicenses(root,component,path.join(home,'toolchains',toolchain,'share/doc/rust'));
 }catch(error){try{console.log(`::warning::No toolchain licence texts for ${component}: ${error.message}`);}catch{}}
}

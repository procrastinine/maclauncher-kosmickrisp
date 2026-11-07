import {collectSoftFp64License} from './kosmickrisp-patches/soft-fp64.mjs';
import {mesaRevision} from './pins.mjs';
import {addLicense,addLicenses} from './licenses.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {adaptedKosmicSources} from './kosmickrisp-source.mjs';
import {buildKosmicTargets,describeNativeTools} from './kosmickrisp-targets.mjs';
import {prepareSourceTree,writeChanged} from './source-files.mjs';

const root=path.dirname(fileURLToPath(import.meta.url));
const pin=JSON.parse(fs.readFileSync(path.join(root,'kosmickrisp-source.json')));
const run=(command,args,options={})=>execFileSync(command,args,{stdio:'inherit',...options});
const read=(command,args)=>execFileSync(command,args,{encoding:'utf8',maxBuffer: Infinity}).trim();
const sparseNotices=path.join(root,'kosmickrisp-patches/sparse/kosmickrisp-sparse-notices');

/** Build only the original Kosmic driver and its normal Mesa shared compiler. */
export function buildKosmic(options){
 if(options.architecture&&options.architecture!=='arm64')return buildKosmicTargets(options,buildKosmic);
 const {source,work,meson,ninja,llvm}=options;
 if(read('git',['-C',source,'rev-parse','HEAD'])!==mesaRevision())throw Error('Kosmic source revision differs from its pin');
 const projected=path.join(work,'source'),build=path.join(work,'build');
 prepareSourceTree(projected,temporary=>{
  fs.cpSync(source,temporary,{recursive:true,filter:file=>path.basename(file)!=='.git'});
  for(const [file,content] of Object.entries(adaptedKosmicSources(source)))
   if(content===null)fs.rmSync(path.join(temporary,file),{force:true});else writeChanged(path.join(temporary,file),content);
 });
 const environment={...process.env,CC:'/usr/bin/clang',CXX:'/usr/bin/clang++',
  LLVM_CONFIG:path.join(llvm,'bin/llvm-config'),MACOSX_DEPLOYMENT_TARGET:pin.minimumMacOS,
  ...(options.pkgConfigPath?{PKG_CONFIG_PATH:options.pkgConfigPath}:{}),
  MESA_GIT_SHA1_OVERRIDE:mesaRevision(),CCACHE_DIR:path.join(work,'ccache'),CCACHE_MAXSIZE:'128M',
  PATH:[path.dirname(meson),path.join(llvm,'bin'),process.env.PATH].join(path.delimiter)};
 // Keep the deployment target in Meson's recorded commands as well as the
 // environment, so an incremental compile or link has the same target.
 const deploymentArgs=['c','cpp','objc'].flatMap(language=>[
  `-D${language}_args=-mmacosx-version-min=${pin.minimumMacOS}`,
  `-D${language}_link_args=-mmacosx-version-min=${pin.minimumMacOS}`]);
 const args=[...deploymentArgs,'--buildtype=release','--wrap-mode=nofallback','-Dvulkan-drivers=kosmickrisp',
  '-Dgallium-drivers=[]','-Dplatforms=macos','-Dopengl=false','-Dgles1=disabled','-Dgles2=disabled',
  '-Degl=disabled','-Dglx=disabled','-Dgbm=disabled','-Dllvm=enabled','-Dshared-llvm=enabled',
  '-Dmesa-clc=enabled','-Dprecomp-compiler=enabled','-Dspirv-tools=enabled','-Dzstd=enabled',
  '-Dxmlconfig=enabled','-Dbuild-tests=false','-Dvideo-codecs=[]'];
 fs.mkdirSync(build,{recursive:true});
 run(meson,['setup',...(fs.existsSync(path.join(build,'build.ninja'))?['--reconfigure']:[]),build,projected,...args],{env:environment});
 const jobs=Number(options.jobs||2);
 if(!Number.isInteger(jobs)||jobs<1||jobs>4)throw Error('Kosmic builds use one to four compiler jobs');
 run(ninja,['-C',build,'-j',String(jobs),'src/kosmickrisp/vulkan/'+pin.library],{env:environment});
 addLicense(path.join(build,'src/kosmickrisp'),'Mesa',path.join(source,'docs/license.rst'));
 addLicenses(path.join(build,'src/kosmickrisp'),'Sparse resources',sparseNotices);
 collectSoftFp64License(path.join(build, 'src/kosmickrisp'));
 return {library:path.join(build,'src/kosmickrisp/vulkan',pin.library),projected,nativeBuild:describeNativeTools({source,projected,build})};
}

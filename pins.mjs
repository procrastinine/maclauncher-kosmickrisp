import {execFileSync} from 'node:child_process';
export const mesaRevision=()=>execFileSync('git',['-C',import.meta.dirname,'rev-parse','HEAD:sources/mesa'],{encoding:'utf8'}).trim();

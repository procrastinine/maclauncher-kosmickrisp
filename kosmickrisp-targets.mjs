import {collectSoftFp64License} from './kosmickrisp-patches/soft-fp64.mjs';
import {mesaRevision} from './pins.mjs';
import {addLicense, addLicenses} from './package-licences.mjs';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {adaptedKosmicSources} from './kosmickrisp-source.mjs';
import {prepareSourceTree, writeChanged} from './source-files.mjs';

const pin = JSON.parse(fs.readFileSync(new URL('./kosmickrisp-source.json', import.meta.url)));
const targets = JSON.parse(fs.readFileSync(new URL('./kosmickrisp-targets.json', import.meta.url)));
pin.revision=mesaRevision();
const sha = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const read = (tool, args, options = {}) => execFileSync(tool, args, {encoding: 'utf8', ...options}).trim();
const run = (tool, args, options = {}) => execFileSync(tool, args, {stdio: 'inherit', ...options});
const sparseNotices = path.join(import.meta.dirname, 'kosmickrisp-patches/sparse/kosmickrisp-sparse-notices');
const toolFiles = {
    mesa_clc: 'src/compiler/clc/mesa_clc',
    vtn_bindgen2: 'src/compiler/spirv/vtn_bindgen2',
    kk_clc: 'src/kosmickrisp/clc/kk_clc',
};

function verifySource(source, input) {
    if (read('git', ['-C', source, 'rev-parse', 'HEAD']) !== input.revision) throw Error('Vulkan source revision differs from its pin');
}

// Generated opcode identities and the source that defines their serialization.
// Mesa has no separate NIR wire-format version; bind the serializer inputs.
const nirAbiGeneratedFiles = [
    'src/compiler/nir/nir_intrinsics.h',
    'src/compiler/nir/nir_intrinsics_indices.h',
    'src/compiler/nir/nir_opcodes.h',
];
const nirAbiSourceFiles = [
    'src/compiler/nir/nir.h',
    'src/compiler/nir/nir_defines.h',
    'src/compiler/nir/nir_serialize.c',
    'src/compiler/nir/nir_serialize.h',
    'src/compiler/nir/nir_shader_compiler_options.h',
    'src/compiler/shader_info.h',
    'src/compiler/shader_enums.h',
];
const nirAbiHash = value => crypto.createHash('sha256').update(JSON.stringify(value)).digest('hex');
const sameNames = (record, names) => JSON.stringify(Object.keys(record || {}).sort()) === JSON.stringify([...names].sort());

export function describeNirAbi({projected, build}) {
    const generated = Object.fromEntries(nirAbiGeneratedFiles.map(file => [file, sha(path.join(build, file))]));
    const serialization = Object.fromEntries(nirAbiSourceFiles.map(file => [file, sha(path.join(projected, file))]));
    const header = fs.readFileSync(path.join(build, nirAbiGeneratedFiles[0]), 'utf8');
    const intrinsicCount = (header.match(/^\s*nir_intrinsic_[A-Za-z0-9_]+,\s*$/gm) || []).length;
    if (!intrinsicCount) throw Error('Kosmic generated NIR intrinsic table is empty');
    const identity = {schemaVersion: 1, intrinsicCount, generated, serialization};
    return {...identity, sha256: nirAbiHash(identity)};
}

export function verifyNativeNirAbi({nativeBuild, native, target}) {
    const abi = native.nirAbi;
    const rebuild = 'Rebuild native tools from the target adapted Mesa sources.';
    if (native.schemaVersion !== 2 || abi?.schemaVersion !== 1)
        throw Error('Kosmic native tools lack NIR ABI metadata. ' + rebuild);
    if (!sameNames(abi.generated, nirAbiGeneratedFiles) || !sameNames(abi.serialization, nirAbiSourceFiles))
        throw Error('Kosmic native tool NIR ABI metadata is incomplete. ' + rebuild);
    const identity = {schemaVersion: abi.schemaVersion, intrinsicCount: abi.intrinsicCount,
        generated: abi.generated, serialization: abi.serialization};
    if (nirAbiHash(identity) !== abi.sha256)
        throw Error('Kosmic native tool NIR ABI metadata changed. ' + rebuild);
    for (const file of nirAbiGeneratedFiles) {
        if (!fs.existsSync(path.join(nativeBuild, file)) || sha(path.join(nativeBuild, file)) !== abi.generated[file])
            throw Error('Kosmic native tool generated NIR header differs from its manifest: ' + file + '. ' + rebuild);
    }
    if (target && abi.sha256 !== target.sha256)
        throw Error(`Kosmic native tool NIR ABI mismatch: native ${abi.intrinsicCount} intrinsics (${abi.sha256}), target ${target.intrinsicCount} (${target.sha256}). ${rebuild}`);
    return abi;
}

export function describeNativeTools({source, projected, build}) {
    verifySource(source, pin);
    const files = Object.fromEntries(Object.entries(toolFiles).map(([name, relative]) => [name, {
        path: relative, sha256: sha(path.join(build, relative)),
    }]));
    const record = {schemaVersion: 2, repository: pin.repository, revision: mesaRevision(),
        nirAbi: describeNirAbi({projected, build}), files};
    writeChanged(path.join(build, 'native-tools.json'), JSON.stringify(record, null, 2) + '\n');
    return build;
}

export function buildKosmicTargets(options, buildNative) {
    const {source, work, meson, ninja} = options;
    verifySource(source, pin);
    const architectures = options.architecture === 'universal' ? ['x64', 'arm64'] : [options.architecture];
    if (architectures.some(architecture => !['arm64', 'x64'].includes(architecture))) throw Error('Kosmic targets are arm64, x64 or universal');
    let nativeBuild = options.nativeBuild || options['native-build'];
    if (!nativeBuild) {
        const native = buildNative({...options, architecture: 'arm64', work: path.join(work, 'native')});
        nativeBuild = native.nativeBuild;
    }
    const native = JSON.parse(fs.readFileSync(path.join(nativeBuild, 'native-tools.json')));
    if (native.revision !== mesaRevision() || native.repository !== pin.repository) throw Error('Kosmic native compilers differ from their source pin');
    if (JSON.stringify(Object.keys(native.files).sort()) !== JSON.stringify(Object.keys(toolFiles).sort())) throw Error('Kosmic native compilers are incomplete');
    for (const [name, file] of Object.entries(native.files)) {
        if (file.path !== toolFiles[name] || file.sha256 !== sha(path.join(nativeBuild, file.path))) throw Error('Kosmic native compiler differs from its metadata');
    }
    verifyNativeNirAbi({nativeBuild, native});
    const projected = path.join(work, 'source');
    prepareSourceTree(projected, temporary => {
        fs.cpSync(source, temporary, {recursive: true, filter: file => path.basename(file) !== '.git'});
        for (const [file, content] of Object.entries(adaptedKosmicSources(source)))
            if (content === null) fs.rmSync(path.join(temporary, file), {force: true});
            else writeChanged(path.join(temporary, file), content);
    });
    const environment = {...process.env, MACOSX_DEPLOYMENT_TARGET: pin.minimumMacOS,
        ...(options.targetPkgConfigPath ? {PKG_CONFIG_PATH: options.targetPkgConfigPath, PKG_CONFIG_LIBDIR: options.targetPkgConfigPath} : {}),
        MESA_GIT_SHA1_OVERRIDE: mesaRevision(),
        PATH: [path.dirname(meson), ...Object.values(toolFiles).map(file => path.dirname(path.join(nativeBuild, file))), process.env.PATH].join(path.delimiter)};
    const outputs = [];
    const jobs = Number(options.jobs || 2);
    if (!Number.isInteger(jobs) || jobs < 1 || jobs > 4) throw Error('Kosmic builds use one to four compiler jobs');
    for (const architecture of architectures) {
        const cpu = architecture === 'x64' ? 'x86_64' : 'arm64';
        const cross = path.join(work, architecture === 'x64' ? 'cross.ini' : 'cross-arm64.ini');
        writeChanged(cross, `[binaries]
c = ['/usr/bin/clang', '-arch', '${cpu}']
cpp = ['/usr/bin/clang++', '-arch', '${cpu}']
objc = ['/usr/bin/clang', '-arch', '${cpu}']
objcpp = ['/usr/bin/clang++', '-arch', '${cpu}']
ar = '/usr/bin/ar'
strip = '/usr/bin/strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'darwin'
cpu_family = '${architecture === 'x64' ? 'x86_64' : 'aarch64'}'
cpu = '${cpu}'
endian = 'little'
[properties]
needs_exe_wrapper = false
[built-in options]
c_args = ['-mmacosx-version-min=${pin.minimumMacOS}']
cpp_args = ['-mmacosx-version-min=${pin.minimumMacOS}']
objc_args = ['-mmacosx-version-min=${pin.minimumMacOS}']
objcpp_args = ['-mmacosx-version-min=${pin.minimumMacOS}']
c_link_args = ['-arch', '${cpu}', '-mmacosx-version-min=${pin.minimumMacOS}']
cpp_link_args = ['-arch', '${cpu}', '-mmacosx-version-min=${pin.minimumMacOS}']
objc_link_args = ['-arch', '${cpu}', '-mmacosx-version-min=${pin.minimumMacOS}']
objcpp_link_args = ['-arch', '${cpu}', '-mmacosx-version-min=${pin.minimumMacOS}']
`);
        const build = path.join(work, architecture === 'x64' ? 'build' : 'build-arm64');
        const args = ['setup', ...(fs.existsSync(path.join(build, 'build.ninja')) ? ['--reconfigure'] : []), build, projected,
            '--cross-file', cross, '--buildtype=release', '--wrap-mode=nofallback', '-Dvulkan-drivers=kosmickrisp',
            '-Dgallium-drivers=[]', '-Dplatforms=macos', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled',
            '-Degl=disabled', '-Dglx=disabled', '-Dgbm=disabled', '-Dllvm=disabled', '-Dmesa-clc=system',
            '-Dprecomp-compiler=system', '-Dspirv-tools=enabled', '-Dzstd=enabled', '-Dxmlconfig=enabled',
            '-Dbuild-tests=false', '-Dvideo-codecs=[]'];
        run(meson, args, {env: environment});
        // Header generation is Python-only. Reject incompatible native tools
        // before any compiler can emit serialized NIR for this target.
        run(ninja, ['-C', build, '-j', String(jobs), ...nirAbiGeneratedFiles], {env: environment});
        verifyNativeNirAbi({nativeBuild, native, target: describeNirAbi({projected, build})});
        run(ninja, ['-C', build, '-j', String(jobs), 'src/kosmickrisp/vulkan/' + pin.library], {env: environment});
        addLicense(path.join(build, 'src/kosmickrisp'), 'Mesa', path.join(source, 'docs/license.rst'));
        addLicenses(path.join(build, 'src/kosmickrisp'), 'Sparse resources', sparseNotices);
 collectSoftFp64License(path.join(build, 'src/kosmickrisp'));
        outputs.push(path.join(build, 'src/kosmickrisp/vulkan', pin.library));
    }
    const library = outputs.length === 1 ? outputs[0] : path.join(work, pin.library);
    if (outputs.length > 1) run('/usr/bin/lipo', ['-create', ...outputs, '-output', library]);
    return {library, projected, nativeBuild, architecture: options.architecture};
}

export function buildKosmicLoader({source, headersSource, work, architecture = 'universal', jobs = 2, loaderRevision, headersRevision}) {
    verifySource(source, {revision: loaderRevision});
    verifySource(headersSource, {revision: headersRevision});
    if (!['arm64', 'x64', 'universal'].includes(architecture)) throw Error('Loader targets are arm64, x64 or universal');
    const headersBuild = path.join(work, 'headers-build'), headers = path.join(work, 'headers');
    run('cmake', ['-S', headersSource, '-B', headersBuild, '-DCMAKE_INSTALL_PREFIX=' + headers,
        '-DVULKAN_HEADERS_ENABLE_TESTS=OFF', '-DVULKAN_HEADERS_ENABLE_MODULE=OFF']);
    run('cmake', ['--install', headersBuild]);
    const architectures = architecture === 'universal' ? ['arm64', 'x64'] : [architecture];
    const loaders = [];
    for (const target of architectures) {
        const cpu = target === 'x64' ? 'x86_64' : 'arm64';
        const build = path.join(work, 'loader-build-' + target);
        // The loader selects its assembly from CMAKE_SYSTEM_PROCESSOR. A thin
        // configure keeps that target and the Mach-O compilation slice aligned.
        run('cmake', ['-S', source, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_SYSTEM_NAME=Darwin', '-DCMAKE_SYSTEM_PROCESSOR=' + cpu,
            '-DCMAKE_OSX_ARCHITECTURES=' + cpu, '-DCMAKE_OSX_DEPLOYMENT_TARGET=' + pin.minimumMacOS,
            '-DCMAKE_C_FLAGS=-ffile-prefix-map=' + source + '=/Vulkan-Loader -ffile-prefix-map=' + work + '=/build',
            '-DVulkanHeaders_DIR=' + path.join(headers, 'share/cmake/VulkanHeaders'), '-DBUILD_TESTS=OFF']);
        run('cmake', ['--build', build, '--target', 'vulkan', '--parallel', String(jobs)]);
        loaders.push(fs.realpathSync(path.join(build, 'loader/libvulkan.1.dylib')));
    }
    // Sealing reads each packaged library's licence texts from the licenses
    // folder beside the library's own directory.
    if (loaders.length === 1) {
        addLicenses(path.dirname(path.dirname(loaders[0])), 'Vulkan-Loader', source);
        return {loader: loaders[0]};
    }
    const universal = path.join(work, 'loader-universal'), loader = path.join(universal, 'loader/libvulkan.1.dylib');
    fs.mkdirSync(path.dirname(loader), {recursive: true});
    run('/usr/bin/lipo', ['-create', ...loaders, '-output', loader]);
    addLicenses(universal, 'Vulkan-Loader', source);
    return {loader};
}

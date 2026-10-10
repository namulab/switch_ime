import { copyFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const root = fileURLToPath(new URL('../', import.meta.url));
process.chdir(root);

export function parseTag(tag) {
  const match = /^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.exec(tag ?? '');
  if (!match || match[0] !== tag || match.slice(1).some(part => Number(part) > 65535)) {
    throw new Error('Release tag must be vMAJOR.MINOR.PATCH, with each component in 0..65535.');
  }
  return { tag, version: match.slice(1).join('.'), parts: match.slice(1) };
}

function run(executable, args, options = {}) {
  // Windows environment names are case insensitive. Remove duplicate casing.
  const env = Object.fromEntries(Object.entries(process.env).map(([key, value]) => [key.toUpperCase(), value]));
  const result = spawnSync(executable, args, { env, stdio: 'inherit', ...options });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${executable} failed (${result.status}).`);
  return result;
}

function main(command) {
  if (command === 'build') {
    run(process.execPath, ['tests/release.test.mjs']);
    const vswhere = join(process.env['ProgramFiles(x86)'], 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
    const found = run(vswhere, ['-latest', '-products', '*', '-version', '[18.0,19.0)',
      '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
    { encoding: 'utf8', stdio: 'pipe' }).stdout.trim();
    if (!found) throw new Error('Visual Studio 2026 with Visual C++ is required.');
    run(join(found, 'MSBuild', 'Current', 'Bin', 'MSBuild.exe'), ['switch_ime.sln',
      '/m:1', '/nr:false', '/nologo', '/verbosity:minimal', '/p:Configuration=Release', '/p:Platform=x64']);
    run(join(root, 'bin', 'x64', 'Release', 'tests', 'switch_ime_tests.exe'), []);
    return;
  }

  const { tag, version, parts } = parseTag(process.env.RELEASE_TAG);
  const packageFolder = join(root, 'bin', 'release-package', version);
  const archive = join(root, 'bin', 'release-package', `switch_ime-${version}-windows-x64.zip`);
  if (command === 'prepare') {
    writeFileSync('version.h', `#pragma once\n// Generated from release tag ${tag}.\n` +
      parts.map((part, index) => `#define SWITCH_IME_VERSION_${['MAJOR', 'MINOR', 'PATCH'][index]} ${part}\n`).join('') +
      `#define SWITCH_IME_VERSION_NUMBER ${parts.join(',')},0\n` +
      `#define SWITCH_IME_VERSION_STRING "${version}"\n`);
  } else if (command === 'package') {
    mkdirSync(packageFolder, { recursive: true });
    copyFileSync('bin/x64/Release/switch_ime.exe', join(packageFolder, 'switch_ime.exe'));
    for (const file of ['LICENSE', 'README.md']) copyFileSync(file, join(packageFolder, file));
    run('tar.exe', ['--format=zip', '-cf', archive, '-C', packageFolder,
      'switch_ime.exe', 'LICENSE', 'README.md']);
  } else if (command === 'publish') {
    let existing = false;
    try {
      run('gh.exe', ['release', 'view', tag], { stdio: 'pipe' });
      existing = true;
    } catch (error) {
      if (error.code) throw error; // Missing CLI or process-launch error.
    }
    if (existing) {
      // GitHub's New Release UI may have created the tag and release together.
      // Keep its notes; do not overwrite an already uploaded asset.
      run('gh.exe', ['release', 'upload', tag, archive]);
      run('gh.exe', ['release', 'edit', tag, '--draft=false']);
    } else {
      run('gh.exe', ['release', 'create', tag, archive, '--verify-tag', '--title', tag, '--generate-notes']);
    }
  } else {
    throw new Error('Usage: node ci/release.mjs prepare|build|package|publish');
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main(process.argv[2]);

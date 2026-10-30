/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import {spawnSync} from 'node:child_process';
import {chmodSync, copyFileSync, existsSync, mkdirSync} from 'node:fs';
import * as path from 'node:path';
import {fileURLToPath} from 'node:url';

/** Package the selected native build, preserving supplied copies for other hosts. */
export function stageFlowTool() {
  const directory = path.dirname(fileURLToPath(import.meta.url));
  const root = path.resolve(directory, '..');
  const os = process.platform === 'darwin' ? 'macos' : process.platform === 'win32' ? 'windows' : 'linux';
  const arch = process.arch === 'arm64' ? 'aarch64' : 'x86_64';
  const name = process.platform === 'win32' ? 'a11-flow.exe' : 'a11-flow';
  let binary = process.env.A11_FLOW_TOOL_BINARY;
  if (!binary) {
    const build = process.env.A11_FLOW_BUILD_DIR ??
      ['cmake-build-debug', 'cmake-build-release', 'build/debug', 'build/release']
        .map((candidate) => path.join(root, candidate))
        .find((candidate) => existsSync(path.join(candidate, 'CMakeCache.txt')));
    if (!build) {
      throw new Error('Configure CMake first, or set A11_FLOW_BUILD_DIR or A11_FLOW_TOOL_BINARY.');
    }
    const result = spawnSync('cmake', ['--build', build, '--target', 'a11_flow_tool', '--parallel', '8'], {stdio: 'inherit'});
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error('The native Flow language-service build failed.');
    binary = path.join(build, 'cpp', name);
  }
  const destination = path.join(directory, 'bin', `${os}-${arch}`, name);
  mkdirSync(path.dirname(destination), {recursive: true});
  if (path.resolve(binary) !== destination) copyFileSync(binary, destination);
  chmodSync(destination, 0o755);
  console.log(`bundled ${destination}`);
}

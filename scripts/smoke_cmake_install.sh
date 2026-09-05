#!/usr/bin/env bash
# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# Defaults to the preset-configured native tree (see BUILDING.md); pass an
# explicit path to smoke-test a different build directory.
build_dir=${1:-"${root}/cmake-build-debug"}
work=$(mktemp -d "${TMPDIR:-/tmp}/a11-install-smoke.XXXXXX")
trap 'rm -rf "${work}"' EXIT

cmake --install "${build_dir}" --prefix "${work}/prefix"
cmake -S "${root}/cpp/tests/install_smoke" -B "${work}/build" -G Ninja \
  -DCMAKE_PREFIX_PATH="${work}/prefix"
cmake --build "${work}/build"
"${work}/build/a11_install_smoke"

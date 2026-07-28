#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"${root}/build"}
work=$(mktemp -d "${TMPDIR:-/tmp}/a11-install-smoke.XXXXXX")
trap 'rm -rf "${work}"' EXIT

cmake --install "${build_dir}" --prefix "${work}/prefix"
cmake -S "${root}/cpp/tests/install_smoke" -B "${work}/build" -G Ninja \
  -DCMAKE_PREFIX_PATH="${work}/prefix"
cmake --build "${work}/build"
"${work}/build/a11_install_smoke"

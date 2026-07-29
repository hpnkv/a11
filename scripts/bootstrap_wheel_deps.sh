#!/usr/bin/env bash
set -euo pipefail

prefix=${A11_DEPS_PREFIX:?A11_DEPS_PREFIX must name an installation prefix}
arch=${A11_WHEEL_ARCH:-$(uname -m)}
host_os=$(uname -s)
deployment_tag=
if [[ "${host_os}" == Darwin ]]; then
  export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
  deployment_tag="-macos-${MACOSX_DEPLOYMENT_TARGET}"
fi
stamp="${prefix}/.a11-wheel-deps-v6-${arch}${deployment_tag}"
if [[ -f "${stamp}" ]]; then
  exit 0
fi

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
work=$(mktemp -d "${TMPDIR:-/tmp}/a11-wheel-deps.XXXXXX")
trap 'rm -rf "${work}"' EXIT
mkdir -p "${prefix}"

download_and_extract() {
  local url=$1
  local archive=$2
  curl --fail --location --retry 5 --output "${work}/${archive}" "${url}"
  tar -xf "${work}/${archive}" -C "${work}"
}

cmake_arch_args=()
boost_arch_args=()
openssl_target=
case "${host_os}:${arch}" in
  Darwin:x86_64|Darwin:amd64)
    cmake_arch_args=(-DCMAKE_OSX_ARCHITECTURES=x86_64
                     -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}")
    boost_arch_args=(toolset=clang target-os=darwin architecture=x86
                     'cxxflags=-arch x86_64' 'linkflags=-arch x86_64')
    openssl_target=darwin64-x86_64-cc
    ;;
  Darwin:arm64|Darwin:aarch64)
    cmake_arch_args=(-DCMAKE_OSX_ARCHITECTURES=arm64
                     -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}")
    boost_arch_args=(toolset=clang target-os=darwin architecture=arm
                     'cxxflags=-arch arm64' 'linkflags=-arch arm64')
    openssl_target=darwin64-arm64-cc
    ;;
  Linux:x86_64|Linux:amd64)
    boost_arch_args=(toolset=gcc target-os=linux architecture=x86
                     cxxflags=-fPIC cflags=-fPIC)
    openssl_target=linux-x86_64
    ;;
  Linux:aarch64|Linux:arm64)
    boost_arch_args=(toolset=gcc target-os=linux architecture=arm
                     cxxflags=-fPIC cflags=-fPIC)
    openssl_target=linux-aarch64
    ;;
  *)
    echo "Unsupported wheel dependency target: ${host_os} ${arch}" >&2
    exit 2
    ;;
esac

install_linux_package() {
  local package=$1
  if command -v dnf >/dev/null 2>&1; then
    dnf install -y "${package}"
  elif command -v yum >/dev/null 2>&1; then
    yum install -y "${package}"
  else
    echo "${package} is missing and no dnf/yum is available to install it" >&2
    exit 1
  fi
}

if [[ "${host_os}" == Linux ]]; then
  # manylinux_2_28's perl-interpreter package omits core modules like
  # IPC::Cmd that OpenSSL's Configure requires; the full "perl" metapackage
  # isn't installed by default the way it was on older manylinux images.
  if ! perl -MIPC::Cmd -e 1 >/dev/null 2>&1; then
    install_linux_package perl-IPC-Cmd
  fi
  # The nlohmann-json, nghttp2, and uvw builds below select Ninja explicitly,
  # but manylinux_2_28 doesn't ship it by default.
  if ! command -v ninja >/dev/null 2>&1; then
    install_linux_package ninja-build
  fi
fi

download_and_extract \
  "https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz" \
  openssl.tar.gz
(
  cd "${work}/openssl-3.5.2"
  ./Configure "${openssl_target}" no-shared no-tests no-module \
    --prefix="${prefix}" --libdir=lib
  make -j "${jobs}"
  make install_sw
)

download_and_extract \
  "https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2" \
  boost.tar.bz2
(
  cd "${work}/boost_1_90_0"
  ./bootstrap.sh --prefix="${prefix}" \
    --with-libraries=atomic,chrono,container,context,date_time,fiber,filesystem,thread
  ./b2 -j "${jobs}" "${boost_arch_args[@]}" cxxstd=20 variant=release \
    link=static runtime-link=shared threading=multi install
)

download_and_extract \
  "https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz" \
  nlohmann-json.tar.gz
cmake -S "${work}/json-3.12.0" -B "${work}/json-build" \
  -G Ninja -DJSON_BuildTests=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${prefix}" "${cmake_arch_args[@]}"
cmake --build "${work}/json-build" --target install -j "${jobs}"

download_and_extract \
  "https://github.com/nghttp2/nghttp2/archive/refs/tags/v1.69.0.tar.gz" \
  nghttp2.tar.gz
cmake -S "${work}/nghttp2-1.69.0" -B "${work}/nghttp2-build" \
  -G Ninja -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_STATIC_LIBS=ON -DBUILD_TESTING=OFF -DENABLE_WERROR=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_INSTALL_LIBDIR=lib \
  "${cmake_arch_args[@]}"
cmake --build "${work}/nghttp2-build" --target install -j "${jobs}"

download_and_extract \
  "https://github.com/skypjack/uvw/archive/refs/tags/v3.4.0_libuv_v1.48.tar.gz" \
  uvw.tar.gz
cmake -S "${work}/uvw-3.4.0_libuv_v1.48" -B "${work}/uvw-build" \
  -G Ninja -DBUILD_UVW_LIBS=ON -DBUILD_UVW_SHARED_LIB=OFF \
  -DFETCH_LIBUV=ON -DUSE_LIBCPP=OFF -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_INSTALL_LIBDIR=lib \
  "${cmake_arch_args[@]}"
cmake --build "${work}/uvw-build" --target install -j "${jobs}"

# uvw's FETCH_LIBUV install copies libuv's public headers to
# include/uvw/uv/include, but the generated uvw CMake package declares the
# libuv (uvw::uv_a) include directory as ${prefix}/include. Flatten the bundled
# libuv headers into the include root so <uv.h> resolves as the package config
# promises. (The pinned uvw tag makes this nested layout stable.)
uv_bundled_include="${prefix}/include/uvw/uv/include"
if [[ -f "${uv_bundled_include}/uv.h" ]]; then
  cp -R "${uv_bundled_include}/." "${prefix}/include/"
fi

touch "${stamp}"

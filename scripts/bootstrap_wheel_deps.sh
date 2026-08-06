#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
prefix=${A11_DEPS_PREFIX:?A11_DEPS_PREFIX must name an installation prefix}
arch=${A11_WHEEL_ARCH:-$(uname -m)}
host_os=$(uname -s)
deployment_tag=
if [[ "${host_os}" == Darwin ]]; then
  # 14.4 is the minimum for os_sync_wait_on_address, which the Boost.Fiber futex
  # spinlock (selected below) relies on. Lowering it disables futex on macOS.
  export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-14.4}
  deployment_tag="-macos-${MACOSX_DEPLOYMENT_TARGET}"
fi

# Boost.Fiber spinlock policy. Default to the adaptive TTAS futex spinlock;
# A11_FIBER_SPINLOCK may switch it to the plain TTAS futex spinlock. The same
# value MUST be used when compiling A11 (cpp/CMakeLists.txt reads
# A11_FIBER_SPINLOCK identically), because Boost.Fiber selects the spinlock in
# headers -- a mismatch changes boost::fibers::mutex's layout across the deps
# library and the extension.
fiber_spinlock=${A11_FIBER_SPINLOCK:-BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX}
case "${fiber_spinlock}" in
  BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX | BOOST_FIBERS_SPINLOCK_TTAS_FUTEX) ;;
  *)
    echo "A11_FIBER_SPINLOCK must be BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX" \
         "or BOOST_FIBERS_SPINLOCK_TTAS_FUTEX (got '${fiber_spinlock}')" >&2
    exit 2
    ;;
esac

# The spinlock macro only changes the build on macOS (cpp/CMakeLists.txt applies
# it under APPLE), so it is part of the cache key there. Folding it into the
# stamp -- like the deployment target -- means switching A11_FIBER_SPINLOCK
# rebuilds Boost against the new spinlock instead of silently reusing a prefix
# whose boost::fibers::mutex layout no longer matches the extension.
spinlock_tag=
if [[ "${host_os}" == Darwin ]]; then
  spinlock_tag="-${fiber_spinlock}"
fi

sanitize_tag=
if [[ -n "${A11_DEPS_SANITIZE:-}" ]]; then
  sanitize_tag="-sanitize-${A11_DEPS_SANITIZE//[^a-zA-Z0-9]/}"
fi
stamp="${prefix}/.a11-wheel-deps-v11-${arch}${deployment_tag}${spinlock_tag}${sanitize_tag}"
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

# These arrays stay empty on some hosts (no arch flags on Linux, no ALSA on
# macOS). Expand them as ${arr[@]+"${arr[@]}"} rather than "${arr[@]}": the
# bash 3.2 that macOS ships treats an empty array as unbound under `set -u`.
cmake_arch_args=()
boost_arch_args=()
openssl_target=
case "${host_os}:${arch}" in
  Darwin:x86_64|Darwin:amd64)
    cmake_arch_args=(-DCMAKE_OSX_ARCHITECTURES=x86_64
                     -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}")
    # Pin every property Boost.Context keys its assembly sources on
    # (architecture/address-model/abi/binary-format). Passing only
    # architecture=x86 lets b2 inherit the host's abi -- aapcs on an Apple
    # silicon runner -- and the impossible x86+aapcs pair makes Boost.Context
    # silently skip its jump/make/ontop_fcontext assembly, yielding a
    # libboost_context.a that link-fails at dlopen (missing _jump_fcontext).
    boost_arch_args=(toolset=clang target-os=darwin
                     architecture=x86 address-model=64 abi=sysv
                     binary-format=mach-o
                     "define=${fiber_spinlock}"
                     'cxxflags=-arch x86_64' 'linkflags=-arch x86_64')
    openssl_target=darwin64-x86_64-cc
    ;;
  Darwin:arm64|Darwin:aarch64)
    cmake_arch_args=(-DCMAKE_OSX_ARCHITECTURES=arm64
                     -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}")
    boost_arch_args=(toolset=clang target-os=darwin
                     architecture=arm address-model=64 abi=aapcs
                     binary-format=mach-o
                     "define=${fiber_spinlock}"
                     'cxxflags=-arch arm64' 'linkflags=-arch arm64')
    openssl_target=darwin64-arm64-cc
    ;;
  Linux:x86_64|Linux:amd64)
    boost_arch_args=(toolset=gcc target-os=linux
                     architecture=x86 address-model=64 abi=sysv
                     binary-format=elf
                     cxxflags=-fPIC cflags=-fPIC)
    openssl_target=linux-x86_64
    ;;
  Linux:aarch64|Linux:arm64)
    boost_arch_args=(toolset=gcc target-os=linux
                     architecture=arm address-model=64 abi=aapcs
                     binary-format=elf
                     cxxflags=-fPIC cflags=-fPIC)
    openssl_target=linux-aarch64
    ;;
  *)
    echo "Unsupported wheel dependency target: ${host_os} ${arch}" >&2
    exit 2
    ;;
esac

# Opt-in sanitizer support for the prefix's Boost. Set A11_DEPS_SANITIZE=address
# (into a *separate* A11_DEPS_PREFIX from your normal one) to build Boost.Context
# with BOOST_USE_ASAN, which emits __sanitizer_start/finish_switch_fiber around
# fiber stack switches. Without it, ASan treats every write to a boost.context
# fiber stack (a malloc'd block) as a heap-buffer-overflow and nested-aborts its
# own report, making ASan useless on A11's fiber-heavy paths. The switch symbols
# resolve from the ASan runtime the extension is preloaded with, so Boost itself
# need not be compiled with -fsanitize=address.
case "${A11_DEPS_SANITIZE:-}" in
  *address*|*undefined*)
    echo "A11_DEPS_SANITIZE=${A11_DEPS_SANITIZE}: building Boost with BOOST_USE_ASAN" >&2
    boost_arch_args+=(define=BOOST_USE_ASAN)
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

# libcurl backs the native OTLP/HTTP span exporter. Built static against the
# OpenSSL above, with optional protocols disabled, so the wheel gains no new
# dynamic dependency (verified by scripts/audit_wheel.py). HTTP/1.1 is
# sufficient for OTLP-compatible endpoints such as Langfuse.
download_and_extract \
  "https://github.com/curl/curl/releases/download/curl-8_11_0/curl-8.11.0.tar.gz" \
  curl.tar.gz
cmake -S "${work}/curl-8.11.0" -B "${work}/curl-build" \
  -G Ninja -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF \
  -DBUILD_TESTING=OFF -DCURL_USE_OPENSSL=ON \
  -DOPENSSL_ROOT_DIR="${prefix}" -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF \
  -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF \
  -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_DISABLE_LDAP=ON \
  -DCURL_DISABLE_LDAPS=ON -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
  -DENABLE_CURL_MANUAL=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_INSTALL_LIBDIR=lib \
  ${cmake_arch_args[@]+"${cmake_arch_args[@]}"}
cmake --build "${work}/curl-build" --target install -j "${jobs}"

download_and_extract \
  "https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2" \
  boost.tar.bz2
(
  cd "${work}/boost_1_90_0"
  # Backport Boost.Fiber's macOS futex fast path (os_sync_wait_on_address). The
  # added code is guarded by BOOST_OS_MACOS and a >=14.4 deployment target, so
  # applying it on every platform is harmless. Fail loudly if it no longer
  # applies (e.g. after a Boost bump) rather than silently building without it.
  patch -p1 < "${script_dir}/patches/boost-fiber-macos-futex.patch"
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
  -DCMAKE_INSTALL_PREFIX="${prefix}" ${cmake_arch_args[@]+"${cmake_arch_args[@]}"}
cmake --build "${work}/json-build" --target install -j "${jobs}"

download_and_extract \
  "https://github.com/nghttp2/nghttp2/archive/refs/tags/v1.69.0.tar.gz" \
  nghttp2.tar.gz
cmake -S "${work}/nghttp2-1.69.0" -B "${work}/nghttp2-build" \
  -G Ninja -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_STATIC_LIBS=ON -DBUILD_TESTING=OFF -DENABLE_WERROR=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_INSTALL_LIBDIR=lib \
  ${cmake_arch_args[@]+"${cmake_arch_args[@]}"}
cmake --build "${work}/nghttp2-build" --target install -j "${jobs}"

download_and_extract \
  "https://github.com/redis/hiredis/archive/refs/tags/v1.3.0.tar.gz" \
  hiredis.tar.gz
cmake -S "${work}/hiredis-1.3.0" -B "${work}/hiredis-build" \
  -G Ninja -DBUILD_SHARED_LIBS=OFF -DDISABLE_TESTS=ON \
  -DENABLE_SSL=OFF -DENABLE_SSL_TESTS=OFF -DENABLE_ASYNC_TESTS=OFF \
  -DENABLE_EXAMPLES=OFF -DENABLE_NUGET=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_INSTALL_LIBDIR=lib \
  ${cmake_arch_args[@]+"${cmake_arch_args[@]}"}
cmake --build "${work}/hiredis-build" --target install -j "${jobs}"

download_and_extract \
  "https://github.com/skypjack/uvw/archive/refs/tags/v3.4.0_libuv_v1.48.tar.gz" \
  uvw.tar.gz
cmake -S "${work}/uvw-3.4.0_libuv_v1.48" -B "${work}/uvw-build" \
  -G Ninja -DBUILD_UVW_LIBS=ON -DBUILD_UVW_SHARED_LIB=OFF \
  -DFETCH_LIBUV=ON -DUSE_LIBCPP=OFF -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_INSTALL_LIBDIR=lib \
  ${cmake_arch_args[@]+"${cmake_arch_args[@]}"}
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

# PortAudio's Linux backend uses ALSA. Build that system-facing library as a
# static archive so the Python extension does not acquire a libasound.so loader
# dependency (scripts/audit_wheel.py intentionally rejects it). macOS uses the
# built-in Core Audio frameworks and needs no corresponding dependency.
alsa_cmake_args=()
if [[ "${host_os}" == Linux ]]; then
  download_and_extract \
    "https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.14.tar.bz2" \
    alsa-lib.tar.bz2
  (
    cd "${work}/alsa-lib-1.2.14"
    CFLAGS="-fPIC ${CFLAGS:-}" ./configure \
      --prefix="${prefix}" --libdir="${prefix}/lib" \
      --disable-shared --enable-static --disable-python
    make -j "${jobs}"
    make install
  )
  alsa_cmake_args=(
    -DALSA_LIBRARY="${prefix}/lib/libasound.a"
    -DALSA_INCLUDE_DIR="${prefix}/include")
fi

# Capture and ASR are default SDK components, so build their native libraries
# once per wheel architecture in the shared prefix instead of recompiling them
# independently for CPython 3.11 through 3.14.
download_and_extract \
  "https://github.com/PortAudio/portaudio/archive/refs/tags/v19.7.0.tar.gz" \
  portaudio.tar.gz
cmake -S "${work}/portaudio-19.7.0" -B "${work}/portaudio-build" \
  -G Ninja -DPA_BUILD_SHARED=OFF -DPA_BUILD_STATIC=ON \
  -DPA_BUILD_TESTS=OFF -DPA_BUILD_EXAMPLES=OFF \
  -DPA_ENABLE_DEBUG_OUTPUT=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_INSTALL_LIBDIR=lib \
  ${alsa_cmake_args[@]+"${alsa_cmake_args[@]}"} ${cmake_arch_args[@]+"${cmake_arch_args[@]}"}
cmake --build "${work}/portaudio-build" --target install -j "${jobs}"

whisper_platform_args=(-DGGML_BLAS=OFF -DGGML_METAL=OFF)
if [[ "${host_os}" == Darwin ]]; then
  whisper_platform_args=(
    -DGGML_ACCELERATE=ON -DGGML_BLAS=ON -DGGML_BLAS_VENDOR=Apple
    -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON
    -DGGML_METAL_NDEBUG=ON)
fi
download_and_extract \
  "https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.2.tar.gz" \
  whisper.tar.gz
cmake -S "${work}/whisper.cpp-1.9.2" -B "${work}/whisper-build" \
  -G Ninja -DBUILD_SHARED_LIBS=OFF -DWHISPER_BUILD_TESTS=OFF \
  -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_SERVER=OFF \
  -DWHISPER_CURL=OFF -DWHISPER_COREML=OFF -DWHISPER_OPENVINO=OFF \
  -DGGML_BUILD_TESTS=OFF -DGGML_BUILD_EXAMPLES=OFF -DGGML_NATIVE=OFF \
  -DGGML_OPENMP=OFF -DGGML_BACKEND_DL=OFF -DGGML_CCACHE=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_INSTALL_LIBDIR=lib \
  "${whisper_platform_args[@]}" ${cmake_arch_args[@]+"${cmake_arch_args[@]}"}
cmake --build "${work}/whisper-build" --target install -j "${jobs}"

touch "${stamp}"

# Building A11

This is the contributor build guide: the development environment, the editable
Python workflow, the native C++ build, how to test both layers, and the release
wheel matrix. If you just want to *use* A11, see the
[README](README.md) (`pip install "a11-kit[llm]"`) and the
[documentation](https://hpnkv.github.io/a11); the README also has a minimal,
self-contained recipe for building and linking the C++ runtime on its own.

A11 is a Python action and streaming runtime backed by a C++20 implementation.
The Python package in `a11/` is the public, language-native API; `cpp/a11/`
contains the native implementation, and `cpp/thread/` contains its cooperative
fiber runtime.

Run all commands from the repository root.

## Python and native architecture

Runtime state lives in C++. Public classes such as `Status`, `Time`,
`Duration`, wire records, `ChunkStoreReader`, `ChunkStoreWriter`, `AsyncNode`,
`NodeMap`, `Action`, `ActionRegistry`, and `Session` are the bound native class
objects, not parallel Python implementations. Thin modules under `a11/` add
Python protocols: awaitables and async iteration, subclass-friendly callback
adaptation, object serialization, and Pydantic-compatible validation, JSON,
copy, and schema methods.

`LocalChunkStore` is a deliberately thin forwarding adapter around the native
in-memory store. Keeping that virtual Python boundary lets applications
subclass it for custom stores and fault injection while all storage and
synchronization remain native. Python serializer/deserializer callbacks remain
in `a11.data.serialization`; the chunks they consume and produce are native
binary values.

Import from the ordinary public modules (`a11`, `a11.actions`,
`a11.data.types`, and so on). Synchronous and asynchronous native failures
cross the boundary as
`a11.status.StatusException` with structured details preserved.

## Prerequisites

The local build needs:

- Python 3.11 or newer and [uv](https://docs.astral.sh/uv/);
- CMake 3.28 or newer, Ninja, a C++20 compiler, Git, `pkg-config`, Curl, Make,
  and Perl;
- static Boost.Context/Fiber/Thread, OpenSSL, nghttp2, nlohmann-json, and uvw;
- GoogleTest for the native test build;
- pybind11 if `A11_BUILD_PYTHON=ON` in a direct CMake build.

CMake fetches the pinned Abseil version and, when it is not installed,
libdatachannel. The latter is only used for WebRTC.

On macOS, a suitable Homebrew setup is:

```sh
brew install \
  boost cmake googletest libnghttp2 ninja nlohmann-json openssl@3 \
  pkg-config pybind11 uv uvw
```

Linux package names vary by distribution. The project can build the main
static dependencies reproducibly instead of relying on system packages:

```sh
export A11_WHEEL_ARCH="$(uname -m)"
export A11_DEPS_PREFIX="/tmp/a11-editable-deps-${A11_WHEEL_ARCH}"
export CMAKE_PREFIX_PATH="${A11_DEPS_PREFIX}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
export OPENSSL_ROOT_DIR="${A11_DEPS_PREFIX}"
export PKG_CONFIG_PATH="${A11_DEPS_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

# Keep this set on macOS so the dependencies and extension have the same
# deployment target. 14.4 is the minimum for the Boost.Fiber futex spinlock
# (os_sync_wait_on_address); it must match between this bootstrap and the CMake
# build below.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.4}"

scripts/bootstrap_wheel_deps.sh
```

That script builds Boost, OpenSSL, nghttp2, nlohmann-json, and uvw. GoogleTest
must still be installed separately for `BUILD_TESTING=ON`. Keep the exported
paths in the shell used for CMake, `uv sync`, and editable rebuilds.

On macOS the script also applies `scripts/patches/boost-fiber-macos-futex.patch`
to enable Boost.Fiber's futex spinlock, and builds Boost with
`BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX`. `cpp/CMakeLists.txt` defines the
same macro for the isolated static-deps build (`A11_REQUIRE_STATIC_DEPS=ON`), so
the two agree — this is why the deployment target and `A11_FIBER_SPINLOCK` (see
below) must be identical in the bootstrap shell and the CMake build. Set
`A11_FIBER_SPINLOCK=BOOST_FIBERS_SPINLOCK_TTAS_FUTEX` in both to use the plain
(non-adaptive) futex spinlock instead of the default adaptive one.

## Editable Python build

Create an environment and install the project in editable mode:

```sh
uv venv --python 3.12

# Point the build at the bootstrapped static-deps prefix. The dependency
# discovery paths must go through CMAKE_ARGS, not the CMAKE_PREFIX_PATH /
# OPENSSL_ROOT_DIR environment variables the Prerequisites block exports:
# uv builds the extension in an isolated environment and overwrites
# CMAKE_PREFIX_PATH with its own build venv, so an exported value never reaches
# the static-deps configure and Boost is not found. Values passed as -D flags in
# CMAKE_ARGS land on the cmake command line and survive.
export CMAKE_ARGS="-DA11_REQUIRE_STATIC_DEPS=ON -DA11_FETCH_MISSING_DEPS=ON \
  -DCMAKE_PREFIX_PATH=${A11_DEPS_PREFIX} -DOPENSSL_ROOT_DIR=${A11_DEPS_PREFIX}"
uv sync --locked --group dev
```

`PKG_CONFIG_PATH` (exported in the Prerequisites block) is *not* overwritten by
uv, so nghttp2, curl, and uvw are still discovered through it. If a system or
Homebrew Boost/OpenSSL is present and you omit the two `-D` flags above, the
configure either fails to find Boost or silently links the wrong copy; keep them
in `CMAKE_ARGS` whenever `A11_REQUIRE_STATIC_DEPS=ON`.

Any supported Python from 3.11 onward can replace 3.12. `uv sync` maps the
Python modules directly to `a11/`, while scikit-build compiles and installs the
ABI-specific native modules into `.venv`. Consequently:

- edits to `.py` files are visible to the next Python process immediately;
- edits to C++ files are not visible until the editable extension is rebuilt.

For an ordinary C++ edit, rebuild and install the extension with the editable
loader hook:

```sh
.venv/bin/python -c \
  'import a11._native as native; native.__loader__.rebuild()'
```

Run tests in a **new process** after this command. The rebuild process imports
the old extension to reach its loader, and a loaded native module cannot be
replaced within that same process.

The hook only runs `cmake --build` and `cmake --install` against the build tree
that `uv sync` already configured under `build/editable/<wheel-tag>`; it does
not configure that tree. When the tree is absent — a fresh clone before the
first `uv sync`, or after `build/` was deleted or cleaned (see the note in
[Native C++ build](#native-c-build)) — the rebuild fails with:

```
Error: not a CMake build directory (missing CMakeCache.txt)
```

Recreating the editable build tree fixes this. The same command is also required
after changing `pyproject.toml`, the CMake install layout, build options, or the
Python interpreter, since each invalidates the configured tree or the editable
metadata:

```sh
uv sync --locked --group dev --reinstall-package a11-kit
```

`--reinstall-package` takes the distribution name (`a11-kit`, from
`pyproject.toml`'s `[project] name`), not the import name (`a11`). Passing
`a11` matches nothing, so `uv sync` silently reports `Checked N packages` and
does **not** recreate the build tree — the next `native.__loader__.rebuild()`
still fails with the same `CMakeCache.txt` error. There is no warning when this
happens, so always double-check with:

```sh
ls build/editable/*/CMakeCache.txt
```

The active files can be checked without relying on shell activation:

```sh
.venv/bin/python - <<'PY'
from pathlib import Path
import a11
import a11._native

root = Path.cwd().resolve()
python_source = Path(a11.__file__).resolve()
native_module = Path(a11._native.__file__).resolve()

print("Python source:", python_source)
print("native module:", native_module)
assert python_source.is_relative_to(root)
PY
```

It is expected for the editable native module to live under `.venv`; the
explicit rebuild above installs the current C++ output there.

## Native C++ build

Use a separate Debug tree for C++ tests. Turning the Python module off here
keeps this build independent of the editable extension; the editable workflow
above compiles and tests the binding layer separately.

This Debug tree and the editable extension's tree both live under `build/`
(`build/` itself and `build/editable/<wheel-tag>`, respectively). Removing or
cleaning `build/` — for example to reconfigure this Debug tree from scratch —
therefore also destroys the editable tree, so the next `native.__loader__.rebuild()`
reports a missing `CMakeCache.txt`. Recreate the editable tree with
`uv sync --locked --group dev --reinstall-package a11-kit` afterward (use the
distribution name `a11-kit`, not the import name `a11` — see the note in
[Editable Python build](#editable-python-build)).

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DA11_BUILD_PYTHON=OFF \
  -DA11_REQUIRE_STATIC_DEPS=ON \
  -DA11_FETCH_MISSING_DEPS=ON

cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

If `CMAKE_PREFIX_PATH` was not exported, pass the dependency prefix explicitly
while configuring:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="${A11_DEPS_PREFIX}" \
  -DOPENSSL_ROOT_DIR="${A11_DEPS_PREFIX}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DA11_BUILD_PYTHON=OFF
```

The native suite consists of `thread_test`, which exercises the cooperative
runtime, and `a11_core_test`, which aggregates the A11 component tests. A
focused test can be run directly with a GoogleTest filter, for example:

```sh
build/cpp/thread/thread_test \
  --gtest_filter='ThreadSelectTest.*:ThreadFiberTest.*'
```

To verify that the installed CMake package can be consumed outside the source
tree, run:

```sh
scripts/smoke_cmake_install.sh build
```

When changing toolchains or dependency prefixes, rerun the full configure
command above with `cmake --fresh` in place of `cmake`. A normal source edit
only needs `cmake --build`.

## Developing in CLion (or another CMake IDE)

The IDE drives raw CMake, so it needs the same discovery settings the wheel and
editable flows inject — otherwise the static-deps configure fails with
`Boost_DIR-NOTFOUND` (the isolated build is pinned to the prefix) or, on macOS,
`"futex not supported"` (the deployment target is unset). `CMakePresets.json` at
the repository root encodes all of it, so CLion, VS Code, and the command line
share one configuration.

1. **Bootstrap the dependency prefix once** (see [Prerequisites](#prerequisites))
   and note the `A11_DEPS_PREFIX` you used. Prefer a persistent location over
   `/tmp` (which is cleared on reboot), e.g.
   `export A11_DEPS_PREFIX="$HOME/.cache/a11-deps/$(uname -m)"`.

2. **Let the IDE see `A11_DEPS_PREFIX`.** A GUI-launched CLion does *not* inherit
   your shell, so pick one:
   - launch it from a terminal that has `A11_DEPS_PREFIX` exported; or
   - add `A11_DEPS_PREFIX` to the CMake profile's *Environment* field
     (Settings → Build, Execution, Deployment → CMake); or
   - create a git-ignored `CMakeUserPresets.json` that hard-codes the absolute
     paths, for example:

     ```json
     {
       "version": 6,
       "configurePresets": [
         {
           "name": "debug-local",
           "inherits": "debug",
           "cacheVariables": {
             "CMAKE_PREFIX_PATH": "/Users/me/.cache/a11-deps/arm64",
             "OPENSSL_ROOT_DIR": "/Users/me/.cache/a11-deps/arm64"
           },
           "environment": {
             "PKG_CONFIG_PATH": "/Users/me/.cache/a11-deps/arm64/lib/pkgconfig"
           }
         }
       ]
     }
     ```

3. **Select a preset.** CLion detects `CMakePresets.json` and lists `debug` and
   `release` under Settings → CMake (enable them); the command-line equivalents
   are `cmake --preset debug`, `cmake --build --preset debug`, and
   `ctest --preset debug`.

The presets set `A11_REQUIRE_STATIC_DEPS=ON`, the macOS deployment target and
`A11_FIBER_SPINLOCK` to `14.4`/`BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX`
(these must match the values the prefix was bootstrapped with — see
[Prerequisites](#prerequisites)), enable `BUILD_TESTING`, and emit
`compile_commands.json`. They default `A11_BUILD_PYTHON=OFF` so CLion builds the
C++ libraries and native test targets without the Python toolchain; build the
importable extension with `uv sync` (see [Editable Python build](#editable-python-build)).
To compile the extension inside CLion too, set `A11_BUILD_PYTHON=ON` (it needs
Homebrew's `pybind11` and a Python 3.11+ interpreter).

## Testing both layers

`pytest` alone does not rebuild C++. For a change that touches C++ or a
binding, use this sequence:

```sh
# 1. Compile and test the C++ implementation.
cmake --build build -j 8
ctest --test-dir build --output-on-failure

# 2. Compile and install that source revision for the editable Python package.
#    This command must finish before pytest starts in its separate process.
#    If it reports a missing CMakeCache.txt, the editable build tree is not
#    configured — recreate it with the reinstall command in "Editable Python
#    build", then rerun this step.
.venv/bin/python -c \
  'import a11._native as native; native.__loader__.rebuild()'

# 3. Exercise the cross-language boundary, then the complete Python contract.
.venv/bin/python -m pytest -q a11/tests/test_native_bindings.py
.venv/bin/python -m pytest -q

# 4. Check exported native targets when public headers or linkage changed.
scripts/smoke_cmake_install.sh build
```

Use the following minimum checks for each kind of change:

| Changed files                     | Required checks                                                |
|-----------------------------------|----------------------------------------------------------------|
| `a11/**/*.py`                     | Full pytest suite                                              |
| `cpp/thread/**`                   | Rebuild; `thread_test`; editable native rebuild; full pytest   |
| `cpp/a11/**`                      | Rebuild; `a11_core_test`; editable native rebuild; full pytest |
| `cpp/python/**`                   | Editable native rebuild; native binding tests; full pytest     |
| Public headers or CMake linkage   | Native suites and install smoke test                           |
| `pyproject.toml` or wheel scripts | Editable reinstall and at least one wheel build/audit          |

Formatting and lock-file checks are:

```sh
find cpp -type f \( -name '*.cc' -o -name '*.h' \) \
  -exec clang-format --dry-run --Werror {} +
.venv/bin/python -m black --check a11 scripts
uv lock --check
bash -n scripts/bootstrap_wheel_deps.sh scripts/smoke_cmake_install.sh
```

## Building wheels

The matrix is configured in `pyproject.toml` and orchestrated by
`scripts/build_wheels.py`:

- CPython 3.11, 3.12, 3.13, and 3.14 (3.15 is added automatically once
  cibuildwheel ships stable images; `enable = ["cpython-prerelease"]` is already
  set);
- macOS x86_64 and arm64 (deployment target 14.4, required by the Boost.Fiber
  futex spinlock — see [Prerequisites](#prerequisites));
- manylinux x86_64 and aarch64.

The wheels are architecture-specific. Do not create `universal2` wheels:
Boost.Context includes architecture-specific assembly.

`build_wheels.py` needs no environment set up by hand. It exports the deps
prefix per platform/arch; `scripts/bootstrap_wheel_deps.sh` (run automatically
as cibuildwheel's `before-all`) builds the static dependencies (Boost, OpenSSL,
curl, nghttp2, nlohmann-json, uvw) into that prefix, and CMake discovers them
from it. It also preflights prerequisites and prints a clear message instead of
failing deep in a build.

### Before you build

```sh
# Development tools (cibuildwheel, stub generators, linters).
uv sync --locked --group dev
```

- **Linux wheels** need Docker running. `build_wheels.py` checks this up front
  and stops with a clear message if it is not. Building a *non-native* Linux
  architecture (for example x86_64 on Apple silicon) additionally needs
  QEMU/binfmt and is slow; prefer building Linux wheels on a Linux/CI host.
- **macOS wheels** are built against the official python.org CPython framework
  builds under `/Library/Frameworks/Python.framework/Versions/` (the uv-managed
  interpreters in `.venv` cannot stand in for them). `build_wheels.py` skips any
  targeted version whose framework is missing and prints which — install the
  version from python.org for a complete matrix. Check what is present with:

  ```sh
  ls /Library/Frameworks/Python.framework/Versions/
  ```

### Commands

```sh
# Iterate: build only the host OS's wheels, native architecture first.
.venv/bin/python scripts/build_wheels.py

# One platform explicitly.
.venv/bin/python scripts/build_wheels.py --platform macos
.venv/bin/python scripts/build_wheels.py --platform linux

# Full release matrix (macOS + Linux). Linux needs Docker; run on a Linux/CI
# host to avoid QEMU. macOS wheels can only be built on macOS.
.venv/bin/python scripts/build_wheels.py --platform all

# Alternate output directory (default is dist/).
.venv/bin/python scripts/build_wheels.py --platform linux --output-dir dist/linux
```

List the builds without compiling them:

```sh
.venv/bin/python -m cibuildwheel --print-build-identifiers --platform macos
.venv/bin/python -m cibuildwheel --print-build-identifiers --platform linux
```

Each platform/architecture gets an isolated static dependency prefix and
ABI-specific `build/wheel/<wheel-tag>` directory, while editable builds use
`build/editable/<wheel-tag>`. A temporary wheel interpreter therefore cannot
invalidate the editable rebuild tree, and a native module from one interpreter
cannot leak into another wheel.

### Audit

Every wheel is tested by `scripts/audit_wheel.py` inside cibuildwheel's clean
test environment. The audit:

- imports `a11._native` and the Abseil status caster;
- requires exactly one ABI-specific `a11/_native` module;
- rejects universal wheels;
- rejects non-system dynamic dependencies; and
- rejects absolute or otherwise non-relocatable RPATH/RUNPATH entries.

`build_wheels.py` audits every wheel it can execute on the build host: native
wheels always, and an x86_64 wheel cross-built on Apple silicon when Rosetta 2
is present. Only genuinely unrunnable wheels (an arm64 wheel on an x86_64 host)
have their audit skipped, and the script prints a notice naming them — run the
matrix on that architecture to audit those.

### Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `Could NOT find Boost` (or another dep) at CMake configure | The deps prefix is missing or incomplete. Delete it (`rm -rf /tmp/a11-wheel-deps-*` on macOS, `/opt/a11-wheel-deps-*` on Linux) and rebuild; `before-all` re-bootstraps it. Static builds are pinned to the prefix, so this fails loudly instead of silently using a system/Homebrew Boost. |
| `symbol not found in flat namespace '_jump_fcontext'` at import/audit | A Boost.Context prefix built before the assembly-ABI fix. Delete the prefix and rebuild (the stamp file `.a11-wheel-deps-vN-<arch>` gates rebuilds; bumping the script's version forces one). |
| `"futex not supported on this platform"` at CMake/compile on macOS | The macOS deployment target is below 14.4. Keep `MACOSX_DEPLOYMENT_TARGET=14.4` in both the bootstrap shell and the CMake build. |
| Fiber crashes/corruption only in a hand-rolled build | The Boost.Fiber spinlock macro differs between the deps prefix and the A11 compile. Use the same `A11_FIBER_SPINLOCK` (default `BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX`) for both, and rebuild the prefix if you change it. |
| Linux build stops immediately citing Docker | Start Docker (or Colima) and retry, or build Linux wheels on a Linux host. |
| A macOS CPython version is skipped with a notice | Its python.org framework is not installed; install that version from python.org. |

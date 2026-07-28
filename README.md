# A11

A11 is a Python action and streaming runtime backed by a C++20 implementation.
The Python package in `a11/` is the public, language-native API; `cpp/a11/`
contains the native implementation, and `cpp/thread/` contains its cooperative
fiber runtime.

This guide covers the development build, tests, and release wheel matrix. Run
all commands from the repository root.

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
# deployment target.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}"

scripts/bootstrap_wheel_deps.sh
```

That script builds Boost, OpenSSL, nghttp2, nlohmann-json, and uvw. GoogleTest
must still be installed separately for `BUILD_TESTING=ON`. Keep the exported
paths in the shell used for CMake, `uv sync`, and editable rebuilds.

## Editable Python build

Create an environment and install the project in editable mode:

```sh
uv venv --python 3.12

export CMAKE_ARGS="-DA11_REQUIRE_STATIC_DEPS=ON -DA11_FETCH_MISSING_DEPS=ON"
uv sync --locked --group dev
```

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
uv sync --locked --group dev --reinstall-package a11
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
`uv sync --locked --group dev --reinstall-package a11` afterward.

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

- CPython 3.11, 3.12, 3.13, 3.14, and 3.15;
- macOS x86_64 and arm64;
- manylinux x86_64 and aarch64.

The wheels are architecture-specific. Do not create `universal2` wheels:
Boost.Context includes architecture-specific assembly.

Install the development tools first:

```sh
uv sync --locked --group dev
```

macOS wheels are built against the official python.org CPython framework
builds, which carry the correct deployment target for portable wheels.
cibuildwheel uses them in place and refuses to install them system-wide
outside CI, so install each targeted version from python.org before building
on macOS — CPython 3.11, 3.12, 3.13, 3.14, and 3.15, under
`/Library/Frameworks/Python.framework/Versions/`. The uv-managed interpreters
in `.venv` cannot stand in for them. Confirm what is present with:

```sh
ls /Library/Frameworks/Python.framework/Versions/
```

To iterate on a subset locally without every framework installed, skip the
missing macOS versions (this produces an incomplete matrix, not a release):

```sh
CIBW_SKIP="cp311-macosx_* cp314-macosx_*" \
  .venv/bin/python scripts/build_wheels.py --platform macos
```

Then build the host-required matrix:

```sh
.venv/bin/python scripts/build_wheels.py
```

On macOS, this builds both macOS architectures and both Linux architectures.
Docker must be running for Linux builds. On Linux, it builds the Linux matrix
only. Building a non-native Linux architecture requires Docker with the
appropriate binfmt/QEMU support.

Build only one platform when iterating:

```sh
.venv/bin/python scripts/build_wheels.py --platform macos
.venv/bin/python scripts/build_wheels.py --platform linux
```

Linux cannot build the macOS matrix. An alternative output directory can be
selected with `--output-dir`; the default is `dist/`:

```sh
.venv/bin/python scripts/build_wheels.py \
  --platform linux --output-dir dist/linux
```

List the builds without compiling them:

```sh
.venv/bin/python -m cibuildwheel \
  --print-build-identifiers --platform macos
.venv/bin/python -m cibuildwheel \
  --print-build-identifiers --platform linux
```

Python 3.15 is enabled through cibuildwheel's `cpython-prerelease` group until
stable images are available. Each platform/architecture gets an isolated
static dependency prefix and ABI-specific `build/wheel/<wheel-tag>` directory,
while editable builds use `build/editable/<wheel-tag>`. A temporary wheel
interpreter therefore cannot invalidate the editable rebuild tree, and a
native module from one interpreter cannot leak into another wheel.

Every wheel is tested by `scripts/audit_wheel.py` inside cibuildwheel's clean
test environment. The audit:

- imports `a11._native` and the Abseil status caster;
- requires exactly one ABI-specific `a11/_native` module;
- rejects universal wheels;
- rejects non-system dynamic dependencies; and
- rejects absolute or otherwise non-relocatable RPATH/RUNPATH entries.

Cross-compiled macOS wheels cannot be executed on the opposite host
architecture, so their runtime test is skipped there. Run the matrix on both
macOS architectures when release policy requires a native execution test for
each wheel.

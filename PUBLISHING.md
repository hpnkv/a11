# Publishing A11

Release the Python and npm packages from the same source revision and use the
same version in `pyproject.toml`, `cpp/python/module.cc`, and
`js/package.json`. Run every command below from the repository root unless a
step changes directory explicitly.

## PyPI

A complete Python release contains one source distribution and 20
architecture-specific wheels:

- CPython 3.11, 3.12, 3.13, and 3.14;
- macOS x86_64 and arm64; and
- manylinux x86_64 and aarch64.

Do not publish `universal2` or musllinux wheels. Build the complete matrix on a
macOS release host with Docker running and binfmt/QEMU configured for Linux
aarch64. Linux hosts can build only the Linux half of the required matrix.

The macOS host must also have the official python.org CPython framework builds
for every targeted version installed under
`/Library/Frameworks/Python.framework/Versions/` — 3.11, 3.12, 3.13, 3.14, and
3.15. cibuildwheel builds macOS wheels against those in place and will not
install them outside CI, and the uv-managed interpreters cannot substitute for
them. Install any that are missing from python.org before building; a partial
set yields an incomplete matrix that must not be released.

Prepare and verify the release (export `A11_DEPS_PREFIX` first, as in
[BUILDING.md](BUILDING.md#prerequisites), so the presets find the static deps):

```sh
uv sync --locked --group dev
.venv/bin/python scripts/generate_stubs.py
.venv/bin/python scripts/generate_stubs.py --check
cmake --preset debug
cmake --build --preset debug -j 8
ctest --preset debug
.venv/bin/python -m pytest -q
scripts/smoke_cmake_install.sh
```

Start with an empty `dist/`, then build the wheel matrix and source
distribution:

```sh
.venv/bin/python scripts/build_wheels.py
uv build --sdist --out-dir dist
```

Confirm that `dist/` contains exactly 20 wheels and one `.tar.gz`, with all
five CPython ABIs represented for each of the four platform/architecture
pairs. `scripts/build_wheels.py` runs `scripts/audit_wheel.py` for every wheel;
that audit checks the native module, dynamic dependencies, loader paths, and
PEP 561 typing files.

Upload to TestPyPI first when validating release credentials or metadata, then
install a wheel in a clean environment. For the production release, configure
PyPI trusted publishing or set `UV_PUBLISH_TOKEN` to a scoped API token and
run:

```sh
uv publish --token "$UV_PUBLISH_TOKEN" dist/*
```

Do not rebuild artifacts after uploading any part of a version. If a release
is wrong, increment the version and build the complete matrix again.

## npm

The TypeScript package is in `js/` and is named `a11`. Confirm that the npm
account or organization has publish rights to that unscoped package name.
After the Python artifacts for the same version have been accepted by PyPI:

```sh
cd js
npm ci
npm run build
npm pack --dry-run
npm publish --access public
```

`prepublishOnly` rebuilds the package as a final guard. Use npm trusted
publishing where configured; otherwise authenticate with `npm login` and
supply an OTP when prompted. Verify the published package with
`npm view a11 version` before creating the release tag.

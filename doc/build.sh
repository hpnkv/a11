#!/usr/bin/env bash
# Build the complete A11 documentation site (Python + TypeScript + C++).
#
#   doc/build.sh              # build into doc/site
#   doc/build.sh --strict     # fail on any MkDocs warning (used in CI)
#
# Requires the `docs` dependency group (see pyproject.toml) and, for the C++
# reference, `doxygen` and `graphviz` on PATH. If Doxygen is missing the Python
# site is still built and a note is printed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

STRICT=""
if [[ "${1:-}" == "--strict" ]]; then
  STRICT="--strict"
fi

# 1. Compile the browser guide clients. MkDocs copies them as normal assets.
#    They are type-checked first: the demos are the code the guides show, so a
#    type error in one is a documentation bug, and esbuild would bundle it
#    happily.
echo "==> Building browser guide clients"
npm --prefix "$HERE/../js" run typecheck:demos
npm --prefix "$HERE/../js" run build:demos

# 2. Python site (MkDocs Material + mkdocstrings). This clears doc/site first.
echo "==> Building Python documentation (mkdocs)"
mkdocs build $STRICT

# 3. TypeScript API. It must run after MkDocs, which clears doc/site.
echo "==> Building TypeScript documentation (TypeDoc)"
npm --prefix "$HERE/../js" run build:docs

# 4. C++ reference (Doxygen + doxygen-awesome-css) into doc/site/cpp.
if command -v doxygen >/dev/null 2>&1; then
  echo "==> Building C++ documentation (doxygen)"
  AWESOME_DIR="$HERE/cpp/doxygen-awesome-css"
  if [[ ! -f "$AWESOME_DIR/doxygen-awesome.css" ]]; then
    echo "    fetching doxygen-awesome-css v2.3.4"
    rm -rf "$AWESOME_DIR"
    git clone --depth 1 --branch v2.3.4 \
      https://github.com/jothepro/doxygen-awesome-css.git "$AWESOME_DIR"
  fi
  ( cd "$HERE/cpp" && doxygen Doxyfile )
  echo "    C++ reference at doc/site/cpp/index.html"
else
  echo "==> Skipping C++ documentation: 'doxygen' not found on PATH."
  echo "    Install doxygen + graphviz to include the C++ reference."
fi

echo "==> Documentation built at doc/site"

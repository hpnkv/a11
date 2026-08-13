# A11 documentation

The hosted documentation is built from three generators into one site:

- **Python API + guides** — [MkDocs Material](https://squidfunk.github.io/mkdocs-material/)
  with [mkdocstrings](https://mkdocstrings.github.io/) (`griffe` backend). Prose
  lives in `docs/`; the API reference is generated from the docstrings in the
  `a11/` package and the `a11/_native/` stubs. griffe reads these
  **statically**, so building the docs does *not* require the compiled native
  extension.
- **C++ internals** — [Doxygen](https://www.doxygen.nl/) themed with
  [doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css), over
  `cpp/a11/`. Output is written into `site/cpp` so the whole set deploys as one
  artifact.
- **TypeScript API** — [TypeDoc](https://typedoc.org/) over `js/src/`, written
  to `site/typescript`. The compile-checked browser guide client in `js/demo/`
  is bundled with esbuild and copied into the MkDocs assets.

## Build

Install the docs tooling and build:

```sh
uv sync --group docs
npm ci --prefix js
doc/build.sh            # -> doc/site  (add --strict to fail on warnings)
```

`doc/build.sh` runs esbuild, MkDocs, TypeDoc, and then Doxygen. Doxygen and Graphviz
(`dot`) must be on `PATH` for the C++ reference; if they are absent the Python
site is still built (the C++ pages are simply omitted). The
`doxygen-awesome-css` theme is fetched automatically into
`cpp/doxygen-awesome-css` on first build.

## Live preview (Python site only)

```sh
uv run --group docs mkdocs serve -f doc/mkdocs.yml
```

## Conventions

- **Python:** Google-style docstrings (see `AGENTS.md` → "Documentation").
- **C++:** Doxygen `///` or `/** ... */` doc-comments; Markdown is enabled.
- Extended, developer-facing prose is maintained for the core surface
  (`ChunkStore`, `WireStream` and implementations, `ChunkStoreReader`,
  `ChunkStoreWriter`, `AsyncNode`, `Session`, `WebSocketSignallingServer`,
  `WebSocketSignallingClient`); other symbols carry brief docstrings.

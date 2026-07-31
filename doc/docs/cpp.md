# C++ internals

A11's runtime is implemented in C++20 under `cpp/a11/` and bound to Python with
pybind11. Application developers never need this layer — the [Python
API](api/nodes.md) is the contract. It is documented for **contributors** to the
runtime itself.

The C++ reference is generated with Doxygen and shipped alongside this site:

<div class="grid cards" markdown>

- :material-language-cpp: **[Browse the C++ API reference →](cpp/index.html)**

    Classes, files, and call/collaboration graphs for the native
    `core`, `data`, `concurrency`, `stores`, `net`, `nodes`, `actions`, and
    `service` components.

</div>

!!! note
    The C++ pages are built by Doxygen (see `doc/cpp/Doxyfile`) and only appear
    in a full build produced by `doc/build.sh` or the documentation CI job. When
    viewing the MkDocs site locally with `mkdocs serve`, the link above is
    populated only after Doxygen has run.

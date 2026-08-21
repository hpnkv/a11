# C++ API

Use A11's C++20 libraries to embed actions, streams, storage, and transports in
a native application. The `core`, `data`, `concurrency`, `stores`, `net`,
`nodes`, `actions`, and `service` components are independently linkable, so an
application can select the layers it needs. The Python bindings reuse these
runtime types and follow the same behavioural contract.

The C++ reference is generated with Doxygen and shipped alongside this site:

<div class="grid cards" markdown>

- :material-language-cpp: **[Browse the C++ API reference →](cpp/index.html)**

    Classes, files, and call/collaboration graphs for the
    `core`, `data`, `concurrency`, `stores`, `net`, `nodes`, `actions`, and
    `service` components.

</div>

!!! note
    The C++ pages are built by Doxygen (see `doc/cpp/Doxyfile`) and only appear
    in a full build produced by `doc/build.sh` or the documentation CI job. When
    viewing the MkDocs site locally with `mkdocs serve`, the link above is
    populated only after Doxygen has run.

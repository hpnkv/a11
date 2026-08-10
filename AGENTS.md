# A11 Engineering Guide

## Source of truth and layout

- `a11/` defines the current Python API and behavioural contract. `cpp/` is
  its native implementation and should converge on the same semantics.
  `actionengine/` is historical reference material, not the source of truth.
- Keep independently linkable C++ components (`core`, `data`, `concurrency`,
  `stores`, `net`, `nodes`, `actions`, and `service`). Each `cpp/a11/<part>/`
  owns its source list in its local `CMakeLists.txt`; the target and dependency
  graph remain visible in `cpp/CMakeLists.txt`.
- Concurrency types live directly in `namespace a11`, even though their files
  remain under `a11/concurrency/`. Do not recreate `a11::concurrency`.
- Public stateful Python runtime types reuse the bound native class objects;
  do not add shadow Python implementations or facade subclasses for concrete
  native types. Attach thin, asynchronous, idiomatic protocols to the bound
  classes, while retaining deliberate virtual adapters such as
  `LocalChunkStore` where Python subclass overrides must cross into C++.
- Pydantic-style validation, JSON, copy, and schema helpers augment native
  binary and schema values; they must not introduce a second public data model.
  Python serializers and deserializers continue to operate on those native
  values.
- A chunk's metadata is the only thing that says how to read its bytes. The
  media type is the representation; a `type` parameter names the value when the
  format does not already describe it. The seven JSON-native shapes (`object`,
  `array`, `string`, `integer`, `number`, `boolean`, `null`) carry no parameter,
  so a bare `application/json` or `application/x-msgpack` is complete and
  decodes to a dict / `nlohmann::json` / plain object / map. Nothing inside a
  payload names a type: a declared model's fields say what they hold, and
  schemaless data is just data. A caller naming an `obj_type` gets a best effort
  and a real deserialization error when the data will not fit.
- A serializable type carries the *same* wire tag in every language:
  `a11.<Class>` for the runtime's own types, `a11.sdk.<Class>` for the SDKs,
  subpackages omitted and the name chosen for what the type is. The table lives
  once per language — `a11/data/serial_tags.py` (an `A11_SERIAL_TAG` ClassVar
  per class), `cpp/a11/data/serial_tags.h` (returned by the `A11SerialTag` ADL
  point), `js/src/serial_tags.ts`, `kotlin/.../SerialTags.kt` — and
  `testdata/serial_tags.json` pins all four, with each suite asserting its own
  constants against it. Renaming a tag is a wire change: add the old spelling to
  the legacy alias map so readers keep accepting it, and never emit it again.
- A status carried as data is a *status chunk*: mimetype
  `application/x-a11-status`, payload the concatenated-MessagePack
  `(code, message, details)` record, built in one place per language
  (`a11::data::MakeStatusChunk`, `statusToChunk`, `status_to_chunk`) and read
  back through that language's decoder. Action dispatch/completion statuses,
  node aborts and the closure marker a drained writer tees all use that one
  shape; `testdata/status_chunk.json` pins the mimetype, the `a11-close`
  marker attribute and the payload bytes, with each suite asserting its own
  helpers against it. It stays outside the serialization registry on purpose:
  `StatusOr` cannot carry a non-OK status as a *value*, which is what the
  dedicated helpers and the `DecodedStatus` box exist for.

## Concurrency architecture

- Thread is A11's scheduling substrate. In A11 code, explicitly spell
  `thread::Mutex`, `thread::MutexLock`, `thread::CondVar`, and
  `thread::SleepFor`. Mutex members are `mu`/`mu_`; condition variables are
  `cv`/`cv_`.
- Protect shared state with Abseil annotations (`ABSL_GUARDED_BY`,
  `ABSL_EXCLUSIVE_LOCKS_REQUIRED`, and `ABSL_LOCKS_EXCLUDED`) and keep
  `-Wthread-safety` clean.
- Boost.Context/Fiber is an implementation detail of Thread. No header may
  include or expose a Boost type. Fixed, stack-resident opaque storage is
  preferred for small Boost-backed primitives; implementation size/alignment
  assertions must guard it.
- The native `std::mutex`/`std::condition_variable` pair in Thread's Boost
  scheduler is deliberate: it parks an OS worker when no fiber is runnable.
  Do not replace that scheduler-internal pair with fiber-aware primitives.
  A11 state and ordinary Thread APIs must use the fiber-aware primitives.
- Use fibers near user-facing synchronous-looking APIs where they improve
  clarity. Use fair, bounded, stackless callback pumps for high-cardinality
  internal state machines. `ChunkStoreReader` and `ChunkStoreWriter` share
  stackless schedulers; never add a permanently allocated fiber per instance.
- Root-fiber stack size is configurable through
  `THREAD_DEFAULT_FIBER_STACK_SIZE` and `thread::TreeOptions::stack_size`.
  Avoid extra context switches and do not introduce unbounded queues.
- Thread changes require cooperative-concurrency tests: cancellation and tree
  propagation, `Select`/`SelectUntil`, `SleepFor`, timed waits, FIFO behaviour,
  deadlock resistance, waiter cleanup, and joined/detached fiber lifetime.

## C++ API and implementation rules

- Model ownership with `std::unique_ptr` and `std::shared_ptr`; use raw pointers
  for temporary non-owning access. Prefer `make_unique`/`make_shared`, write
  `ptr == nullptr` or `ptr != nullptr`, and apply `absl_nonnull`,
  `absl_nullable`, or `absl_nullability_unknown` to raw-pointer contracts.
- Avoid reference counting where ownership is singular or scope-bound. Prefer
  inline storage, contiguous data, `absl::InlinedVector`, bounded arenas, or a
  stack-optimised pImpl when lifetime and size justify them.
- Thread is an accepted public A11 dependency. Keep direct members and inline
  implementations when Thread was the only reason for a pImpl. Hide other
  third-party implementation types with forward declarations or pImpl unless
  they are intrinsically part of a public data contract.
- Use `absl::flat_hash_map` and `absl::flat_hash_set`. Use
  `absl::node_hash_map` only when mapped-value addresses truly must remain
  stable; pointer-valued flat maps already have stable pointees. Test lookup
  iterators instead of using `.contains()`.
- Use unqualified `size_t`, not `std::size_t`.
- Virtual functions have no default arguments. Provide non-virtual convenience
  overloads when defaults are useful.
- Initialise every aggregate field deliberately. Treat use-after-move,
  implicit narrowing, and partially initialised state as correctness bugs.

## Abseil conventions

- Abseil `Status`, `StatusOr`, `Time`, and `Duration` are the native error and
  timing types. Prefer `ABSL_RETURN_IF_ERROR` and `ABSL_ASSIGN_OR_RETURN`.
- For `absl::StatusOr<absl::Status>`, use `AssignStatus` to represent an outer
  error; do not add a wrapper type or conflate it with the inner status value.
- Log through Abseil (`LOG(ERROR)`, `DLOG`, and checks), never `std::cerr`.
  Add `AbslStringify` to custom value types used in diagnostics.
- Preserve structured A11 status details across C++, MessagePack/JSON, and
  Python. Python callers must receive `a11.status.StatusException`, not a
  generic or pybind11-specific exception.

## Networking

- WebSocket transport and WebSocket signalling use A11's nghttp2/HTTP2 stack.
  libdatachannel is reserved for WebRTC data channels and peer connections.
- All channel transports use the Action Engine-compatible byte-chunking wire
  format. Keep `ChannelFramingOptions` aligned with `ByteChunkingOptions`,
  enforce message and pending-byte bounds, and retain out-of-order/interleaved
  reassembly tests. WebRTC chunk sizes must remain below SCTP limits;
  WebSocket chunking also prevents large messages from monopolising a stream.

## Python boundary

- Python-specific policy belongs only in `cpp/python/` and thin Python facade
  modules. Core A11 and Thread must not know about the GIL, asyncio, pybind11,
  or Python exception classes.
- Binding callbacks may arrive from fibers, libuv, or libdatachannel threads.
  Acquire the GIL before touching Python, release it around blocking native
  waits, marshal coroutine work to its captured asyncio loop, and retain Python
  objects until completion without leaking references.
- Native `absl::Time`, `absl::Duration`, and `absl::Status` may back Python
  convenience types, but arithmetic, exceptions, reprs, and async methods must
  continue to feel native to Python.
- Synchronous binding methods returning `absl::Status` or `StatusOr` must use
  the shared status boundary so failures raise `a11.status.StatusException`;
  never expose a raw Abseil or pybind11 status exception to callers.
- Attach the Python protocol as a readable `class` body and copy it onto the
  bound native class with `a11._native_protocol.attach_protocol`, rather than a
  flat list of `NativeClass.method = _fn` assignments. Keep native descriptor
  captures (`_native_x = NativeClass.x`) as module globals before the attach,
  and keep truly-internal helpers module-level so they stay out of the stub.
  Export the public native classes with `from a11._native import X` (an import
  alias griffe and type checkers resolve to the class), not `X = _native.X`
  (an opaque attribute assignment). Field-driven option structs stay with
  `a11._native_options.install_native_options`.

## Documentation

- Docs live in `doc/` and build to `doc/site` via `doc/build.sh` (see
  `doc/README.md`): MkDocs Material + `mkdocstrings` for the Python API and
  guides, Doxygen + doxygen-awesome-css for the C++ internals. The Python site
  is static — `griffe` reads `a11/` and `a11/_native.pyi`, so no native build is
  needed to generate it. CI builds and deploys it (`.github/workflows/docs.yml`).
- Python: Google-style docstrings. Write for a developer *building an AI agent* —
  explain the asynchronous, streaming intent and when to reach for a thing, not
  just its mechanics. Maintain extended prose for the core surface (`ChunkStore`,
  `WireStream` and implementations, `ChunkStoreReader`, `ChunkStoreWriter`,
  `AsyncNode`, `Session`, `WebSocketSignallingServer`,
  `WebSocketSignallingClient`); keep other symbols briefly but accurately
  documented.
- C++ / pybind11: give every `.def*` real parameter names (`py::arg("...")`, not
  `arg0`) and a docstring; extended for the core surface, brief elsewhere. Prose
  belongs in the C++ header doc-comments (`///` or `/** */`) where practical.
  After changing bindings, rebuild the extension and regenerate `a11/_native.pyi`
  with `scripts/generate_stubs.py`; `--check` gates it in CI.

## Dependencies, installation, and wheels

- Non-system C++ dependencies are statically linked. Shared objects are allowed
  only where a runtime/module boundary requires them; any such dependency must
  use loader-relative RPATH/RUNPATH and never an absolute build-machine path.
- Abseil is pinned and fetched from upstream because A11 depends on current
  status macros. Boost, OpenSSL, nghttp2, uvw/libuv, and libdatachannel targets
  must pass the static-target checks. Installed CMake targets must pass the
  out-of-tree smoke test.
- `scripts/build_wheels.py` builds architecture-specific CPython 3.11-3.14
  wheels for macOS x86_64/arm64 and Linux x86_64/aarch64. Do not emit
  `universal2` wheels: Boost.Context contains architecture-specific assembly.
- On macOS, build both macOS and Linux matrices; on Linux, Linux-only is valid.
  The dependency bootstrap builds static archives per target architecture and
  deployment target. scikit-build outputs stay in ABI-specific build trees;
  generated extensions must never enter the sdist or leak into another ABI's
  wheel.
- Every wheel must contain exactly one `a11/_native` extension and pass
  `scripts/audit_wheel.py`, which rejects non-system loader dependencies,
  absolute RPATH/RUNPATH entries, and universal wheels.

## Verification

- Build the native tree through the CMake presets (see BUILDING.md); export
  `A11_DEPS_PREFIX` first:

  ```sh
  cmake --preset debug
  cmake --build --preset debug -j 8
  ctest --preset debug
  .venv/bin/pytest -q
  scripts/smoke_cmake_install.sh
  ```

- Format C++ with the root `.clang-format`; run the configured `.clang-tidy`
  checks when the tool is available. Keep Python formatted with Black/Ruff and
  regenerate `uv.lock` after dependency or Python-version changes.
- Add regression tests at the lowest useful layer. Remove or update tests that
  assert obsolete pure-Python internals, while retaining language-native public
  behaviour and cross-language callback/error tests.

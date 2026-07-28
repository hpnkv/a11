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

## Dependencies, installation, and wheels

- Non-system C++ dependencies are statically linked. Shared objects are allowed
  only where a runtime/module boundary requires them; any such dependency must
  use loader-relative RPATH/RUNPATH and never an absolute build-machine path.
- Abseil is pinned and fetched from upstream because A11 depends on current
  status macros. Boost, OpenSSL, nghttp2, uvw/libuv, and libdatachannel targets
  must pass the static-target checks. Installed CMake targets must pass the
  out-of-tree smoke test.
- `scripts/build_wheels.py` builds architecture-specific CPython 3.11-3.15
  wheels for macOS x86_64/arm64 and Linux x86_64/aarch64. Python 3.15 remains
  enabled through cibuildwheel's `cpython-prerelease` group until it is stable.
  Do not emit `universal2` wheels: Boost.Context contains architecture-specific
  assembly.
- On macOS, build both macOS and Linux matrices; on Linux, Linux-only is valid.
  The dependency bootstrap builds static archives per target architecture and
  deployment target. scikit-build outputs stay in ABI-specific build trees;
  generated extensions must never enter the sdist or leak into another ABI's
  wheel.
- Every wheel must contain exactly one `a11/_native` extension and pass
  `scripts/audit_wheel.py`, which rejects non-system loader dependencies,
  absolute RPATH/RUNPATH entries, and universal wheels.

## Verification

- Use `build` as the normal native build tree:

  ```sh
  cmake --build build-codex -j 8
  ctest --test-dir build-codex --output-on-failure
  .venv/bin/pytest -q
  scripts/smoke_cmake_install.sh build-codex
  ```

- Format C++ with the root `.clang-format`; run the configured `.clang-tidy`
  checks when the tool is available. Keep Python formatted with Black/Ruff and
  regenerate `uv.lock` after dependency or Python-version changes.
- Add regression tests at the lowest useful layer. Remove or update tests that
  assert obsolete pure-Python internals, while retaining language-native public
  behaviour and cross-language callback/error tests.

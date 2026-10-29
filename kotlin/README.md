# A11 for Kotlin

Kotlin APIs for A11 on JDK 21. Native bindings share C++ stores, transports,
codecs, timing, and Flow tooling. Coroutines await native futures without
blocking JVM workers.

[A11 documentation](https://docs.a11.to/) ·
[Repository](https://github.com/hpnkv/a11) ·
[Questions and issues](https://github.com/hpnkv/a11/issues)

## What's here

- **Core** (`a11`): `Status`/`StatusOr`, byte utils, the concatenated-MessagePack
  codec, `Status` codec, coroutine primitives, and the wire data model
  (`Chunk`, `ChunkMetadata`, `NodeRef`, `NodeFragment`, `Port`, `ActionMessage`,
  `WireMessage`) — byte-for-byte compatible with the Python/JS encoders.
- **Streaming & RPC**: `AsyncNode`/`NodeMap`, the serialization registry,
  `ActionSchema`/`Action`/`ActionRegistry`, and `Session`/dispatch (including the
  reserved `__status__`/`__dispatch_status__` protocol).
- **Native runtime**: local and SQLite chunk stores, readers, writers, native
  futures, `SessionWithRecv`, Abseil timing, and fiber diagnostics.
- **Transport** (`a11.net`): native in-process and WebSocket streams, a
  WebSocket server, and HTTP protocol/TLS options. Native `AUTO` negotiation is
  the default. A gateway listener configured for HTTP/1.1 uses
  `Http2Options(clientPreference = Http2Options.ProtocolPreference.HTTP11)`.
- **Flow** (`a11.flow`): requests to the shared native language service and
  compiled programs with native plans.
- **SDK** (`a11.sdk`): `LlmHeaders`/`Role`/`Interaction`, the
  `INTERACT_WITH_LLM_SCHEMA`, the JSON-Schema tool adapter, and
  `getToolDefinitions`.

## Idioms

- Fallible operations return `StatusOr<T>`: `Ok(value)` or a non-OK `Status`.
  Propagate either a `Status` or `StatusOr` with
  `.orElse { status -> return status }`. The callback receives `Err`, the
  non-OK subtype, so it can be returned directly without a wrapper.
  `Err(status)` explicitly checks a general status when that conversion is
  needed. An OK status cannot masquerade as a successful value result.
- Inspect `ok()` or `.status` without accessing a value. `map` and `flatMap`
  transform successful values and retain errors unchanged. `Ok(null)` is a
  successful nullable payload. Read it through `orElse` or a checked `value()`;
  failures have no nullable fallback accessor.
- `value()` requires success. Invalid access prints a fatal diagnostic and stack
  trace to stderr and calls `Runtime.halt(1)`. Constructing `Err(Status.ok())`
  also terminates the JVM. These invariant failures bypass exception handlers
  and shutdown hooks. The constructor check applies in every JVM build;
  Abseil uses a fatal check in debug and an INTERNAL fallback in release.
- `Status()` and `Status.ok()` have code OK, an empty message, and no details.
  Error equality includes code, message, and structured details; the optional
  local exception cause is excluded. Integer detail values use `Long` and
  floating values use `Double` across codec round trips. `StatusException` remains an explicit
  application-boundary adapter; ordinary operations return statuses.
- Action state uses properties such as `action.id`, `action.schema`,
  `action.status`, and `action.isDone`. Instance options use immutable data
  classes. Wire records expose mutable native fields and compare their contents,
  including byte arrays; runtime objects retain identity equality.
- Native operations return real native futures. Several coroutines may observe
  one result. Cancelling a coroutine unsubscribes its observer and requests
  native cancellation; cancellation remains a request to the producer.
- Native wrappers follow JVM reference lifetime. The binding retains native
  shared ownership and keeps receivers and arguments reachable while calls
  adopt their handles. Ordinary code needs no ownership scopes or acquisition
  helpers. Explicit operations such as `server.stop()` control resources with
  a defined shutdown protocol; garbage collection does not replace them.
- Wire records refer to C++ values. Child records, lists, and byte maps expose
  native members, so changes through either language remain visible. Assigning
  a record copies its value, as C++ member assignment does. Replacing a member
  or structurally editing a list invalidates earlier child views; access then
  fails a native invariant check. These mutable values need external
  synchronization when shared between threads, as their C++ counterparts do.
- Wire values expose `copyFrom`, `moveFrom`, and `swap` for native assignment,
  move assignment, and swapping. A moved source remains accessible in its C++
  moved-from state. `sendMoved` and `putMoved` transfer wire contents directly
  into native transport/store calls; the ordinary methods copy their input.
  Reacquire child views after copy/move assignment or swapping contents.
  Collection views are created on first access. Bulk list
  assignment and native-to-native map assignment run in C++ without producing
  JVM copies of each element or byte buffer.
- `chunk.data` and byte-map values return owned JVM arrays. Assign the property
  or map entry to replace native bytes; editing an array returned by a getter
  does not mutate native storage. Wire sequence numbers and node-reference
  offsets use `UInt`; reference lengths use `ULong` and native validation.
- Completion encoding runs on the JVM event pump. Native completion callbacks
  retain shared future state rather than copying payloads. JVM/native byte
  transport still copies buffers; the binding does not promise zero-copy data.
- `node.read()` returns `NodeRead.Value(value)` or `NodeRead.End` inside a
  `StatusOr`. This distinguishes JSON null, clean completion, and an error.
  `node.next()` retains its nullable convenience form. Reader cancellation
  reports ABORTED; coroutine cancellation propagates through the coroutine.
- Close nodes and streams explicitly. Garbage collection retains references
  but provides no deterministic close or completion guarantee.

## Compare the native contracts

| Kotlin API | Native reference | Kotlin scope |
| --- | --- | --- |
| `Status`, `StatusOr<T>` | `a11/status.h`, Abseil `Status`/`StatusOr` | Canonical codes, OK normalization, structured errors, fatal invalid access; JVM references replace C++ moves. |
| Wire records and `NodePayload` | `a11/data/types.h` | Concatenated MessagePack, validation, owned copies, content equality; timestamps retain native `Time`, with microsecond adapters. |
| `SerializationRegistry` | `a11/data/serialization.h`, Python serialization registry | Kotlin codecs select dynamic values by wire tag; JSON-native payloads remain plain values. |
| `AsyncNode`, `NodeMap` | `a11/nodes/`, `a11/stores/` | One in-memory sequence and consuming cursor; unique uint32 sequences, finality, closure, abort, and cancellation. |
| `Action`, schemas, registry | `a11/actions/`, Python action protocols | Local handlers, remote calls, port finalization, status nodes, schema copies, cancellation, and cleanup. |
| `Session`, wire streams | `a11/service/session.h`, `a11/net/` | Dispatch, half-close, abort trailers, callback failure, and completion after the done callback. |
| SDK interactions and tools | Python SDK models and LLM action schemas | Tagged maps and tool definitions; provider execution remains on the remote service. |

The original Kotlin `AsyncNode`, action, registry, and callback-session APIs
still contain Kotlin runtime logic. Their native migration and the remaining
Python binding surface are incomplete. WebRTC, signalling, Flow execution,
provider execution, and observability configuration are not yet exposed through
these Kotlin bindings. Dynamic SDK models remain host-side maps.

## Migrate callers

Return code-specific error statuses directly. Use `orElse` to propagate an
existing failure, or `Err(status)` when explicitly requiring a general status
to be non-OK. An OK status is a result value only when wrapped as
`Ok(Status.ok())`. Replace Action getters with properties and use `copy(...)`
for changes to creation options. `NodeFragment.data` accepts only `Chunk` or
`NodeRef`. `BytePacket.sequence` and `packetCount` use `UInt`;
`transientId` uses `ULong`.

`valueOrThrow()` is removed. Use `value()` only after establishing success;
application boundaries that require exceptions can explicitly use
`.orElse { status -> throw StatusException(status) }`.

## Build & test

```sh
cd kotlin
./gradlew check       # tests and formatting
./gradlew kotlinFormat

# Live cross-language interop against the Python service:
#   (start `a11 gateway` first)
A11_BACKEND_URL=ws://127.0.0.1:8011/a11 ./gradlew test
```

`GoldenVectorsTest` decodes bytes produced by the installed Python `a11`
package and re-encodes them to identical bytes — the byte-compatibility proof.

Formatting uses [Google's Kotlin guide](https://developer.android.com/kotlin/style-guide):
four-space indentation, 100 columns, and explicit imports. The root
`.editorconfig` supplies IDEA's settings; Gradle's `kotlinFormat` and
`kotlinFormatCheck` tasks cover the library and example 010.

The Gradle build configures CMake's debug preset with `A11_BUILD_JVM=ON` and
packages the platform library in the jar. Set `A11_DEPS_PREFIX` as described in
[BUILDING.md](../BUILDING.md). `-Da11.native.library=/absolute/path/to/library`
selects an existing native build.

Application examples:
[009 C++ echo](../examples/009-websocket-echo-cc) and
[010 Kotlin echo](../examples/010-websocket-echo-kotlin).

Use `kotlin.time.Duration` for typed timeouts, such as `store.get(0, timeout =
5.seconds)`. Native `a11.Duration` retains Abseil's full range and quarter-
nanosecond precision, with checked conversion through `toKotlinDuration()`.
`a11.Time` supports native arithmetic, ordering, and infinite bounds. Its
checked Kotlin `Instant` conversion requires `ExperimentalTime` on Kotlin 2.2;
ordinary native `Time` operations do not require that opt-in.

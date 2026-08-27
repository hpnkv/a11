# A11 for Kotlin

A byte-compatible Kotlin port of the A11 client runtime, mirroring
[`../js`](../js) minus WebRTC, signalling, and the in-browser Gemma backend. It
gives JVM/Kotlin clients (such as the CLion plugin in
[`../intellij-plugin`](../intellij-plugin)) everything needed to expose tools and
call LLM-based actions over an A11 WebSocket.

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
- **Transport** (`a11.net`): byte-packet framing, `ChannelWireStream`, an
  in-process pair, and `WebSocketWireStream` — a JDK-WebSocket (RFC 6455 /
  HTTP/1.1) client that connects directly to the native `WebSocketWireServer`.
- **SDK** (`a11.sdk`): `LlmHeaders`/`Role`/`Interaction`, the
  `INTERACT_WITH_LLM_SCHEMA`, the JSON-Schema tool adapter, and
  `getToolDefinitions`.

## Idioms

- Fallible operations return `StatusOr<T>` (`Ok(value)` or a non-OK `Status`);
  unwrap with `.orElse { return it }` or `.valueOrThrow()`.
- Async is `kotlinx.coroutines`; blocking transport edges run on a
  virtual-thread dispatcher (`A11Runtime`).

## Build & test

```sh
cd kotlin
./gradlew test        # round-trip, golden byte-vector, and end-to-end tests

# Live cross-language interop against the Python service:
#   (start `a11 gateway` first)
A11_BACKEND_URL=ws://127.0.0.1:8011/a11 ./gradlew test
```

`GoldenVectorsTest` decodes bytes produced by the installed Python `a11`
package and re-encodes them to identical bytes — the byte-compatibility proof.

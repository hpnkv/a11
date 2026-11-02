# TypeScript export and call-path audit

`node js/scripts/audit_public_exports.mjs` writes
[`WASM_EXPORT_AUDIT.json`](WASM_EXPORT_AUDIT.json). It resolves each export
from the five package entry points to its declaring module. The current
inventory has 1,056 export paths, including repeated names across subpaths;
317 are type-only. The route describes the first implementation boundary.

| Route | Call path |
| --- | --- |
| `native-worker` | TypeScript facade → threaded worker → C++ stores, nodes, actions, sessions, channel streams, and Flow |
| `native-after-init` | TypeScript value conversion → synchronous WASM binding → C++ data, framing, status, schema, authorization, references, and Flow tooling |
| `native-worker-facade` | Usage convenience → native action, node, and reference state |
| `generated-native-data` | Flow vocabulary generated from C++ |
| `value-conversion` | TypeScript classes and JSON conversion at a native boundary |
| `host` | Browser/Node storage, network, callbacks, provider SDKs, presentation, and status ergonomics |
| `typescript-protocol` | Host codec and adapter code described below |
| `type-only` | No runtime call path |

The synchronous A11 record, byte framing, status chunk, action schema,
authorization, stream reference, and Flow token helpers require
`await createA11Runtime()` first. They return the SDK's
`FAILED_PRECONDITION` status when no WASM runtime has initialized their
bindings. The threaded worker owns the graph, snapshots, action registry,
sessions, Flow execution, and `ChannelWireStream` lifecycle. Pull-style and
observer reads over native channel streams use C++ `WireStreamWithRecv`.
Browser and Node transports provide packet I/O to that C++ stream.

## Host-implemented boundaries

- `SerializationRegistry` selects user-provided asynchronous codecs and maps
  TypeScript model objects to fields. Built-in plain JSON, generic MessagePack,
  text, and bytes use C++ codecs when no custom codec matches. Tagged SDK
  values use their TypeScript model converters before C++ receives a chunk.
- A caller-provided `WireStream` with no native channel handle uses the
  TypeScript `WireStreamWithRecv` callback adapter. Native channel streams use
  the C++ reader. WebSocket, WebRTC, SSE, and in-process packet transports
  remain browser or Node host APIs.
- `schema_json.ts` maps native action documents into public `ActionSchema`
  objects. C++ parses, describes, and validates the schema.
- TypeScript `Status` and `StatusOr` preserve language ergonomics. C++ owns
  their wire codec and status chunks.
- Provider-specific LLM orchestration, Gemma host inference, tool-model
  conversion, and presentation have no corresponding C++ implementation;
  they run as TypeScript callbacks over native actions and nodes.
- TypeScript fills default option values for public byte and stream option
  objects. C++ validates the effective channel and reassembly limits.

`backendFor()` reports ownership of the selected runtime feature. For
`codecs`, it means the runtime's wire and record codecs. `backendOf()` reports
ownership for graph and store objects created by that runtime. Neither method
reports the backend of a user-provided codec or transport callback.

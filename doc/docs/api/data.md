# Data and serialization

The wire records A11 moves — chunks, fragments, node references, and wire
messages — and the registry that turns Python objects into chunks and back.

## Representation and value type

A chunk's metadata defines how to read its bytes. The media type names the
representation, such as `text/plain`, `application/json`, `image/png`, or
`application/x-msgpack`. A `type` media-type parameter names the application
value when the representation does not describe it fully.

The seven JSON-native shapes (`object`, `array`, `string`, `integer`, `number`,
`boolean`, and `null`) need no type parameter. Bare `application/json` and
`application/x-msgpack` therefore decode to ordinary schemaless values. Model
fields and action port schemas provide the expected type when one is declared.

Payload content does not select an application class. A requested object type
is a best-effort decode target, and incompatible data returns a
deserialization error.

## Application values and encoded payloads

Use `a11.to_chunk(value)` and the serialization registry for Python values.
Serializable A11 and SDK types carry stable wire tags so another language can
select its corresponding type. JSON-native values remain plain data without a
tag.

Use `Chunk(data=..., metadata=...)` for bytes already encoded in their final
format. For example, PNG bytes belong in an `image/png` chunk and an HTTP body
retains its received media type. Nodes write these through `put_chunk()` and
read them through `next_chunk()`; no application-value codec is involved.

This boundary keeps serialization and transport separate: codecs map values to
representations, while chunks carry any valid representation through stores,
nodes, and wire streams.

## Chunk

::: a11.data.types.Chunk

::: a11.data.types.ChunkMetadata

## Fragments and references

A [`NodeFragment`][a11.data.types.NodeFragment] associates one chunk with a
node ID, sequence position, and continuation state. Sequence numbers restore
the node's logical order even when transport messages arrive out of order.

A [`NodeRef`][a11.data.types.NodeRef] identifies a range held by another node.
Support depends on the backing store; the [store reference](stores.md) lists
the implementations that retain indexed references.

::: a11.data.types.NodeFragment

::: a11.data.types.NodeRef

## Messages

A [`WireMessage`][a11.data.types.WireMessage] batches action control messages
and node fragments. Control establishes or updates an action lifecycle; data
continues on the mapped nodes independently. One transport message may contain
work for several actions and nodes, and one action's stream may span many wire
messages.

::: a11.data.types.Port

::: a11.data.types.ActionMessage

::: a11.data.types.WireMessage

## Serialization

::: a11.data.serialization.SerializationRegistry

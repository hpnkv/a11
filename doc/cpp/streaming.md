# Stream values through a node {#cpp_streaming}

An `a11::nodes::AsyncNode` is an ordered sequence backed by a
`a11::stores::ChunkStore`. Action ports use nodes, but a native application can
also create one directly for model tokens, audio frames, progress records, or
other incremental data.

## Create a local node

The default local store keeps fragments in memory. A serialization registry is
optional; `AsyncNode::Create` uses the process-wide registry when none is
supplied.

```cpp
ABSL_ASSIGN_OR_RETURN(
    std::shared_ptr<a11::stores::LocalChunkStore> store,
    a11::stores::LocalChunkStore::Create("tokens"));
ABSL_ASSIGN_OR_RETURN(
    std::shared_ptr<a11::nodes::AsyncNode> node,
    a11::nodes::AsyncNode::Create(store));
```

## Write typed values

`Put()` serializes one value and returns its assigned sequence number after the
store accepts it. Awaiting that result propagates storage errors and applies
backpressure to a producer.

```cpp
for (const std::string& token : {"A11", "streams", "everything"}) {
  ABSL_ASSIGN_OR_RETURN(std::uint32_t sequence,
                        node->Put(token).Await());
  LOG(INFO) << "stored sequence " << sequence;
}
ABSL_RETURN_IF_ERROR(node->Finalize({.wait = true}).Await().status());
```

The no-value `Finalize()` writes an invisible final marker. It is useful when a
producer learns that the preceding value was last only after writing it.
`Finalize(value)` records that value itself as final and avoids the extra
marker.

## Read values one-by-one

`NextObject<T>()` returns the next decoded value or `std::nullopt` at a clean
end. Each call advances this node's reader.

```cpp
while (true) {
  ABSL_ASSIGN_OR_RETURN(
      std::optional<std::string> token,
      node->NextObject<std::string>().Await());
  if (!token.has_value()) break;
  std::cout << *token << '\n';
}
```

Use `ResetReader()` before replaying a node. Durable Redis and SQLite stores
retain finality as data, so a later reader observes the same end after a
restart.

## Carry encoded data

Use chunks when bytes already have their final representation. The chunk's
metadata is the only source of truth for decoding those bytes.

```cpp
a11::data::Chunk preview{
    .metadata = a11::data::ChunkMetadata{.mimetype = "image/png"},
    .data = png_bytes,
};
ABSL_RETURN_IF_ERROR(
    node->Finalize(std::move(preview), {.wait = true}).Await().status());
```

A reader that needs the representation calls `NextChunk()` and checks its size
before allocating or decoding application objects:

```cpp
ABSL_ASSIGN_OR_RETURN(std::optional<a11::data::Chunk> chunk,
                      node->NextChunk().Await());
if (!chunk.has_value()) {
  return absl::DataLossError("preview stream ended without an image");
}
if (chunk->data.size() > maximum_image_bytes) {
  return absl::ResourceExhaustedError("preview exceeds the size limit");
}
```

`Close()` only closes storage. It does not identify a final value. Use it for a
source such as a log that can promise no more records but cannot define a
single final sequence. Use `AbortWithStatus()` when partial output represents a
failure; readers then receive the status instead of a successful end.

## Backpressure and completion

Node writes enter a bounded writer before reaching the store. A fast producer
can use `WaitForBufferToDrain()` at a natural batching boundary:

```cpp
for (const Batch& batch : batches) {
  ABSL_RETURN_IF_ERROR(WriteBatch(*node, batch));
  ABSL_RETURN_IF_ERROR(node->WaitForBufferToDrain().Await().status());
}
```

The confirmation from `Put()` means the local store accepted the fragment. It
does not acknowledge that a remote reader processed it. End-to-end
acknowledgements belong in an application output port.

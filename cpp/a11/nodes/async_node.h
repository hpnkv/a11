// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A11's unit of streaming state: the AsyncNode.
 *
 * An `AsyncNode` is a single, ordered sequence of chunks (keyed by
 * sequence number) that one side writes and another side reads. It is
 * backed by a `stores::ChunkStore` -- the ordered storage boundary the
 * reader and writer stream through -- and can optionally be mirrored
 * across a `net::WireStream` to a remote peer.
 *
 * Nodes are how the input and output ports of an `actions::Action`
 * carry data, and how an agent streams partial results (tokens, audio
 * frames, tool calls) to its caller before the work is finished. Values
 * enter as raw `data::Chunk` / `data::NodeFragment` objects or as
 * arbitrary typed values encoded through the node's
 * `data::SerializationRegistry`, and leave the reader end deserialized.
 */

#ifndef A11_NODES_ASYNC_NODE_H_
#define A11_NODES_ASYNC_NODE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serializable.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/chunk_store_reader.h"
#include "a11/stores/chunk_store_writer.h"
#include "thread/boost_primitives.h"

namespace a11::net {
class WireStream;
}  // namespace a11::net

namespace a11::nodes {

/**
 * @brief How an `AsyncNode::Finalize()` call ends the stream.
 *
 * The defaults are the ordinary case: mark the logical end, close the writer,
 * and let the writer's pump carry both out after the producer has moved on.
 */
struct FinalizeOptions {
  /// Whether to resolve only once the store confirmed the write and, if
  /// `close` is set, closed. Leave it false to hand both to the writer's pump.
  bool wait = false;
  /// Whether to close the writer after the final chunk. Clear it to finalise
  /// now and close later -- a producer closing several nodes at once, say.
  bool close = true;
  /// Explicit sequence number for the final chunk; assigned in order when
  /// omitted.
  std::optional<std::uint32_t> seq = {};
  /// MIME type selecting the encoding of a typed value. Ignored by the
  /// no-value and raw-chunk overloads.
  std::string_view mimetype = {};
};

/**
 * @brief An asynchronous, ordered stream of chunks read from and written
 * to A11.
 *
 * A node has two halves. The writer end admits values into the backing
 * `stores::ChunkStore` in sequence; the reader end yields them back,
 * optionally deserializing typed objects on the way out. Every `Put*`
 * resolves once the backing store accepts the chunk. During the same flush,
 * the writer attempts to enqueue the batch on attached `net::WireStream`s;
 * this is not a remote-delivery acknowledgement, and a later tee failure
 * cannot revoke the current batch's store confirmation.
 *
 * A producer ends a node with Finalize(): it marks the logical end of the data
 * and, by default, closes the writer. Finality and closure remain two distinct
 * facts -- see the AsyncNode lifecycle guide -- and Finalize() writes one and
 * requests the other in the order readers expect. Close() is the rarer half on
 * its own, for a producer that cannot say which chunk was last.
 *
 * Instances are always heap-allocated and shared via `Create`; the class
 * is non-copyable and derives from `enable_shared_from_this`.
 */
class AsyncNode : public std::enable_shared_from_this<AsyncNode> {
 public:
  /**
   * @brief Create a node over an existing chunk store.
   *
   * @param store The backing ordered buffer the reader and writer stream
   *   through.
   * @param serialization_registry Registry used to (de)serialize typed
   *   values passed to `Put`/`NextObject`; defaults to none.
   * @param reader_options Options controlling how the reader buffers and
   *   orders chunks.
   * @param writer_options Options controlling how the writer buffers
   *   chunks.
   * @return The new node, or an error status on failure.
   */
  static absl::StatusOr<std::shared_ptr<AsyncNode>> Create(
      std::shared_ptr<stores::ChunkStore> store,
      std::shared_ptr<data::SerializationRegistry> serialization_registry =
          nullptr,
      stores::ChunkStoreReaderOptions reader_options = {},
      stores::ChunkStoreWriterOptions writer_options = {});

  ~AsyncNode() = default;

  AsyncNode(const AsyncNode&) = delete;
  AsyncNode& operator=(const AsyncNode&) = delete;

  /**
   * @brief Return the node's stable identifier.
   * @return The id, or an error status if it cannot be resolved. Use it to
   *   correlate the node with the rest of an agent's state or to key it in
   *   a NodeMap.
   */
  absl::StatusOr<std::string> GetId() const;

  /**
   * @brief Return the underlying chunk store backing this node.
   * @return The ordered storage boundary the reader and writer stream through;
   *   reach for it when you need lower-level access than the Put/Next API
   *   provides.
   */
  [[nodiscard]] std::shared_ptr<stores::ChunkStore> GetChunkStore() const;

  /**
   * @brief Return the registry used to (de)serialize typed values.
   * @return The current serialization registry (may be null).
   */
  [[nodiscard]] std::shared_ptr<data::SerializationRegistry>
  serialization_registry() const;

  /**
   * @brief Replace the serialization registry.
   * @param registry The registry to install.
   * @return OK, or an error status on failure.
   */
  absl::Status SetSerializationRegistry(
      std::shared_ptr<data::SerializationRegistry> registry);

  /**
   * @brief Return the node's chunk store reader (the consuming half).
   * @return The reader, or an error status on failure.
   */
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreReader>> reader();

  /**
   * @brief Return the node's chunk store writer (the producing half).
   * @return The writer, or an error status on failure.
   */
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> writer();

  /**
   * @brief Return a copy of the reader's current options.
   * @return The reader options in effect.
   */
  [[nodiscard]] stores::ChunkStoreReaderOptions GetReaderOptions() const;

  /**
   * @brief Replace the reader options.
   * @param options The new reader options.
   * @return OK, or an error status on failure.
   */
  absl::Status SetReaderOptions(stores::ChunkStoreReaderOptions options);

  /**
   * @brief Rewind/reconfigure the reader (e.g. to re-read from an offset).
   * @param options Optional new reader options; the existing options are
   *   kept when omitted.
   * @return OK, or an error status on failure.
   */
  absl::Status ResetReader(
      std::optional<stores::ChunkStoreReaderOptions> options = std::nullopt);

  /**
   * @brief Return a copy of the writer's current options.
   * @return The writer options in effect.
   */
  [[nodiscard]] stores::ChunkStoreWriterOptions GetWriterOptions() const;

  /**
   * @brief Replace the writer options.
   * @param options The new writer options.
   * @return OK, or an error status on failure.
   */
  absl::Status SetWriterOptions(stores::ChunkStoreWriterOptions options);

  /**
   * @brief Return the current status of the node's reader.
   * @return Whether the consuming end is healthy, has completed, or has
   *   failed while streaming.
   */
  [[nodiscard]] absl::Status GetReaderStatus() const;

  /**
   * @brief Return the current status of the node's writer.
   * @return Whether the producing end is healthy, has completed, or has
   *   failed while streaming.
   */
  [[nodiscard]] absl::Status GetWriterStatus() const;

  /**
   * @brief Return the status the writer was aborted with.
   * @return The abort status, or nullopt if the writer has not been
   *   aborted. Use it to surface why a stream was cut short.
   */
  [[nodiscard]] std::optional<absl::Status> GetWriterAbortStatus() const;

  /**
   * @brief Report whether the node can currently accept writes.
   * @return An awaitable that resolves once writability is known; await it
   *   before producing chunks to respect backpressure.
   */
  a11::Future<bool> IsWritable();

  /**
   * @brief Enqueue a raw chunk into the stream.
   * @param chunk The chunk to admit.
   * @param seq Optional explicit sequence number; assigned in order when
   *   omitted.
   * @param final Set true on the last chunk to establish the logical final
   *   sequence. This does not close the writer.
   * @return An awaitable that resolves to the sequence number once the backing
   *   store accepts the chunk. Attached stream sends are attempted during the
   *   flush but do not acknowledge remote delivery.
   */
  a11::Future<std::uint32_t> PutChunk(
      data::Chunk chunk, std::optional<std::uint32_t> seq = std::nullopt,
      bool final = false);

  /**
   * @brief Enqueue a fragment carrying its own sequence and final flag.
   * @param fragment The fragment to admit.
   * @return An awaitable that resolves to the stored sequence number.
   */
  a11::Future<std::uint32_t> PutFragment(data::NodeFragment fragment);

  /**
   * @brief Serialize and write a typed value to the stream.
   *
   * Encodes `value` via the node's serialization registry and admits the
   * resulting chunk.
   *
   * @tparam T The type of the value being written.
   * @param value The value to encode and write.
   * @param seq Optional explicit sequence number; assigned in order when
   *   omitted.
   * @param final Set true on the last write to establish the logical final
   *   sequence. This does not close the writer.
   * @param mimetype Optional MIME type selecting the encoding.
   * @return An awaitable that resolves to the stored sequence number, or a
   *   failed future if serialization fails.
   */
  /**
   * @brief Write a typed value **without encoding it**.
   *
   * The local fast path. Put() encodes on the way in and NextObject() decodes
   * on the way out, and for a value that never leaves this process both are
   * waste -- decode especially, being the dearer half by an order of magnitude.
   * This admits a chunk carrying the value itself; the bytes are produced only
   * if something actually needs them, which is a peer, a persisting store, or a
   * reader asking for a chunk rather than a value.
   *
   * A reader calling NextObject<T>() with the same @c T gets a copy of the
   * value, having encoded and decoded nothing.
   *
   * Two obligations, and both are already true of anything put in a store:
   *
   *   * **The value must not change afterwards.** A node replays its fragments
   *     to every reader, including readers that attach later, so a mutable value
   *     was never sound here. Consumers are handed copies, so nothing they do
   *     can reach back.
   *   * **@p mimetype is stated rather than derived.** Put() lets the registry
   *     choose it; here the caller says it, because a chunk whose mimetype
   *     differed from the one its bytes would have had would be filtered
   *     differently by `| mime` depending on whether anybody had asked for the
   *     bytes yet. Pass what Put() would have produced.
   *
   * @tparam T Type of the value. Its serialisation tag identifies it to
   *   readers, so it must have one.
   */
  template <typename T>
  requires data::HasSerialTypeTag<T> a11::Future<std::uint32_t> PutObject(
      T value, std::string_view mimetype,
      std::optional<std::uint32_t> seq = std::nullopt, bool final = false) {
    std::shared_ptr<data::SerializationRegistry> registry;
    {
      thread::MutexLock lock(&mu_);
      registry = serialization_registry_;
    }
    return PutChunk(
        data::MakeChunkObject<T>(std::move(value), data::SerialTypeTag<T>(),
                                 std::string(mimetype), registry),
        seq, final);
  }

  template <typename T>
  a11::Future<std::uint32_t> Put(
      const T& value, std::optional<std::uint32_t> seq = std::nullopt,
      bool final = false, std::string_view mimetype = {}) {
    std::shared_ptr<data::SerializationRegistry> registry;
    {
      thread::MutexLock lock(&mu_);
      registry = serialization_registry_;
    }
    absl::StatusOr<data::Chunk> chunk = registry->ToChunk<T>(value, mimetype);
    if (!chunk.ok()) {
      return a11::FailedFuture<std::uint32_t>(chunk.status());
    }
    return PutChunk(std::move(*chunk), seq, final);
  }

  /**
   * @brief End the stream with an explicit null terminator (no value).
   *
   * The ordinary way to finish a node: it marks the logical end of the data so
   * ordered readers stop immediately, and -- unless `options.close` is cleared
   * -- closes the writer so the backing store admits nothing more.
   *
   * With `options.wait` left false the returned awaitable is already resolved
   * and both the write and the close proceed on the writer's pump, which is
   * safe after the producing frame is gone. Nothing is silently dropped: a
   * failed write or close is logged, and remains visible through
   * GetWriterStatus(). Set `options.wait` when the producer must know the store
   * accepted the end of the stream before continuing.
   *
   * @param options How to end the stream; the defaults finalise and close
   *   without waiting.
   * @return An awaitable that resolves once the requested work is done, or
   *   immediately when not waiting.
   */
  a11::Task Finalize(FinalizeOptions options = {});

  /**
   * @brief End the stream with a raw chunk as its final data.
   * @param chunk The last chunk of the stream.
   * @param options How to end the stream; `options.mimetype` is ignored, since
   *   the chunk carries its own.
   * @return An awaitable resolving as described by Finalize(FinalizeOptions).
   */
  a11::Task Finalize(data::Chunk chunk, FinalizeOptions options = {});

  /**
   * @brief Serialize `value` as the stream's final data, then end the stream.
   *
   * The unary case -- a node that carries one result -- and the streaming case
   * where the producer knows which value is the last one and can save
   * transmitting a separate terminator for it.
   *
   * @tparam T The type of the value being written.
   * @param value The value to encode and write as final.
   * @param options How to end the stream.
   * @return An awaitable resolving as described by Finalize(FinalizeOptions),
   *   or a failed awaitable if serialization fails.
   */
  template <typename T>
  a11::Task Finalize(const T& value, FinalizeOptions options = {}) {
    std::shared_ptr<data::SerializationRegistry> registry;
    {
      thread::MutexLock lock(&mu_);
      registry = serialization_registry_;
    }
    absl::StatusOr<data::Chunk> chunk =
        registry->ToChunk<T>(value, options.mimetype);
    if (!chunk.ok()) {
      return a11::FailedTask(chunk.status());
    }
    return Finalize(std::move(*chunk), options);
  }

  /**
   * @brief Read up to `limit` fragments in a single await.
   *
   * The batched counterpart to NextFragment(), with the same end-of-stream
   * marker: a trailing empty optional. It returns whatever is already
   * buffered and waits only when nothing is, so it never trades latency for
   * throughput on a live stream. See ChunkStoreReader::NextMany().
   * @param limit Maximum number of fragments to return.
   * @param timeout How long to wait when no fragments are buffered.
   */
  a11::Future<std::vector<std::optional<data::NodeFragment>>> NextFragments(
      size_t limit, absl::Duration timeout = absl::InfiniteDuration());

  /**
   * @brief Read the next raw fragment.
   * @param timeout How long to wait for a fragment.
   * @return An awaitable that resolves to the next fragment, or nullopt at
   *   end of stream.
   */
  a11::Future<std::optional<data::NodeFragment>> NextFragment(
      absl::Duration timeout = absl::InfiniteDuration());

  /**
   * @brief Read the next fragment without producing bytes for it.
   *
   * Preserves an in-process object for NextObject<T>(); use NextFragment() when
   * the caller needs materialized bytes.
   */
  a11::Future<std::optional<data::NodeFragment>> NextFragmentRaw(
      absl::Duration timeout = absl::InfiniteDuration());

  /**
   * @brief Read the next raw chunk.
   * @param timeout How long to wait for a chunk.
   * @return An awaitable that resolves to the next chunk, or nullopt at end
   *   of stream.
   */
  a11::Future<std::optional<data::Chunk>> NextChunk(
      absl::Duration timeout = absl::InfiniteDuration());

  /**
   * @brief Read and deserialize the next value.
   * @tparam T The type to deserialize into.
   * @param timeout How long to wait for a value.
   * @param mimetype_patterns Optional MIME patterns constraining which
   *   encodings are accepted.
   * @return An awaitable that resolves to the next value, or nullopt at end
   *   of stream.
   */
  template <typename T>
  a11::Future<std::optional<T>> NextObject(
      absl::Duration timeout = absl::InfiniteDuration(),
      std::vector<std::string> mimetype_patterns = {}) {
    std::shared_ptr<AsyncNode> self = shared_from_this();
    return a11::Submit<std::optional<T>>(
        [self = std::move(self), timeout,
         mimetype_patterns = std::move(
             mimetype_patterns)]() mutable -> absl::StatusOr<std::optional<T>> {
          // Raw, so a chunk carrying a value still is one when it gets here.
          // NextFragment() would have materialised it, which is right for every
          // caller that wants bytes and is exactly what this one does not.
          absl::StatusOr<std::optional<data::NodeFragment>> fragment =
              self->NextFragmentRaw(timeout).Await();
          if (!fragment.ok()) {
            return fragment.status();
          }
          if (!fragment->has_value()) {
            return std::nullopt;
          }
          absl::StatusOr<const data::Chunk*> chunk = (*fragment)->GetChunk();
          if (!chunk.ok()) {
            return chunk.status();
          }
          // The fast path: the producer put a T and this reader wants a T, so
          // nothing was encoded and nothing is decoded. Guarded by the tag, not
          // by type identity -- see a11::data::ChunkObject.
          if ((*chunk)->HasObject()) {
            // `if constexpr`, because NextObject is instantiated for types with
            // no tag at all -- a JSON-native value, a bare string -- and those
            // simply have no fast path to take.
            if constexpr (data::HasSerialTypeTag<T>) {
              if (std::optional<T> taken =
                      data::TryTakeObject<T>(**chunk, data::SerialTypeTag<T>());
                  taken.has_value()) {
                return taken;
              }
            }
            // Carrying something else. Fall through to the bytes, which means
            // producing them now: a reader asking for a different type than the
            // producer wrote is exactly when the wire format earns its keep.
            data::Chunk materialised = **chunk;
            ABSL_RETURN_IF_ERROR(materialised.Materialize());
            std::shared_ptr<data::SerializationRegistry> registry;
            {
              thread::MutexLock lock(&self->mu_);
              registry = self->serialization_registry_;
            }
            absl::StatusOr<T> value =
                registry->FromChunk<T>(materialised, mimetype_patterns);
            if (!value.ok()) {
              return value.status();
            }
            return std::optional<T>(std::move(*value));
          }
          if ((*chunk)->IsNull()) {
            if ((*fragment)->continued) {
              return absl::FailedPreconditionError(
                  "A null stream marker must be final");
            }
            return std::nullopt;
          }
          std::shared_ptr<data::SerializationRegistry> registry;
          {
            thread::MutexLock lock(&self->mu_);
            registry = self->serialization_registry_;
          }
          absl::StatusOr<T> value =
              registry->FromChunk<T>(**chunk, mimetype_patterns);
          if (!value.ok()) {
            return value.status();
          }
          return std::optional<T>(std::move(*value));
        });
  }

  /**
   * @brief Wait for the write buffer to drain.
   * @return An awaitable that resolves once the write buffer has drained;
   *   await it to let consumers catch up before pushing more chunks.
   */
  a11::Task WaitForBufferToDrain();

  /**
   * @brief Flush all buffered chunks and close the writer, without finality.
   *
   * The specialised half of Finalize(): a storage-lifecycle operation and not
   * an end-of-data marker. It appends no fragment and chooses no final
   * sequence number, so an ordered reader learns only that nothing more can
   * arrive -- which is all a producer that cannot say which chunk was last,
   * such as a log, is able to promise. Prefer Finalize() everywhere else.
   *
   * Closing always drains: making a closure with nothing to mark asynchronous
   * would buy nothing, so this awaitable resolves when the store is closed.
   *
   * Attached streams learn of the closure: the writer follows the last teed
   * batch with a closure marker, so a peer holding a mirror of this node closes
   * its write half too and its readers reach a clean end.
   * @return An awaitable that resolves once every produced chunk has been
   *   flushed and the backing store is closed to further writes.
   */
  a11::Task Close();

  /**
   * @brief Fail the stream with an error status.
   * @param status The error to abort with.
   * @return An awaitable that resolves once the abort has propagated, so
   *   consumers observe the error instead of a normal end-of-stream.
   */
  a11::Task AbortWithStatus(absl::Status status);

  /**
   * @brief Tee this node's stored chunks onto a wire stream.
   *
   * `WireStream::Send()` confirms local transport admission, not receipt by
   * the remote agent. A send failure stops later writes but cannot revoke the
   * current batch's store confirmations. Closing the writer also tees a
   * closure marker to every attached stream.
   * @param stream The transport to attach; kept alive for the node's
   *   lifetime.
   * @return OK, or an error status on failure.
   */
  absl::Status AttachStream(std::shared_ptr<net::WireStream> stream);

  /**
   * @brief Stop mirroring chunks over a previously attached wire stream.
   * @param stream The transport to detach.
   * @return OK, or an error status on failure.
   */
  absl::Status DetachStream(const std::shared_ptr<net::WireStream>& stream);

  /**
   * @brief Cancel the reader, unblocking pending Next* awaits.
   */
  void CancelReader();

  /**
   * @brief Cancel the writer, unblocking pending Put/drain awaits.
   */
  void CancelWriter();

  /**
   * @brief Cancel both reader and writer, tearing down all pending
   * streaming operations on the node at once.
   */
  void Cancel();

 private:
  AsyncNode(std::shared_ptr<stores::ChunkStore> store,
            std::shared_ptr<data::SerializationRegistry> registry,
            stores::ChunkStoreReaderOptions reader_options,
            stores::ChunkStoreWriterOptions writer_options)
      : store_(std::move(store)),
        serialization_registry_(std::move(registry)),
        reader_options_(reader_options),
        writer_options_(writer_options) {}

  const std::shared_ptr<stores::ChunkStore> store_;
  mutable thread::Mutex mu_;
  std::shared_ptr<data::SerializationRegistry> serialization_registry_
      ABSL_GUARDED_BY(mu_);
  stores::ChunkStoreReaderOptions reader_options_ ABSL_GUARDED_BY(mu_);
  stores::ChunkStoreWriterOptions writer_options_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<stores::ChunkStoreReader> reader_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<stores::ChunkStoreWriter> writer_ ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::nodes

#endif  // A11_NODES_ASYNC_NODE_H_

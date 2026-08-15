// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   A write cursor over a ChunkStore that admits chunks into the store in
 *   sequence, applying backpressure and optionally mirroring to wire streams.
 */

#ifndef A11_STORES_CHUNK_STORE_WRITER_H_
#define A11_STORES_CHUNK_STORE_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"

namespace a11::net {
class WireStream;
}  // namespace a11::net

namespace a11::stores {

/**
 * @brief
 *   Tunables controlling how a ChunkStoreWriter batches and buffers chunks.
 *
 * Configures the starting offset, how many chunks are flushed to the store per
 * batch, and an optional bound on the in-flight write buffer that governs
 * backpressure. Sticky mimetypes can omit repeated MIME metadata while keeping
 * explicit sequence discontinuities self-describing.
 */
struct ChunkStoreWriterOptions {
  /// Sequence number at which writing begins.
  std::uint32_t offset = 0;
  /// Maximum number of chunks flushed to the store per batch.
  std::uint64_t max_chunks_to_write_at_once = 8;
  /// Optional bound on the in-flight write buffer size.
  std::optional<std::uint64_t> num_chunks_to_buffer;
  /// Whether repeated contiguous chunk mimetypes are omitted when writing.
  bool sticky_mimetype = false;

  /** @brief
   *    Validate that the options are internally consistent.
   *
   *  @return
   *    OK if the options are valid, otherwise an error status.
   */
  absl::Status Validate() const;
};

/**
 * @brief
 *   The pair of awaitables returned when enqueuing a chunk, separating queue
 *   admission from backing-store confirmation.
 *
 * The Python API distinguishes admission to the bounded native queue from
 * backing-store confirmation. Native callers that only need confirmation can
 * continue to use ChunkStoreWriter::PutChunk.
 */
struct ChunkStoreWrite {
  /// Resolves once the chunk is admitted into the bounded queue (backpressure).
  a11::Task admitted;
  /// Resolves with the assigned sequence once the backing store accepts it.
  a11::Future<std::uint32_t> confirmation;
};

/**
 * @brief
 *   A buffered, backpressured write cursor over a ChunkStore.
 *
 * A writer admits `data::Chunk` values into a store in sequence, pacing the
 * producer through a bounded buffer sized by its `ChunkStoreWriterOptions`. A
 * background flush loop drains the queue into the store; persisted fragments
 * can additionally be mirrored to attached wire streams. It is usable directly
 * though most code reaches one through a node. State is reference-counted and
 * held via shared_ptr.
 */
class ChunkStoreWriter {
 public:
  /** @brief
   *    Create a writer over `store`.
   *
   *  @param store
   *    The store to persist chunks to.
   *  @param options
   *    Tuning for offset, sticky-mimetype compression, and how much is
   *    buffered/flushed at once.
   *  @return
   *    A shared, ready-to-use writer, or an error status if the options are
   *    invalid.
   */
  static absl::StatusOr<std::shared_ptr<ChunkStoreWriter>> Create(
      std::shared_ptr<ChunkStore> store, ChunkStoreWriterOptions options = {});

  ~ChunkStoreWriter() = default;

  /** @brief
   *    Start the background flush loop if it is not already running.
   *
   *  Writing normally starts the loop lazily; call this to begin flushing
   *  before the first chunk is enqueued.
   */
  void EnsureStarted();

  /** @brief
   *    Run the flush loop now, on the calling thread, if it is idle.
   *
   *  Where EnsureStarted() hands the flush to a worker -- a scheduler hop, and
   *  for a caller who then awaits the confirmation an event-loop turn before
   *  the store is even asked -- this does the work here. A store that accepts
   *  the batch inline, as LocalChunkStore does, has confirmed the write by the
   *  time this returns, so the confirmation is already resolved and its awaiter
   *  never suspends.
   *
   *  Call it when you are about to wait for a write, not when you make one.
   *  Enqueuing deliberately does not flush: a producer pacing itself against
   *  the admission buffer is running ahead of its store and should not be made
   *  to do the store's work. The Python binding flushes from the
   *  confirmation's first `await` for that reason.
   *
   *  A store that cannot answer inline leaves the operation in flight and this
   *  is a no-op, which is what keeps batching where batching is worth
   *  something: chunks enqueued while a slow store is busy accumulate and go
   *  out in one PutMany.
   */
  void Flush();

  /** @brief
   *    Enqueue a chunk, exposing backpressure and confirmation separately.
   *
   *  Unlike PutChunk(), this returns both awaitables: `admitted` resolves once
   *  the chunk is accepted into the bounded queue and `confirmation` resolves
   *  with the sequence assigned by the backing store. Await admission to pace
   *  production and confirmation to know the store accepted the write.
   *
   *  @param chunk
   *    The chunk to enqueue.
   *  @param seq
   *    Optional explicit sequence number; assigned automatically if unset.
   *  @param final
   *    Whether this chunk establishes the logical final sequence. This does
   *    not close the writer or backing store.
   *  @param ensure_started
   *    Whether to start the flush loop as part of enqueuing.
   *  @return
   *    A ChunkStoreWrite holding the admission and confirmation awaitables.
   */
  ChunkStoreWrite EnqueueChunk(data::Chunk chunk,
                               std::optional<std::uint32_t> seq = std::nullopt,
                               bool final = false, bool ensure_started = true);

  /** @brief
   *    Write a chunk and await backing-store confirmation.
   *
   *  Convenience path for callers that only need confirmation, without
   *  observing queue admission separately.
   *
   *  @param chunk
   *    The chunk to write.
   *  @param seq
   *    Optional explicit sequence number; assigned automatically if unset.
   *  @param final
   *    Whether this chunk establishes the logical final sequence. This does
   *    not close the writer or backing store.
   *  @return
   *    An awaitable that resolves with the stored sequence number.
   */
  a11::Future<std::uint32_t> PutChunk(
      data::Chunk chunk, std::optional<std::uint32_t> seq = std::nullopt,
      bool final = false);

  /// @return The writer's terminal status, or empty while still open.
  [[nodiscard]] std::optional<absl::Status> GetStatus() const;
  /// @return The status the writer was aborted with, or empty if not aborted.
  [[nodiscard]] std::optional<absl::Status> GetAbortStatus() const;
  /// @return Whether the writer still accepts chunks (false once drained,
  ///         closed, or aborted).
  [[nodiscard]] bool IsWritable() const;

  /** @brief
   *    Stop the writer immediately, discarding any queued chunks.
   *
   *  @return
   *    An awaitable that resolves once teardown completes.
   */
  a11::Task Cancel();

  /** @brief
   *    Flush every queued chunk, then close the writer.
   *
   *  This closes the backing store to further writes, but it does not append a
   *  final fragment. The producer must mark its last chunk `final=true` (or
   *  write a null final chunk through AsyncNode) before draining when readers
   *  need a final sequence number to identify the logical end of the stream.
   *
   *  Attached streams are told: after the last batch is flushed and teed, the
   *  writer sends one closure marker -- a status chunk carrying
   *  `data::kCloseAttribute` and the OK close status -- so a mirror of this
   *  node on the far side closes its own write half. A peer that cannot be
   *  reached does not keep the store open; the send error surfaces as this
   *  writer's terminal status and through the returned awaitable.
   *
   *  @return
   *    An awaitable that resolves once the flush and storage close complete.
   */
  a11::Task DrainAndClose();

  /** @brief
   *    Abort the writer with an error status.
   *
   *  Propagates a failure downstream so readers observe the error instead of a
   *  clean end-of-stream.
   *
   *  @param status
   *    The error status to record.
   *  @return
   *    An awaitable that resolves once teardown completes.
   */
  a11::Task AbortWithStatus(absl::Status status);

  /** @brief
   *    Wait until the in-flight write buffer empties.
   *
   *  A backpressure checkpoint before enqueuing more chunks.
   *
   *  @return
   *    An awaitable that resolves once the buffer has drained.
   */
  a11::Task WaitForBufferToDrain();

  /** @brief
   *    Mirror persisted fragments to an additional wire stream.
   *
   *  After the store accepts a batch, the writer calls `WireStream::Send()` on
   *  each attached stream, and DrainAndClose() follows the last batch with a
   *  closure marker. A successful send means local transport admission, not
   *  remote delivery. A transport failure stops subsequent writes but cannot
   *  revoke store confirmations returned for the current batch. The writer
   *  keeps the stream alive while attached.
   *
   *  @param stream
   *    The wire stream to fan output out to.
   *  @return
   *    OK if the stream was attached, otherwise an error status.
   */
  absl::Status AttachStream(std::shared_ptr<net::WireStream> stream);

  /** @brief
   *    Stop mirroring fragments to a previously attached wire stream.
   *
   *  @param stream
   *    The stream to detach.
   *  @return
   *    OK if the stream was detached, or an error if it was not attached.
   */
  absl::Status DetachStream(const std::shared_ptr<net::WireStream>& stream);

  /// @return The store this writer persists chunks to.
  [[nodiscard]] std::shared_ptr<ChunkStore> store() const;
  /// @return The options this writer was created with.
  [[nodiscard]] ChunkStoreWriterOptions options() const;
  /// @return The number of chunks currently waiting in the flush queue.
  [[nodiscard]] size_t queue_size() const;

 private:
  struct State;

  explicit ChunkStoreWriter(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

}  // namespace a11::stores

#endif  // A11_STORES_CHUNK_STORE_WRITER_H_

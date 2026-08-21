// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   A11's pluggable storage interface for streamed node data: an ordered,
 *   appendable log of fragments keyed by sequence number and arrival order.
 */

#ifndef A11_STORES_CHUNK_STORE_H_
#define A11_STORES_CHUNK_STORE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"

namespace a11::stores {

/**
 * @brief
 *   Abstract, pluggable backing store for the data of a node: an ordered,
 *   appendable log of fragments.
 *
 * A ChunkStore is where the fragments of a node actually live -- an ordered,
 * appendable log of `data::NodeFragment` values that writers append to and
 * readers pull from, addressable both by sequence number and by arrival
 * order. Everything above it (nodes, readers, writers, actions) is written
 * against this interface, so the storage backend is a pluggable detail.
 *
 * This makes ChunkStore a deliberate extension point. The default
 * `LocalChunkStore` keeps data in memory, but the interface can be
 * implemented (or subclassed) to persist a stream to disk, a database, or a
 * blob store, to inject faults in tests, or to enforce a custom retention
 * policy, without touching any node, action, or session code. Streaming
 * semantics -- buffering, backpressure, cursoring -- live in
 * `ChunkStoreReader` and `ChunkStoreWriter`, which are layered on top.
 *
 * Every data method returns an `a11::Future`: an awaitable that resolves when
 * the operation completes, so callers never block the event loop. Retrieval
 * methods take a deadline and resolve once the fragment is available or the
 * deadline elapses.
 */
class ChunkStore {
 public:
  virtual ~ChunkStore() = default;

  /** @brief
   *    Get the fragment at a sequence number, waiting indefinitely.
   *
   *  Convenience overload equivalent to Get() with an infinite deadline.
   *
   *  @param seq
   *    The sequence number of the fragment to retrieve.
   *  @return
   *    An awaitable that resolves with the requested fragment once it is
   *    available.
   */
  a11::Future<data::NodeFragment> Get(std::uint32_t seq) {
    return Get(seq, absl::InfiniteFuture());
  }

  /** @brief
   *    Get the fragment stored at a sequence number.
   *
   *  As storage may be non-local, errors are surfaced through the resolved
   *  fragment/status. Correct implementations resolve with an error rather
   *  than blocking forever once the fragment can no longer arrive (e.g. writes
   *  were closed with a smaller final sequence number).
   *
   *  @param seq
   *    The sequence number of the fragment to retrieve.
   *  @param deadline
   *    The absolute time after which the wait gives up.
   *  @return
   *    An awaitable that resolves with the fragment once available, or with an
   *    error if the deadline elapses or the fragment can never arrive.
   */
  virtual a11::Future<data::NodeFragment> Get(std::uint32_t seq,
                                              absl::Time deadline) = 0;

  /** @brief
   *    Get a fragment by arrival order, waiting indefinitely.
   *
   *  Convenience overload equivalent to GetByArrivalOrder() with an infinite
   *  deadline.
   *
   *  @param arrival_order
   *    The zero-based rank in which the fragment arrived in the store.
   *  @return
   *    An awaitable that resolves with the requested fragment once available.
   */
  a11::Future<data::NodeFragment> GetByArrivalOrder(
      std::uint64_t arrival_order) {
    return GetByArrivalOrder(arrival_order, absl::InfiniteFuture());
  }

  /** @brief
   *    Get the fragment identified by the order in which it arrived, rather
   *    than by its sequence number.
   *
   *  Useful for replaying fragments in ingestion order regardless of the
   *  sequence numbers assigned to them.
   *
   *  @param arrival_order
   *    The zero-based rank in which the fragment arrived in the store.
   *  @param deadline
   *    The absolute time after which the wait gives up.
   *  @return
   *    An awaitable that resolves with the fragment once available, or with an
   *    error if the deadline elapses.
   */
  virtual a11::Future<data::NodeFragment> GetByArrivalOrder(
      std::uint64_t arrival_order, absl::Time deadline) = 0;

  /** @brief
   *    Get the next logical-sequence fragment, waiting indefinitely.
   *
   *  Convenience overload equivalent to Next() with an infinite deadline and a
   *  limit of one.
   *
   *  @return
   *    An awaitable that resolves with at most one fragment, or a nullopt
   *    sentinel once the store reaches a clean logical end.
   */
  a11::Future<std::vector<std::optional<data::NodeFragment>>> Next() {
    return Next(absl::InfiniteFuture(), 1);
  }

  /** @brief
   *    Get the next logical-sequence fragment before a deadline.
   *
   *  Convenience overload equivalent to Next() with a limit of one.
   *
   *  @param deadline
   *    The absolute time after which the wait gives up.
   *  @return
   *    An awaitable that resolves with at most one fragment, or a nullopt
   *    sentinel at a clean logical end.
   */
  a11::Future<std::vector<std::optional<data::NodeFragment>>> Next(
      absl::Time deadline) {
    return Next(deadline, 1);
  }

  /** @brief
   *    Get up to `limit` fragments from the shared logical-sequence cursor.
   *
   *  `Next()` advances through sequence numbers 0, 1, 2, and so on. It waits
   *  at a gap rather than switching to ingestion order; use
   *  `GetByArrivalOrder()` for that view. After the final sequence or a clean
   *  write closure, the result includes a nullopt end sentinel. Call
   *  repeatedly to follow the store as it grows.
   *
   *  @param deadline
   *    The absolute time after which the wait gives up.
   *  @param limit
   *    The maximum number of fragments to return in one call.
   *  @return
   *    An awaitable that resolves with up to `limit` data fragments; a clean
   *    end may append a nullopt sentinel.
   */
  virtual a11::Future<std::vector<std::optional<data::NodeFragment>>> Next(
      absl::Time deadline, size_t limit) = 0;

  /**
   * @brief Whether this store can hold a chunk that carries a value.
   *
   * Override this for stores that retain in-process ChunkObject values without
   * serialization. Callers materialize values before writing to other stores.
   */
  [[nodiscard]] virtual bool HoldsObjects() const { return false; }

  /**
   * @brief Append one fragment to the log.
   * @param fragment Fragment to append.
   * @return An awaitable resolving to its assigned sequence number.
   */
  virtual a11::Future<std::uint32_t> Put(data::NodeFragment fragment) = 0;

  /** @brief
   *    Append several fragments in one batch.
   *
   *  Preferred over repeated Put() calls when many fragments are emitted at
   *  once, to reduce round-trips.
   *
   *  @param fragments
   *    The fragments to append, in order.
   *  @return
   *    An awaitable that resolves with the sequence numbers assigned to the
   *    fragments, in the same order.
   */
  virtual a11::Future<std::vector<std::uint32_t>> PutMany(
      std::vector<data::NodeFragment> fragments) = 0;

  /** @brief
   *    Erase the payload of the fragment at a sequence number while keeping
   *    its slot.
   *
   *  Used to reclaim memory for fragments that have already been consumed
   *  without disturbing the sequence numbering.
   *
   *  @param seq
   *    The sequence number whose payload should be cleared.
   *  @return
   *    An awaitable that resolves with the (now payload-cleared) fragment.
   */
  virtual a11::Future<data::NodeFragment> ClearData(std::uint32_t seq) = 0;

  /** @brief
   *    Translate an arrival order into the sequence number of that fragment.
   *
   *  @param arrival_order
   *    The zero-based rank in which the fragment arrived in the store.
   *  @return
   *    An awaitable that resolves with the corresponding sequence number.
   */
  virtual a11::Future<std::uint32_t> GetSeqForArrivalOrder(
      std::uint64_t arrival_order) = 0;

  /** @brief
   *    Get the sequence number explicitly marked as the final fragment.
   *
   *  @return
   *    An awaitable that resolves with the logical final sequence number, or
   *    an empty optional when no fragment has declared finality.
   *
   *  Finality and write closure are separate state transitions. A producer may
   *  mark a fragment final before closing the store, and
   *  CloseWritesWithStatus() does not invent a final fragment.
   */
  virtual a11::Future<std::optional<std::uint32_t>> GetFinalSeq() = 0;

  /** @brief
   *    Seal the store against further writes with a terminal status.
   *
   *  Convenience overload that does not return the previously recorded status
   *  if the store is already closed.
   *
   *  @param status
   *    The terminal status to record; readers awaiting Next() are released.
   *  @return
   *    An awaitable that resolves when the close completes.
   */
  a11::Future<absl::Status> CloseWritesWithStatus(absl::Status status) {
    return CloseWritesWithStatus(std::move(status), false);
  }

  /** @brief
   *    Seal the store against further writes with a terminal status.
   *
   *  Once closed, readers blocked on Next() are released. Call this when a
   *  producer has finished (or failed).
   *
   *  @param status
   *    The terminal status to record.
   *  @param return_status_if_already_closed
   *    When true, a second close resolves with the status recorded by the
   *    first close instead of overwriting it.
   *  @return
   *    An awaitable that resolves when the close completes.
   */
  virtual a11::Future<absl::Status> CloseWritesWithStatus(
      absl::Status status, bool return_status_if_already_closed) = 0;

  /** @brief
   *    Get the number of fragments currently in the store.
   *
   *  @return
   *    An awaitable that resolves with the current fragment count.
   */
  virtual a11::Future<size_t> Size() = 0;

  /** @brief
   *    Get the store's node identifier.
   *
   *  @return
   *    The node id, or an error status if the identifier is unavailable.
   */
  virtual absl::StatusOr<std::string> GetId() const = 0;
};

}  // namespace a11::stores

#endif  // A11_STORES_CHUNK_STORE_H_

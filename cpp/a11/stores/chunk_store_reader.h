// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   A read cursor over a ChunkStore that pulls fragments out in order (or by
 *   arrival), buffering ahead per its options.
 */

#ifndef A11_STORES_CHUNK_STORE_READER_H_
#define A11_STORES_CHUNK_STORE_READER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"

namespace a11::stores {

/**
 * @brief
 *   Tunables controlling how a ChunkStoreReader delivers and buffers
 *   fragments.
 *
 * Configures ordering, prefetch depth, the starting offset, whether fragments
 * are removed from the store as they are read, an optional cap on the total
 * number read, and whether ordered reads expand sticky mimetypes.
 */
struct ChunkStoreReaderOptions {
  /// Whether fragments are delivered strictly in sequence order.
  bool ordered = true;
  /// Whether fragments are removed from the store as they are read.
  bool pop_chunks = false;
  /// Maximum number of fragments to prefetch into the buffer.
  std::uint64_t num_chunks_to_buffer = 1024;
  /// Sequence number at which reading begins.
  std::uint32_t offset = 0;
  /// Optional cap on the total number of fragments to read.
  std::optional<std::uint64_t> max_chunks_to_read;
  /// Whether ordered reads inherit the last explicitly set chunk mimetype.
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
 *   An ordered, buffered read cursor over a ChunkStore.
 *
 * A reader pulls `data::NodeFragment` values out of a store, prefetching ahead
 * into a bounded buffer per its `ChunkStoreReaderOptions`. A background pump
 * fetches fragments; callers await Next() to consume them. It is a first-class
 * object that can be driven directly, though most code reaches one through a
 * node. State is reference-counted and held via shared_ptr.
 */
class ChunkStoreReader {
 public:
  /** @brief
   *    Create a reader over `store`.
   *
   *  @param store
   *    The store to read fragments from.
   *  @param options
   *    Tuning for ordering, buffering, offset, pop-on-read behavior, and
   *    sticky-mimetype expansion.
   *  @return
   *    A shared, ready-to-use reader, or an error status if the options are
   *    invalid.
   */
  static absl::StatusOr<std::shared_ptr<ChunkStoreReader>> Create(
      std::shared_ptr<ChunkStore> store, ChunkStoreReaderOptions options = {});

  ~ChunkStoreReader() = default;

  /** @brief
   *    Start the background read pump if it is not already running.
   *
   *  Reading normally starts the pump lazily; call this to begin buffering
   *  before the first Next().
   */
  void EnsureStarted();

  /** @brief
   *    Stop the background read pump.
   *
   *  Pending Next() awaitables are resolved and no further fragments are
   *  fetched.
   */
  void Cancel();

  /** @brief
   *    Get the reader's current status.
   *
   *  @return
   *    The status distinguishing a healthy stream from one that has failed or
   *    ended.
   */
  absl::Status GetStatus() const;

  /** @brief
   *    Await completion of the background read pump.
   *
   *  @return
   *    An awaitable that resolves once the reader has drained the store or
   *    been cancelled.
   */
  a11::Task Done() const;

  /** @brief
   *    Get the next fragment from the store.
   *
   *  @param timeout
   *    The maximum duration to wait before giving up.
   *  @return
   *    An awaitable that resolves with the next fragment, or an empty optional
   *    at end of stream.
   */
  a11::Future<std::optional<data::NodeFragment>> Next(
      absl::Duration timeout = absl::InfiniteDuration());

  /// @return The store this reader draws fragments from.
  [[nodiscard]] std::shared_ptr<ChunkStore> store() const;
  /// @return The options this reader was created with.
  [[nodiscard]] ChunkStoreReaderOptions options() const;
  /// @return The number of prefetched fragments currently buffered.
  [[nodiscard]] size_t buffer_size() const;

 private:
  struct State;

  explicit ChunkStoreReader(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

}  // namespace a11::stores

#endif  // A11_STORES_CHUNK_STORE_READER_H_

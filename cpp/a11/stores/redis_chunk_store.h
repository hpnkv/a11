// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A Redis Streams implementation of the ChunkStore contract.
 */

#ifndef A11_STORES_REDIS_CHUNK_STORE_H_
#define A11_STORES_REDIS_CHUNK_STORE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"
#include "redis/client.h"

namespace a11::stores {

/** Storage layout policy for RedisChunkStore. */
struct RedisChunkStoreOptions {
  /** Prefix before the per-node Redis Cluster hash tag. */
  std::string key_prefix = "a11:";

  /** Raw chunk bytes larger than this are moved to the separate blob hash. */
  size_t inline_data_threshold = 256 * 1024;

  /// Validate the key prefix and inline-data threshold.
  absl::Status Validate() const;
  /// Read storage policy from the A11_REDIS_CHUNK_STORE_* environment values.
  static absl::StatusOr<RedisChunkStoreOptions> FromEnvironment();

  friend bool operator==(const RedisChunkStoreOptions&,
                         const RedisChunkStoreOptions&) = default;
};

/** The sharding-safe Redis keys owned by one node stream. */
struct RedisChunkStoreKeys {
  std::string metadata;        ///< Node state hash.
  std::string stream;          ///< Ordered chunk/control Redis Stream.
  std::string sequence_index;  ///< Sequence-to-stream-entry hash.
  std::string arrival_index;   ///< Arrival-order-to-sequence hash.
  std::string blobs;           ///< Encoded chunks stored outside stream fields.
  std::string events;  ///< Pub/Sub invalidation channel for waiting readers.

  /** Keys in the stable order expected by the store's Lua state machine. */
  [[nodiscard]] std::vector<std::string> ScriptKeys() const;

  friend bool operator==(const RedisChunkStoreKeys&,
                         const RedisChunkStoreKeys&) = default;
};

/** Node-level state read directly from the metadata hash, without chunks. */
struct RedisChunkStoreMetadata {
  std::string id;                      ///< Node id persisted with the key set.
  bool closed = false;                 ///< Whether writes have been sealed.
  std::optional<absl::Status> status;  ///< Terminal status when closed.
  std::optional<std::uint32_t>
      final_seq;    ///< Logical final fragment, if marked.
  size_t size = 0;  ///< Number of fragment slots currently indexed.
  std::uint64_t total_chunks_put = 0;    ///< Lifetime successful write count.
  std::uint64_t next_cursor = 0;         ///< Cursor used by Next() reads.
  std::optional<std::uint32_t> max_seq;  ///< Greatest assigned sequence.
  std::uint64_t revision = 0;            ///< Monotonic state-change revision.
};

/**
 * A persistent, multi-process ChunkStore backed by Redis Streams.
 *
 * Every key for a node contains the same Redis Cluster hash tag, so each Lua
 * operation is valid on both standalone and sharded Redis deployments. The
 * metadata hash exposes node identity, closure, final sequence, counts and
 * cursors without walking the stream. Chunks up to the configured threshold
 * are inline stream fields; larger encoded chunks live in a separate blob hash
 * and the stream records `storage=redis` plus a reference. The storage-kind
 * field reserves `s3` for a future S3-compatible blob implementation.
 *
 * Writes, closure and tombstoning are validated and committed by one Lua
 * invocation. A getter first performs an optimistic atomic lookup. Only when
 * it must wait does it subscribe and recheck state before waiting on a
 * broadcast generation, eliminating the lookup/notification race without a
 * blocking Redis command or a fiber parked on a foreign lock.
 */
class RedisChunkStore final : public ChunkStore {
 private:
  struct ConstructorToken {};

 public:
  /// Create a store with an injected client and explicit storage policy.
  static absl::StatusOr<std::shared_ptr<RedisChunkStore>> Create(
      std::string node_id, std::shared_ptr<redis::Client> client,
      RedisChunkStoreOptions options);

  /// Create a store with an injected client and environment/default policy.
  static absl::StatusOr<std::shared_ptr<RedisChunkStore>> Create(
      std::string node_id, std::shared_ptr<redis::Client> client);

  /** Create with the process-global, environment-configured Redis client. */
  static absl::StatusOr<std::shared_ptr<RedisChunkStore>> Create(
      std::string node_id);

  ~RedisChunkStore() override = default;

  using ChunkStore::CloseWritesWithStatus;
  using ChunkStore::Get;
  using ChunkStore::GetByArrivalOrder;
  using ChunkStore::Next;

  a11::Future<data::NodeFragment> Get(std::uint32_t seq,
                                      absl::Time deadline) override;
  a11::Future<data::NodeFragment> GetByArrivalOrder(
      std::uint64_t arrival_order, absl::Time deadline) override;
  a11::Future<std::vector<std::optional<data::NodeFragment>>> Next(
      absl::Time deadline, size_t limit) override;
  a11::Future<std::uint32_t> Put(data::NodeFragment fragment) override;
  a11::Future<std::vector<std::uint32_t>> PutMany(
      std::vector<data::NodeFragment> fragments) override;
  a11::Future<data::NodeFragment> ClearData(std::uint32_t seq) override;
  a11::Future<std::uint32_t> GetSeqForArrivalOrder(
      std::uint64_t arrival_order) override;
  a11::Future<std::optional<std::uint32_t>> GetFinalSeq() override;
  a11::Future<absl::Status> CloseWritesWithStatus(
      absl::Status status, bool return_status_if_already_closed) override;
  a11::Future<size_t> Size() override;
  absl::StatusOr<std::string> GetId() const override;

  /** Ensure that the metadata hash exists, without writing chunk data. */
  a11::Task Initialize();

  /** Read all node-level metadata without iterating over stream entries. */
  a11::Future<RedisChunkStoreMetadata> GetMetadata();

  /// Return the shared Redis client used for commands and subscriptions.
  [[nodiscard]] std::shared_ptr<redis::Client> client() const {
    return client_;
  }

  /// Return the validated storage policy captured at construction.
  [[nodiscard]] const RedisChunkStoreOptions& options() const {
    return options_;
  }

  /// Return the sharding-safe key set owned by this node.
  [[nodiscard]] const RedisChunkStoreKeys& keys() const { return keys_; }

  RedisChunkStore(ConstructorToken, std::string node_id,
                  std::shared_ptr<redis::Client> client,
                  RedisChunkStoreOptions options, RedisChunkStoreKeys keys)
      : node_id_(std::move(node_id)),
        client_(std::move(client)),
        options_(std::move(options)),
        keys_(std::move(keys)) {}

 private:
  enum class ReadKind { kSequence, kArrivalOrder };

  a11::Future<data::NodeFragment> Read(ReadKind kind, std::uint64_t value,
                                       absl::Time deadline);

  const std::string node_id_;
  const std::shared_ptr<redis::Client> client_;
  const RedisChunkStoreOptions options_;
  const RedisChunkStoreKeys keys_;
};

}  // namespace a11::stores

#endif  // A11_STORES_REDIS_CHUNK_STORE_H_

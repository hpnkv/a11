// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A durable, embedded ChunkStore backed by SQLite and blob files.
 */

#ifndef A11_STORES_SQLITE_CHUNK_STORE_H_
#define A11_STORES_SQLITE_CHUNK_STORE_H_

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
#include "a11/stores/internal/sqlite_database.h"

namespace a11::stores {

/** Storage layout policy for SQLiteChunkStore. */
struct SQLiteChunkStoreOptions {
  /**
   * Payloads strictly larger than this move out of the row into a blob file.
   *
   * Keeping small payloads inline avoids a file per chunk; keeping large ones
   * out of the row keeps the database page cache useful and the WAL small.
   */
  size_t inline_data_threshold = 128 * 1024;

  /**
   * Recorded on the node row as its owner. Ownership carries no enforcement
   * yet; it exists so nodes can be attributed and filtered.
   */
  std::string owner_id;

  /** Durability level applied with `PRAGMA synchronous`. */
  internal::SqliteSynchronous synchronous =
      internal::SqliteSynchronous::kNormal;

  /**
   * How often to look for commits made by other processes, or zero to disable.
   *
   * SQLite's change hooks are per-connection, so a writer in another process
   * cannot wake a reader parked in this one. Zero -- the default -- means
   * in-process notification only, which is exact and never polls.
   */
  absl::Duration cross_process_poll_interval = absl::ZeroDuration();

  /**
   * How long an unreferenced blob must survive before a sweep may remove it.
   */
  absl::Duration blob_grace_period = absl::Hours(1);

  /// Validate the threshold, owner id and durations.
  absl::Status Validate() const;
  /// Read storage policy from the A11_SQLITE_CHUNK_STORE_* environment values.
  static absl::StatusOr<SQLiteChunkStoreOptions> FromEnvironment();

  friend bool operator==(const SQLiteChunkStoreOptions&,
                         const SQLiteChunkStoreOptions&) = default;
};

/** Node-level state read from the node row, without touching any fragment. */
struct SQLiteChunkStoreMetadata {
  std::string id;                          ///< Node id, as stored.
  std::string owner_id;                    ///< Recorded owner, possibly empty.
  bool closed = false;                     ///< Whether writes have been sealed.
  std::optional<absl::Status> status;      ///< Terminal status when closed.
  std::optional<std::uint32_t> final_seq;  ///< Logical final fragment, if any.
  size_t size = 0;                     ///< Fragment slots, tombstones included.
  std::uint64_t total_chunks_put = 0;  ///< Lifetime accepted write count.
  std::uint64_t next_cursor = 0;  ///< Next seq the shared Next() will want.
  std::uint64_t data_bytes = 0;   ///< Cached total of stored payload bytes.
  std::optional<std::uint32_t> max_seq;  ///< Greatest assigned sequence.
  std::uint64_t revision = 0;            ///< Monotonic state-change revision.
  absl::Time created_at = absl::InfinitePast();  ///< First accepted write.
  absl::Time updated_at = absl::InfinitePast();  ///< Most recent mutation.
};

class SQLiteChunkStoreFactory;

/**
 * A persistent ChunkStore backed by one SQLite database and a blob directory.
 *
 * The layout under the storage root is:
 *
 * ```
 * ./store.sqlite
 * ./blobs/939f2184-db19-4dd0-b949-bb31c5eadcf8
 * ./blobs/7ee4a05e-f439-4e5f-bb97-8d1388960f29
 * ```
 *
 * Node rows carry the shared producer and consumer cursors, closure state and
 * cached counters, so `Size()` and `GetFinalSeq()` are single-row reads. Each
 * fragment is decomposed into columns rather than stored as an opaque blob, so
 * ordinary SQL can filter by owner, timestamp or node reference. Payloads above
 * the inline threshold live in `blobs/` under a UUID name recorded in the row.
 *
 * Unlike `LocalChunkStore` and `RedisChunkStore`, this backend accepts
 * `data::NodeRef` payloads: their target, offset and length become indexed
 * columns, which is what makes traversal between nodes a query rather than an
 * application-level walk.
 *
 * Every mutation is one `BEGIN IMMEDIATE` transaction, so a batch either lands
 * whole or not at all. Readers never poll: they snapshot a per-node generation
 * event, run an optimistic read, and park on that event when the fragment they
 * want has not arrived, which a committing writer then fires.
 */
class SQLiteChunkStore final : public ChunkStore {
 private:
  struct ConstructorToken {};

 public:
  /**
   * @brief Create a store for @p node_id under the process-default root.
   *
   * @param node_id
   *   The node whose fragment log this store backs.
   * @return
   *   The store, or an error if the id or storage root is unusable.
   */
  static absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> Create(
      std::string node_id);

  /**
   * @brief Create a store for @p node_id under an explicit root.
   *
   * @param node_id
   *   The node whose fragment log this store backs.
   * @param root
   *   Directory holding `store.sqlite` and `blobs/`; created when absent.
   * @return
   *   The store, or an error if the id or storage root is unusable.
   */
  static absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> Create(
      std::string node_id, std::string root);

  /**
   * @brief Create a store for @p node_id with an explicit root and policy.
   *
   * @param node_id
   *   The node whose fragment log this store backs.
   * @param root
   *   Directory holding `store.sqlite` and `blobs/`; created when absent.
   * @param options
   *   Storage policy; per-root settings apply only on first open of that root.
   * @return
   *   The store, or an error if the id, root or options are unusable.
   */
  static absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> Create(
      std::string node_id, const std::string& root,
      SQLiteChunkStoreOptions options);

  ~SQLiteChunkStore() override = default;

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

  /** Read all node-level state in one row read, without listing fragments. */
  a11::Future<SQLiteChunkStoreMetadata> GetMetadata();

  /**
   * @brief
   *   Find fragments elsewhere in the database whose NodeRef points at this
   *   node, newest sequence last.
   *
   * This is the traversal the relational layout exists for: the answer comes
   * from an index on `node_ref_id` rather than from scanning every node.
   *
   * @param limit
   *   Maximum number of referring fragments to return.
   * @return
   *   The referring fragments, resolved and ordered by (node id, seq).
   */
  a11::Future<std::vector<data::NodeFragment>> FindReferrers(size_t limit);

  /** Delete unreferenced blob files older than the configured grace period. */
  a11::Future<size_t> SweepOrphanBlobs();

  /** The validated storage policy captured at construction. */
  [[nodiscard]] const SQLiteChunkStoreOptions& options() const {
    return options_;
  }

  /** The storage root this store reads and writes under. */
  [[nodiscard]] std::string root() const;

  SQLiteChunkStore(ConstructorToken, std::string node_id,
                   std::shared_ptr<internal::SqliteDatabase> database,
                   SQLiteChunkStoreOptions options)
      : node_id_(std::move(node_id)),
        database_(std::move(database)),
        options_(std::move(options)) {}

 private:
  friend class SQLiteChunkStoreFactory;

  enum class ReadKind { kSequence, kArrivalOrder };

  a11::Future<data::NodeFragment> Read(ReadKind kind, std::uint64_t value,
                                       absl::Time deadline);

  const std::string node_id_;
  const std::shared_ptr<internal::SqliteDatabase> database_;
  const SQLiteChunkStoreOptions options_;
};

/**
 * Creates SQLiteChunkStores rooted at one directory.
 *
 * Stores are cheap; the database behind them is not. The factory holds the
 * shared per-root database so that every store it creates reuses one set of
 * connections, worker threads and change hooks. Pass `AsChunkStoreFactory()` to
 * a `NodeMap` or `AsyncNode` to make SQLite the default backing store.
 */
class SQLiteChunkStoreFactory {
 private:
  struct ConstructorToken {};

 public:
  /**
   * @brief Create a factory rooted at @p root with an explicit policy.
   *
   * @param root
   *   Directory holding `store.sqlite` and `blobs/`; created when absent.
   * @param options
   *   Storage policy; per-root settings apply only on first open of that root.
   * @return
   *   The factory, or an error when the root or options are unusable.
   */
  static absl::StatusOr<std::shared_ptr<SQLiteChunkStoreFactory>> Create(
      const std::string& root, SQLiteChunkStoreOptions options);

  /** Create a factory rooted at @p root with the environment/default policy. */
  static absl::StatusOr<std::shared_ptr<SQLiteChunkStoreFactory>> Create(
      const std::string& root);

  /**
   * Create a factory at the default root with the environment/default policy.
   */
  static absl::StatusOr<std::shared_ptr<SQLiteChunkStoreFactory>> Create();

  /**
   * @brief The process-wide default storage root.
   *
   * `$A11_SQLITE_CHUNK_STORE_ROOT` when set, otherwise
   * `$XDG_CACHE_HOME/a11/chunks`, otherwise `~/.cache/a11/chunks`.
   *
   * @return
   *   The default root path.
   */
  static std::string DefaultRoot();

  /**
   * @brief Open a store for @p node_id under this factory's root.
   *
   * @param node_id
   *   The node whose fragment log the store backs.
   * @return
   *   The store, or an error when the id is invalid.
   */
  absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> Open(std::string node_id);

  /** The root this factory creates stores under. */
  [[nodiscard]] std::string root() const;

  /** The validated storage policy applied to every store created here. */
  [[nodiscard]] const SQLiteChunkStoreOptions& options() const {
    return options_;
  }

  /** Delete unreferenced blob files older than the configured grace period. */
  a11::Future<size_t> SweepOrphanBlobs();

  SQLiteChunkStoreFactory(ConstructorToken,
                          std::shared_ptr<internal::SqliteDatabase> database,
                          SQLiteChunkStoreOptions options)
      : database_(std::move(database)), options_(std::move(options)) {}

 private:
  const std::shared_ptr<internal::SqliteDatabase> database_;
  const SQLiteChunkStoreOptions options_;
};

}  // namespace a11::stores

#endif  // A11_STORES_SQLITE_CHUNK_STORE_H_

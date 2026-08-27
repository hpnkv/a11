// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   The per-filesystem-root SQLite resources shared by every SQLiteChunkStore
 *   rooted there: connections, schema, blob files, and reader wakeups.
 *
 * This is an implementation detail of `a11::stores`, not public API. It exists
 * so connections, prepared statements, the SQLite worker threads and the commit
 * hooks are owned once per storage root rather than once per store: a process
 * holding a thousand nodes under one root pays for one database, not a
 * thousand.
 */

#ifndef A11_STORES_INTERNAL_SQLITE_DATABASE_H_
#define A11_STORES_INTERNAL_SQLITE_DATABASE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/functional/any_invocable.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "thread/boost_primitives.h"
#include "thread/selectables.h"

struct sqlite3;
struct sqlite3_stmt;

namespace a11::stores::internal {

/**
 * State the SQLite change hooks write to.
 *
 * Defined in the implementation because the hooks are `extern "C"` free
 * functions and must reach it without being members.
 */
struct SqliteHookState;

/** How much durability the database trades for write throughput. */
enum class SqliteSynchronous {
  /** `PRAGMA synchronous=OFF`: fastest, unsafe across an OS crash. */
  kOff,
  /**
   * `PRAGMA synchronous=NORMAL`: the default. In WAL mode this survives an
   * application crash but may lose the most recent commits on power loss.
   */
  kNormal,
  /** `PRAGMA synchronous=FULL`: every commit is fsynced. */
  kFull,
};

/** Per-root storage policy, shared by every store opened under that root. */
struct SqliteDatabaseOptions {
  /** Durability level applied with `PRAGMA synchronous`. */
  SqliteSynchronous synchronous = SqliteSynchronous::kNormal;

  /**
   * How often to check `PRAGMA data_version` for commits made by *other
   * processes*, or zero to disable.
   *
   * SQLite's hooks are per-connection, so a writer in another process cannot
   * wake a reader parked in this one. Zero -- the default -- means in-process
   * notification only, which is exact and never polls. Set this only when more
   * than one process writes the same root.
   */
  absl::Duration cross_process_poll_interval = absl::ZeroDuration();

  /**
   * How long a blob file must go unreferenced before the sweeper may remove it.
   *
   * A blob is renamed into place *before* the transaction referencing it
   * commits, so a fresh blob is legitimately unreferenced for a short window.
   * The grace period stops the sweeper from deleting one out from under an
   * in-flight commit in another process.
   */
  absl::Duration blob_grace_period = absl::Hours(1);

  /** Reader threads to run, or zero to derive a small count from the CPUs. */
  size_t reader_threads = 0;
};

/** The mutable node-level row, readable without touching the fragment table. */
struct SqliteNodeState {
  /** False when no row exists yet; every other field is then meaningless. */
  bool exists = false;
  std::string owner_id;
  bool closed = false;
  /** The terminal status recorded by CloseWritesWithStatus(). */
  std::optional<absl::Status> status;
  std::optional<std::uint32_t> final_seq;
  /**
   * The shared Next() cursor: the *next expected seq*, not a delivered count.
   */
  std::uint64_t next_cursor = 0;
  /** Producer cursor; doubles as the next arrival order. */
  std::uint64_t put_count = 0;
  size_t size = 0;
  std::uint64_t data_bytes = 0;
  std::optional<std::uint32_t> max_seq;
  std::uint64_t revision = 0;
  absl::Time created_at = absl::InfinitePast();
  absl::Time updated_at = absl::InfinitePast();
};

class SqliteDatabase;

/**
 * @brief A SQLite connection plus the prepared-statement cache belonging to it.
 *
 * Each connection is owned by exactly one worker thread and is never shared, so
 * it needs no lock of its own. Statements are reset when a transaction body
 * finishes: an un-reset statement pins the connection's WAL snapshot, and a
 * reader on a pinned snapshot would keep seeing stale data no matter how many
 * times it was woken.
 */
class SqliteConnection {
 public:
  SqliteConnection(const SqliteConnection&) = delete;
  SqliteConnection& operator=(const SqliteConnection&) = delete;
  ~SqliteConnection();

  /**
   * @brief Prepare (or fetch from the cache) a statement, reset and unbound.
   *
   * @param sql
   *   Statement text, also the cache key; pass a stable literal.
   * @return
   *   A statement borrowed from this connection's cache.
   */
  absl::StatusOr<sqlite3_stmt*> Prepare(std::string_view sql);

  /**
   * @brief Run a statement that returns no rows.
   *
   * @param sql
   *   The statement text.
   * @return
   *   OK, or the translated SQLite error.
   */
  absl::Status Execute(std::string_view sql);

  /** The raw handle, for pragmas and hook installation. */
  [[nodiscard]] sqlite3* handle() const { return handle_; }

 private:
  friend class SqliteDatabase;

  explicit SqliteConnection(sqlite3* handle) : handle_(handle) {}

  /** Reset every cached statement so no WAL snapshot stays pinned. */
  void ResetAll();

  sqlite3* handle_ = nullptr;
  absl::flat_hash_map<std::string, sqlite3_stmt*> statements_;
};

/**
 * @brief
 *   Everything one storage root owns: the database file, the blob directory,
 *   the SQLite worker threads and the wakeup state for parked readers.
 *
 * Instances are shared per canonicalized root through Open(), so two factories
 * aimed at the same directory share one connection set and one set of hooks.
 *
 * ## Threading
 *
 * SQLite calls block their OS thread on `pread`/`fsync`. A11's fiber pool has
 * exactly `hardware_concurrency()` OS threads, and those same threads drain
 * every `thread::PostAt` deadline timer -- so running SQLite directly on a
 * fiber risks stalling the whole runtime, including the timers that would
 * otherwise rescue it. Every SQLite call therefore runs on a dedicated worker
 * thread here, and the calling fiber waits on an `a11::Future` in the ordinary
 * way. This is the same foreign-thread-completes-a-Promise boundary the Redis
 * client already uses from its libuv loop.
 *
 * One writer thread owns the single write connection; each reader thread owns
 * its own read connection. No connection is ever touched by two threads, so
 * none of them needs a lock.
 */
class SqliteDatabase : public std::enable_shared_from_this<SqliteDatabase> {
 private:
  struct ConstructorToken {};

 public:
  /**
   * @brief
   *   Open (or create) the database rooted at @p root, reusing an already-open
   *   instance when one exists for the same canonical path.
   *
   * @param root
   *   Directory holding `store.sqlite` and the `blobs/` subdirectory.
   * @param options
   *   Storage policy; honored only by the call that actually opens the root.
   * @return
   *   The shared database, or an error when the root or schema is unusable.
   */
  static absl::StatusOr<std::shared_ptr<SqliteDatabase>> Open(
      const std::filesystem::path& root, const SqliteDatabaseOptions& options);

  SqliteDatabase(ConstructorToken, std::filesystem::path root,
                 SqliteDatabaseOptions options);
  SqliteDatabase(const SqliteDatabase&) = delete;
  SqliteDatabase& operator=(const SqliteDatabase&) = delete;
  ~SqliteDatabase();

  /** The canonicalized storage root. */
  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

  /** The directory holding externalized payload blobs. */
  [[nodiscard]] std::filesystem::path blobs_directory() const {
    return root_ / "blobs";
  }

  /** The policy this root was opened with. */
  [[nodiscard]] const SqliteDatabaseOptions& options() const {
    return options_;
  }

  /**
   * @brief
   *   Run @p body inside `BEGIN IMMEDIATE` / `COMMIT` on the write connection,
   *   then wake readers of every node the body reports touching.
   *
   * The body returns the node ids it mutated. Any error rolls the transaction
   * back and wakes nobody. `SQLITE_BUSY` is retried by re-running the whole
   * body, so bodies must be idempotent with respect to their own reads.
   *
   * Wake readers only after COMMIT returns. Earlier replacement of the
   * generation event can let a reader observe the new event with pre-commit
   * data and then miss the notification on the old event.
   *
   * @param body
   *   Work to run inside the transaction, returning the touched node ids.
   * @return
   *   OK once the transaction committed and readers were woken.
   */
  absl::Status RunWrite(
      absl::AnyInvocable<
          absl::StatusOr<std::vector<std::string>>(SqliteConnection&)>
          body);

  /**
   * @brief Run @p body on a reader thread, outside any explicit transaction.
   *
   * @param body
   *   Read-only work; results are captured into the caller's own storage.
   * @return
   *   Whatever @p body returned, or a dispatch error.
   */
  absl::Status RunRead(
      absl::AnyInvocable<absl::Status(SqliteConnection&)> body);

  /**
   * @brief
   *   Snapshot the wakeup event for @p node_id before checking node state.
   *
   * Take this snapshot *before* the read that decides whether to wait, and
   * retake it on every retry: a stale handle both misses the next notification
   * and may already be latched.
   *
   * @param node_id
   *   The node whose changes are of interest.
   * @return
   *   The current generation event for that node.
   */
  std::shared_ptr<thread::PermanentEvent> WatchNode(std::string_view node_id);

  /**
   * @brief Write @p data to a new blob file and return its UUID filename.
   *
   * Written under a temporary name, fsynced, renamed into place, then the
   * containing directory is fsynced -- without that last step a crash can leave
   * a committed row pointing at a blob whose directory entry never reached
   * disk. Call this *before* committing the referencing row.
   *
   * @param data
   *   The payload bytes to externalize.
   * @return
   *   The blob's filename (a UUID) to store in `fragments.ref`.
   */
  absl::StatusOr<std::string> WriteBlob(std::string_view data) const;

  /**
   * @brief Read a blob previously written by WriteBlob().
   *
   * @param name
   *   The blob filename from `fragments.ref`.
   * @return
   *   The payload bytes, or `DataLossError` when the file is missing.
   */
  absl::StatusOr<std::string> ReadBlob(std::string_view name) const;

  /**
   * @brief Delete a blob file, tolerating one that is already gone.
   *
   * Only call this *after* the transaction dropping the last reference has
   * committed; unlinking first would lose data if that transaction rolled back.
   *
   * @param name
   *   The blob filename to remove.
   */
  void RemoveBlob(std::string_view name) const;

  /**
   * @brief
   *   Delete blob files that no row references and that are older than the
   *   configured grace period.
   *
   * @return
   *   The number of files removed.
   */
  absl::StatusOr<size_t> SweepOrphanBlobs();

 private:
  class Workers;

  /** Per-node wakeup generation, mirroring LocalChunkStore's event swap. */
  struct NodeWaitState {
    thread::Mutex mu;
    std::shared_ptr<thread::PermanentEvent> changed ABSL_GUARDED_BY(mu) =
        std::make_shared<thread::PermanentEvent>();
  };

  absl::Status Initialize();
  static absl::Status ApplySchema(SqliteConnection& connection);
  absl::StatusOr<std::unique_ptr<SqliteConnection>> OpenConnection(
      bool read_only) const;
  std::shared_ptr<NodeWaitState> WaitStateFor(std::string_view node_id);
  void NotifyNodes(const std::vector<std::string>& node_ids);
  void PollCrossProcess();

  const std::filesystem::path root_;
  const SqliteDatabaseOptions options_;

  std::unique_ptr<SqliteHookState> hooks_;
  std::unique_ptr<Workers> workers_;

  thread::Mutex waiters_mu_;
  absl::flat_hash_map<std::string, std::shared_ptr<NodeWaitState>> waiters_
      ABSL_GUARDED_BY(waiters_mu_);

  /** Last `PRAGMA data_version` seen by the cross-process watcher. */
  std::atomic<std::int64_t> last_data_version_{-1};
};

}  // namespace a11::stores::internal

#endif  // A11_STORES_INTERNAL_SQLITE_DATABASE_H_

// Copyright 2026 The A11 Authors.

#include "a11/stores/internal/sqlite_database.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <unistd.h>

#include "a11/concurrency/future.h"
#include "a11/uuid.h"
#include "thread/boost_primitives.h"
#include "thread/executor.h"
#include "thread/selectables.h"

namespace a11::stores::internal {
namespace {

/**
 * Bumped whenever the on-disk layout changes in a way older binaries cannot
 * read. Stored in `PRAGMA user_version`.
 */
constexpr int kSchemaVersion = 1;

/** How long to keep retrying a transaction that keeps losing the write lock. */
constexpr absl::Duration kBusyRetryBudget = absl::Seconds(10);
/** Initial backoff between busy retries; doubles up to the cap below. */
constexpr absl::Duration kBusyRetryInitialBackoff = absl::Milliseconds(1);
constexpr absl::Duration kBusyRetryMaximumBackoff = absl::Milliseconds(50);

/** Translate a SQLite result code into the closest absl::Status. */
absl::Status SqliteStatus(sqlite3* handle, int code, std::string_view what) {
  if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) {
    return absl::OkStatus();
  }
  const char* detail =
      handle != nullptr ? sqlite3_errmsg(handle) : sqlite3_errstr(code);
  const std::string message =
      absl::StrCat(what, ": ", detail == nullptr ? "unknown error" : detail,
                   " (sqlite ", code, ")");
  switch (code & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
      return absl::UnavailableError(message);
    case SQLITE_CONSTRAINT:
      return absl::AlreadyExistsError(message);
    case SQLITE_READONLY:
    case SQLITE_AUTH:
    case SQLITE_PERM:
      return absl::PermissionDeniedError(message);
    case SQLITE_NOTFOUND:
      return absl::NotFoundError(message);
    case SQLITE_FULL:
    case SQLITE_NOMEM:
      return absl::ResourceExhaustedError(message);
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
      return absl::DataLossError(message);
    case SQLITE_INTERRUPT:
      return absl::CancelledError(message);
    case SQLITE_MISUSE:
      return absl::InternalError(message);
    default:
      return absl::UnknownError(message);
  }
}

/** True when a failure means "someone else holds the write lock, try again". */
bool IsBusy(const absl::Status& status) {
  return absl::IsUnavailable(status);
}

/** SQLite's global init is explicit because SQLITE_OMIT_AUTOINIT is set. */
absl::Status EnsureSqliteInitialized() {
  static const absl::NoDestructor<absl::Status> once([] {
    const int code = sqlite3_initialize();
    return SqliteStatus(nullptr, code, "sqlite3_initialize");
  }());
  return *once;
}

/** fsync a directory so a rename into it survives a crash. */
absl::Status SyncDirectory(const std::filesystem::path& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY);
  if (fd < 0) {
    return absl::InternalError(absl::StrCat("Cannot open blob directory ",
                                            directory.string(), ": ",
                                            std::strerror(errno)));
  }
  const int synced = ::fsync(fd);
  const int saved_errno = errno;
  ::close(fd);
  if (synced != 0) {
    return absl::InternalError(absl::StrCat("Cannot fsync blob directory ",
                                            directory.string(), ": ",
                                            std::strerror(saved_errno)));
  }
  return absl::OkStatus();
}

/** The process-wide table of open roots, so one root means one database. */
struct Registry {
  thread::Mutex mu;
  absl::flat_hash_map<std::string, std::weak_ptr<SqliteDatabase>> databases
      ABSL_GUARDED_BY(mu);
};

Registry& GetRegistry() {
  static absl::NoDestructor<Registry> registry;
  return *registry;
}

}  // namespace

// -------------------------------------------------------------------------
// SqliteConnection

SqliteConnection::~SqliteConnection() {
  for (auto& [sql, statement] : statements_) {
    sqlite3_finalize(statement);
  }
  statements_.clear();
  if (handle_ != nullptr) {
    sqlite3_close_v2(handle_);
    handle_ = nullptr;
  }
}

absl::StatusOr<sqlite3_stmt*> SqliteConnection::Prepare(std::string_view sql) {
  const auto found = statements_.find(sql);
  if (found != statements_.end()) {
    sqlite3_reset(found->second);
    sqlite3_clear_bindings(found->second);
    return found->second;
  }
  sqlite3_stmt* statement = nullptr;
  const int code =
      sqlite3_prepare_v3(handle_, sql.data(), static_cast<int>(sql.size()),
                         SQLITE_PREPARE_PERSISTENT, &statement, nullptr);
  if (code != SQLITE_OK) {
    return SqliteStatus(handle_, code, absl::StrCat("Cannot prepare: ", sql));
  }
  statements_.emplace(std::string(sql), statement);
  return statement;
}

absl::Status SqliteConnection::Execute(std::string_view sql) {
  char* error = nullptr;
  const int code =
      sqlite3_exec(handle_, std::string(sql).c_str(), nullptr, nullptr, &error);
  if (code != SQLITE_OK) {
    const std::string detail = error != nullptr ? error : "unknown error";
    sqlite3_free(error);
    return SqliteStatus(handle_, code,
                        absl::StrCat("Cannot execute '", sql, "': ", detail));
  }
  return absl::OkStatus();
}

void SqliteConnection::ResetAll() {
  for (auto& [sql, statement] : statements_) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
  }
}

// -------------------------------------------------------------------------
// Hooks

/**
 * State the SQLite change hooks write to.
 *
 * The hooks run in a C frame compiled without unwind tables, so nothing that
 * allocates or can throw is permitted inside them. They therefore touch only
 * preallocated scalars. Their job is to be the authoritative answer to "did
 * this transaction commit or roll back"; the *identity* of the affected nodes
 * comes from the write path, which issued the statements and already knows.
 */
struct SqliteHookState {
  /** Set by the commit hook, which fires as the transaction is committing. */
  std::atomic<bool> committed{false};
  /** Set by the rollback hook, including on implicit rollbacks. */
  std::atomic<bool> rolled_back{false};
  /** Rows the update hook saw change; a cheap sanity signal, not a set. */
  std::atomic<std::uint64_t> row_changes{0};

  void BeginTransaction() {
    committed.store(false, std::memory_order_release);
    rolled_back.store(false, std::memory_order_release);
    row_changes.store(0, std::memory_order_release);
  }
};

namespace {

extern "C" int SqliteCommitHook(void* context) {
  auto* hooks = static_cast<SqliteHookState*>(context);
  hooks->committed.store(true, std::memory_order_release);
  // Returning non-zero here would turn this commit into a rollback.
  return 0;
}

extern "C" void SqliteRollbackHook(void* context) {
  auto* hooks = static_cast<SqliteHookState*>(context);
  hooks->rolled_back.store(true, std::memory_order_release);
}

extern "C" void SqliteUpdateHook(void* context, int /*operation*/,
                                 const char* /*database*/,
                                 const char* /*table*/,
                                 sqlite3_int64 /*rowid*/) {
  auto* hooks = static_cast<SqliteHookState*>(context);
  hooks->row_changes.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace

// -------------------------------------------------------------------------
// Workers

/**
 * The dedicated OS threads that own the SQLite connections.
 *
 * One writer thread with the single write connection, plus N reader threads
 * each owning their own read connection. Because a connection belongs to
 * exactly one thread, none of them needs a lock. Task queues use `std::mutex`
 * and `std::condition_variable` because these threads never run fibers.
 */
class SqliteDatabase::Workers {
 public:
  using Task = absl::AnyInvocable<void(SqliteConnection&) &&>;

  Workers() = default;
  Workers(const Workers&) = delete;
  Workers& operator=(const Workers&) = delete;

  ~Workers() { Shutdown(); }

  /** Take ownership of the connections and start the threads. */
  void Start(std::unique_ptr<SqliteConnection> write_connection,
             std::vector<std::unique_ptr<SqliteConnection>> read_connections) {
    writer_thread_ =
        std::thread([this, connection = std::move(write_connection)]() {
          Loop(write_queue_, *connection);
        });
    for (auto& connection : read_connections) {
      reader_threads_.emplace_back(
          [this, owned = std::move(connection)]() mutable {
            Loop(read_queue_, *owned);
          });
    }
  }

  /** Enqueue writer work; returns false once the pool is shutting down. */
  bool SubmitWrite(Task task) { return Submit(write_queue_, std::move(task)); }

  /** Enqueue reader work; returns false once the pool is shutting down. */
  bool SubmitRead(Task task) { return Submit(read_queue_, std::move(task)); }

  /** Stop accepting work and join every thread. */
  void Shutdown() {
    write_queue_.Close();
    read_queue_.Close();
    if (writer_thread_.joinable()) {
      writer_thread_.join();
    }
    for (std::thread& reader : reader_threads_) {
      if (reader.joinable()) {
        reader.join();
      }
    }
    reader_threads_.clear();
  }

 private:
  struct Queue {
    std::mutex mu;
    std::condition_variable available;
    std::deque<Task> tasks;
    bool closed = false;

    void Close() {
      {
        const std::lock_guard<std::mutex> lock(mu);
        closed = true;
      }
      available.notify_all();
    }
  };

  static bool Submit(Queue& queue, Task task) {
    {
      const std::lock_guard<std::mutex> lock(queue.mu);
      if (queue.closed) {
        return false;
      }
      queue.tasks.push_back(std::move(task));
    }
    queue.available.notify_one();
    return true;
  }

  static void Loop(Queue& queue, SqliteConnection& connection) {
    while (true) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(queue.mu);
        queue.available.wait(
            lock, [&queue] { return queue.closed || !queue.tasks.empty(); });
        if (queue.tasks.empty()) {
          return;  // Closed and drained.
        }
        task = std::move(queue.tasks.front());
        queue.tasks.pop_front();
      }
      std::move(task)(connection);
      // A statement left un-reset would pin this connection's WAL snapshot, so
      // the next task on this thread would keep reading stale data.
      connection.ResetAll();
    }
  }

  Queue write_queue_;
  Queue read_queue_;
  std::thread writer_thread_;
  std::vector<std::thread> reader_threads_;
};

// -------------------------------------------------------------------------
// SqliteDatabase

SqliteDatabase::SqliteDatabase(ConstructorToken, std::filesystem::path root,
                               SqliteDatabaseOptions options)
    : root_(std::move(root)),
      options_(options),
      hooks_(std::make_unique<SqliteHookState>()),
      workers_(std::make_unique<Workers>()) {}

SqliteDatabase::~SqliteDatabase() {
  // Join before anything else is destroyed: a worker mid-task would otherwise
  // touch freed queues, connections and hook state.
  workers_->Shutdown();
}

absl::StatusOr<std::shared_ptr<SqliteDatabase>> SqliteDatabase::Open(
    const std::filesystem::path& root, const SqliteDatabaseOptions& options) {
  ABSL_RETURN_IF_ERROR(EnsureSqliteInitialized());

  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    return absl::InternalError(absl::StrCat("Cannot create chunk store root ",
                                            root.string(), ": ",
                                            error.message()));
  }
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(root, error);
  if (error) {
    return absl::InternalError(absl::StrCat("Cannot resolve chunk store root ",
                                            root.string(), ": ",
                                            error.message()));
  }
  std::filesystem::create_directories(canonical / "blobs", error);
  if (error) {
    return absl::InternalError(
        absl::StrCat("Cannot create blob directory under ", canonical.string(),
                     ": ", error.message()));
  }

  Registry& registry = GetRegistry();
  const std::string key = canonical.string();
  thread::MutexLock lock(&registry.mu);
  const auto found = registry.databases.find(key);
  if (found != registry.databases.end()) {
    if (std::shared_ptr<SqliteDatabase> existing = found->second.lock()) {
      return existing;
    }
    registry.databases.erase(found);
  }

  auto database =
      std::make_shared<SqliteDatabase>(ConstructorToken{}, canonical, options);
  ABSL_RETURN_IF_ERROR(database->Initialize());
  registry.databases.emplace(key, database);
  return database;
}

absl::StatusOr<std::unique_ptr<SqliteConnection>>
SqliteDatabase::OpenConnection(bool read_only) const {
  const std::filesystem::path file = root_ / "store.sqlite";
  // NOMUTEX: each connection is confined to one thread, so SQLite's own
  // serialization would be pure overhead.
  int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI;
  flags |= read_only ? SQLITE_OPEN_READONLY
                     : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  sqlite3* handle = nullptr;
  const int code = sqlite3_open_v2(file.c_str(), &handle, flags, nullptr);
  if (code != SQLITE_OK) {
    absl::Status status =
        SqliteStatus(handle, code, absl::StrCat("Cannot open ", file.string()));
    sqlite3_close_v2(handle);
    return status;
  }
  auto connection =
      std::unique_ptr<SqliteConnection>(new SqliteConnection(handle));

  // No sqlite3_busy_timeout: it sleeps the calling thread inside sqlite3_step.
  // Contention is retried at the C++ level instead, where the backoff is
  // visible and bounded.
  ABSL_RETURN_IF_ERROR(connection->Execute("PRAGMA busy_timeout=0"));
  ABSL_RETURN_IF_ERROR(connection->Execute("PRAGMA journal_mode=WAL"));
  ABSL_RETURN_IF_ERROR(connection->Execute("PRAGMA foreign_keys=ON"));
  switch (options_.synchronous) {
    case SqliteSynchronous::kOff:
      ABSL_RETURN_IF_ERROR(connection->Execute("PRAGMA synchronous=OFF"));
      break;
    case SqliteSynchronous::kNormal:
      ABSL_RETURN_IF_ERROR(connection->Execute("PRAGMA synchronous=NORMAL"));
      break;
    case SqliteSynchronous::kFull:
      ABSL_RETURN_IF_ERROR(connection->Execute("PRAGMA synchronous=FULL"));
      break;
  }
  return connection;
}

absl::Status SqliteDatabase::ApplySchema(SqliteConnection& connection) {
  // `nodes` carries everything answerable without touching a fragment: the
  // cursors, the closure state and the cached size, so Size() and GetFinalSeq()
  // are single-row reads.
  return connection.Execute(R"sql(
    CREATE TABLE IF NOT EXISTS nodes (
      id          TEXT    PRIMARY KEY,
      owner_id    TEXT,
      closed      INTEGER NOT NULL DEFAULT 0,
      status      BLOB,
      final_seq   INTEGER,
      next_cursor INTEGER NOT NULL DEFAULT 0,
      put_count   INTEGER NOT NULL DEFAULT 0,
      size        INTEGER NOT NULL DEFAULT 0,
      data_bytes  INTEGER NOT NULL DEFAULT 0,
      max_seq     INTEGER,
      revision    INTEGER NOT NULL DEFAULT 0,
      created_at  INTEGER NOT NULL,
      updated_at  INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS fragments (
      node_id            TEXT    NOT NULL REFERENCES nodes(id),
      seq                INTEGER NOT NULL,
      arrival_order      INTEGER NOT NULL,
      final_marker       INTEGER NOT NULL DEFAULT 0,
      tombstone          INTEGER NOT NULL DEFAULT 0,
      storage            INTEGER NOT NULL,
      created_at         INTEGER NOT NULL,
      timestamp          INTEGER NOT NULL,
      timestamp_explicit INTEGER NOT NULL DEFAULT 0,
      mimetype           TEXT,
      data               BLOB,
      ref                TEXT    NOT NULL DEFAULT '',
      metadata           BLOB,
      node_ref_id        TEXT,
      node_ref_offset    INTEGER,
      node_ref_length    INTEGER
    );

    CREATE UNIQUE INDEX IF NOT EXISTS fragments_seq
      ON fragments(node_id, seq);
    CREATE UNIQUE INDEX IF NOT EXISTS fragments_arrival
      ON fragments(node_id, arrival_order);
    CREATE INDEX IF NOT EXISTS nodes_owner
      ON nodes(owner_id) WHERE owner_id IS NOT NULL;
    CREATE INDEX IF NOT EXISTS fragments_node_ref
      ON fragments(node_ref_id) WHERE node_ref_id IS NOT NULL;
    CREATE INDEX IF NOT EXISTS fragments_blob
      ON fragments(ref) WHERE ref <> '';
    CREATE INDEX IF NOT EXISTS fragments_timestamp
      ON fragments(node_id, timestamp);
  )sql");
}

absl::Status SqliteDatabase::Initialize() {
  ABSL_ASSIGN_OR_RETURN(std::unique_ptr<SqliteConnection> writer,
                        OpenConnection(/*read_only=*/false));

  ABSL_RETURN_IF_ERROR(writer->Execute("BEGIN IMMEDIATE"));
  absl::Status schema = ApplySchema(*writer);
  if (!schema.ok()) {
    writer->Execute("ROLLBACK").IgnoreError();
    return schema;
  }
  ABSL_RETURN_IF_ERROR(
      writer->Execute(absl::StrCat("PRAGMA user_version=", kSchemaVersion)));
  ABSL_RETURN_IF_ERROR(writer->Execute("COMMIT"));

  sqlite3_commit_hook(writer->handle(), &SqliteCommitHook, hooks_.get());
  sqlite3_rollback_hook(writer->handle(), &SqliteRollbackHook, hooks_.get());
  sqlite3_update_hook(writer->handle(), &SqliteUpdateHook, hooks_.get());

  size_t readers = options_.reader_threads;
  if (readers == 0) {
    const unsigned int cpus = std::thread::hardware_concurrency();
    readers = cpus >= 8 ? 4 : (cpus >= 4 ? 2 : 1);
  }
  std::vector<std::unique_ptr<SqliteConnection>> read_connections;
  read_connections.reserve(readers);
  for (size_t index = 0; index < readers; ++index) {
    ABSL_ASSIGN_OR_RETURN(std::unique_ptr<SqliteConnection> reader,
                          OpenConnection(/*read_only=*/true));
    read_connections.push_back(std::move(reader));
  }

  workers_->Start(std::move(writer), std::move(read_connections));

  if (options_.cross_process_poll_interval > absl::ZeroDuration()) {
    PollCrossProcess();
  }
  return absl::OkStatus();
}

absl::Status SqliteDatabase::RunRead(
    absl::AnyInvocable<absl::Status(SqliteConnection&)> body) {
  a11::Promise<absl::Status> promise;
  a11::Future<absl::Status> future = promise.future();
  const bool submitted = workers_->SubmitRead(
      [body = std::move(body),
       promise = std::move(promise)](SqliteConnection& connection) mutable {
        promise.SetValue(std::move(body)(connection)).IgnoreError();
      });
  if (!submitted) {
    return absl::UnavailableError("Chunk store database is shutting down");
  }
  ABSL_ASSIGN_OR_RETURN(absl::Status result, future.Await());
  return result;
}

absl::Status SqliteDatabase::RunWrite(
    absl::AnyInvocable<
        absl::StatusOr<std::vector<std::string>>(SqliteConnection&)>
        body) {
  // Result of the transaction plus the ids to wake, resolved on the writer
  // thread and consumed back on the caller's fiber.
  struct Outcome {
    absl::Status status;
    std::vector<std::string> touched;
  };

  a11::Promise<Outcome> promise;
  a11::Future<Outcome> future = promise.future();
  SqliteHookState* hooks = hooks_.get();

  const bool submitted = workers_->SubmitWrite(
      [body = std::move(body), promise = std::move(promise),
       hooks](SqliteConnection& connection) mutable {
        Outcome outcome;
        absl::Duration backoff = kBusyRetryInitialBackoff;
        const absl::Time give_up = absl::Now() + kBusyRetryBudget;
        while (true) {
          hooks->BeginTransaction();
          absl::Status status = connection.Execute("BEGIN IMMEDIATE");
          if (status.ok()) {
            absl::StatusOr<std::vector<std::string>> touched = body(connection);
            if (touched.ok()) {
              status = connection.Execute("COMMIT");
              if (status.ok()) {
                // The commit hook is the authoritative confirmation.
                if (!hooks->committed.load(std::memory_order_acquire)) {
                  status = absl::InternalError(
                      "SQLite commit completed without firing the commit hook");
                } else {
                  outcome.touched = *std::move(touched);
                }
              }
            } else {
              status = touched.status();
            }
            if (!status.ok()) {
              connection.Execute("ROLLBACK").IgnoreError();
            }
          }
          if (!IsBusy(status) || absl::Now() >= give_up) {
            outcome.status = std::move(status);
            break;
          }
          // Another process holds the write lock. Yield this worker thread
          // briefly rather than spinning inside SQLite.
          std::this_thread::sleep_for(
              std::chrono::nanoseconds(absl::ToInt64Nanoseconds(backoff)));
          backoff = std::min(backoff * 2, kBusyRetryMaximumBackoff);
        }
        promise.SetValue(std::move(outcome)).IgnoreError();
      });
  if (!submitted) {
    return absl::UnavailableError("Chunk store database is shutting down");
  }

  ABSL_ASSIGN_OR_RETURN(Outcome outcome, future.Await());
  ABSL_RETURN_IF_ERROR(outcome.status);
  // Strictly after COMMIT returned. Swapping the generation event any earlier
  // would let a reader snapshot the new event, read the pre-commit snapshot,
  // park on it, and then miss a notification aimed at the old one.
  NotifyNodes(outcome.touched);
  return absl::OkStatus();
}

std::shared_ptr<SqliteDatabase::NodeWaitState> SqliteDatabase::WaitStateFor(
    std::string_view node_id) {
  thread::MutexLock lock(&waiters_mu_);
  const auto found = waiters_.find(node_id);
  if (found != waiters_.end()) {
    return found->second;
  }
  auto state = std::make_shared<NodeWaitState>();
  waiters_.emplace(std::string(node_id), state);
  return state;
}

std::shared_ptr<thread::PermanentEvent> SqliteDatabase::WatchNode(
    std::string_view node_id) {
  const std::shared_ptr<NodeWaitState> state = WaitStateFor(node_id);
  thread::MutexLock lock(&state->mu);
  return state->changed;
}

void SqliteDatabase::NotifyNodes(const std::vector<std::string>& node_ids) {
  std::vector<std::shared_ptr<thread::PermanentEvent>> to_notify;
  to_notify.reserve(node_ids.size());
  for (const std::string& node_id : node_ids) {
    const std::shared_ptr<NodeWaitState> state = WaitStateFor(node_id);
    thread::MutexLock lock(&state->mu);
    to_notify.push_back(std::exchange(
        state->changed, std::make_shared<thread::PermanentEvent>()));
  }
  // Fire outside every lock: a woken fiber immediately re-reads, and doing that
  // while we still held the wait-state mutex would serialize the wakeups.
  for (const std::shared_ptr<thread::PermanentEvent>& event : to_notify) {
    event->Notify();
  }
}

void SqliteDatabase::PollCrossProcess() {
  // SQLite bumps PRAGMA data_version on a connection whenever *another*
  // connection -- in this or any other process -- commits.
  const std::weak_ptr<SqliteDatabase> weak = weak_from_this();
  thread::PostAt(absl::Now() + options_.cross_process_poll_interval, [weak] {
    const std::shared_ptr<SqliteDatabase> database = weak.lock();
    if (database == nullptr) {
      return;
    }
    std::int64_t version = -1;
    database
        ->RunRead([&version](SqliteConnection& connection) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(sqlite3_stmt * statement,
                                connection.Prepare("PRAGMA data_version"));
          if (sqlite3_step(statement) == SQLITE_ROW) {
            version = sqlite3_column_int64(statement, 0);
          }
          return absl::OkStatus();
        })
        .IgnoreError();

    const std::int64_t previous = database->last_data_version_.exchange(
        version, std::memory_order_acq_rel);
    if (version >= 0 && previous >= 0 && version != previous) {
      std::vector<std::string> all_nodes;
      {
        thread::MutexLock lock(&database->waiters_mu_);
        all_nodes.reserve(database->waiters_.size());
        for (const auto& [node_id, state] : database->waiters_) {
          all_nodes.push_back(node_id);
        }
      }
      database->NotifyNodes(all_nodes);
    }
    database->PollCrossProcess();
  });
}

// -------------------------------------------------------------------------
// Blob files

absl::StatusOr<std::string> SqliteDatabase::WriteBlob(
    std::string_view data) const {
  const std::filesystem::path directory = blobs_directory();
  std::string name = a11::NewUuid();
  const std::filesystem::path final_path = directory / name;
  const std::filesystem::path temporary =
      directory / absl::StrCat(".tmp-", name);

  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    return absl::InternalError(absl::StrCat(
        "Cannot create blob ", temporary.string(), ": ", std::strerror(errno)));
  }
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t wrote =
        ::write(fd, data.data() + written, data.size() - written);
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved_errno = errno;
      ::close(fd);
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return absl::InternalError(absl::StrCat("Cannot write blob ",
                                              temporary.string(), ": ",
                                              std::strerror(saved_errno)));
    }
    written += static_cast<size_t>(wrote);
  }
  if (::fsync(fd) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return absl::InternalError(absl::StrCat("Cannot fsync blob ",
                                            temporary.string(), ": ",
                                            std::strerror(saved_errno)));
  }
  ::close(fd);

  std::error_code error;
  std::filesystem::rename(temporary, final_path, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return absl::InternalError(absl::StrCat(
        "Cannot publish blob ", final_path.string(), ": ", error.message()));
  }
  // fsync on the file made its contents durable but not its directory entry.
  // Without this a crash can leave a committed row referencing a blob that is
  // no longer reachable.
  ABSL_RETURN_IF_ERROR(SyncDirectory(directory));
  return name;
}

absl::StatusOr<std::string> SqliteDatabase::ReadBlob(
    std::string_view name) const {
  const std::filesystem::path path = blobs_directory() / std::string(name);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return absl::DataLossError(absl::StrCat(
        "Chunk store blob ", name, " is missing from ", path.string()));
  }
  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  if (input.bad()) {
    return absl::DataLossError(
        absl::StrCat("Cannot read chunk store blob ", path.string()));
  }
  return contents;
}

void SqliteDatabase::RemoveBlob(std::string_view name) const {
  if (name.empty()) {
    return;
  }
  std::error_code ignored;
  std::filesystem::remove(blobs_directory() / std::string(name), ignored);
}

absl::StatusOr<size_t> SqliteDatabase::SweepOrphanBlobs() {
  const std::filesystem::path directory = blobs_directory();
  // Compare in the filesystem clock's own domain: converting a file_time_type
  // to a system_clock time is not portable across the toolchains A11 builds on.
  using FileClock = std::filesystem::file_time_type::clock;
  const auto grace = std::chrono::nanoseconds(
      absl::ToInt64Nanoseconds(options_.blob_grace_period));
  const auto cutoff = FileClock::now() - grace;

  // Collect candidates first, then ask the database which are still referenced.
  std::vector<std::string> candidates;
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return absl::InternalError(absl::StrCat("Cannot scan blob directory ",
                                            directory.string(), ": ",
                                            error.message()));
  }
  for (const auto& entry : iterator) {
    if (!entry.is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    const auto modified = std::filesystem::last_write_time(entry.path(), error);
    if (error) {
      error.clear();
      continue;
    }
    if (modified >= cutoff) {
      continue;  // Too fresh: a commit referencing it may still be in flight.
    }
    candidates.push_back(entry.path().filename().string());
  }

  // Decide inside a write transaction so the reference check cannot race a
  // commit in this process; the grace period covers other processes.
  std::vector<std::string> doomed;
  ABSL_RETURN_IF_ERROR(
      RunWrite([&candidates, &doomed](SqliteConnection& connection)
                   -> absl::StatusOr<std::vector<std::string>> {
        ABSL_ASSIGN_OR_RETURN(
            sqlite3_stmt * statement,
            connection.Prepare(
                "SELECT 1 FROM fragments WHERE ref = ?1 LIMIT 1"));
        for (const std::string& name : candidates) {
          if (name.rfind(".tmp-", 0) == 0) {
            doomed.push_back(name);  // An abandoned partial write.
            continue;
          }
          sqlite3_reset(statement);
          sqlite3_clear_bindings(statement);
          sqlite3_bind_text(statement, 1, name.data(),
                            static_cast<int>(name.size()), SQLITE_TRANSIENT);
          if (sqlite3_step(statement) != SQLITE_ROW) {
            doomed.push_back(name);
          }
        }
        sqlite3_reset(statement);
        return std::vector<std::string>{};  // No node state changed.
      }));

  // Unlink only once the deciding transaction has committed, so a rollback can
  // never take the files with it.
  for (const std::string& name : doomed) {
    RemoveBlob(name);
  }
  return doomed.size();
}

}  // namespace a11::stores::internal

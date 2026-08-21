// Copyright 2026 The A11 Authors.

#include "a11/stores/sqlite_chunk_store.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <sqlite3.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/msgpack.h"
#include "a11/data/types.h"
#include "a11/stores/internal/chunk_store_common.h"
#include "a11/stores/internal/sqlite_database.h"
#include "thread/selectables.h"

namespace a11::stores {
namespace {

using internal::SqliteConnection;
using internal::SqliteDatabase;
using internal::SqliteNodeState;

/** What a fragment row's payload columns mean. */
enum StorageKind : int {
  /** Payload bytes are inline in the `data` column. */
  kStorageInline = 0,
  /** Payload lives in `blobs/<ref>`; `data` is NULL. */
  kStorageBlob = 1,
  /** The Chunk carries its own semantic node reference in `ref`. */
  kStorageChunkRef = 2,
  /** The fragment is a NodeRef; the `node_ref_*` columns are populated. */
  kStorageNodeRef = 3,
  /** ClearData() erased the payload but kept the slot. */
  kStorageTombstone = 4,
};

/** The `ref` LocalChunkStore and RedisChunkStore put on a cleared chunk. */
constexpr std::string_view kTombstoneReference = "__tombstone__";

constexpr std::uint32_t kMaxSeq = std::numeric_limits<std::uint32_t>::max();
constexpr std::int64_t kMaxSqliteInteger =
    std::numeric_limits<std::int64_t>::max();

/** Every fragment column, in the order the row decoder expects. */
constexpr std::string_view kFragmentColumns =
    "seq, arrival_order, tombstone, storage, timestamp, timestamp_explicit, "
    "mimetype, data, ref, metadata, node_ref_id, node_ref_offset, "
    "node_ref_length";

absl::Status StepDone(sqlite3_stmt* statement, std::string_view what) {
  const int code = sqlite3_step(statement);
  if (code != SQLITE_DONE) {
    sqlite3* handle = sqlite3_db_handle(statement);
    return absl::InternalError(absl::StrCat(what, ": ", sqlite3_errmsg(handle),
                                            " (sqlite ", code, ")"));
  }
  return absl::OkStatus();
}

void BindText(sqlite3_stmt* statement, int index, std::string_view value) {
  sqlite3_bind_text(statement, index, value.data(),
                    static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void BindBlob(sqlite3_stmt* statement, int index, std::string_view value) {
  sqlite3_bind_blob(statement, index, value.data(),
                    static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string ColumnText(sqlite3_stmt* statement, int index) {
  const auto* text = sqlite3_column_text(statement, index);
  if (text == nullptr) {
    return {};
  }
  return {reinterpret_cast<const char*>(text),
          static_cast<size_t>(sqlite3_column_bytes(statement, index))};
}

std::string ColumnBlob(sqlite3_stmt* statement, int index) {
  const void* blob = sqlite3_column_blob(statement, index);
  if (blob == nullptr) {
    return {};
  }
  return {static_cast<const char*>(blob),
          static_cast<size_t>(sqlite3_column_bytes(statement, index))};
}

bool ColumnIsNull(sqlite3_stmt* statement, int index) {
  return sqlite3_column_type(statement, index) == SQLITE_NULL;
}

/** Read the node row. A missing row is not an error: it means "no writes yet". */
absl::StatusOr<SqliteNodeState> LoadNodeState(SqliteConnection& connection,
                                              std::string_view node_id) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3_stmt * statement,
      connection.Prepare(
          "SELECT owner_id, closed, status, final_seq, next_cursor, put_count, "
          "size, data_bytes, max_seq, revision, created_at, updated_at "
          "FROM nodes WHERE id = ?1"));
  BindText(statement, 1, node_id);

  SqliteNodeState state;
  const int code = sqlite3_step(statement);
  if (code == SQLITE_DONE) {
    return state;  // exists == false.
  }
  if (code != SQLITE_ROW) {
    sqlite3* handle = sqlite3_db_handle(statement);
    return absl::InternalError(
        absl::StrCat("Cannot read node row: ", sqlite3_errmsg(handle)));
  }
  state.exists = true;
  state.owner_id = ColumnText(statement, 0);
  state.closed = sqlite3_column_int(statement, 1) != 0;
  if (!ColumnIsNull(statement, 2)) {
    const std::string packed = ColumnBlob(statement, 2);
    absl::StatusOr<absl::Status> unpacked = data::UnpackStatus(packed);
    if (!unpacked.ok()) {
      return absl::DataLossError(
          absl::StrCat("Chunk store terminal status is unreadable: ",
                       unpacked.status().message()));
    }
    state.status = *std::move(unpacked);
  } else if (state.closed) {
    state.status = absl::OkStatus();
  }
  if (!ColumnIsNull(statement, 3)) {
    state.final_seq =
        static_cast<std::uint32_t>(sqlite3_column_int64(statement, 3));
  }
  state.next_cursor =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 4));
  state.put_count =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 5));
  state.size = static_cast<size_t>(sqlite3_column_int64(statement, 6));
  state.data_bytes =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 7));
  if (!ColumnIsNull(statement, 8)) {
    state.max_seq =
        static_cast<std::uint32_t>(sqlite3_column_int64(statement, 8));
  }
  state.revision =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 9));
  state.created_at = absl::FromUnixMicros(sqlite3_column_int64(statement, 10));
  state.updated_at = absl::FromUnixMicros(sqlite3_column_int64(statement, 11));
  return state;
}

/** The row shape produced by a `SELECT kFragmentColumns` query. */
struct FragmentRow {
  std::uint32_t seq = 0;
  std::uint64_t arrival_order = 0;
  bool tombstone = false;
  int storage = kStorageInline;
  std::optional<absl::Time> timestamp;
  std::string mimetype;
  std::string data;
  std::string ref;
  std::optional<std::string> metadata;
  std::optional<std::string> node_ref_id;
  std::uint32_t node_ref_offset = 0;
  std::optional<std::uint64_t> node_ref_length;
};

FragmentRow ReadFragmentRow(sqlite3_stmt* statement) {
  FragmentRow row;
  row.seq = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 0));
  row.arrival_order =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1));
  row.tombstone = sqlite3_column_int(statement, 2) != 0;
  row.storage = sqlite3_column_int(statement, 3);
  // The timestamp column is always populated so it can be indexed; only report
  // it back when the source metadata actually carried one.
  if (sqlite3_column_int(statement, 5) != 0) {
    row.timestamp = absl::FromUnixMicros(sqlite3_column_int64(statement, 4));
  }
  row.mimetype = ColumnText(statement, 6);
  row.data = ColumnBlob(statement, 7);
  row.ref = ColumnText(statement, 8);
  if (!ColumnIsNull(statement, 9)) {
    row.metadata = ColumnBlob(statement, 9);
  }
  if (!ColumnIsNull(statement, 10)) {
    row.node_ref_id = ColumnText(statement, 10);
    row.node_ref_offset =
        static_cast<std::uint32_t>(sqlite3_column_int64(statement, 11));
    if (!ColumnIsNull(statement, 12)) {
      row.node_ref_length =
          static_cast<std::uint64_t>(sqlite3_column_int64(statement, 12));
    }
  }
  return row;
}

/**
 * Rebuild the fragment a caller originally wrote.
 *
 * `continued` is always recomputed from the node's current final sequence
 * rather than read back from the row: a later batch can declare finality, and
 * a stored flag would then disagree with the store.
 */
absl::StatusOr<data::NodeFragment> DecodeFragment(
    const SqliteDatabase& database, std::string_view node_id,
    const FragmentRow& row, const std::optional<std::uint32_t>& final_seq) {
  data::NodeFragment fragment;
  fragment.id = std::string(node_id);
  fragment.seq = row.seq;
  fragment.continued = !final_seq.has_value() || row.seq < *final_seq;

  if (row.storage == kStorageNodeRef) {
    if (!row.node_ref_id.has_value()) {
      return absl::DataLossError(absl::StrCat(
          "Fragment ", row.seq, " claims to be a node reference but has none"));
    }
    fragment.data = data::NodeRef{
        .id = *row.node_ref_id,
        .offset = row.node_ref_offset,
        .length = row.node_ref_length,
    };
    return fragment;
  }

  data::Chunk chunk;
  if (row.metadata.has_value()) {
    ABSL_ASSIGN_OR_RETURN(chunk.metadata,
                          data::ChunkMetadata::FromMsgpack(*row.metadata));
  }
  switch (row.storage) {
    case kStorageInline:
      chunk.data = row.data;
      break;
    case kStorageBlob: {
      ABSL_ASSIGN_OR_RETURN(chunk.data, database.ReadBlob(row.ref));
      break;
    }
    case kStorageChunkRef:
      chunk.ref = row.ref;
      break;
    case kStorageTombstone:
      chunk.ref = std::string(kTombstoneReference);
      break;
    default:
      return absl::DataLossError(absl::StrCat(
          "Fragment ", row.seq, " has unknown storage kind ", row.storage));
  }
  fragment.data = std::move(chunk);
  return fragment;
}

/** Everything needed to insert one fragment, resolved before the transaction. */
struct EncodedFragment {
  std::uint32_t seq = 0;
  std::uint64_t arrival_order = 0;
  bool final_marker = false;
  int storage = kStorageInline;
  std::int64_t timestamp_micros = 0;
  bool timestamp_explicit = false;
  std::optional<std::string> mimetype;
  std::string data;
  std::string ref;
  std::optional<std::string> metadata;
  std::optional<std::string> node_ref_id;
  std::uint32_t node_ref_offset = 0;
  std::optional<std::uint64_t> node_ref_length;
  /** Set when this row owns a blob file, so a failed batch can unlink it. */
  std::string owned_blob;
  std::uint64_t payload_bytes = 0;
};

/**
 * Decompose a fragment into columns, externalizing an oversized payload.
 *
 * Blobs are written here, before the transaction opens, because a blob must be
 * durable before the row referencing it commits. The caller unlinks
 * `owned_blob` if the batch then fails.
 */
absl::StatusOr<EncodedFragment> EncodeFragment(
    const SqliteDatabase& database, const data::NodeFragment& fragment,
    size_t inline_threshold, absl::Time now) {
  EncodedFragment encoded;
  encoded.final_marker = !fragment.continued;
  encoded.timestamp_micros = absl::ToUnixMicros(now);

  if (const auto* node_ref = std::get_if<data::NodeRef>(&fragment.data)) {
    encoded.storage = kStorageNodeRef;
    encoded.node_ref_id = node_ref->id;
    encoded.node_ref_offset = node_ref->offset;
    encoded.node_ref_length = node_ref->length;
    return encoded;
  }

  const auto& chunk = std::get<data::Chunk>(fragment.data);
  if (chunk.metadata.has_value()) {
    ABSL_ASSIGN_OR_RETURN(std::string packed, chunk.metadata->ToMsgpack());
    encoded.metadata = std::move(packed);
    encoded.mimetype = chunk.metadata->mimetype;
    if (chunk.metadata->timestamp.has_value()) {
      encoded.timestamp_micros = absl::ToUnixMicros(*chunk.metadata->timestamp);
      encoded.timestamp_explicit = true;
    }
  }

  encoded.payload_bytes = chunk.data.size();
  if (chunk.ref == kTombstoneReference) {
    // A caller may replay a fragment that was already tombstoned elsewhere.
    encoded.storage = kStorageTombstone;
  } else if (!chunk.ref.empty()) {
    encoded.storage = kStorageChunkRef;
    encoded.ref = chunk.ref;
  } else if (chunk.data.size() > inline_threshold) {
    ABSL_ASSIGN_OR_RETURN(std::string name, database.WriteBlob(chunk.data));
    encoded.storage = kStorageBlob;
    encoded.ref = name;
    encoded.owned_blob = name;
  } else {
    encoded.storage = kStorageInline;
    encoded.data = chunk.data;
  }
  return encoded;
}

absl::Status InsertFragment(SqliteConnection& connection,
                            std::string_view node_id,
                            const EncodedFragment& encoded, absl::Time now) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3_stmt * statement,
      connection.Prepare(
          "INSERT INTO fragments (node_id, seq, arrival_order, final_marker, "
          "tombstone, storage, created_at, timestamp, timestamp_explicit, "
          "mimetype, data, ref, metadata, node_ref_id, node_ref_offset, "
          "node_ref_length) VALUES (?1, ?2, ?3, ?4, 0, ?5, ?6, ?7, ?8, ?9, "
          "?10, ?11, ?12, ?13, ?14, ?15)"));
  BindText(statement, 1, node_id);
  sqlite3_bind_int64(statement, 2, static_cast<std::int64_t>(encoded.seq));
  sqlite3_bind_int64(statement, 3,
                     static_cast<std::int64_t>(encoded.arrival_order));
  sqlite3_bind_int(statement, 4, encoded.final_marker ? 1 : 0);
  sqlite3_bind_int(statement, 5, encoded.storage);
  sqlite3_bind_int64(statement, 6, absl::ToUnixMicros(now));
  sqlite3_bind_int64(statement, 7, encoded.timestamp_micros);
  sqlite3_bind_int(statement, 8, encoded.timestamp_explicit ? 1 : 0);
  if (encoded.mimetype.has_value()) {
    BindText(statement, 9, *encoded.mimetype);
  } else {
    sqlite3_bind_null(statement, 9);
  }
  if (encoded.storage == kStorageInline) {
    BindBlob(statement, 10, encoded.data);
  } else {
    sqlite3_bind_null(statement, 10);
  }
  BindText(statement, 11, encoded.ref);
  if (encoded.metadata.has_value()) {
    BindBlob(statement, 12, *encoded.metadata);
  } else {
    sqlite3_bind_null(statement, 12);
  }
  if (encoded.node_ref_id.has_value()) {
    BindText(statement, 13, *encoded.node_ref_id);
    sqlite3_bind_int64(statement, 14,
                       static_cast<std::int64_t>(encoded.node_ref_offset));
    if (encoded.node_ref_length.has_value()) {
      sqlite3_bind_int64(statement, 15,
                         static_cast<std::int64_t>(*encoded.node_ref_length));
    } else {
      sqlite3_bind_null(statement, 15);
    }
  } else {
    sqlite3_bind_null(statement, 13);
    sqlite3_bind_null(statement, 14);
    sqlite3_bind_null(statement, 15);
  }
  return StepDone(statement, "Cannot insert fragment");
}

/** Create the node row on first write, or refresh `updated_at`. */
absl::Status EnsureNodeRow(SqliteConnection& connection,
                           std::string_view node_id, std::string_view owner_id,
                           absl::Time now) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3_stmt * statement,
      connection.Prepare(
          "INSERT INTO nodes (id, owner_id, created_at, updated_at) "
          "VALUES (?1, ?2, ?3, ?3) "
          "ON CONFLICT(id) DO UPDATE SET updated_at = ?3"));
  BindText(statement, 1, node_id);
  if (owner_id.empty()) {
    sqlite3_bind_null(statement, 2);
  } else {
    BindText(statement, 2, owner_id);
  }
  sqlite3_bind_int64(statement, 3, absl::ToUnixMicros(now));
  return StepDone(statement, "Cannot create node row");
}

/** True when the node already holds a fragment at @p seq. */
absl::StatusOr<bool> FragmentExists(SqliteConnection& connection,
                                    std::string_view node_id,
                                    std::uint32_t seq) {
  ABSL_ASSIGN_OR_RETURN(
      sqlite3_stmt * statement,
      connection.Prepare(
          "SELECT 1 FROM fragments WHERE node_id = ?1 AND seq = ?2"));
  BindText(statement, 1, node_id);
  sqlite3_bind_int64(statement, 2, static_cast<std::int64_t>(seq));
  return sqlite3_step(statement) == SQLITE_ROW;
}

std::string HomeDirectory() {
  if (const char* home = std::getenv("HOME"); home != nullptr) {
    return home;
  }
  return ".";
}

}  // namespace

// -------------------------------------------------------------------------
// Options

absl::Status SQLiteChunkStoreOptions::Validate() const {
  if (inline_data_threshold > static_cast<size_t>(kMaxSqliteInteger)) {
    return absl::InvalidArgumentError(
        "SQLite chunk-store inline_data_threshold is implausibly large");
  }
  if (!owner_id.empty()) {
    ABSL_RETURN_IF_ERROR(data::ValidateName(owner_id));
  }
  if (cross_process_poll_interval < absl::ZeroDuration()) {
    return absl::InvalidArgumentError(
        "SQLite chunk-store cross_process_poll_interval must not be negative");
  }
  if (blob_grace_period < absl::ZeroDuration()) {
    return absl::InvalidArgumentError(
        "SQLite chunk-store blob_grace_period must not be negative");
  }
  return absl::OkStatus();
}

absl::StatusOr<SQLiteChunkStoreOptions>
SQLiteChunkStoreOptions::FromEnvironment() {
  SQLiteChunkStoreOptions options;
  if (const std::optional<std::string> value = internal::EnvironmentValue(
          "A11_SQLITE_CHUNK_STORE_INLINE_DATA_THRESHOLD_BYTES");
      value.has_value()) {
    ABSL_ASSIGN_OR_RETURN(
        options.inline_data_threshold,
        internal::ParseEnvironmentSize(
            *value, "A11_SQLITE_CHUNK_STORE_INLINE_DATA_THRESHOLD_BYTES"));
  }
  if (const std::optional<std::string> value =
          internal::EnvironmentValue("A11_SQLITE_CHUNK_STORE_OWNER_ID");
      value.has_value()) {
    options.owner_id = *value;
  }
  if (const std::optional<std::string> value = internal::EnvironmentValue(
          "A11_SQLITE_CHUNK_STORE_CROSS_PROCESS_POLL_MS");
      value.has_value()) {
    ABSL_ASSIGN_OR_RETURN(
        options.cross_process_poll_interval,
        internal::ParseEnvironmentMilliseconds(
            *value, "A11_SQLITE_CHUNK_STORE_CROSS_PROCESS_POLL_MS"));
  }
  if (const std::optional<std::string> value =
          internal::EnvironmentValue("A11_SQLITE_CHUNK_STORE_BLOB_GRACE_MS");
      value.has_value()) {
    ABSL_ASSIGN_OR_RETURN(options.blob_grace_period,
                          internal::ParseEnvironmentMilliseconds(
                              *value, "A11_SQLITE_CHUNK_STORE_BLOB_GRACE_MS"));
  }
  if (const std::optional<std::string> value =
          internal::EnvironmentValue("A11_SQLITE_CHUNK_STORE_SYNCHRONOUS");
      value.has_value()) {
    if (*value == "off" || *value == "OFF") {
      options.synchronous = internal::SqliteSynchronous::kOff;
    } else if (*value == "normal" || *value == "NORMAL") {
      options.synchronous = internal::SqliteSynchronous::kNormal;
    } else if (*value == "full" || *value == "FULL") {
      options.synchronous = internal::SqliteSynchronous::kFull;
    } else {
      return absl::InvalidArgumentError(
          "A11_SQLITE_CHUNK_STORE_SYNCHRONOUS must be off, normal, or full");
    }
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  return options;
}

// -------------------------------------------------------------------------
// Factory

std::string SQLiteChunkStoreFactory::DefaultRoot() {
  if (const std::optional<std::string> configured =
          internal::EnvironmentValue("A11_SQLITE_CHUNK_STORE_ROOT");
      configured.has_value() && !configured->empty()) {
    return *configured;
  }
  if (const std::optional<std::string> cache =
          internal::EnvironmentValue("XDG_CACHE_HOME");
      cache.has_value() && !cache->empty()) {
    return (std::filesystem::path(*cache) / "a11" / "chunks").string();
  }
  return (std::filesystem::path(HomeDirectory()) / ".cache" / "a11" / "chunks")
      .string();
}

absl::StatusOr<std::shared_ptr<SQLiteChunkStoreFactory>>
SQLiteChunkStoreFactory::Create(const std::string& root,
                                SQLiteChunkStoreOptions options) {
  if (root.empty()) {
    return absl::InvalidArgumentError(
        "SQLite chunk-store root must not be empty");
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  internal::SqliteDatabaseOptions database_options{
      .synchronous = options.synchronous,
      .cross_process_poll_interval = options.cross_process_poll_interval,
      .blob_grace_period = options.blob_grace_period,
  };
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<SqliteDatabase> database,
      SqliteDatabase::Open(std::filesystem::path(root), database_options));
  return std::make_shared<SQLiteChunkStoreFactory>(
      ConstructorToken{}, std::move(database), std::move(options));
}

absl::StatusOr<std::shared_ptr<SQLiteChunkStoreFactory>>
SQLiteChunkStoreFactory::Create(const std::string& root) {
  ABSL_ASSIGN_OR_RETURN(SQLiteChunkStoreOptions options,
                        SQLiteChunkStoreOptions::FromEnvironment());
  return Create(root, std::move(options));
}

absl::StatusOr<std::shared_ptr<SQLiteChunkStoreFactory>>
SQLiteChunkStoreFactory::Create() {
  return Create(DefaultRoot());
}

absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> SQLiteChunkStoreFactory::Open(
    std::string node_id) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(node_id));
  return std::make_shared<SQLiteChunkStore>(
      SQLiteChunkStore::ConstructorToken{}, std::move(node_id), database_,
      options_);
}

std::string SQLiteChunkStoreFactory::root() const {
  return database_->root().string();
}

a11::Future<size_t> SQLiteChunkStoreFactory::SweepOrphanBlobs() {
  std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<size_t>(
      [database = std::move(database)]() -> absl::StatusOr<size_t> {
        return database->SweepOrphanBlobs();
      });
}

// -------------------------------------------------------------------------
// Construction

absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> SQLiteChunkStore::Create(
    std::string node_id, const std::string& root,
    SQLiteChunkStoreOptions options) {
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<SQLiteChunkStoreFactory> factory,
      SQLiteChunkStoreFactory::Create(root, std::move(options)));
  return factory->Open(std::move(node_id));
}

absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> SQLiteChunkStore::Create(
    std::string node_id, std::string root) {
  ABSL_ASSIGN_OR_RETURN(SQLiteChunkStoreOptions options,
                        SQLiteChunkStoreOptions::FromEnvironment());
  return Create(std::move(node_id), root, std::move(options));
}

absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> SQLiteChunkStore::Create(
    std::string node_id) {
  return Create(std::move(node_id), SQLiteChunkStoreFactory::DefaultRoot());
}

absl::StatusOr<std::string> SQLiteChunkStore::GetId() const {
  return node_id_;
}

std::string SQLiteChunkStore::root() const {
  return database_->root().string();
}

// -------------------------------------------------------------------------
// Reads

a11::Future<data::NodeFragment> SQLiteChunkStore::Get(std::uint32_t seq,
                                                      absl::Time deadline) {
  return Read(ReadKind::kSequence, seq, deadline);
}

a11::Future<data::NodeFragment> SQLiteChunkStore::GetByArrivalOrder(
    std::uint64_t arrival_order, absl::Time deadline) {
  return Read(ReadKind::kArrivalOrder, arrival_order, deadline);
}

a11::Future<data::NodeFragment> SQLiteChunkStore::Read(ReadKind kind,
                                                       std::uint64_t value,
                                                       absl::Time deadline) {
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<data::NodeFragment>(
      [node_id, database, kind, value,
       deadline]() -> absl::StatusOr<data::NodeFragment> {
        while (true) {
          // Snapshot the generation event *before* the read, and retake it on
          // every pass. A handle taken after the read could already be latched,
          // and a hoisted one would be stale.
          const std::shared_ptr<thread::PermanentEvent> changed =
              database->WatchNode(node_id);

          std::optional<absl::StatusOr<data::NodeFragment>> outcome;
          ABSL_RETURN_IF_ERROR(database->RunRead(
              [&](SqliteConnection& connection) -> absl::Status {
                ABSL_ASSIGN_OR_RETURN(const SqliteNodeState state,
                                      LoadNodeState(connection, node_id));

                std::optional<FragmentRow> row;
                if (kind == ReadKind::kSequence) {
                  ABSL_ASSIGN_OR_RETURN(
                      sqlite3_stmt * statement,
                      connection.Prepare(absl::StrCat(
                          "SELECT ", kFragmentColumns,
                          " FROM fragments WHERE node_id = ?1 AND seq = ?2")));
                  BindText(statement, 1, node_id);
                  sqlite3_bind_int64(statement, 2,
                                     static_cast<std::int64_t>(value));
                  if (sqlite3_step(statement) == SQLITE_ROW) {
                    row = ReadFragmentRow(statement);
                  }
                } else {
                  ABSL_ASSIGN_OR_RETURN(
                      sqlite3_stmt * statement,
                      connection.Prepare(
                          absl::StrCat("SELECT ", kFragmentColumns,
                                       " FROM fragments WHERE node_id = ?1 AND "
                                       "arrival_order = ?2")));
                  BindText(statement, 1, node_id);
                  sqlite3_bind_int64(statement, 2,
                                     static_cast<std::int64_t>(value));
                  if (sqlite3_step(statement) == SQLITE_ROW) {
                    row = ReadFragmentRow(statement);
                  }
                }

                if (row.has_value()) {
                  outcome =
                      DecodeFragment(*database, node_id, *row, state.final_seq);
                  return absl::OkStatus();
                }
                if (!state.closed) {
                  return absl::OkStatus();  // Keep waiting.
                }
                if (state.status.has_value() && !state.status->ok()) {
                  outcome = *state.status;
                  return absl::OkStatus();
                }
                outcome = absl::NotFoundError(
                    kind == ReadKind::kSequence
                        ? absl::StrCat("Chunk store closed without seq ", value)
                        : absl::StrCat(
                              "Chunk store closed without arrival order ",
                              value));
                return absl::OkStatus();
              }));

          if (outcome.has_value()) {
            return *std::move(outcome);
          }
          ABSL_RETURN_IF_ERROR(internal::WaitForChange(
              changed, deadline,
              "Chunk store fragment was not available before the deadline"));
        }
      });
}

a11::Future<std::vector<std::optional<data::NodeFragment>>>
SQLiteChunkStore::Next(absl::Time deadline, size_t limit) {
  if (limit == 0) {
    return a11::FailedFuture<std::vector<std::optional<data::NodeFragment>>>(
        absl::InvalidArgumentError("limit must be positive"));
  }
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<std::vector<std::optional<data::NodeFragment>>>(
      [node_id, database, deadline, limit]()
          -> absl::StatusOr<std::vector<std::optional<data::NodeFragment>>> {
        std::vector<std::optional<data::NodeFragment>> fragments;
        // A batch may return `limit` fragments *and* a trailing end sentinel.
        fragments.reserve(limit + 1);

        while (true) {
          const std::shared_ptr<thread::PermanentEvent> changed =
              database->WatchNode(node_id);

          bool done = false;
          std::optional<absl::Status> terminal;
          // The shared cursor is persisted, so consuming is a write. The wait
          // happens after this transaction ends -- parking inside it would hold
          // the write lock and deadlock every other writer.
          ABSL_RETURN_IF_ERROR(database->RunWrite(
              [&](SqliteConnection& connection)
                  -> absl::StatusOr<std::vector<std::string>> {
                ABSL_ASSIGN_OR_RETURN(SqliteNodeState state,
                                      LoadNodeState(connection, node_id));
                std::uint64_t cursor = state.next_cursor;
                const std::uint64_t started_at = cursor;

                while (true) {
                  if (cursor > kMaxSeq) {
                    fragments.emplace_back(std::nullopt);
                    done = true;
                    break;
                  }
                  const bool final_was_read =
                      state.final_seq.has_value() && cursor > *state.final_seq;
                  if (final_was_read) {
                    if (state.status.has_value() && !state.status->ok()) {
                      if (fragments.empty()) {
                        terminal = *state.status;
                      }
                    } else {
                      fragments.emplace_back(std::nullopt);
                    }
                    done = true;
                    break;
                  }

                  const auto expected = static_cast<std::uint32_t>(cursor);
                  ABSL_ASSIGN_OR_RETURN(
                      sqlite3_stmt * statement,
                      connection.Prepare(absl::StrCat(
                          "SELECT ", kFragmentColumns,
                          " FROM fragments WHERE node_id = ?1 AND seq = ?2")));
                  BindText(statement, 1, node_id);
                  sqlite3_bind_int64(statement, 2,
                                     static_cast<std::int64_t>(expected));
                  const bool present = sqlite3_step(statement) == SQLITE_ROW;
                  std::optional<FragmentRow> row;
                  if (present) {
                    row = ReadFragmentRow(statement);
                  }
                  sqlite3_reset(statement);

                  if (!present && state.closed) {
                    if (state.status.has_value() && !state.status->ok()) {
                      if (fragments.empty()) {
                        terminal = *state.status;
                      }
                    } else {
                      fragments.emplace_back(std::nullopt);
                    }
                    done = true;
                    break;
                  }
                  // Checked after the end conditions above, which is what lets
                  // a full batch still carry a trailing sentinel.
                  if (fragments.size() == limit) {
                    done = true;
                    break;
                  }
                  if (!present) {
                    break;  // Gap in an open store: park and retry.
                  }

                  ABSL_ASSIGN_OR_RETURN(data::NodeFragment fragment,
                                        DecodeFragment(*database, node_id, *row,
                                                       state.final_seq));
                  fragments.emplace_back(std::move(fragment));
                  ++cursor;
                }

                if (cursor != started_at) {
                  ABSL_ASSIGN_OR_RETURN(
                      sqlite3_stmt * update,
                      connection.Prepare(
                          "UPDATE nodes SET next_cursor = ?2, updated_at = ?3 "
                          "WHERE id = ?1"));
                  BindText(update, 1, node_id);
                  sqlite3_bind_int64(update, 2,
                                     static_cast<std::int64_t>(cursor));
                  sqlite3_bind_int64(update, 3,
                                     absl::ToUnixMicros(absl::Now()));
                  ABSL_RETURN_IF_ERROR(
                      StepDone(update, "Cannot advance Next() cursor"));
                }
                // Advancing the cursor publishes no new data, so nobody is
                // woken: an empty list means no notification.
                return std::vector<std::string>{};
              }));

          if (terminal.has_value()) {
            return *terminal;
          }
          if (done) {
            return fragments;
          }

          absl::Status wait = internal::WaitForChange(
              changed, deadline,
              "Expected seq was not available before the deadline");
          if (!wait.ok()) {
            // Data already collected outranks a deadline: returning a short
            // batch is more useful than discarding it.
            if (!fragments.empty()) {
              return fragments;
            }
            return wait;
          }
        }
      });
}

// -------------------------------------------------------------------------
// Writes

a11::Future<std::uint32_t> SQLiteChunkStore::Put(data::NodeFragment fragment) {
  return internal::PutOneViaPutMany(
      [this](std::vector<data::NodeFragment> batch) {
        return PutMany(std::move(batch));
      },
      std::move(fragment), "SQLiteChunkStore");
}

a11::Future<std::vector<std::uint32_t>> SQLiteChunkStore::PutMany(
    std::vector<data::NodeFragment> fragments) {
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  const SQLiteChunkStoreOptions options = options_;
  return a11::Submit<std::vector<std::uint32_t>>(
      [node_id, database, options, fragments = std::move(fragments)]() mutable
          -> absl::StatusOr<std::vector<std::uint32_t>> {
        // Validation order is observable, because each rule reports a distinct
        // code. It mirrors LocalChunkStore exactly: per-fragment validity, then
        // the all-or-none sequence rule, and only then any state check.
        bool any_explicit = false;
        bool all_explicit = true;
        absl::flat_hash_set<std::uint32_t> explicit_sequences;
        for (const data::NodeFragment& fragment : fragments) {
          ABSL_RETURN_IF_ERROR(fragment.Validate());
          any_explicit = any_explicit || fragment.seq.has_value();
          all_explicit = all_explicit && fragment.seq.has_value();
          if (fragment.seq.has_value() &&
              !explicit_sequences.insert(*fragment.seq).second) {
            return absl::InvalidArgumentError(absl::StrCat(
                "Explicit seq ", *fragment.seq, " occurs more than once"));
          }
          // Unlike the in-memory and Redis backends, a NodeRef is a first-class
          // payload here: it becomes indexed columns rather than being refused.
        }
        if (any_explicit != all_explicit) {
          return absl::InvalidArgumentError(
              "Sequence numbers must be set on every fragment or none");
        }

        // Payload encoding, including externalizing oversized chunks to blob
        // files, happens before the transaction opens. That keeps file I/O off
        // the write lock, guarantees a blob is durable before the row naming it
        // commits, and means a busy-retry of the transaction cannot write the
        // same payload twice.
        const absl::Time now = absl::Now();
        std::vector<EncodedFragment> encoded;
        std::vector<std::string> owned_blobs;
        encoded.reserve(fragments.size());
        absl::Status encode_status = absl::OkStatus();
        for (const data::NodeFragment& fragment : fragments) {
          absl::StatusOr<EncodedFragment> row = EncodeFragment(
              *database, fragment, options.inline_data_threshold, now);
          if (!row.ok()) {
            encode_status = row.status();
            break;
          }
          if (!row->owned_blob.empty()) {
            owned_blobs.push_back(row->owned_blob);
          }
          encoded.push_back(*std::move(row));
        }
        if (!encode_status.ok()) {
          for (const std::string& blob : owned_blobs) {
            database->RemoveBlob(blob);
          }
          return encode_status;
        }

        std::vector<std::uint32_t> assigned;

        absl::Status outcome =
            database->RunWrite([&](SqliteConnection& connection)
                                   -> absl::StatusOr<std::vector<std::string>> {
              // A busy-retry re-runs this body, so discard what the previous
              // attempt assigned. The encoded payloads above are reused.
              assigned.clear();

              ABSL_ASSIGN_OR_RETURN(SqliteNodeState state,
                                    LoadNodeState(connection, node_id));
              if (state.closed) {
                return absl::FailedPreconditionError(absl::StrCat(
                    "Chunk store ", node_id, " is closed for writes"));
              }
              // Checked after the closed test, so an empty batch against a
              // closed store still fails.
              if (fragments.empty()) {
                return std::vector<std::string>{};
              }

              assigned.reserve(fragments.size());
              if (all_explicit) {
                for (const data::NodeFragment& fragment : fragments) {
                  assigned.push_back(*fragment.seq);
                }
              } else {
                // The probe starts at put_count, not max_seq + 1, so a store
                // with explicit gaps keeps filling them in arrival order.
                std::uint64_t candidate = state.put_count;
                for (size_t index = 0; index < fragments.size(); ++index) {
                  while (candidate <= kMaxSeq) {
                    ABSL_ASSIGN_OR_RETURN(
                        const bool taken,
                        FragmentExists(connection, node_id,
                                       static_cast<std::uint32_t>(candidate)));
                    if (!taken) {
                      break;
                    }
                    ++candidate;
                  }
                  if (candidate > kMaxSeq) {
                    return absl::ResourceExhaustedError(
                        "Maximum implicit sequence number exceeded");
                  }
                  assigned.push_back(static_cast<std::uint32_t>(candidate++));
                }
              }
              for (const std::uint32_t seq : assigned) {
                ABSL_ASSIGN_OR_RETURN(const bool taken,
                                      FragmentExists(connection, node_id, seq));
                if (taken) {
                  return absl::AlreadyExistsError(absl::StrCat(
                      "A fragment with seq ", seq, " already exists"));
                }
              }

              std::optional<std::uint32_t> batch_final;
              bool saw_final = false;
              for (size_t index = 0; index < fragments.size(); ++index) {
                if (fragments[index].continued) {
                  if (saw_final && !all_explicit) {
                    return absl::InvalidArgumentError(
                        "The final implicit fragment must be last");
                  }
                  continue;
                }
                if (saw_final) {
                  return absl::InvalidArgumentError(
                      "More than one fragment in the batch is marked final");
                }
                saw_final = true;
                batch_final = assigned[index];
              }
              if (batch_final.has_value() && state.final_seq.has_value() &&
                  batch_final != state.final_seq) {
                return absl::FailedPreconditionError(
                    "The chunk store already has a different final sequence");
              }
              const std::optional<std::uint32_t> pending_final =
                  batch_final.has_value() ? batch_final : state.final_seq;
              if (pending_final.has_value()) {
                for (const std::uint32_t seq : assigned) {
                  if (seq > *pending_final) {
                    return absl::InvalidArgumentError(
                        "A fragment sequence exceeds the final sequence");
                  }
                }
                if (state.max_seq.has_value() &&
                    *state.max_seq > *pending_final) {
                  return absl::InvalidArgumentError(
                      "An existing fragment exceeds the proposed final "
                      "sequence");
                }
              }

              // Everything below mutates.
              std::uint64_t added_bytes = 0;
              for (size_t index = 0; index < encoded.size(); ++index) {
                encoded[index].seq = assigned[index];
                encoded[index].arrival_order = state.put_count + index;
                added_bytes += encoded[index].payload_bytes;
              }

              ABSL_RETURN_IF_ERROR(
                  EnsureNodeRow(connection, node_id, options.owner_id, now));
              for (const EncodedFragment& row : encoded) {
                ABSL_RETURN_IF_ERROR(
                    InsertFragment(connection, node_id, row, now));
              }

              const std::uint32_t highest =
                  *std::max_element(assigned.begin(), assigned.end());
              const std::uint32_t new_max =
                  state.max_seq.has_value() ? std::max(*state.max_seq, highest)
                                            : highest;
              ABSL_ASSIGN_OR_RETURN(
                  sqlite3_stmt * update,
                  connection.Prepare(
                      "UPDATE nodes SET put_count = ?2, size = ?3, "
                      "data_bytes = ?4, max_seq = ?5, final_seq = ?6, "
                      "revision = revision + 1, updated_at = ?7 "
                      "WHERE id = ?1"));
              BindText(update, 1, node_id);
              sqlite3_bind_int64(update, 2,
                                 static_cast<std::int64_t>(state.put_count +
                                                           fragments.size()));
              sqlite3_bind_int64(
                  update, 3,
                  static_cast<std::int64_t>(state.size + fragments.size()));
              sqlite3_bind_int64(
                  update, 4,
                  static_cast<std::int64_t>(state.data_bytes + added_bytes));
              sqlite3_bind_int64(update, 5, static_cast<std::int64_t>(new_max));
              if (pending_final.has_value()) {
                sqlite3_bind_int64(update, 6,
                                   static_cast<std::int64_t>(*pending_final));
              } else {
                sqlite3_bind_null(update, 6);
              }
              sqlite3_bind_int64(update, 7, absl::ToUnixMicros(now));
              ABSL_RETURN_IF_ERROR(
                  StepDone(update, "Cannot update node counters"));

              return std::vector<std::string>{node_id};
            });

        if (!outcome.ok()) {
          // The rows never landed, so the blobs they would have referenced are
          // garbage. Unlink them now rather than leaving them for the sweeper.
          for (const std::string& blob : owned_blobs) {
            database->RemoveBlob(blob);
          }
          return outcome;
        }
        return assigned;
      });
}

a11::Future<data::NodeFragment> SQLiteChunkStore::ClearData(std::uint32_t seq) {
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<
      data::NodeFragment>([node_id, database,
                           seq]() -> absl::StatusOr<data::NodeFragment> {
    std::optional<data::NodeFragment> original;
    std::string freed_blob;

    ABSL_RETURN_IF_ERROR(
        database->RunWrite([&](SqliteConnection& connection)
                               -> absl::StatusOr<std::vector<std::string>> {
          original.reset();
          freed_blob.clear();

          ABSL_ASSIGN_OR_RETURN(const SqliteNodeState state,
                                LoadNodeState(connection, node_id));
          ABSL_ASSIGN_OR_RETURN(
              sqlite3_stmt * statement,
              connection.Prepare(absl::StrCat(
                  "SELECT ", kFragmentColumns,
                  " FROM fragments WHERE node_id = ?1 AND seq = ?2")));
          BindText(statement, 1, node_id);
          sqlite3_bind_int64(statement, 2, static_cast<std::int64_t>(seq));
          if (sqlite3_step(statement) != SQLITE_ROW) {
            return absl::NotFoundError(
                absl::StrCat("No fragment with seq ", seq, " exists"));
          }
          const FragmentRow row = ReadFragmentRow(statement);
          sqlite3_reset(statement);

          if (row.storage == kStorageNodeRef) {
            // A tombstone is Chunk-shaped in every backend, so handing one
            // back where a NodeRef was written would misinform the caller.
            return absl::UnimplementedError(absl::StrCat(
                "Cannot clear the payload of node-reference fragment ", seq));
          }
          ABSL_ASSIGN_OR_RETURN(
              data::NodeFragment decoded,
              DecodeFragment(*database, node_id, row, state.final_seq));
          original = std::move(decoded);
          if (row.storage == kStorageBlob) {
            freed_blob = row.ref;
          }
          // The decoded chunk holds the payload wherever it lived, so this
          // is the right figure for both inline rows and blob files.
          std::uint64_t cleared_bytes = 0;
          if (const auto* chunk = std::get_if<data::Chunk>(&original->data)) {
            cleared_bytes = chunk->data.size();
          }

          ABSL_ASSIGN_OR_RETURN(
              sqlite3_stmt * update,
              connection.Prepare("UPDATE fragments SET data = NULL, ref = '', "
                                 "storage = ?3, tombstone = 1 "
                                 "WHERE node_id = ?1 AND seq = ?2"));
          BindText(update, 1, node_id);
          sqlite3_bind_int64(update, 2, static_cast<std::int64_t>(seq));
          sqlite3_bind_int(update, 3, kStorageTombstone);
          ABSL_RETURN_IF_ERROR(StepDone(update, "Cannot clear fragment"));

          // The slot survives, so `size` is unchanged; only the cached byte
          // total shrinks.
          ABSL_ASSIGN_OR_RETURN(
              sqlite3_stmt * node_update,
              connection.Prepare(
                  "UPDATE nodes SET data_bytes = MAX(0, data_bytes - ?2), "
                  "revision = revision + 1, updated_at = ?3 WHERE id = ?1"));
          BindText(node_update, 1, node_id);
          sqlite3_bind_int64(node_update, 2,
                             static_cast<std::int64_t>(cleared_bytes));
          sqlite3_bind_int64(node_update, 3, absl::ToUnixMicros(absl::Now()));
          ABSL_RETURN_IF_ERROR(
              StepDone(node_update, "Cannot update node counters"));

          return std::vector<std::string>{node_id};
        }));

    // Unlink only after the commit: doing it first would lose the payload
    // if the transaction rolled back.
    if (!freed_blob.empty()) {
      database->RemoveBlob(freed_blob);
    }
    return *std::move(original);
  });
}

a11::Future<absl::Status> SQLiteChunkStore::CloseWritesWithStatus(
    absl::Status status, bool return_status_if_already_closed) {
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  const std::string owner_id = options_.owner_id;
  return a11::Submit<absl::Status>([node_id, database, owner_id,
                                    status = std::move(status),
                                    return_status_if_already_closed]() mutable
                                       -> absl::StatusOr<absl::Status> {
    std::optional<absl::Status> published;
    std::optional<absl::Status> already_closed_with;

    const absl::Status outcome =
        database->RunWrite([&](SqliteConnection& connection)
                               -> absl::StatusOr<std::vector<std::string>> {
          published.reset();
          already_closed_with.reset();

          ABSL_ASSIGN_OR_RETURN(const SqliteNodeState state,
                                LoadNodeState(connection, node_id));
          if (state.closed) {
            already_closed_with = state.status.value_or(absl::OkStatus());
            return std::vector<std::string>{};
          }

          const absl::Time now = absl::Now();
          ABSL_RETURN_IF_ERROR(
              EnsureNodeRow(connection, node_id, owner_id, now));
          ABSL_ASSIGN_OR_RETURN(const std::string packed,
                                data::PackStatus(status));
          ABSL_ASSIGN_OR_RETURN(
              sqlite3_stmt * update,
              connection.Prepare(
                  "UPDATE nodes SET closed = 1, status = ?2, "
                  "revision = revision + 1, updated_at = ?3 WHERE id = ?1"));
          BindText(update, 1, node_id);
          BindBlob(update, 2, packed);
          sqlite3_bind_int64(update, 3, absl::ToUnixMicros(now));
          ABSL_RETURN_IF_ERROR(StepDone(update, "Cannot close writes"));

          published = status;
          // Closing is what releases readers parked at a permanent gap.
          return std::vector<std::string>{node_id};
        });

    if (!outcome.ok()) {
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(outcome);
      return result;
    }
    if (already_closed_with.has_value()) {
      if (return_status_if_already_closed) {
        return absl::StatusOr<absl::Status>(std::in_place,
                                            *already_closed_with);
      }
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(absl::FailedPreconditionError(
          "Chunk store is already closed for writes"));
      return result;
    }
    return absl::StatusOr<absl::Status>(std::in_place, *std::move(published));
  });
}

// -------------------------------------------------------------------------
// Metadata

a11::Future<std::uint32_t> SQLiteChunkStore::GetSeqForArrivalOrder(
    std::uint64_t arrival_order) {
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<std::uint32_t>(
      [node_id, database, arrival_order]() -> absl::StatusOr<std::uint32_t> {
        std::optional<std::uint32_t> seq;
        // SQLite integers are signed, so an arrival order past INT64_MAX cannot
        // round-trip. It can never have been stored, so report it as absent
        // rather than letting the bind silently wrap.
        if (arrival_order <= static_cast<std::uint64_t>(kMaxSqliteInteger)) {
          ABSL_RETURN_IF_ERROR(database->RunRead(
              [&](SqliteConnection& connection) -> absl::Status {
                ABSL_ASSIGN_OR_RETURN(
                    sqlite3_stmt * statement,
                    connection.Prepare("SELECT seq FROM fragments WHERE "
                                       "node_id = ?1 AND arrival_order = ?2"));
                BindText(statement, 1, node_id);
                sqlite3_bind_int64(statement, 2,
                                   static_cast<std::int64_t>(arrival_order));
                if (sqlite3_step(statement) == SQLITE_ROW) {
                  seq = static_cast<std::uint32_t>(
                      sqlite3_column_int64(statement, 0));
                }
                return absl::OkStatus();
              }));
        }
        if (!seq.has_value()) {
          return absl::NotFoundError(
              absl::StrCat("No fragment has arrival order ", arrival_order));
        }
        return *seq;
      });
}

a11::Future<std::optional<std::uint32_t>> SQLiteChunkStore::GetFinalSeq() {
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<std::optional<std::uint32_t>>(
      [node_id, database]() -> absl::StatusOr<std::optional<std::uint32_t>> {
        std::optional<std::uint32_t> final_seq;
        ABSL_RETURN_IF_ERROR(database->RunRead(
            [&](SqliteConnection& connection) -> absl::Status {
              ABSL_ASSIGN_OR_RETURN(const SqliteNodeState state,
                                    LoadNodeState(connection, node_id));
              final_seq = state.final_seq;
              return absl::OkStatus();
            }));
        return final_seq;
      });
}

a11::Future<size_t> SQLiteChunkStore::Size() {
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<size_t>([node_id, database]() -> absl::StatusOr<size_t> {
    size_t size = 0;
    ABSL_RETURN_IF_ERROR(
        database->RunRead([&](SqliteConnection& connection) -> absl::Status {
          ABSL_ASSIGN_OR_RETURN(const SqliteNodeState state,
                                LoadNodeState(connection, node_id));
          size = state.size;
          return absl::OkStatus();
        }));
    return size;
  });
}

a11::Future<SQLiteChunkStoreMetadata> SQLiteChunkStore::GetMetadata() {
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<SQLiteChunkStoreMetadata>(
      [node_id, database]() -> absl::StatusOr<SQLiteChunkStoreMetadata> {
        SQLiteChunkStoreMetadata metadata;
        metadata.id = node_id;
        ABSL_RETURN_IF_ERROR(database->RunRead(
            [&](SqliteConnection& connection) -> absl::Status {
              ABSL_ASSIGN_OR_RETURN(const SqliteNodeState state,
                                    LoadNodeState(connection, node_id));
              metadata.owner_id = state.owner_id;
              metadata.closed = state.closed;
              metadata.status = state.status;
              metadata.final_seq = state.final_seq;
              metadata.size = state.size;
              metadata.total_chunks_put = state.put_count;
              metadata.next_cursor = state.next_cursor;
              metadata.data_bytes = state.data_bytes;
              metadata.max_seq = state.max_seq;
              metadata.revision = state.revision;
              metadata.created_at = state.created_at;
              metadata.updated_at = state.updated_at;
              return absl::OkStatus();
            }));
        return metadata;
      });
}

a11::Future<std::vector<data::NodeFragment>> SQLiteChunkStore::FindReferrers(
    size_t limit) {
  const std::string node_id = node_id_;
  const std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<std::vector<data::NodeFragment>>(
      [node_id, database,
       limit]() -> absl::StatusOr<std::vector<data::NodeFragment>> {
        std::vector<data::NodeFragment> referrers;
        ABSL_RETURN_IF_ERROR(database->RunRead([&](SqliteConnection& connection)
                                                   -> absl::Status {
          // Served by the partial index on node_ref_id, so the cost tracks
          // the number of referrers, not the size of the database.
          ABSL_ASSIGN_OR_RETURN(sqlite3_stmt * statement,
                                connection.Prepare(absl::StrCat(
                                    "SELECT node_id, ", kFragmentColumns,
                                    " FROM fragments WHERE node_ref_id = ?1 "
                                    "ORDER BY node_id, seq LIMIT ?2")));
          BindText(statement, 1, node_id);
          sqlite3_bind_int64(statement, 2, static_cast<std::int64_t>(limit));
          while (sqlite3_step(statement) == SQLITE_ROW) {
            const std::string referrer_id = ColumnText(statement, 0);
            // The row decoder expects the fragment columns at offset zero.
            FragmentRow row;
            {
              // Shift by one to account for the leading node_id column.
              row.seq = static_cast<std::uint32_t>(
                  sqlite3_column_int64(statement, 1));
              row.arrival_order = static_cast<std::uint64_t>(
                  sqlite3_column_int64(statement, 2));
              row.tombstone = sqlite3_column_int(statement, 3) != 0;
              row.storage = sqlite3_column_int(statement, 4);
              if (sqlite3_column_int(statement, 6) != 0) {
                row.timestamp =
                    absl::FromUnixMicros(sqlite3_column_int64(statement, 5));
              }
              row.mimetype = ColumnText(statement, 7);
              row.data = ColumnBlob(statement, 8);
              row.ref = ColumnText(statement, 9);
              if (!ColumnIsNull(statement, 10)) {
                row.metadata = ColumnBlob(statement, 10);
              }
              if (!ColumnIsNull(statement, 11)) {
                row.node_ref_id = ColumnText(statement, 11);
                row.node_ref_offset = static_cast<std::uint32_t>(
                    sqlite3_column_int64(statement, 12));
                if (!ColumnIsNull(statement, 13)) {
                  row.node_ref_length = static_cast<std::uint64_t>(
                      sqlite3_column_int64(statement, 13));
                }
              }
            }
            ABSL_ASSIGN_OR_RETURN(const SqliteNodeState referrer_state,
                                  LoadNodeState(connection, referrer_id));
            ABSL_ASSIGN_OR_RETURN(data::NodeFragment fragment,
                                  DecodeFragment(*database, referrer_id, row,
                                                 referrer_state.final_seq));
            referrers.push_back(std::move(fragment));
          }
          return absl::OkStatus();
        }));
        return referrers;
      });
}

a11::Future<size_t> SQLiteChunkStore::SweepOrphanBlobs() {
  std::shared_ptr<SqliteDatabase> database = database_;
  return a11::Submit<size_t>(
      [database = std::move(database)]() -> absl::StatusOr<size_t> {
        return database->SweepOrphanBlobs();
      });
}

}  // namespace a11::stores

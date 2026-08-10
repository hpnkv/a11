// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11_abseil/no_throw_status.h>
#include <pybind11_abseil/status_casters.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/chunk_store_reader.h"
#include "a11/stores/chunk_store_writer.h"
#include "a11/stores/local_chunk_store.h"
#ifdef A11_BUILD_REDIS
#include "a11/stores/redis_chunk_store.h"
#include "redis/client.h"
#endif
#ifdef A11_BUILD_SQLITE
#include "a11/stores/sqlite_chunk_store.h"
#endif
#include "python/bindings.h"
#include "python/casters.h"
#include "python/interop.h"

namespace a11::python {
namespace {

using stores::ChunkStore;

template <typename T>
a11::Future<T> InvalidFuture(const absl::Status& status) {
  return a11::FailedFuture<T>(status);
}

class PyChunkStore : public ChunkStore,
                     public py::trampoline_self_life_support {
 public:
  PyChunkStore() {
    absl::StatusOr<std::shared_ptr<PythonLoop>> loop = PythonLoop::Capture();
    if (loop.ok()) {
      loop_ = std::move(*loop);
    } else {
      loop_status_ = loop.status();
    }
  }

  a11::Future<data::NodeFragment> Get(std::uint32_t seq,
                                      absl::Time deadline) override {
    py::gil_scoped_acquire acquire;
    return Call<data::NodeFragment>("get", seq, TimeToPython(deadline));
  }

  a11::Future<data::NodeFragment> GetByArrivalOrder(
      std::uint64_t arrival_order, absl::Time deadline) override {
    py::gil_scoped_acquire acquire;
    return Call<data::NodeFragment>("get_by_arrival_order", arrival_order,
                                    TimeToPython(deadline));
  }

  a11::Future<std::vector<std::optional<data::NodeFragment>>> Next(
      absl::Time deadline, size_t limit) override {
    py::gil_scoped_acquire acquire;
    return Call<std::vector<std::optional<data::NodeFragment>>>(
        "next", TimeToPython(deadline), limit);
  }

  a11::Future<std::uint32_t> Put(data::NodeFragment fragment) override {
    return Call<std::uint32_t>("put", std::move(fragment));
  }

  a11::Future<std::vector<std::uint32_t>> PutMany(
      std::vector<data::NodeFragment> fragments) override {
    return Call<std::vector<std::uint32_t>>("put_many", std::move(fragments));
  }

  a11::Future<data::NodeFragment> ClearData(std::uint32_t seq) override {
    return Call<data::NodeFragment>("clear_data", seq);
  }

  a11::Future<std::uint32_t> GetSeqForArrivalOrder(
      std::uint64_t arrival_order) override {
    return Call<std::uint32_t>("get_seq_for_arrival_order", arrival_order);
  }

  a11::Future<std::optional<std::uint32_t>> GetFinalSeq() override {
    return Call<std::optional<std::uint32_t>>("get_final_seq");
  }

  a11::Future<absl::Status> CloseWritesWithStatus(
      absl::Status status, bool return_status_if_already_closed) override {
    py::gil_scoped_acquire acquire;
    return Call<absl::Status>("close_writes_with_status",
                              StatusToPython(status),
                              return_status_if_already_closed);
  }

  a11::Future<size_t> Size() override { return Call<size_t>("size"); }

  absl::StatusOr<std::string> GetId() const override {
    py::gil_scoped_acquire acquire;
    try {
      py::function override =
          py::get_override(static_cast<const ChunkStore*>(this), "get_id");
      if (!override) {
        return absl::UnimplementedError(
            "Python ChunkStore.get_id is not overridden");
      }
      return override().cast<std::string>();
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    } catch (const std::exception& error) {
      return absl::InvalidArgumentError(error.what());
    } catch (...) {
      return absl::UnknownError("Python ChunkStore.get_id raised an exception");
    }
  }

 private:
  template <typename T, typename... Args>
  a11::Future<T> Call(const char* name, Args&&... args) const {
    if (loop_ == nullptr) {
      return InvalidFuture<T>(loop_status_);
    }
    py::gil_scoped_acquire acquire;
    try {
      py::function override =
          py::get_override(static_cast<const ChunkStore*>(this), name);
      if (!override) {
        return InvalidFuture<T>(absl::UnimplementedError(
            std::string("Python ChunkStore.") + name + " is not overridden"));
      }
      return CallPythonAsync<T>(loop_, override, std::forward<Args>(args)...);
    } catch (py::error_already_set& error) {
      return InvalidFuture<T>(StatusFromPythonException(error));
    } catch (const std::exception& error) {
      return InvalidFuture<T>(absl::UnknownError(error.what()));
    } catch (...) {
      return InvalidFuture<T>(absl::UnknownError(
          "Calling a Python ChunkStore raised an exception"));
    }
  }

  std::shared_ptr<PythonLoop> loop_;
  absl::Status loop_status_ = absl::FailedPreconditionError(
      "Python ChunkStore has no asyncio event loop");
};

template <typename T>
py::object StoreFuture(a11::Future<T> future) {
  return FutureToPython(std::move(future));
}

absl::StatusOr<absl::Time> PythonDeadline(const py::object& deadline) {
  return TimeFromPython(deadline);
}

std::uint64_t UnsignedOption(const py::handle& value, std::uint64_t maximum,
                             const char* name) {
  try {
    if (!py::isinstance<py::int_>(value) ||
        py::cast<bool>(value.attr("__lt__")(0))) {
      ThrowStatus(absl::InvalidArgumentError(std::string(name) +
                                             " must be non-negative"));
    }
    const std::uint64_t converted = value.cast<std::uint64_t>();
    if (converted > maximum) {
      ThrowStatus(absl::OutOfRangeError(std::string(name) +
                                        " exceeds its supported range"));
    }
    return converted;
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  }
}

std::optional<std::uint64_t> OptionalUnsignedOption(const py::handle& value,
                                                    std::uint64_t maximum,
                                                    const char* name) {
  if (value.is_none()) {
    return std::nullopt;
  }
  return UnsignedOption(value, maximum, name);
}

void ValidateReaderOptions(const stores::ChunkStoreReaderOptions& options) {
  const absl::Status status = options.Validate();
  if (!status.ok()) {
    ThrowStatus(status);
  }
}

void ValidateWriterOptions(const stores::ChunkStoreWriterOptions& options) {
  const absl::Status status = options.Validate();
  if (!status.ok()) {
    ThrowStatus(status);
  }
}

}  // namespace

void BindStores(py::module_& module) {
  py::class_<stores::ChunkStoreReaderOptions>(module, "ChunkStoreReaderOptions")
      .def(py::init([](bool ordered, bool pop_chunks,
                       const py::handle& num_chunks_to_buffer,
                       const py::handle& offset,
                       const py::handle& max_chunks_to_read,
                       bool sticky_mimetype) {
             constexpr std::uint64_t kMaximum = std::uint64_t{1} << 32U;
             stores::ChunkStoreReaderOptions options{
                 .ordered = ordered,
                 .pop_chunks = pop_chunks,
                 .num_chunks_to_buffer = UnsignedOption(
                     num_chunks_to_buffer, kMaximum, "num_chunks_to_buffer"),
                 .offset = static_cast<std::uint32_t>(UnsignedOption(
                     offset, std::numeric_limits<std::uint32_t>::max(),
                     "offset")),
                 .max_chunks_to_read = OptionalUnsignedOption(
                     max_chunks_to_read, kMaximum, "max_chunks_to_read"),
                 .sticky_mimetype = sticky_mimetype};
             ValidateReaderOptions(options);
             return options;
           }),
           "Construct validated options for a ChunkStoreReader.",
           py::arg("ordered") = true, py::arg("pop_chunks") = false,
           py::arg("num_chunks_to_buffer") = 32, py::arg("offset") = 0,
           py::arg("max_chunks_to_read") = py::none(),
           py::arg("sticky_mimetype") = false)
      .def_readwrite("ordered", &stores::ChunkStoreReaderOptions::ordered,
                     "Whether chunks are delivered strictly in sequence order.")
      .def_readwrite("pop_chunks", &stores::ChunkStoreReaderOptions::pop_chunks,
                     "Whether chunks are removed from the store as they are "
                     "read.")
      .def_readwrite("num_chunks_to_buffer",
                     &stores::ChunkStoreReaderOptions::num_chunks_to_buffer,
                     "Maximum number of chunks to prefetch into the buffer.")
      .def_readwrite("offset", &stores::ChunkStoreReaderOptions::offset,
                     "Sequence number at which reading begins.")
      .def_readwrite("max_chunks_to_read",
                     &stores::ChunkStoreReaderOptions::max_chunks_to_read,
                     "Optional cap on the total number of chunks to read.")
      .def_readwrite(
          "sticky_mimetype", &stores::ChunkStoreReaderOptions::sticky_mimetype,
          "Whether ordered chunks inherit the last explicitly set mimetype.")
      .def("validate", &ValidateReaderOptions,
           "Raise if the options are not internally consistent.");

  py::class_<stores::ChunkStoreWriterOptions>(module, "ChunkStoreWriterOptions")
      .def(py::init([](const py::handle& offset,
                       const py::handle& max_chunks_to_write_at_once,
                       const py::handle& num_chunks_to_buffer,
                       bool sticky_mimetype) {
             constexpr std::uint64_t kMaximum = std::uint64_t{1} << 32U;
             stores::ChunkStoreWriterOptions options{
                 .offset = static_cast<std::uint32_t>(UnsignedOption(
                     offset, std::numeric_limits<std::uint32_t>::max(),
                     "offset")),
                 .max_chunks_to_write_at_once =
                     UnsignedOption(max_chunks_to_write_at_once, kMaximum,
                                    "max_chunks_to_write_at_once"),
                 .num_chunks_to_buffer = OptionalUnsignedOption(
                     num_chunks_to_buffer, kMaximum, "num_chunks_to_buffer"),
                 .sticky_mimetype = sticky_mimetype};
             ValidateWriterOptions(options);
             return options;
           }),
           "Construct validated options for a ChunkStoreWriter.",
           py::arg("offset") = 0, py::arg("max_chunks_to_write_at_once") = 8,
           py::arg("num_chunks_to_buffer") = py::none(),
           py::arg("sticky_mimetype") = false)
      .def_readwrite("offset", &stores::ChunkStoreWriterOptions::offset,
                     "Sequence number at which writing begins.")
      .def_readwrite(
          "max_chunks_to_write_at_once",
          &stores::ChunkStoreWriterOptions::max_chunks_to_write_at_once,
          "Maximum number of chunks flushed to the store per batch.")
      .def_readwrite("num_chunks_to_buffer",
                     &stores::ChunkStoreWriterOptions::num_chunks_to_buffer,
                     "Optional bound on the in-flight write buffer size.")
      .def_readwrite("sticky_mimetype",
                     &stores::ChunkStoreWriterOptions::sticky_mimetype,
                     "Whether repeated contiguous chunk mimetypes are omitted.")
      .def("validate", &ValidateWriterOptions,
           "Raise if the options are not internally consistent.");

  py::classh<ChunkStore, PyChunkStore> chunk_store(module, "ChunkStore");
  chunk_store
      .def(py::init<>(),
           "Construct the abstract base. Subclass this in Python to back an "
           "agent with a custom asynchronous chunk store; every data method "
           "returns an awaitable so callers never block the event loop.")
      .def(
          "get",
          [](const std::shared_ptr<ChunkStore>& self, std::uint32_t seq,
             const py::object& deadline) {
            absl::StatusOr<absl::Time> converted = PythonDeadline(deadline);
            if (!converted.ok()) {
              return StoreFuture(
                  InvalidFuture<data::NodeFragment>(converted.status()));
            }
            return StoreFuture(self->Get(seq, *converted));
          },
          R"doc(Await the fragment stored at a sequence number. The future resolves when the fragment is available or the optional deadline elapses.

Examples:
    Read back a fragment after retaining its assigned position:

    ```python
    fragment = await store.get(seq)
    ```
)doc",
          py::arg("seq"), py::arg("deadline") = py::none())
      .def(
          "get_by_arrival_order",
          [](const std::shared_ptr<ChunkStore>& self,
             std::uint64_t arrival_order, const py::object& deadline) {
            absl::StatusOr<absl::Time> converted = PythonDeadline(deadline);
            if (!converted.ok()) {
              return StoreFuture(
                  InvalidFuture<data::NodeFragment>(converted.status()));
            }
            return StoreFuture(
                self->GetByArrivalOrder(arrival_order, *converted));
          },
          "Await the fragment identified by the order in which it arrived "
          "rather than its sequence number. The future resolves when the "
          "fragment is present or the optional deadline passes.",
          py::arg("arrival_order"), py::arg("deadline") = py::none())
      .def(
          "next",
          [](const std::shared_ptr<ChunkStore>& self,
             const py::object& deadline, size_t limit) {
            absl::StatusOr<absl::Time> converted = PythonDeadline(deadline);
            if (!converted.ok()) {
              return StoreFuture(
                  InvalidFuture<std::vector<std::optional<data::NodeFragment>>>(
                      converted.status()));
            }
            return StoreFuture(self->Next(*converted, limit));
          },
          "Await up to `limit` of the next available fragments as a stream. "
          "This is the primary way an agent consumes chunks as they are "
          "produced: the future resolves with whatever is ready before the "
          "optional deadline, and slots may be None when a fragment is "
          "missing. Loop over successive calls to follow a growing store.",
          py::arg("deadline") = py::none(), py::arg("limit") = 1)
      .def(
          "put",
          [](const std::shared_ptr<ChunkStore>& self,
             data::NodeFragment fragment) {
            return StoreFuture(self->Put(std::move(fragment)));
          },
          R"doc(Append a single fragment and await its assigned sequence number. The future resolves once the backing store accepts the write.

Examples:
    Store a fragment and retain its assigned position:

    ```python
    seq = await store.put(fragment)
    ```
)doc",
          py::arg("fragment"))
      .def(
          "put_many",
          [](const std::shared_ptr<ChunkStore>& self,
             std::vector<data::NodeFragment> fragments) {
            return StoreFuture(self->PutMany(std::move(fragments)));
          },
          "Append several fragments in one batch and await their assigned "
          "sequence numbers. Prefer this over repeated `put` calls when an "
          "agent emits many chunks at once, to reduce round-trips.",
          py::arg("fragments"))
      .def(
          "clear_data",
          [](const std::shared_ptr<ChunkStore>& self, std::uint32_t seq) {
            return StoreFuture(self->ClearData(seq));
          },
          "Erase the payload of the fragment at a sequence number while "
          "keeping its slot, and await the resulting fragment.",
          py::arg("seq"))
      .def(
          "get_seq_for_arrival_order",
          [](const std::shared_ptr<ChunkStore>& self,
             std::uint64_t arrival_order) {
            return StoreFuture(self->GetSeqForArrivalOrder(arrival_order));
          },
          "Await the sequence number that corresponds to a given arrival "
          "order.",
          py::arg("arrival_order"))
      .def(
          "get_final_seq",
          [](const std::shared_ptr<ChunkStore>& self) {
            return StoreFuture(self->GetFinalSeq());
          },
          "Await the explicitly marked final sequence, or None if no "
          "fragment has declared finality. Finality is independent of write "
          "closure: closing the store does not create a final sequence.")
      .def(
          "close_writes_with_status",
          [](const std::shared_ptr<ChunkStore>& self, const py::handle& status,
             bool return_existing) {
            return StoreFuture(self->CloseWritesWithStatus(
                StatusFromPython(status), return_existing));
          },
          R"doc(Seal the store against further writes with a terminal status and await completion. Waiting readers are released. With `return_status_if_already_closed`, a repeated close returns the status recorded by the first.

Examples:
    Publish clean producer completion to all readers:

    ```python
    await store.close_writes_with_status(Status.ok())
    ```
)doc",
          py::arg("status"), py::arg("return_status_if_already_closed") = false)
      .def(
          "size",
          [](const std::shared_ptr<ChunkStore>& self) {
            return StoreFuture(self->Size());
          },
          "Await the number of fragments currently in the store.")
      .def(
          "get_id",
          [](const ChunkStore& self) { return ValueOrThrow(self.GetId()); },
          "Return the store's node identifier. Raises if a Python subclass "
          "does not override `get_id`.");

  py::classh<stores::LocalChunkStore, ChunkStore>(module, "LocalChunkStore")
      .def(
          py::init([](std::string id) {
            return ValueOrThrow(stores::LocalChunkStore::Create(std::move(id)));
          }),
          "Create an in-memory ChunkStore identified by `id`. This is the "
          "default backing store for an agent running in a single process: "
          "all reads and writes stay in local memory yet still return "
          "awaitables, so it composes with the same async reader and writer "
          "as remote stores.",
          py::arg("id"))
      .def_static(
          "create",
          [](std::string node_id) {
            return ValueOrThrow(
                stores::LocalChunkStore::Create(std::move(node_id)));
          },
          "Create a LocalChunkStore for the given node id. Equivalent to the "
          "constructor; use whichever reads more clearly at the call site.",
          py::arg("node_id"));

#ifdef A11_BUILD_REDIS
  py::class_<stores::RedisChunkStoreOptions>(
      module, "RedisChunkStoreOptions",
      "Key layout and inline-payload policy for RedisChunkStore.")
      .def(py::init([](std::string key_prefix,
                       const py::handle& inline_data_threshold) {
             stores::RedisChunkStoreOptions options{
                 .key_prefix = std::move(key_prefix),
                 .inline_data_threshold = static_cast<size_t>(UnsignedOption(
                     inline_data_threshold, std::numeric_limits<size_t>::max(),
                     "inline_data_threshold")),
             };
             const absl::Status status = options.Validate();
             if (!status.ok()) {
               ThrowStatus(status);
             }
             return options;
           }),
           "Construct validated Redis chunk-store options.",
           py::arg("key_prefix") = "a11:",
           py::arg("inline_data_threshold") = 256 * 1024)
      .def_readwrite("key_prefix", &stores::RedisChunkStoreOptions::key_prefix,
                     "Prefix before the per-node Redis Cluster hash tag.")
      .def_readwrite(
          "inline_data_threshold",
          &stores::RedisChunkStoreOptions::inline_data_threshold,
          "Chunk data larger than this many bytes uses the blob hash.")
      .def(
          "validate",
          [](const stores::RedisChunkStoreOptions& self) {
            const absl::Status status = self.Validate();
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Raise if the key layout policy is invalid.")
      .def_static(
          "from_environment",
          [] {
            return ValueOrThrow(
                stores::RedisChunkStoreOptions::FromEnvironment());
          },
          "Read the A11_REDIS_CHUNK_STORE_* environment variables.")
      .def(
          "__eq__",
          [](const stores::RedisChunkStoreOptions& self,
             const stores::RedisChunkStoreOptions& other) {
            return self == other;
          },
          py::is_operator());

  py::class_<stores::RedisChunkStoreKeys>(
      module, "RedisChunkStoreKeys",
      "The sharding-safe Redis keys owned by one node stream.")
      .def_readonly("metadata", &stores::RedisChunkStoreKeys::metadata,
                    "Hash containing node-level metadata.")
      .def_readonly("stream", &stores::RedisChunkStoreKeys::stream,
                    "Redis Stream containing chunk and control entries.")
      .def_readonly("sequence_index",
                    &stores::RedisChunkStoreKeys::sequence_index,
                    "Sequence-to-stream-entry hash.")
      .def_readonly("arrival_index",
                    &stores::RedisChunkStoreKeys::arrival_index,
                    "Arrival-order-to-sequence hash.")
      .def_readonly("blobs", &stores::RedisChunkStoreKeys::blobs,
                    "Hash containing large encoded chunk payloads.")
      .def_readonly("events", &stores::RedisChunkStoreKeys::events,
                    "Pub/Sub channel used for invalidation notifications.")
      .def("script_keys", &stores::RedisChunkStoreKeys::ScriptKeys,
           "Return keys in the stable order used by the Lua state machine.")
      .def(
          "__eq__",
          [](const stores::RedisChunkStoreKeys& self,
             const stores::RedisChunkStoreKeys& other) {
            return self == other;
          },
          py::is_operator());

  py::class_<stores::RedisChunkStoreMetadata>(
      module, "RedisChunkStoreMetadata",
      "Node-level Redis state read without iterating over chunk entries.")
      .def_readonly("id", &stores::RedisChunkStoreMetadata::id,
                    "The owning AsyncNode identifier.")
      .def_readonly("closed", &stores::RedisChunkStoreMetadata::closed,
                    "Whether the store rejects new writes.")
      .def_property_readonly(
          "status",
          [](const stores::RedisChunkStoreMetadata& self) -> py::object {
            return self.status.has_value() ? StatusToPython(*self.status)
                                           : py::none();
          },
          "The terminal Status when closed, otherwise None.")
      .def_readonly("final_seq", &stores::RedisChunkStoreMetadata::final_seq,
                    "The declared final sequence, if one has arrived.")
      .def_readonly("size", &stores::RedisChunkStoreMetadata::size,
                    "Number of chunk slots in the store.")
      .def_readonly("total_chunks_put",
                    &stores::RedisChunkStoreMetadata::total_chunks_put,
                    "Number of chunks appended over the store lifetime.")
      .def_readonly("next_cursor",
                    &stores::RedisChunkStoreMetadata::next_cursor,
                    "Global SPMC cursor used by next().")
      .def_readonly("max_seq", &stores::RedisChunkStoreMetadata::max_seq,
                    "Largest sequence currently present.")
      .def_readonly("revision", &stores::RedisChunkStoreMetadata::revision,
                    "Monotonic mutation generation published to waiters.");

  py::classh<stores::RedisChunkStore, ChunkStore>(
      module, "RedisChunkStore",
      "A persistent, multi-process ChunkStore backed by Redis Streams.")
      .def(py::init([](std::string id, const py::object& client_value,
                       const py::object& options_value) {
             std::shared_ptr<redis::Client> client;
             if (client_value.is_none()) {
               client = ValueOrThrow(redis::DefaultClient());
             } else {
               client = client_value.cast<std::shared_ptr<redis::Client>>();
             }
             stores::RedisChunkStoreOptions options =
                 options_value.is_none()
                     ? ValueOrThrow(
                           stores::RedisChunkStoreOptions::FromEnvironment())
                     : options_value.cast<stores::RedisChunkStoreOptions>();
             return ValueOrThrow(stores::RedisChunkStore::Create(
                 std::move(id), std::move(client), std::move(options)));
           }),
           "Create a Redis store. By default it composes the process-global "
           "environment-configured RedisClient.",
           py::arg("id"), py::arg("client") = py::none(),
           py::arg("options") = py::none(), py::keep_alive<1, 2>())
      .def_static(
          "create",
          [](std::string id, const py::object& client_value,
             const py::object& options_value) {
            std::shared_ptr<redis::Client> client;
            if (client_value.is_none()) {
              client = ValueOrThrow(redis::DefaultClient());
            } else {
              client = client_value.cast<std::shared_ptr<redis::Client>>();
            }
            stores::RedisChunkStoreOptions options =
                options_value.is_none()
                    ? ValueOrThrow(
                          stores::RedisChunkStoreOptions::FromEnvironment())
                    : options_value.cast<stores::RedisChunkStoreOptions>();
            return ValueOrThrow(stores::RedisChunkStore::Create(
                std::move(id), std::move(client), std::move(options)));
          },
          "Create a Redis store with optional injected client and options.",
          py::arg("id"), py::arg("client") = py::none(),
          py::arg("options") = py::none(), py::keep_alive<0, 2>())
      .def(
          "initialize",
          [](const std::shared_ptr<stores::RedisChunkStore>& self) {
            return StoreFuture(self->Initialize());
          },
          "Ensure node metadata exists without writing chunk data.")
      .def(
          "get_metadata",
          [](const std::shared_ptr<stores::RedisChunkStore>& self) {
            return StoreFuture(self->GetMetadata());
          },
          "Read node-level state without iterating over chunks.")
      .def_property_readonly("client", &stores::RedisChunkStore::client,
                             "The explicitly composed RedisClient.")
      .def_property_readonly(
          "options",
          [](const stores::RedisChunkStore& self) { return self.options(); },
          "A copy of this store's key and payload policy.")
      .def_property_readonly(
          "keys",
          [](const stores::RedisChunkStore& self) { return self.keys(); },
          "A copy of the sharding-safe Redis key layout.");
#endif

#ifdef A11_BUILD_SQLITE
  py::enum_<stores::internal::SqliteSynchronous>(
      module, "SQLiteSynchronous",
      "How much durability a SQLite chunk store trades for write throughput.")
      .value("OFF", stores::internal::SqliteSynchronous::kOff,
             "Fastest; a machine crash can corrupt recent commits.")
      .value("NORMAL", stores::internal::SqliteSynchronous::kNormal,
             "Default; survives an application crash, may lose the newest "
             "commits on power loss.")
      .value("FULL", stores::internal::SqliteSynchronous::kFull,
             "Every commit is fsynced.");

  py::class_<stores::SQLiteChunkStoreOptions>(
      module, "SQLiteChunkStoreOptions",
      "Payload, ownership and durability policy for SQLiteChunkStore.")
      .def(
          py::init([](const py::handle& inline_data_threshold,
                      std::string owner_id,
                      stores::internal::SqliteSynchronous synchronous,
                      const py::object& cross_process_poll_interval,
                      const py::object& blob_grace_period) {
            stores::SQLiteChunkStoreOptions options;
            options.inline_data_threshold = static_cast<size_t>(UnsignedOption(
                inline_data_threshold, std::numeric_limits<size_t>::max(),
                "inline_data_threshold"));
            options.owner_id = std::move(owner_id);
            options.synchronous = synchronous;
            if (!cross_process_poll_interval.is_none()) {
              options.cross_process_poll_interval = ValueOrThrow(
                  DurationFromPython(cross_process_poll_interval,
                                     "cross_process_poll_interval"));
            }
            if (!blob_grace_period.is_none()) {
              options.blob_grace_period = ValueOrThrow(
                  DurationFromPython(blob_grace_period, "blob_grace_period"));
            }
            const absl::Status status = options.Validate();
            if (!status.ok()) {
              ThrowStatus(status);
            }
            return options;
          }),
          "Construct validated SQLite chunk-store options.",
          py::arg("inline_data_threshold") = 128 * 1024,
          py::arg("owner_id") = "",
          py::arg("synchronous") = stores::internal::SqliteSynchronous::kNormal,
          py::arg("cross_process_poll_interval") = py::none(),
          py::arg("blob_grace_period") = py::none())
      .def_readwrite(
          "inline_data_threshold",
          &stores::SQLiteChunkStoreOptions::inline_data_threshold,
          "Payloads larger than this many bytes move into a blob file.")
      .def_readwrite("owner_id", &stores::SQLiteChunkStoreOptions::owner_id,
                     "Owner recorded on the node row; carries no enforcement.")
      .def_readwrite("synchronous",
                     &stores::SQLiteChunkStoreOptions::synchronous,
                     "Durability level applied with PRAGMA synchronous.")
      .def_property(
          "cross_process_poll_interval",
          [](const stores::SQLiteChunkStoreOptions& self) {
            return DurationToPython(self.cross_process_poll_interval);
          },
          [](stores::SQLiteChunkStoreOptions& self, const py::handle& value) {
            self.cross_process_poll_interval = ValueOrThrow(
                DurationFromPython(value, "cross_process_poll_interval"));
          },
          "How often to notice other processes' commits; zero disables it.")
      .def_property(
          "blob_grace_period",
          [](const stores::SQLiteChunkStoreOptions& self) {
            return DurationToPython(self.blob_grace_period);
          },
          [](stores::SQLiteChunkStoreOptions& self, const py::handle& value) {
            self.blob_grace_period =
                ValueOrThrow(DurationFromPython(value, "blob_grace_period"));
          },
          "How long an unreferenced blob survives before a sweep removes it.")
      .def(
          "validate",
          [](const stores::SQLiteChunkStoreOptions& self) {
            const absl::Status status = self.Validate();
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Raise if the storage policy is invalid.")
      .def_static(
          "from_environment",
          [] {
            return ValueOrThrow(
                stores::SQLiteChunkStoreOptions::FromEnvironment());
          },
          "Read the A11_SQLITE_CHUNK_STORE_* environment variables.")
      .def(
          "__eq__",
          [](const stores::SQLiteChunkStoreOptions& self,
             const stores::SQLiteChunkStoreOptions& other) {
            return self == other;
          },
          py::is_operator());

  py::class_<stores::SQLiteChunkStoreMetadata>(
      module, "SQLiteChunkStoreMetadata",
      "Node-level SQLite state read without listing fragments.")
      .def_readonly("id", &stores::SQLiteChunkStoreMetadata::id,
                    "The owning AsyncNode identifier.")
      .def_readonly("owner_id", &stores::SQLiteChunkStoreMetadata::owner_id,
                    "Owner recorded on the node row, possibly empty.")
      .def_readonly("closed", &stores::SQLiteChunkStoreMetadata::closed,
                    "Whether the store rejects new writes.")
      .def_property_readonly(
          "status",
          [](const stores::SQLiteChunkStoreMetadata& self) -> py::object {
            return self.status.has_value() ? StatusToPython(*self.status)
                                           : py::none();
          },
          "The terminal Status when closed, otherwise None.")
      .def_readonly("final_seq", &stores::SQLiteChunkStoreMetadata::final_seq,
                    "The declared final sequence, if one has arrived.")
      .def_readonly("size", &stores::SQLiteChunkStoreMetadata::size,
                    "Number of fragment slots, tombstones included.")
      .def_readonly("total_chunks_put",
                    &stores::SQLiteChunkStoreMetadata::total_chunks_put,
                    "Fragments accepted over the store lifetime.")
      .def_readonly("next_cursor",
                    &stores::SQLiteChunkStoreMetadata::next_cursor,
                    "The next sequence the shared next() cursor will want.")
      .def_readonly("data_bytes", &stores::SQLiteChunkStoreMetadata::data_bytes,
                    "Cached total of stored payload bytes.")
      .def_readonly("max_seq", &stores::SQLiteChunkStoreMetadata::max_seq,
                    "Largest sequence currently present.")
      .def_readonly("revision", &stores::SQLiteChunkStoreMetadata::revision,
                    "Monotonic mutation generation.")
      .def_property_readonly(
          "created_at",
          [](const stores::SQLiteChunkStoreMetadata& self) {
            return TimeToPython(self.created_at);
          },
          "When the node row was created by its first accepted write.")
      .def_property_readonly(
          "updated_at",
          [](const stores::SQLiteChunkStoreMetadata& self) {
            return TimeToPython(self.updated_at);
          },
          "When the node row was last mutated.");

  py::classh<stores::SQLiteChunkStore, ChunkStore>(
      module, "SQLiteChunkStore",
      "A durable, embedded ChunkStore backed by SQLite and blob files.")
      .def(py::init([](std::string id, const py::object& root,
                       const py::object& options_value) {
             stores::SQLiteChunkStoreOptions options =
                 options_value.is_none()
                     ? ValueOrThrow(
                           stores::SQLiteChunkStoreOptions::FromEnvironment())
                     : options_value.cast<stores::SQLiteChunkStoreOptions>();
             std::string directory =
                 root.is_none() ? stores::SQLiteChunkStoreFactory::DefaultRoot()
                                : root.cast<std::string>();
             return ValueOrThrow(stores::SQLiteChunkStore::Create(
                 std::move(id), std::move(directory), std::move(options)));
           }),
           "Create a SQLite store. Without a root it uses the default cache "
           "directory; stores sharing a root share one database.",
           py::arg("id"), py::arg("root") = py::none(),
           py::arg("options") = py::none())
      .def_static(
          "create",
          [](std::string id, const py::object& root,
             const py::object& options_value) {
            stores::SQLiteChunkStoreOptions options =
                options_value.is_none()
                    ? ValueOrThrow(
                          stores::SQLiteChunkStoreOptions::FromEnvironment())
                    : options_value.cast<stores::SQLiteChunkStoreOptions>();
            std::string directory =
                root.is_none() ? stores::SQLiteChunkStoreFactory::DefaultRoot()
                               : root.cast<std::string>();
            return ValueOrThrow(stores::SQLiteChunkStore::Create(
                std::move(id), std::move(directory), std::move(options)));
          },
          "Create a SQLite store with an optional root and options.",
          py::arg("id"), py::arg("root") = py::none(),
          py::arg("options") = py::none())
      .def(
          "get_metadata",
          [](const std::shared_ptr<stores::SQLiteChunkStore>& self) {
            return StoreFuture(self->GetMetadata());
          },
          "Read node-level state without listing fragments.")
      .def(
          "find_referrers",
          [](const std::shared_ptr<stores::SQLiteChunkStore>& self,
             const py::handle& limit) {
            return StoreFuture(
                self->FindReferrers(static_cast<size_t>(UnsignedOption(
                    limit, std::numeric_limits<size_t>::max(), "limit"))));
          },
          "Find fragments elsewhere in the database whose NodeRef points at "
          "this node, using the node-reference index rather than a scan.",
          py::arg("limit") = 100)
      .def(
          "sweep_orphan_blobs",
          [](const std::shared_ptr<stores::SQLiteChunkStore>& self) {
            return StoreFuture(self->SweepOrphanBlobs());
          },
          "Delete unreferenced blob files older than the grace period.")
      .def_property_readonly(
          "options",
          [](const stores::SQLiteChunkStore& self) { return self.options(); },
          "A copy of this store's storage policy.")
      .def_property_readonly("root", &stores::SQLiteChunkStore::root,
                             "The storage root this store reads and writes.");

  py::classh<stores::SQLiteChunkStoreFactory>(
      module, "SQLiteChunkStoreFactory",
      "Creates SQLiteChunkStores that share one database per storage root.")
      .def(
          py::init([](const py::object& root, const py::object& options_value) {
            stores::SQLiteChunkStoreOptions options =
                options_value.is_none()
                    ? ValueOrThrow(
                          stores::SQLiteChunkStoreOptions::FromEnvironment())
                    : options_value.cast<stores::SQLiteChunkStoreOptions>();
            std::string directory =
                root.is_none() ? stores::SQLiteChunkStoreFactory::DefaultRoot()
                               : root.cast<std::string>();
            return ValueOrThrow(stores::SQLiteChunkStoreFactory::Create(
                std::move(directory), std::move(options)));
          }),
          "Create a factory rooted at a directory, defaulting to the A11 "
          "cache directory.",
          py::arg("root") = py::none(), py::arg("options") = py::none())
      .def_static(
          "default_root", &stores::SQLiteChunkStoreFactory::DefaultRoot,
          "The process-wide default storage root: "
          "$A11_SQLITE_CHUNK_STORE_ROOT, else $XDG_CACHE_HOME/a11/chunks, "
          "else ~/.cache/a11/chunks.")
      .def(
          "open",
          [](const std::shared_ptr<stores::SQLiteChunkStoreFactory>& self,
             std::string node_id) {
            return ValueOrThrow(self->Open(std::move(node_id)));
          },
          "Open a store for a node id under this factory's root.",
          py::arg("node_id"))
      .def(
          "__call__",
          [](const std::shared_ptr<stores::SQLiteChunkStoreFactory>& self,
             std::string node_id) {
            return ValueOrThrow(self->Open(std::move(node_id)));
          },
          "Open a store, so the factory can be passed directly wherever a "
          "chunk_store_factory callable is expected.",
          py::arg("node_id"))
      .def(
          "sweep_orphan_blobs",
          [](const std::shared_ptr<stores::SQLiteChunkStoreFactory>& self) {
            return StoreFuture(self->SweepOrphanBlobs());
          },
          "Delete unreferenced blob files older than the grace period.")
      .def_property_readonly("root", &stores::SQLiteChunkStoreFactory::root,
                             "The root this factory creates stores under.")
      .def_property_readonly(
          "options",
          [](const stores::SQLiteChunkStoreFactory& self) {
            return self.options();
          },
          "A copy of the storage policy applied to every store created here.");
#endif

  py::classh<stores::ChunkStoreReader>(module, "ChunkStoreReader",
                                       py::dynamic_attr())
      .def(py::init([](std::shared_ptr<ChunkStore> store,
                       stores::ChunkStoreReaderOptions options) {
             return ValueOrThrow(
                 stores::ChunkStoreReader::Create(std::move(store), options));
           }),
           "Create a streaming reader over a ChunkStore. The reader runs a "
           "background pump that prefetches fragments per the given options, "
           "so an agent can consume a store as an async stream via `next` "
           "without managing sequence numbers itself. The reader keeps the "
           "store alive for its lifetime.",
           py::arg("store"),
           py::arg("options") = stores::ChunkStoreReaderOptions{},
           py::keep_alive<1, 2>())
      .def("ensure_started", &stores::ChunkStoreReader::EnsureStarted,
           "Start the background read pump if it is not already running. "
           "Reading normally starts it lazily; call this to begin buffering "
           "before the first `next`.")
      .def("cancel", &stores::ChunkStoreReader::Cancel,
           "Stop the background read pump. Pending `next` awaitables are "
           "resolved and no further chunks are fetched.")
      .def(
          "get_status",
          [](const stores::ChunkStoreReader& self) {
            return StatusToPython(self.GetStatus());
          },
          "Return the reader's current status. An agent can inspect this to "
          "distinguish a healthy stream from one that has failed or ended.")
      .def(
          "wait",
          [](const stores::ChunkStoreReader& self) {
            return StoreFuture(self.Done());
          },
          "Await completion of the background read pump. The returned future "
          "resolves once the reader has drained the store or been cancelled.")
      .def(
          "next",
          [](const std::shared_ptr<stores::ChunkStoreReader>& self,
             const py::object& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return StoreFuture(
                  InvalidFuture<std::optional<data::NodeFragment>>(
                      converted.status()));
            }
            return StoreFuture(self->Next(*converted));
          },
          "Await the next buffered fragment, or None when the stream ends "
          "or the optional timeout elapses. This is the main consumption "
          "loop for an agent: repeatedly await `next` to pull chunks as the "
          "background pump makes them available. Pass None to wait "
          "indefinitely.",
          py::arg("timeout") = py::none())
      .def_property_readonly("store", &stores::ChunkStoreReader::store,
                             "The ChunkStore this reader draws fragments from.")
      .def_property_readonly("options", &stores::ChunkStoreReader::options,
                             "The ChunkStoreReaderOptions this reader was "
                             "created with.")
      .def_property_readonly("buffer_size",
                             &stores::ChunkStoreReader::buffer_size,
                             "Number of prefetched fragments currently held in "
                             "the reader's buffer.");

  py::classh<stores::ChunkStoreWriter>(module, "ChunkStoreWriter",
                                       py::dynamic_attr())
      .def(py::init([](std::shared_ptr<ChunkStore> store,
                       stores::ChunkStoreWriterOptions options) {
             return ValueOrThrow(
                 stores::ChunkStoreWriter::Create(std::move(store), options));
           }),
           "Create a streaming writer over a ChunkStore. The writer runs a "
           "background flush loop with a bounded queue, so an agent can emit "
           "chunks as an async stream while backpressure and batching are "
           "handled for it. The writer keeps the store alive for its "
           "lifetime.",
           py::arg("store"),
           py::arg("options") = stores::ChunkStoreWriterOptions{},
           py::keep_alive<1, 2>())
      .def("ensure_started", &stores::ChunkStoreWriter::EnsureStarted,
           "Start the background flush loop if it is not already running. "
           "Writing normally starts it lazily; call this to begin flushing "
           "before the first chunk is enqueued.")
      .def(
          "put_chunk",
          [](const std::shared_ptr<stores::ChunkStoreWriter>& self,
             data::Chunk chunk, const py::handle& seq, bool final) {
            const std::optional<std::uint64_t> converted =
                OptionalUnsignedOption(
                    seq, std::numeric_limits<std::uint32_t>::max(), "seq");
            return StoreFuture(
                self->PutChunk(std::move(chunk),
                               converted.has_value()
                                   ? std::optional<std::uint32_t>(
                                         static_cast<std::uint32_t>(*converted))
                                   : std::nullopt,
                               final));
          },
          "Write one chunk and await its assigned sequence number. This is the "
          "simple producer path for an agent: the returned future resolves "
          "only once the chunk is confirmed by the store. Provide `seq` to "
          "pin an explicit sequence number, and set `final` to mark this "
          "chunk as the last in the stream.",
          py::arg("chunk"), py::arg("seq") = py::none(),
          py::arg("final") = false)
      .def(
          "enqueue_chunk",
          [](const std::shared_ptr<stores::ChunkStoreWriter>& self,
             data::Chunk chunk, const py::handle& seq, bool final) {
            const std::optional<std::uint64_t> converted =
                OptionalUnsignedOption(
                    seq, std::numeric_limits<std::uint32_t>::max(), "seq");
            stores::ChunkStoreWrite write = self->EnqueueChunk(
                std::move(chunk),
                converted.has_value()
                    ? std::optional<std::uint32_t>(
                          static_cast<std::uint32_t>(*converted))
                    : std::nullopt,
                final, false);
            py::object confirmation = StoreFuture(write.confirmation);
            py::object admission = py::none();
            if (write.admitted.IsReady()) {
              absl::StatusOr<a11::Unit> result = write.admitted.Await();
              if (!result.ok()) {
                confirmation.attr("cancel")();
                ThrowStatus(result.status());
              }
              py::module_::import("asyncio").attr("get_running_loop")().attr(
                  "call_soon")(
                  py::cpp_function([self] { self->EnsureStarted(); }));
            } else {
              admission = StoreFuture(std::move(write.admitted));
            }
            return py::make_tuple(std::move(confirmation),
                                  std::move(admission));
          },
          "Enqueue a chunk and get back a (confirmation, admission) pair of "
          "awaitables. Unlike `put_chunk`, this exposes backpressure "
          "explicitly: `admission` resolves when the chunk is accepted into "
          "the bounded queue (None if it fit immediately) and `confirmation` "
          "resolves with the sequence assigned by the backing store. An agent "
          "awaits admission to pace production and confirmation to know the "
          "store accepted the write.",
          py::arg("chunk"), py::arg("seq") = py::none(),
          py::arg("final") = false)
      .def(
          "get_status",
          [](const stores::ChunkStoreWriter& self) -> py::object {
            std::optional<absl::Status> status = self.GetStatus();
            return status.has_value() ? StatusToPython(*status) : py::none();
          },
          "Return the writer's terminal status, or None while it is still "
          "open. An agent can poll this to detect that the stream has "
          "closed or failed.")
      .def(
          "get_abort_status",
          [](const stores::ChunkStoreWriter& self) -> py::object {
            std::optional<absl::Status> status = self.GetAbortStatus();
            return status.has_value() ? StatusToPython(*status) : py::none();
          },
          "Return the status the writer was aborted with, or None if it was "
          "not aborted. Use this to distinguish a clean close from an "
          "error-driven abort.")
      .def("is_writable", &stores::ChunkStoreWriter::IsWritable,
           "Return whether the writer still accepts chunks. False once the "
           "stream has been drained, closed, or aborted.")
      .def(
          "cancel",
          [](const std::shared_ptr<stores::ChunkStoreWriter>& self) {
            return StoreFuture(self->Cancel());
          },
          "Stop the writer immediately and await teardown, discarding any "
          "chunks still queued.")
      .def(
          "drain_and_close",
          [](const std::shared_ptr<stores::ChunkStoreWriter>& self) {
            return StoreFuture(self->DrainAndClose());
          },
          "Flush every queued chunk, close the writer, and await completion. "
          "This does not append a final fragment: mark the last chunk final "
          "before draining when readers need a final sequence number.")
      .def(
          "abort_with_status",
          [](const std::shared_ptr<stores::ChunkStoreWriter>& self,
             const py::handle& status) {
            return StoreFuture(self->AbortWithStatus(StatusFromPython(status)));
          },
          "Abort the writer with an error status and await teardown. Readers "
          "then observe the error rather than a clean end-of-stream.",
          py::arg("status"))
      .def(
          "wait_for_buffer_to_drain",
          [](const std::shared_ptr<stores::ChunkStoreWriter>& self) {
            return StoreFuture(self->WaitForBufferToDrain());
          },
          "Await until the in-flight write buffer empties. An agent can use "
          "this as a backpressure checkpoint before enqueuing more chunks.")
      .def(
          "attach_stream",
          [](stores::ChunkStoreWriter& self,
             std::shared_ptr<net::WireStream> stream) {
            const absl::Status status = self.AttachStream(std::move(stream));
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Tee stored fragments to an additional wire stream. After the store "
          "accepts a batch, the writer calls send on attached streams; a "
          "successful send confirms local transport admission, not peer "
          "delivery. A transport failure stops later writes but cannot revoke "
          "the current batch's store confirmations. The writer keeps the "
          "stream alive while attached.",
          py::arg("stream"), py::keep_alive<1, 2>())
      .def(
          "detach_stream",
          [](stores::ChunkStoreWriter& self,
             const std::shared_ptr<net::WireStream>& stream) {
            const absl::Status status = self.DetachStream(stream);
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Stop mirroring fragments to a previously attached wire stream. "
          "Raises if the stream was not attached.",
          py::arg("stream"))
      .def_property_readonly("store", &stores::ChunkStoreWriter::store,
                             "The ChunkStore this writer persists chunks to.")
      .def_property_readonly("options", &stores::ChunkStoreWriter::options,
                             "The ChunkStoreWriterOptions this writer was "
                             "created with.")
      .def_property_readonly("queue_size",
                             &stores::ChunkStoreWriter::queue_size,
                             "Number of chunks currently waiting in the "
                             "writer's flush queue.");
}

}  // namespace a11::python

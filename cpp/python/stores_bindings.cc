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

class PyChunkStore : public ChunkStore {
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
    if (loop_ == nullptr)
      return InvalidFuture<T>(loop_status_);
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
  if (value.is_none())
    return std::nullopt;
  return UnsignedOption(value, maximum, name);
}

void ValidateReaderOptions(const stores::ChunkStoreReaderOptions& options) {
  const absl::Status status = options.Validate();
  if (!status.ok())
    ThrowStatus(status);
}

void ValidateWriterOptions(const stores::ChunkStoreWriterOptions& options) {
  const absl::Status status = options.Validate();
  if (!status.ok())
    ThrowStatus(status);
}

}  // namespace

void BindStores(py::module_& module) {
  py::class_<stores::ChunkStoreReaderOptions>(module, "ChunkStoreReaderOptions")
      .def(py::init([](bool ordered, bool pop_chunks,
                       const py::handle& num_chunks_to_buffer,
                       const py::handle& offset,
                       const py::handle& max_chunks_to_read) {
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
                     max_chunks_to_read, kMaximum, "max_chunks_to_read")};
             ValidateReaderOptions(options);
             return options;
           }),
           "Construct validated options for a ChunkStoreReader.",
           py::arg("ordered") = true, py::arg("pop_chunks") = false,
           py::arg("num_chunks_to_buffer") = 32, py::arg("offset") = 0,
           py::arg("max_chunks_to_read") = py::none())
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
      .def("validate", &ValidateReaderOptions,
           "Raise if the options are not internally consistent.");

  py::class_<stores::ChunkStoreWriterOptions>(module, "ChunkStoreWriterOptions")
      .def(py::init([](const py::handle& offset,
                       const py::handle& max_chunks_to_write_at_once,
                       const py::handle& num_chunks_to_buffer) {
             constexpr std::uint64_t kMaximum = std::uint64_t{1} << 32U;
             stores::ChunkStoreWriterOptions options{
                 .offset = static_cast<std::uint32_t>(UnsignedOption(
                     offset, std::numeric_limits<std::uint32_t>::max(),
                     "offset")),
                 .max_chunks_to_write_at_once =
                     UnsignedOption(max_chunks_to_write_at_once, kMaximum,
                                    "max_chunks_to_write_at_once"),
                 .num_chunks_to_buffer = OptionalUnsignedOption(
                     num_chunks_to_buffer, kMaximum, "num_chunks_to_buffer")};
             ValidateWriterOptions(options);
             return options;
           }),
           "Construct validated options for a ChunkStoreWriter.",
           py::arg("offset") = 0, py::arg("max_chunks_to_write_at_once") = 8,
           py::arg("num_chunks_to_buffer") = py::none())
      .def_readwrite("offset", &stores::ChunkStoreWriterOptions::offset,
                     "Sequence number at which writing begins.")
      .def_readwrite(
          "max_chunks_to_write_at_once",
          &stores::ChunkStoreWriterOptions::max_chunks_to_write_at_once,
          "Maximum number of chunks flushed to the store per batch.")
      .def_readwrite("num_chunks_to_buffer",
                     &stores::ChunkStoreWriterOptions::num_chunks_to_buffer,
                     "Optional bound on the in-flight write buffer size.")
      .def("validate", &ValidateWriterOptions,
           "Raise if the options are not internally consistent.");

  py::class_<ChunkStore, PyChunkStore, std::shared_ptr<ChunkStore>> chunk_store(
      module, "ChunkStore");
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
          "Await the fragment stored at a sequence number. The returned "
          "future resolves once the fragment is available or the optional "
          "deadline elapses, so an agent can read a specific chunk without "
          "polling. Pass None for the deadline to wait indefinitely.",
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
          "rather than its sequence number. Use this when an agent needs to "
          "replay chunks in ingestion order; the future resolves when the "
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
          "Append a single fragment and await its assigned sequence number. "
          "Use this to feed an agent's output into the store; the returned "
          "future resolves once the write is durably accepted.",
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
          "keeping its slot, and await the resulting fragment. Use this to "
          "reclaim memory for chunks an agent has already consumed.",
          py::arg("seq"))
      .def(
          "get_seq_for_arrival_order",
          [](const std::shared_ptr<ChunkStore>& self,
             std::uint64_t arrival_order) {
            return StoreFuture(self->GetSeqForArrivalOrder(arrival_order));
          },
          "Await the sequence number that corresponds to a given arrival "
          "order. Use this to translate ingestion-order references into the "
          "sequence numbers the rest of the API expects.",
          py::arg("arrival_order"))
      .def("get_final_seq",
           [](const std::shared_ptr<ChunkStore>& self) {
             return StoreFuture(self->GetFinalSeq());
           },
           "Await the sequence number of the final chunk, or None if the "
           "store is still open. An agent can await this to learn when a "
           "stream has been fully closed and how many chunks it contains.")
      .def(
          "close_writes_with_status",
          [](const std::shared_ptr<ChunkStore>& self, const py::handle& status,
             bool return_existing) {
            return StoreFuture(self->CloseWritesWithStatus(
                StatusFromPython(status), return_existing));
          },
          "Seal the store against further writes with a terminal status and "
          "await completion. Call this when an agent has finished producing "
          "output; readers awaiting `next` are then released. When "
          "`return_status_if_already_closed` is set, a second close returns "
          "the status recorded by the first instead of overwriting it.",
          py::arg("status"), py::arg("return_status_if_already_closed") = false)
      .def("size",
           [](const std::shared_ptr<ChunkStore>& self) {
             return StoreFuture(self->Size());
           },
           "Await the number of fragments currently in the store. Useful for "
           "an agent to gauge backlog or progress without reading chunks.")
      .def("get_id",
           [](const ChunkStore& self) { return ValueOrThrow(self.GetId()); },
           "Return the store's node identifier. Raises if a Python subclass "
           "does not override `get_id`.");

  py::class_<stores::LocalChunkStore, ChunkStore,
             std::shared_ptr<stores::LocalChunkStore>>(module,
                                                       "LocalChunkStore")
      .def(py::init([](std::string id) {
             return ValueOrThrow(
                 stores::LocalChunkStore::Create(std::move(id)));
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

  py::class_<stores::ChunkStoreReader,
             std::shared_ptr<stores::ChunkStoreReader>>(
      module, "ChunkStoreReader", py::dynamic_attr())
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
      .def("get_status",
           [](const stores::ChunkStoreReader& self) {
             return StatusToPython(self.GetStatus());
           },
           "Return the reader's current status. An agent can inspect this to "
           "distinguish a healthy stream from one that has failed or ended.")
      .def("wait",
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

  py::class_<stores::ChunkStoreWriter,
             std::shared_ptr<stores::ChunkStoreWriter>>(
      module, "ChunkStoreWriter", py::dynamic_attr())
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
          "Write one chunk and await its durable sequence number. This is the "
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
          "resolves with its durable sequence number. An agent awaits "
          "admission to pace production and confirmation to know the write "
          "landed.",
          py::arg("chunk"), py::arg("seq") = py::none(),
          py::arg("final") = false)
      .def("get_status",
           [](const stores::ChunkStoreWriter& self) -> py::object {
             std::optional<absl::Status> status = self.GetStatus();
             return status.has_value() ? StatusToPython(*status) : py::none();
           },
           "Return the writer's terminal status, or None while it is still "
           "open. An agent can poll this to detect that the stream has "
           "closed or failed.")
      .def("get_abort_status",
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
      .def("cancel",
           [](const std::shared_ptr<stores::ChunkStoreWriter>& self) {
             return StoreFuture(self->Cancel());
           },
           "Stop the writer immediately and await teardown, discarding any "
           "chunks still queued. Use this to abandon a stream an agent no "
           "longer needs.")
      .def("drain_and_close",
           [](const std::shared_ptr<stores::ChunkStoreWriter>& self) {
             return StoreFuture(self->DrainAndClose());
           },
           "Flush every queued chunk, then close the stream, and await "
           "completion. This is the graceful shutdown path once an agent has "
           "finished producing output.")
      .def(
          "abort_with_status",
          [](const std::shared_ptr<stores::ChunkStoreWriter>& self,
             const py::handle& status) {
            return StoreFuture(self->AbortWithStatus(StatusFromPython(status)));
          },
          "Abort the writer with an error status and await teardown. Use this "
          "to propagate a failure downstream so readers observe the error "
          "instead of a clean end-of-stream.",
          py::arg("status"))
      .def("wait_for_buffer_to_drain",
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
            if (!status.ok())
              ThrowStatus(status);
          },
          "Mirror persisted fragments to an additional wire stream. Each "
          "confirmed chunk is copied to every attached stream, letting an "
          "agent fan its output out to remote peers. A transport failure "
          "stops later writes but never revokes confirmations already "
          "returned. The writer keeps the stream alive while attached.",
          py::arg("stream"), py::keep_alive<1, 2>())
      .def("detach_stream",
           [](stores::ChunkStoreWriter& self,
              const std::shared_ptr<net::WireStream>& stream) {
             const absl::Status status = self.DetachStream(stream);
             if (!status.ok())
               ThrowStatus(status);
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

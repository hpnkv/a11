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
           py::arg("ordered") = true, py::arg("pop_chunks") = false,
           py::arg("num_chunks_to_buffer") = 32, py::arg("offset") = 0,
           py::arg("max_chunks_to_read") = py::none())
      .def_readwrite("ordered", &stores::ChunkStoreReaderOptions::ordered)
      .def_readwrite("pop_chunks", &stores::ChunkStoreReaderOptions::pop_chunks)
      .def_readwrite("num_chunks_to_buffer",
                     &stores::ChunkStoreReaderOptions::num_chunks_to_buffer)
      .def_readwrite("offset", &stores::ChunkStoreReaderOptions::offset)
      .def_readwrite("max_chunks_to_read",
                     &stores::ChunkStoreReaderOptions::max_chunks_to_read)
      .def("validate", &ValidateReaderOptions);

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
           py::arg("offset") = 0, py::arg("max_chunks_to_write_at_once") = 8,
           py::arg("num_chunks_to_buffer") = py::none())
      .def_readwrite("offset", &stores::ChunkStoreWriterOptions::offset)
      .def_readwrite(
          "max_chunks_to_write_at_once",
          &stores::ChunkStoreWriterOptions::max_chunks_to_write_at_once)
      .def_readwrite("num_chunks_to_buffer",
                     &stores::ChunkStoreWriterOptions::num_chunks_to_buffer)
      .def("validate", &ValidateWriterOptions);

  py::class_<ChunkStore, PyChunkStore, std::shared_ptr<ChunkStore>> chunk_store(
      module, "ChunkStore");
  chunk_store.def(py::init<>())
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
          py::arg("deadline") = py::none(), py::arg("limit") = 1)
      .def("put",
           [](const std::shared_ptr<ChunkStore>& self,
              data::NodeFragment fragment) {
             return StoreFuture(self->Put(std::move(fragment)));
           })
      .def("put_many",
           [](const std::shared_ptr<ChunkStore>& self,
              std::vector<data::NodeFragment> fragments) {
             return StoreFuture(self->PutMany(std::move(fragments)));
           })
      .def("clear_data",
           [](const std::shared_ptr<ChunkStore>& self, std::uint32_t seq) {
             return StoreFuture(self->ClearData(seq));
           })
      .def("get_seq_for_arrival_order",
           [](const std::shared_ptr<ChunkStore>& self,
              std::uint64_t arrival_order) {
             return StoreFuture(self->GetSeqForArrivalOrder(arrival_order));
           })
      .def("get_final_seq",
           [](const std::shared_ptr<ChunkStore>& self) {
             return StoreFuture(self->GetFinalSeq());
           })
      .def(
          "close_writes_with_status",
          [](const std::shared_ptr<ChunkStore>& self, const py::handle& status,
             bool return_existing) {
            return StoreFuture(self->CloseWritesWithStatus(
                StatusFromPython(status), return_existing));
          },
          py::arg("status"), py::arg("return_status_if_already_closed") = false)
      .def("size",
           [](const std::shared_ptr<ChunkStore>& self) {
             return StoreFuture(self->Size());
           })
      .def("get_id",
           [](const ChunkStore& self) { return ValueOrThrow(self.GetId()); });

  py::class_<stores::LocalChunkStore, ChunkStore,
             std::shared_ptr<stores::LocalChunkStore>>(module,
                                                       "LocalChunkStore")
      .def(py::init([](std::string id) {
        return ValueOrThrow(stores::LocalChunkStore::Create(std::move(id)));
      }))
      .def_static(
          "create",
          [](std::string node_id) {
            return ValueOrThrow(
                stores::LocalChunkStore::Create(std::move(node_id)));
          },
          py::arg("node_id"));

  py::class_<stores::ChunkStoreReader,
             std::shared_ptr<stores::ChunkStoreReader>>(
      module, "ChunkStoreReader", py::dynamic_attr())
      .def(py::init([](std::shared_ptr<ChunkStore> store,
                       stores::ChunkStoreReaderOptions options) {
             return ValueOrThrow(
                 stores::ChunkStoreReader::Create(std::move(store), options));
           }),
           py::arg("store"),
           py::arg("options") = stores::ChunkStoreReaderOptions{},
           py::keep_alive<1, 2>())
      .def("ensure_started", &stores::ChunkStoreReader::EnsureStarted)
      .def("cancel", &stores::ChunkStoreReader::Cancel)
      .def("get_status",
           [](const stores::ChunkStoreReader& self) {
             return StatusToPython(self.GetStatus());
           })
      .def("wait",
           [](const stores::ChunkStoreReader& self) {
             return StoreFuture(self.Done());
           })
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
          py::arg("timeout") = py::none())
      .def_property_readonly("store", &stores::ChunkStoreReader::store)
      .def_property_readonly("options", &stores::ChunkStoreReader::options)
      .def_property_readonly("buffer_size",
                             &stores::ChunkStoreReader::buffer_size);

  py::class_<stores::ChunkStoreWriter,
             std::shared_ptr<stores::ChunkStoreWriter>>(
      module, "ChunkStoreWriter", py::dynamic_attr())
      .def(py::init([](std::shared_ptr<ChunkStore> store,
                       stores::ChunkStoreWriterOptions options) {
             return ValueOrThrow(
                 stores::ChunkStoreWriter::Create(std::move(store), options));
           }),
           py::arg("store"),
           py::arg("options") = stores::ChunkStoreWriterOptions{},
           py::keep_alive<1, 2>())
      .def("ensure_started", &stores::ChunkStoreWriter::EnsureStarted)
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
          py::arg("chunk"), py::arg("seq") = py::none(),
          py::arg("final") = false)
      .def("get_status",
           [](const stores::ChunkStoreWriter& self) -> py::object {
             std::optional<absl::Status> status = self.GetStatus();
             return status.has_value() ? StatusToPython(*status) : py::none();
           })
      .def("get_abort_status",
           [](const stores::ChunkStoreWriter& self) -> py::object {
             std::optional<absl::Status> status = self.GetAbortStatus();
             return status.has_value() ? StatusToPython(*status) : py::none();
           })
      .def("is_writable", &stores::ChunkStoreWriter::IsWritable)
      .def("cancel",
           [](const std::shared_ptr<stores::ChunkStoreWriter>& self) {
             return StoreFuture(self->Cancel());
           })
      .def("drain_and_close",
           [](const std::shared_ptr<stores::ChunkStoreWriter>& self) {
             return StoreFuture(self->DrainAndClose());
           })
      .def(
          "abort_with_status",
          [](const std::shared_ptr<stores::ChunkStoreWriter>& self,
             const py::handle& status) {
            return StoreFuture(self->AbortWithStatus(StatusFromPython(status)));
          })
      .def("wait_for_buffer_to_drain",
           [](const std::shared_ptr<stores::ChunkStoreWriter>& self) {
             return StoreFuture(self->WaitForBufferToDrain());
           })
      .def(
          "attach_stream",
          [](stores::ChunkStoreWriter& self,
             std::shared_ptr<net::WireStream> stream) {
            const absl::Status status = self.AttachStream(std::move(stream));
            if (!status.ok())
              ThrowStatus(status);
          },
          py::keep_alive<1, 2>())
      .def("detach_stream",
           [](stores::ChunkStoreWriter& self,
              const std::shared_ptr<net::WireStream>& stream) {
             const absl::Status status = self.DetachStream(stream);
             if (!status.ok())
               ThrowStatus(status);
           })
      .def_property_readonly("store", &stores::ChunkStoreWriter::store)
      .def_property_readonly("options", &stores::ChunkStoreWriter::options)
      .def_property_readonly("queue_size",
                             &stores::ChunkStoreWriter::queue_size);
}

}  // namespace a11::python

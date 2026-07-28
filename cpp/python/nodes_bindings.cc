// Copyright 2026 The A11 Authors.

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Python.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11_abseil/no_throw_status.h>
#include <pybind11_abseil/status_casters.h>

#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/chunk_store_reader.h"
#include "a11/stores/chunk_store_writer.h"
#include "python/bindings.h"
#include "python/casters.h"
#include "python/interop.h"

namespace a11::python {
namespace {

class PythonFactory {
 public:
  explicit PythonFactory(const py::object& function)
      : function_(function.inc_ref().ptr()) {}

  PythonFactory(const PythonFactory&) = delete;
  PythonFactory& operator=(const PythonFactory&) = delete;

  ~PythonFactory() {
    if (Py_IsInitialized() == 0)
      return;
    PyGILState_STATE state = PyGILState_Ensure();
    Py_CLEAR(function_);
    for (PyObject*& value : values_)
      Py_CLEAR(value);
    PyGILState_Release(state);
  }

  absl::StatusOr<std::shared_ptr<stores::ChunkStore>> Call(
      std::string node_id) {
    py::gil_scoped_acquire acquire;
    try {
      py::function function = py::reinterpret_borrow<py::function>(function_);
      py::object value = function(std::move(node_id));
      std::shared_ptr<stores::ChunkStore> store =
          value.cast<std::shared_ptr<stores::ChunkStore>>();
      // A shared_ptr keeps the C++ trampoline alive, but pybind11 override
      // lookup also needs the corresponding Python instance.  Retain factory
      // results for the NodeMap lifetime so a temporary Python store remains
      // virtual when native readers and writers call it later.
      values_.push_back(value.inc_ref().ptr());
      return store;
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    } catch (const std::exception& error) {
      return absl::InvalidArgumentError(error.what());
    } catch (...) {
      return absl::UnknownError(
          "Python ChunkStore factory raised an exception");
    }
  }

 private:
  PyObject* function_ = nullptr;
  std::vector<PyObject*> values_;
};

py::object StatusObject(const absl::Status& status) {
  return StatusToPython(status);
}

}  // namespace

void BindNodes(py::module_& module) {
  py::class_<data::SerializationRegistry,
             std::shared_ptr<data::SerializationRegistry>>(
      module, "SerializationRegistry")
      .def(py::init<bool>(), py::arg("register_defaults") = false)
      .def("register_defaults",
           [](data::SerializationRegistry& self) {
             const absl::Status status = self.RegisterDefaults();
             if (!status.ok())
               ThrowStatus(status);
           })
      .def_property_readonly("serializer_count",
                             &data::SerializationRegistry::serializer_count)
      .def_property_readonly("deserializer_count",
                             &data::SerializationRegistry::deserializer_count);

  py::class_<nodes::NodeMap, std::shared_ptr<nodes::NodeMap>>(
      module, "NodeMap", py::dynamic_attr())
      .def(py::init([](const py::object& factory) {
             if (factory.is_none()) {
               return ValueOrThrow(nodes::NodeMap::Create());
             }
             if (PyCallable_Check(factory.ptr()) == 0) {
               ThrowStatus(
                   absl::InvalidArgumentError("factory must be callable"));
             }
             auto owner = std::make_shared<PythonFactory>(factory);
             return ValueOrThrow(
                 nodes::NodeMap::Create([owner](std::string node_id) {
                   return owner->Call(std::move(node_id));
                 }));
           }),
           py::arg("chunk_store_factory") = py::none())
      .def("get",
           [](nodes::NodeMap& self, std::string node_id) {
             return ValueOrThrow(self.Get(std::move(node_id)));
           })
      .def("get_if_exists",
           [](const nodes::NodeMap& self, const std::string& node_id) {
             return ValueOrThrow(self.GetIfExists(node_id));
           })
      .def(
          "discard",
          [](nodes::NodeMap& self, const std::string& node_id,
             const std::shared_ptr<nodes::AsyncNode>& expected) {
            return ValueOrThrow(self.Discard(node_id, expected));
          },
          py::arg("node_id"), py::arg("expected") = nullptr)
      .def("contains", &nodes::NodeMap::Contains)
      .def("__contains__", &nodes::NodeMap::Contains)
      .def("size", &nodes::NodeMap::Size)
      .def("__len__", &nodes::NodeMap::Size);

  py::class_<nodes::AsyncNode, std::shared_ptr<nodes::AsyncNode>>(
      module, "AsyncNode", py::dynamic_attr())
      .def(py::init([](std::shared_ptr<stores::ChunkStore> store,
                       std::shared_ptr<data::SerializationRegistry> registry,
                       stores::ChunkStoreReaderOptions reader_options,
                       stores::ChunkStoreWriterOptions writer_options) {
             return ValueOrThrow(
                 nodes::AsyncNode::Create(std::move(store), std::move(registry),
                                          reader_options, writer_options));
           }),
           py::arg("chunk_store"), py::arg("serialization_registry") = nullptr,
           py::arg("reader_options") = stores::ChunkStoreReaderOptions{},
           py::arg("writer_options") = stores::ChunkStoreWriterOptions{},
           py::keep_alive<1, 2>())
      .def("get_id",
           [](const nodes::AsyncNode& self) {
             return ValueOrThrow(self.GetId());
           })
      .def_property_readonly("id",
                             [](const nodes::AsyncNode& self) {
                               return ValueOrThrow(self.GetId());
                             })
      .def("get_chunk_store", &nodes::AsyncNode::GetChunkStore)
      .def_property_readonly("chunk_store", &nodes::AsyncNode::GetChunkStore)
      .def_property("serialization_registry",
                    &nodes::AsyncNode::serialization_registry,
                    [](nodes::AsyncNode& self,
                       std::shared_ptr<data::SerializationRegistry> registry) {
                      const absl::Status status =
                          self.SetSerializationRegistry(std::move(registry));
                      if (!status.ok())
                        ThrowStatus(status);
                    })
      .def(
          "reader",
          [](nodes::AsyncNode& self) { return ValueOrThrow(self.reader()); },
          py::keep_alive<0, 1>())
      .def(
          "writer",
          [](nodes::AsyncNode& self) { return ValueOrThrow(self.writer()); },
          py::keep_alive<0, 1>())
      .def_property(
          "reader_options", &nodes::AsyncNode::GetReaderOptions,
          [](nodes::AsyncNode& self, stores::ChunkStoreReaderOptions options) {
            const absl::Status status = self.SetReaderOptions(options);
            if (!status.ok())
              ThrowStatus(status);
          })
      .def(
          "reset_reader",
          [](nodes::AsyncNode& self,
             std::optional<stores::ChunkStoreReaderOptions> options) {
            const absl::Status status = self.ResetReader(options);
            if (!status.ok())
              ThrowStatus(status);
          },
          py::arg("options") = std::nullopt)
      .def_property(
          "writer_options", &nodes::AsyncNode::GetWriterOptions,
          [](nodes::AsyncNode& self, stores::ChunkStoreWriterOptions options) {
            const absl::Status status = self.SetWriterOptions(options);
            if (!status.ok())
              ThrowStatus(status);
          })
      .def("get_reader_status",
           [](const nodes::AsyncNode& self) {
             return StatusObject(self.GetReaderStatus());
           })
      .def("get_writer_status",
           [](const nodes::AsyncNode& self) {
             return StatusObject(self.GetWriterStatus());
           })
      .def("get_writer_abort_status",
           [](const nodes::AsyncNode& self) -> py::object {
             std::optional<absl::Status> status = self.GetWriterAbortStatus();
             return status.has_value() ? StatusToPython(*status) : py::none();
           })
      .def("is_writable",
           [](const std::shared_ptr<nodes::AsyncNode>& self) {
             return FutureToPython(self->IsWritable());
           })
      .def(
          "put_chunk",
          [](const std::shared_ptr<nodes::AsyncNode>& self, data::Chunk chunk,
             std::optional<std::uint32_t> seq, bool final) {
            return FutureToPython(self->PutChunk(std::move(chunk), seq, final));
          },
          py::arg("chunk"), py::arg("seq") = std::nullopt,
          py::arg("final") = false)
      .def("put_fragment",
           [](const std::shared_ptr<nodes::AsyncNode>& self,
              data::NodeFragment fragment) {
             return FutureToPython(self->PutFragment(std::move(fragment)));
           })
      .def(
          "put_null_final",
          [](const std::shared_ptr<nodes::AsyncNode>& self,
             std::optional<std::uint32_t> seq) {
            return FutureToPython(self->PutNullFinal(seq));
          },
          py::arg("seq") = std::nullopt)
      .def(
          "next_fragment",
          [](const std::shared_ptr<nodes::AsyncNode>& self,
             const py::object& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::optional<data::NodeFragment>>(
                      converted.status()));
            }
            return FutureToPython(self->NextFragment(*converted));
          },
          py::arg("timeout") = py::none())
      .def(
          "next_chunk",
          [](const std::shared_ptr<nodes::AsyncNode>& self,
             const py::object& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::optional<data::Chunk>>(
                      converted.status()));
            }
            return FutureToPython(self->NextChunk(*converted));
          },
          py::arg("timeout") = py::none())
      .def("wait_for_buffer_to_drain",
           [](const std::shared_ptr<nodes::AsyncNode>& self) {
             return FutureToPython(self->WaitForBufferToDrain());
           })
      .def("drain_and_close",
           [](const std::shared_ptr<nodes::AsyncNode>& self) {
             return FutureToPython(self->DrainAndClose());
           })
      .def("abort_with_status",
           [](const std::shared_ptr<nodes::AsyncNode>& self,
              const py::handle& status) {
             return FutureToPython(
                 self->AbortWithStatus(StatusFromPython(status)));
           })
      .def(
          "attach_stream",
          [](nodes::AsyncNode& self, std::shared_ptr<net::WireStream> stream) {
            const absl::Status status = self.AttachStream(std::move(stream));
            if (!status.ok())
              ThrowStatus(status);
          },
          py::keep_alive<1, 2>())
      .def("detach_stream",
           [](nodes::AsyncNode& self,
              const std::shared_ptr<net::WireStream>& stream) {
             const absl::Status status = self.DetachStream(stream);
             if (!status.ok())
               ThrowStatus(status);
           })
      .def("cancel_reader", &nodes::AsyncNode::CancelReader)
      .def("cancel_writer", &nodes::AsyncNode::CancelWriter)
      .def("cancel", &nodes::AsyncNode::Cancel);
}

}  // namespace a11::python

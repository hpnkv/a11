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
#include <pybind11/typing.h>
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
    if (Py_IsInitialized() == 0) {
      return;
    }
    PyGILState_STATE state = PyGILState_Ensure();
    Py_CLEAR(function_);
    for (PyObject*& value : values_) {
      Py_CLEAR(value);
    }
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

NativeStatus StatusObject(const absl::Status& status) {
  return NativeStatus(status);
}

// A node map's readers take its mutex, which a fibre creating a node may be
// holding, so these wait without the GIL like everything else here. See
// [WithoutGil].
const auto Contains = [](const nodes::NodeMap& self,
                         const std::string& node_id) {
  return WithoutGil([&] { return self.Contains(node_id); });
};

const auto Ids = [](const nodes::NodeMap& self) {
  return WithoutGil([&] { return self.Ids(); });
};

const auto Size = [](const nodes::NodeMap& self) {
  return WithoutGil([&] { return self.Size(); });
};

}  // namespace

void BindNodes(py::module_& module) {
  py::classh<data::SerializationRegistry>(module, "SerializationRegistry")
      .def(py::init<bool>(),
           "Creates a serialization registry, optionally pre-populated with "
           "the built-in serializers and deserializers.",
           py::arg("register_defaults") = false)
      .def(
          "register_defaults",
          [](data::SerializationRegistry& self) {
            const absl::Status status = self.RegisterDefaults();
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Registers the built-in serializers and deserializers on this "
          "registry.")
      .def_property_readonly("serializer_count",
                             &data::SerializationRegistry::serializer_count,
                             "Number of registered serializers.")
      .def_property_readonly("deserializer_count",
                             &data::SerializationRegistry::deserializer_count,
                             "Number of registered deserializers.");

  py::classh<nodes::NodeMap>(module, "NodeMap", py::dynamic_attr())
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
           "Creates a node map, optionally backed by a chunk-store factory "
           "callable invoked to construct the backing store for each new node.",
           py::arg("chunk_store_factory") = py::none())
      .def(
          "get",
          [](nodes::NodeMap& self, std::string node_id) {
            return ValueOrThrow(WithoutGil(
                [&] { return self.Get(std::move(node_id)); }));
          },
          "Returns the node for the given id, creating it if it does not "
          "already exist.",
          py::arg("node_id"))
      .def(
          "get_if_exists",
          [](const nodes::NodeMap& self, const std::string& node_id) {
            return ValueOrThrow(
                WithoutGil([&] { return self.GetIfExists(node_id); }));
          },
          "Returns the node for the given id, or None if it does not exist.",
          py::arg("node_id"))
      .def(
          "discard",
          [](nodes::NodeMap& self, const std::string& node_id,
             const std::shared_ptr<nodes::AsyncNode>& expected) {
            return ValueOrThrow(
                WithoutGil([&] { return self.Discard(node_id, expected); }));
          },
          "Removes the node for the given id, optionally only if it matches "
          "the expected node.",
          py::arg("node_id"), py::arg("expected") = nullptr)
      .def("contains", Contains,
           "Returns whether a node with the given id exists.",
           py::arg("node_id"))
      .def("__contains__", Contains,
           "Returns whether a node with the given id exists.",
           py::arg("node_id"))
      .def("ids", Ids,
           "The id of every node the map holds, sorted.\n\nA snapshot: nodes "
           "are created on demand, so this is what was there when it was asked "
           "for.")
      .def("size", Size, "Number of nodes in the map.")
      .def("__len__", Size, "Number of nodes in the map.");

  py::classh<nodes::AsyncNode>(module, "AsyncNode", py::dynamic_attr())
      .def(py::init([](std::shared_ptr<stores::ChunkStore> store,
                       std::shared_ptr<data::SerializationRegistry> registry,
                       stores::ChunkStoreReaderOptions reader_options,
                       stores::ChunkStoreWriterOptions writer_options) {
             return ValueOrThrow(
                 nodes::AsyncNode::Create(std::move(store), std::move(registry),
                                          reader_options, writer_options));
           }),
           "Creates an asynchronous node over a chunk store. An AsyncNode is "
           "the core streaming primitive in A11: it wraps a chunk store with a "
           "reader and a writer so an agent can push chunks in and pull "
           "fragments out concurrently, without blocking. The optional "
           "serialization registry lets typed values be encoded to and decoded "
           "from chunks; reader and writer options tune buffering and flow "
           "control.",
           py::arg("chunk_store"), py::arg("serialization_registry") = nullptr,
           py::arg("reader_options") = stores::ChunkStoreReaderOptions{},
           py::arg("writer_options") = stores::ChunkStoreWriterOptions{},
           py::keep_alive<1, 2>())
      .def(
          "get_id",
          [](const nodes::AsyncNode& self) {
            return ValueOrThrow(self.GetId());
          },
          "Returns the node's stable identifier. Raises if the id cannot be "
          "resolved.")
      .def_property_readonly(
          "id",
          [](const nodes::AsyncNode& self) {
            return ValueOrThrow(self.GetId());
          },
          "The node's stable identifier (see get_id).")
      .def("get_chunk_store", &nodes::AsyncNode::GetChunkStore,
           "Returns the underlying chunk store backing this node. The chunk "
           "store is the ordered storage boundary that the node's reader and "
           "writer stream through; reach for it when you need lower-level "
           "access than the async put/next API provides.")
      .def_property_readonly("chunk_store", &nodes::AsyncNode::GetChunkStore,
                             "The underlying chunk store backing this node "
                             "(see get_chunk_store).")
      .def_property(
          "serialization_registry",
          [](const nodes::AsyncNode& self) {
            return WithoutGil([&] { return self.serialization_registry(); });
          },
          [](nodes::AsyncNode& self,
             std::shared_ptr<data::SerializationRegistry> registry) {
            const absl::Status status = WithoutGil([&] {
              return self.SetSerializationRegistry(std::move(registry));
            });
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "The serialization registry used to encode and decode "
          "typed values streamed through this node. Set it to change "
          "how put()/next-object conversions map values to chunks.")
      .def(
          "reader",
          [](nodes::AsyncNode& self) {
            return ValueOrThrow(WithoutGil([&] { return self.reader(); }));
          },
          "Returns the node's chunk-store reader, the consuming end of the "
          "stream. Read from it to pull chunks as they become available; it is "
          "kept alive for as long as you hold the returned reader.",
          py::keep_alive<0, 1>())
      .def(
          "writer",
          [](nodes::AsyncNode& self) {
            return ValueOrThrow(WithoutGil([&] { return self.writer(); }));
          },
          "Returns the node's chunk-store writer, the producing end of the "
          "stream. Write to it to append chunks that readers can consume "
          "concurrently; it is kept alive for as long as you hold the returned "
          "writer.",
          py::keep_alive<0, 1>())
      .def_property(
          "reader_options",
          [](const nodes::AsyncNode& self) {
            return WithoutGil([&] { return self.GetReaderOptions(); });
          },
          [](nodes::AsyncNode& self, stores::ChunkStoreReaderOptions options) {
            const absl::Status status =
                WithoutGil([&] { return self.SetReaderOptions(options); });
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Options controlling how this node reads from its chunk store, such "
          "as buffering and flow control.")
      .def(
          "reset_reader",
          [](nodes::AsyncNode& self,
             std::optional<stores::ChunkStoreReaderOptions> options) {
            const absl::Status status =
                WithoutGil([&] { return self.ResetReader(options); });
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Rewinds the node's reader back to the start of the stream, "
          "optionally applying new reader options.",
          py::arg("options") = std::nullopt)
      .def_property(
          "writer_options",
          [](const nodes::AsyncNode& self) {
            return WithoutGil([&] { return self.GetWriterOptions(); });
          },
          [](nodes::AsyncNode& self, stores::ChunkStoreWriterOptions options) {
            const absl::Status status =
                WithoutGil([&] { return self.SetWriterOptions(options); });
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Options controlling how this node writes to its chunk store, such "
          "as buffering and flow control.")
      .def(
          "get_reader_status",
          [](const nodes::AsyncNode& self) -> NativeStatus {
            return StatusObject(
                WithoutGil([&] { return self.GetReaderStatus(); }));
          },
          "Returns the current status of the node's reader. Check it to tell "
          "whether the consuming end of the stream is healthy, has completed, "
          "or has failed while streaming.")
      .def(
          "get_writer_status",
          [](const nodes::AsyncNode& self) -> NativeStatus {
            return StatusObject(
                WithoutGil([&] { return self.GetWriterStatus(); }));
          },
          "Returns the current status of the node's writer. Check it to tell "
          "whether the producing end of the stream is healthy, has completed, "
          "or has failed while streaming.")
      .def(
          "get_writer_abort_status",
          [](const nodes::AsyncNode& self)
              -> py::typing::Optional<NativeStatus> {
            const std::optional<absl::Status> status =
                WithoutGil([&] { return self.GetWriterAbortStatus(); });
            return status.has_value() ? StatusToPython(*status) : py::none();
          },
          "Returns the status the writer was aborted with, or None if the "
          "writer has not been aborted.")
      .def(
          "is_writable",
          [](const std::shared_ptr<nodes::AsyncNode>& self) {
            return FutureToPython(
                WithoutGil([&] { return self->IsWritable(); }));
          },
          "Returns a future that resolves once it is known whether the node "
          "can currently accept writes. Await it before producing chunks to "
          "respect backpressure rather than blocking a busy stream.")
      .def(
          "put_chunk",
          [](const std::shared_ptr<nodes::AsyncNode>& self, data::Chunk chunk,
             std::optional<std::uint32_t> seq, bool final) {
            return FutureToPython(WithoutGil(
                [&] { return self->PutChunk(std::move(chunk), seq, final); }));
          },
          "Appends a chunk to the stream and returns a future resolving to its "
          "sequence number. This is the primary way an agent produces "
          "streaming output: call it repeatedly as data becomes available, "
          "passing final=True on the last chunk to establish the logical "
          "final sequence. Finality does not close the writer; call "
          "drain_and_close() afterwards. An explicit seq can place the chunk "
          "at a specific position.",
          py::arg("chunk"), py::arg("seq") = std::nullopt,
          py::arg("final") = false)
      .def(
          "put_fragment",
          [](const std::shared_ptr<nodes::AsyncNode>& self,
             data::NodeFragment fragment) {
            return FutureToPython(WithoutGil(
                [&] { return self->PutFragment(std::move(fragment)); }));
          },
          "Appends a pre-assembled node fragment to the stream and returns a "
          "future resolving to its sequence number, for a NodeFragment "
          "already in hand (one forwarded from another node, say).",
          py::arg("fragment"))
      .def(
          "put_null_final",
          [](const std::shared_ptr<nodes::AsyncNode>& self,
             std::optional<std::uint32_t> seq) {
            return FutureToPython(
                WithoutGil([&] { return self->PutNullFinal(seq); }));
          },
          "Appends a final null marker and returns a future resolving to its "
          "sequence number. Use this to declare the logical end when there is "
          "no final payload, then call drain_and_close() to close writes.",
          py::arg("seq") = std::nullopt)
      .def(
          "next_fragment",
          [](const std::shared_ptr<nodes::AsyncNode>& self,
             const py::typing::Optional<NativeDuration>& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::optional<data::NodeFragment>>(
                      converted.status()));
            }
            return FutureToPython(
                WithoutGil([&] { return self->NextFragment(*converted); }));
          },
          "Returns a future resolving to the next fragment in the stream, or "
          "None at end-of-stream. This is the consuming counterpart to "
          "put_fragment: await it in a loop to process a node's output "
          "incrementally as it arrives. The optional timeout bounds how long "
          "the future waits for the next fragment.",
          py::arg("timeout") = py::none())
      .def(
          "next_fragments",
          [](const std::shared_ptr<nodes::AsyncNode>& self, size_t limit,
             const py::typing::Optional<NativeDuration>& timeout) {
            using Batch = std::vector<std::optional<data::NodeFragment>>;
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(a11::FailedFuture<Batch>(converted.status()));
            }
            return FutureToPython(WithoutGil(
                [&] { return self->NextFragments(limit, *converted); }));
          },
          "Returns a future resolving to a list of up to `limit` fragments, "
          "with a trailing None at end-of-stream. The batched counterpart to "
          "next_fragment, and the one to prefer when draining: every await "
          "costs an event-loop turn, so reading a hundred values one await at "
          "a time is a hundred turns. It returns whatever is already buffered "
          "and waits only when nothing is, so a live stream still yields each "
          "value as soon as it arrives.",
          py::arg("limit"), py::arg("timeout") = py::none())
      .def(
          "next_chunk",
          [](const std::shared_ptr<nodes::AsyncNode>& self,
             const py::typing::Optional<NativeDuration>& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::optional<data::Chunk>>(
                      converted.status()));
            }
            return FutureToPython(
                WithoutGil([&] { return self->NextChunk(*converted); }));
          },
          "Returns a future resolving to the next chunk in the stream, or None "
          "at end-of-stream. This is the consuming counterpart to put_chunk: "
          "await it in a loop to read a node's raw chunks incrementally as "
          "they arrive. The optional timeout bounds how long the future waits "
          "for the next chunk.",
          py::arg("timeout") = py::none())
      .def(
          "wait_for_buffer_to_drain",
          [](const std::shared_ptr<nodes::AsyncNode>& self) {
            return FutureToPython(
                WithoutGil([&] { return self->WaitForBufferToDrain(); }));
          },
          "Returns a future that resolves once the write buffer has drained. "
          "Await it to apply backpressure from a fast producer, letting "
          "consumers catch up before you push more chunks.")
      .def(
          "drain_and_close",
          [](const std::shared_ptr<nodes::AsyncNode>& self) {
            return FutureToPython(
                WithoutGil([&] { return self->DrainAndClose(); }));
          },
          "Returns a future that resolves once all buffered chunks have been "
          "flushed and the writer is closed. This does not mark a chunk as "
          "final: call put_final() or put_null_final() first when readers "
          "must synchronise on the logical end of the stream.")
      .def(
          "abort_with_status",
          [](const std::shared_ptr<nodes::AsyncNode>& self,
             const PyLike<NativeStatus>& status) {
            absl::Status aborted = StatusFromPython(status);
            return FutureToPython(WithoutGil(
                [&] { return self->AbortWithStatus(std::move(aborted)); }));
          },
          "Aborts the stream with the given error status and returns a future "
          "that resolves once the abort has propagated. Consumers then "
          "observe the error rather than a normal end-of-stream.",
          py::arg("status"))
      .def(
          "attach_stream",
          [](nodes::AsyncNode& self, std::shared_ptr<net::WireStream> stream) {
            const absl::Status status = WithoutGil(
                [&] { return self.AttachStream(std::move(stream)); });
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Attaches a wire stream so this node's chunks are mirrored over the "
          "network transport. The stream is kept alive for the node's "
          "lifetime.",
          py::arg("stream"), py::keep_alive<1, 2>())
      .def(
          "detach_stream",
          [](nodes::AsyncNode& self,
             const std::shared_ptr<net::WireStream>& stream) {
            const absl::Status status =
                WithoutGil([&] { return self.DetachStream(stream); });
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Detaches a previously attached wire stream so the node stops "
          "mirroring its chunks over that transport.",
          py::arg("stream"))
      .def("cancel_reader",
           [](nodes::AsyncNode& self) {
             WithoutGil([&] { self.CancelReader(); });
           },
           "Cancels the node's reader, unblocking any pending next-chunk or "
           "next-fragment awaits on the consuming side of the stream.")
      .def("cancel_writer",
           [](nodes::AsyncNode& self) {
             WithoutGil([&] { self.CancelWriter(); });
           },
           "Cancels the node's writer, unblocking any pending put or drain "
           "awaits on the producing side of the stream.")
      .def("cancel",
           [](nodes::AsyncNode& self) { WithoutGil([&] { self.Cancel(); }); },
           "Cancels both the reader and the writer, tearing down all pending "
           "streaming operations on the node at once.");
}

}  // namespace a11::python

// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Python.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/node_map.h"
#include "a11/service/session.h"
#include "a11/status.h"
#include "python/bindings.h"
#include "python/casters.h"
#include "python/interop.h"

namespace a11::python {
namespace {

class PythonSessionCallback {
 public:
  static absl::StatusOr<std::shared_ptr<PythonSessionCallback>> Create(
      const py::object& callable, const char* name) {
    if (PyCallable_Check(callable.ptr()) == 0) {
      return absl::InvalidArgumentError(std::string(name) +
                                        " must be callable");
    }
    absl::StatusOr<std::shared_ptr<PythonLoop>> loop = PythonLoop::Capture();
    if (!loop.ok())
      return loop.status();

    struct MakeSharedEnabler final : PythonSessionCallback {
      MakeSharedEnabler(PyObject* callable, std::shared_ptr<PythonLoop> loop)
          : PythonSessionCallback(callable, std::move(loop)) {}
    };

    return std::make_shared<MakeSharedEnabler>(callable.inc_ref().ptr(),
                                               std::move(*loop));
  }

  PythonSessionCallback(const PythonSessionCallback&) = delete;
  PythonSessionCallback& operator=(const PythonSessionCallback&) = delete;

  ~PythonSessionCallback() {
    if (Py_IsInitialized() == 0)
      return;
    const PyGILState_STATE state = PyGILState_Ensure();
    Py_CLEAR(callable_);
    PyGILState_Release(state);
  }

  template <typename... Args>
  a11::Task Call(Args&&... args) const {
    py::gil_scoped_acquire acquire;
    py::function callable = py::reinterpret_borrow<py::function>(callable_);
    return CallPythonAsync<a11::Unit>(loop_, callable,
                                      std::forward<Args>(args)...);
  }

 private:
  PythonSessionCallback(PyObject* callable, std::shared_ptr<PythonLoop> loop)
      : callable_(callable), loop_(std::move(loop)) {}

  PyObject* callable_ = nullptr;
  std::shared_ptr<PythonLoop> loop_;
};

struct SessionCallbacks {
  service::OnSessionStreamMessage on_message;
  service::OnSessionStreamDone on_done;
};

size_t SessionSizeOption(const py::handle& value, const char* name) {
  try {
    if (!py::isinstance<py::int_>(value) || py::isinstance<py::bool_>(value) ||
        py::cast<bool>(value.attr("__lt__")(0))) {
      ThrowStatus(absl::InvalidArgumentError(
          std::string(name) + " must be a non-negative integer"));
    }
    const std::uint64_t converted = value.cast<std::uint64_t>();
    if (converted > std::numeric_limits<size_t>::max()) {
      ThrowStatus(absl::OutOfRangeError(std::string(name) +
                                        " exceeds its supported range"));
    }
    return static_cast<size_t>(converted);
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  }
}

absl::StatusOr<SessionCallbacks> MakeSessionCallbacks(
    const py::object& on_message, const py::object& on_done) {
  SessionCallbacks result;
  if (!on_message.is_none()) {
    absl::StatusOr<std::shared_ptr<PythonSessionCallback>> callback =
        PythonSessionCallback::Create(on_message, "on_stream_message");
    if (!callback.ok())
      return callback.status();
    result.on_message = [callback = std::move(*callback)](
                            std::optional<data::WireMessage> message,
                            std::shared_ptr<net::WireStream> stream,
                            std::shared_ptr<service::Session> session) {
      return callback->Call(std::move(message), std::move(stream),
                            std::move(session));
    };
  }
  if (!on_done.is_none()) {
    absl::StatusOr<std::shared_ptr<PythonSessionCallback>> callback =
        PythonSessionCallback::Create(on_done, "on_stream_done");
    if (!callback.ok())
      return callback.status();
    result.on_done = [callback = std::move(*callback)](
                         std::shared_ptr<net::WireStream> stream,
                         std::shared_ptr<service::Session> session) {
      return callback->Call(std::move(stream), std::move(session));
    };
  }
  return result;
}

absl::StatusOr<service::StreamMode> StreamModeFromPython(
    const py::object& value) {
  if (py::isinstance<py::str>(value)) {
    const std::string mode = value.cast<std::string>();
    if (mode == "start")
      return service::StreamMode::kStart;
    if (mode == "accept")
      return service::StreamMode::kAccept;
    return absl::InvalidArgumentError(
        "mode must be either 'start' or 'accept'");
  }
  try {
    return value.cast<service::StreamMode>();
  } catch (const py::error_already_set&) {
    PyErr_Clear();
    return absl::InvalidArgumentError(
        "mode must be either 'start', 'accept', or StreamMode");
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  } catch (...) {
    return absl::InvalidArgumentError("invalid Session stream mode");
  }
}

service::SessionOptions MakeSessionOptions(
    const py::handle& max_buffered_messages_total,
    const py::handle& max_buffered_messages_per_stream,
    const py::handle& max_concurrent_root_actions,
    const py::handle& max_concurrent_nested_actions,
    const py::handle& max_single_message_size,
    const py::handle& max_buffered_bytes_total,
    const py::handle& max_buffered_bytes_per_stream,
    const py::object& no_stream_timeout, const py::object& deadline) {
  service::SessionOptions options;
  options.max_buffered_messages_total = SessionSizeOption(
      max_buffered_messages_total, "max_buffered_messages_total");
  options.max_buffered_messages_per_stream = SessionSizeOption(
      max_buffered_messages_per_stream, "max_buffered_messages_per_stream");
  options.max_concurrent_root_actions = SessionSizeOption(
      max_concurrent_root_actions, "max_concurrent_root_actions");
  options.max_concurrent_nested_actions = SessionSizeOption(
      max_concurrent_nested_actions, "max_concurrent_nested_actions");
  options.max_single_message_size =
      SessionSizeOption(max_single_message_size, "max_single_message_size");
  options.max_buffered_bytes_total =
      SessionSizeOption(max_buffered_bytes_total, "max_buffered_bytes_total");
  options.max_buffered_bytes_per_stream = SessionSizeOption(
      max_buffered_bytes_per_stream, "max_buffered_bytes_per_stream");
  if (!no_stream_timeout.is_none()) {
    options.no_stream_timeout =
        ValueOrThrow(DurationFromPython(no_stream_timeout, false));
  }
  if (!deadline.is_none()) {
    options.deadline = ValueOrThrow(TimeFromPython(deadline, false));
  }
  const absl::Status validation = options.Validate();
  if (!validation.ok())
    ThrowStatus(validation);
  return options;
}

void ValidateSessionOptions(const service::SessionOptions& options) {
  const absl::Status status = options.Validate();
  if (!status.ok())
    ThrowStatus(status);
}

absl::StatusOr<std::shared_ptr<service::Session>> CreateSession(
    std::string session_id, const py::object& on_message,
    const py::object& on_done, const py::object& headers,
    std::optional<service::SessionOptions> options,
    std::shared_ptr<nodes::NodeMap> node_map,
    std::shared_ptr<actions::ActionRegistry> registry) {
  py::object resolved_message = on_message;
  if (resolved_message.is_none()) {
    resolved_message =
        py::module_::import("a11._asyncio").attr("_dispatch_session_message");
  }
  absl::StatusOr<SessionCallbacks> callbacks =
      MakeSessionCallbacks(resolved_message, on_done);
  if (!callbacks.ok())
    return callbacks.status();
  absl::StatusOr<data::ByteMap> converted_headers = ByteMapFromPython(headers);
  if (!converted_headers.ok())
    return converted_headers.status();
  return service::Session::Create(
      std::move(session_id), std::move(callbacks->on_message),
      std::move(callbacks->on_done), std::move(*converted_headers),
      options.value_or(service::SessionOptions{}), std::move(node_map),
      std::move(registry));
}

absl::StatusOr<std::shared_ptr<service::SessionWithRecv>> CreateSessionWithRecv(
    std::string session_id, const py::object& headers,
    std::optional<service::SessionOptions> options,
    std::shared_ptr<nodes::NodeMap> node_map,
    std::shared_ptr<actions::ActionRegistry> registry) {
  absl::StatusOr<data::ByteMap> converted_headers = ByteMapFromPython(headers);
  if (!converted_headers.ok())
    return converted_headers.status();
  return service::SessionWithRecv::Create(
      std::move(session_id), std::move(*converted_headers),
      options.value_or(service::SessionOptions{}), std::move(node_map),
      std::move(registry));
}

void ThrowIfNotOk(const absl::Status& status) {
  if (!status.ok())
    ThrowStatus(status);
}

}  // namespace

void BindService(py::module_& module) {
  py::class_<service::SessionOptions>(module, "SessionOptions")
      .def(py::init(&MakeSessionOptions), py::kw_only(),
           py::arg("max_buffered_messages_total") = 256,
           py::arg("max_buffered_messages_per_stream") = 32,
           py::arg("max_concurrent_root_actions") = 32,
           py::arg("max_concurrent_nested_actions") = 128,
           py::arg("max_single_message_size") = service::kMaxSingleMessageSize,
           py::arg("max_buffered_bytes_total") = 32 * 1024 * 1024,
           py::arg("max_buffered_bytes_per_stream") = 4 * 1024 * 1024,
           py::arg("no_stream_timeout") = py::none(),
           py::arg("deadline") = py::none())
      .def_readwrite("max_buffered_messages_total",
                     &service::SessionOptions::max_buffered_messages_total)
      .def_readwrite("max_buffered_messages_per_stream",
                     &service::SessionOptions::max_buffered_messages_per_stream)
      .def_readwrite("max_concurrent_root_actions",
                     &service::SessionOptions::max_concurrent_root_actions)
      .def_readwrite("max_concurrent_nested_actions",
                     &service::SessionOptions::max_concurrent_nested_actions)
      .def_readwrite("max_single_message_size",
                     &service::SessionOptions::max_single_message_size)
      .def_readwrite("max_buffered_bytes_total",
                     &service::SessionOptions::max_buffered_bytes_total)
      .def_readwrite("max_buffered_bytes_per_stream",
                     &service::SessionOptions::max_buffered_bytes_per_stream)
      .def_property(
          "no_stream_timeout",
          [](const service::SessionOptions& options) {
            return DurationToPython(options.no_stream_timeout);
          },
          [](service::SessionOptions& options, const py::object& value) {
            options.no_stream_timeout =
                ValueOrThrow(DurationFromPython(value, false));
          })
      .def_property(
          "deadline",
          [](const service::SessionOptions& options) {
            return TimeToPython(options.deadline);
          },
          [](service::SessionOptions& options, const py::object& value) {
            options.deadline = ValueOrThrow(TimeFromPython(value));
          })
      .def("validate", &ValidateSessionOptions);

  py::enum_<service::StreamMode>(module, "StreamMode")
      .value("START", service::StreamMode::kStart)
      .value("ACCEPT", service::StreamMode::kAccept)
      .export_values();

  py::class_<service::Session, std::shared_ptr<service::Session>> session(
      module, "Session", py::dynamic_attr());
  session
      .def(py::init(
               [](std::string session_id, const py::object& on_stream_message,
                  const py::object& on_stream_done, const py::object& headers,
                  std::optional<service::SessionOptions> options,
                  std::shared_ptr<nodes::NodeMap> node_map,
                  std::shared_ptr<actions::ActionRegistry> action_registry) {
                 return ValueOrThrow(CreateSession(
                     std::move(session_id), on_stream_message, on_stream_done,
                     headers, options, std::move(node_map),
                     std::move(action_registry)));
               }),
           py::arg("session_id") = "",
           py::arg("on_stream_message") = py::none(),
           py::arg("on_stream_done") = py::none(),
           py::arg("headers") = py::none(), py::arg("options") = std::nullopt,
           py::arg("node_map") = nullptr, py::arg("action_registry") = nullptr)
      .def("streams",
           [](const service::Session& self) {
             return ValueOrThrow(self.Streams());
           })
      .def("get_stream",
           [](const service::Session& self, const std::string& stream_id) {
             return ValueOrThrow(self.GetStream(stream_id));
           })
      .def("get_id", &service::Session::GetId)
      .def_property_readonly("id", &service::Session::GetId)
      .def("get_node_map", &service::Session::GetNodeMap)
      .def_property(
          "node_map", &service::Session::GetNodeMap,
          [](service::Session& self, std::shared_ptr<nodes::NodeMap> node_map) {
            ThrowIfNotOk(self.SetNodeMap(std::move(node_map)));
          })
      .def(
          "set_node_map",
          [](service::Session& self, std::shared_ptr<nodes::NodeMap> node_map) {
            ThrowIfNotOk(self.SetNodeMap(std::move(node_map)));
          })
      .def("get_action_registry", &service::Session::GetActionRegistry)
      .def_property("action_registry", &service::Session::GetActionRegistry,
                    [](service::Session& self,
                       std::shared_ptr<actions::ActionRegistry> registry) {
                      ThrowIfNotOk(self.SetActionRegistry(std::move(registry)));
                    })
      .def("set_action_registry",
           [](service::Session& self,
              std::shared_ptr<actions::ActionRegistry> registry) {
             ThrowIfNotOk(self.SetActionRegistry(std::move(registry)));
           })
      .def("actions", &service::Session::Actions)
      .def("get_action",
           [](const service::Session& self, const std::string& action_id) {
             return ValueOrThrow(self.GetAction(action_id));
           })
      .def("cancel_action",
           [](service::Session& self, const std::string& action_id) {
             ThrowIfNotOk(self.CancelAction(action_id));
           })
      .def(
          "cancel_all_actions",
          [](service::Session& self) { ThrowIfNotOk(self.CancelAllActions()); })
      .def(
          "await_all_actions",
          [](const std::shared_ptr<service::Session>& self,
             const py::object& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(a11::FailedTask(converted.status()));
            }
            return FutureToPython(self->AwaitAllActions(*converted));
          },
          py::arg("timeout") = py::none())
      .def("dispatch_node_fragment",
           [](const std::shared_ptr<service::Session>& self,
              data::NodeFragment fragment) {
             return FutureToPython(
                 self->DispatchNodeFragment(std::move(fragment)));
           })
      .def(
          "dispatch_action_message",
          [](const std::shared_ptr<service::Session>& self,
             data::ActionMessage message,
             std::shared_ptr<net::WireStream> origin_stream) {
            return FutureToPython(self->DispatchActionMessage(
                std::move(message), std::move(origin_stream)));
          },
          py::arg("action_message"), py::arg("origin_stream") = nullptr)
      .def("dispatch_action",
           [](const std::shared_ptr<service::Session>& self,
              const py::object& action) {
             if (!py::isinstance<actions::Action>(action)) {
               return FutureToPython(a11::FailedTask(absl::InvalidArgumentError(
                   "action must be an Action instance")));
             }
             return FutureToPython(self->DispatchAction(
                 action.cast<std::shared_ptr<actions::Action>>()));
           })
      .def(
          "dispatch_wire_message",
          [](const std::shared_ptr<service::Session>& self,
             data::WireMessage message,
             std::shared_ptr<net::WireStream> origin_stream) {
            return FutureToPython(self->DispatchWireMessage(
                std::move(message), std::move(origin_stream)));
          },
          py::arg("message"), py::arg("origin_stream") = nullptr)
      .def("is_closed", &service::Session::IsClosed)
      .def("is_done", &service::Session::IsDone)
      .def_property_readonly("done",
                             [](const std::shared_ptr<service::Session>& self) {
                               return FutureToPython(self->Done());
                             })
      .def("wait_done",
           [](const std::shared_ptr<service::Session>& self) {
             return FutureToPython(self->Done());
           })
      .def("get_status",
           [](const service::Session& self) {
             return StatusToPython(self.GetStatus());
           })
      .def(
          "add_stream",
          [](const std::shared_ptr<service::Session>& self,
             std::shared_ptr<net::WireStream> stream, const py::object& mode) {
            service::StreamMode converted =
                ValueOrThrow(StreamModeFromPython(mode));
            a11::Task task =
                ValueOrThrow(self->AddStream(std::move(stream), converted));
            return FutureToPython(std::move(task));
          },
          py::arg("stream"), py::arg("mode") = "start", py::keep_alive<1, 2>())
      .def("half_close",
           [](service::Session& self) { ThrowIfNotOk(self.HalfClose()); })
      .def("abort",
           [](service::Session& self, const py::handle& status) {
             ThrowIfNotOk(self.Abort(StatusFromPython(status)));
           })
      .def(
          "send",
          [](service::Session& self, data::WireMessage message,
             const std::string& stream_id) {
            ThrowIfNotOk(self.Send(std::move(message), stream_id));
          },
          py::arg("message"), py::arg("stream_id") = "")
      .def_property_readonly("deadline",
                             [](const service::Session& self) {
                               return TimeToPython(self.deadline());
                             })
      .def(
          "set_deadline",
          [](service::Session& self, const py::object& deadline) {
            ThrowIfNotOk(
                self.SetDeadline(ValueOrThrow(TimeFromPython(deadline))));
          },
          py::arg("deadline") = py::none());

  py::class_<service::SessionWithRecv, service::Session,
             std::shared_ptr<service::SessionWithRecv>>(module,
                                                        "SessionWithRecv")
      .def(py::init(
               [](std::string session_id, const py::object& headers,
                  std::optional<service::SessionOptions> options,
                  std::shared_ptr<nodes::NodeMap> node_map,
                  std::shared_ptr<actions::ActionRegistry> action_registry) {
                 return ValueOrThrow(CreateSessionWithRecv(
                     std::move(session_id), headers, options,
                     std::move(node_map), std::move(action_registry)));
               }),
           py::arg("session_id") = "", py::arg("headers") = py::none(),
           py::arg("options") = std::nullopt, py::arg("node_map") = nullptr,
           py::arg("action_registry") = nullptr)
      .def(
          "receive_with_stream_id",
          [](const std::shared_ptr<service::SessionWithRecv>& self,
             const py::object& deadline) {
            absl::StatusOr<absl::Time> converted = TimeFromPython(deadline);
            if (!converted.ok()) {
              return FutureToPythonConverted(
                  a11::FailedFuture<
                      std::optional<service::ReceivedSessionMessage>>(
                      converted.status()),
                  [](const std::optional<service::ReceivedSessionMessage>&)
                      -> py::object { return py::none(); });
            }
            return FutureToPythonConverted(
                self->ReceiveWithStreamId(*converted),
                [](const std::optional<service::ReceivedSessionMessage>& value)
                    -> py::object {
                  if (!value.has_value())
                    return py::none();
                  return py::make_tuple(value->message, value->stream_id);
                });
          },
          py::arg("deadline") = py::none())
      .def(
          "receive",
          [](const std::shared_ptr<service::SessionWithRecv>& self,
             const py::object& deadline) {
            absl::StatusOr<absl::Time> converted = TimeFromPython(deadline);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::optional<data::WireMessage>>(
                      converted.status()));
            }
            return FutureToPython(self->Receive(*converted));
          },
          py::arg("deadline") = py::none());

  module.def(
      "normalize_session_headers",
      [](const py::object& headers) {
        data::ByteMap converted = ValueOrThrow(ByteMapFromPython(headers));
        return ByteMapToPython(ValueOrThrow(
            service::NormalizeSessionHeaders(std::move(converted))));
      },
      py::arg("headers") = py::none());
  module.attr("SESSION_STATUS_HEADER") =
      std::string(service::kSessionStatusHeader);
  module.attr("MAX_SINGLE_MESSAGE_SIZE") = service::kMaxSingleMessageSize;
  module.attr("SESSION_MAX_SINGLE_MESSAGE_SIZE") =
      service::kMaxSingleMessageSize;
}

}  // namespace a11::python

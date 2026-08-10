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
#include <pybind11/typing.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/node_map.h"
#include "a11/service/service.h"
#include "a11/service/session.h"
#include "a11/status.h"
#include "python/bindings.h"
#include "python/casters.h"
#include "python/interop.h"

namespace a11::python {
namespace {

// What receive_with_stream_id() resolves to: the message paired with the
// transport it arrived on, or None once the session has finished.
using ReceivedMessage =
    py::typing::Optional<py::typing::Tuple<data::WireMessage, py::str>>;

class PythonSessionCallback {
 public:
  static absl::StatusOr<std::shared_ptr<PythonSessionCallback>> Create(
      const py::object& callable, const char* name) {
    if (PyCallable_Check(callable.ptr()) == 0) {
      return absl::InvalidArgumentError(std::string(name) +
                                        " must be callable");
    }
    absl::StatusOr<std::shared_ptr<PythonLoop>> loop = PythonLoop::Capture();
    if (!loop.ok()) {
      return loop.status();
    }

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
    if (Py_IsInitialized() == 0) {
      return;
    }
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
    if (!callback.ok()) {
      return callback.status();
    }
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
    if (!callback.ok()) {
      return callback.status();
    }
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
    if (mode == "start") {
      return service::StreamMode::kStart;
    }
    if (mode == "accept") {
      return service::StreamMode::kAccept;
    }
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
    const py::typing::Optional<py::int_>& max_buffered_messages_total,
    const py::typing::Optional<py::int_>& max_buffered_messages_per_stream,
    const py::typing::Optional<py::int_>& max_concurrent_root_actions,
    const py::typing::Optional<py::int_>& max_concurrent_nested_actions,
    const py::typing::Optional<py::int_>& max_single_message_size,
    const py::typing::Optional<py::int_>& max_buffered_bytes_total,
    const py::typing::Optional<py::int_>& max_buffered_bytes_per_stream,
    const py::typing::Optional<NativeDuration>& no_stream_timeout,
    const py::typing::Optional<NativeTime>& deadline) {
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
  if (!validation.ok()) {
    ThrowStatus(validation);
  }
  return options;
}

void ValidateSessionOptions(const service::SessionOptions& options) {
  const absl::Status status = options.Validate();
  if (!status.ok()) {
    ThrowStatus(status);
  }
}

absl::StatusOr<std::shared_ptr<service::Session>> CreateSession(
    std::string session_id, const py::object& on_message,
    const py::object& on_done,
    const py::typing::Optional<PyMapping<py::str, py::bytes>>& headers,
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
  if (!callbacks.ok()) {
    return callbacks.status();
  }
  absl::StatusOr<data::ByteMap> converted_headers = ByteMapFromPython(headers);
  if (!converted_headers.ok()) {
    return converted_headers.status();
  }
  return service::Session::Create(
      std::move(session_id), std::move(callbacks->on_message),
      std::move(callbacks->on_done), std::move(*converted_headers),
      options.value_or(service::SessionOptions{}), std::move(node_map),
      std::move(registry));
}

absl::StatusOr<std::shared_ptr<service::SessionWithRecv>> CreateSessionWithRecv(
    std::string session_id,
    const py::typing::Optional<PyMapping<py::str, py::bytes>>& headers,
    std::optional<service::SessionOptions> options,
    std::shared_ptr<nodes::NodeMap> node_map,
    std::shared_ptr<actions::ActionRegistry> registry) {
  absl::StatusOr<data::ByteMap> converted_headers = ByteMapFromPython(headers);
  if (!converted_headers.ok()) {
    return converted_headers.status();
  }
  return service::SessionWithRecv::Create(
      std::move(session_id), std::move(*converted_headers),
      options.value_or(service::SessionOptions{}), std::move(node_map),
      std::move(registry));
}

void ThrowIfNotOk(const absl::Status& status) {
  if (!status.ok()) {
    ThrowStatus(status);
  }
}

// Release the GIL around a blocking native call, then report its status. The uv
// loop thread completes work by touching Python objects and must be able to take
// the GIL, so blocking while holding it deadlocks the loop.
template <typename Operation>
void CallWithoutGil(Operation&& operation) {
  absl::Status status;
  {
    py::gil_scoped_release release;
    status = std::forward<Operation>(operation)();
  }
  ThrowIfNotOk(status);
}

// As CallWithoutGil, for a blocking operation yielding an absl::StatusOr<T>.
// Convert any Python arguments *before* calling: the GIL is not held inside.
template <typename Operation>
auto ValueWithoutGil(Operation&& operation) {
  auto result = [&] {
    py::gil_scoped_release release;
    return std::forward<Operation>(operation)();
  }();
  return ValueOrThrow(std::move(result));
}


}  // namespace

void BindService(py::module_& module) {
  py::class_<service::SessionOptions>(module, "SessionOptions")
      .def(py::init(&MakeSessionOptions),
           "Construct session limits and timeouts; all parameters are "
           "keyword-only.",
           py::kw_only(), py::arg("max_buffered_messages_total") = 256,
           py::arg("max_buffered_messages_per_stream") = 32,
           py::arg("max_concurrent_root_actions") = 32,
           py::arg("max_concurrent_nested_actions") = 128,
           py::arg("max_single_message_size") = service::kMaxSingleMessageSize,
           py::arg("max_buffered_bytes_total") = 32 * 1024 * 1024,
           py::arg("max_buffered_bytes_per_stream") = 4 * 1024 * 1024,
           py::arg("no_stream_timeout") = py::none(),
           py::arg("deadline") = py::none())
      .def_readwrite("max_buffered_messages_total",
                     &service::SessionOptions::max_buffered_messages_total,
                     "Maximum number of messages buffered across all streams.")
      .def_readwrite("max_buffered_messages_per_stream",
                     &service::SessionOptions::max_buffered_messages_per_stream,
                     "Maximum number of messages buffered per stream.")
      .def_readwrite("max_concurrent_root_actions",
                     &service::SessionOptions::max_concurrent_root_actions,
                     "Maximum number of concurrently running root actions.")
      .def_readwrite("max_concurrent_nested_actions",
                     &service::SessionOptions::max_concurrent_nested_actions,
                     "Maximum number of concurrently running nested actions.")
      .def_readwrite("max_single_message_size",
                     &service::SessionOptions::max_single_message_size,
                     "Maximum size in bytes of a single wire message.")
      .def_readwrite("max_buffered_bytes_total",
                     &service::SessionOptions::max_buffered_bytes_total,
                     "Maximum total bytes buffered across all streams.")
      .def_readwrite("max_buffered_bytes_per_stream",
                     &service::SessionOptions::max_buffered_bytes_per_stream,
                     "Maximum bytes buffered per stream.")
      .def_property(
          "no_stream_timeout",
          [](const service::SessionOptions& options) -> NativeDuration {
            return NativeDuration(options.no_stream_timeout);
          },
          [](service::SessionOptions& options, const py::object& value) {
            options.no_stream_timeout =
                ValueOrThrow(DurationFromPython(value, false));
          },
          "How long the session waits with no active stream before finishing.")
      .def_property(
          "deadline",
          [](const service::SessionOptions& options) -> NativeTime {
            return NativeTime(options.deadline);
          },
          [](service::SessionOptions& options, const py::object& value) {
            options.deadline = ValueOrThrow(TimeFromPython(value));
          },
          "Absolute time after which the session is aborted.")
      .def("validate", &ValidateSessionOptions,
           "Validate the option values, raising on invalid configuration.");

  py::enum_<service::StreamMode>(module, "StreamMode")
      .value("START", service::StreamMode::kStart)
      .value("ACCEPT", service::StreamMode::kAccept)
      .export_values();

  py::classh<service::Session> session(module, "Session", py::dynamic_attr());
  session
      .def(py::init(
               [](std::string session_id, const py::object& on_stream_message,
                  const py::object& on_stream_done,
                  const py::typing::Optional<PyMapping<py::str, py::bytes>>&
                      headers,
                  std::optional<service::SessionOptions> options,
                  std::shared_ptr<nodes::NodeMap> node_map,
                  std::shared_ptr<actions::ActionRegistry> action_registry) {
                 return ValueOrThrow(CreateSession(
                     std::move(session_id), on_stream_message, on_stream_done,
                     headers, options, std::move(node_map),
                     std::move(action_registry)));
               }),
           "Create an A11 session that multiplexes wire streams and actions. "
           "Streams deliver messages asynchronously to the optional "
           "on_stream_message and on_stream_done callbacks, which may be "
           "coroutines. This is the top-level object an agent drives to "
           "exchange wire messages and run actions.",
           py::arg("session_id") = "",
           py::arg("on_stream_message") = py::none(),
           py::arg("on_stream_done") = py::none(),
           py::arg("headers") = py::none(), py::arg("options") = std::nullopt,
           py::arg("node_map") = nullptr, py::arg("action_registry") = nullptr)
      .def(
          "streams",
          [](const service::Session& self) {
            return ValueOrThrow(self.Streams());
          },
          "Return the (stream_id, stream) pairs currently attached to the "
          "session. Streams are added and removed asynchronously as peers "
          "connect and disconnect, so treat the result as a snapshot taken at "
          "call time.")
      .def(
          "get_stream",
          [](const service::Session& self, const std::string& stream_id) {
            return ValueOrThrow(self.GetStream(stream_id));
          },
          "Look up an attached stream by its id, raising if no such stream "
          "exists. Because streams come and go over the session's lifetime, "
          "guard against a stream having been removed since you last observed "
          "it.",
          py::arg("stream_id"))
      .def("get_id", &service::Session::GetId,
           "Return the session's unique identifier string.")
      .def_property_readonly("id", &service::Session::GetId,
                             "The session's unique identifier string.")
      .def("get_node_map", &service::Session::GetNodeMap,
           "Return the NodeMap backing this session's node state. Node "
           "fragments dispatched to the session are applied to this map as "
           "messages stream in.")
      .def_property(
          "node_map", &service::Session::GetNodeMap,
          [](service::Session& self, std::shared_ptr<nodes::NodeMap> node_map) {
            ThrowIfNotOk(self.SetNodeMap(std::move(node_map)));
          },
          "The NodeMap backing this session's node state; assigning replaces "
          "it.")
      .def(
          "set_node_map",
          [](service::Session& self, std::shared_ptr<nodes::NodeMap> node_map) {
            ThrowIfNotOk(self.SetNodeMap(std::move(node_map)));
          },
          "Replace the NodeMap backing this session's node state, raising on "
          "failure. Active actions are rebound, but existing fragments are "
          "not migrated; set it before traffic to avoid splitting state "
          "between maps.",
          py::arg("node_map"))
      .def("get_action_registry", &service::Session::GetActionRegistry,
           "Return the ActionRegistry used to resolve incoming action messages "
           "into runnable actions.")
      .def_property(
          "action_registry", &service::Session::GetActionRegistry,
          [](service::Session& self,
             std::shared_ptr<actions::ActionRegistry> registry) {
            ThrowIfNotOk(self.SetActionRegistry(std::move(registry)));
          },
          "The ActionRegistry used to resolve action messages; "
          "assigning replaces it.")
      .def(
          "set_action_registry",
          [](service::Session& self,
             std::shared_ptr<actions::ActionRegistry> registry) {
            ThrowIfNotOk(self.SetActionRegistry(std::move(registry)));
          },
          "Replace the ActionRegistry used to resolve incoming action "
          "messages, raising on failure. Active actions are rebound for "
          "later nested-name resolution; configure it before dispatch to "
          "avoid mixing registry versions.",
          py::arg("registry"))
      .def(
          "actions", &service::Session::Actions,
          "Return the (action_id, action) pairs currently running in the "
          "session. Actions execute asynchronously, so this is a point-in-time "
          "snapshot of in-flight work.")
      .def(
          "get_action",
          [](const service::Session& self, const std::string& action_id) {
            return ValueOrThrow(self.GetAction(action_id));
          },
          "Look up a running action by its id, raising if none matches.",
          py::arg("action_id"))
      .def(
          "cancel_action",
          [](service::Session& self, const std::string& action_id) {
            ThrowIfNotOk(self.CancelAction(action_id));
          },
          "Request cancellation of the running action with the given id, "
          "raising if it is unknown. Cancellation is cooperative and completes "
          "asynchronously as the action unwinds.",
          py::arg("action_id"))
      .def(
          "cancel_all_actions",
          [](service::Session& self) { ThrowIfNotOk(self.CancelAllActions()); },
          "Request cancellation of every action currently running in the "
          "session. Each action unwinds asynchronously; await "
          "await_all_actions "
          "to observe completion.")
      .def(
          "await_all_actions",
          [](const std::shared_ptr<service::Session>& self,
             const py::typing::Optional<NativeDuration>& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(a11::FailedTask(converted.status()));
            }
            return FutureToPython(self->AwaitAllActions(*converted));
          },
          "Return an awaitable that resolves once all in-flight actions have "
          "finished, or the optional timeout elapses. Await this to "
          "synchronize "
          "on the session's outstanding asynchronous work before proceeding.",
          py::arg("timeout") = py::none())
      .def(
          "dispatch_node_fragment",
          [](const std::shared_ptr<service::Session>& self,
             data::NodeFragment fragment) {
            return FutureToPython(
                self->DispatchNodeFragment(std::move(fragment)));
          },
          "Dispatch a node fragment into the session's NodeMap and return an "
          "awaitable resolving to the applied revision. Fragments are applied "
          "asynchronously in order, letting an agent stream incremental "
          "document updates.",
          py::arg("fragment"))
      .def(
          "dispatch_action_message",
          [](const std::shared_ptr<service::Session>& self,
             data::ActionMessage message,
             std::shared_ptr<net::WireStream> origin_stream) {
            return FutureToPython(self->DispatchActionMessage(
                std::move(message), std::move(origin_stream)));
          },
          "Dispatch an action message, resolving it against the action "
          "registry "
          "and running the resulting action. Returns an awaitable that "
          "completes when the action has been handled; origin_stream "
          "attributes "
          "the message to a source stream.",
          py::arg("action_message"), py::arg("origin_stream") = nullptr)
      .def(
          "dispatch_action",
          [](const std::shared_ptr<service::Session>& self,
             const py::object& action) {
            if (!py::isinstance<actions::Action>(action)) {
              return FutureToPython(a11::FailedTask(absl::InvalidArgumentError(
                  "action must be an Action instance")));
            }
            return FutureToPython(self->DispatchAction(
                action.cast<std::shared_ptr<actions::Action>>()));
          },
          "Dispatch an already-constructed Action to run within the session, "
          "returning an awaitable for its handling.",
          py::arg("action"))
      .def(
          "dispatch_wire_message",
          [](const std::shared_ptr<service::Session>& self,
             data::WireMessage message,
             std::shared_ptr<net::WireStream> origin_stream) {
            return FutureToPython(self->DispatchWireMessage(
                std::move(message), std::move(origin_stream)));
          },
          "Route a wire message through the session as though it arrived on a "
          "stream, returning an awaitable for its processing. origin_stream "
          "optionally records which stream the message is attributed to.",
          py::arg("message"), py::arg("origin_stream") = nullptr)
      .def("is_closed", &service::Session::IsClosed,
           "Return whether the session has been closed and no longer accepts "
           "new streams or messages.")
      .def("is_done", &service::Session::IsDone,
           "Return whether the session has fully finished, including all "
           "streams and actions. Prefer awaiting done for asynchronous "
           "completion rather than polling this flag.")
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<service::Session>& self) {
            return FutureToPython(self->Done());
          },
          "An awaitable that resolves when the session has "
          "fully finished all streams and actions.")
      .def(
          "wait_done",
          [](const std::shared_ptr<service::Session>& self) {
            return FutureToPython(self->Done());
          },
          "Return an awaitable that resolves when the session has fully "
          "finished. Await this to block until every stream and action has "
          "completed asynchronously.")
      .def(
          "get_status",
          [](const service::Session& self) -> NativeStatus {
            return NativeStatus(self.GetStatus());
          },
          "Return the session's terminal status, indicating whether it "
          "completed successfully or was aborted.")
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
          R"doc(Attach a wire stream and begin pumping its messages, returning an awaitable for the stream's lifetime. `mode` selects whether this side starts (`"start"`) or accepts (`"accept"`) the stream.

Examples:
    Attach the client transport before exchanging messages:

    ```python
    stream_lifetime = session.add_stream(websocket_stream)
    ```
)doc",
          py::arg("stream"), py::arg("mode") = "start", py::keep_alive<1, 2>())
      .def(
          "half_close",
          [](service::Session& self) { ThrowIfNotOk(self.HalfClose()); },
          R"doc(Signal that this side will send no more messages, allowing the session to drain and finish once peers do the same. Remaining inbound messages continue to be processed asynchronously.

Examples:
    Finish an exchange after sending the last message:

    ```python
    session.half_close()
    await session.done.wait()
    ```
)doc")
      .def(
          "abort",
          [](service::Session& self, const PyLike<NativeStatus>& status) {
            ThrowIfNotOk(self.Abort(StatusFromPython(status)));
          },
          R"doc(Abort the session immediately with the given error status, cancelling streams and actions.

Examples:
    Propagate an authentication failure to the peer:

    ```python
    session.abort(Status(
        code=StatusCode.PERMISSION_DENIED,
        message=str(error),
    ))
    ```
)doc",
          py::arg("status"))
      .def(
          "send",
          [](service::Session& self, data::WireMessage message,
             const std::string& stream_id) {
            ThrowIfNotOk(self.Send(std::move(message), stream_id));
          },
          R"doc(Enqueue a wire message for delivery on the named stream (or the default stream), raising on failure. Delivery happens asynchronously as the stream drains.

Examples:
    Route a response through a particular attached transport:

    ```python
    session.send(response, stream_id=websocket_stream.get_id())
    ```
)doc",
          py::arg("message"), py::arg("stream_id") = "")
      .def_property_readonly(
          "deadline",
          [](const service::Session& self) -> NativeTime {
            return NativeTime(self.deadline());
          },
          "The absolute time after which the session will be "
          "aborted.")
      .def(
          "set_deadline",
          [](service::Session& self,
             const py::typing::Optional<NativeTime>& deadline) {
            ThrowIfNotOk(
                self.SetDeadline(ValueOrThrow(TimeFromPython(deadline))));
          },
          "Set the absolute deadline after which the session is aborted; "
          "passing None clears it to no deadline. The session enforces this "
          "asynchronously as time passes.",
          py::arg("deadline") = py::none());

  py::classh<service::SessionWithRecv, service::Session>(module,
                                                         "SessionWithRecv")
      .def(py::init(
               [](std::string session_id,
                  const py::typing::Optional<PyMapping<py::str, py::bytes>>&
                      headers,
                  std::optional<service::SessionOptions> options,
                  std::shared_ptr<nodes::NodeMap> node_map,
                  std::shared_ptr<actions::ActionRegistry> action_registry) {
                 return ValueOrThrow(CreateSessionWithRecv(
                     std::move(session_id), headers, options,
                     std::move(node_map), std::move(action_registry)));
               }),
           "Create a session that buffers inbound messages for explicit "
           "pull-based reception instead of callbacks. receive and "
           "receive_with_stream_id await messages as they stream in.",
           py::arg("session_id") = "", py::arg("headers") = py::none(),
           py::arg("options") = std::nullopt, py::arg("node_map") = nullptr,
           py::arg("action_registry") = nullptr)
      .def(
          "receive_with_stream_id",
          [](const std::shared_ptr<service::SessionWithRecv>& self,
             const py::typing::Optional<NativeTime>& deadline) {
            absl::StatusOr<absl::Time> converted = TimeFromPython(deadline);
            if (!converted.ok()) {
              return FutureToPythonAs<ReceivedMessage>(
                  a11::FailedFuture<
                      std::optional<service::ReceivedSessionMessage>>(
                      converted.status()),
                  [](const std::optional<service::ReceivedSessionMessage>&)
                      -> py::object { return py::none(); });
            }
            return FutureToPythonAs<ReceivedMessage>(
                self->ReceiveWithStreamId(*converted),
                [](const std::optional<service::ReceivedSessionMessage>& value)
                    -> py::object {
                  if (!value.has_value()) {
                    return py::none();
                  }
                  return py::make_tuple(value->message, value->stream_id);
                });
          },
          "Return an awaitable that resolves to the next (message, stream_id) "
          "tuple, or None once the session is done. Await this to pull "
          "messages "
          "one at a time along with the stream they arrived on, honoring the "
          "optional deadline.",
          py::arg("deadline") = py::none())
      .def(
          "receive",
          [](const std::shared_ptr<service::SessionWithRecv>& self,
             const py::typing::Optional<NativeTime>& deadline) {
            absl::StatusOr<absl::Time> converted = TimeFromPython(deadline);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::optional<data::WireMessage>>(
                      converted.status()));
            }
            return FutureToPython(self->Receive(*converted));
          },
          "Return an awaitable that resolves to the next inbound message, or "
          "None once the session is done. Await this in a loop to consume the "
          "session's message stream without registering callbacks.",
          py::arg("deadline") = py::none());

  // --- Service: what a peer can call, decoupled from where it listens ------

  py::class_<service::ServiceOptions>(module, "ServiceOptions")
      .def(py::init([](std::optional<service::SessionOptions> session_options,
                       bool copy_registry_per_connection,
                       const py::typing::Optional<
                           PyMapping<py::str, py::bytes>>& session_headers,
                       const py::typing::Optional<NativeDuration>&
                           drain_timeout) {
             service::ServiceOptions options;
             if (session_options.has_value()) {
               options.session_options = *session_options;
             }
             options.copy_registry_per_connection =
                 copy_registry_per_connection;
             options.session_headers =
                 ValueOrThrow(ByteMapFromPython(session_headers));
             if (!drain_timeout.is_none()) {
               options.drain_timeout =
                   ValueOrThrow(DurationFromPython(drain_timeout, false));
             }
             return options;
           }),
           "Construct service options; all parameters are keyword-only.",
           py::kw_only(), py::arg("session_options") = std::nullopt,
           py::arg("copy_registry_per_connection") = false,
           py::arg("session_headers") = py::none(),
           py::arg("drain_timeout") = py::none())
      .def_readwrite("session_options",
                     &service::ServiceOptions::session_options,
                     "Limits and timeouts for every session created.")
      .def_readwrite("copy_registry_per_connection",
                     &service::ServiceOptions::copy_registry_per_connection,
                     "Give each connection its own copy of the registry. Leave "
                     "false when the connection hook makes the copy itself.")
      .def_property(
          "session_headers",
          [](const service::ServiceOptions& options) {
            return ByteMapToPython(options.session_headers);
          },
          [](service::ServiceOptions& options,
             const py::typing::Optional<PyMapping<py::str, py::bytes>>&
                 headers) {
            options.session_headers =
                ValueOrThrow(ByteMapFromPython(headers));
          },
          "Headers stamped on every session the service creates.")
      .def_property(
          "drain_timeout",
          [](const service::ServiceOptions& options) -> NativeDuration {
            return NativeDuration(options.drain_timeout);
          },
          [](service::ServiceOptions& options,
             const py::typing::Optional<NativeDuration>& value) {
            options.drain_timeout =
                ValueOrThrow(DurationFromPython(value, false));
          },
          "How long draining waits for live sessions.")
      .def(
          "validate",
          [](const service::ServiceOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the options, raising on error.");

  py::classh<service::Service>(module, "Service", py::dynamic_attr())
      .def(py::init([](std::shared_ptr<actions::ActionRegistry> action_registry,
                       const py::object& on_connection,
                       std::optional<service::ServiceOptions> options) {
             service::OnServiceConnection hook;
             if (!on_connection.is_none()) {
               // The same mechanism `on_stream_message` uses: capture the
               // asyncio loop now, and hand the call back to it from whichever
               // fiber accepts the connection. So a Python hook -- the gateway's
               // registry copy and tool-bridge bind -- stays plain Python.
               std::shared_ptr<PythonSessionCallback> callback = ValueOrThrow(
                   PythonSessionCallback::Create(on_connection,
                                                 "on_connection"));
               hook = [callback](std::shared_ptr<service::Session> session,
                                 std::shared_ptr<net::WireStream> stream)
                   -> a11::Task {
                 return callback->Call(std::move(session), std::move(stream));
               };
             }
             return ValueOrThrow(service::Service::Create(
                 std::move(action_registry), std::move(hook),
                 options.value_or(service::ServiceOptions{})));
           }),
           R"doc(A service: an action registry plus the sessions serving it.

`accept` is shaped to be a transport's on-stream callback, so one service can be
bound to several listeners, or to none at all (hand it an in-process stream). The
optional `on_connection(session, stream)` coroutine runs once per connection,
after the session exists and before it starts pumping -- the only window in which
a connection can be specialised without racing its first message.

Examples:
    Serve a gateway over WebSocket:

    ```python
    service = a11.Service(action_registry=registry, on_connection=prepare)
    server = a11.net.WebSocketWireServer.create(service.accept, options)
    ```
)doc",
           py::kw_only(), py::arg("action_registry") = nullptr,
           py::arg("on_connection") = py::none(),
           py::arg("options") = std::nullopt)
      .def(
          "accept",
          [](const std::shared_ptr<service::Service>& self,
             std::shared_ptr<net::WireStream> stream) {
            return FutureToPython(
                self->Serve(std::move(stream), service::StreamMode::kAccept));
          },
          "Serve an accepted stream, awaiting its session's whole lifetime.",
          py::arg("stream"))
      .def(
          "start",
          [](const std::shared_ptr<service::Service>& self,
             std::shared_ptr<net::WireStream> stream) {
            return FutureToPython(
                self->Serve(std::move(stream), service::StreamMode::kStart));
          },
          "Serve a stream this side initiated, awaiting its whole lifetime.",
          py::arg("stream"))
      .def(
          "serve",
          [](const std::shared_ptr<service::Service>& self,
             std::shared_ptr<net::WireStream> stream, const py::object& mode) {
            return FutureToPython(self->Serve(
                std::move(stream), ValueOrThrow(StreamModeFromPython(mode))));
          },
          "Serve a stream in the given mode (\"start\" or \"accept\").",
          py::arg("stream"), py::arg("mode") = "accept")
      .def(
          "start_stream_handler",
          [](const std::shared_ptr<service::Service>& self,
             std::shared_ptr<net::WireStream> stream, const py::object& mode) {
            // Blocking: it awaits the connection hook and the stream handshake
            // on a fiber. Without releasing the GIL the libuv loop cannot take
            // it to complete either, and this deadlocks.
            return ValueWithoutGil([&self, &stream, &mode] {
              const service::StreamMode converted =
                  ValueOrThrow(StreamModeFromPython(mode));
              return self->StartStreamHandler(std::move(stream), converted);
            });
          },
          "Begin serving a stream and return its session immediately.",
          py::arg("stream"), py::arg("mode") = "accept")
      .def(
          "add_stream_to_session",
          [](const std::shared_ptr<service::Service>& self,
             std::string session_id, std::shared_ptr<net::WireStream> stream,
             const py::object& mode) {
            CallWithoutGil([&self, &session_id, &stream, &mode] {
              const service::StreamMode converted =
                  ValueOrThrow(StreamModeFromPython(mode));
              return self->AddStreamToSession(session_id, std::move(stream),
                                              converted);
            });
          },
          "Attach another transport to an existing session.",
          py::arg("session_id"), py::arg("stream"),
          py::arg("mode") = "accept")
      .def("session_ids", &service::Service::SessionIds,
           "The ids of the sessions currently being served.")
      .def(
          "get_session",
          [](const std::shared_ptr<service::Service>& self,
             std::string_view session_id) {
            return ValueOrThrow(self->GetSession(session_id));
          },
          "The session with this id, raising NOT_FOUND if there is none.",
          py::arg("session_id"))
      .def(
          "get_session_for_stream",
          [](const std::shared_ptr<service::Service>& self,
             std::string_view stream_id) {
            return ValueOrThrow(self->GetSessionForStream(stream_id));
          },
          "The session serving this stream.", py::arg("stream_id"))
      .def_property_readonly("session_count",
                             &service::Service::SessionCount,
                             "How many sessions are being served.")
      .def_property_readonly("accepting", &service::Service::accepting,
                             "Whether new connections are still admitted.")
      .def_property_readonly(
          "action_registry", &service::Service::GetActionRegistry,
          "The template registry new connections are built from.")
      .def(
          "set_action_registry",
          [](const std::shared_ptr<service::Service>& self,
             std::shared_ptr<actions::ActionRegistry> action_registry) {
            ThrowIfNotOk(
                self->SetActionRegistry(std::move(action_registry)));
          },
          "Replace the registry new connections are built from, without "
          "interrupting any stream.",
          py::arg("action_registry"))
      .def(
          "stop_accepting",
          [](const std::shared_ptr<service::Service>& self) {
            ThrowIfNotOk(self->StopAccepting());
          },
          "Refuse new connections, leaving live ones alone.")
      .def(
          "drain",
          [](const std::shared_ptr<service::Service>& self,
             const py::typing::Optional<NativeDuration>& timeout) {
            const absl::Duration converted =
                ValueOrThrow(DurationFromPython(timeout, false));
            return FutureToPython(self->Drain(converted));
          },
          "Await the completion of every live session.",
          py::arg("timeout") = py::none())
      .def(
          "abort",
          [](const std::shared_ptr<service::Service>& self,
             const PyLike<NativeStatus>& status) {
            ThrowIfNotOk(self->Abort(StatusFromPython(status)));
          },
          "Stop accepting and abort every live session.", py::arg("status"))
      .def(
          "wait_done",
          [](const std::shared_ptr<service::Service>& self) {
            return FutureToPython(self->Done());
          },
          "Await the service being closed and empty.");

  module.def(
      "normalize_session_headers",
      [](const py::typing::Optional<PyMapping<py::str, py::bytes>>& headers) {
        data::ByteMap converted = ValueOrThrow(ByteMapFromPython(headers));
        return ByteMapToPython(ValueOrThrow(
            service::NormalizeSessionHeaders(std::move(converted))));
      },
      "Normalize a session headers mapping, returning the canonicalized "
      "header dict.",
      py::arg("headers") = py::none());
  module.attr("SESSION_STATUS_HEADER") =
      std::string(service::kSessionStatusHeader);
  module.attr("MAX_SINGLE_MESSAGE_SIZE") = service::kMaxSingleMessageSize;
  module.attr("SESSION_MAX_SINGLE_MESSAGE_SIZE") =
      service::kMaxSingleMessageSize;
}

}  // namespace a11::python

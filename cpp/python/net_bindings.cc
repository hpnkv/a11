// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <Python.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <cmath>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/typing.h>
#include <pybind11_abseil/no_throw_status.h>
#include <pybind11_abseil/status_casters.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/describe_endpoint.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/net/websocket_wire_stream.h"
#include "a11/net/wire_stream.h"
#include "a11/net/wire_stream_with_recv.h"
#include "python/bindings.h"
#include "python/casters.h"
#include "python/interop.h"
#include "thread/boost_primitives.h"

namespace a11::python {
namespace {

size_t SizeOption(const py::handle& value, size_t maximum, const char* name) {
  try {
    if (!py::isinstance<py::int_>(value) ||
        py::cast<bool>(value.attr("__lt__")(0))) {
      ThrowStatus(absl::InvalidArgumentError(std::string(name) +
                                             " must be non-negative"));
    }
    const auto converted = value.cast<std::uint64_t>();
    if (converted > maximum) {
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

absl::Duration MessageTimeoutOption(const py::handle& value) {
  if (value.is_none()) {
    return absl::InfiniteDuration();
  }
  try {
    if (py::isinstance<py::int_>(value)) {
      const auto integer = py::reinterpret_borrow<py::int_>(value);
      if (py::cast<bool>(integer.attr("__lt__")(0))) {
        return absl::InfiniteDuration();
      }
      return absl::Milliseconds(integer.cast<std::int64_t>());
    }
    if (py::isinstance<py::float_>(value)) {
      const auto number = value.cast<double>();
      if (number == -std::numeric_limits<double>::infinity() ||
          number < -1e-8) {
        return absl::InfiniteDuration();
      }
      if (!std::isfinite(number)) {
        ThrowStatus(absl::InvalidArgumentError(
            "message_timeout_millis must be finite"));
      }
      if (std::abs(number) < 1e-8) {
        return absl::ZeroDuration();
      }
      const double micros = number * 1000.0;
      if (micros >
          static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        ThrowStatus(absl::OutOfRangeError(
            "message_timeout_millis exceeds its supported range"));
      }
      return absl::Microseconds(static_cast<std::int64_t>(micros));
    }
    return ValueOrThrow(DurationFromPython(value, false));
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  }
}

void ValidateWireStreamOptions(const net::WireStreamOptions& options) {
  const absl::Status status = options.Validate();
  if (!status.ok()) {
    ThrowStatus(status);
  }
}

net::OnMessage MakeOnMessage(
    const std::shared_ptr<AsyncPythonCallback>& callback) {
  return [callback](std::optional<data::WireMessage> message) {
    return callback->Call(std::move(message));
  };
}

net::OnDone MakeOnDone(const std::shared_ptr<AsyncPythonCallback>& callback) {
  return [callback]() {
    return callback->Call();
  };
}

absl::StatusOr<std::pair<net::OnMessage, net::OnDone>> MakeStreamCallbacks(
    const py::object& on_message, const py::object& on_done) {
  absl::StatusOr<std::shared_ptr<AsyncPythonCallback>> message =
      AsyncPythonCallback::Create(on_message);
  if (!message.ok()) {
    return message.status();
  }
  absl::StatusOr<std::shared_ptr<AsyncPythonCallback>> done =
      AsyncPythonCallback::Create(on_done);
  if (!done.ok()) {
    return done.status();
  }
  return std::pair(MakeOnMessage(*message), MakeOnDone(*done));
}

class PyWireStream : public net::WireStream,
                     public py::trampoline_self_life_support {
 public:
  PyWireStream() {
    absl::StatusOr<std::shared_ptr<PythonLoop>> loop = PythonLoop::Capture();
    if (loop.ok()) {
      loop_ = std::move(*loop);
    } else {
      SetOverrideStatus(loop.status());
    }
  }

  absl::Status Send(data::WireMessage message) override {
    return CallStatus("send", std::move(message));
  }

  a11::Task Start(net::OnMessage on_message, net::OnDone on_done) override {
    return StartOrAccept("start", std::move(on_message), std::move(on_done));
  }

  a11::Task Accept(net::OnMessage on_message, net::OnDone on_done) override {
    return StartOrAccept("accept", std::move(on_message), std::move(on_done));
  }

  absl::Status HalfClose(data::ByteMap trailers) override {
    py::gil_scoped_acquire acquire;
    return CallStatus("half_close", ByteMapToPython(trailers));
  }

  a11::Task DrainOutgoingMessages() override {
    return CallTask("drain_outgoing_messages");
  }

  absl::Status Abort(absl::Status status) override {
    py::gil_scoped_acquire acquire;
    return CallStatus("abort", StatusToPython(status));
  }

  absl::Status SetDeadline(absl::Time deadline) override {
    py::gil_scoped_acquire acquire;
    return CallStatus("set_deadline", TimeToPython(deadline));
  }

  absl::Time deadline() const override {
    py::gil_scoped_acquire acquire;
    try {
      py::object self = py::cast(const_cast<PyWireStream*>(this),
                                 py::return_value_policy::reference);
      py::object value =
          py::module_::import("a11._asyncio")
              .attr("_get_python_override_attribute")(self, "deadline");
      absl::StatusOr<absl::Time> converted = TimeFromPython(value, false);
      if (!converted.ok()) {
        SetOverrideStatus(converted.status());
        return absl::InfinitePast();
      }
      return *converted;
    } catch (py::error_already_set& error) {
      SetOverrideStatus(StatusFromPythonException(error));
      return absl::InfinitePast();
    } catch (const std::exception& error) {
      SetOverrideStatus(absl::UnknownError(error.what()));
      return absl::InfinitePast();
    } catch (...) {
      SetOverrideStatus(
          absl::UnknownError("Python WireStream.deadline raised an exception"));
      return absl::InfinitePast();
    }
  }

  absl::Status GetStatus() const override {
    {
      thread::MutexLock lock(&mu_);
      if (!override_status_.ok()) {
        return override_status_;
      }
    }
    py::gil_scoped_acquire acquire;
    try {
      py::function override = py::get_override(
          static_cast<const net::WireStream*>(this), "get_status");
      if (!override) {
        return absl::UnimplementedError(
            "Python WireStream.get_status is not overridden");
      }
      return StatusFromPython(override());
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    } catch (const std::exception& error) {
      return absl::UnknownError(error.what());
    } catch (...) {
      return absl::UnknownError(
          "Python WireStream.get_status raised an exception");
    }
  }

  std::optional<data::ByteMap> GetTrailers() const override {
    py::gil_scoped_acquire acquire;
    try {
      py::function override = py::get_override(
          static_cast<const net::WireStream*>(this), "get_trailers");
      if (!override) {
        SetOverrideStatus(absl::UnimplementedError(
            "Python WireStream.get_trailers is not overridden"));
        return std::nullopt;
      }
      py::object result = override();
      if (result.is_none()) {
        return std::nullopt;
      }
      absl::StatusOr<data::ByteMap> converted =
          ByteMapFromPython(result, false);
      if (!converted.ok()) {
        SetOverrideStatus(converted.status());
        return std::nullopt;
      }
      return std::move(*converted);
    } catch (py::error_already_set& error) {
      SetOverrideStatus(StatusFromPythonException(error));
      return std::nullopt;
    } catch (const std::exception& error) {
      SetOverrideStatus(absl::UnknownError(error.what()));
      return std::nullopt;
    } catch (...) {
      SetOverrideStatus(absl::UnknownError(
          "Python WireStream.get_trailers raised an exception"));
      return std::nullopt;
    }
  }

  std::string GetId() const override {
    py::gil_scoped_acquire acquire;
    try {
      py::function override =
          py::get_override(static_cast<const net::WireStream*>(this), "get_id");
      if (!override) {
        SetOverrideStatus(absl::UnimplementedError(
            "Python WireStream.get_id is not overridden"));
        return {};
      }
      return override().cast<std::string>();
    } catch (py::error_already_set& error) {
      SetOverrideStatus(StatusFromPythonException(error));
      return {};
    } catch (const std::exception& error) {
      SetOverrideStatus(absl::UnknownError(error.what()));
      return {};
    } catch (...) {
      SetOverrideStatus(
          absl::UnknownError("Python WireStream.get_id raised an exception"));
      return {};
    }
  }

  void* absl_nullable GetImpl() const override { return nullptr; }

 private:
  template <typename... Args>
  absl::Status CallStatus(const char* name, Args&&... args) const {
    py::gil_scoped_acquire acquire;
    try {
      py::function override =
          py::get_override(static_cast<const net::WireStream*>(this), name);
      if (!override) {
        return absl::UnimplementedError(std::string("Python WireStream.") +
                                        name + " is not overridden");
      }
      py::object result = override(std::forward<Args>(args)...);
      if (!result.is_none()) {
        return absl::InvalidArgumentError(std::string("Python WireStream.") +
                                          name + " must return None");
      }
      return absl::OkStatus();
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    } catch (const std::exception& error) {
      return absl::UnknownError(error.what());
    } catch (...) {
      return absl::UnknownError(std::string("Python WireStream.") + name +
                                " raised an exception");
    }
  }

  a11::Task CallTask(const char* name) const {
    if (loop_ == nullptr) {
      return a11::FailedTask(absl::FailedPreconditionError(
          "Python WireStream has no asyncio event loop"));
    }
    py::gil_scoped_acquire acquire;
    try {
      py::function override =
          py::get_override(static_cast<const net::WireStream*>(this), name);
      if (!override) {
        return a11::FailedTask(absl::UnimplementedError(
            std::string("Python WireStream.") + name + " is not overridden"));
      }
      return CallPythonAsync<a11::Unit>(loop_, override);
    } catch (py::error_already_set& error) {
      return a11::FailedTask(StatusFromPythonException(error));
    } catch (const std::exception& error) {
      return a11::FailedTask(absl::UnknownError(error.what()));
    } catch (...) {
      return a11::FailedTask(absl::UnknownError(
          std::string("Python WireStream.") + name + " raised an exception"));
    }
  }

  a11::Task StartOrAccept(const char* name, net::OnMessage on_message,
                          net::OnDone on_done) const {
    if (loop_ == nullptr) {
      return a11::FailedTask(absl::FailedPreconditionError(
          "Python WireStream has no asyncio event loop"));
    }
    py::gil_scoped_acquire acquire;
    try {
      py::function override =
          py::get_override(static_cast<const net::WireStream*>(this), name);
      if (!override) {
        return a11::FailedTask(absl::UnimplementedError(
            std::string("Python WireStream.") + name + " is not overridden"));
      }
      py::object native_on_message = py::cpp_function(
          [callback = std::move(on_message)](
              std::optional<data::WireMessage> message) mutable {
            return FutureToPython(callback(std::move(message)));
          });
      py::object native_on_done =
          py::cpp_function([callback = std::move(on_done)]() mutable {
            return FutureToPython(callback());
          });
      py::object wrap =
          py::module_::import("a11._asyncio").attr("_wrap_async_callback");
      py::object py_on_message = wrap(std::move(native_on_message));
      py::object py_on_done = wrap(std::move(native_on_done));
      return CallPythonAsync<a11::Unit>(
          loop_, override, std::move(py_on_message), std::move(py_on_done));
    } catch (py::error_already_set& error) {
      return a11::FailedTask(StatusFromPythonException(error));
    } catch (const std::exception& error) {
      return a11::FailedTask(absl::UnknownError(error.what()));
    } catch (...) {
      return a11::FailedTask(absl::UnknownError(
          std::string("Python WireStream.") + name + " raised an exception"));
    }
  }

  void SetOverrideStatus(absl::Status status) const {
    if (status.ok()) {
      return;
    }
    thread::MutexLock lock(&mu_);
    if (override_status_.ok()) {
      override_status_ = std::move(status);
    }
  }

  std::shared_ptr<PythonLoop> loop_;
  mutable thread::Mutex mu_;
  mutable absl::Status override_status_;
};

PyFuture<py::none> StartStream(const std::shared_ptr<net::WireStream>& stream,
                               bool accept, const py::object& on_message,
                               const py::object& on_done) {
  absl::StatusOr<std::pair<net::OnMessage, net::OnDone>> callbacks =
      MakeStreamCallbacks(on_message, on_done);
  if (!callbacks.ok()) {
    return FutureToPython(a11::FailedTask(callbacks.status()));
  }
  a11::Task task = a11::SubmitTask(
      [stream, accept, callbacks = std::move(*callbacks)]() mutable {
        a11::Task started = accept ? stream->Accept(std::move(callbacks.first),
                                                    std::move(callbacks.second))
                                   : stream->Start(std::move(callbacks.first),
                                                   std::move(callbacks.second));
        return started.Await().status();
      });
  return FutureToPython(std::move(task));
}

py::typing::Optional<py::capsule> VoidPointer(void* pointer, const char* name) {
  if (pointer == nullptr) {
    return py::typing::Optional<py::capsule>(py::none());
  }
  return py::capsule(pointer, name);
}

}  // namespace

void BindNet(py::module_& module) {
  // First because more than one listener's options carry one.
  py::class_<net::DescribeEndpointOptions>(module, "DescribeEndpointOptions")
      .def(py::init<>(), "Construct default (disabled) describe options.")
      .def_readwrite("path", &net::DescribeEndpointOptions::path,
                     "Path the action descriptors are served on.")
      .def_property_readonly("enabled", &net::DescribeEndpointOptions::Enabled,
                             "Whether this server answers discovery over HTTP. "
                             "Turned on by Service.expose_descriptors_on.");

  py::enum_<net::CachePolicy>(module, "CachePolicy")
      .value("STREAM", net::CachePolicy::kStream,
             "A live stream: never cached, and not to be buffered.")
      .value("VOLATILE", net::CachePolicy::kVolatile,
             "A document that may change at any time: revalidate before reuse.")
      .value("UNSET", net::CachePolicy::kUnset, "Say nothing about caching.");

  py::class_<net::CorsOptions>(module, "CorsOptions")
      .def(py::init<>(), "Construct permissive cross-origin options.")
      .def_readwrite("enabled", &net::CorsOptions::enabled,
                     "Whether cross-origin headers are sent at all.")
      .def_readwrite("allow_origin", &net::CorsOptions::allow_origin,
                     "Access-Control-Allow-Origin; '*' admits any page.")
      .def_readwrite("allow_methods", &net::CorsOptions::allow_methods,
                     "Access-Control-Allow-Methods.")
      .def_readwrite("allow_headers", &net::CorsOptions::allow_headers,
                     "Access-Control-Allow-Headers.")
      .def_readwrite("expose_headers", &net::CorsOptions::expose_headers,
                     "Access-Control-Expose-Headers: what a page may read.")
      .def_readwrite("max_age_seconds", &net::CorsOptions::max_age_seconds,
                     "Access-Control-Max-Age in seconds; 0 omits it.")
      .def(
          "validate",
          [](const net::CorsOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the options, raising on error.");

  py::class_<net::ServerHeaderOptions>(module, "ServerHeaderOptions")
      .def(py::init<>(), "Construct default server response-header options.")
      .def_readwrite("server", &net::ServerHeaderOptions::server,
                     "Value of the Server header; empty sends none.")
      .def_readwrite("cors", &net::ServerHeaderOptions::cors,
                     "Cross-origin policy. Permissive by default, because a "
                     "browser is a first-class A11 client.")
      .def_readwrite("nosniff", &net::ServerHeaderOptions::nosniff,
                     "Send X-Content-Type-Options: nosniff.")
      .def(
          "validate",
          [](const net::ServerHeaderOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the options, raising on error.");

  py::class_<net::WireStreamOptions>(module, "WireStreamOptions")
      .def(py::init(
               [](const py::typing::Optional<py::int_>&
                      max_buffered_incoming_messages,
                  const py::typing::Optional<py::int_>& max_single_message_size,
                  const py::typing::Optional<py::int_>&
                      max_buffered_incoming_bytes,
                  const py::handle& message_timeout_millis,
                  const py::typing::Optional<NativeTime>& deadline) {
                 net::WireStreamOptions options{
                     .max_buffered_incoming_messages =
                         SizeOption(max_buffered_incoming_messages, 1024,
                                    "max_buffered_incoming_messages"),
                     .max_single_message_size = SizeOption(
                         max_single_message_size, net::kMaxSingleMessageSize,
                         "max_single_message_size"),
                     .max_buffered_incoming_bytes =
                         SizeOption(max_buffered_incoming_bytes,
                                    std::numeric_limits<size_t>::max(),
                                    "max_buffered_incoming_bytes"),
                     .message_timeout =
                         MessageTimeoutOption(message_timeout_millis),
                     .deadline = ValueOrThrow(TimeFromPython(deadline)),
                 };
                 ValidateWireStreamOptions(options);
                 return options;
               }),
           "Construct wire-stream options controlling buffering and timeouts "
           "for "
           "an agent stream. All arguments are keyword-friendly and validated "
           "on "
           "construction.",
           py::arg("max_buffered_incoming_messages") = 100,
           py::arg("max_single_message_size") = net::kMaxSingleMessageSize,
           py::arg("max_buffered_incoming_bytes") = 32 * 1024 * 1024,
           py::arg("message_timeout_millis") = py::none(),
           py::arg("deadline") = py::none())
      .def_readwrite("max_buffered_incoming_messages",
                     &net::WireStreamOptions::max_buffered_incoming_messages,
                     "Maximum number of inbound messages buffered before "
                     "backpressure is applied.")
      .def_readwrite("max_buffered_incoming_bytes",
                     &net::WireStreamOptions::max_buffered_incoming_bytes,
                     "Maximum total bytes of buffered inbound messages before "
                     "backpressure is applied.")
      .def_readwrite("max_single_message_size",
                     &net::WireStreamOptions::max_single_message_size,
                     "Maximum size, in bytes, of a single wire message.")
      .def_property(
          "message_timeout",
          [](const net::WireStreamOptions& options) -> NativeDuration {
            return NativeDuration(options.message_timeout);
          },
          [](net::WireStreamOptions& options, const py::object& value) {
            options.message_timeout = MessageTimeoutOption(value);
          },
          "Per-message inactivity timeout as a duration.")
      .def_property(
          "message_timeout_millis",
          [](const net::WireStreamOptions& options) -> NativeDuration {
            return NativeDuration(options.message_timeout);
          },
          [](net::WireStreamOptions& options, const py::object& value) {
            options.message_timeout = MessageTimeoutOption(value);
          },
          "Per-message inactivity timeout expressed in milliseconds.")
      .def_property(
          "deadline",
          [](const net::WireStreamOptions& options) -> NativeTime {
            return NativeTime(options.deadline);
          },
          [](net::WireStreamOptions& options, const py::object& value) {
            options.deadline = ValueOrThrow(TimeFromPython(value));
          },
          "Absolute wall-clock deadline after which the stream is aborted.")
      .def("validate", &ValidateWireStreamOptions,
           "Validate the options, raising on invalid configuration.");

  py::class_<net::ChannelFramingOptions>(module, "ChannelFramingOptions")
      .def(py::init<>(), "Construct default channel framing options.")
      .def_readwrite("split_size", &net::ChannelFramingOptions::split_size,
                     "Maximum payload size before a message is split into "
                     "multiple frames.")
      .def_readwrite("max_pending_messages",
                     &net::ChannelFramingOptions::max_pending_messages,
                     "Maximum number of in-flight (unacknowledged) frames.")
      .def_readwrite("max_pending_bytes",
                     &net::ChannelFramingOptions::max_pending_bytes,
                     "Maximum total bytes of in-flight frames.")
      .def(
          "validate",
          [](const net::ChannelFramingOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the framing options, raising on invalid configuration.");

  py::classh<net::WireStream, PyWireStream> wire_stream(module, "WireStream");
  wire_stream
      .def(py::init<>(),
           "Construct the abstract WireStream base. Subclass this in Python to "
           "implement a custom asynchronous, bidirectional transport for an "
           "agent; the abstract operations (send, start/accept, get_status, "
           "get_trailers, ...) are dispatched to your overrides.")
      .def(
          "send",
          [](net::WireStream& self, data::WireMessage message) {
            // Without the GIL, because `send` is not always as non-blocking as
            // it looks. It hung `wire/one_way_throughput` on both event loops.
            ThrowIfNotOk(
                WithoutGil([&] { return self.Send(std::move(message)); }));
          },
          R"doc(Queue a message for asynchronous delivery to the peer. This call is non-blocking: the message enters the ordered outbound queue and the transport applies backpressure.

Examples:
    Admit a request before closing the local sending side:

    ```python
    stream.send(request_message)
    ```
)doc",
          py::arg("message"))
      .def(
          "start",
          [](const std::shared_ptr<net::WireStream>& self,
             const py::typing::Callable<py::object(
                 py::typing::Optional<data::WireMessage>)>& on_message,
             const py::typing::Callable<py::object()>& on_done) {
            return StartStream(self, false, on_message, on_done);
          },
          R"doc(Begin driving the stream as the initiating side, delivering inbound messages to `on_message` and completion to `on_done`. Callbacks are awaited as data arrives.

Examples:
    Start a client transport with application callbacks:

    ```python
    await stream.start(on_message, on_transport_done)
    ```
)doc",
          py::arg("on_message"), py::arg("on_done"))
      .def(
          "accept",
          [](const std::shared_ptr<net::WireStream>& self,
             const py::typing::Callable<py::object(
                 py::typing::Optional<data::WireMessage>)>& on_message,
             const py::typing::Callable<py::object()>& on_done) {
            return StartStream(self, true, on_message, on_done);
          },
          "Begin driving the stream as the responding (server) side, "
          "delivering "
          "each inbound message to the asynchronous on_message callback and "
          "end-of-stream to on_done. Use this instead of start() when this "
          "endpoint is answering an incoming agent connection. Returns an "
          "awaitable that resolves when acceptance completes; use on_done as "
          "the terminal barrier.",
          py::arg("on_message"), py::arg("on_done"))
      .def(
          "half_close",
          [](const std::shared_ptr<net::WireStream>& self,
             const py::typing::Optional<PyMapping<py::str, py::bytes>>&
                 trailers) {
            absl::StatusOr<data::ByteMap> converted =
                ByteMapFromPython(trailers);
            if (!converted.ok()) {
              ThrowStatus(converted.status());
            }
            // Without the GIL: half-closing queues a message the same way
            // `send` does, and takes the same locks.
            ThrowIfNotOk(WithoutGil(
                [&] { return self->HalfClose(std::move(*converted)); }));
          },
          R"doc(Signal that this endpoint has finished sending, optionally attaching trailers. The stream stays open for inbound messages.

Examples:
    End the local half and wait until queued messages reach the transport:

    ```python
    stream.half_close()
    await stream.drain_outgoing_messages()
    ```
)doc",
          py::arg("trailers") = py::none())
      .def(
          "drain_outgoing_messages",
          [](const std::shared_ptr<net::WireStream>& self) {
            // Without the GIL: this takes the stream's fiber-aware mutex to
            // read the drain future out, and whoever holds that mutex may need
            // the GIL to finish.
            return FutureToPython(
                WithoutGil([&] { return self->DrainOutgoingMessages(); }));
          },
          R"doc(Await until every queued outbound message has been handed to the transport. Call `half_close` first so buffered output is not dropped.

Examples:
    Use the transport delivery barrier during orderly shutdown:

    ```python
    stream.half_close()
    await stream.drain_outgoing_messages()
    ```
)doc")
      .def(
          "abort",
          [](net::WireStream& self, const PyLike<NativeStatus>& status) {
            // Convert while the GIL is held, then abort without it: aborting
            // takes the stream's fiber-aware locks and wakes its pumps.
            absl::Status requested = StatusFromPython(status);
            ThrowIfNotOk(
                WithoutGil([&] { return self.Abort(std::move(requested)); }));
          },
          R"doc(Terminate the stream immediately with an error status, discarding buffered messages and propagating failure to the peer and pending receivers.

Examples:
    End an exchange when its upstream disappears:

    ```python
    stream.abort(Status(
        code=StatusCode.UNAVAILABLE,
        message="upstream connection was lost",
    ))
    ```
)doc",
          py::arg("status"))
      .def(
          "set_deadline",
          [](const std::shared_ptr<net::WireStream>& self,
             const py::typing::Optional<NativeTime>& deadline) {
            absl::StatusOr<absl::Time> converted = TimeFromPython(deadline);
            if (!converted.ok()) {
              ThrowStatus(converted.status());
            }
            // Without the GIL: a deadline already in the past aborts the stream
            // from this call, and aborting ends it -- which awaits the caller's
            // own on_done.
            ThrowIfNotOk(
                WithoutGil([&] { return self->SetDeadline(*converted); }));
          },
          "Set an absolute wall-clock deadline after which the stream is "
          "automatically aborted; pass None to clear it.",
          py::arg("deadline") = py::none())
      .def_property_readonly(
          "deadline",
          [](const net::WireStream& self) -> NativeTime {
            return NativeTime(self.deadline());
          },
          "The stream's current absolute deadline, after which it is "
          "automatically aborted.")
      .def(
          "get_status",
          [](const net::WireStream& self) -> NativeStatus {
            // Also without the GIL: reading the status notices an expired
            // deadline and aborts, which lands in the same await as
            // set_deadline above.
            return NativeStatus(WithoutGil([&] { return self.GetStatus(); }));
          },
          "Return the stream's terminal status once it has finished, or OK "
          "while it is still active. Inspect this after the stream completes "
          "to "
          "learn whether the agent exchange succeeded or failed.")
      .def(
          "get_trailers",
          [](const net::WireStream& self)
              -> py::typing::Optional<py::typing::Dict<py::str, py::bytes>> {
            std::optional<data::ByteMap> trailers = self.GetTrailers();
            if (!trailers.has_value()) {
              return py::none();
            }
            return ByteMapToPython(*trailers);
          },
          "Return the trailers (final metadata) the peer sent at half-close, "
          "or "
          "None if none were received. Read this after the stream ends to "
          "recover end-of-turn metadata from the agent exchange.")
      .def(
          "get_id", &net::WireStream::GetId,
          "Return the stream's stable identifier, which also seeds its tracing "
          "trace id.")
      .def(
          "get_impl",
          [](const net::WireStream& self) {
            return VoidPointer(self.GetImpl(), "a11.WireStream.impl");
          },
          "Return an opaque native handle to the underlying implementation, or "
          "None. Intended for advanced interop, not normal agent code.");

  py::classh<net::InProcessWireStream, net::WireStream>(
      module, "InProcessWireStream", py::dynamic_attr())
      .def_static(
          "create_pair",
          [](std::optional<net::WireStreamOptions> options,
             std::optional<net::WireStreamOptions> first_options,
             std::optional<net::WireStreamOptions> second_options) {
            return ValueOrThrow(net::InProcessWireStream::CreatePair(
                options, first_options, second_options));
          },
          "Create a connected pair of in-process wire streams that talk to "
          "each other directly in memory, with no network involved. One "
          "endpoint drives start() while the other drives accept(). Pass "
          "shared options, or per-endpoint first_options/second_options, to "
          "tune buffering and timeouts.",
          py::arg("options") = std::nullopt,
          py::arg("first_options") = std::nullopt,
          py::arg("second_options") = std::nullopt)
      .def(
          "wait",
          [](const net::InProcessWireStream& self) {
            return FutureToPython(self.Done());
          },
          "Await until this in-process stream has fully finished. Block on "
          "this "
          "to know a local agent exchange has completed before tearing the "
          "pair "
          "down.");
  module.def(
      "create_in_process_wire_stream_pair",
      [](std::optional<net::WireStreamOptions> options,
         std::optional<net::WireStreamOptions> first_options,
         std::optional<net::WireStreamOptions> second_options) {
        return ValueOrThrow(net::InProcessWireStream::CreatePair(
            options, first_options, second_options));
      },
      "Create a connected pair of in-process wire streams (free-function form "
      "of InProcessWireStream.create_pair).",
      py::arg("options") = std::nullopt,
      py::arg("first_options") = std::nullopt,
      py::arg("second_options") = std::nullopt);

  py::classh<net::WireStreamWithRecv, net::WireStream>(
      module, "WireStreamWithRecv", py::dynamic_attr())
      .def(py::init([](const py::object& value) {
             if (!py::isinstance<net::WireStream>(value)) {
               ThrowStatus(absl::InvalidArgumentError(
                   "stream must be a WireStream instance"));
             }
             return ValueOrThrow(net::WireStreamWithRecv::Create(
                 value.cast<std::shared_ptr<net::WireStream>>()));
           }),
           "Wrap a callback-based WireStream in a pull-oriented adapter that "
           "exposes receive().",
           py::arg("stream"), py::keep_alive<1, 2>())
      .def(
          "start",
          [](const std::shared_ptr<net::WireStreamWithRecv>& self) {
            return FutureToPython(a11::SubmitTask(
                [self] { return self->Start().Await().status(); }));
          },
          "Start the wrapped stream as the initiating side; returns an "
          "awaitable.")
      .def(
          "accept",
          [](const std::shared_ptr<net::WireStreamWithRecv>& self) {
            return FutureToPython(a11::SubmitTask(
                [self] { return self->Accept().Await().status(); }));
          },
          "Accept on the wrapped stream as the responding side; returns an "
          "awaitable.")
      .def(
          "receive",
          [](const std::shared_ptr<net::WireStreamWithRecv>& self,
             const py::typing::Optional<NativeDuration>& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::optional<data::WireMessage>>(
                      converted.status()));
            }
            // Without the GIL, for the same reason as
            // `drain_outgoing_messages`: starting a receive touches the
            // stream's fiber-aware state.
            return FutureToPython(
                WithoutGil([&] { return self->Receive(*converted); }));
          },
          "Await the next inbound message, or None at end of stream, honoring "
          "the optional timeout.",
          py::arg("timeout") = py::none())
      .def_property_readonly("wrapped_stream",
                             &net::WireStreamWithRecv::wrapped_stream,
                             "The underlying WireStream being adapted.");

  module.attr("WIRE_STREAM_ABORT_STATUS_HEADER") =
      std::string(net::kAbortStatusHeader);
  module.attr("WIRE_STREAM_MAX_SINGLE_MESSAGE_SIZE") =
      net::kMaxSingleMessageSize;

  py::class_<net::WebSocketClientOptions>(module, "WebSocketClientOptions")
      .def(py::init<>(), "Construct default WebSocket client options.")
      .def_readwrite("http2_options",
                     &net::WebSocketClientOptions::http2_options,
                     "HTTP/2 transport options for the client connection.")
      .def_property(
          "headers",
          [](const net::WebSocketClientOptions& options) -> PyHeaderPairs {
            return HeaderPairsToPython(options.headers);
          },
          [](net::WebSocketClientOptions& options, const PyHeadersLike& value) {
            net::HttpHeaders headers =
                ValueOrThrow(HeaderPairsFromPython(value, "WebSocket headers"));
            ThrowIfNotOk(net::ValidateHttpHeaders(headers));
            options.headers = std::move(headers);
          },
          "Extra HTTP headers sent on the WebSocket handshake, as a list of "
          "(name, value) string pairs.")
      .def_readwrite("framing", &net::WebSocketClientOptions::framing,
                     "Channel framing options controlling message splitting "
                     "and buffering.")
      .def_property(
          "handshake_deadline",
          [](const net::WebSocketClientOptions& options) -> NativeTime {
            return NativeTime(options.handshake_deadline);
          },
          [](net::WebSocketClientOptions& options,
             const py::typing::Optional<NativeTime>& value) {
            options.handshake_deadline = ValueOrThrow(TimeFromPython(value));
          },
          "Absolute deadline for the handshake alone. Bounds reaching a "
          "silent peer without bounding the session that follows.")
      .def(
          "validate",
          [](const net::WebSocketClientOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the client options, raising on invalid configuration.");

  py::classh<net::WebSocketWireStream, net::WireStream>(module,
                                                        "WebSocketWireStream")
      .def_static(
          "connect",
          [](const std::string& url, net::WireStreamOptions options,
             net::WebSocketClientOptions websocket_options) {
            return ValueOrThrow(net::WebSocketWireStream::CreateClient(
                url, options, std::move(websocket_options)));
          },
          "Open a client WebSocket connection to url and return a WireStream "
          "over it. This is the standard way for an agent to dial out to a "
          "remote A11 endpoint; the returned stream is then driven "
          "asynchronously via start()/send(). Tune transport buffering with "
          "options and the handshake (headers, framing, HTTP/2, TLS) with "
          "websocket_options.",
          py::arg("url"), py::arg("options") = net::WireStreamOptions{},
          py::arg("websocket_options") = net::WebSocketClientOptions{})
      .def_property_readonly(
          "request_path",
          [](const net::WebSocketWireStream& self) {
            return self.GetRequestPath();
          },
          "The path this stream was accepted on, query string included, or "
          "empty for a client stream. On a server accepting under "
          "WebSocketServerOptions.path_prefix this is the only place the rest "
          "of the path survives, and so the only way one port can serve more "
          "than one thing.")
      .def_property_readonly(
          "request_headers",
          [](const net::WebSocketWireStream& self) -> PyHeaderPairs {
            return HeaderPairsToPython(self.GetRequestHeaders());
          },
          "The headers the accepted request carried, as (name, value) pairs, "
          "or empty for a client stream. A per-connection credential arrives "
          "here, which is what lets a server authenticate a stream rather "
          "than a port.");

  py::class_<net::WebSocketServerOptions>(module, "WebSocketServerOptions")
      .def(py::init<>(), "Construct default WebSocket server options.")
      .def_readwrite("path", &net::WebSocketServerOptions::path,
                     "URL path on which the server accepts WebSocket "
                     "connections.")
      .def_readwrite("stream_options",
                     &net::WebSocketServerOptions::stream_options,
                     "Default WireStreamOptions applied to each accepted "
                     "stream.")
      .def_readwrite("describe", &net::WebSocketServerOptions::describe,
                     "Server-side GET /actions on this same port, for whoever "
                     "has the port number but not an A11 client. Point it at a "
                     "service with Service.expose_descriptors_on.")
      .def_readwrite(
          "headers", &net::WebSocketServerOptions::headers,
          "Response-header policy for this port's HTTP surface: the "
          "Server header, cross-origin access and cache hints. A 404 "
          "and a GET /actions are ordinary HTTP responses even on a "
          "port whose business is upgrades.")
      .def_readwrite("framing", &net::WebSocketServerOptions::framing,
                     "Channel framing options for accepted streams.")
      .def_readwrite("http2_options",
                     &net::WebSocketServerOptions::http2_options,
                     "HTTP/2 transport options, including TLS settings.")
      .def_readwrite("port", &net::WebSocketServerOptions::port,
                     "TCP port to listen on; 0 selects an ephemeral port.")
      .def_readwrite(
          "path_prefix", &net::WebSocketServerOptions::path_prefix,
          "Also accept any path under this prefix, as well as `path`. Empty "
          "by default, which serves exactly one endpoint. Set it to serve "
          "many on one port -- one per agent, say -- and read the rest of the "
          "path from WebSocketWireStream.request_path in your on_stream "
          "handler. Must start and end with '/'.")
      .def_readwrite("bind_address", &net::WebSocketServerOptions::bind_address,
                     "Local address the server binds to.")
      .def_property(
          "enable_tls",
          [](const net::WebSocketServerOptions& options) {
            return options.http2_options.tls.enabled;
          },
          [](net::WebSocketServerOptions& options, bool value) {
            options.http2_options.tls.enabled = value;
          },
          "Whether TLS is enabled (mirrors http2_options.tls.enabled).")
      .def(
          "validate",
          [](const net::WebSocketServerOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the server options, raising on invalid configuration.");

  py::classh<net::WebSocketWireServer>(module, "WebSocketWireServer")
      .def_static(
          "create",
          [](const py::object& on_stream, net::WebSocketServerOptions options) {
            absl::StatusOr<std::shared_ptr<AsyncPythonCallback>> callback =
                AsyncPythonCallback::Create(on_stream);
            if (!callback.ok()) {
              ThrowStatus(callback.status());
            }
            // Create() blocks on the libuv loop (Http2Server::Create ->
            // RunOnUv), so release the GIL while it runs.
            return ValueWithoutGil([&] {
              return net::WebSocketWireServer::Create(
                  [callback = std::move(*callback)](
                      const std::shared_ptr<net::WebSocketWireStream>& stream) {
                    return callback->Call(stream);
                  },
                  std::move(options));
            });
          },
          "Start a WebSocket server that accepts incoming A11 connections, "
          "invoking the asynchronous on_stream callback with a fresh "
          "WireStream "
          "for each accepted client. This is the server-side entry point for "
          "hosting an agent: each callback runs concurrently and typically "
          "drives accept() on its stream. Configure the listen address, port, "
          "path and TLS via options.",
          py::arg("on_stream"),
          py::arg("options") = net::WebSocketServerOptions{})
      .def(
          "stop",
          [](net::WebSocketWireServer& self) {
            CallWithoutGil([&self] { return self.Stop(); });
          },
          "Stop the server and close the listening socket, releasing the bound "
          "port. Call this to shut the agent host down cleanly; it blocks "
          "until "
          "shutdown completes.")
      .def_property_readonly(
          "port",
          [](const net::WebSocketWireServer& self) {
            return ValueOrThrow(self.port());
          },
          "The actual TCP port the server is listening on, resolved even when "
          "an ephemeral port (0) was requested.")
      .def_property_readonly(
          "running", &net::WebSocketWireServer::running,
          "Whether the server is currently accepting connections.")
      .def(
          "get_impl",
          [](const net::WebSocketWireServer& self) {
            return VoidPointer(self.GetImpl(), "a11.WebSocketWireServer.impl");
          },
          "Return an opaque native handle to the underlying implementation, or "
          "None. Intended for advanced interop.");
}

}  // namespace a11::python

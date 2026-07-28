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
#include <pybind11_abseil/no_throw_status.h>
#include <pybind11_abseil/status_casters.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
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
    const std::uint64_t converted = value.cast<std::uint64_t>();
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
  if (value.is_none())
    return absl::InfiniteDuration();
  try {
    if (py::isinstance<py::int_>(value)) {
      const py::int_ integer = py::reinterpret_borrow<py::int_>(value);
      if (py::cast<bool>(integer.attr("__lt__")(0)))
        return absl::InfiniteDuration();
      return absl::Milliseconds(integer.cast<std::int64_t>());
    }
    if (py::isinstance<py::float_>(value)) {
      const double number = value.cast<double>();
      if (number == -std::numeric_limits<double>::infinity() ||
          number < -1e-8) {
        return absl::InfiniteDuration();
      }
      if (!std::isfinite(number)) {
        ThrowStatus(absl::InvalidArgumentError(
            "message_timeout_millis must be finite"));
      }
      if (std::abs(number) < 1e-8)
        return absl::ZeroDuration();
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
  if (!status.ok())
    ThrowStatus(status);
}

void CheckStatus(const absl::Status& status) {
  if (!status.ok())
    ThrowStatus(status);
}

template <typename Operation>
void CallWithoutGil(Operation&& operation) {
  absl::Status status;
  {
    py::gil_scoped_release release;
    status = std::forward<Operation>(operation)();
  }
  CheckStatus(status);
}

class AsyncPythonCallback {
 public:
  static absl::StatusOr<std::shared_ptr<AsyncPythonCallback>> Create(
      const py::object& callable) {
    if (!PyCallable_Check(callable.ptr())) {
      return absl::InvalidArgumentError("callback must be callable");
    }
    absl::StatusOr<std::shared_ptr<PythonLoop>> loop = PythonLoop::Capture();
    if (!loop.ok())
      return loop.status();

    struct MakeSharedEnabler final : AsyncPythonCallback {
      MakeSharedEnabler(PyObject* callable, std::shared_ptr<PythonLoop> loop)
          : AsyncPythonCallback(callable, std::move(loop)) {}
    };

    return std::make_shared<MakeSharedEnabler>(callable.inc_ref().ptr(),
                                               std::move(*loop));
  }

  AsyncPythonCallback(const AsyncPythonCallback&) = delete;
  AsyncPythonCallback& operator=(const AsyncPythonCallback&) = delete;

  ~AsyncPythonCallback() {
    if (Py_IsInitialized() == 0)
      return;
    PyGILState_STATE state = PyGILState_Ensure();
    Py_CLEAR(callable_);
    PyGILState_Release(state);
  }

  template <typename... Args>
  a11::Task Call(Args&&... args) const {
    py::gil_scoped_acquire acquire;
    py::function function = py::reinterpret_borrow<py::function>(callable_);
    return CallPythonAsync<a11::Unit>(loop_, function,
                                      std::forward<Args>(args)...);
  }

 private:
  AsyncPythonCallback(PyObject* callable, std::shared_ptr<PythonLoop> loop)
      : callable_(callable), loop_(std::move(loop)) {}

  PyObject* callable_ = nullptr;
  std::shared_ptr<PythonLoop> loop_;
};

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
  if (!message.ok())
    return message.status();
  absl::StatusOr<std::shared_ptr<AsyncPythonCallback>> done =
      AsyncPythonCallback::Create(on_done);
  if (!done.ok())
    return done.status();
  return std::pair(MakeOnMessage(*message), MakeOnDone(*done));
}

class PyWireStream : public net::WireStream {
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
      if (!override_status_.ok())
        return override_status_;
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
      if (result.is_none())
        return std::nullopt;
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
    if (status.ok())
      return;
    thread::MutexLock lock(&mu_);
    if (override_status_.ok())
      override_status_ = std::move(status);
  }

  std::shared_ptr<PythonLoop> loop_;
  mutable thread::Mutex mu_;
  mutable absl::Status override_status_;
};

py::object StartStream(const std::shared_ptr<net::WireStream>& stream,
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

py::object VoidPointer(void* pointer, const char* name) {
  if (pointer == nullptr)
    return py::none();
  return py::capsule(pointer, name);
}

}  // namespace

void BindNet(py::module_& module) {
  py::class_<net::WireStreamOptions>(module, "WireStreamOptions")
      .def(
          py::init([](const py::handle& max_buffered_incoming_messages,
                      const py::handle& max_single_message_size,
                      const py::handle& max_buffered_incoming_bytes,
                      const py::handle& message_timeout_millis,
                      const py::handle& deadline) {
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
                .message_timeout = MessageTimeoutOption(message_timeout_millis),
                .deadline = ValueOrThrow(TimeFromPython(deadline)),
            };
            ValidateWireStreamOptions(options);
            return options;
          }),
          py::arg("max_buffered_incoming_messages") = 100,
          py::arg("max_single_message_size") = net::kMaxSingleMessageSize,
          py::arg("max_buffered_incoming_bytes") = 32 * 1024 * 1024,
          py::arg("message_timeout_millis") = py::none(),
          py::arg("deadline") = py::none())
      .def_readwrite("max_buffered_incoming_messages",
                     &net::WireStreamOptions::max_buffered_incoming_messages)
      .def_readwrite("max_buffered_incoming_bytes",
                     &net::WireStreamOptions::max_buffered_incoming_bytes)
      .def_readwrite("max_single_message_size",
                     &net::WireStreamOptions::max_single_message_size)
      .def_property(
          "message_timeout",
          [](const net::WireStreamOptions& options) {
            return DurationToPython(options.message_timeout);
          },
          [](net::WireStreamOptions& options, const py::object& value) {
            options.message_timeout = MessageTimeoutOption(value);
          })
      .def_property(
          "message_timeout_millis",
          [](const net::WireStreamOptions& options) {
            return DurationToPython(options.message_timeout);
          },
          [](net::WireStreamOptions& options, const py::object& value) {
            options.message_timeout = MessageTimeoutOption(value);
          })
      .def_property(
          "deadline",
          [](const net::WireStreamOptions& options) {
            return TimeToPython(options.deadline);
          },
          [](net::WireStreamOptions& options, const py::object& value) {
            options.deadline = ValueOrThrow(TimeFromPython(value));
          })
      .def("validate", &ValidateWireStreamOptions);

  py::class_<net::ChannelFramingOptions>(module, "ChannelFramingOptions")
      .def(py::init<>())
      .def_readwrite("split_size", &net::ChannelFramingOptions::split_size)
      .def_readwrite("max_pending_messages",
                     &net::ChannelFramingOptions::max_pending_messages)
      .def_readwrite("max_pending_bytes",
                     &net::ChannelFramingOptions::max_pending_bytes)
      .def("validate", [](const net::ChannelFramingOptions& options) {
        CheckStatus(options.Validate());
      });

  py::class_<net::WireStream, PyWireStream, std::shared_ptr<net::WireStream>>
      wire_stream(module, "WireStream");
  wire_stream.def(py::init<>())
      .def("send",
           [](net::WireStream& self, data::WireMessage message) {
             CheckStatus(self.Send(std::move(message)));
           })
      .def("start",
           [](const std::shared_ptr<net::WireStream>& self,
              const py::object& on_message, const py::object& on_done) {
             return StartStream(self, false, on_message, on_done);
           })
      .def("accept",
           [](const std::shared_ptr<net::WireStream>& self,
              const py::object& on_message, const py::object& on_done) {
             return StartStream(self, true, on_message, on_done);
           })
      .def(
          "half_close",
          [](const std::shared_ptr<net::WireStream>& self,
             const py::object& trailers) {
            absl::StatusOr<data::ByteMap> converted =
                ByteMapFromPython(trailers);
            if (!converted.ok())
              ThrowStatus(converted.status());
            CheckStatus(self->HalfClose(std::move(*converted)));
          },
          py::arg("trailers") = py::none())
      .def("drain_outgoing_messages",
           [](const std::shared_ptr<net::WireStream>& self) {
             return FutureToPython(self->DrainOutgoingMessages());
           })
      .def("abort",
           [](net::WireStream& self, const py::handle& status) {
             CheckStatus(self.Abort(StatusFromPython(status)));
           })
      .def(
          "set_deadline",
          [](const std::shared_ptr<net::WireStream>& self,
             const py::object& deadline) {
            absl::StatusOr<absl::Time> converted = TimeFromPython(deadline);
            if (!converted.ok())
              ThrowStatus(converted.status());
            CheckStatus(self->SetDeadline(*converted));
          },
          py::arg("deadline") = py::none())
      .def_property_readonly("deadline",
                             [](const net::WireStream& self) {
                               return TimeToPython(self.deadline());
                             })
      .def("get_status",
           [](const net::WireStream& self) {
             return StatusToPython(self.GetStatus());
           })
      .def("get_trailers",
           [](const net::WireStream& self) -> py::object {
             std::optional<data::ByteMap> trailers = self.GetTrailers();
             if (!trailers.has_value())
               return py::none();
             return ByteMapToPython(*trailers);
           })
      .def("get_id", &net::WireStream::GetId)
      .def("get_impl", [](const net::WireStream& self) {
        return VoidPointer(self.GetImpl(), "a11.WireStream.impl");
      });

  py::class_<net::InProcessWireStream, net::WireStream,
             std::shared_ptr<net::InProcessWireStream>>(
      module, "InProcessWireStream", py::dynamic_attr())
      .def_static(
          "create_pair",
          [](std::optional<net::WireStreamOptions> options,
             std::optional<net::WireStreamOptions> first_options,
             std::optional<net::WireStreamOptions> second_options) {
            return ValueOrThrow(net::InProcessWireStream::CreatePair(
                std::move(options), std::move(first_options),
                std::move(second_options)));
          },
          py::arg("options") = std::nullopt,
          py::arg("first_options") = std::nullopt,
          py::arg("second_options") = std::nullopt)
      .def("wait", [](const net::InProcessWireStream& self) {
        return FutureToPython(self.Done());
      });
  module.def(
      "create_in_process_wire_stream_pair",
      [](std::optional<net::WireStreamOptions> options,
         std::optional<net::WireStreamOptions> first_options,
         std::optional<net::WireStreamOptions> second_options) {
        return ValueOrThrow(net::InProcessWireStream::CreatePair(
            std::move(options), std::move(first_options),
            std::move(second_options)));
      },
      py::arg("options") = std::nullopt,
      py::arg("first_options") = std::nullopt,
      py::arg("second_options") = std::nullopt);

  py::class_<net::WireStreamWithRecv, net::WireStream,
             std::shared_ptr<net::WireStreamWithRecv>>(
      module, "WireStreamWithRecv", py::dynamic_attr())
      .def(py::init([](const py::object& value) {
             if (!py::isinstance<net::WireStream>(value)) {
               ThrowStatus(absl::InvalidArgumentError(
                   "stream must be a WireStream instance"));
             }
             return ValueOrThrow(net::WireStreamWithRecv::Create(
                 value.cast<std::shared_ptr<net::WireStream>>()));
           }),
           py::arg("stream"), py::keep_alive<1, 2>())
      .def("start",
           [](const std::shared_ptr<net::WireStreamWithRecv>& self) {
             return FutureToPython(a11::SubmitTask(
                 [self] { return self->Start().Await().status(); }));
           })
      .def("accept",
           [](const std::shared_ptr<net::WireStreamWithRecv>& self) {
             return FutureToPython(a11::SubmitTask(
                 [self] { return self->Accept().Await().status(); }));
           })
      .def(
          "receive",
          [](const std::shared_ptr<net::WireStreamWithRecv>& self,
             const py::object& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::optional<data::WireMessage>>(
                      converted.status()));
            }
            return FutureToPython(self->Receive(*converted));
          },
          py::arg("timeout") = py::none())
      .def_property_readonly("wrapped_stream",
                             &net::WireStreamWithRecv::wrapped_stream);

  module.attr("WIRE_STREAM_ABORT_STATUS_HEADER") =
      std::string(net::kAbortStatusHeader);
  module.attr("WIRE_STREAM_MAX_SINGLE_MESSAGE_SIZE") =
      net::kMaxSingleMessageSize;

  py::class_<net::WebSocketClientOptions>(module, "WebSocketClientOptions")
      .def(py::init<>())
      .def_readwrite("http2_options",
                     &net::WebSocketClientOptions::http2_options)
      .def_property(
          "headers",
          [](const net::WebSocketClientOptions& options) {
            py::list result;
            for (const auto& [name, value] : options.headers) {
              result.append(py::make_tuple(name, value));
            }
            return result;
          },
          [](net::WebSocketClientOptions& options, const py::object& value) {
            net::HttpHeaders headers;
            py::object entries =
                PyMapping_Check(value.ptr()) != 0 && py::hasattr(value, "items")
                    ? value.attr("items")()
                    : value;
            for (const py::handle item : entries) {
              py::sequence pair = py::reinterpret_borrow<py::sequence>(item);
              if (pair.size() != 2 || !py::isinstance<py::str>(pair[0]) ||
                  !py::isinstance<py::str>(pair[1])) {
                ThrowStatus(absl::InvalidArgumentError(
                    "WebSocket headers must contain pairs of strings"));
              }
              headers.emplace_back(pair[0].cast<std::string>(),
                                   pair[1].cast<std::string>());
            }
            CheckStatus(net::ValidateHttpHeaders(headers));
            options.headers = std::move(headers);
          })
      .def_readwrite("framing", &net::WebSocketClientOptions::framing)
      .def("validate", [](const net::WebSocketClientOptions& options) {
        CheckStatus(options.Validate());
      });

  py::class_<net::WebSocketWireStream, net::WireStream,
             std::shared_ptr<net::WebSocketWireStream>>(module,
                                                        "WebSocketWireStream")
      .def_static(
          "connect",
          [](std::string url, net::WireStreamOptions options,
             net::WebSocketClientOptions websocket_options) {
            return ValueOrThrow(net::WebSocketWireStream::CreateClient(
                std::move(url), options, std::move(websocket_options)));
          },
          py::arg("url"), py::arg("options") = net::WireStreamOptions{},
          py::arg("websocket_options") = net::WebSocketClientOptions{});

  py::class_<net::WebSocketServerOptions>(module, "WebSocketServerOptions")
      .def(py::init<>())
      .def_readwrite("path", &net::WebSocketServerOptions::path)
      .def_readwrite("stream_options",
                     &net::WebSocketServerOptions::stream_options)
      .def_readwrite("framing", &net::WebSocketServerOptions::framing)
      .def_readwrite("http2_options",
                     &net::WebSocketServerOptions::http2_options)
      .def_readwrite("port", &net::WebSocketServerOptions::port)
      .def_readwrite("bind_address", &net::WebSocketServerOptions::bind_address)
      .def_property(
          "enable_tls",
          [](const net::WebSocketServerOptions& options) {
            return options.http2_options.tls.enabled;
          },
          [](net::WebSocketServerOptions& options, bool value) {
            options.http2_options.tls.enabled = value;
          })
      .def("validate", [](const net::WebSocketServerOptions& options) {
        CheckStatus(options.Validate());
      });

  py::class_<net::WebSocketWireServer,
             std::shared_ptr<net::WebSocketWireServer>>(module,
                                                        "WebSocketWireServer")
      .def_static(
          "create",
          [](const py::object& on_stream, net::WebSocketServerOptions options) {
            absl::StatusOr<std::shared_ptr<AsyncPythonCallback>> callback =
                AsyncPythonCallback::Create(on_stream);
            if (!callback.ok())
              ThrowStatus(callback.status());
            return ValueOrThrow(net::WebSocketWireServer::Create(
                [callback = std::move(*callback)](
                    std::shared_ptr<net::WebSocketWireStream> stream) {
                  return callback->Call(std::move(stream));
                },
                std::move(options)));
          },
          py::arg("on_stream"),
          py::arg("options") = net::WebSocketServerOptions{})
      .def("stop",
           [](net::WebSocketWireServer& self) {
             CallWithoutGil([&self] { return self.Stop(); });
           })
      .def_property_readonly("port",
                             [](const net::WebSocketWireServer& self) {
                               return ValueOrThrow(self.port());
                             })
      .def_property_readonly("running", &net::WebSocketWireServer::running)
      .def("get_impl", [](const net::WebSocketWireServer& self) {
        return VoidPointer(self.GetImpl(), "a11.WebSocketWireServer.impl");
      });
}

}  // namespace a11::python

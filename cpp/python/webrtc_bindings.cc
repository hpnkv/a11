// Copyright 2026 The A11 Authors.

#include <cstddef>
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
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <rtc/configuration.hpp>

#include "a11/concurrency/future.h"
#include "a11/net/signalling.h"
#include "a11/net/webrtc_wire_stream.h"
#include "a11/net/websocket_signalling.h"
#include "a11/net/wire_stream.h"
#include "a11/status.h"
#include "python/bindings.h"
#include "python/casters.h"
#include "python/interop.h"
#include "thread/boost_primitives.h"

namespace a11::python {
namespace {

class PythonSignallingCallback {
 public:
  static absl::StatusOr<std::shared_ptr<PythonSignallingCallback>> Create(
      const py::object& callable, const char* name) {
    if (PyCallable_Check(callable.ptr()) == 0) {
      return absl::InvalidArgumentError(std::string(name) +
                                        " must be callable");
    }
    absl::StatusOr<std::shared_ptr<PythonLoop>> loop = PythonLoop::Capture();
    if (!loop.ok())
      return loop.status();

    struct MakeSharedEnabler final : PythonSignallingCallback {
      MakeSharedEnabler(PyObject* callable, std::shared_ptr<PythonLoop> loop)
          : PythonSignallingCallback(callable, std::move(loop)) {}
    };

    return std::make_shared<MakeSharedEnabler>(callable.inc_ref().ptr(),
                                               std::move(*loop));
  }

  PythonSignallingCallback(const PythonSignallingCallback&) = delete;
  PythonSignallingCallback& operator=(const PythonSignallingCallback&) = delete;

  ~PythonSignallingCallback() {
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
  PythonSignallingCallback(PyObject* callable, std::shared_ptr<PythonLoop> loop)
      : callable_(callable), loop_(std::move(loop)) {}

  PyObject* callable_ = nullptr;
  std::shared_ptr<PythonLoop> loop_;
};

net::OnSignallingMessage MakeSignallingCallback(
    const std::shared_ptr<PythonSignallingCallback>& callback) {
  return [callback](net::SignallingMessage message) {
    return callback->Call(std::move(message));
  };
}

class PySignallingTransport : public net::SignallingTransport {
 public:
  absl::Status Send(net::SignallingMessage message) override {
    return CallNone("send", std::move(message));
  }

  absl::Status SetOnMessage(net::OnSignallingMessage on_message) override {
    py::gil_scoped_acquire acquire;
    try {
      py::function override = py::get_override(
          static_cast<const net::SignallingTransport*>(this), "set_on_message");
      if (!override) {
        return absl::UnimplementedError(
            "Python SignallingTransport.set_on_message is not overridden");
      }
      py::object callback =
          py::cpp_function([on_message = std::move(on_message)](
                               net::SignallingMessage message) mutable {
            return FutureToPython(on_message(std::move(message)));
          });
      py::object result = override(std::move(callback));
      if (!result.is_none()) {
        return absl::InvalidArgumentError(
            "Python SignallingTransport.set_on_message must return None");
      }
      return absl::OkStatus();
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    } catch (const std::exception& error) {
      return absl::UnknownError(error.what());
    } catch (...) {
      return absl::UnknownError(
          "Python SignallingTransport.set_on_message raised an exception");
    }
  }

  absl::Status Close() override { return CallNone("close"); }

  std::string identity() const override {
    py::gil_scoped_acquire acquire;
    try {
      py::function override = py::get_override(
          static_cast<const net::SignallingTransport*>(this), "identity");
      if (!override) {
        SetReadStatus(absl::UnimplementedError(
            "Python SignallingTransport.identity is not overridden"));
        return {};
      }
      return override().cast<std::string>();
    } catch (py::error_already_set& error) {
      SetReadStatus(StatusFromPythonException(error));
      return {};
    } catch (const std::exception& error) {
      SetReadStatus(absl::UnknownError(error.what()));
      return {};
    } catch (...) {
      SetReadStatus(absl::UnknownError(
          "Python SignallingTransport.identity raised an exception"));
      return {};
    }
  }

  bool connected() const override {
    py::gil_scoped_acquire acquire;
    try {
      py::function override = py::get_override(
          static_cast<const net::SignallingTransport*>(this), "connected");
      if (!override) {
        SetReadStatus(absl::UnimplementedError(
            "Python SignallingTransport.connected is not overridden"));
        return false;
      }
      return override().cast<bool>();
    } catch (py::error_already_set& error) {
      SetReadStatus(StatusFromPythonException(error));
      return false;
    } catch (const std::exception& error) {
      SetReadStatus(absl::UnknownError(error.what()));
      return false;
    } catch (...) {
      SetReadStatus(absl::UnknownError(
          "Python SignallingTransport.connected raised an exception"));
      return false;
    }
  }

  absl::Status GetStatus() const override {
    {
      thread::MutexLock lock(&mu_);
      if (!read_status_.ok())
        return read_status_;
    }
    py::gil_scoped_acquire acquire;
    try {
      py::function override = py::get_override(
          static_cast<const net::SignallingTransport*>(this), "get_status");
      if (!override) {
        return absl::UnimplementedError(
            "Python SignallingTransport.get_status is not overridden");
      }
      return StatusFromPython(override());
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    } catch (const std::exception& error) {
      return absl::UnknownError(error.what());
    } catch (...) {
      return absl::UnknownError(
          "Python SignallingTransport.get_status raised an exception");
    }
  }

 private:
  template <typename... Args>
  absl::Status CallNone(const char* name, Args&&... args) const {
    py::gil_scoped_acquire acquire;
    try {
      py::function override = py::get_override(
          static_cast<const net::SignallingTransport*>(this), name);
      if (!override) {
        return absl::UnimplementedError(
            std::string("Python SignallingTransport.") + name +
            " is not overridden");
      }
      py::object result = override(std::forward<Args>(args)...);
      if (!result.is_none()) {
        return absl::InvalidArgumentError(
            std::string("Python SignallingTransport.") + name +
            " must return None");
      }
      return absl::OkStatus();
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    } catch (const std::exception& error) {
      return absl::UnknownError(error.what());
    } catch (...) {
      return absl::UnknownError(std::string("Python SignallingTransport.") +
                                name + " raised an exception");
    }
  }

  void SetReadStatus(absl::Status status) const {
    if (status.ok())
      return;
    thread::MutexLock lock(&mu_);
    if (read_status_.ok())
      read_status_ = std::move(status);
  }

  mutable thread::Mutex mu_;
  mutable absl::Status read_status_;
};

py::object PointerCapsule(void* pointer, const char* name) {
  if (pointer == nullptr)
    return py::none();
  return py::capsule(pointer, name);
}

void ThrowIfNotOk(const absl::Status& status) {
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
  ThrowIfNotOk(status);
}

}  // namespace

void BindWebRtc(py::module_& module) {
  py::enum_<net::SignallingMessageType>(module, "SignallingMessageType")
      .value("DESCRIPTION", net::SignallingMessageType::kDescription)
      .value("CANDIDATE", net::SignallingMessageType::kCandidate)
      .value("ERROR", net::SignallingMessageType::kError)
      .export_values();

  py::class_<net::SignallingMessage>(module, "SignallingMessage")
      .def(py::init([](net::SignallingMessageType type, std::string sender,
                       std::string recipient, std::string description,
                       std::string description_type, std::string candidate,
                       std::string mid, const py::object& error) {
             net::SignallingMessage result{
                 .type = type,
                 .sender = std::move(sender),
                 .recipient = std::move(recipient),
                 .description = std::move(description),
                 .description_type = std::move(description_type),
                 .candidate = std::move(candidate),
                 .mid = std::move(mid)};
             if (!error.is_none())
               result.error = StatusFromPython(error);
             return result;
           }),
           py::arg("type") = net::SignallingMessageType::kDescription,
           py::arg("sender") = "", py::arg("recipient") = "",
           py::arg("description") = "", py::arg("description_type") = "",
           py::arg("candidate") = "", py::arg("mid") = "",
           py::arg("error") = py::none())
      .def_readwrite("type", &net::SignallingMessage::type)
      .def_readwrite("sender", &net::SignallingMessage::sender)
      .def_readwrite("recipient", &net::SignallingMessage::recipient)
      .def_readwrite("description", &net::SignallingMessage::description)
      .def_readwrite("description_type",
                     &net::SignallingMessage::description_type)
      .def_readwrite("candidate", &net::SignallingMessage::candidate)
      .def_readwrite("mid", &net::SignallingMessage::mid)
      .def_property(
          "error",
          [](const net::SignallingMessage& message) {
            return StatusToPython(message.error);
          },
          [](net::SignallingMessage& message, const py::object& status) {
            message.error = StatusFromPython(status);
          })
      .def("validate",
           [](const net::SignallingMessage& message) {
             ThrowIfNotOk(message.Validate());
           })
      .def("to_json",
           [](const net::SignallingMessage& message) {
             return ValueOrThrow(message.ToJson());
           })
      .def_static("from_json", [](const std::string& json) {
        return ValueOrThrow(net::SignallingMessage::FromJson(json));
      });

  py::class_<net::SignallingTransport, PySignallingTransport,
             std::shared_ptr<net::SignallingTransport>>
      transport(module, "SignallingTransport");
  transport.def(py::init<>())
      .def("send",
           [](net::SignallingTransport& self, net::SignallingMessage message) {
             ThrowIfNotOk(self.Send(std::move(message)));
           })
      .def("set_on_message",
           [](net::SignallingTransport& self, const py::object& callback) {
             std::shared_ptr<PythonSignallingCallback> owner = ValueOrThrow(
                 PythonSignallingCallback::Create(callback, "on_message"));
             ThrowIfNotOk(self.SetOnMessage(MakeSignallingCallback(owner)));
           })
      .def("close",
           [](net::SignallingTransport& self) { ThrowIfNotOk(self.Close()); })
      .def("identity", &net::SignallingTransport::identity)
      .def("connected", &net::SignallingTransport::connected)
      .def("get_status", [](const net::SignallingTransport& self) {
        return StatusToPython(self.GetStatus());
      });

  py::class_<net::SignallingEndpoint, net::SignallingTransport,
             std::shared_ptr<net::SignallingEndpoint>>(module,
                                                       "SignallingEndpoint");

  py::class_<net::SignallingService, std::shared_ptr<net::SignallingService>>(
      module, "SignallingService")
      .def_static("create", &net::SignallingService::Create)
      .def(py::init([]() { return net::SignallingService::Create(); }))
      .def("connect",
           [](net::SignallingService& self, std::string identity,
              const py::object& on_message) {
             std::shared_ptr<PythonSignallingCallback> callback = ValueOrThrow(
                 PythonSignallingCallback::Create(on_message, "on_message"));
             return ValueOrThrow(self.Connect(
                 std::move(identity), MakeSignallingCallback(callback)));
           })
      .def("contains", &net::SignallingService::Contains)
      .def("__contains__", &net::SignallingService::Contains)
      .def("identities", &net::SignallingService::Identities)
      .def("stop",
           [](net::SignallingService& self) { ThrowIfNotOk(self.Stop()); });

  py::enum_<net::TurnRelayType>(module, "TurnRelayType")
      .value("UDP", net::TurnRelayType::kUdp)
      .value("TCP", net::TurnRelayType::kTcp)
      .value("TLS", net::TurnRelayType::kTls)
      .export_values();

  py::class_<net::TurnServer>(module, "TurnServer")
      .def(py::init<>())
      .def_static("from_string",
                  [](const std::string& value) {
                    return ValueOrThrow(net::TurnServer::FromString(value));
                  })
      .def_readwrite("hostname", &net::TurnServer::hostname)
      .def_readwrite("port", &net::TurnServer::port)
      .def_readwrite("username", &net::TurnServer::username)
      .def_readwrite("password", &net::TurnServer::password)
      .def_readwrite("relay_type", &net::TurnServer::relay_type)
      .def(py::self == py::self);

  py::class_<net::WebRtcConfiguration>(module, "WebRtcConfiguration")
      .def(py::init<>())
      .def_readwrite("max_message_size",
                     &net::WebRtcConfiguration::max_message_size)
      .def_readwrite("channel_split_size",
                     &net::WebRtcConfiguration::channel_split_size)
      .def_readwrite("enable_ice_udp_mux",
                     &net::WebRtcConfiguration::enable_ice_udp_mux)
      .def_readwrite("stun_servers", &net::WebRtcConfiguration::stun_servers)
      .def_readwrite("turn_servers", &net::WebRtcConfiguration::turn_servers)
      .def_readwrite("preferred_port_range",
                     &net::WebRtcConfiguration::preferred_port_range)
      .def_readwrite("bind_address", &net::WebRtcConfiguration::bind_address)
      .def("validate", [](const net::WebRtcConfiguration& configuration) {
        ThrowIfNotOk(configuration.Validate());
      });

  py::class_<net::WebRtcWireStream, net::WireStream,
             std::shared_ptr<net::WebRtcWireStream>>(module, "WebRtcWireStream")
      .def_static(
          "create_client",
          [](std::string identity, std::string peer_identity,
             std::shared_ptr<net::SignallingService> signalling,
             net::WebRtcConfiguration configuration,
             net::WireStreamOptions options) {
            return ValueOrThrow(net::WebRtcWireStream::CreateClient(
                std::move(identity), std::move(peer_identity),
                std::move(signalling), std::move(configuration), options));
          },
          py::arg("identity"), py::arg("peer_identity"), py::arg("signalling"),
          py::arg("configuration") = net::WebRtcConfiguration{},
          py::arg("options") = net::WireStreamOptions{})
      .def_static(
          "create_client",
          [](std::string peer_identity,
             std::shared_ptr<net::SignallingTransport> signalling,
             net::WebRtcConfiguration configuration,
             net::WireStreamOptions options) {
            return ValueOrThrow(net::WebRtcWireStream::CreateClient(
                std::move(peer_identity), std::move(signalling),
                std::move(configuration), options));
          },
          py::arg("peer_identity"), py::arg("signalling"),
          py::arg("configuration") = net::WebRtcConfiguration{},
          py::arg("options") = net::WireStreamOptions{})
      .def_property_readonly("data_channel",
                             [](const net::WebRtcWireStream& self) {
                               return PointerCapsule(
                                   self.data_channel().get(),
                                   "a11.WebRtcWireStream.data_channel");
                             })
      .def_property_readonly("peer_connection",
                             [](const net::WebRtcWireStream& self) {
                               return PointerCapsule(
                                   self.peer_connection().get(),
                                   "a11.WebRtcWireStream.peer_connection");
                             })
      .def_property_readonly("signalling_endpoint",
                             &net::WebRtcWireStream::signalling_endpoint);

  py::class_<net::WebRtcWireServer, std::shared_ptr<net::WebRtcWireServer>>(
      module, "WebRtcWireServer")
      .def_static(
          "create",
          [](std::string identity,
             std::shared_ptr<net::SignallingService> signalling,
             const py::object& on_stream,
             net::WebRtcConfiguration configuration,
             net::WireStreamOptions stream_options) {
            std::shared_ptr<PythonSignallingCallback> callback = ValueOrThrow(
                PythonSignallingCallback::Create(on_stream, "on_stream"));
            return ValueOrThrow(net::WebRtcWireServer::Create(
                std::move(identity), std::move(signalling),
                [callback = std::move(callback)](
                    std::shared_ptr<net::WebRtcWireStream> stream) {
                  return callback->Call(std::move(stream));
                },
                std::move(configuration), stream_options));
          },
          py::arg("identity"), py::arg("signalling"), py::arg("on_stream"),
          py::arg("configuration") = net::WebRtcConfiguration{},
          py::arg("stream_options") = net::WireStreamOptions{})
      .def("stop",
           [](net::WebRtcWireServer& self) {
             CallWithoutGil([&self] { return self.Stop(); });
           })
      .def_property_readonly("identity", &net::WebRtcWireServer::identity)
      .def_property_readonly("running", &net::WebRtcWireServer::running)
      .def_property_readonly("pending_peer_count",
                             &net::WebRtcWireServer::pending_peer_count)
      .def_property_readonly("signalling_endpoint",
                             &net::WebRtcWireServer::signalling_endpoint);

  py::class_<net::WebSocketSignallingClientOptions>(
      module, "WebSocketSignallingClientOptions")
      .def(py::init<>())
      .def_readwrite("http2_options",
                     &net::WebSocketSignallingClientOptions::http2_options)
      .def_readwrite("max_message_size",
                     &net::WebSocketSignallingClientOptions::max_message_size)
      .def_property(
          "deadline",
          [](const net::WebSocketSignallingClientOptions& options) {
            return TimeToPython(options.deadline);
          },
          [](net::WebSocketSignallingClientOptions& options,
             const py::object& deadline) {
            options.deadline = ValueOrThrow(TimeFromPython(deadline));
          })
      .def("validate",
           [](const net::WebSocketSignallingClientOptions& options) {
             ThrowIfNotOk(options.Validate());
           });

  py::class_<net::WebSocketSignallingClient, net::SignallingTransport,
             std::shared_ptr<net::WebSocketSignallingClient>>(
      module, "WebSocketSignallingClient")
      .def_static(
          "connect",
          [](std::string url, std::string identity,
             const py::object& on_message,
             net::WebSocketSignallingClientOptions options) {
            net::OnSignallingMessage callback;
            if (!on_message.is_none()) {
              std::shared_ptr<PythonSignallingCallback> owner = ValueOrThrow(
                  PythonSignallingCallback::Create(on_message, "on_message"));
              callback = MakeSignallingCallback(owner);
            }
            return FutureToPython(net::WebSocketSignallingClient::Connect(
                std::move(url), std::move(identity), std::move(callback),
                std::move(options)));
          },
          py::arg("url"), py::arg("identity"),
          py::arg("on_message") = py::none(),
          py::arg("options") = net::WebSocketSignallingClientOptions{})
      .def("get_impl", [](const net::WebSocketSignallingClient& self) {
        return PointerCapsule(self.GetImpl(),
                              "a11.WebSocketSignallingClient.impl");
      });

  py::class_<net::WebSocketSignallingServerOptions>(
      module, "WebSocketSignallingServerOptions")
      .def(py::init<>())
      .def_readwrite("path_prefix",
                     &net::WebSocketSignallingServerOptions::path_prefix)
      .def_readwrite("port", &net::WebSocketSignallingServerOptions::port)
      .def_readwrite("bind_address",
                     &net::WebSocketSignallingServerOptions::bind_address)
      .def_readwrite("http2_options",
                     &net::WebSocketSignallingServerOptions::http2_options)
      .def_property(
          "enable_tls",
          [](const net::WebSocketSignallingServerOptions& options) {
            return options.http2_options.tls.enabled;
          },
          [](net::WebSocketSignallingServerOptions& options, bool value) {
            options.http2_options.tls.enabled = value;
          })
      .def_readwrite("max_message_size",
                     &net::WebSocketSignallingServerOptions::max_message_size)
      .def("validate",
           [](const net::WebSocketSignallingServerOptions& options) {
             ThrowIfNotOk(options.Validate());
           });

  py::class_<net::WebSocketSignallingServer,
             std::shared_ptr<net::WebSocketSignallingServer>>(
      module, "WebSocketSignallingServer")
      .def_static(
          "create",
          [](std::shared_ptr<net::SignallingService> service,
             net::WebSocketSignallingServerOptions options) {
            return ValueOrThrow(net::WebSocketSignallingServer::Create(
                std::move(service), std::move(options)));
          },
          py::arg("service"),
          py::arg("options") = net::WebSocketSignallingServerOptions{})
      .def("stop",
           [](net::WebSocketSignallingServer& self) {
             CallWithoutGil([&self] { return self.Stop(); });
           })
      .def_property_readonly("port", &net::WebSocketSignallingServer::port)
      .def_property_readonly("running",
                             &net::WebSocketSignallingServer::running)
      .def_property_readonly("service",
                             &net::WebSocketSignallingServer::service)
      .def("get_impl", [](const net::WebSocketSignallingServer& self) {
        return PointerCapsule(self.GetImpl(),
                              "a11.WebSocketSignallingServer.impl");
      });
}

}  // namespace a11::python

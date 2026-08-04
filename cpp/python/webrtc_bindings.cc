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

// Like CallWithoutGil, but for a blocking operation returning absl::StatusOr<T>:
// releases the GIL while it runs, then unwraps the value (or throws) with the
// GIL re-held. Blocking calls that reach Http2Server::Create (RunOnUv ->
// Future::Await) must release the GIL, or the libuv loop thread deadlocks trying
// to take the GIL to complete the work. Convert any Python arguments before
// calling this, while the GIL is still held.
template <typename Operation>
auto ValueWithoutGil(Operation&& operation) {
  auto result = [&] {
    py::gil_scoped_release release;
    return std::forward<Operation>(operation)();
  }();
  return ValueOrThrow(std::move(result));
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
           "Construct a signalling message describing an SDP offer/answer, an "
           "ICE candidate, or an error.",
           py::arg("type") = net::SignallingMessageType::kDescription,
           py::arg("sender") = "", py::arg("recipient") = "",
           py::arg("description") = "", py::arg("description_type") = "",
           py::arg("candidate") = "", py::arg("mid") = "",
           py::arg("error") = py::none())
      .def_readwrite("type", &net::SignallingMessage::type,
                     "Kind of signalling payload this message carries.")
      .def_readwrite("sender", &net::SignallingMessage::sender,
                     "Identity of the peer that sent this message.")
      .def_readwrite("recipient", &net::SignallingMessage::recipient,
                     "Identity of the peer this message is addressed to.")
      .def_readwrite("description", &net::SignallingMessage::description,
                     "SDP description body (for DESCRIPTION messages).")
      .def_readwrite("description_type",
                     &net::SignallingMessage::description_type,
                     "SDP description type, e.g. \"offer\" or \"answer\".")
      .def_readwrite("candidate", &net::SignallingMessage::candidate,
                     "ICE candidate string (for CANDIDATE messages).")
      .def_readwrite("mid", &net::SignallingMessage::mid,
                     "Media stream identifier associated with the candidate.")
      .def_property(
          "error",
          [](const net::SignallingMessage& message) {
            return StatusToPython(message.error);
          },
          [](net::SignallingMessage& message, const py::object& status) {
            message.error = StatusFromPython(status);
          },
          "Error status carried by ERROR messages, or OK otherwise.")
      .def("validate",
           [](const net::SignallingMessage& message) {
             ThrowIfNotOk(message.Validate());
           },
           "Raise if the message fields are inconsistent for its type.")
      .def("to_json",
           [](const net::SignallingMessage& message) {
             return ValueOrThrow(message.ToJson());
           },
           "Serialize this message to its JSON wire representation.")
      .def_static("from_json",
                  [](const std::string& json) {
                    return ValueOrThrow(net::SignallingMessage::FromJson(json));
                  },
                  "Parse a signalling message from its JSON wire "
                  "representation.",
                  py::arg("json"));

  py::class_<net::SignallingTransport, PySignallingTransport,
             std::shared_ptr<net::SignallingTransport>>
      transport(module, "SignallingTransport");
  transport.def(py::init<>(), "Construct a base signalling transport.")
      .def("send",
           [](net::SignallingTransport& self, net::SignallingMessage message) {
             ThrowIfNotOk(self.Send(std::move(message)));
           },
           "Send a signalling message to the peer (non-blocking).",
           py::arg("message"))
      .def("set_on_message",
           [](net::SignallingTransport& self, const py::object& callback) {
             std::shared_ptr<PythonSignallingCallback> owner = ValueOrThrow(
                 PythonSignallingCallback::Create(callback, "on_message"));
             ThrowIfNotOk(self.SetOnMessage(MakeSignallingCallback(owner)));
           },
           "Register an async callback invoked for each inbound message.",
           py::arg("callback"))
      .def("close",
           [](net::SignallingTransport& self) { ThrowIfNotOk(self.Close()); },
           "Close the transport and release its resources.")
      .def("identity", &net::SignallingTransport::identity,
           "Return the local identity bound to this transport.")
      .def("connected", &net::SignallingTransport::connected,
           "Return whether the transport is currently connected.")
      .def("get_status",
           [](const net::SignallingTransport& self) {
             return StatusToPython(self.GetStatus());
           },
           "Return the current transport status.");

  py::class_<net::SignallingEndpoint, net::SignallingTransport,
             std::shared_ptr<net::SignallingEndpoint>>(module,
                                                       "SignallingEndpoint");

  py::class_<net::SignallingService, std::shared_ptr<net::SignallingService>>(
      module, "SignallingService")
      .def_static("create", &net::SignallingService::Create,
                  "Create a new in-process signalling service.")
      .def(py::init([]() { return net::SignallingService::Create(); }),
           "Create a new in-process signalling service.")
      .def("connect",
           [](net::SignallingService& self, std::string identity,
              const py::object& on_message) {
             std::shared_ptr<PythonSignallingCallback> callback = ValueOrThrow(
                 PythonSignallingCallback::Create(on_message, "on_message"));
             return ValueOrThrow(self.Connect(
                 std::move(identity), MakeSignallingCallback(callback)));
           },
           "Register an identity and its async inbound-message callback, "
           "returning a signalling endpoint.",
           py::arg("identity"), py::arg("on_message"))
      .def("contains", &net::SignallingService::Contains,
           "Return whether the given identity is currently connected.",
           py::arg("identity"))
      .def("__contains__", &net::SignallingService::Contains,
           "Return whether the given identity is currently connected.",
           py::arg("identity"))
      .def("identities", &net::SignallingService::Identities,
           "Return the list of currently connected identities.")
      .def("stop",
           [](net::SignallingService& self) { ThrowIfNotOk(self.Stop()); },
           "Stop the service and disconnect all endpoints.");

  py::enum_<net::TurnRelayType>(module, "TurnRelayType")
      .value("UDP", net::TurnRelayType::kUdp)
      .value("TCP", net::TurnRelayType::kTcp)
      .value("TLS", net::TurnRelayType::kTls)
      .export_values();

  py::class_<net::TurnServer>(module, "TurnServer")
      .def(py::init<>(), "Construct an empty TURN server configuration.")
      .def_static("from_string",
                  [](const std::string& value) {
                    return ValueOrThrow(net::TurnServer::FromString(value));
                  },
                  "Parse a TURN server from a URL-like string.",
                  py::arg("value"))
      .def_readwrite("hostname", &net::TurnServer::hostname,
                     "TURN server hostname.")
      .def_readwrite("port", &net::TurnServer::port,
                     "TURN server port (default 3478).")
      .def_readwrite("username", &net::TurnServer::username,
                     "Username used to authenticate with the TURN server.")
      .def_readwrite("password", &net::TurnServer::password,
                     "Password used to authenticate with the TURN server.")
      .def_readwrite("relay_type", &net::TurnServer::relay_type,
                     "Transport used to reach the TURN server (UDP/TCP/TLS).")
      .def(py::self == py::self,
           "Return whether two TURN server configurations are equal.");

  py::class_<net::WebRtcConfiguration>(module, "WebRtcConfiguration")
      .def(py::init<>(), "Construct a default WebRTC configuration.")
      .def_readwrite("max_message_size",
                     &net::WebRtcConfiguration::max_message_size,
                     "Advertised local libdatachannel message size ceiling.")
      .def_readwrite("channel_split_size",
                     &net::WebRtcConfiguration::channel_split_size,
                     "Size at which A11 fragments large logical messages.")
      .def_readwrite("enable_ice_udp_mux",
                     &net::WebRtcConfiguration::enable_ice_udp_mux,
                     "Whether to multiplex ICE traffic over a single UDP port.")
      .def_readwrite("stun_servers", &net::WebRtcConfiguration::stun_servers,
                     "List of STUN server URLs used for ICE.")
      .def_readwrite("turn_servers", &net::WebRtcConfiguration::turn_servers,
                     "List of TURN servers used to relay ICE traffic.")
      .def_readwrite("preferred_port_range",
                     &net::WebRtcConfiguration::preferred_port_range,
                     "Optional (min, max) local port range for ICE.")
      .def_readwrite("bind_address", &net::WebRtcConfiguration::bind_address,
                     "Optional local address to bind ICE sockets to.")
      .def_readwrite(
          "desired_channels", &net::WebRtcConfiguration::desired_channels,
          "Number of WebRTC data channels a dialing client opens per "
          "connection and keeps replenished. Streaming with several channels "
          "lets slow per-channel acknowledgement round-trips overlap; the "
          "stream still behaves as one ordered, reliable channel. Defaults to "
          "8. Has no effect on the accepting side.")
      .def_readwrite(
          "max_channels", &net::WebRtcConfiguration::max_channels,
          "Maximum number of WebRTC data channels an accepting server admits "
          "per peer connection. Surplus channels a client opens beyond this "
          "are refused. Defaults to 8. Has no effect on the dialing side.")
      .def("validate",
           [](const net::WebRtcConfiguration& configuration) {
             ThrowIfNotOk(configuration.Validate());
           },
           "Raise if the configuration is invalid.");

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
          "Open a WebRTC data-channel wire stream to a peer over a shared "
          "in-process signalling service. Use this when building an agent that "
          "connects out to a named peer: it performs the ICE/SDP handshake and "
          "resolves to a WireStream you read and write logical messages on "
          "asynchronously. The returned stream carries A11-framed messages, "
          "fragmenting large payloads transparently.",
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
          "Open a WebRTC data-channel wire stream to a peer over an explicit "
          "signalling transport (e.g. a WebSocket signalling client). Prefer "
          "this overload when your agent reaches a peer across a network via a "
          "remote signalling server rather than an in-process service. The "
          "call drives the asynchronous ICE/SDP negotiation and yields a "
          "WireStream for streaming logical messages once connected.",
          py::arg("peer_identity"), py::arg("signalling"),
          py::arg("configuration") = net::WebRtcConfiguration{},
          py::arg("options") = net::WireStreamOptions{})
      .def_property_readonly("data_channel",
                             [](const net::WebRtcWireStream& self) {
                               return PointerCapsule(
                                   self.data_channel().get(),
                                   "a11.WebRtcWireStream.data_channel");
                             },
                             "Opaque capsule around the underlying "
                             "libdatachannel DataChannel. Exposed for advanced "
                             "interop and diagnostics; agent code normally "
                             "reads and writes through the WireStream API "
                             "rather than touching this directly.")
      .def_property_readonly("peer_connection",
                             [](const net::WebRtcWireStream& self) {
                               return PointerCapsule(
                                   self.peer_connection().get(),
                                   "a11.WebRtcWireStream.peer_connection");
                             },
                             "Opaque capsule around the underlying "
                             "libdatachannel PeerConnection. Useful for "
                             "inspecting ICE/connection state during "
                             "debugging; not required for normal streaming.")
      .def_property_readonly("signalling_endpoint",
                             &net::WebRtcWireStream::signalling_endpoint,
                             "Signalling transport this stream negotiated over. "
                             "Lets an agent observe or reuse the channel that "
                             "carried the asynchronous SDP/ICE handshake.");

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
          "Create a WebRTC server that accepts peer connections and invokes "
          "the async on_stream callback with each new WebRtcWireStream.",
          py::arg("identity"), py::arg("signalling"), py::arg("on_stream"),
          py::arg("configuration") = net::WebRtcConfiguration{},
          py::arg("stream_options") = net::WireStreamOptions{})
      .def("stop",
           [](net::WebRtcWireServer& self) {
             CallWithoutGil([&self] { return self.Stop(); });
           },
           "Stop the server and stop accepting new peer connections.")
      .def_property_readonly("identity", &net::WebRtcWireServer::identity,
                             "Local identity this server listens as.")
      .def_property_readonly("running", &net::WebRtcWireServer::running,
                             "Whether the server is currently running.")
      .def_property_readonly("pending_peer_count",
                             &net::WebRtcWireServer::pending_peer_count,
                             "Number of peers still completing negotiation.")
      .def_property_readonly("signalling_endpoint",
                             &net::WebRtcWireServer::signalling_endpoint,
                             "Signalling endpoint the server negotiates over.");

  py::class_<net::WebSocketSignallingClientOptions>(
      module, "WebSocketSignallingClientOptions")
      .def(py::init<>(),
           "Construct default WebSocket signalling client options.")
      .def_readwrite("http2_options",
                     &net::WebSocketSignallingClientOptions::http2_options,
                     "HTTP/2 transport options used for the connection.")
      .def_readwrite("max_message_size",
                     &net::WebSocketSignallingClientOptions::max_message_size,
                     "Maximum inbound signalling message size in bytes.")
      .def_property(
          "deadline",
          [](const net::WebSocketSignallingClientOptions& options) {
            return TimeToPython(options.deadline);
          },
          [](net::WebSocketSignallingClientOptions& options,
             const py::object& deadline) {
            options.deadline = ValueOrThrow(TimeFromPython(deadline));
          },
          "Deadline by which the connection must be established.")
      .def("validate",
           [](const net::WebSocketSignallingClientOptions& options) {
             ThrowIfNotOk(options.Validate());
           },
           "Raise if the options are invalid.");

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
          "Asynchronously connect to a WebSocket signalling server, resolving "
          "to a client once registered under the given identity.",
          py::arg("url"), py::arg("identity"),
          py::arg("on_message") = py::none(),
          py::arg("options") = net::WebSocketSignallingClientOptions{})
      .def("get_impl",
           [](const net::WebSocketSignallingClient& self) {
             return PointerCapsule(self.GetImpl(),
                                   "a11.WebSocketSignallingClient.impl");
           },
           "Opaque capsule around the native implementation, for interop.");

  py::class_<net::WebSocketSignallingServerOptions>(
      module, "WebSocketSignallingServerOptions")
      .def(py::init<>(),
           "Construct default WebSocket signalling server options.")
      .def_readwrite("path_prefix",
                     &net::WebSocketSignallingServerOptions::path_prefix,
                     "URL path prefix the server listens on.")
      .def_readwrite("port", &net::WebSocketSignallingServerOptions::port,
                     "TCP port to listen on (0 selects an ephemeral port).")
      .def_readwrite("bind_address",
                     &net::WebSocketSignallingServerOptions::bind_address,
                     "Local address to bind the listening socket to.")
      .def_readwrite("http2_options",
                     &net::WebSocketSignallingServerOptions::http2_options,
                     "HTTP/2 transport options used for the server.")
      .def_property(
          "enable_tls",
          [](const net::WebSocketSignallingServerOptions& options) {
            return options.http2_options.tls.enabled;
          },
          [](net::WebSocketSignallingServerOptions& options, bool value) {
            options.http2_options.tls.enabled = value;
          },
          "Whether TLS is enabled for the server transport.")
      .def_readwrite("max_message_size",
                     &net::WebSocketSignallingServerOptions::max_message_size,
                     "Maximum inbound signalling message size in bytes.")
      .def("validate",
           [](const net::WebSocketSignallingServerOptions& options) {
             ThrowIfNotOk(options.Validate());
           },
           "Raise if the options are invalid.");

  py::class_<net::WebSocketSignallingServer,
             std::shared_ptr<net::WebSocketSignallingServer>>(
      module, "WebSocketSignallingServer")
      .def_static(
          "create",
          [](std::shared_ptr<net::SignallingService> service,
             net::WebSocketSignallingServerOptions options) {
            // Create() blocks on the libuv loop (Http2Server::Create ->
            // RunOnUv), so release the GIL while it runs.
            return ValueWithoutGil([&] {
              return net::WebSocketSignallingServer::Create(
                  std::move(service), std::move(options));
            });
          },
          "Create a WebSocket signalling server that fronts the given "
          "in-process signalling service.",
          py::arg("service"),
          py::arg("options") = net::WebSocketSignallingServerOptions{})
      .def("stop",
           [](net::WebSocketSignallingServer& self) {
             CallWithoutGil([&self] { return self.Stop(); });
           },
           "Stop the server and close all client connections.")
      .def_property_readonly("port", &net::WebSocketSignallingServer::port,
                             "Port the server is listening on.")
      .def_property_readonly("running",
                             &net::WebSocketSignallingServer::running,
                             "Whether the server is currently running.")
      .def_property_readonly("service",
                             &net::WebSocketSignallingServer::service,
                             "Signalling service this server fronts.")
      .def("get_impl",
           [](const net::WebSocketSignallingServer& self) {
             return PointerCapsule(self.GetImpl(),
                                   "a11.WebSocketSignallingServer.impl");
           },
           "Opaque capsule around the native implementation, for interop.");
}

}  // namespace a11::python

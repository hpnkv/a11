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
#include <pybind11/typing.h>
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
#include "python/native_types.h"
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
    if (!loop.ok()) {
      return loop.status();
    }

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
    // Queued rather than released here: a destructor may run on a pool
    // worker, and taking the GIL there races interpreter finalization.
    // See DeferredPythonRefs.
    DeferredPythonRefs::Retire(std::exchange(callable_, nullptr));
  }

  template <typename... Args>
  [[nodiscard]] a11::Task Call(Args&&... args) const {
    py::gil_scoped_acquire acquire;
    auto callable = py::reinterpret_borrow<py::function>(callable_);
    return CallPythonAsync<a11::Unit>(loop_, callable,
                                      std::forward<Args>(args)...);
  }

 private:
  PythonSignallingCallback(PyObject* callable, std::shared_ptr<PythonLoop> loop)
      : callable_(callable), loop_(std::move(loop)) {}

  PyObject* callable_ = nullptr;
  std::shared_ptr<PythonLoop> loop_;
};

/**
 * A Python callable invoked *synchronously*, from whichever thread calls it.
 *
 * The signalling server's per-message hooks are synchronous on purpose --
 * an asynchronous filter would reorder a connection's messages -- so this
 * takes the GIL, calls, and turns whatever happened into a Status. The
 * callable must not block: it runs on a transport thread, ahead of the
 * message it is deciding about.
 */
class PythonSyncCallback {
 public:
  static absl::StatusOr<std::shared_ptr<PythonSyncCallback>> Create(
      const py::object& callable, const char* name) {
    if (PyCallable_Check(callable.ptr()) == 0) {
      return absl::InvalidArgumentError(std::string(name) +
                                        " must be callable");
    }

    struct MakeSharedEnabler final : PythonSyncCallback {
      explicit MakeSharedEnabler(PyObject* value) : PythonSyncCallback(value) {}
    };

    return std::make_shared<MakeSharedEnabler>(callable.inc_ref().ptr());
  }

  PythonSyncCallback(const PythonSyncCallback&) = delete;
  PythonSyncCallback& operator=(const PythonSyncCallback&) = delete;

  ~PythonSyncCallback() {
    DeferredPythonRefs::Retire(std::exchange(callable_, nullptr));
  }

  /// Calls the callable and returns OK, or the status its exception carried.
  template <typename... Args>
  [[nodiscard]] absl::Status Call(Args&&... args) const {
    py::gil_scoped_acquire acquire;
    DeferredPythonRefs::Drain();
    try {
      auto callable = py::reinterpret_borrow<py::function>(callable_);
      callable(std::forward<Args>(args)...);
      return absl::OkStatus();
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    } catch (const std::exception& error) {
      return absl::UnknownError(error.what());
    } catch (...) {
      return absl::UnknownError("A signalling hook raised an exception");
    }
  }

 private:
  explicit PythonSyncCallback(PyObject* callable) : callable_(callable) {}

  PyObject* callable_ = nullptr;
};

net::OnSignallingMessage MakeSignallingCallback(
    const std::shared_ptr<PythonSignallingCallback>& callback) {
  return [callback](net::SignallingMessage message) {
    return callback->Call(std::move(message));
  };
}

class PySignallingTransport : public net::SignallingTransport,
                              public py::trampoline_self_life_support {
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
      if (!read_status_.ok()) {
        return read_status_;
      }
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
    if (status.ok()) {
      return;
    }
    thread::MutexLock lock(&mu_);
    if (read_status_.ok()) {
      read_status_ = std::move(status);
    }
  }

  mutable thread::Mutex mu_;
  mutable absl::Status read_status_;
};

py::typing::Optional<py::capsule> PointerCapsule(void* pointer,
                                                 const char* name) {
  if (pointer == nullptr) {
    return py::typing::Optional<py::capsule>(py::none());
  }
  return py::capsule(pointer, name);
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
             if (!error.is_none()) {
               result.error = StatusFromPython(error);
             }
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
                     R"(SDP description type, e.g. "offer" or "answer".)")
      .def_readwrite("candidate", &net::SignallingMessage::candidate,
                     "ICE candidate string (for CANDIDATE messages).")
      .def_readwrite("mid", &net::SignallingMessage::mid,
                     "Media stream identifier associated with the candidate.")
      .def_property(
          "error",
          [](const net::SignallingMessage& message) -> NativeStatus {
            return NativeStatus(message.error);
          },
          [](net::SignallingMessage& message,
             const PyLike<NativeStatus>& status) {
            message.error = StatusFromPython(status);
          },
          "Error status carried by ERROR messages, or OK otherwise.")
      .def(
          "validate",
          [](const net::SignallingMessage& message) {
            ThrowIfNotOk(message.Validate());
          },
          "Raise if the message fields are inconsistent for its type.")
      .def(
          "to_json",
          [](const net::SignallingMessage& message) {
            return ValueOrThrow(message.ToJson());
          },
          "Serialize this message to its JSON wire representation.")
      .def_static(
          "from_json",
          [](const std::string& json) {
            return ValueOrThrow(net::SignallingMessage::FromJson(json));
          },
          "Parse a signalling message from its JSON wire "
          "representation.",
          py::arg("json"));

  py::classh<net::SignallingTransport, PySignallingTransport> transport(
      module, "SignallingTransport");
  transport.def(py::init<>(), "Construct a base signalling transport.")
      .def(
          "send",
          [](net::SignallingTransport& self, net::SignallingMessage message) {
            ThrowIfNotOk(self.Send(std::move(message)));
          },
          "Send a signalling message to the peer (non-blocking).",
          py::arg("message"))
      .def(
          "set_on_message",
          [](net::SignallingTransport& self, const py::object& callback) {
            std::shared_ptr<PythonSignallingCallback> owner = ValueOrThrow(
                PythonSignallingCallback::Create(callback, "on_message"));
            ThrowIfNotOk(self.SetOnMessage(MakeSignallingCallback(owner)));
          },
          "Register an async callback invoked for each inbound message.",
          py::arg("callback"))
      .def(
          "close",
          [](net::SignallingTransport& self) { ThrowIfNotOk(self.Close()); },
          "Close the transport and release its resources.")
      .def("identity", &net::SignallingTransport::identity,
           "Return the local identity bound to this transport.")
      .def("connected", &net::SignallingTransport::connected,
           "Return whether the transport is currently connected.")
      .def(
          "get_status",
          [](const net::SignallingTransport& self) -> NativeStatus {
            return NativeStatus(self.GetStatus());
          },
          "Return the current transport status.");

  py::classh<net::SignallingEndpoint, net::SignallingTransport>(
      module, "SignallingEndpoint");

  py::classh<net::SignallingService>(module, "SignallingService")
      .def_static("create", &net::SignallingService::Create,
                  "Create a new in-process signalling service.")
      .def(py::init([]() { return net::SignallingService::Create(); }),
           "Create a new in-process signalling service.")
      .def(
          "connect",
          [](net::SignallingService& self, std::string identity,
             const py::object& on_message) {
            std::shared_ptr<PythonSignallingCallback> callback = ValueOrThrow(
                PythonSignallingCallback::Create(on_message, "on_message"));
            return ValueOrThrow(self.Connect(std::move(identity),
                                             MakeSignallingCallback(callback)));
          },
          "Register an identity and its async inbound-message callback, "
          "returning a signalling endpoint.",
          py::arg("identity"), py::arg("on_message"))
      .def(
          "deliver",
          [](net::SignallingService& self, net::SignallingMessage message) {
            ThrowIfNotOk(self.Deliver(std::move(message)));
          },
          "Deliver a message to a locally connected recipient, as though it "
          "had been routed from an endpoint of this service. This is the "
          "ingress half of a federated signalling fabric: pair it with "
          "WebSocketSignallingServerOptions.on_unroutable, which is the "
          "egress half, to make several servers behave as one. Raises "
          "NOT_FOUND when the recipient is not connected here.",
          py::arg("message"))
      .def("contains", &net::SignallingService::Contains,
           "Return whether the given identity is currently connected.",
           py::arg("identity"))
      .def("__contains__", &net::SignallingService::Contains,
           "Return whether the given identity is currently connected.",
           py::arg("identity"))
      .def("identities", &net::SignallingService::Identities,
           "Return the list of currently connected identities.")
      .def(
          "stop",
          [](net::SignallingService& self) { ThrowIfNotOk(self.Stop()); },
          "Stop the service and disconnect all endpoints.");

  py::enum_<net::TurnRelayType>(module, "TurnRelayType")
      .value("UDP", net::TurnRelayType::kUdp)
      .value("TCP", net::TurnRelayType::kTcp)
      .value("TLS", net::TurnRelayType::kTls)
      .export_values();

  py::class_<net::TurnServer>(module, "TurnServer")
      .def(py::init<>(), "Construct an empty TURN server configuration.")
      .def_static(
          "from_string",
          [](const std::string& value) {
            return ValueOrThrow(net::TurnServer::FromString(value));
          },
          "Parse a TURN server from a URL-like string.", py::arg("value"))
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
      .def_readwrite(
          "mtu", &net::WebRtcConfiguration::mtu,
          "Network MTU in bytes that SCTP builds packets to; None means 1280. "
          "The largest performance knob this transport has: path MTU discovery "
          "is unavailable, so the default fragments every message into "
          "1172-byte chunks regardless of what the path can carry, and raising "
          "it to 4096 is worth about 3x at 64 KiB (131 -> 368 MiB/s on Linux "
          "loopback). Above roughly 4 KiB, messages that need more than one "
          "chunk silently stop arriving while small ones keep flowing, so set "
          "it only for a peer whose end-to-end path MTU is known -- leave it "
          "None for a browser or an internet peer.")
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
      .def_readwrite(
          "path_mtu_discovery", &net::WebRtcConfiguration::path_mtu_discovery,
          "Whether to discover the path MTU by probing instead of holding "
          "`mtu`. Enabled by default. Discovery raises the association MTU "
          "after padded SCTP heartbeat probes succeed. If larger data packets "
          "are then dropped, the association waits for black-hole detection "
          "before falling back. Set False to pin the MTU for paths that cannot "
          "be characterised reliably, including TURN relays and internet "
          "peers.")
      .def_readwrite(
          "max_discovered_mtu", &net::WebRtcConfiguration::max_discovered_mtu,
          "Ceiling the search may raise the MTU to, in bytes. Defaults to "
          "9216. Lowering it bounds how wrong discovery can be without "
          "switching it off; setting it to `mtu` is another way of pinning.")
      .def_property(
          "probe_timeout",
          [](const net::WebRtcConfiguration& self) -> NativeDuration {
            return NativeDuration(self.probe_timeout);
          },
          [](net::WebRtcConfiguration& self, const py::handle& value) {
            self.probe_timeout = ValueOrThrow(DurationFromPython(value, false));
          },
          "How long one probe has to be acknowledged before it counts as "
          "lost. Defaults to 500ms.")
      .def_property(
          "path_mtu_raise_interval",
          [](const net::WebRtcConfiguration& self) -> NativeDuration {
            return NativeDuration(self.path_mtu_raise_interval);
          },
          [](net::WebRtcConfiguration& self, const py::handle& value) {
            self.path_mtu_raise_interval =
                ValueOrThrow(DurationFromPython(value, false));
          },
          "How long after the search settles before it tries to raise the MTU "
          "again. Defaults to 10 minutes. A long interval is not a substitute "
          "for turning discovery off: it bounds how often the stall can "
          "happen, not whether it can.")
      .def_property(
          "path_mtu_startup_retry",
          [](const net::WebRtcConfiguration& self) -> NativeDuration {
            return NativeDuration(self.path_mtu_startup_retry);
          },
          [](net::WebRtcConfiguration& self, const py::handle& value) {
            self.path_mtu_startup_retry =
                ValueOrThrow(DurationFromPython(value, false));
          },
          "How long to wait before retrying while the SCTP association is not "
          "yet available. Defaults to 250ms.")
      .def(
          "validate",
          [](const net::WebRtcConfiguration& configuration) {
            ThrowIfNotOk(configuration.Validate());
          },
          "Raise if the configuration is invalid.");

  py::classh<net::WebRtcWireStream, net::WireStream>(module, "WebRtcWireStream")
      .def_static(
          "create_client",
          [](std::string identity, std::string peer_identity,
             const std::shared_ptr<net::SignallingService>& signalling,
             net::WebRtcConfiguration configuration,
             net::WireStreamOptions options) {
            return ValueOrThrow(net::WebRtcWireStream::CreateClient(
                std::move(identity), std::move(peer_identity), signalling,
                configuration, options));
          },
          "Open a WebRTC data-channel wire stream to a named peer over a "
          "shared in-process signalling service. It performs the ICE/SDP "
          "handshake and resolves to a WireStream carrying A11-framed "
          "messages, fragmenting large payloads transparently.",
          py::arg("identity"), py::arg("peer_identity"), py::arg("signalling"),
          py::arg("configuration") = net::WebRtcConfiguration{},
          py::arg("options") = net::WireStreamOptions{})
      .def_static(
          "create_client",
          [](std::string peer_identity,
             const std::shared_ptr<net::SignallingTransport>& signalling,
             const net::WebRtcConfiguration& configuration,
             net::WireStreamOptions options) {
            return ValueOrThrow(net::WebRtcWireStream::CreateClient(
                std::move(peer_identity), signalling, configuration, options));
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
      .def_property_readonly(
          "data_channel",
          [](const net::WebRtcWireStream& self) {
            return PointerCapsule(self.data_channel().get(),
                                  "a11.WebRtcWireStream.data_channel");
          },
          "Opaque capsule around the underlying "
          "libdatachannel DataChannel. Exposed for advanced "
          "interop and diagnostics; agent code normally "
          "reads and writes through the WireStream API "
          "rather than touching this directly.")
      .def_property_readonly(
          "peer_connection",
          [](const net::WebRtcWireStream& self) {
            return PointerCapsule(self.peer_connection().get(),
                                  "a11.WebRtcWireStream.peer_connection");
          },
          "Opaque capsule around the underlying "
          "libdatachannel PeerConnection, for "
          "inspecting ICE/connection state during "
          "debugging.")
      .def_property_readonly(
          "signalling_endpoint", &net::WebRtcWireStream::signalling_endpoint,
          "Signalling transport this stream negotiated over, the channel that "
          "carried the asynchronous SDP/ICE handshake.");

  py::classh<net::WebRtcWireServer>(module, "WebRtcWireServer")
      .def_static(
          "create",
          [](std::string identity,
             const std::shared_ptr<net::SignallingService>& signalling,
             const py::object& on_stream,
             net::WebRtcConfiguration configuration,
             net::WireStreamOptions stream_options) {
            std::shared_ptr<PythonSignallingCallback> callback = ValueOrThrow(
                PythonSignallingCallback::Create(on_stream, "on_stream"));
            return ValueOrThrow(net::WebRtcWireServer::Create(
                std::move(identity), signalling,
                [callback = std::move(callback)](
                    const std::shared_ptr<net::WebRtcWireStream>& stream) {
                  return callback->Call(stream);
                },
                std::move(configuration), stream_options));
          },
          "Create a WebRTC server that accepts peer connections and invokes "
          "the async on_stream callback with each new WebRtcWireStream, "
          "negotiating over a signalling service shared within this process.",
          py::arg("identity"), py::arg("signalling"), py::arg("on_stream"),
          py::arg("configuration") = net::WebRtcConfiguration{},
          py::arg("stream_options") = net::WireStreamOptions{})
      .def_static(
          "create",
          [](const std::shared_ptr<net::SignallingTransport>& signalling,
             const py::object& on_stream,
             net::WebRtcConfiguration configuration,
             net::WireStreamOptions stream_options) {
            std::shared_ptr<PythonSignallingCallback> callback = ValueOrThrow(
                PythonSignallingCallback::Create(on_stream, "on_stream"));
            return ValueOrThrow(net::WebRtcWireServer::Create(
                signalling,
                [callback = std::move(callback)](
                    const std::shared_ptr<net::WebRtcWireStream>& stream) {
                  return callback->Call(stream);
                },
                std::move(configuration), stream_options));
          },
          "Create a WebRTC server that accepts peer connections over an "
          "explicit signalling transport (e.g. a WebSocket signalling client). "
          "Prefer this overload when peers reach this agent through a remote "
          "signalling server rather than a service in this process: the server "
          "listens as the identity the transport registered under, takes over "
          "its message callback, and closes it when stopped.",
          py::arg("signalling"), py::arg("on_stream"),
          py::arg("configuration") = net::WebRtcConfiguration{},
          py::arg("stream_options") = net::WireStreamOptions{})
      .def(
          "stop",
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
          "headers",
          [](const net::WebSocketSignallingClientOptions& options)
              -> PyHeaderPairs { return HeaderPairsToPython(options.headers); },
          [](net::WebSocketSignallingClientOptions& options,
             const PyHeadersLike& value) {
            net::HttpHeaders headers = ValueOrThrow(
                HeaderPairsFromPython(value, "Signalling handshake headers"));
            ThrowIfNotOk(net::ValidateHttpHeaders(headers));
            options.headers = std::move(headers);
          },
          "Extra HTTP headers sent on the signalling handshake, as a mapping "
          "or a list of (name, value) pairs. This is how a client presents "
          "credentials to a signalling server that authenticates; without it "
          "the only place to put one is the URL's query string, where it ends "
          "up in logs.")
      .def_property(
          "deadline",
          [](const net::WebSocketSignallingClientOptions& options)
              -> NativeTime { return NativeTime(options.deadline); },
          [](net::WebSocketSignallingClientOptions& options,
             const py::typing::Optional<NativeTime>& deadline) {
            options.deadline = ValueOrThrow(TimeFromPython(deadline));
          },
          "Deadline by which the connection must be established.")
      .def(
          "validate",
          [](const net::WebSocketSignallingClientOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Raise if the options are invalid.");

  py::classh<net::WebSocketSignallingClient, net::SignallingTransport>(
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
      .def(
          "get_impl",
          [](const net::WebSocketSignallingClient& self) {
            return PointerCapsule(self.GetImpl(),
                                  "a11.WebSocketSignallingClient.impl");
          },
          "Opaque capsule around the native implementation, for interop.");

  py::class_<net::SignallingAdmission>(module, "SignallingAdmission")
      .def_readonly("identity", &net::SignallingAdmission::identity,
                    "Identity the peer is asking to register under.")
      .def_readonly("path", &net::SignallingAdmission::path,
                    "Full request path, query string included.")
      .def_readonly("query", &net::SignallingAdmission::query,
                    "The part of the path after '?', without it.")
      .def_property_readonly(
          "headers",
          [](const net::SignallingAdmission& admission) -> PyHeaderPairs {
            return HeaderPairsToPython(admission.headers);
          },
          "Request headers as sent, as a list of (name, value) pairs.")
      .def("__repr__", [](const net::SignallingAdmission& admission) {
        return "<SignallingAdmission identity=" + admission.identity + ">";
      });

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
      .def_readwrite(
          "replace_existing",
          &net::WebSocketSignallingServerOptions::replace_existing,
          "Whether a new registration displaces a live one for the same "
          "identity. Off by default, which answers ALREADY_EXISTS. Set it "
          "when on_admit already decides which of two claimants is "
          "legitimate; without it a host that restarted cannot take its own "
          "identity back until the socket its dead predecessor left behind "
          "is noticed.")
      .def_property(
          "on_admit",
          [](const net::WebSocketSignallingServerOptions&) {
            return py::none();
          },
          [](net::WebSocketSignallingServerOptions& options,
             const py::object& callable) {
            if (callable.is_none()) {
              options.on_admit = nullptr;
              return;
            }
            auto callback = ValueOrThrow(
                PythonSignallingCallback::Create(callable, "on_admit"));
            options.on_admit = [callback](net::SignallingAdmission admission) {
              return callback->Call(std::move(admission));
            };
          },
          "Async callable deciding whether a peer may register, given a "
          "SignallingAdmission. Raise a StatusException to refuse: its code "
          "becomes the HTTP status of the refused upgrade, so the peer is "
          "told why instead of getting a socket that closes immediately. "
          "Runs once per connection, before the WebSocket upgrade.")
      .def_property(
          "on_departed",
          [](const net::WebSocketSignallingServerOptions&) {
            return py::none();
          },
          [](net::WebSocketSignallingServerOptions& options,
             const py::object& callable) {
            if (callable.is_none()) {
              options.on_departed = nullptr;
              return;
            }
            auto callback = ValueOrThrow(
                PythonSyncCallback::Create(callable, "on_departed"));
            options.on_departed = [callback](std::string identity) {
              const absl::Status status = callback->Call(std::move(identity));
              if (!status.ok()) {
                LOG(ERROR) << "A signalling on_departed hook failed: "
                           << status;
              }
            };
          },
          "Synchronous callable invoked with an identity whose connection has "
          "gone, for presence bookkeeping. Called from a transport thread, so "
          "it must not block; marshal anything slow onto your own loop.")
      .def_property(
          "on_message",
          [](const net::WebSocketSignallingServerOptions&) {
            return py::none();
          },
          [](net::WebSocketSignallingServerOptions& options,
             const py::object& callable) {
            if (callable.is_none()) {
              options.on_message = nullptr;
              return;
            }
            auto callback = ValueOrThrow(
                PythonSyncCallback::Create(callable, "on_message"));
            options.on_message =
                [callback](net::SignallingMessage* absl_nonnull message) {
                  // The pointer is handed over as a pointer and converted
                  // inside Call, which is where the GIL is held.
                  return callback->Call(message);
                };
          },
          "Synchronous callable invoked with each inbound SignallingMessage "
          "before it is routed. Mutating the message changes what is routed; "
          "raising a StatusException refuses that one message, which is "
          "reported to its sender as an error message and leaves the "
          "connection open. Synchronous because signalling is ordered per "
          "connection and an async hook would reorder it.")
      .def_property(
          "on_unroutable",
          [](const net::WebSocketSignallingServerOptions&) {
            return py::none();
          },
          [](net::WebSocketSignallingServerOptions& options,
             const py::object& callable) {
            if (callable.is_none()) {
              options.on_unroutable = nullptr;
              return;
            }
            auto callback = ValueOrThrow(
                PythonSyncCallback::Create(callable, "on_unroutable"));
            options.on_unroutable =
                [callback](const net::SignallingMessage& message) {
                  return callback->Call(message);
                };
          },
          "Synchronous callable offered each message whose recipient is not "
          "connected to this server. Return normally once it has been handed "
          "to whatever will carry it elsewhere -- the other half is "
          "SignallingService.deliver on the instance that holds the "
          "recipient -- or raise to say it is undeliverable, which its "
          "sender is told.")
      .def(
          "validate",
          [](const net::WebSocketSignallingServerOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Raise if the options are invalid.");

  py::classh<net::WebSocketSignallingServer>(module,
                                             "WebSocketSignallingServer")
      .def_static(
          "create",
          [](std::shared_ptr<net::SignallingService> service,
             net::WebSocketSignallingServerOptions options) {
            // Create() blocks on the libuv loop (Http2Server::Create ->
            // RunOnUv), so release the GIL while it runs.
            return ValueWithoutGil([&] {
              return net::WebSocketSignallingServer::Create(std::move(service),
                                                            std::move(options));
            });
          },
          "Create a WebSocket signalling server that fronts the given "
          "in-process signalling service.",
          py::arg("service"),
          py::arg("options") = net::WebSocketSignallingServerOptions{})
      .def(
          "stop",
          [](net::WebSocketSignallingServer& self) {
            CallWithoutGil([&self] { return self.Stop(); });
          },
          "Stop the server and close all client connections.")
      .def(
          "disconnect",
          [](net::WebSocketSignallingServer& self,
             const std::string& identity) {
            CallWithoutGil(
                [&self, &identity] { return self.Disconnect(identity); });
          },
          "Close one identity's connection, if this server holds it. The "
          "counterpart to admission: whatever authorised a registration can "
          "be withdrawn, and the socket has to go with it rather than "
          "surviving until its next message. Raises NOT_FOUND when this "
          "server is not holding that identity.",
          py::arg("identity"))
      .def_property_readonly("port", &net::WebSocketSignallingServer::port,
                             "Port the server is listening on.")
      .def_property_readonly("running",
                             &net::WebSocketSignallingServer::running,
                             "Whether the server is currently running.")
      .def_property_readonly("service",
                             &net::WebSocketSignallingServer::service,
                             "Signalling service this server fronts.")
      .def(
          "get_impl",
          [](const net::WebSocketSignallingServer& self) {
            return PointerCapsule(self.GetImpl(),
                                  "a11.WebSocketSignallingServer.impl");
          },
          "Opaque capsule around the native implementation, for interop.");
}

}  // namespace a11::python

// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <Python.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11_abseil/status_caster.h>    // IWYU pragma: keep
#include <pybind11_abseil/statusor_caster.h>  // IWYU pragma: keep

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"
#include "a11/net/http_sse_wire_stream.h"
#include "a11/net/wire_stream.h"
#include "a11/status.h"
#include "python/bindings.h"
#include "python/casters.h"
#include "python/interop.h"

namespace a11::python {
namespace {

absl::StatusOr<net::HttpHeaders> HttpHeadersFromPython(
    const py::handle& value) {
  if (value.is_none()) {
    return net::HttpHeaders{};
  }

  try {
    py::object entries;
    if (PyMapping_Check(value.ptr()) != 0 && py::hasattr(value, "items")) {
      entries = py::reinterpret_borrow<py::object>(value).attr("items")();
    } else {
      entries = py::reinterpret_borrow<py::object>(value);
    }

    net::HttpHeaders result;
    for (const py::handle item : entries) {
      py::sequence pair = py::reinterpret_borrow<py::sequence>(item);
      if (pair.size() != 2 || !py::isinstance<py::str>(pair[0]) ||
          !py::isinstance<py::str>(pair[1])) {
        return absl::InvalidArgumentError(
            "HTTP headers must contain pairs of strings");
      }
      result.emplace_back(pair[0].cast<std::string>(),
                          pair[1].cast<std::string>());
    }
    net::NormalizeHttpHeaders(&result);
    ABSL_RETURN_IF_ERROR(net::ValidateHttpHeaders(result));
    return result;
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  } catch (...) {
    return absl::InvalidArgumentError("invalid HTTP headers");
  }
}

py::list HttpHeadersToPython(const net::HttpHeaders& headers) {
  py::list result;
  for (const auto& [name, value] : headers) {
    result.append(py::make_tuple(name, value));
  }
  return result;
}

absl::StatusOr<std::string> HttpBodyFromPython(const py::handle& value) {
  try {
    if (value.is_none()) {
      return std::string();
    }
    if (py::isinstance<py::bytes>(value)) {
      return value.cast<std::string>();
    }
    if (py::isinstance<py::str>(value)) {
      return value.cast<std::string>();
    }
    return absl::InvalidArgumentError("HTTP body must be bytes or str");
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}

class PythonHttpCallback {
 public:
  static absl::StatusOr<std::shared_ptr<PythonHttpCallback>> Create(
      const py::object& callable, const char* name) {
    if (PyCallable_Check(callable.ptr()) == 0) {
      return absl::InvalidArgumentError(std::string(name) +
                                        " must be callable");
    }
    ABSL_ASSIGN_OR_RETURN(std::shared_ptr<PythonLoop> loop,
                          PythonLoop::Capture());

    struct MakeSharedEnabler final : PythonHttpCallback {
      MakeSharedEnabler(PyObject* callable, std::shared_ptr<PythonLoop> loop)
          : PythonHttpCallback(callable, std::move(loop)) {}
    };

    return std::make_shared<MakeSharedEnabler>(callable.inc_ref().ptr(),
                                               std::move(loop));
  }

  PythonHttpCallback(const PythonHttpCallback&) = delete;
  PythonHttpCallback& operator=(const PythonHttpCallback&) = delete;

  ~PythonHttpCallback() {
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
  PythonHttpCallback(PyObject* callable, std::shared_ptr<PythonLoop> loop)
      : callable_(callable), loop_(std::move(loop)) {}

  PyObject* callable_ = nullptr;
  std::shared_ptr<PythonLoop> loop_;
};

void* CheckedImpl(void* pointer) {
  return pointer;
}

py::object ImplCapsule(void* pointer, const char* name) {
  if (CheckedImpl(pointer) == nullptr) {
    return py::none();
  }
  return py::capsule(pointer, name);
}

void ThrowIfNotOk(const absl::Status& status) {
  if (!status.ok()) {
    ThrowStatus(status);
  }
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

// Like CallWithoutGil, but for a blocking operation that yields an
// absl::StatusOr<T>: the GIL is released while it runs, then the value is
// unwrapped (or thrown) with the GIL re-held. Releasing is essential for calls
// that block on the libuv loop (RunOnUv/RunStatusOnUv -> Future::Await): the uv
// thread completes work by touching Python objects (FutureToPython callbacks,
// py::object destructors) and must be able to take the GIL, so a caller that
// blocked while holding it would deadlock the loop. Any argument conversion that
// touches Python must happen before calling this, while the GIL is still held.
template <typename Operation>
auto ValueWithoutGil(Operation&& operation) {
  auto result = [&] {
    py::gil_scoped_release release;
    return std::forward<Operation>(operation)();
  }();
  return ValueOrThrow(std::move(result));
}

}  // namespace

void BindHttp(py::module_& module) {
  py::classh<net::Http2RequestBodyStream>(module, "Http2RequestBodyStream")
      .def(
          "read",
          [](const std::shared_ptr<net::Http2RequestBodyStream>& self) {
            return FutureToPythonConverted(
                self->Read(),
                [](const std::optional<std::string>& value) -> py::object {
                  if (!value.has_value()) {
                    return py::none();
                  }
                  return py::bytes(*value);
                });
          },
          "Await the next chunk of request body data, or None at end of "
          "stream.")
      .def(
          "wait_done",
          [](const std::shared_ptr<net::Http2RequestBodyStream>& self) {
            return FutureToPython(self->Done());
          },
          "Await completion of the request body stream.")
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2RequestBodyStream>& self) {
            return FutureToPython(self->Done());
          },
          "Future that completes when the request body stream is done.")
      .def(
          "cancel",
          [](net::Http2RequestBodyStream& self, const py::object& status) {
            std::optional<absl::Status> converted;
            if (!status.is_none()) {
              converted = StatusFromPython(status);
            }
            CallWithoutGil([&self, converted = std::move(converted)] {
              return converted.has_value() ? self.Cancel(*converted)
                                           : self.Cancel();
            });
          },
          "Cancel the request body stream with an optional status.",
          py::arg("status") = py::none())
      .def_property_readonly("stream_id",
                             &net::Http2RequestBodyStream::stream_id,
                             "The HTTP/2 stream identifier.");

  py::class_<net::HttpRequest>(module, "HttpRequest")
      .def(py::init([](std::string_view method, std::string_view scheme,
                       std::string_view authority, std::string_view path,
                       const py::object& headers, const py::object& body) {
             return net::HttpRequest{
                 .method = std::string(method),
                 .protocol = {},
                 .scheme = std::string(scheme),
                 .authority = std::string(authority),
                 .path = std::string(path),
                 .headers = ValueOrThrow(HttpHeadersFromPython(headers)),
                 .body = ValueOrThrow(HttpBodyFromPython(body)),
                 .body_stream = nullptr};
           }),
           "Construct an HTTP request.", py::arg("method") = "GET",
           py::arg("scheme") = "http", py::arg("authority") = "",
           py::arg("path") = "/", py::arg("headers") = py::none(),
           py::arg("body") = py::bytes())
      .def_readwrite("method", &net::HttpRequest::method,
                     "The HTTP request method (e.g. GET, POST).")
      .def_readwrite("protocol", &net::HttpRequest::protocol,
                     "The extended CONNECT protocol, if any.")
      .def_readwrite("scheme", &net::HttpRequest::scheme,
                     "The URI scheme (e.g. http, https).")
      .def_readwrite("authority", &net::HttpRequest::authority,
                     "The request authority (host and optional port).")
      .def_readwrite("path", &net::HttpRequest::path,
                     "The request target path.")
      .def_readonly("body_stream", &net::HttpRequest::body_stream,
                    "Pull-oriented request body stream, present only for "
                    "requests that remain open after their headers.")
      .def_property(
          "headers",
          [](const net::HttpRequest& request) {
            return HttpHeadersToPython(request.headers);
          },
          [](net::HttpRequest& request,
             const py::object& headers) -> absl::Status {
            ABSL_ASSIGN_OR_RETURN(request.headers,
                                  HttpHeadersFromPython(headers));
            return absl::OkStatus();
          },
          "The request headers as a list of (name, value) pairs.")
      .def_property(
          "body",
          [](const net::HttpRequest& request) {
            return py::bytes(request.body);
          },
          [](net::HttpRequest& request,
             const py::object& body) -> absl::Status {
            ABSL_ASSIGN_OR_RETURN(request.body, HttpBodyFromPython(body));
            return absl::OkStatus();
          },
          "The request body as bytes.");

  py::class_<net::HttpResponseHead>(module, "HttpResponseHead")
      .def(py::init([](int status, const py::object& headers) {
             return net::HttpResponseHead{
                 .status = status,
                 .headers = ValueOrThrow(HttpHeadersFromPython(headers))};
           }),
           "Construct an HTTP response head (status and headers).",
           py::arg("status") = 0, py::arg("headers") = py::none())
      .def_readwrite("status", &net::HttpResponseHead::status,
                     "The HTTP status code.")
      .def_property(
          "headers",
          [](const net::HttpResponseHead& response) {
            return HttpHeadersToPython(response.headers);
          },
          [](net::HttpResponseHead& response, const py::object& headers) {
            response.headers = ValueOrThrow(HttpHeadersFromPython(headers));
          },
          "The response headers as a list of (name, value) pairs.");

  py::class_<net::HttpResponse>(module, "HttpResponse")
      .def(py::init([](net::HttpResponseHead head, const py::object& body) {
             return net::HttpResponse{
                 .head = std::move(head),
                 .body = ValueOrThrow(HttpBodyFromPython(body))};
           }),
           "Construct an HTTP response from a head and body.",
           py::arg("head") = net::HttpResponseHead{},
           py::arg("body") = py::bytes())
      .def_readwrite("head", &net::HttpResponse::head,
                     "The response head (status and headers).")
      .def_property(
          "body",
          [](const net::HttpResponse& response) {
            return py::bytes(response.body);
          },
          [](net::HttpResponse& response, const py::object& body) {
            response.body = ValueOrThrow(HttpBodyFromPython(body));
          },
          "The response body as bytes.");

  py::class_<net::Http2TlsOptions>(module, "Http2TlsOptions")
      .def(py::init<>(), "Construct default HTTP/2 TLS options.")
      .def_readwrite("enabled", &net::Http2TlsOptions::enabled,
                     "Whether TLS is enabled.")
      .def_readwrite("verify_peer", &net::Http2TlsOptions::verify_peer,
                     "Whether to verify the peer certificate.")
      .def_readwrite("certificate_pem_file",
                     &net::Http2TlsOptions::certificate_pem_file,
                     "Path to the PEM certificate file.")
      .def_readwrite("key_pem_file", &net::Http2TlsOptions::key_pem_file,
                     "Path to the PEM private key file.")
      .def_readwrite("ca_certificate_pem_file",
                     &net::Http2TlsOptions::ca_certificate_pem_file,
                     "Path to the PEM CA certificate file.")
      .def(
          "validate",
          [](const net::Http2TlsOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the TLS options, raising on error.");

  py::class_<net::Http2Options>(module, "Http2Options")
      .def(py::init<>(), "Construct default HTTP/2 options.")
      .def_readwrite("max_request_body_size",
                     &net::Http2Options::max_request_body_size,
                     "Maximum accepted request body size in bytes.")
      .def_readwrite("max_response_body_size",
                     &net::Http2Options::max_response_body_size,
                     "Maximum accepted response body size in bytes.")
      .def_readwrite("max_buffered_request_bytes",
                     &net::Http2Options::max_buffered_request_bytes,
                     "Maximum buffered request bytes before backpressure.")
      .def_readwrite("max_buffered_response_bytes",
                     &net::Http2Options::max_buffered_response_bytes,
                     "Maximum buffered response bytes before backpressure.")
      .def_readwrite("tls", &net::Http2Options::tls, "The TLS options.")
      .def_property(
          "deadline",
          [](const net::Http2Options& options) {
            return TimeToPython(options.deadline);
          },
          [](net::Http2Options& options, const py::object& deadline) {
            options.deadline = ValueOrThrow(TimeFromPython(deadline));
          },
          "The operation deadline.")
      .def(
          "validate",
          [](const net::Http2Options& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the options, raising on error.");

  py::classh<net::Http2ResponseStream>(module, "Http2ResponseStream")
      .def(
          "headers",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPython(self->Headers());
          },
          "Await the response head (status and headers).")
      .def(
          "read",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPythonConverted(
                self->Read(),
                [](const std::optional<std::string>& value) -> py::object {
                  if (!value.has_value()) {
                    return py::none();
                  }
                  return py::bytes(*value);
                });
          },
          "Await the next chunk of response body data, or None at end of "
          "stream.")
      .def(
          "wait_done",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPython(self->Done());
          },
          "Await completion of the response stream.")
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPython(self->Done());
          },
          "Future that completes when the response stream is done.")
      .def(
          "cancel",
          [](net::Http2ResponseStream& self, const py::object& status) {
            std::optional<absl::Status> converted;
            if (!status.is_none()) {
              converted = StatusFromPython(status);
            }
            CallWithoutGil([&self, converted = std::move(converted)] {
              return converted.has_value() ? self.Cancel(*converted)
                                           : self.Cancel();
            });
          },
          "Cancel the response stream with an optional status.",
          py::arg("status") = py::none())
      .def_property_readonly("stream_id", &net::Http2ResponseStream::stream_id,
                             "The HTTP/2 stream identifier.");

  py::classh<net::Http2DuplexStream>(module, "Http2DuplexStream")
      .def(
          "headers",
          [](const std::shared_ptr<net::Http2DuplexStream>& self) {
            return FutureToPython(self->Headers());
          },
          "Await the response head (status and headers).")
      .def(
          "read",
          [](const std::shared_ptr<net::Http2DuplexStream>& self) {
            return FutureToPythonConverted(
                self->Read(),
                [](const std::optional<std::string>& value) -> py::object {
                  if (!value.has_value()) {
                    return py::none();
                  }
                  return py::bytes(*value);
                });
          },
          "Await the next chunk of response data, or None at end of stream.")
      .def(
          "write",
          [](net::Http2DuplexStream& self, const py::object& data) {
            std::string converted = ValueOrThrow(HttpBodyFromPython(data));
            CallWithoutGil([&self, converted = std::move(converted)]() mutable {
              return self.Write(std::move(converted));
            });
          },
          "Write a chunk of request data to the duplex stream.",
          py::arg("data"))
      .def(
          "finish",
          [](net::Http2DuplexStream& self) {
            CallWithoutGil([&self] { return self.Finish(); });
          },
          "Signal the end of the request side of the duplex stream.")
      .def(
          "abort",
          [](net::Http2DuplexStream& self, const py::object& status) {
            std::optional<absl::Status> converted;
            if (!status.is_none()) {
              converted = StatusFromPython(status);
            }
            CallWithoutGil([&self, converted = std::move(converted)] {
              return converted.has_value() ? self.Abort(*converted)
                                           : self.Abort();
            });
          },
          "Abort the duplex stream with an optional status.",
          py::arg("status") = py::none())
      .def(
          "wait_done",
          [](const std::shared_ptr<net::Http2DuplexStream>& self) {
            return FutureToPython(self->Done());
          },
          "Await completion of the duplex stream.")
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2DuplexStream>& self) {
            return FutureToPython(self->Done());
          },
          "Future that completes when the duplex stream is done.")
      .def_property_readonly("stream_id", &net::Http2DuplexStream::stream_id,
                             "The HTTP/2 stream identifier.");

  py::classh<net::Http2ResponseWriter>(module, "Http2ResponseWriter")
      .def(
          "send_headers",
          [](net::Http2ResponseWriter& self, int status,
             const py::object& headers) {
            net::HttpHeaders converted =
                ValueOrThrow(HttpHeadersFromPython(headers));
            CallWithoutGil(
                [&self, status, converted = std::move(converted)]() mutable {
                  return self.SendHeaders(status, std::move(converted));
                });
          },
          "Send the response status and headers.", py::arg("status"),
          py::arg("headers") = py::none())
      .def(
          "write",
          [](net::Http2ResponseWriter& self, const py::object& data) {
            std::string converted = ValueOrThrow(HttpBodyFromPython(data));
            CallWithoutGil([&self, converted = std::move(converted)]() mutable {
              return self.Write(std::move(converted));
            });
          },
          "Write a chunk of response body data.", py::arg("data"))
      .def(
          "finish",
          [](net::Http2ResponseWriter& self) {
            CallWithoutGil([&self] { return self.Finish(); });
          },
          "Signal the end of the response body.")
      .def(
          "send_response",
          [](net::Http2ResponseWriter& self, int status,
             const py::object& headers, const py::object& body) {
            net::HttpHeaders converted_headers =
                ValueOrThrow(HttpHeadersFromPython(headers));
            std::string converted_body = ValueOrThrow(HttpBodyFromPython(body));
            CallWithoutGil([&self, status,
                            headers = std::move(converted_headers),
                            body = std::move(converted_body)]() mutable {
              return self.SendResponse(status, std::move(headers),
                                       std::move(body));
            });
          },
          "Send a complete response (status, headers, and body) at once.",
          py::arg("status"), py::arg("headers") = py::none(),
          py::arg("body") = py::bytes())
      .def(
          "abort",
          [](net::Http2ResponseWriter& self, const py::handle& status) {
            absl::Status converted = StatusFromPython(status);
            CallWithoutGil([&self, converted = std::move(converted)]() mutable {
              return self.Abort(std::move(converted));
            });
          },
          "Abort the response with the given status.", py::arg("status"))
      .def(
          "wait_done",
          [](const std::shared_ptr<net::Http2ResponseWriter>& self) {
            return FutureToPython(self->Done());
          },
          "Await completion of the response.")
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2ResponseWriter>& self) {
            return FutureToPython(self->Done());
          },
          "Future that completes when the response is done.")
      .def_property_readonly("headers_sent",
                             &net::Http2ResponseWriter::headers_sent,
                             "Whether the response headers have been sent.")
      .def_property_readonly("finished", &net::Http2ResponseWriter::finished,
                             "Whether the response has been finished.")
      .def_property_readonly("stream_id", &net::Http2ResponseWriter::stream_id,
                             "The HTTP/2 stream identifier.");

  py::classh<net::Http2Server>(module, "Http2Server")
      .def_static(
          "create",
          [](std::string bind_address, std::uint16_t port,
             const py::object& handler, net::Http2Options options) {
            std::shared_ptr<PythonHttpCallback> callback =
                ValueOrThrow(PythonHttpCallback::Create(handler, "handler"));
            return ValueWithoutGil([&] {
              return net::Http2Server::Create(
                  std::move(bind_address), port,
                  [callback = std::move(callback)](
                      net::HttpRequest request,
                      std::shared_ptr<net::Http2ResponseWriter> response) {
                    return callback->Call(std::move(request),
                                          std::move(response));
                  },
                  options);
            });
          },
          "Create and start an HTTP/2 server bound to the given address and "
          "port, dispatching each request to the async handler.",
          py::arg("bind_address") = "127.0.0.1", py::arg("port") = 0,
          py::arg("handler") = py::none(),
          py::arg("options") = net::Http2Options{})
      .def(
          "stop",
          [](net::Http2Server& self) {
            CallWithoutGil([&self] { return self.Stop(); });
          },
          "Stop the server and release its resources.")
      .def_property_readonly("port", &net::Http2Server::port,
                             "The port the server is listening on.")
      .def_property_readonly("bind_address", &net::Http2Server::bind_address,
                             "The address the server is bound to.")
      .def_property_readonly("running", &net::Http2Server::running,
                             "Whether the server is currently running.")
      .def_property_readonly("secure", &net::Http2Server::secure,
                             "Whether the server is using TLS.")
      .def(
          "get_impl",
          [](const net::Http2Server& self) {
            return ImplCapsule(self.GetImpl(), "a11.Http2Server.impl");
          },
          "Return an opaque capsule wrapping the native server handle.");

  py::classh<net::Http2Client>(module, "Http2Client")
      .def_static(
          "connect",
          [](std::string host, std::uint16_t port, net::Http2Options options) {
            return FutureToPython(
                net::Http2Client::Connect(std::move(host), port, options));
          },
          "Asynchronously connect to an HTTP/2 server, returning a future "
          "that resolves to the connected client.",
          py::arg("host"), py::arg("port"),
          py::arg("options") = net::Http2Options{})
      .def(
          "request_stream",
          [](net::Http2Client& self, std::string method, std::string path,
             const py::object& headers, const py::object& body,
             std::string scheme) {
            // Convert Python arguments before releasing the GIL; the call then
            // blocks on the libuv loop and must not hold the GIL (see
            // ValueWithoutGil).
            auto native_headers = ValueOrThrow(HttpHeadersFromPython(headers));
            auto native_body = ValueOrThrow(HttpBodyFromPython(body));
            return ValueWithoutGil([&] {
              return self.RequestStream(
                  std::move(method), std::move(path), std::move(native_headers),
                  std::move(native_body), std::move(scheme));
            });
          },
          "Open a request and return a pull-oriented response stream for "
          "reading the response body incrementally.",
          py::arg("method"), py::arg("path"), py::arg("headers") = py::none(),
          py::arg("body") = py::bytes(), py::arg("scheme") = "")
      .def(
          "request",
          [](net::Http2Client& self, std::string method, std::string path,
             const py::object& headers, const py::object& body,
             std::string scheme) {
            return FutureToPython(self.Request(
                std::move(method), std::move(path),
                ValueOrThrow(HttpHeadersFromPython(headers)),
                ValueOrThrow(HttpBodyFromPython(body)), std::move(scheme)));
          },
          "Send a request and await the full buffered response.",
          py::arg("method"), py::arg("path"), py::arg("headers") = py::none(),
          py::arg("body") = py::bytes(), py::arg("scheme") = "")
      .def(
          "extended_connect",
          [](net::Http2Client& self, std::string protocol, std::string path,
             const py::object& headers, std::string scheme) {
            // Convert headers under the GIL, then block on the libuv loop with
            // the GIL released so the uv thread can take it (see
            // ValueWithoutGil); otherwise the loop deadlocks against this call.
            auto native_headers = ValueOrThrow(HttpHeadersFromPython(headers));
            return ValueWithoutGil([&] {
              return self.ExtendedConnect(std::move(protocol), std::move(path),
                                          std::move(native_headers),
                                          std::move(scheme));
            });
          },
          "Open an extended CONNECT duplex stream for bidirectional data.",
          py::arg("protocol"), py::arg("path"), py::arg("headers") = py::none(),
          py::arg("scheme") = "")
      .def(
          "close",
          [](net::Http2Client& self) {
            CallWithoutGil([&self] { return self.Close(); });
          },
          "Close the client connection.")
      .def_property_readonly("host", &net::Http2Client::host,
                             "The host the client is connected to.")
      .def_property_readonly("port", &net::Http2Client::port,
                             "The port the client is connected to.")
      .def_property_readonly("connected", &net::Http2Client::connected,
                             "Whether the client is currently connected.")
      .def_property_readonly("secure", &net::Http2Client::secure,
                             "Whether the connection is using TLS.")
      .def(
          "get_impl",
          [](const net::Http2Client& self) {
            return ImplCapsule(self.GetImpl(), "a11.Http2Client.impl");
          },
          "Return an opaque capsule wrapping the native client handle.");

  py::class_<net::HttpSseOptions>(module, "HttpSseOptions")
      .def(py::init<>(), "Construct default HTTP SSE wire stream options.")
      .def_readwrite("stream_options", &net::HttpSseOptions::stream_options,
                     "The underlying wire stream options.")
      .def_readwrite("http2_options", &net::HttpSseOptions::http2_options,
                     "The underlying HTTP/2 transport options.")
      .def_readwrite("connect_endpoint", &net::HttpSseOptions::connect_endpoint,
                     "The endpoint path used to open the SSE connection.")
      .def_readwrite("message_endpoint", &net::HttpSseOptions::message_endpoint,
                     "The endpoint path template used to post messages.")
      .def_readwrite(
          "cors_allow_origin", &net::HttpSseOptions::cors_allow_origin,
          "Value for Access-Control-Allow-Origin; empty disables CORS.")
      .def_readwrite("cors_allow_methods",
                     &net::HttpSseOptions::cors_allow_methods,
                     "Value for Access-Control-Allow-Methods.")
      .def_readwrite("cors_allow_headers",
                     &net::HttpSseOptions::cors_allow_headers,
                     "Value for Access-Control-Allow-Headers.")
      .def_readwrite("cors_expose_headers",
                     &net::HttpSseOptions::cors_expose_headers,
                     "Value for Access-Control-Expose-Headers.")
      .def(
          "validate",
          [](const net::HttpSseOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the options, raising on error.");

  py::classh<net::HttpSseWireStream, net::WireStream>(module,
                                                      "HttpSseWireStream")
      .def(
          "get_http_request_headers",
          [](const net::HttpSseWireStream& self) {
            return HttpHeadersToPython(self.GetHttpRequestHeaders());
          },
          "Return the HTTP headers carried on the underlying SSE request. "
          "This is the base class shared by the client and server SSE wire "
          "streams that transport A11 messages over an HTTP/2 Server-Sent "
          "Events connection. Use it when building an agent that needs to "
          "inspect the transport-level request metadata.")
      .def(
          "get_http_response_headers",
          [](const net::HttpSseWireStream& self) -> py::object {
            const std::optional<net::HttpHeaders> headers =
                self.GetHttpResponseHeaders();
            if (!headers.has_value()) {
              return py::none();
            }
            return HttpHeadersToPython(*headers);
          },
          "Return the HTTP response headers negotiated for the SSE "
          "connection, or None if they have not arrived yet. Because the "
          "connection is established asynchronously, prefer awaiting "
          "wait_for_http_headers() before relying on this value.")
      .def(
          "set_http_request_headers",
          [](net::HttpSseWireStream& self, const py::object& headers) {
            ThrowIfNotOk(self.SetHttpRequestHeaders(
                ValueOrThrow(HttpHeadersFromPython(headers))));
          },
          "Set the HTTP headers to send on the underlying SSE request. Call "
          "this before the stream connects to attach auth or routing "
          "metadata that your agent's transport needs.",
          py::arg("headers"))
      .def(
          "set_http_response_headers",
          [](net::HttpSseWireStream& self, const py::object& headers) {
            ThrowIfNotOk(self.SetHttpResponseHeaders(
                ValueOrThrow(HttpHeadersFromPython(headers))));
          },
          "Set the HTTP headers to send on the SSE response. Used on the "
          "server side to attach transport metadata before the streaming "
          "response is flushed to the client.",
          py::arg("headers"))
      .def(
          "wait_for_http_headers",
          [](const std::shared_ptr<net::HttpSseWireStream>& self) {
            return FutureToPython(self->WaitForHttpHeaders());
          },
          "Await the exchange of HTTP headers for the SSE connection. "
          "Because SSE wire streams connect asynchronously, await this "
          "future before reading response headers or assuming the stream is "
          "live.");

  py::classh<net::HttpSseClientWireStream, net::HttpSseWireStream>(
      module, "HttpSseClientWireStream")
      .def(py::init([](std::string url, net::HttpSseOptions options,
                       std::shared_ptr<net::Http2Client> client,
                       const py::object& request_headers) {
             return ValueOrThrow(net::HttpSseClientWireStream::Create(
                 std::move(url), std::move(options), std::move(client),
                 ValueOrThrow(HttpHeadersFromPython(request_headers))));
           }),
           "Construct a client-side SSE wire stream that connects to the "
           "given URL. This is the transport an A11 agent uses to exchange "
           "messages with a remote service over HTTP/2 Server-Sent Events; "
           "the connection is opened lazily and runs asynchronously, so "
           "await the stream's lifecycle futures rather than blocking.",
           py::arg("url"), py::arg("options") = net::HttpSseOptions{},
           py::arg("client") = nullptr, py::arg("request_headers") = py::none())
      .def_static(
          "create",
          [](std::string url, net::HttpSseOptions options,
             std::shared_ptr<net::Http2Client> client,
             const py::object& request_headers) {
            return ValueOrThrow(net::HttpSseClientWireStream::Create(
                std::move(url), std::move(options), std::move(client),
                ValueOrThrow(HttpHeadersFromPython(request_headers))));
          },
          "Create a client-side SSE wire stream connecting to the given URL, "
          "optionally reusing an existing HTTP/2 client. Prefer this factory "
          "when wiring an agent's outbound transport, and drive the returned "
          "stream asynchronously as SSE events arrive.",
          py::arg("url"), py::arg("options") = net::HttpSseOptions{},
          py::arg("client") = nullptr, py::arg("request_headers") = py::none())
      .def_property_readonly(
          "client", &net::HttpSseClientWireStream::client,
          "The underlying HTTP/2 client backing this SSE wire stream, which "
          "you can reuse to multiplex additional streams from the same "
          "agent connection.");

  py::classh<net::HttpSseServerWireStream, net::HttpSseWireStream>(
      module, "HttpSseServerWireStream")
      .def(
          "accepted",
          [](const std::shared_ptr<net::HttpSseServerWireStream>& self) {
            return FutureToPython(self->Accepted());
          },
          "Await acceptance of this server-side SSE wire stream. This is the "
          "server counterpart delivered to your on_connect handler when a "
          "client opens an SSE connection; await this future to know the "
          "stream has been fully established before your agent starts "
          "sending messages on it.");

  py::classh<net::HttpSseServer>(module, "HttpSseServer")
      .def_static(
          "create",
          [](std::string bind_address, std::uint16_t port,
             const py::object& on_connect, net::HttpSseOptions options) {
            net::OnHttpSseConnect callback;
            if (!on_connect.is_none()) {
              std::shared_ptr<PythonHttpCallback> owner = ValueOrThrow(
                  PythonHttpCallback::Create(on_connect, "on_connect"));
              callback =
                  [owner = std::move(owner)](
                      std::shared_ptr<net::HttpSseServerWireStream> stream) {
                    return owner->Call(std::move(stream));
                  };
            }
            return ValueWithoutGil([&] {
              return net::HttpSseServer::Create(std::move(bind_address), port,
                                                std::move(callback),
                                                std::move(options));
            });
          },
          "Create and start an SSE server that accepts A11 wire streams, "
          "invoking the optional async on_connect callback for each client.",
          py::arg("bind_address") = "127.0.0.1", py::arg("port") = 0,
          py::arg("on_connect") = py::none(),
          py::arg("options") = net::HttpSseOptions{})
      .def(
          "wait_for_stream",
          [](const std::shared_ptr<net::HttpSseServer>& self) {
            return FutureToPython(self->WaitForStream());
          },
          "Await the next incoming SSE wire stream from a connecting client.")
      .def(
          "stop",
          [](net::HttpSseServer& self) {
            CallWithoutGil([&self] { return self.Stop(); });
          },
          "Stop the server and release its resources.")
      .def_property_readonly("port", &net::HttpSseServer::port,
                             "The port the server is listening on.")
      .def_property_readonly("running", &net::HttpSseServer::running,
                             "Whether the server is currently running.")
      .def_property_readonly("http2_server", &net::HttpSseServer::http2_server,
                             "The underlying HTTP/2 server.");

  module.attr("HttpSseWireStreamServer") = module.attr("HttpSseServer");
  module.def(
      "get_http_header",
      [](const py::object& headers, std::string name) -> py::object {
        std::optional<std::string> value = net::GetHttpHeader(
            ValueOrThrow(HttpHeadersFromPython(headers)), name);
        if (!value.has_value()) {
          return py::none();
        }
        return py::str(*value);
      },
      "Look up a header value by name, returning None if it is absent.",
      py::arg("headers"), py::arg("name"));
  module.def(
      "validate_http_headers",
      [](const py::object& headers) {
        ThrowIfNotOk(net::ValidateHttpHeaders(
            ValueOrThrow(HttpHeadersFromPython(headers))));
      },
      "Validate a collection of HTTP headers, raising on error.",
      py::arg("headers"));
  module.attr("SSE_STREAM_ID_HEADER") = std::string(net::kSseStreamIdHeader);
  module.attr("SSE_HTTP_HEADER_PREFIX") =
      std::string(net::kSseHttpHeaderPrefix);
  module.attr("DEFAULT_SSE_CONNECT_ENDPOINT") =
      std::string(net::kDefaultSseConnectEndpoint);
  module.attr("DEFAULT_SSE_MESSAGE_ENDPOINT") =
      std::string(net::kDefaultSseMessageEndpoint);
}

}  // namespace a11::python

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
  if (value.is_none())
    return net::HttpHeaders{};

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
    if (value.is_none())
      return std::string();
    if (py::isinstance<py::bytes>(value))
      return value.cast<std::string>();
    if (py::isinstance<py::str>(value))
      return value.cast<std::string>();
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
  PythonHttpCallback(PyObject* callable, std::shared_ptr<PythonLoop> loop)
      : callable_(callable), loop_(std::move(loop)) {}

  PyObject* callable_ = nullptr;
  std::shared_ptr<PythonLoop> loop_;
};

void* CheckedImpl(void* pointer) {
  return pointer;
}

py::object ImplCapsule(void* pointer, const char* name) {
  if (CheckedImpl(pointer) == nullptr)
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

void BindHttp(py::module_& module) {
  py::class_<net::Http2RequestBodyStream,
             std::shared_ptr<net::Http2RequestBodyStream>>(
      module, "Http2RequestBodyStream")
      .def("read",
           [](const std::shared_ptr<net::Http2RequestBodyStream>& self) {
             return FutureToPythonConverted(
                 self->Read(),
                 [](const std::optional<std::string>& value) -> py::object {
                   if (!value.has_value())
                     return py::none();
                   return py::bytes(*value);
                 });
           })
      .def("wait_done",
           [](const std::shared_ptr<net::Http2RequestBodyStream>& self) {
             return FutureToPython(self->Done());
           })
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2RequestBodyStream>& self) {
            return FutureToPython(self->Done());
          })
      .def(
          "cancel",
          [](net::Http2RequestBodyStream& self, const py::object& status) {
            std::optional<absl::Status> converted;
            if (!status.is_none())
              converted = StatusFromPython(status);
            CallWithoutGil([&self, converted = std::move(converted)] {
              return converted.has_value() ? self.Cancel(*converted)
                                           : self.Cancel();
            });
          },
          py::arg("status") = py::none())
      .def_property_readonly("stream_id",
                             &net::Http2RequestBodyStream::stream_id);

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
           py::arg("method") = "GET", py::arg("scheme") = "http",
           py::arg("authority") = "", py::arg("path") = "/",
           py::arg("headers") = py::none(), py::arg("body") = py::bytes())
      .def_readwrite("method", &net::HttpRequest::method)
      .def_readwrite("protocol", &net::HttpRequest::protocol)
      .def_readwrite("scheme", &net::HttpRequest::scheme)
      .def_readwrite("authority", &net::HttpRequest::authority)
      .def_readwrite("path", &net::HttpRequest::path)
      .def_readonly("body_stream", &net::HttpRequest::body_stream)
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
          })
      .def_property(
          "body",
          [](const net::HttpRequest& request) {
            return py::bytes(request.body);
          },
          [](net::HttpRequest& request,
             const py::object& body) -> absl::Status {
            ABSL_ASSIGN_OR_RETURN(request.body, HttpBodyFromPython(body));
            return absl::OkStatus();
          });

  py::class_<net::HttpResponseHead>(module, "HttpResponseHead")
      .def(py::init([](int status, const py::object& headers) {
             return net::HttpResponseHead{
                 .status = status,
                 .headers = ValueOrThrow(HttpHeadersFromPython(headers))};
           }),
           py::arg("status") = 0, py::arg("headers") = py::none())
      .def_readwrite("status", &net::HttpResponseHead::status)
      .def_property(
          "headers",
          [](const net::HttpResponseHead& response) {
            return HttpHeadersToPython(response.headers);
          },
          [](net::HttpResponseHead& response, const py::object& headers) {
            response.headers = ValueOrThrow(HttpHeadersFromPython(headers));
          });

  py::class_<net::HttpResponse>(module, "HttpResponse")
      .def(py::init([](net::HttpResponseHead head, const py::object& body) {
             return net::HttpResponse{
                 .head = std::move(head),
                 .body = ValueOrThrow(HttpBodyFromPython(body))};
           }),
           py::arg("head") = net::HttpResponseHead{},
           py::arg("body") = py::bytes())
      .def_readwrite("head", &net::HttpResponse::head)
      .def_property(
          "body",
          [](const net::HttpResponse& response) {
            return py::bytes(response.body);
          },
          [](net::HttpResponse& response, const py::object& body) {
            response.body = ValueOrThrow(HttpBodyFromPython(body));
          });

  py::class_<net::Http2TlsOptions>(module, "Http2TlsOptions")
      .def(py::init<>())
      .def_readwrite("enabled", &net::Http2TlsOptions::enabled)
      .def_readwrite("verify_peer", &net::Http2TlsOptions::verify_peer)
      .def_readwrite("certificate_pem_file",
                     &net::Http2TlsOptions::certificate_pem_file)
      .def_readwrite("key_pem_file", &net::Http2TlsOptions::key_pem_file)
      .def_readwrite("ca_certificate_pem_file",
                     &net::Http2TlsOptions::ca_certificate_pem_file)
      .def("validate", [](const net::Http2TlsOptions& options) {
        ThrowIfNotOk(options.Validate());
      });

  py::class_<net::Http2Options>(module, "Http2Options")
      .def(py::init<>())
      .def_readwrite("max_request_body_size",
                     &net::Http2Options::max_request_body_size)
      .def_readwrite("max_response_body_size",
                     &net::Http2Options::max_response_body_size)
      .def_readwrite("max_buffered_request_bytes",
                     &net::Http2Options::max_buffered_request_bytes)
      .def_readwrite("max_buffered_response_bytes",
                     &net::Http2Options::max_buffered_response_bytes)
      .def_readwrite("tls", &net::Http2Options::tls)
      .def_property(
          "deadline",
          [](const net::Http2Options& options) {
            return TimeToPython(options.deadline);
          },
          [](net::Http2Options& options, const py::object& deadline) {
            options.deadline = ValueOrThrow(TimeFromPython(deadline));
          })
      .def("validate", [](const net::Http2Options& options) {
        ThrowIfNotOk(options.Validate());
      });

  py::class_<net::Http2ResponseStream,
             std::shared_ptr<net::Http2ResponseStream>>(module,
                                                        "Http2ResponseStream")
      .def("headers",
           [](const std::shared_ptr<net::Http2ResponseStream>& self) {
             return FutureToPython(self->Headers());
           })
      .def("read",
           [](const std::shared_ptr<net::Http2ResponseStream>& self) {
             return FutureToPythonConverted(
                 self->Read(),
                 [](const std::optional<std::string>& value) -> py::object {
                   if (!value.has_value())
                     return py::none();
                   return py::bytes(*value);
                 });
           })
      .def("wait_done",
           [](const std::shared_ptr<net::Http2ResponseStream>& self) {
             return FutureToPython(self->Done());
           })
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPython(self->Done());
          })
      .def(
          "cancel",
          [](net::Http2ResponseStream& self, const py::object& status) {
            std::optional<absl::Status> converted;
            if (!status.is_none())
              converted = StatusFromPython(status);
            CallWithoutGil([&self, converted = std::move(converted)] {
              return converted.has_value() ? self.Cancel(*converted)
                                           : self.Cancel();
            });
          },
          py::arg("status") = py::none())
      .def_property_readonly("stream_id", &net::Http2ResponseStream::stream_id);

  py::class_<net::Http2DuplexStream, std::shared_ptr<net::Http2DuplexStream>>(
      module, "Http2DuplexStream")
      .def("headers",
           [](const std::shared_ptr<net::Http2DuplexStream>& self) {
             return FutureToPython(self->Headers());
           })
      .def("read",
           [](const std::shared_ptr<net::Http2DuplexStream>& self) {
             return FutureToPythonConverted(
                 self->Read(),
                 [](const std::optional<std::string>& value) -> py::object {
                   if (!value.has_value())
                     return py::none();
                   return py::bytes(*value);
                 });
           })
      .def(
          "write",
          [](net::Http2DuplexStream& self, const py::object& data) {
            std::string converted = ValueOrThrow(HttpBodyFromPython(data));
            CallWithoutGil([&self, converted = std::move(converted)]() mutable {
              return self.Write(std::move(converted));
            });
          })
      .def("finish",
           [](net::Http2DuplexStream& self) {
             CallWithoutGil([&self] { return self.Finish(); });
           })
      .def(
          "abort",
          [](net::Http2DuplexStream& self, const py::object& status) {
            std::optional<absl::Status> converted;
            if (!status.is_none())
              converted = StatusFromPython(status);
            CallWithoutGil([&self, converted = std::move(converted)] {
              return converted.has_value() ? self.Abort(*converted)
                                           : self.Abort();
            });
          },
          py::arg("status") = py::none())
      .def("wait_done",
           [](const std::shared_ptr<net::Http2DuplexStream>& self) {
             return FutureToPython(self->Done());
           })
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2DuplexStream>& self) {
            return FutureToPython(self->Done());
          })
      .def_property_readonly("stream_id", &net::Http2DuplexStream::stream_id);

  py::class_<net::Http2ResponseWriter,
             std::shared_ptr<net::Http2ResponseWriter>>(module,
                                                        "Http2ResponseWriter")
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
          py::arg("status"), py::arg("headers") = py::none())
      .def(
          "write",
          [](net::Http2ResponseWriter& self, const py::object& data) {
            std::string converted = ValueOrThrow(HttpBodyFromPython(data));
            CallWithoutGil([&self, converted = std::move(converted)]() mutable {
              return self.Write(std::move(converted));
            });
          })
      .def("finish",
           [](net::Http2ResponseWriter& self) {
             CallWithoutGil([&self] { return self.Finish(); });
           })
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
          py::arg("status"), py::arg("headers") = py::none(),
          py::arg("body") = py::bytes())
      .def(
          "abort",
          [](net::Http2ResponseWriter& self, const py::handle& status) {
            absl::Status converted = StatusFromPython(status);
            CallWithoutGil([&self, converted = std::move(converted)]() mutable {
              return self.Abort(std::move(converted));
            });
          })
      .def("wait_done",
           [](const std::shared_ptr<net::Http2ResponseWriter>& self) {
             return FutureToPython(self->Done());
           })
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2ResponseWriter>& self) {
            return FutureToPython(self->Done());
          })
      .def_property_readonly("headers_sent",
                             &net::Http2ResponseWriter::headers_sent)
      .def_property_readonly("finished", &net::Http2ResponseWriter::finished)
      .def_property_readonly("stream_id", &net::Http2ResponseWriter::stream_id);

  py::class_<net::Http2Server, std::shared_ptr<net::Http2Server>>(module,
                                                                  "Http2Server")
      .def_static(
          "create",
          [](std::string bind_address, std::uint16_t port,
             const py::object& handler, net::Http2Options options) {
            std::shared_ptr<PythonHttpCallback> callback =
                ValueOrThrow(PythonHttpCallback::Create(handler, "handler"));
            return ValueOrThrow(net::Http2Server::Create(
                std::move(bind_address), port,
                [callback = std::move(callback)](
                    net::HttpRequest request,
                    std::shared_ptr<net::Http2ResponseWriter> response) {
                  return callback->Call(std::move(request),
                                        std::move(response));
                },
                options));
          },
          py::arg("bind_address") = "127.0.0.1", py::arg("port") = 0,
          py::arg("handler") = py::none(),
          py::arg("options") = net::Http2Options{})
      .def("stop",
           [](net::Http2Server& self) {
             CallWithoutGil([&self] { return self.Stop(); });
           })
      .def_property_readonly("port", &net::Http2Server::port)
      .def_property_readonly("bind_address", &net::Http2Server::bind_address)
      .def_property_readonly("running", &net::Http2Server::running)
      .def_property_readonly("secure", &net::Http2Server::secure)
      .def("get_impl", [](const net::Http2Server& self) {
        return ImplCapsule(self.GetImpl(), "a11.Http2Server.impl");
      });

  py::class_<net::Http2Client, std::shared_ptr<net::Http2Client>>(module,
                                                                  "Http2Client")
      .def_static(
          "connect",
          [](std::string host, std::uint16_t port, net::Http2Options options) {
            return FutureToPython(
                net::Http2Client::Connect(std::move(host), port, options));
          },
          py::arg("host"), py::arg("port"),
          py::arg("options") = net::Http2Options{})
      .def(
          "request_stream",
          [](net::Http2Client& self, std::string method, std::string path,
             const py::object& headers, const py::object& body,
             std::string scheme) {
            return ValueOrThrow(self.RequestStream(
                std::move(method), std::move(path),
                ValueOrThrow(HttpHeadersFromPython(headers)),
                ValueOrThrow(HttpBodyFromPython(body)), std::move(scheme)));
          },
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
          py::arg("method"), py::arg("path"), py::arg("headers") = py::none(),
          py::arg("body") = py::bytes(), py::arg("scheme") = "")
      .def(
          "extended_connect",
          [](net::Http2Client& self, std::string protocol, std::string path,
             const py::object& headers, std::string scheme) {
            return ValueOrThrow(self.ExtendedConnect(
                std::move(protocol), std::move(path),
                ValueOrThrow(HttpHeadersFromPython(headers)),
                std::move(scheme)));
          },
          py::arg("protocol"), py::arg("path"), py::arg("headers") = py::none(),
          py::arg("scheme") = "")
      .def("close",
           [](net::Http2Client& self) {
             CallWithoutGil([&self] { return self.Close(); });
           })
      .def_property_readonly("host", &net::Http2Client::host)
      .def_property_readonly("port", &net::Http2Client::port)
      .def_property_readonly("connected", &net::Http2Client::connected)
      .def_property_readonly("secure", &net::Http2Client::secure)
      .def("get_impl", [](const net::Http2Client& self) {
        return ImplCapsule(self.GetImpl(), "a11.Http2Client.impl");
      });

  py::class_<net::HttpSseOptions>(module, "HttpSseOptions")
      .def(py::init<>())
      .def_readwrite("stream_options", &net::HttpSseOptions::stream_options)
      .def_readwrite("http2_options", &net::HttpSseOptions::http2_options)
      .def_readwrite("connect_endpoint", &net::HttpSseOptions::connect_endpoint)
      .def_readwrite("message_endpoint", &net::HttpSseOptions::message_endpoint)
      .def("validate", [](const net::HttpSseOptions& options) {
        ThrowIfNotOk(options.Validate());
      });

  py::class_<net::HttpSseWireStream, net::WireStream,
             std::shared_ptr<net::HttpSseWireStream>>(module,
                                                      "HttpSseWireStream")
      .def("get_http_request_headers",
           [](const net::HttpSseWireStream& self) {
             return HttpHeadersToPython(self.GetHttpRequestHeaders());
           })
      .def("get_http_response_headers",
           [](const net::HttpSseWireStream& self) -> py::object {
             const std::optional<net::HttpHeaders> headers =
                 self.GetHttpResponseHeaders();
             if (!headers.has_value())
               return py::none();
             return HttpHeadersToPython(*headers);
           })
      .def("set_http_request_headers",
           [](net::HttpSseWireStream& self, const py::object& headers) {
             ThrowIfNotOk(self.SetHttpRequestHeaders(
                 ValueOrThrow(HttpHeadersFromPython(headers))));
           })
      .def("set_http_response_headers",
           [](net::HttpSseWireStream& self, const py::object& headers) {
             ThrowIfNotOk(self.SetHttpResponseHeaders(
                 ValueOrThrow(HttpHeadersFromPython(headers))));
           })
      .def("wait_for_http_headers",
           [](const std::shared_ptr<net::HttpSseWireStream>& self) {
             return FutureToPython(self->WaitForHttpHeaders());
           });

  py::class_<net::HttpSseClientWireStream, net::HttpSseWireStream,
             std::shared_ptr<net::HttpSseClientWireStream>>(
      module, "HttpSseClientWireStream")
      .def(py::init([](std::string url, net::HttpSseOptions options,
                       std::shared_ptr<net::Http2Client> client,
                       const py::object& request_headers) {
             return ValueOrThrow(net::HttpSseClientWireStream::Create(
                 std::move(url), std::move(options), std::move(client),
                 ValueOrThrow(HttpHeadersFromPython(request_headers))));
           }),
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
          py::arg("url"), py::arg("options") = net::HttpSseOptions{},
          py::arg("client") = nullptr, py::arg("request_headers") = py::none())
      .def_property_readonly("client", &net::HttpSseClientWireStream::client);

  py::class_<net::HttpSseServerWireStream, net::HttpSseWireStream,
             std::shared_ptr<net::HttpSseServerWireStream>>(
      module, "HttpSseServerWireStream")
      .def("accepted",
           [](const std::shared_ptr<net::HttpSseServerWireStream>& self) {
             return FutureToPython(self->Accepted());
           });

  py::class_<net::HttpSseServer, std::shared_ptr<net::HttpSseServer>>(
      module, "HttpSseServer")
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
            return ValueOrThrow(net::HttpSseServer::Create(
                std::move(bind_address), port, std::move(callback),
                std::move(options)));
          },
          py::arg("bind_address") = "127.0.0.1", py::arg("port") = 0,
          py::arg("on_connect") = py::none(),
          py::arg("options") = net::HttpSseOptions{})
      .def("wait_for_stream",
           [](const std::shared_ptr<net::HttpSseServer>& self) {
             return FutureToPython(self->WaitForStream());
           })
      .def("stop",
           [](net::HttpSseServer& self) {
             CallWithoutGil([&self] { return self.Stop(); });
           })
      .def_property_readonly("port", &net::HttpSseServer::port)
      .def_property_readonly("running", &net::HttpSseServer::running)
      .def_property_readonly("http2_server", &net::HttpSseServer::http2_server);

  module.attr("HttpSseWireStreamServer") = module.attr("HttpSseServer");
  module.def("get_http_header",
             [](const py::object& headers, std::string name) -> py::object {
               std::optional<std::string> value = net::GetHttpHeader(
                   ValueOrThrow(HttpHeadersFromPython(headers)), name);
               if (!value.has_value())
                 return py::none();
               return py::str(*value);
             });
  module.def("validate_http_headers", [](const py::object& headers) {
    ThrowIfNotOk(
        net::ValidateHttpHeaders(ValueOrThrow(HttpHeadersFromPython(headers))));
  });
  module.attr("SSE_STREAM_ID_HEADER") = std::string(net::kSseStreamIdHeader);
  module.attr("SSE_HTTP_HEADER_PREFIX") =
      std::string(net::kSseHttpHeaderPrefix);
  module.attr("DEFAULT_SSE_CONNECT_ENDPOINT") =
      std::string(net::kDefaultSseConnectEndpoint);
  module.attr("DEFAULT_SSE_MESSAGE_ENDPOINT") =
      std::string(net::kDefaultSseMessageEndpoint);
}

}  // namespace a11::python

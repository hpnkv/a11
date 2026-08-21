// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <Python.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/typing.h>
#include <pybind11_abseil/status_caster.h>    // IWYU pragma: keep
#include <pybind11_abseil/statusor_caster.h>  // IWYU pragma: keep

#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/net/http/connection_pool.h"
#include "a11/net/http/download.h"
#include "a11/net/http/fetch.h"
#include "a11/net/http/url.h"
#include "a11/net/http2.h"
#include "a11/net/http_sse_wire_stream.h"
#include "a11/net/wire_stream.h"
#include "a11/status.h"
#include "python/bindings.h"
#include "python/casters.h"
#include "python/interop.h"
#include "python/native_types.h"
#include "sdk/http/actions/http_actions.h"

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

py::typing::List<py::typing::Tuple<py::str, py::str>> HttpHeadersToPython(
    const net::HttpHeaders& headers) {
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
    // Queued rather than released here: a destructor may run on a pool
    // worker, and taking the GIL there races interpreter finalization.
    // See DeferredPythonRefs.
    DeferredPythonRefs::Retire(std::exchange(callable_, nullptr));
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

py::typing::Optional<py::capsule> ImplCapsule(void* pointer, const char* name) {
  if (CheckedImpl(pointer) == nullptr) {
    return py::typing::Optional<py::capsule>(py::none());
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

/// A progress callback as Python sees it: `Callable[[int, int], None] | None`.
using OnProgressPython = py::typing::Optional<
    py::typing::Callable<void(py::int_, py::int_)>>;

// GIL-reacquiring release for a Python type held as an ActionPortSchema
// typeinfo handle. Mirrors the actions binding's own deleter so the referent
// stays alive for exactly as long as any copy of the schema.
// Deferred rather than released here: a shared_ptr deleter runs on whichever
// thread drops the last copy, which may be a pool worker. See
// DeferredPythonRefs.
void ReleaseHttpTypeInfo(void* object) {
  DeferredPythonRefs::Retire(static_cast<PyObject*>(object));
}

std::shared_ptr<void> TypeInfoFromClass(const py::object& cls) {
  Py_INCREF(cls.ptr());
  return std::shared_ptr<void>(cls.ptr(), &ReleaseHttpTypeInfo);
}

// One HTTP Action, ready to register. Both the Python export and
// RegisterHttpActionsPy build from this table, so they cannot drift apart.
struct HttpActionEntry {
  std::string_view name;
  a11::actions::ActionSchema schema;
  a11::actions::ActionHandler handler;
};

/**
 * The two HTTP Actions, with Python types attached to the ports C++ cannot name.
 *
 * Every port here carries JSON, bytes, or a scalar, so the mapping is to Python
 * builtins rather than to bound native classes -- an HTTP header map really is a
 * dict and a body really is bytes, which is also why none of this needed a new
 * serialization tag.
 */
std::vector<HttpActionEntry> HttpActionEntries() {
  const auto type_for = [](std::string_view port_type) -> PyObject* {
    if (port_type == "string") {
      return reinterpret_cast<PyObject*>(&PyUnicode_Type);
    }
    if (port_type == "integer") {
      return reinterpret_cast<PyObject*>(&PyLong_Type);
    }
    if (port_type == "bool") {
      return reinterpret_cast<PyObject*>(&PyBool_Type);
    }
    if (port_type == "application/octet-stream") {
      return reinterpret_cast<PyObject*>(&PyBytes_Type);
    }
    return nullptr;  // JSON: a dict, a list or a scalar, so nothing to pin.
  };
  const auto attach = [&type_for](a11::actions::ActionSchema& schema) {
    for (auto* ports : {&schema.inputs, &schema.outputs}) {
      for (auto& [name, port] : *ports) {
        if (PyObject* type = type_for(port.type); type != nullptr) {
          port.typeinfo = TypeInfoFromClass(
              py::reinterpret_borrow<py::object>(py::handle(type)));
        }
      }
    }
  };
  std::vector<HttpActionEntry> entries;
  const auto add = [&](std::string_view name,
                       a11::actions::ActionSchema schema,
                       a11::actions::ActionHandler handler) {
    attach(schema);
    entries.push_back(HttpActionEntry{.name = name,
                                      .schema = std::move(schema),
                                      .handler = std::move(handler)});
  };
  add(sdk::http::kMakeHttpRequestAction, sdk::http::MakeHttpRequestSchema(),
      sdk::http::MakeHttpRequestHandler());
  add(sdk::http::kWebFetchAction, sdk::http::WebFetchSchema(),
      sdk::http::WebFetchHandler());
  return entries;
}

PyActionTriples HttpActionsPy() {
  py::list result;
  for (HttpActionEntry& entry : HttpActionEntries()) {
    result.append(py::make_tuple(
        py::str(std::string(entry.name)), py::cast(std::move(entry.schema)),
        py::cast(NativeActionHandler(std::move(entry.handler)))));
  }
  return result;
}

void RegisterHttpActionsPy(
    const std::shared_ptr<a11::actions::ActionRegistry>& registry) {
  if (registry == nullptr) {
    ThrowStatus(absl::InvalidArgumentError("registry must not be None"));
  }
  for (HttpActionEntry& entry : HttpActionEntries()) {
    if (const absl::Status status =
            registry->Register(std::string(entry.name), std::move(entry.schema),
                               std::move(entry.handler));
        !status.ok()) {
      ThrowStatus(status);
    }
  }
}

/**
 * Wraps a Python progress callable for a fetch or download.
 *
 * The callback runs on the pooled fiber doing the transfer, not on the asyncio
 * loop, so it has to take the GIL itself. A callback that raises is reported and
 * dropped rather than propagated: a broken progress bar is not a reason to fail
 * a download that is otherwise succeeding, and letting the exception cross the
 * fiber boundary would surface as an unrelated Unknown status.
 */
net::OnFetchProgress ProgressFromPython(const py::object& on_progress) {
  if (on_progress.is_none()) {
    return {};
  }
  auto shared = std::make_shared<py::object>(on_progress);
  return [shared](std::uint64_t done, std::uint64_t total) {
    py::gil_scoped_acquire acquire;
    try {
      (*shared)(done, total);
    } catch (const py::error_already_set& error) {
      PyErr_WarnFormat(PyExc_RuntimeWarning, 1,
                       "a11 progress callback raised: %s", error.what());
    }
  };
}

}  // namespace

void BindHttp(py::module_& module) {
  py::classh<net::Http2RequestBodyStream>(module, "Http2RequestBodyStream")
      .def(
          "read",
          [](const std::shared_ptr<net::Http2RequestBodyStream>& self) {
            return FutureToPythonAs<py::typing::Optional<py::bytes>>(
                WithoutGil([&] { return self->Read(); }),
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
            return FutureToPython(WithoutGil([&] { return self->Done(); }));
          },
          "Await completion of the request body stream.")
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2RequestBodyStream>& self) {
            return FutureToPython(WithoutGil([&] { return self->Done(); }));
          },
          "Future that completes when the request body stream is done.")
      .def(
          "cancel",
          [](net::Http2RequestBodyStream& self,
             const py::typing::Optional<NativeStatus>& status) {
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
                       const py::typing::Optional<py::typing::Iterable<
                           py::typing::Tuple<py::str, py::str>>>& headers,
                       const py::object& body) {
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
             const py::typing::Optional<
                 py::typing::Iterable<py::typing::Tuple<py::str, py::str>>>&
                 headers) -> absl::Status {
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
      .def(py::init([](int status,
                       const py::typing::Optional<py::typing::Iterable<
                           py::typing::Tuple<py::str, py::str>>>& headers) {
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
          [](net::HttpResponseHead& response,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers) {
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

  py::enum_<net::Http2Options::ProtocolPreference>(module,
                                                   "HttpProtocolPreference")
      .value("AUTO", net::Http2Options::ProtocolPreference::kAuto,
             "Prefer HTTP/2, fall back to HTTP/1.1 (ALPN order / downgrade).")
      .value("HTTP2", net::Http2Options::ProtocolPreference::kHttp2,
             "Require HTTP/2 (h2 over TLS, prior-knowledge h2c cleartext).")
      .value("HTTP11", net::Http2Options::ProtocolPreference::kHttp11,
             "Require HTTP/1.1.");

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
      .def_readwrite("enable_h2", &net::Http2Options::enable_h2,
                     "Serve/accept HTTP/2 over TLS (ALPN \"h2\").")
      .def_readwrite("enable_h2c", &net::Http2Options::enable_h2c,
                     "Serve/accept cleartext prior-knowledge HTTP/2.")
      .def_readwrite("enable_http1", &net::Http2Options::enable_http1,
                     "Serve/accept HTTP/1.1 (ALPN and/or cleartext).")
      .def_readwrite("enable_push", &net::Http2Options::enable_push,
                     "Client: accept HTTP/2 server pushes. Off by default, and "
                     "advertised as off, so a peer cannot spend this side's "
                     "streams on responses nobody asked for. A client that "
                     "enables it must read Http2ResponseStream.next_push and "
                     "either consume or cancel each pushed response.")
      .def_readwrite("client_preference", &net::Http2Options::client_preference,
                     "Client protocol preference and cleartext attempt order.")
      .def_readwrite("client_allow_downgrade",
                     &net::Http2Options::client_allow_downgrade,
                     "Whether a cleartext client may retry with the other "
                     "protocol when its first attempt fails.")
      .def_property(
          "deadline",
          [](const net::Http2Options& options) -> NativeTime {
            return NativeTime(options.deadline);
          },
          [](net::Http2Options& options,
             const py::typing::Optional<NativeTime>& deadline) {
            options.deadline = ValueOrThrow(TimeFromPython(deadline));
          },
          "The operation deadline.")
      .def(
          "validate",
          [](const net::Http2Options& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the options, raising on error.");

  // Declared before either gains members because each names the other: a
  // response stream hands back pushed responses, and a pushed response *is* a
  // response stream. pybind11 renders a signature when def() runs, so both types
  // have to be registered by then or one of them has no name to render.
  py::class_<net::HttpPushedResponse> pushed_response(module,
                                                     "HttpPushedResponse");
  py::classh<net::Http2ResponseStream> response_stream(module,
                                                       "Http2ResponseStream");

  response_stream
      .def(
          "headers",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPython(WithoutGil([&] { return self->Headers(); }));
          },
          "Await the response head (status and headers).")
      .def(
          "read",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPythonAs<py::typing::Optional<py::bytes>>(
                WithoutGil([&] { return self->Read(); }),
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
          "trailers",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPythonAs<
                py::typing::List<py::typing::Tuple<py::str, py::str>>>(
                WithoutGil([&] { return self->Trailers(); }),
                [](const net::HttpHeaders& fields) {
                  return HttpHeadersToPython(fields);
                });
          },
          "Await the trailer fields that followed the body. Resolves to an "
          "empty list when the peer sent no trailer section, so this can be "
          "awaited without knowing whether one was coming.")
      .def(
          "next_push",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPythonAs<
                py::typing::Optional<net::HttpPushedResponse>>(
                WithoutGil([&] { return self->NextPush(); }),
                [](const std::optional<net::HttpPushedResponse>& value)
                    -> py::object {
                  if (!value.has_value()) {
                    return py::none();
                  }
                  return py::cast(*value);
                });
          },
          "Await the next response the server pushed alongside this one, or "
          "None once this response has ended (after which no push can arrive). "
          "Requires Http2Options.enable_push.")
      .def(
          "wait_done",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPython(WithoutGil([&] { return self->Done(); }));
          },
          "Await completion of the response stream.")
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2ResponseStream>& self) {
            return FutureToPython(WithoutGil([&] { return self->Done(); }));
          },
          "Future that completes when the response stream is done.")
      .def(
          "cancel",
          [](net::Http2ResponseStream& self,
             const py::typing::Optional<NativeStatus>& status) {
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

  pushed_response
      .def_readonly("method", &net::HttpPushedResponse::method,
                    "Method of the request the server promised.")
      .def_readonly("scheme", &net::HttpPushedResponse::scheme,
                    "Scheme of the promised request.")
      .def_readonly("authority", &net::HttpPushedResponse::authority,
                    "Authority of the promised request.")
      .def_readonly("path", &net::HttpPushedResponse::path,
                    "Path and query of the promised request.")
      .def_property_readonly(
          "headers",
          [](const net::HttpPushedResponse& push) {
            return HttpHeadersToPython(push.headers);
          },
          "Header fields of the promised request, as (name, value) pairs.")
      .def_readonly("response", &net::HttpPushedResponse::response,
                    "The pushed response: its own head, body and trailers. "
                    "Cancel it to refuse the push.")
      .def("__repr__", [](const net::HttpPushedResponse& push) {
        return absl::StrCat("HttpPushedResponse('", push.method, " ", push.path,
                            "')");
      });

  py::classh<net::Http2DuplexStream>(module, "Http2DuplexStream")
      .def(
          "headers",
          [](const std::shared_ptr<net::Http2DuplexStream>& self) {
            return FutureToPython(WithoutGil([&] { return self->Headers(); }));
          },
          "Await the response head (status and headers).")
      .def(
          "read",
          [](const std::shared_ptr<net::Http2DuplexStream>& self) {
            return FutureToPythonAs<py::typing::Optional<py::bytes>>(
                WithoutGil([&] { return self->Read(); }),
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
          [](net::Http2DuplexStream& self,
             const py::typing::Optional<NativeStatus>& status) {
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
            return FutureToPython(WithoutGil([&] { return self->Done(); }));
          },
          "Await completion of the duplex stream.")
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2DuplexStream>& self) {
            return FutureToPython(WithoutGil([&] { return self->Done(); }));
          },
          "Future that completes when the duplex stream is done.")
      .def_property_readonly(
          "response", &net::Http2DuplexStream::response,
          "The read half, for the parts of a response this facade does not "
          "forward: its trailers, and any pushed responses.")
      .def_property_readonly("stream_id", &net::Http2DuplexStream::stream_id,
                             "The HTTP/2 stream identifier.");

  py::classh<net::Http2ResponseWriter>(module, "Http2ResponseWriter")
      .def(
          "send_headers",
          [](net::Http2ResponseWriter& self, int status,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers) {
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
          "finish_with_trailers",
          [](net::Http2ResponseWriter& self,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& trailers) {
            net::HttpHeaders converted =
                ValueOrThrow(HttpHeadersFromPython(trailers));
            CallWithoutGil([&self, converted = std::move(converted)]() mutable {
              return self.FinishWithTrailers(std::move(converted));
            });
          },
          "End the response body with a trailer section -- the only place a "
          "value computed while streaming (a checksum, a row count) can be "
          "reported from. Equivalent to finish() when empty.",
          py::arg("trailers") = py::none())
      .def(
          "push_promise",
          [](net::Http2ResponseWriter& self, std::string method,
             std::string path,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers) {
            net::HttpHeaders converted =
                ValueOrThrow(HttpHeadersFromPython(headers));
            return ValueWithoutGil(
                [&self, method = std::move(method), path = std::move(path),
                 headers = std::move(converted)]() mutable {
                  return self.PushPromise(std::move(method), std::move(path),
                                          std::move(headers));
                });
          },
          "Promise a response the client did not ask for, returning the writer "
          "for it. Must be called before this response is finished, and fails "
          "when the client did not enable push.",
          py::arg("method"), py::arg("path"), py::arg("headers") = py::none())
      .def(
          "send_response",
          [](net::Http2ResponseWriter& self, int status,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers,
             const py::object& body) {
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
          [](net::Http2ResponseWriter& self,
             const PyLike<NativeStatus>& status) {
            absl::Status converted = StatusFromPython(status);
            CallWithoutGil([&self, converted = std::move(converted)]() mutable {
              return self.Abort(std::move(converted));
            });
          },
          "Abort the response with the given status.", py::arg("status"))
      .def(
          "wait_done",
          [](const std::shared_ptr<net::Http2ResponseWriter>& self) {
            return FutureToPython(WithoutGil([&] { return self->Done(); }));
          },
          "Await completion of the response.")
      .def_property_readonly(
          "done",
          [](const std::shared_ptr<net::Http2ResponseWriter>& self) {
            return FutureToPython(WithoutGil([&] { return self->Done(); }));
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
            return FutureToPython(WithoutGil([&] {
              return net::Http2Client::Connect(std::move(host), port, options);
            }));
          },
          "Asynchronously connect to an HTTP/2 server, returning a future "
          "that resolves to the connected client.",
          py::arg("host"), py::arg("port"),
          py::arg("options") = net::Http2Options{})
      .def(
          "request_stream",
          [](net::Http2Client& self, std::string method, std::string path,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers,
             const py::object& body, std::string scheme) {
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
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers,
             const py::object& body, std::string scheme) {
            net::HttpHeaders fields =
                ValueOrThrow(HttpHeadersFromPython(headers));
            std::string payload = ValueOrThrow(HttpBodyFromPython(body));
            return FutureToPython(WithoutGil([&] {
              return self.Request(std::move(method), std::move(path),
                                  std::move(fields), std::move(payload),
                                  std::move(scheme));
            }));
          },
          "Send a request and await the full buffered response.",
          py::arg("method"), py::arg("path"), py::arg("headers") = py::none(),
          py::arg("body") = py::bytes(), py::arg("scheme") = "")
      .def(
          "extended_connect",
          [](net::Http2Client& self, std::string protocol, std::string path,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers,
             std::string scheme) {
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
          "request_streaming_body",
          [](net::Http2Client& self, std::string method, std::string path,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers,
             std::string scheme) {
            auto native_headers = ValueOrThrow(HttpHeadersFromPython(headers));
            return ValueWithoutGil([&] {
              return self.RequestStreamingBody(std::move(method),
                                               std::move(path),
                                               std::move(native_headers),
                                               std::move(scheme));
            });
          },
          "Open a request whose body is written incrementally afterwards, for "
          "an upload of unknown or unbounded length. Returns a duplex stream: "
          "write() sends more of the body, finish() ends it, and the response "
          "is read from the same handle. Do not set content-length.",
          py::arg("method"), py::arg("path"), py::arg("headers") = py::none(),
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
      .def_property_readonly(
          "multiplexed", &net::Http2Client::multiplexed,
          "Whether this connection can carry several exchanges at once: true "
          "for HTTP/2, false for HTTP/1.1, which A11 limits to one request per "
          "connection.")
      .def(
          "get_impl",
          [](const net::Http2Client& self) {
            return ImplCapsule(self.GetImpl(), "a11.Http2Client.impl");
          },
          "Return an opaque capsule wrapping the native client handle.");

  py::enum_<net::SseOutboundDelivery>(
      module, "SseOutboundDelivery",
      "How an SSE client delivers its outbound WireMessages. Servers accept "
      "either; only the client chooses.")
      .value("POST", net::SseOutboundDelivery::kPost,
             "One HTTP POST per message, issued concurrently up to "
             "max_concurrent_posts. Reachable from anything that can fetch().")
      .value("STREAM", net::SseOutboundDelivery::kStream,
             "One long-lived request body carrying every message: HTTP/2 DATA "
             "frames or an HTTP/1.1 chunked body. Removes the "
             "one-request-per-message ceiling; falls back to POST against a "
             "server that does not advertise it.");

  py::class_<net::HttpSseOptions>(module, "HttpSseOptions")
      .def(py::init<>(), "Construct default HTTP SSE wire stream options.")
      .def_readwrite("describe", &net::HttpSseOptions::describe,
                     "Server-side GET /actions. Point it at a service with "
                     "Service.expose_descriptors_on.")
      .def_readwrite("stream_options", &net::HttpSseOptions::stream_options,
                     "The underlying wire stream options.")
      .def_readwrite("http2_options", &net::HttpSseOptions::http2_options,
                     "The underlying HTTP/2 transport options.")
      .def_readwrite("connect_endpoint", &net::HttpSseOptions::connect_endpoint,
                     "The endpoint path used to open the SSE connection.")
      .def_readwrite("message_endpoint", &net::HttpSseOptions::message_endpoint,
                     "The endpoint path template used to post messages.")
      .def_readwrite("outbound", &net::HttpSseOptions::outbound,
                     "Client-side outbound delivery method.")
      .def_readwrite("accept_streamed_outbound",
                     &net::HttpSseOptions::accept_streamed_outbound,
                     "Server-side: whether a streamed outbound request body is "
                     "accepted and advertised. Clearing it leaves clients with "
                     "POST-per-message, which over HTTP/1.1 costs a connection "
                     "per message.")
      .def_readwrite("max_concurrent_posts",
                     &net::HttpSseOptions::max_concurrent_posts,
                     "Outbound POSTs kept in flight at once; 1 restores "
                     "strictly serialised delivery.")
      .def_readwrite("headers", &net::HttpSseOptions::headers,
                     "Server-side response-header policy: the Server header, "
                     "cross-origin access, and the cache hints a reply "
                     "carries. See a11.net.ServerHeaderOptions.")
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
          "Return the HTTP headers carried on the underlying SSE request.")
      .def(
          "get_http_response_headers",
          [](const net::HttpSseWireStream& self)
              -> py::typing::Optional<
                  py::typing::List<py::typing::Tuple<py::str, py::str>>> {
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
          [](net::HttpSseWireStream& self,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers) {
            ThrowIfNotOk(self.SetHttpRequestHeaders(
                ValueOrThrow(HttpHeadersFromPython(headers))));
          },
          "Set the HTTP headers to send on the underlying SSE request. Call "
          "this before the stream connects to attach auth or routing "
          "metadata that your agent's transport needs.",
          py::arg("headers"))
      .def(
          "set_http_response_headers",
          [](net::HttpSseWireStream& self,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers) {
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
            return FutureToPython(
                WithoutGil([&] { return self->WaitForHttpHeaders(); }));
          },
          "Await the exchange of HTTP headers for the SSE connection. "
          "Because SSE wire streams connect asynchronously, await this "
          "future before reading response headers or assuming the stream is "
          "live.");

  py::classh<net::HttpSseClientWireStream, net::HttpSseWireStream>(
      module, "HttpSseClientWireStream")
      .def(py::init(
               [](std::string url, net::HttpSseOptions options,
                  std::shared_ptr<net::Http2Client> client,
                  const py::typing::Optional<py::typing::Iterable<
                      py::typing::Tuple<py::str, py::str>>>& request_headers) {
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
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& request_headers) {
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
          "agent connection.")
      .def_property_readonly(
          "outbound_delivery", &net::HttpSseClientWireStream::outbound_delivery,
          "The outbound delivery method actually in use. Equals the requested "
          "one once connected, except where a STREAM request fell back to POST "
          "because the server did not advertise streamed delivery.");

  py::classh<net::HttpSseServerWireStream, net::HttpSseWireStream>(
      module, "HttpSseServerWireStream")
      .def(
          "accepted",
          [](const std::shared_ptr<net::HttpSseServerWireStream>& self) {
            return FutureToPython(WithoutGil([&] { return self->Accepted(); }));
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
            return FutureToPython(
                WithoutGil([&] { return self->WaitForStream(); }));
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

  py::class_<net::ParsedUrl>(module, "ParsedUrl")
      .def(py::init<>(), "Construct an empty parsed URL.")
      .def_readwrite("scheme", &net::ParsedUrl::scheme,
                     "Lowercase scheme, without \"://\".")
      .def_readwrite("host", &net::ParsedUrl::host,
                     "Hostname or IP literal, without IPv6 brackets.")
      .def_readwrite("port", &net::ParsedUrl::port,
                     "Explicit port, or the scheme's default.")
      .def_readwrite("path", &net::ParsedUrl::path,
                     "Path beginning with '/', or empty when none was given.")
      .def_readwrite("query", &net::ParsedUrl::query,
                     "Query without the leading '?'.")
      .def_property_readonly("secure", &net::ParsedUrl::secure,
                             "Whether the scheme implies TLS.")
      .def_property_readonly("authority", &net::ParsedUrl::authority,
                             "The authority as a header value.")
      .def_property_readonly("target", &net::ParsedUrl::target,
                             "The request target: path and query, at least \"/\".")
      .def_property_readonly("origin", &net::ParsedUrl::origin,
                             "scheme://authority, with no trailing slash.")
      .def("__str__", &net::ParsedUrl::ToString)
      .def("__repr__", [](const net::ParsedUrl& url) {
        return absl::StrCat("ParsedUrl('", url.ToString(), "')");
      });

  module.def(
      "parse_url",
      [](std::string_view url) { return ValueOrThrow(net::ParseUrl(url)); },
      "Parse an absolute http/https/ws/wss URL, raising on a malformed one.",
      py::arg("url"));
  module.def(
      "resolve_url_reference",
      [](const net::ParsedUrl& base, std::string_view reference) {
        return ValueOrThrow(net::ResolveReference(base, reference));
      },
      "Resolve a reference (such as a Location header) against a base URL.",
      py::arg("base"), py::arg("reference"));

  py::class_<net::FetchOptions>(module, "FetchOptions")
      .def(py::init<>(), "Construct default fetch options.")
      .def_readwrite("method", &net::FetchOptions::method,
                     "Request method.")
      .def_property(
          "headers",
          [](const net::FetchOptions& options) {
            return HttpHeadersToPython(options.headers);
          },
          [](net::FetchOptions& options,
             const py::typing::Optional<py::typing::Iterable<
                 py::typing::Tuple<py::str, py::str>>>& headers) {
            options.headers = ValueOrThrow(HttpHeadersFromPython(headers));
          },
          "Extra request headers as a list of (name, value) pairs.")
      .def_property(
          "body",
          [](const net::FetchOptions& options) {
            return py::bytes(options.body);
          },
          [](net::FetchOptions& options, const py::object& body) {
            options.body = ValueOrThrow(HttpBodyFromPython(body));
          },
          "Request body, for methods that take one.")
      .def_readwrite("max_redirects", &net::FetchOptions::max_redirects,
                     "Redirects to follow; 0 returns the 3xx response itself.")
      .def_readwrite("transport", &net::FetchOptions::transport,
                     "Transport settings; tls.enabled follows the URL scheme.")
      .def_readwrite("default_user_agent",
                     &net::FetchOptions::default_user_agent,
                     "Send a default user-agent when headers omit one.")
      .def_property(
          "timeout",
          [](const net::FetchOptions& options) -> NativeDuration {
            return NativeDuration(options.timeout);
          },
          [](net::FetchOptions& options,
             const py::typing::Optional<NativeDuration>& timeout) {
            options.timeout = ValueOrThrow(DurationFromPython(timeout));
          },
          "Wall-clock bound on the whole operation, redirects included.")
      .def(
          "validate",
          [](const net::FetchOptions& options) {
            ThrowIfNotOk(options.Validate());
          },
          "Validate the options, raising on error.");

  py::class_<net::DownloadOptions>(module, "DownloadOptions")
      .def(py::init<>(), "Construct default download options.")
      .def_property(
          "destination",
          [](const net::DownloadOptions& options) {
            return options.destination.string();
          },
          [](net::DownloadOptions& options, std::string destination) {
            options.destination = std::move(destination);
          },
          "Final path; parent directories are created.")
      .def_readwrite("expected_sha1", &net::DownloadOptions::expected_sha1,
                     "Expected SHA-1 as hex, or empty to skip verification.")
      .def_readwrite("fetch", &net::DownloadOptions::fetch,
                     "Request settings.")
      .def_property(
          "on_progress",
          [](const net::DownloadOptions&) -> OnProgressPython {
            // Write-only: what is stored is a C++ closure over the Python
            // callable, not the callable itself, so there is nothing to hand
            // back.
            return py::none();
          },
          [](net::DownloadOptions& options,
             const OnProgressPython& on_progress) {
            options.on_progress = ProgressFromPython(on_progress);
          },
          "Callable taking (bytes_done, bytes_total); write-only.");

  module.def(
      "fetch",
      [](std::string url, const py::typing::Optional<net::FetchOptions>&
                              options) {
        net::FetchOptions converted = options.is_none()
                                          ? net::FetchOptions{}
                                          : options.cast<net::FetchOptions>();
        return FutureToPython(WithoutGil([&] {
          return net::Fetch(std::move(url), std::move(converted));
        }));
      },
      R"doc(Fetch a URL and buffer the whole response.

Follows redirects, maps a 4xx/5xx onto a status error, and enables TLS from the
scheme. Awaitable.

Examples:
    ```python
    response = await a11.net.http.fetch("https://example.com/index.html")
    print(response.head.status, len(response.body))
    ```
)doc",
      py::arg("url"), py::arg("options") = py::none());

  module.def(
      "download",
      [](std::string url, net::DownloadOptions options) {
        return FutureToPythonAs<py::str>(
            WithoutGil(
                [&] { return net::Download(std::move(url), std::move(options)); }),
            [](const std::filesystem::path& path) -> py::object {
              return py::str(path.string());
            });
      },
      R"doc(Download a URL to a verified file, atomically.

Returns the destination path. A destination that already exists and matches
``expected_sha1`` is returned without touching the network. Awaitable.
)doc",
      py::arg("url"), py::arg("options"));

  module.def(
      "file_sha1",
      [](std::string path) {
        return ValueWithoutGil([&path] {
          return net::FileSha1(std::filesystem::path(path));
        });
      },
      "Compute the SHA-1 of a file as lowercase hex. Blocks.",
      py::arg("path"));

  module.def(
      "get_http_header",
      [](const py::typing::List<py::typing::Tuple<py::str, py::str>>& headers,
         std::string name) -> py::typing::Optional<py::str> {
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
      [](const py::typing::Optional<
          py::typing::Iterable<py::typing::Tuple<py::str, py::str>>>& headers) {
        ThrowIfNotOk(net::ValidateHttpHeaders(
            ValueOrThrow(HttpHeadersFromPython(headers))));
      },
      "Validate a collection of HTTP headers, raising on error.",
      py::arg("headers"));
  module.def("http_actions", &HttpActionsPy,
             "Return the HTTP Actions as (name, schema, handler) triples: "
             "make_http_request and web-fetch, in that order.");
  module.def("register_http_actions", &RegisterHttpActionsPy,
             py::arg("registry"),
             "Register make_http_request and web-fetch on `registry`.");
  module.attr("MAKE_HTTP_REQUEST_ACTION") =
      std::string(sdk::http::kMakeHttpRequestAction);
  module.attr("WEB_FETCH_ACTION") = std::string(sdk::http::kWebFetchAction);

  module.attr("SSE_STREAM_ID_HEADER") = std::string(net::kSseStreamIdHeader);
  module.attr("SSE_HTTP_HEADER_PREFIX") =
      std::string(net::kSseHttpHeaderPrefix);
  module.attr("DEFAULT_SSE_CONNECT_ENDPOINT") =
      std::string(net::kDefaultSseConnectEndpoint);
  module.attr("DEFAULT_SSE_MESSAGE_ENDPOINT") =
      std::string(net::kDefaultSseMessageEndpoint);
}

}  // namespace a11::python

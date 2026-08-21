// Copyright 2026 The A11 Authors.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "a11/data/types.h"
#include "a11/obs/provider.h"
#include "a11/obs/span.h"
#include "a11/obs/trace_context.h"
#include "a11/obs/tracer.h"
#include "python/bindings.h"
#include "python/interop.h"

namespace a11::python {
namespace {

namespace py = pybind11;

obs::SpanKind ParseSpanKind(std::string_view kind) {
  if (kind == "server") {
    return obs::SpanKind::kServer;
  }
  if (kind == "client") {
    return obs::SpanKind::kClient;
  }
  if (kind == "producer") {
    return obs::SpanKind::kProducer;
  }
  if (kind == "consumer") {
    return obs::SpanKind::kConsumer;
  }
  return obs::SpanKind::kInternal;
}

// Python-facing handle to a standalone span (not tied to an Action). Owns the
// move-only obs::Span; ends it on end()/destruction.
class PySpan {
 public:
  explicit PySpan(obs::Span span) : span_(std::move(span)) {}

  // W3C traceparent for this span, so it can parent child actions/spans.
  [[nodiscard]] std::string Traceparent() const {
    const std::string trace_id = span_.TraceIdHex();
    if (trace_id.empty()) {
      return "";
    }
    return "00-" + trace_id + "-" + span_.SpanIdHex() + "-01";
  }

  void SetAttribute(const std::string& key, const py::object& value) {
    if (py::isinstance<py::bool_>(value)) {
      span_.SetAttribute(key, value.cast<bool>());
    } else if (py::isinstance<py::int_>(value)) {
      span_.SetAttribute(key, value.cast<std::int64_t>());
    } else if (py::isinstance<py::float_>(value)) {
      span_.SetAttribute(key, value.cast<double>());
    } else {
      span_.SetAttribute(key, py::str(value).cast<std::string>());
    }
  }

  void SetName(const std::string& name) { span_.UpdateName(name); }

  void SetStatus(std::string_view code, const std::string& description) {
    obs::SpanStatus status = obs::SpanStatus::kUnset;
    if (code == "ok") {
      status = obs::SpanStatus::kOk;
    } else if (code == "error") {
      status = obs::SpanStatus::kError;
    } else if (code != "unset") {
      ThrowStatus(absl::InvalidArgumentError(
          "span status must be 'ok', 'error' or 'unset'"));
    }
    span_.SetStatus(status, description);
  }

  void End() { span_.End(); }

 private:
  obs::Span span_;
};

std::unique_ptr<PySpan> StartSpan(const std::string& name,
                                  std::string_view kind,
                                  const std::string& parent_traceparent) {
  const obs::SpanKind span_kind = ParseSpanKind(kind);
  if (!parent_traceparent.empty()) {
    data::ByteMap headers;
    headers[std::string(obs::kTraceparentHeader)] = parent_traceparent;
    absl::StatusOr<std::optional<obs::TraceContext>> context =
        obs::ExtractTraceContext(headers);
    if (!context.ok()) {
      ThrowStatus(context.status());
    }
    if (context->has_value()) {
      return std::make_unique<PySpan>(
          obs::Tracer::StartSpan(name, span_kind, &context->value()));
    }
  }
  return std::make_unique<PySpan>(
      obs::Tracer::StartSpan(name, span_kind, nullptr));
}

std::vector<std::pair<std::string, std::string>> ToPairs(
    const std::map<std::string, std::string>& map) {
  std::vector<std::pair<std::string, std::string>> pairs;
  pairs.reserve(map.size());
  for (const auto& [key, value] : map) {
    pairs.emplace_back(key, value);
  }
  return pairs;
}

void Configure(const std::string& service_name,
               const std::map<std::string, std::string>& resource_attrs,
               const std::string& exporter, bool use_simple_processor,
               const std::string& otlp_endpoint,
               const std::map<std::string, std::string>& otlp_headers,
               int otlp_timeout_millis,
               const std::vector<std::string>& baggage_span_attributes) {
  obs::ProviderOptions options;
  options.service_name = service_name;
  options.resource_attributes = ToPairs(resource_attrs);
  options.use_simple_processor = use_simple_processor;
  options.otlp_endpoint = otlp_endpoint;
  options.otlp_headers = ToPairs(otlp_headers);
  options.otlp_timeout_millis = otlp_timeout_millis;
  options.baggage_span_attributes = baggage_span_attributes;

  if (exporter == "none") {
    options.exporter = obs::ExporterKind::kNone;
  } else if (exporter == "in_memory") {
    options.exporter = obs::ExporterKind::kInMemory;
  } else if (exporter == "ostream") {
    options.exporter = obs::ExporterKind::kOstream;
  } else if (exporter == "otlp_http") {
    options.exporter = obs::ExporterKind::kOtlpHttp;
  } else {
    ThrowStatus(absl::InvalidArgumentError("Unknown exporter: " + exporter));
  }
  // Reconfiguring tears down the previous provider, which joins the batch span
  // processor's worker thread (and may block on a final export). Release the
  // GIL across the native call so a worker thread that needs the GIL to finish
  // destroying an in-flight action (PyGILState_Ensure) is not starved -- see
  // obs_shutdown below for why that starvation is fatal at interpreter exit.
  absl::Status status;
  {
    py::gil_scoped_release release;
    status = obs::Configure(options);
  }
  if (!status.ok()) {
    ThrowStatus(status);
  }
}

PyJsonObjects RecordedSpans() {
  // GetRecordedSpans() force-flushes the processor, which can block on the
  // exporter; drop the GIL for that and re-take it to build the Python list.
  std::vector<obs::RecordedSpan> spans;
  {
    py::gil_scoped_release release;
    spans = obs::GetRecordedSpans();
  }
  py::list result;
  for (const obs::RecordedSpan& span : spans) {
    py::dict entry;
    entry["name"] = span.name;
    entry["trace_id"] = span.trace_id;
    entry["span_id"] = span.span_id;
    entry["parent_span_id"] = span.parent_span_id;
    entry["kind"] = static_cast<int>(span.kind);
    entry["status_code"] = span.status_code;
    entry["status_description"] = span.status_description;
    py::dict attributes;
    for (const auto& [key, value] : span.attributes) {
      attributes[py::str(key)] = value;
    }
    entry["attributes"] = attributes;
    py::list events;
    for (const obs::RecordedEvent& event : span.events) {
      py::dict event_dict;
      event_dict["name"] = event.name;
      py::dict event_attrs;
      for (const auto& [key, value] : event.attributes) {
        event_attrs[py::str(key)] = value;
      }
      event_dict["attributes"] = event_attrs;
      events.append(event_dict);
    }
    entry["events"] = events;
    result.append(entry);
  }
  return result;
}

}  // namespace

void BindObs(py::module_& module) {
  // Exposed as flat top-level functions (not a submodule) so pybind11-stubgen
  // keeps emitting a single _native.pyi. The a11.observability package wraps
  // these into a friendly API.
  module.attr("OTEL_TRACEPARENT_HEADER") =
      std::string(a11::obs::kTraceparentHeader);
  module.attr("OTEL_TRACESTATE_HEADER") =
      std::string(a11::obs::kTracestateHeader);
  module.attr("OTEL_BAGGAGE_HEADER") = std::string(a11::obs::kBaggageHeader);

  module.def(
      "obs_configure", &Configure,
      "Installs the global tracer provider with the given service name, "
      "exporter, and OTLP options; replaces any existing provider.",
      py::arg("service_name") = "a11",
      py::arg("resource_attributes") = std::map<std::string, std::string>{},
      py::arg("exporter") = "otlp_http",
      py::arg("use_simple_processor") = false, py::arg("otlp_endpoint") = "",
      py::arg("otlp_headers") = std::map<std::string, std::string>{},
      py::arg("otlp_timeout_millis") = 10000,
      py::arg("baggage_span_attributes") = std::vector<std::string>{});
  py::class_<PySpan>(module, "_Span")
      .def("traceparent", &PySpan::Traceparent,
           "Returns the W3C traceparent for this span, so it can parent child "
           "actions or spans.")
      .def("set_attribute", &PySpan::SetAttribute,
           "Sets an attribute on the span.", py::arg("key"), py::arg("value"))
      .def("set_name", &PySpan::SetName, "Updates the span's name.",
           py::arg("name"))
      .def("set_status", &PySpan::SetStatus,
           "Sets the span's status code ('ok', 'error', or 'unset') and an "
           "optional description.",
           py::arg("code"), py::arg("description") = "")
      .def("end", &PySpan::End, "Ends the span.");
  module.def("obs_start_span", &StartSpan,
             "Starts a new span with the given name and kind, optionally "
             "parented by a W3C traceparent.",
             py::arg("name"), py::arg("kind") = "internal",
             py::arg("parent_traceparent") = "");

  module.def(
      "obs_shutdown",
      []() {
        // Shutdown() flushes and joins the batch span processor's worker
        // thread, which can block on a final OTLP export. It must run with the
        // GIL released: a native worker thread finishing an in-flight action
        // destroys the last shared_ptr<Action>, whose Python members require
        // the GIL (PyGILState_Ensure). If this (typically the atexit) call held
        // the GIL while blocking, that worker would only acquire it once we
        // return -- by which point the interpreter is finalizing, so CPython
        // force-exits the thread via pthread_exit. The forced unwind then tears
        // through a noexcept destructor and std::terminate aborts the process
        // ("terminate called without an active exception"). Dropping the GIL
        // lets the worker complete its teardown before finalization.
        py::gil_scoped_release release;
        a11::obs::Shutdown();
      },
      "Flushes and tears down the global tracer provider.");
  module.def("obs_is_configured", &a11::obs::IsConfigured,
             "Returns whether a tracer provider is currently installed.");
  module.def("obs_recorded_spans", &RecordedSpans,
             "Returns finished spans captured by the in-memory exporter, "
             "oldest first.");
  module.def("obs_clear_recorded_spans", &a11::obs::ClearRecordedSpans,
             "Clears the in-memory span buffer.");
}

}  // namespace a11::python

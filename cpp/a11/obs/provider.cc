// Copyright 2026 The A11 Authors.

#include "a11/obs/provider.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/status/status.h>
#include <absl/strings/str_cat.h>

#include <opentelemetry/exporters/memory/in_memory_span_data.h>
#include <opentelemetry/exporters/memory/in_memory_span_exporter.h>
#include <opentelemetry/exporters/ostream/span_exporter_factory.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/variant.h>
#include <opentelemetry/sdk/common/attribute_utils.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/processor.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/span_data.h>
#include <opentelemetry/sdk/trace/tracer_context_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span_id.h>
#include <opentelemetry/trace/span_metadata.h>
#include <opentelemetry/trace/trace_id.h>

#include "a11/obs/internal/otel_internal.h"
#include "a11/obs/span.h"
#include "thread/boost_primitives.h"

#ifdef A11_WITH_OTLP_HTTP
#include <chrono>

#include "a11/obs/internal/otlp_http_exporter.h"
#endif

namespace otel = opentelemetry;
namespace otel_sdk = opentelemetry::sdk::trace;

namespace a11::obs {
namespace {

// Fiber-aware mutex (thread::) so that, when locked from an A11 fiber, the
// worker thread yields to other outstanding work instead of blocking.
thread::Mutex g_mu;
bool g_configured ABSL_GUARDED_BY(g_mu) = false;
std::shared_ptr<otel_sdk::TracerProvider> g_provider ABSL_GUARDED_BY(g_mu);
std::shared_ptr<otel::exporter::memory::InMemorySpanData> g_in_memory
    ABSL_GUARDED_BY(g_mu);
std::vector<std::string> g_baggage_keys ABSL_GUARDED_BY(g_mu);
// Accumulates finished spans across drains so repeated GetRecordedSpans() calls
// are additive rather than destructive.
std::vector<RecordedSpan> g_recorded ABSL_GUARDED_BY(g_mu);

SpanKind FromOtelKind(otel::trace::SpanKind kind) {
  switch (kind) {
    case otel::trace::SpanKind::kServer:
      return SpanKind::kServer;
    case otel::trace::SpanKind::kClient:
      return SpanKind::kClient;
    case otel::trace::SpanKind::kProducer:
      return SpanKind::kProducer;
    case otel::trace::SpanKind::kConsumer:
      return SpanKind::kConsumer;
    case otel::trace::SpanKind::kInternal:
    default:
      return SpanKind::kInternal;
  }
}

std::string AttributeToString(
    const otel::sdk::common::OwnedAttributeValue& value) {
  return otel::nostd::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
          return v;
        } else if constexpr (std::is_arithmetic_v<T>) {
          return absl::StrCat(v);
        } else {
          // Arrays and other composite attribute values are not needed by the
          // introspection callers; represent them compactly.
          return "[...]";
        }
      },
      value);
}

std::string TraceIdHex(const otel::trace::TraceId& id) {
  if (!id.IsValid()) {
    return "";
  }
  char buf[2 * otel::trace::TraceId::kSize];
  id.ToLowerBase16(buf);
  return std::string(buf, sizeof(buf));
}

std::string SpanIdHex(const otel::trace::SpanId& id) {
  if (!id.IsValid()) {
    return "";
  }
  char buf[2 * otel::trace::SpanId::kSize];
  id.ToLowerBase16(buf);
  return std::string(buf, sizeof(buf));
}

RecordedSpan Convert(const otel_sdk::SpanData& data) {
  RecordedSpan span;
  span.name = std::string(data.GetName());
  span.trace_id = TraceIdHex(data.GetTraceId());
  span.span_id = SpanIdHex(data.GetSpanId());
  span.parent_span_id = SpanIdHex(data.GetParentSpanId());
  span.kind = FromOtelKind(data.GetSpanKind());
  switch (data.GetStatus()) {
    case otel::trace::StatusCode::kOk:
      span.status_code = 1;
      break;
    case otel::trace::StatusCode::kError:
      span.status_code = 2;
      break;
    case otel::trace::StatusCode::kUnset:
    default:
      span.status_code = 0;
      break;
  }
  span.status_description = std::string(data.GetDescription());
  for (const auto& [key, value] : data.GetAttributes()) {
    span.attributes.emplace_back(key, AttributeToString(value));
  }
  for (const auto& event : data.GetEvents()) {
    RecordedEvent recorded;
    recorded.name = std::string(event.GetName());
    for (const auto& [key, value] : event.GetAttributes()) {
      recorded.attributes.emplace_back(key, AttributeToString(value));
    }
    span.events.push_back(std::move(recorded));
  }
  return span;
}

// Drains the in-memory buffer into g_recorded. Caller holds g_mu.
void DrainLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(g_mu) {
  if (g_in_memory == nullptr) {
    return;
  }
  for (const auto& data : g_in_memory->GetSpans()) {
    if (data != nullptr) {
      g_recorded.push_back(Convert(*data));
    }
  }
}

void ShutdownLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(g_mu) {
  if (g_provider != nullptr) {
    g_provider->ForceFlush();
    g_provider->Shutdown();
  }
  // The now shut-down provider stays installed as the global (its tracers
  // simply produce non-recording spans); a later Configure() replaces it.
  // Deliberately not setting a null provider, which would make
  // GetTracerProvider() return null for any ungated caller.
  g_provider.reset();
  g_in_memory.reset();
  g_baggage_keys.clear();
  g_configured = false;
}

}  // namespace

namespace internal {

otel::nostd::shared_ptr<otel::trace::Tracer> GlobalTracer() {
  return otel::trace::Provider::GetTracerProvider()->GetTracer("a11", "0.1.5");
}

std::shared_ptr<otel::exporter::memory::InMemorySpanData> InMemoryData() {
  thread::MutexLock lock(&g_mu);
  return g_in_memory;
}

void ForceFlush() {
  thread::MutexLock lock(&g_mu);
  if (g_provider != nullptr) {
    g_provider->ForceFlush();
  }
}

std::vector<std::string> PromotedBaggageKeys() {
  thread::MutexLock lock(&g_mu);
  return g_baggage_keys;
}

}  // namespace internal

absl::Status Configure(const ProviderOptions& options) {
  std::unique_ptr<otel_sdk::SpanExporter> exporter;
  std::shared_ptr<otel::exporter::memory::InMemorySpanData> in_memory;

  switch (options.exporter) {
    case ExporterKind::kNone:
      // Leave `exporter` null; handled below with a drop-everything processor.
      break;
    case ExporterKind::kInMemory: {
      auto memory_exporter =
          std::make_unique<otel::exporter::memory::InMemorySpanExporter>();
      in_memory = memory_exporter->GetData();
      exporter = std::move(memory_exporter);
      break;
    }
    case ExporterKind::kOstream:
      exporter = otel::exporter::trace::OStreamSpanExporterFactory::Create();
      break;
    case ExporterKind::kOtlpHttp: {
#ifdef A11_WITH_OTLP_HTTP
      if (options.otlp_endpoint.empty()) {
        return absl::InvalidArgumentError(
            "ExporterKind::kOtlpHttp requires a non-empty otlp_endpoint");
      }
      internal::OtlpHttpOptions otlp;
      otlp.endpoint = options.otlp_endpoint;
      otlp.headers = options.otlp_headers;
      otlp.timeout = std::chrono::milliseconds(options.otlp_timeout_millis);
      exporter = internal::MakeOtlpHttpJsonExporter(std::move(otlp));
#else
      return absl::UnimplementedError(
          "A11 was built without OTLP/HTTP support (A11_WITH_OTLP_HTTP=OFF)");
#endif
      break;
    }
  }

  if (exporter == nullptr) {
    return absl::InvalidArgumentError(
        "ExporterKind::kNone is not yet a valid provider configuration");
  }

  std::unique_ptr<otel_sdk::SpanProcessor> processor;
  if (options.use_simple_processor) {
    processor =
        otel_sdk::SimpleSpanProcessorFactory::Create(std::move(exporter));
  } else {
    processor = otel_sdk::BatchSpanProcessorFactory::Create(
        std::move(exporter), otel_sdk::BatchSpanProcessorOptions{});
  }

  otel::sdk::resource::ResourceAttributes attributes;
  attributes.SetAttribute("service.name", options.service_name);
  for (const auto& [key, value] : options.resource_attributes) {
    attributes.SetAttribute(key, value);
  }
  auto resource = otel::sdk::resource::Resource::Create(attributes);

  std::vector<std::unique_ptr<otel_sdk::SpanProcessor>> processors;
  processors.push_back(std::move(processor));
  auto context = otel_sdk::TracerContextFactory::Create(std::move(processors),
                                                        resource);
  auto provider =
      std::make_shared<otel_sdk::TracerProvider>(std::move(context));

  thread::MutexLock lock(&g_mu);
  if (g_configured) {
    ShutdownLocked();
  }
  g_provider = provider;
  g_in_memory = std::move(in_memory);
  g_baggage_keys = options.baggage_span_attributes;
  g_recorded.clear();
  otel::trace::Provider::SetTracerProvider(
      otel::nostd::shared_ptr<otel::trace::TracerProvider>(
          std::shared_ptr<otel::trace::TracerProvider>(provider)));
  g_configured = true;
  return absl::OkStatus();
}

void Shutdown() {
  thread::MutexLock lock(&g_mu);
  if (g_configured) {
    ShutdownLocked();
  }
  g_recorded.clear();
}

bool IsConfigured() {
  thread::MutexLock lock(&g_mu);
  return g_configured;
}

std::vector<RecordedSpan> GetRecordedSpans() {
  internal::ForceFlush();
  thread::MutexLock lock(&g_mu);
  DrainLocked();
  return g_recorded;
}

void ClearRecordedSpans() {
  thread::MutexLock lock(&g_mu);
  if (g_in_memory != nullptr) {
    g_in_memory->GetSpans();  // Discard buffered spans.
  }
  g_recorded.clear();
}

}  // namespace a11::obs

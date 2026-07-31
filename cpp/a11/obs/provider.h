// Copyright 2026 The A11 Authors.

#ifndef A11_OBS_PROVIDER_H_
#define A11_OBS_PROVIDER_H_

#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>

#include "a11/obs/span.h"

namespace a11::obs {

// Span exporters available in the tracing core.
enum class ExporterKind {
  kNone,      // Drop spans (tracing effectively off, but context still flows).
  kInMemory,  // Retain finished spans for GetRecordedSpans(); used by tests.
  kOstream,   // Human-readable dump to stderr; used for local debugging.
  kOtlpHttp,  // Native OTLP/HTTP (JSON) export to an OTLP-compatible backend
              // such as Langfuse. Requires the build option A11_WITH_OTLP_HTTP.
};

struct ProviderOptions {
  std::string service_name = "a11";
  std::vector<std::pair<std::string, std::string>> resource_attributes;
  ExporterKind exporter = ExporterKind::kNone;
  // Simple (synchronous) processing exports each span as it ends -- convenient
  // and deterministic for tests. Batch processing is used otherwise.
  bool use_simple_processor = false;

  // OTLP/HTTP options (only used when exporter == kOtlpHttp).
  std::string otlp_endpoint;  // Full traces URL.
  std::vector<std::pair<std::string, std::string>> otlp_headers;  // e.g. auth.
  int otlp_timeout_millis = 10000;

  // Baggage keys promoted onto every span as attributes (of the same name).
  // This is how request-scoped values set upstream via the x-otel-baggage
  // header -- e.g. langfuse.session.id -- become span attributes that a
  // backend can read, while still propagating across nested/remote actions.
  std::vector<std::string> baggage_span_attributes;
};

// Installs the global tracer provider. Idempotent replacement: a second call
// shuts down the previous provider first. Tracing stays off (Tracer returns
// inactive spans) until this succeeds.
absl::Status Configure(const ProviderOptions& options);

// Flushes and tears down the global provider, restoring the inactive state.
void Shutdown();

[[nodiscard]] bool IsConfigured();

// --- Introspection (meaningful only with ExporterKind::kInMemory) ---------

struct RecordedEvent {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
};

struct RecordedSpan {
  std::string name;
  std::string trace_id;         // 32 lowercase hex chars.
  std::string span_id;          // 16 lowercase hex chars.
  std::string parent_span_id;   // 16 lowercase hex chars, or empty if a root.
  SpanKind kind = SpanKind::kInternal;
  int status_code = 0;          // 0 unset, 1 ok, 2 error.
  std::string status_description;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<RecordedEvent> events;
};

// Returns finished spans captured by the in-memory exporter, oldest first.
std::vector<RecordedSpan> GetRecordedSpans();

// Clears the in-memory span buffer.
void ClearRecordedSpans();

}  // namespace a11::obs

#endif  // A11_OBS_PROVIDER_H_

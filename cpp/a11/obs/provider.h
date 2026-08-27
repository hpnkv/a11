// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Process-wide tracing configuration and in-memory trace inspection.
 */

#ifndef A11_OBS_PROVIDER_H_
#define A11_OBS_PROVIDER_H_

#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>

#include "a11/obs/span.h"

namespace a11::obs {

/// Destination used for spans produced by actions, sessions, and transports.
enum class ExporterKind {
  kNone,      ///< Unconfigured sentinel; Configure() rejects this value.
  kInMemory,  ///< Retain finished spans for GetRecordedSpans(); useful in tests.
  kOstream,   ///< Human-readable output for local debugging.
  /// Native OTLP/HTTP export; requires the A11_WITH_OTLP_HTTP build option.
  kOtlpHttp,
};

/** Configure how an A11 process records and exports agent traces. */
struct ProviderOptions {
  std::string service_name = "a11";  ///< OTel service.name resource value.
  /// Additional attributes attached to the process-level OTel resource.
  std::vector<std::pair<std::string, std::string>> resource_attributes;
  /// Span destination; must not remain kNone when passed to Configure().
  ExporterKind exporter = ExporterKind::kNone;
  // Simple (synchronous) processing exports each span as it ends -- convenient
  // and deterministic for tests. Batch processing is used otherwise.
  bool use_simple_processor = false;  ///< Export synchronously as spans end.

  // OTLP/HTTP options (only used when exporter == kOtlpHttp).
  std::string otlp_endpoint;  ///< Full OTLP/HTTP traces URL.
  /// Request headers such as backend authentication.
  std::vector<std::pair<std::string, std::string>> otlp_headers;
  int otlp_timeout_millis = 10000;  ///< Export request timeout.

  // Baggage keys promoted onto every span as attributes (of the same name).
  /// Baggage keys promoted to same-named span attributes.
  std::vector<std::string> baggage_span_attributes;
};

/// Install or replace the process-wide tracer provider.
/// Tracing remains inactive until this returns OK.
absl::Status Configure(const ProviderOptions& options);

/// Flush pending spans and restore the process to its unconfigured state.
void Shutdown();

/// Whether a tracer provider is currently installed.
[[nodiscard]] bool IsConfigured();

// --- Introspection (meaningful only with ExporterKind::kInMemory) ---------

/// Event captured on a span by the in-memory exporter.
struct RecordedEvent {
  std::string name;  ///< Event name.
  std::vector<std::pair<std::string, std::string>>
      attributes;  ///< Snapshot attributes.
};

/// Finished span snapshot returned by the in-memory exporter.
struct RecordedSpan {
  std::string name;            ///< Final span name.
  std::string trace_id;        ///< 32 lowercase hex characters.
  std::string span_id;         ///< 16 lowercase hex characters.
  std::string parent_span_id;  ///< Parent id, or empty for a root span.
  SpanKind kind = SpanKind::kInternal;  ///< OTel relationship kind.
  int status_code = 0;             ///< OTel status: 0 unset, 1 OK, 2 error.
  std::string status_description;  ///< Optional error explanation.
  std::vector<std::pair<std::string, std::string>>
      attributes;                     ///< Final attributes.
  std::vector<RecordedEvent> events;  ///< Events in recording order.
};

/// Return in-memory spans oldest first; meaningful for kInMemory only.
std::vector<RecordedSpan> GetRecordedSpans();

/// Clear spans retained by the in-memory exporter.
void ClearRecordedSpans();

}  // namespace a11::obs

#endif  // A11_OBS_PROVIDER_H_

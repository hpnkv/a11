// Copyright 2026 The A11 Authors.

#ifndef A11_OBS_TRACE_CONTEXT_H_
#define A11_OBS_TRACE_CONTEXT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/data/types.h"

namespace a11::obs {

/// Reserved action header carrying the W3C traceparent value.
inline constexpr std::string_view kTraceparentHeader = "x-otel-traceparent";
/// Reserved action header carrying the W3C tracestate value.
inline constexpr std::string_view kTracestateHeader = "x-otel-tracestate";
/// Reserved action header carrying request-scoped W3C baggage.
inline constexpr std::string_view kBaggageHeader = "x-otel-baggage";

/// One decoded W3C baggage member propagated through nested and remote actions.
struct BaggageEntry {
  std::string key;    ///< Trimmed member key; percent escapes stay encoded.
  std::string value;  ///< Percent-decoded member value.
  std::string properties;  ///< Trailing semicolon-separated properties.

  friend bool operator==(const BaggageEntry&, const BaggageEntry&) = default;
};

/// SDK-independent W3C parent context recovered from reserved action headers.
struct TraceContext {
  std::string trace_id;               ///< 32 lowercase hex characters.
  std::string span_id;                ///< 16 lowercase hex characters.
  std::uint8_t trace_flags = 0;       ///< W3C trace flags byte.
  std::string tracestate;             ///< Raw tracestate header; may be empty.
  std::vector<BaggageEntry> baggage;  ///< Request-scoped propagated values.

  /// Whether the W3C sampled flag is set.
  [[nodiscard]] bool sampled() const { return (trace_flags & 0x01U) != 0; }

  friend bool operator==(const TraceContext&, const TraceContext&) = default;
};

/// Extract a remote parent; nullopt means no tracing headers were supplied.
/// Malformed or inconsistent reserved headers return an error so an action
/// cannot silently detach from the caller's requested trace.
absl::StatusOr<std::optional<TraceContext>> ExtractTraceContext(
    const data::ByteMap& headers);

/// Serialize @p context into reserved headers, replacing existing values.
absl::Status InjectTraceContext(const TraceContext& context,
                                data::ByteMap& headers);

}  // namespace a11::obs

#endif  // A11_OBS_TRACE_CONTEXT_H_

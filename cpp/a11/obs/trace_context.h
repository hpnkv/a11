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

// Reserved W3C trace-context header names. These mirror the action header slots
// declared in the Python layer (a11/actions/action.py DefaultHeaders) and are
// the sole channel A11 uses to propagate trace context across the C++/Python
// boundary and across the wire.
inline constexpr std::string_view kTraceparentHeader = "x-otel-traceparent";
inline constexpr std::string_view kTracestateHeader = "x-otel-tracestate";
inline constexpr std::string_view kBaggageHeader = "x-otel-baggage";

// A single W3C baggage member. `key` and `value` are already percent-decoded;
// `properties` holds any trailing ";"-separated properties verbatim.
struct BaggageEntry {
  std::string key;
  std::string value;
  std::string properties;

  friend bool operator==(const BaggageEntry&, const BaggageEntry&) = default;
};

// Trace context recovered from the reserved headers. Holds only serialized W3C
// values: no OpenTelemetry type is exposed here, so callers never need to
// include the OTel SDK.
struct TraceContext {
  std::string trace_id;    // 32 lowercase hex chars.
  std::string span_id;     // 16 lowercase hex chars.
  std::uint8_t trace_flags = 0;
  std::string tracestate;  // Raw tracestate header value; may be empty.
  std::vector<BaggageEntry> baggage;

  [[nodiscard]] bool sampled() const { return (trace_flags & 0x01U) != 0; }

  friend bool operator==(const TraceContext&, const TraceContext&) = default;
};

// Extracts trace context from the reserved headers. The three possible
// outcomes map directly onto A11's telemetry contract:
//   * error         -> a reserved header is present but malformed, or the set
//                      is internally inconsistent (e.g. tracestate/baggage
//                      without a traceparent). The caller MUST fail.
//   * std::nullopt  -> no reserved header is present. Emit no telemetry.
//   * TraceContext  -> a valid remote parent context to continue.
absl::StatusOr<std::optional<TraceContext>> ExtractTraceContext(
    const data::ByteMap& headers);

// Serializes `context` into the reserved headers, overwriting any existing
// reserved values. Baggage is only written when non-empty.
absl::Status InjectTraceContext(const TraceContext& context,
                                data::ByteMap& headers);

}  // namespace a11::obs

#endif  // A11_OBS_TRACE_CONTEXT_H_

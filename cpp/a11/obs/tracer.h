// Copyright 2026 The A11 Authors.

#ifndef A11_OBS_TRACER_H_
#define A11_OBS_TRACER_H_

#include <string_view>

#include "a11/obs/span.h"
#include "a11/obs/trace_context.h"

namespace a11::obs {

/**
 * @brief Factory that makes parentage explicit for A11's migrating fibers.
 *
 * A remote TraceContext or in-process Span is always supplied explicitly;
 * OpenTelemetry's thread-local current span cannot represent handlers that
 * migrate between workers. Every method returns an inactive Span when tracing
 * is unconfigured, so instrumentation needs no conditional path.
 */
class Tracer {
 public:
  // Starts a span continuing a remote parent recovered from headers.
  /// Continue @p parent, or begin a root when it is null or empty.
  static Span StartSpan(std::string_view name, SpanKind kind,
                        const TraceContext* parent);

  // Starts a span parented to an in-process span, inheriting its baggage.
  /// Start an in-process child and inherit the parent's baggage.
  static Span StartChildSpan(std::string_view name, SpanKind kind,
                             const Span& parent);

  // Starts a root span for a session or stream, optionally pinning the trace id
  // (32 lowercase hex chars).
  /// Start a root span, optionally using a preassigned 32-character trace id.
  static Span StartRootSpan(std::string_view name, SpanKind kind,
                            std::string_view preassigned_trace_id = {});
};

}  // namespace a11::obs

#endif  // A11_OBS_TRACER_H_

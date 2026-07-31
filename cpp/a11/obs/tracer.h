// Copyright 2026 The A11 Authors.

#ifndef A11_OBS_TRACER_H_
#define A11_OBS_TRACER_H_

#include <string_view>

#include "a11/obs/span.h"
#include "a11/obs/trace_context.h"

namespace a11::obs {

// Factory for spans. Every method returns an inactive Span when tracing is not
// configured (see provider.h), so call sites never branch on configuration.
//
// A11 runs handlers on migrating fibers, so OpenTelemetry's implicit
// thread-local "current span" is unusable here. Parentage is therefore always
// passed explicitly -- either as a remote TraceContext recovered from headers
// or as an in-process parent Span.
class Tracer {
 public:
  // Starts a span continuing a remote parent recovered from headers. When
  // `parent` is null (or points at an empty context), the span begins a fresh
  // root trace. Baggage carried by the parent is retained for propagation.
  static Span StartSpan(std::string_view name, SpanKind kind,
                        const TraceContext* parent);

  // Starts a span parented to an in-process span, inheriting its baggage.
  static Span StartChildSpan(std::string_view name, SpanKind kind,
                             const Span& parent);

  // Starts a root span for a session or stream, optionally pinning the trace
  // id (32 lowercase hex chars). An empty `preassigned_trace_id` lets the SDK
  // generate one. This is the implementation-level hook for preassigning trace
  // ids without widening any public transport interface.
  static Span StartRootSpan(std::string_view name, SpanKind kind,
                            std::string_view preassigned_trace_id = {});
};

}  // namespace a11::obs

#endif  // A11_OBS_TRACER_H_

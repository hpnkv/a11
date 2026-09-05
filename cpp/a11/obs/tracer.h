/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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

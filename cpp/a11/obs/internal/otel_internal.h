// Copyright 2026 The A11 Authors.

// Internal bridge between A11's OTel-free facade and the OpenTelemetry SDK.
// This header pulls in OTel types and is therefore never installed or included
// by any public A11 header. Only obs implementation files use it.

#ifndef A11_OBS_INTERNAL_OTEL_INTERNAL_H_
#define A11_OBS_INTERNAL_OTEL_INTERNAL_H_

#include <memory>
#include <string>
#include <vector>

#include <opentelemetry/exporters/memory/in_memory_span_data.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/tracer.h>

namespace a11::obs::internal {

// The tracer used to mint spans. Only valid when Configure() has succeeded;
// callers gate on IsConfigured() first.
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> GlobalTracer();

// The in-memory span buffer, or nullptr unless the in-memory exporter is
// active. Used only by the introspection API.
std::shared_ptr<opentelemetry::exporter::memory::InMemorySpanData>
InMemoryData();

// Best-effort flush of the active provider so finished spans reach exporters.
void ForceFlush();

// Snapshot of the baggage keys configured to be promoted to span attributes.
std::vector<std::string> PromotedBaggageKeys();

}  // namespace a11::obs::internal

#endif  // A11_OBS_INTERNAL_OTEL_INTERNAL_H_

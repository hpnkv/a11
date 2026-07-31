// Copyright 2026 The A11 Authors.

#include "a11/obs/tracer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/random/random.h>
#include <absl/status/status.h>
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/common/key_value_iterable_view.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/span.h>
#include <opentelemetry/nostd/string_view.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/span_id.h>
#include <opentelemetry/trace/span_metadata.h>
#include <opentelemetry/trace/span_startoptions.h>
#include <opentelemetry/trace/trace_flags.h>
#include <opentelemetry/trace/trace_id.h>
#include <opentelemetry/trace/trace_state.h>
#include <opentelemetry/trace/tracer.h>

#include "a11/obs/internal/otel_internal.h"
#include "a11/obs/provider.h"
#include "a11/obs/span.h"
#include "a11/obs/trace_context.h"

namespace otel = opentelemetry;

namespace a11::obs {

// Holds the live OpenTelemetry span plus the baggage inherited from the parent
// so it can be re-propagated to children. Kept small enough to live inside
// Span's inline storage.
struct Span::Impl {
  otel::nostd::shared_ptr<otel::trace::Span> span;
  std::vector<BaggageEntry> baggage;
};

namespace {

constexpr std::string_view kLowerHex = "0123456789abcdef";

int HexNibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return 0;
}

void HexDecode(std::string_view hex, std::uint8_t* out, size_t out_size) {
  for (size_t i = 0; i < out_size && (2 * i + 1) < hex.size(); ++i) {
    out[i] = static_cast<std::uint8_t>((HexNibble(hex[2 * i]) << 4) |
                                       HexNibble(hex[2 * i + 1]));
  }
}

otel::trace::TraceId TraceIdFromHex(std::string_view hex) {
  std::uint8_t buf[otel::trace::TraceId::kSize] = {};
  HexDecode(hex, buf, sizeof(buf));
  return otel::trace::TraceId(
      otel::nostd::span<const std::uint8_t, otel::trace::TraceId::kSize>(
          buf, sizeof(buf)));
}

otel::trace::SpanId SpanIdFromHex(std::string_view hex) {
  std::uint8_t buf[otel::trace::SpanId::kSize] = {};
  HexDecode(hex, buf, sizeof(buf));
  return otel::trace::SpanId(
      otel::nostd::span<const std::uint8_t, otel::trace::SpanId::kSize>(
          buf, sizeof(buf)));
}

otel::trace::SpanId RandomSpanId() {
  thread_local absl::BitGen bitgen;
  std::uint8_t buf[otel::trace::SpanId::kSize];
  for (auto& byte : buf) {
    byte =
        static_cast<std::uint8_t>(absl::Uniform<unsigned int>(bitgen, 0, 256));
  }
  return otel::trace::SpanId(
      otel::nostd::span<const std::uint8_t, otel::trace::SpanId::kSize>(
          buf, sizeof(buf)));
}

std::string TraceIdToHex(const otel::trace::TraceId& id) {
  char buf[2 * otel::trace::TraceId::kSize];
  id.ToLowerBase16(buf);
  return std::string(buf, sizeof(buf));
}

std::string SpanIdToHex(const otel::trace::SpanId& id) {
  char buf[2 * otel::trace::SpanId::kSize];
  id.ToLowerBase16(buf);
  return std::string(buf, sizeof(buf));
}

otel::trace::SpanKind ToOtelKind(SpanKind kind) {
  switch (kind) {
    case SpanKind::kServer:
      return otel::trace::SpanKind::kServer;
    case SpanKind::kClient:
      return otel::trace::SpanKind::kClient;
    case SpanKind::kProducer:
      return otel::trace::SpanKind::kProducer;
    case SpanKind::kConsumer:
      return otel::trace::SpanKind::kConsumer;
    case SpanKind::kInternal:
    default:
      return otel::trace::SpanKind::kInternal;
  }
}

// Copies the configured baggage keys onto `span` as attributes of the same
// name. This is what surfaces request-scoped baggage (e.g. langfuse.session.id)
// where a tracing backend can read it, while baggage still propagates onward.
void PromoteBaggageAttributes(Span& span,
                              const std::vector<BaggageEntry>& baggage) {
  if (baggage.empty() || !span.IsRecording()) {
    return;
  }
  const std::vector<std::string> keys = internal::PromotedBaggageKeys();
  if (keys.empty()) {
    return;
  }
  for (const BaggageEntry& entry : baggage) {
    if (std::find(keys.begin(), keys.end(), entry.key) != keys.end()) {
      span.SetAttribute(entry.key, entry.value);
    }
  }
}

}  // namespace

// --- Span -----------------------------------------------------------------

Span::Impl* Span::impl() noexcept {
  return std::launder(reinterpret_cast<Impl*>(&storage_));
}

const Span::Impl* Span::impl() const noexcept {
  return std::launder(reinterpret_cast<const Impl*>(&storage_));
}

Span::Impl* Span::Engage() noexcept {
  // Verified here (a member, with access to the private nested type) that the
  // inline storage is large and aligned enough for Impl.
  static_assert(sizeof(Impl) <= kStorageSize,
                "Span inline storage is too small for its implementation");
  static_assert(alignof(Impl) <= kStorageAlign,
                "Span inline storage is under-aligned for its implementation");
  ::new (static_cast<void*>(&storage_)) Impl();
  engaged_ = true;
  return impl();
}

void Span::Reset() noexcept {
  if (engaged_) {
    impl()->~Impl();
    engaged_ = false;
  }
}

Span::~Span() {
  End();
  Reset();
}

Span::Span(Span&& other) noexcept {
  if (other.engaged_) {
    ::new (static_cast<void*>(&storage_)) Impl(std::move(*other.impl()));
    engaged_ = true;
    other.Reset();
  }
}

Span& Span::operator=(Span&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  End();
  Reset();
  if (other.engaged_) {
    ::new (static_cast<void*>(&storage_)) Impl(std::move(*other.impl()));
    engaged_ = true;
    other.Reset();
  }
  return *this;
}

bool Span::IsRecording() const noexcept {
  return engaged_ && impl()->span && impl()->span->IsRecording();
}

void Span::SetAttribute(std::string_view key, std::string_view value) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  impl()->span->SetAttribute(key, value);
}

void Span::SetAttribute(std::string_view key, const char* value) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  impl()->span->SetAttribute(key, value);
}

void Span::SetAttribute(std::string_view key, std::int64_t value) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  impl()->span->SetAttribute(key, value);
}

void Span::SetAttribute(std::string_view key, bool value) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  impl()->span->SetAttribute(key, value);
}

void Span::SetAttribute(std::string_view key, double value) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  impl()->span->SetAttribute(key, value);
}

void Span::AddEvent(std::string_view name) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  impl()->span->AddEvent(name);
}

void Span::AddEvent(
    std::string_view name,
    const std::vector<std::pair<std::string, std::string>>& attributes) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  std::vector<std::pair<std::string, otel::common::AttributeValue>> kv;
  kv.reserve(attributes.size());
  for (const auto& [key, value] : attributes) {
    kv.emplace_back(key, otel::common::AttributeValue(std::string_view(value)));
  }
  impl()->span->AddEvent(
      name,
      otel::common::KeyValueIterableView<
          std::vector<std::pair<std::string, otel::common::AttributeValue>>>(
          kv));
}

void Span::SetStatus(const absl::Status& status) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  if (status.ok()) {
    impl()->span->SetStatus(otel::trace::StatusCode::kOk);
  } else {
    impl()->span->SetStatus(otel::trace::StatusCode::kError,
                            std::string(status.message()));
  }
}

void Span::SetStatus(SpanStatus status, std::string_view description) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  switch (status) {
    case SpanStatus::kOk:
      impl()->span->SetStatus(otel::trace::StatusCode::kOk, description);
      break;
    case SpanStatus::kError:
      impl()->span->SetStatus(otel::trace::StatusCode::kError, description);
      break;
    case SpanStatus::kUnset:
      impl()->span->SetStatus(otel::trace::StatusCode::kUnset, description);
      break;
  }
}

void Span::UpdateName(std::string_view name) {
  if (!engaged_ || !impl()->span) {
    return;
  }
  impl()->span->UpdateName(name);
}

void Span::End() noexcept {
  if (engaged_ && impl()->span) {
    impl()->span->End();
    impl()->span = nullptr;
  }
}

std::string Span::TraceIdHex() const {
  if (!engaged_ || !impl()->span) {
    return "";
  }
  const otel::trace::SpanContext sc = impl()->span->GetContext();
  return sc.IsValid() ? TraceIdToHex(sc.trace_id()) : "";
}

std::string Span::SpanIdHex() const {
  if (!engaged_ || !impl()->span) {
    return "";
  }
  const otel::trace::SpanContext sc = impl()->span->GetContext();
  return sc.IsValid() ? SpanIdToHex(sc.span_id()) : "";
}

absl::Status Span::InjectContext(data::ByteMap& headers) const {
  if (!engaged_ || !impl()->span) {
    return absl::OkStatus();
  }
  const otel::trace::SpanContext sc = impl()->span->GetContext();
  if (!sc.IsValid()) {
    return absl::OkStatus();
  }
  TraceContext context;
  context.trace_id = TraceIdToHex(sc.trace_id());
  context.span_id = SpanIdToHex(sc.span_id());
  context.trace_flags = sc.trace_flags().flags();
  context.tracestate = sc.trace_state()->ToHeader();
  context.baggage = impl()->baggage;
  return InjectTraceContext(context, headers);
}

// --- Tracer ---------------------------------------------------------------

Span Tracer::StartSpan(std::string_view name, SpanKind kind,
                       const TraceContext* parent) {
  if (!IsConfigured()) {
    return Span{};
  }
  otel::trace::StartSpanOptions options;
  options.kind = ToOtelKind(kind);
  std::vector<BaggageEntry> baggage;
  if (parent != nullptr && parent->trace_id.size() == 32) {
    options.parent = otel::trace::SpanContext(
        TraceIdFromHex(parent->trace_id), SpanIdFromHex(parent->span_id),
        otel::trace::TraceFlags(parent->trace_flags), /*is_remote=*/true,
        otel::trace::TraceState::FromHeader(parent->tracestate));
    baggage = parent->baggage;
  }
  Span span;
  Span::Impl* impl = span.Engage();
  impl->span = internal::GlobalTracer()->StartSpan(name, options);
  impl->baggage = std::move(baggage);
  PromoteBaggageAttributes(span, impl->baggage);
  return span;
}

Span Tracer::StartChildSpan(std::string_view name, SpanKind kind,
                            const Span& parent) {
  if (!IsConfigured() || !parent.engaged_ || !parent.impl()->span) {
    return Span{};
  }
  otel::trace::StartSpanOptions options;
  options.kind = ToOtelKind(kind);
  options.parent = parent.impl()->span->GetContext();
  Span span;
  Span::Impl* impl = span.Engage();
  impl->span = internal::GlobalTracer()->StartSpan(name, options);
  impl->baggage = parent.impl()->baggage;
  PromoteBaggageAttributes(span, impl->baggage);
  return span;
}

Span Tracer::StartRootSpan(std::string_view name, SpanKind kind,
                           std::string_view preassigned_trace_id) {
  if (!IsConfigured()) {
    return Span{};
  }
  otel::trace::StartSpanOptions options;
  options.kind = ToOtelKind(kind);
  if (preassigned_trace_id.size() == 32) {
    // Pin the trace id by parenting the span to a synthetic (non-exported)
    // context carrying the desired trace id and a fresh span id.
    options.parent = otel::trace::SpanContext(
        TraceIdFromHex(preassigned_trace_id), RandomSpanId(),
        otel::trace::TraceFlags(otel::trace::TraceFlags::kIsSampled),
        /*is_remote=*/false, otel::trace::TraceState::GetDefault());
  }
  Span span;
  Span::Impl* impl = span.Engage();
  impl->span = internal::GlobalTracer()->StartSpan(name, options);
  return span;
}

}  // namespace a11::obs

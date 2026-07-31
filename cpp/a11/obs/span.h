// Copyright 2026 The A11 Authors.

#ifndef A11_OBS_SPAN_H_
#define A11_OBS_SPAN_H_

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>

#include "a11/data/types.h"

namespace a11::obs {

// OpenTelemetry span kinds, mirrored so this header stays free of OTel types.
enum class SpanKind { kInternal, kServer, kClient, kProducer, kConsumer };

// OpenTelemetry span status codes, mirrored to keep OTel out of this header.
enum class SpanStatus { kUnset, kOk, kError };

// Move-only RAII handle to a tracing span.
//
// A default-constructed or moved-from Span is "inactive": IsRecording() is
// false and every mutator is a no-op. Tracer factory methods also return an
// inactive Span when tracing is not configured, so instrumentation call sites
// need no conditionals. The span is ended on End() (idempotent) or on
// destruction.
//
// The OpenTelemetry span is held in a fixed-size inline buffer -- a "fast
// pImpl". This keeps every OTel type out of this header (satisfying the rule
// that A11's public headers never expose third-party implementation types) and
// avoids a per-span heap allocation. tracer.cc static_asserts that the buffer
// is large enough and suitably aligned for the implementation object.
class Span {
 public:
  Span() noexcept = default;
  ~Span();

  Span(Span&& other) noexcept;
  Span& operator=(Span&& other) noexcept;
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

  [[nodiscard]] bool IsRecording() const noexcept;

  // This span's identifiers as lowercase hex (32 chars / 16 chars), or empty
  // strings for an inactive span or an invalid context.
  [[nodiscard]] std::string TraceIdHex() const;
  [[nodiscard]] std::string SpanIdHex() const;

  void SetAttribute(std::string_view key, std::string_view value);
  void SetAttribute(std::string_view key, const char* value);
  void SetAttribute(std::string_view key, std::int64_t value);
  void SetAttribute(std::string_view key, bool value);
  void SetAttribute(std::string_view key, double value);

  void AddEvent(std::string_view name);
  void AddEvent(
      std::string_view name,
      const std::vector<std::pair<std::string, std::string>>& attributes);

  // Records the span's outcome from an absl::Status: Ok clears to a non-error
  // status, any error maps to the OTel error status with the message.
  void SetStatus(const absl::Status& status);

  // Sets the span status explicitly. `description` is used for kError.
  void SetStatus(SpanStatus status, std::string_view description = {});

  // Renames the span (OTel UpdateName). The exported name is whatever it is at
  // End(). No-op for an inactive span.
  void UpdateName(std::string_view name);

  void End() noexcept;

  // Serializes this span's context (and any inherited baggage) into the
  // reserved headers so nested or remote actions continue the same trace.
  // A no-op that returns OkStatus for an inactive span.
  absl::Status InjectContext(data::ByteMap& headers) const;

 private:
  friend class Tracer;

  struct Impl;
  // Sized/aligned to hold Impl (a shared span handle plus a small string).
  // Verified with static_assert in tracer.cc.
  static constexpr std::size_t kStorageSize = 64;
  static constexpr std::size_t kStorageAlign = alignof(std::max_align_t);

  // Defined in tracer.cc, where Impl is a complete type (std::launder and
  // Impl's members require completeness).
  [[nodiscard]] Impl* impl() noexcept;
  [[nodiscard]] const Impl* impl() const noexcept;
  // Default-constructs an Impl in the inline storage, marks the span engaged,
  // and returns it for the tracer to populate.
  Impl* Engage() noexcept;
  void Reset() noexcept;

  alignas(kStorageAlign) unsigned char storage_[kStorageSize];
  bool engaged_ = false;
};

}  // namespace a11::obs

#endif  // A11_OBS_SPAN_H_

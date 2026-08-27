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

/// OpenTelemetry span relationships, mirrored to keep OTel out of this API.
enum class SpanKind { kInternal, kServer, kClient, kProducer, kConsumer };

/// OpenTelemetry outcome states, mirrored to keep OTel out of this API.
enum class SpanStatus { kUnset, kOk, kError };

/**
 * @brief Move-only RAII handle to one action, session, or transport span.
 *
 * A default-constructed or moved-from Span is inactive: IsRecording() is
 * false and every mutator is a no-op. Tracer also returns an inactive span
 * when tracing is not configured, so agent runtime call sites need no feature
 * branches. End() is idempotent, and destruction ends a live span.
 *
 * OpenTelemetry types remain hidden in fixed inline storage so this public
 * header does not expose the SDK or allocate a separate pImpl for every span.
 */
class Span {
 public:
  Span() noexcept = default;
  ~Span();

  /// Transfer ownership without ending the span.
  Span(Span&& other) noexcept;
  /// End the currently owned span, then transfer @p other.
  Span& operator=(Span&& other) noexcept;
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

  /// Whether this handle currently records attributes and events.
  [[nodiscard]] bool IsRecording() const noexcept;

  // This span's identifiers as lowercase hex (32 chars / 16 chars), or empty
  // strings for an inactive span or an invalid context.
  /// Return the 32-character trace id, or empty when inactive.
  [[nodiscard]] std::string TraceIdHex() const;
  /// Return the 16-character span id, or empty when inactive.
  [[nodiscard]] std::string SpanIdHex() const;

  /// Set or replace a string attribute on the live span.
  void SetAttribute(std::string_view key, std::string_view value);
  /// Set or replace a C-string attribute on the live span.
  void SetAttribute(std::string_view key, const char* value);
  /// Set or replace an integer attribute on the live span.
  void SetAttribute(std::string_view key, std::int64_t value);
  /// Set or replace a boolean attribute on the live span.
  void SetAttribute(std::string_view key, bool value);
  /// Set or replace a floating-point attribute on the live span.
  void SetAttribute(std::string_view key, double value);

  /// Record a named point-in-time event.
  void AddEvent(std::string_view name);
  /// Record an event with string attributes.
  void AddEvent(
      std::string_view name,
      const std::vector<std::pair<std::string, std::string>>& attributes);

  /// Map an A11 operation status onto the span's OTel outcome.
  void SetStatus(const absl::Status& status);

  /// Set an explicit OTel outcome; @p description explains kError.
  void SetStatus(SpanStatus status, std::string_view description = {});

  // Renames the span (OTel UpdateName). The exported name is whatever it is at
  // End(). No-op for an inactive span.
  /// Replace the name that will be exported when the span ends.
  void UpdateName(std::string_view name);

  /// Finish the span and release its implementation; safe to call repeatedly.
  void End() noexcept;

  // Serializes this span's context (and any inherited baggage) into the
  // reserved headers so nested or remote actions continue the same trace.
  /// Inject trace context and inherited baggage into reserved A11 headers.
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

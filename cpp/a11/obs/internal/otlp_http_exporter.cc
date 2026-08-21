// Copyright 2026 The A11 Authors.

#include "a11/obs/internal/otlp_http_exporter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/log/log.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <opentelemetry/nostd/span.h>
#include <opentelemetry/nostd/variant.h>
#include <opentelemetry/sdk/common/attribute_utils.h>
#include <opentelemetry/sdk/common/exporter_utils.h>
#include <opentelemetry/sdk/common/global_log_handler.h>
#include <opentelemetry/sdk/trace/exporter.h>
#include <opentelemetry/sdk/trace/recordable.h>
#include <opentelemetry/sdk/trace/span_data.h>
#include <opentelemetry/trace/span_id.h>
#include <opentelemetry/trace/span_metadata.h>
#include <opentelemetry/trace/trace_id.h>

#include "a11/json_codec.h"

namespace otel = opentelemetry;
namespace otel_sdk = opentelemetry::sdk::trace;
using json = nlohmann::json;

namespace a11::obs::internal {
namespace {

void EnsureCurlGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string TraceIdHex(const otel::trace::TraceId& id) {
  char buf[2 * otel::trace::TraceId::kSize];
  id.ToLowerBase16(buf);
  return {buf, sizeof(buf)};
}

std::string SpanIdHex(const otel::trace::SpanId& id) {
  char buf[2 * otel::trace::SpanId::kSize];
  id.ToLowerBase16(buf);
  return {buf, sizeof(buf)};
}

int OtlpKind(otel::trace::SpanKind kind) {
  switch (kind) {
    case otel::trace::SpanKind::kInternal:
      return 1;
    case otel::trace::SpanKind::kServer:
      return 2;
    case otel::trace::SpanKind::kClient:
      return 3;
    case otel::trace::SpanKind::kProducer:
      return 4;
    case otel::trace::SpanKind::kConsumer:
      return 5;
    default:
      return 0;
  }
}

int OtlpStatus(otel::trace::StatusCode code) {
  switch (code) {
    case otel::trace::StatusCode::kOk:
      return 1;
    case otel::trace::StatusCode::kError:
      return 2;
    default:
      return 0;
  }
}

std::string UnixNanos(const otel::common::SystemTimestamp& timestamp) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         timestamp.time_since_epoch())
                         .count();
  return std::to_string(nanos);
}

// Converts an OTel owned attribute value into an OTLP AnyValue JSON object.
json AnyValue(const otel::sdk::common::OwnedAttributeValue& value) {
  return otel::nostd::visit(
      [](const auto& v) -> json {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
          return json{{"boolValue", v}};
        } else if constexpr (std::is_same_v<T, std::string>) {
          return json{{"stringValue", v}};
        } else if constexpr (std::is_floating_point_v<T>) {
          return json{{"doubleValue", static_cast<double>(v)}};
        } else if constexpr (std::is_integral_v<T>) {
          // OTLP/JSON encodes 64-bit integers as strings.
          const auto wide = static_cast<std::int64_t>(v);
          return json{{"intValue", std::to_string(wide)}};
        } else {
          return json{{"stringValue", "[unsupported]"}};
        }
      },
      value);
}

template <typename Map>
json KeyValueList(const Map& attributes) {
  json list = json::array();
  for (const auto& [key, value] : attributes) {
    list.push_back(json{{"key", key}, {"value", AnyValue(value)}});
  }
  return list;
}

json SpanToJson(const otel_sdk::SpanData& span) {
  json out;
  out["traceId"] = TraceIdHex(span.GetTraceId());
  out["spanId"] = SpanIdHex(span.GetSpanId());
  if (span.GetParentSpanId().IsValid()) {
    out["parentSpanId"] = SpanIdHex(span.GetParentSpanId());
  }
  out["name"] = std::string(span.GetName());
  out["kind"] = OtlpKind(span.GetSpanKind());
  out["startTimeUnixNano"] = UnixNanos(span.GetStartTime());
  out["endTimeUnixNano"] = UnixNanos(otel::common::SystemTimestamp(
      span.GetStartTime().time_since_epoch() + span.GetDuration()));
  out["attributes"] = KeyValueList(span.GetAttributes());

  json events = json::array();
  for (const auto& event : span.GetEvents()) {
    events.push_back(json{{"name", std::string(event.GetName())},
                          {"timeUnixNano", UnixNanos(event.GetTimestamp())},
                          {"attributes", KeyValueList(event.GetAttributes())}});
  }
  out["events"] = std::move(events);

  json status;
  status["code"] = OtlpStatus(span.GetStatus());
  const std::string description(span.GetDescription());
  if (!description.empty()) {
    status["message"] = description;
  }
  out["status"] = std::move(status);
  return out;
}

// Serializes a batch of spans into a single OTLP ExportTraceServiceRequest.
// Spans are grouped under one resource/scope; the resource attributes are read
// from the first span's instrumentation scope resource is not exposed per-span
// here, so a minimal resource is emitted and enriched by resource attributes
// configured on the provider (which OTel copies onto span resources).
std::string BuildPayload(
    const otel::nostd::span<std::unique_ptr<otel_sdk::Recordable>>& spans) {
  json scope_spans = json::array();
  const otel::sdk::resource::Resource* resource = nullptr;
  const otel_sdk::InstrumentationScope* scope = nullptr;
  json span_array = json::array();
  for (const auto& recordable : spans) {
    if (recordable == nullptr) {
      continue;
    }
    // Every recordable here came from this exporter's own MakeRecordable,
    // which returns a SpanData: the SDK hands back what it was given.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* data = static_cast<otel_sdk::SpanData*>(recordable.get());
    if (resource == nullptr) {
      resource = &data->GetResource();
      scope = &data->GetInstrumentationScope();
    }
    span_array.push_back(SpanToJson(*data));
  }

  json scope_obj = json::object();
  if (scope != nullptr) {
    scope_obj =
        json{{"name", scope->GetName()}, {"version", scope->GetVersion()}};
  }
  scope_spans.push_back(json{{"scope", scope_obj}, {"spans", span_array}});

  json resource_obj = json::object();
  if (resource != nullptr) {
    resource_obj["attributes"] = KeyValueList(resource->GetAttributes());
  }

  json request;
  request["resourceSpans"] = json::array(
      {json{{"resource", resource_obj}, {"scopeSpans", scope_spans}}});
  // Lossy on purpose: a span attribute is a diagnostic, and losing one byte of
  // it to U+FFFD beats losing the whole batch because a caller put a raw byte
  // in an attribute value. See a11/json_codec.h.
  return DumpJsonLossy(request);
}

size_t DiscardBody(char* /*ptr*/, size_t size, size_t nmemb, void* /*ud*/) {
  return size * nmemb;
}

class OtlpHttpJsonExporter final : public otel_sdk::SpanExporter {
 public:
  explicit OtlpHttpJsonExporter(OtlpHttpOptions options)
      : options_(std::move(options)) {
    EnsureCurlGlobalInit();
  }

  std::unique_ptr<otel_sdk::Recordable> MakeRecordable() noexcept override {
    return std::make_unique<otel_sdk::SpanData>();
  }

  otel::sdk::common::ExportResult Export(
      const otel::nostd::span<std::unique_ptr<otel_sdk::Recordable>>&
          spans) noexcept override {
    if (shutdown_.load(std::memory_order_acquire)) {
      return otel::sdk::common::ExportResult::kFailure;
    }
    if (spans.empty()) {
      return otel::sdk::common::ExportResult::kSuccess;
    }

    const std::string payload = BuildPayload(spans);

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      return otel::sdk::common::ExportResult::kFailure;
    }
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    for (const auto& [key, value] : options_.headers) {
      headers = curl_slist_append(headers, (key + ": " + value).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, options_.endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(options_.timeout.count()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &DiscardBody);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
      // LOG(ERROR) << "OTLP export transport error: " << curl_easy_strerror(rc);
      return otel::sdk::common::ExportResult::kFailure;
    }
    if (http_status < 200 || http_status >= 300) {
      LOG(ERROR) << "OTLP export rejected with HTTP status " << http_status;
      return otel::sdk::common::ExportResult::kFailure;
    }
    return otel::sdk::common::ExportResult::kSuccess;
  }

  bool ForceFlush(std::chrono::microseconds /*timeout*/) noexcept override {
    return true;
  }

  bool Shutdown(std::chrono::microseconds /*timeout*/) noexcept override {
    shutdown_.store(true, std::memory_order_release);
    return true;
  }

 private:
  const OtlpHttpOptions options_;
  std::atomic<bool> shutdown_{false};
};

}  // namespace

std::unique_ptr<otel_sdk::SpanExporter> MakeOtlpHttpJsonExporter(
    OtlpHttpOptions options) {
  if (options.endpoint.empty()) {
    return nullptr;
  }
  return std::make_unique<OtlpHttpJsonExporter>(std::move(options));
}

}  // namespace a11::obs::internal

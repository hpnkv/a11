// Copyright 2026 The A11 Authors.

// Protobuf-free OTLP/HTTP (JSON) span exporter. OTLP over HTTP supports a JSON
// encoding, which lets A11 export natively to any OTLP-compatible backend
// (including Langfuse) using nlohmann_json + libcurl -- no protobuf, and no
// second Abseil dragged in by it. Only built when A11_WITH_OTLP_HTTP is set.
//
// This header is internal to a11_obs and pulls in the OTel SDK, so it is never
// installed or included by a public A11 header. Only a factory is exposed here;
// the SpanExporter subclass lives in the .cc.

#ifndef A11_OBS_INTERNAL_OTLP_HTTP_EXPORTER_H_
#define A11_OBS_INTERNAL_OTLP_HTTP_EXPORTER_H_

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opentelemetry/sdk/trace/exporter.h>

namespace a11::obs::internal {

struct OtlpHttpOptions {
  // Full traces endpoint URL, e.g.
  // https://cloud.langfuse.com/api/public/otel/v1/traces
  std::string endpoint;
  // Extra request headers (e.g. Authorization). Content-Type is set to
  // application/json automatically.
  std::vector<std::pair<std::string, std::string>> headers;
  std::chrono::milliseconds timeout{10000};
};

// Builds an OTLP/HTTP JSON span exporter. Returns nullptr if `endpoint` is
// empty.
std::unique_ptr<opentelemetry::sdk::trace::SpanExporter>
MakeOtlpHttpJsonExporter(OtlpHttpOptions options);

}  // namespace a11::obs::internal

#endif  // A11_OBS_INTERNAL_OTLP_HTTP_EXPORTER_H_

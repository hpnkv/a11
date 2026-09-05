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

// Protobuf-free OTLP/HTTP (JSON) span exporter.

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

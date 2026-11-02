// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "a11/obs/tracer.h"

namespace a11::obs {

Span::~Span() = default;
Span::Span(Span&&) noexcept = default;
Span& Span::operator=(Span&&) noexcept = default;

bool Span::IsRecording() const noexcept {
  return false;
}

std::string Span::TraceIdHex() const {
  return {};
}

std::string Span::SpanIdHex() const {
  return {};
}

void Span::SetAttribute(std::string_view, std::string_view) {}

void Span::SetAttribute(std::string_view, const char*) {}

void Span::SetAttribute(std::string_view, std::int64_t) {}

void Span::SetAttribute(std::string_view, bool) {}

void Span::SetAttribute(std::string_view, double) {}

void Span::AddEvent(std::string_view) {}

void Span::AddEvent(std::string_view,
                    const std::vector<std::pair<std::string, std::string>>&) {}

void Span::SetStatus(const absl::Status&) {}

void Span::SetStatus(SpanStatus, std::string_view) {}

void Span::UpdateName(std::string_view) {}

void Span::End() noexcept {}

absl::Status Span::InjectContext(data::ByteMap&) const {
  return absl::OkStatus();
}

Span Tracer::StartSpan(std::string_view, SpanKind, const TraceContext*) {
  return {};
}

Span Tracer::StartChildSpan(std::string_view, SpanKind, const Span&) {
  return {};
}

Span Tracer::StartRootSpan(std::string_view, SpanKind, std::string_view) {
  return {};
}

}  // namespace a11::obs

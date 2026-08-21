// Copyright 2026 The A11 Authors.

#include "a11/net/server_headers.h"

#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>

#include "a11/net/http2.h"

namespace a11::net {
namespace {

/// Whether a header value would break the header block in two.
bool Injects(std::string_view value) {
  return value.find_first_of("\r\n") != std::string_view::npos;
}

/// Whether @p headers already names @p name, case-insensitively.
bool Has(const HttpHeaders& headers, std::string_view name) {
  for (const auto& [existing, unused] : headers) {
    (void)unused;
    if (existing.size() != name.size()) continue;
    bool same = true;
    for (size_t index = 0; index < name.size(); ++index) {
      const char left = existing[index] >= 'A' && existing[index] <= 'Z'
                            ? static_cast<char>(existing[index] + 32)
                            : existing[index];
      const char right = name[index] >= 'A' && name[index] <= 'Z'
                             ? static_cast<char>(name[index] + 32)
                             : name[index];
      if (left != right) {
        same = false;
        break;
      }
    }
    if (same) return true;
  }
  return false;
}

/// Sets @p name only where the route has not already spoken for it.
void SetUnlessPresent(HttpHeaders* absl_nonnull headers, std::string_view name,
                      std::string value) {
  if (Has(*headers, name)) return;
  SetHttpHeader(headers, std::string(name), std::move(value));
}

}  // namespace

absl::Status CorsOptions::Validate() const {
  for (const std::string* value :
       {&allow_origin, &allow_methods, &allow_headers, &expose_headers}) {
    if (Injects(*value)) {
      return absl::InvalidArgumentError(
          "CORS option values must not contain newlines");
    }
  }
  if (max_age_seconds < 0) {
    return absl::InvalidArgumentError(
        "Access-Control-Max-Age cannot be negative");
  }
  if (enabled && allow_origin.empty()) {
    return absl::InvalidArgumentError(
        "Cross-origin responses need an allowed origin; set allow_origin, or "
        "clear enabled to send no CORS headers at all");
  }
  return absl::OkStatus();
}

absl::Status ServerHeaderOptions::Validate() const {
  if (Injects(server)) {
    return absl::InvalidArgumentError(
        "The Server header value must not contain newlines");
  }
  return cors.Validate();
}

void ApplyCorsHeaders(const CorsOptions& options,
                      HttpHeaders* absl_nonnull headers) {
  if (!options.enabled || options.allow_origin.empty()) return;
  SetUnlessPresent(headers, "access-control-allow-origin",
                   options.allow_origin);
  if (options.allow_origin != "*") {
    // One origin named means the answer differs per origin, and a cache that
    // did not know that would serve the wrong one.
    SetUnlessPresent(headers, "vary", "Origin");
  }
  if (!options.allow_methods.empty()) {
    SetUnlessPresent(headers, "access-control-allow-methods",
                     options.allow_methods);
  }
  if (!options.allow_headers.empty()) {
    SetUnlessPresent(headers, "access-control-allow-headers",
                     options.allow_headers);
  }
  if (!options.expose_headers.empty()) {
    SetUnlessPresent(headers, "access-control-expose-headers",
                     options.expose_headers);
  }
  if (options.max_age_seconds > 0) {
    SetUnlessPresent(headers, "access-control-max-age",
                     absl::StrCat(options.max_age_seconds));
  }
}

HttpHeaders CorsHeaders(const CorsOptions& options) {
  HttpHeaders headers;
  ApplyCorsHeaders(options, &headers);
  return headers;
}

void ApplyServerHeaders(const ServerHeaderOptions& options, CachePolicy cache,
                        HttpHeaders* absl_nonnull headers) {
  if (!options.server.empty()) {
    SetUnlessPresent(headers, "server", options.server);
  }
  if (options.nosniff) {
    SetUnlessPresent(headers, "x-content-type-options", "nosniff");
  }
  switch (cache) {
    case CachePolicy::kStream:
      SetUnlessPresent(headers, "cache-control", "no-store");
      // Not a standard header, and worth sending anyway: nginx buffers a
      // proxied response by default, which for an event stream means the client
      // sees nothing until the buffer fills or the stream ends. This is the
      // documented way to ask it not to, and anything that does not know the
      // header ignores it.
      SetUnlessPresent(headers, "x-accel-buffering", "no");
      break;
    case CachePolicy::kVolatile:
      // `no-cache` rather than `no-store`: a client may keep the document, but
      // has to ask before reusing it. What a registry serves changes when the
      // registry does, and there is no version to key on.
      SetUnlessPresent(headers, "cache-control", "no-cache");
      break;
    case CachePolicy::kUnset:
      break;
  }
  ApplyCorsHeaders(options.cors, headers);
}

bool IsPreflight(const CorsOptions& options, std::string_view method) {
  return options.enabled && method == "OPTIONS";
}

}  // namespace a11::net

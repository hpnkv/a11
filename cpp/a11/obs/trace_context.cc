// Copyright 2026 The A11 Authors.

#include "a11/obs/trace_context.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_split.h>
#include <absl/strings/string_view.h>

#include "a11/data/types.h"

namespace a11::obs {
namespace {

constexpr std::string_view kLowerHex = "0123456789abcdef";

bool IsLowerHex(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  for (const char c : value) {
    if (kLowerHex.find(c) == std::string_view::npos) {
      return false;
    }
  }
  return true;
}

bool AllZero(std::string_view hex) {
  return hex.find_first_not_of('0') == std::string_view::npos;
}

// Returns the numeric value 0-15 of a hex digit, or -1 if `c` is not one.
int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Looks up a header, returning its value only when present. Header names are
// stored lowercased by the callers that populate ByteMap, so the reserved
// names above (already lowercase) match directly.
std::optional<std::string_view> Find(const data::ByteMap& headers,
                                     std::string_view name) {
  const auto it = headers.find(std::string(name));
  if (it == headers.end()) {
    return std::nullopt;
  }
  return std::string_view(it->second);
}

// Percent-decodes a W3C baggage value. Invalid escapes are left verbatim,
// matching the lenient posture propagators take on the value portion.
std::string PercentDecode(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const int hi = HexValue(value[i + 1]);
      const int lo = HexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

// Percent-encodes the characters W3C baggage disallows in a value.
std::string PercentEncode(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    const bool unreserved =
        absl::ascii_isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
    if (unreserved) {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kLowerHex[c >> 4]);
      out.push_back(kLowerHex[c & 0x0F]);
    }
  }
  return out;
}

absl::StatusOr<TraceContext> ParseTraceparent(std::string_view value) {
  // version "-" trace-id "-" parent-id "-" trace-flags [ "-" ... ]
  const std::vector<std::string_view> parts = absl::StrSplit(value, '-');
  if (parts.size() < 4) {
    return absl::InvalidArgumentError(
        absl::StrCat("Malformed traceparent header: ", value));
  }
  const std::string_view version = parts[0];
  const std::string_view trace_id = parts[1];
  const std::string_view span_id = parts[2];
  const std::string_view flags = parts[3];

  if (version.size() != 2 || !IsLowerHex(version) || version == "ff") {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid traceparent version: ", value));
  }
  if (trace_id.size() != 32 || !IsLowerHex(trace_id) || AllZero(trace_id)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid traceparent trace-id: ", value));
  }
  if (span_id.size() != 16 || !IsLowerHex(span_id) || AllZero(span_id)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid traceparent parent-id: ", value));
  }
  if (flags.size() != 2 || !IsLowerHex(flags)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid traceparent trace-flags: ", value));
  }
  // Version 00 forbids trailing fields; later versions may add them.
  if (version == "00" && parts.size() != 4) {
    return absl::InvalidArgumentError(
        absl::StrCat("Unexpected trailing traceparent fields: ", value));
  }

  TraceContext context;
  context.trace_id = std::string(trace_id);
  context.span_id = std::string(span_id);
  context.trace_flags =
      static_cast<std::uint8_t>((HexValue(flags[0]) << 4) | HexValue(flags[1]));
  return context;
}

absl::StatusOr<std::vector<BaggageEntry>> ParseBaggage(std::string_view value) {
  std::vector<BaggageEntry> entries;
  for (std::string_view member :
       absl::StrSplit(value, ',', absl::SkipEmpty())) {
    member = absl::StripAsciiWhitespace(member);
    if (member.empty()) {
      continue;
    }
    std::string_view properties;
    const size_t semi = member.find(';');
    if (semi != std::string_view::npos) {
      properties = member.substr(semi + 1);
      member = member.substr(0, semi);
    }
    const size_t eq = member.find('=');
    if (eq == std::string_view::npos) {
      return absl::InvalidArgumentError(
          absl::StrCat("Malformed baggage member: ", member));
    }
    BaggageEntry entry;
    entry.key = std::string(absl::StripAsciiWhitespace(member.substr(0, eq)));
    if (entry.key.empty()) {
      return absl::InvalidArgumentError("Baggage member has an empty key");
    }
    entry.value =
        PercentDecode(absl::StripAsciiWhitespace(member.substr(eq + 1)));
    entry.properties = std::string(absl::StripAsciiWhitespace(properties));
    entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace

absl::StatusOr<std::optional<TraceContext>> ExtractTraceContext(
    const data::ByteMap& headers) {
  const std::optional<std::string_view> traceparent =
      Find(headers, kTraceparentHeader);
  const std::optional<std::string_view> tracestate =
      Find(headers, kTracestateHeader);
  const std::optional<std::string_view> baggage = Find(headers, kBaggageHeader);

  if (!traceparent.has_value()) {
    // tracestate and baggage are only meaningful alongside a traceparent.
    // Their presence without one is an inconsistent context the caller must
    // reject rather than silently drop.
    if (tracestate.has_value() || baggage.has_value()) {
      return absl::InvalidArgumentError(
          "OTel tracestate/baggage present without a traceparent header");
    }
    return std::nullopt;
  }

  ABSL_ASSIGN_OR_RETURN(TraceContext context, ParseTraceparent(*traceparent));
  if (tracestate.has_value()) {
    context.tracestate = std::string(*tracestate);
  }
  if (baggage.has_value()) {
    ABSL_ASSIGN_OR_RETURN(context.baggage, ParseBaggage(*baggage));
  }
  return std::optional<TraceContext>(std::move(context));
}

absl::Status InjectTraceContext(const TraceContext& context,
                                data::ByteMap& headers) {
  if (context.trace_id.size() != 32 || !IsLowerHex(context.trace_id)) {
    return absl::InvalidArgumentError("TraceContext has an invalid trace-id");
  }
  if (context.span_id.size() != 16 || !IsLowerHex(context.span_id)) {
    return absl::InvalidArgumentError("TraceContext has an invalid span-id");
  }

  char flags[3];
  flags[0] = kLowerHex[(context.trace_flags >> 4) & 0x0F];
  flags[1] = kLowerHex[context.trace_flags & 0x0F];
  flags[2] = '\0';
  headers[std::string(kTraceparentHeader)] =
      absl::StrCat("00-", context.trace_id, "-", context.span_id, "-", flags);

  if (!context.tracestate.empty()) {
    headers[std::string(kTracestateHeader)] = context.tracestate;
  } else {
    headers.erase(std::string(kTracestateHeader));
  }

  if (!context.baggage.empty()) {
    std::vector<std::string> members;
    members.reserve(context.baggage.size());
    for (const BaggageEntry& entry : context.baggage) {
      std::string member =
          absl::StrCat(entry.key, "=", PercentEncode(entry.value));
      if (!entry.properties.empty()) {
        absl::StrAppend(&member, ";", entry.properties);
      }
      members.push_back(std::move(member));
    }
    headers[std::string(kBaggageHeader)] = absl::StrJoin(members, ",");
  } else {
    headers.erase(std::string(kBaggageHeader));
  }

  return absl::OkStatus();
}

}  // namespace a11::obs

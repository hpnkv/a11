// Copyright 2026 The A11 Authors.

#include "a11/net/http/url.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/strip.h>

namespace a11::net {
namespace {

/** @return Whether @p host needs bracketing in an authority (an IPv6 literal). */
bool IsIpV6Literal(std::string_view host) {
  return host.find(':') != std::string_view::npos;
}

/**
 * Splits an authority into host and port. The port stays unset when absent, so
 * the caller can apply the scheme default without losing "was it explicit".
 */
absl::Status ParseAuthority(std::string_view authority, std::string* host,
                            std::uint16_t* port, bool* port_was_explicit) {
  if (authority.empty()) {
    return absl::InvalidArgumentError("URL has no host");
  }
  *port_was_explicit = false;
  std::string_view port_text;
  if (authority.front() == '[') {
    // An IPv6 literal: the colons inside the brackets are part of the address,
    // so only a colon after the closing bracket introduces a port.
    const size_t closing = authority.find(']');
    if (closing == std::string_view::npos) {
      return absl::InvalidArgumentError("URL has an unterminated IPv6 host");
    }
    *host = std::string(authority.substr(1, closing - 1));
    std::string_view rest = authority.substr(closing + 1);
    if (!rest.empty()) {
      if (rest.front() != ':') {
        return absl::InvalidArgumentError(
            "URL has trailing characters after its IPv6 host");
      }
      port_text = rest.substr(1);
    }
  } else {
    const size_t colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
      *host = std::string(authority);
    } else {
      *host = std::string(authority.substr(0, colon));
      port_text = authority.substr(colon + 1);
    }
  }
  if (host->empty()) {
    return absl::InvalidArgumentError("URL has no host");
  }
  if (!port_text.empty()) {
    std::uint32_t parsed = 0;
    if (!absl::SimpleAtoi(port_text, &parsed) || parsed == 0 ||
        parsed > 65535) {
      return absl::InvalidArgumentError(
          absl::StrCat("URL has an invalid port: ", port_text));
    }
    *port = static_cast<std::uint16_t>(parsed);
    *port_was_explicit = true;
  }
  return absl::OkStatus();
}

/** Splits off the fragment and query from a path, in that order. */
void SplitPathParts(std::string_view rest, std::string* path,
                    std::string* query) {
  // The fragment goes first: a '?' after a '#' is part of the fragment, not a
  // query.
  const size_t hash = rest.find('#');
  if (hash != std::string_view::npos) {
    rest = rest.substr(0, hash);
  }
  const size_t question = rest.find('?');
  if (question == std::string_view::npos) {
    *path = std::string(rest);
    query->clear();
  } else {
    *path = std::string(rest.substr(0, question));
    *query = std::string(rest.substr(question + 1));
  }
}

/** @return @p path with its last segment removed, keeping the trailing slash. */
std::string DirectoryOf(std::string_view path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) {
    return "/";
  }
  return std::string(path.substr(0, slash + 1));
}

}  // namespace

bool ParsedUrl::secure() const {
  return scheme == "https" || scheme == "wss";
}

std::string ParsedUrl::authority() const {
  const std::string bracketed =
      IsIpV6Literal(host) ? absl::StrCat("[", host, "]") : host;
  if (port == 0 || port == DefaultPortForScheme(scheme)) {
    return bracketed;
  }
  return absl::StrCat(bracketed, ":", port);
}

std::string ParsedUrl::target() const {
  const std::string_view base = path.empty() ? "/" : std::string_view(path);
  if (query.empty()) {
    return std::string(base);
  }
  return absl::StrCat(base, "?", query);
}

std::string ParsedUrl::origin() const {
  return absl::StrCat(scheme, "://", authority());
}

std::string ParsedUrl::ToString() const {
  return absl::StrCat(origin(), target());
}

std::uint16_t DefaultPortForScheme(std::string_view scheme) {
  if (scheme == "http" || scheme == "ws") {
    return 80;
  }
  if (scheme == "https" || scheme == "wss") {
    return 443;
  }
  return 0;
}

absl::StatusOr<ParsedUrl> ParseUrl(std::string_view url) {
  const size_t separator = url.find("://");
  if (separator == std::string_view::npos) {
    return absl::InvalidArgumentError(absl::StrCat(
        "URL must be absolute, with a scheme and \"://\": ", url));
  }
  ParsedUrl parsed;
  parsed.scheme = absl::AsciiStrToLower(url.substr(0, separator));
  const std::uint16_t default_port = DefaultPortForScheme(parsed.scheme);
  if (default_port == 0) {
    return absl::InvalidArgumentError(absl::StrCat(
        "URL scheme must be http, https, ws, or wss, got: ", parsed.scheme));
  }
  parsed.port = default_port;

  std::string_view rest = url.substr(separator + 3);
  const size_t slash = rest.find_first_of("/?#");
  const std::string_view authority = rest.substr(0, slash);
  bool port_was_explicit = false;
  ABSL_RETURN_IF_ERROR(ParseAuthority(authority, &parsed.host, &parsed.port,
                                      &port_was_explicit));
  if (slash != std::string_view::npos) {
    std::string_view tail = rest.substr(slash);
    // A URL like "http://host?q" has a query but no path; give it the root so
    // the request target is always well-formed.
    if (tail.front() != '/') {
      std::string path;
      SplitPathParts(tail, &path, &parsed.query);
      parsed.path = "/";
    } else {
      SplitPathParts(tail, &parsed.path, &parsed.query);
    }
  }
  return parsed;
}

absl::StatusOr<ParsedUrl> ResolveReference(const ParsedUrl& base,
                                           std::string_view reference) {
  if (reference.empty()) {
    return absl::InvalidArgumentError("URL reference is empty");
  }
  if (reference.find("://") != std::string_view::npos) {
    return ParseUrl(reference);
  }
  if (absl::StartsWith(reference, "//")) {
    // Protocol-relative: keep the base scheme, take everything else from the
    // reference.
    return ParseUrl(absl::StrCat(base.scheme, ":", reference));
  }
  ParsedUrl resolved = base;
  if (absl::StartsWith(reference, "?")) {
    resolved.query = std::string(reference.substr(1));
    const size_t hash = resolved.query.find('#');
    if (hash != std::string::npos) {
      resolved.query.erase(hash);
    }
    return resolved;
  }
  std::string path;
  std::string query;
  SplitPathParts(reference, &path, &query);
  if (path.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("URL reference has no target: ", reference));
  }
  resolved.path = absl::StartsWith(path, "/")
                      ? std::move(path)
                      : absl::StrCat(DirectoryOf(base.path), path);
  resolved.query = std::move(query);
  return resolved;
}

}  // namespace a11::net

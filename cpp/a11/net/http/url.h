// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Parsing and resolution for the URLs the HTTP and WebSocket transports
 *        dial.
 *
 * Three transports each grew a private copy of this (the SSE client, the
 * WebSocket client, and the WebSocket signalling client), and a fourth caller
 * -- following a `Location` header while downloading -- needs reference
 * resolution none of them had. One parser serves them all.
 *
 * The scope is deliberately narrow: absolute `http`/`https`/`ws`/`wss` URLs
 * with an authority, plus the relative forms a redirect may answer with. This
 * is not a general RFC 3986 implementation -- there is no userinfo, no percent
 * normalisation, and no dot-segment removal beyond what ResolveReference needs.
 */

#ifndef A11_NET_HTTP_URL_H_
#define A11_NET_HTTP_URL_H_

#include <cstdint>
#include <string>
#include <string_view>

#include <absl/status/statusor.h>

namespace a11::net {

/**
 * @brief An absolute URL split into the parts a connection needs.
 *
 * The fragment is dropped during parsing: it is a client-side concern and never
 * travels to a server.
 */
struct ParsedUrl {
  std::string scheme;    ///< Lowercase, without "://" (e.g. "https").
  std::string host;      ///< Hostname or IP literal, without IPv6 brackets.
  std::uint16_t port = 0;  ///< Explicit port, or the scheme's default.
  std::string path;      ///< Begins with '/', or is empty when none was given.
  std::string query;     ///< Query without the leading '?'; empty when absent.

  /** @return Whether the scheme implies TLS ("https" or "wss"). */
  [[nodiscard]] bool secure() const;
  /**
   * @return The authority as a header value: `host`, `host:port`, or
   *         `[v6host]:port`. The port is omitted when it is the scheme default.
   */
  [[nodiscard]] std::string authority() const;
  /** @return The request target: path and query, at least "/". */
  [[nodiscard]] std::string target() const;
  /** @return `scheme://authority`, with no trailing slash. */
  [[nodiscard]] std::string origin() const;
  /** @return The full URL, as origin() plus target(). */
  [[nodiscard]] std::string ToString() const;
};

/** @return The default port for @p scheme, or 0 when the scheme is unknown. */
std::uint16_t DefaultPortForScheme(std::string_view scheme);

/**
 * @brief Parses an absolute `http`/`https`/`ws`/`wss` URL.
 *
 * The scheme is matched case-insensitively and stored lowercase. An absent port
 * takes the scheme's default. A trailing fragment is discarded.
 *
 * @param url The URL to parse.
 * @return The parsed URL, or InvalidArgument when the scheme is not one of the
 *         four, the authority is missing, or the port is not a number.
 */
absl::StatusOr<ParsedUrl> ParseUrl(std::string_view url);

/**
 * @brief Resolves @p reference against @p base, as a redirect target.
 *
 * Handles the four forms a `Location` header uses in practice: an absolute URL,
 * a protocol-relative `//host/path`, an absolute path `/path`, and a relative
 * path resolved against @p base's directory. Enough of RFC 3986 section 5 to
 * follow redirects correctly, and no more.
 *
 * @param base The URL the reference was received from.
 * @param reference The reference to resolve.
 * @return The resolved absolute URL, or InvalidArgument when @p reference is
 *         empty or names an unsupported scheme.
 */
absl::StatusOr<ParsedUrl> ResolveReference(const ParsedUrl& base,
                                           std::string_view reference);

}  // namespace a11::net

#endif  // A11_NET_HTTP_URL_H_

// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief `GET /actions`, on whichever server happens to hold the port.
 *
 * A11 is a protocol before it is an HTTP API, and discovery is an action
 * (`__list_actions__`) that works over every transport. These endpoints exist
 * anyway, because a service is a thing people `curl`, and because the answer to
 * "what is listening on 8011" should not require an A11 client to ask.
 *
 * **Why a function and not a registry.** `a11::net` sits below
 * `a11::actions` -- the dependency runs net -> actions, never back -- so an
 * options struct here cannot hold an ActionRegistry. What it holds instead is a
 * callback the layer above fills in, which has the useful side effect that the
 * endpoint and the action are answered by the same describer and so cannot
 * disagree.
 */

#ifndef A11_NET_DESCRIBE_ENDPOINT_H_
#define A11_NET_DESCRIBE_ENDPOINT_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <absl/base/nullability.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"

namespace a11::net {

/**
 * @brief Answers a discovery request without opening a wire stream.
 *
 * @param name Action to describe, or empty for the whole collection.
 * @param query The URL query string, without the `?`. Parsed by the
 *        implementation, which is where the request's shape is known.
 * @return The JSON body, or a status whose code becomes the HTTP status.
 */
using DescribeActionsHandler = std::function<absl::StatusOr<std::string>(
    std::string_view name, std::string_view query)>;

/** @brief Default path serving the action descriptors. */
inline constexpr std::string_view kDefaultDescribeEndpoint = "/actions";

/**
 * @brief Where and whether a server answers discovery over plain HTTP.
 *
 * An empty @c handler leaves the endpoint unserved, and the server's existing
 * 404 stands. That is the default: a transport does not describe anything until
 * something above it says what to describe.
 */
struct DescribeEndpointOptions {
  DescribeActionsHandler handler{};
  std::string path = std::string(kDefaultDescribeEndpoint);

  /** @brief Whether this server should answer discovery at all. */
  [[nodiscard]] bool Enabled() const {
    return static_cast<bool>(handler) && !path.empty();
  }
};

/**
 * @brief Whether @p path addresses the describe endpoint, and what it names.
 *
 * Matches both `/actions` and `/actions/<name>`, so one predicate answers the
 * collection and the item route.
 *
 * @param path Request path, already stripped of its query.
 * @param options The endpoint's configuration.
 * @param name Set to the named action, or empty for the collection.
 * @return Whether @p path is this endpoint at all.
 */
bool MatchDescribePath(std::string_view path,
                       const DescribeEndpointOptions& options,
                       std::string* absl_nonnull name);

/**
 * @brief Answers @p request from @p options, if it is addressed to it.
 *
 * The whole endpoint, for whichever server holds the port. `GET` and `HEAD`
 * are answered with the JSON document; a failing describer's status code
 * becomes the HTTP one, so an unknown action is a 404 with the describer's own
 * message.
 *
 * @param options The endpoint's configuration; a disabled one matches nothing.
 * @param request The inbound request, query string included.
 * @param response Where to write the answer.
 * @param extra_headers Headers to include, typically CORS.
 * @return The task answering it, or nullopt when this is not that endpoint --
 *         which leaves the caller's own routing, and its own 404, in charge.
 */
std::optional<a11::Task> TryDescribeOverHttp(
    const DescribeEndpointOptions& options, const HttpRequest& request,
    const std::shared_ptr<Http2ResponseWriter>& response,
    HttpHeaders extra_headers = {});

/** @brief The query part of @p path, without the `?`. */
std::string_view QueryOfPath(std::string_view path);

}  // namespace a11::net

#endif  // A11_NET_DESCRIBE_ENDPOINT_H_

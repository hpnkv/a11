// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief HTTP as A11 Actions: one protocol-faithful, one shaped like `fetch()`.
 *
 * Two Actions over the same engine:
 *   - @c make_http_request -- HTTP with nothing hidden. Every concern the
 *     protocol keeps separate gets a port of its own: the status, the header
 *     fields, the body, the trailer section, the redirect chain, the responses
 *     the server pushed, and how the connection was carried. A caller reads the
 *     ones it cares about, in whatever order they become available, and can act
 *     on the status while the body is still arriving.
 *   - @c web-fetch -- the same machinery with the protocol turned down: a status,
 *     a header map, and the body as text, as JSON, as bytes, or decoded into a
 *     stream of items. What a caller who just wants a document asks for.
 *
 * Why an Action rather than a function. An ordinary HTTP client hands back one
 * `Response` object because its language gives it nothing better to hand back;
 * headers, body and trailers are one value that is only complete at the end. An
 * A11 port is a stream, and there is no reason for these to share one -- so they
 * do not. That is the whole idea, and everything else here follows from it.
 *
 * ### Headers
 *
 * An action header that does not begin with @c x-a11- is sent as an HTTP request
 * header, verbatim. So Flow's `with "accept": "application/json"` and
 * `forward headers "authorization"` are already HTTP header syntax, and A11's own
 * @c x-a11- headers (a deadline, a trace) stay out of the request. Anything that
 * cannot be spelled as an A11 header name goes in @c options.headers instead.
 *
 * Registered on any ActionRegistry with @ref RegisterHttpActions, in C++ or
 * through the Python binding.
 */

#ifndef A11_SDK_HTTP_ACTIONS_HTTP_ACTIONS_H_
#define A11_SDK_HTTP_ACTIONS_HTTP_ACTIONS_H_

#include <string_view>

#include <absl/status/status.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"

namespace a11::sdk::http {

/** @brief Registered name of the low-level request Action. */
inline constexpr std::string_view kMakeHttpRequestAction = "make_http_request";
/** @brief Registered name of the `fetch()`-shaped adapter. */
inline constexpr std::string_view kWebFetchAction = "web-fetch";

/** @brief Schema for @c make_http_request. */
a11::actions::ActionSchema MakeHttpRequestSchema();
/** @brief Schema for @c web-fetch. */
a11::actions::ActionSchema WebFetchSchema();

/** @brief Handler for @c make_http_request. */
a11::actions::ActionHandler MakeHttpRequestHandler();
/** @brief Handler for @c web-fetch. */
a11::actions::ActionHandler WebFetchHandler();

/**
 * @brief Registers both HTTP Actions on @p registry.
 * @return OK, or the first registration error.
 */
absl::Status RegisterHttpActions(a11::actions::ActionRegistry& registry);

}  // namespace a11::sdk::http

#endif  // A11_SDK_HTTP_ACTIONS_HTTP_ACTIONS_H_

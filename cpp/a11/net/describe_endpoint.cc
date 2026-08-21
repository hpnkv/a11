// Copyright 2026 The A11 Authors.

#include "a11/net/describe_endpoint.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"
#include "a11/status.h"

namespace a11::net {
namespace {

a11::Task Answered(absl::Status status) {
  return status.ok() ? a11::ReadyTask() : a11::FailedTask(std::move(status));
}

/// The path with its query and fragment removed.
std::string_view PathOnly(std::string_view path) {
  const size_t cut = path.find_first_of("?#");
  return cut == std::string_view::npos ? path : path.substr(0, cut);
}

}  // namespace

std::string_view QueryOfPath(std::string_view path) {
  const size_t query = path.find('?');
  if (query == std::string_view::npos) {
    return {};
  }
  std::string_view rest = path.substr(query + 1);
  const size_t fragment = rest.find('#');
  return fragment == std::string_view::npos ? rest : rest.substr(0, fragment);
}

bool MatchDescribePath(std::string_view path,
                       const DescribeEndpointOptions& options,
                       std::string* absl_nonnull name) {
  name->clear();
  if (!options.Enabled()) {
    return false;
  }
  if (path == options.path) {
    return true;
  }
  // `/actions/` with nothing after it is the collection, spelled with a
  // trailing slash. Treating it as an action named "" would 404 on a URL that
  // plainly means the list.
  const std::string prefix = options.path + "/";
  if (!absl::StartsWith(path, prefix)) {
    return false;
  }
  *name = std::string(path.substr(prefix.size()));
  return true;
}

std::optional<a11::Task> TryDescribeOverHttp(
    const DescribeEndpointOptions& options, const HttpRequest& request,
    const std::shared_ptr<Http2ResponseWriter>& response,
    HttpHeaders extra_headers) {
  std::string named;
  if (!MatchDescribePath(PathOnly(request.path), options, &named)) {
    return std::nullopt;
  }
  if (request.method != "GET" && request.method != "HEAD") {
    // The path is ours, so answering 405 here is more use than letting it fall
    // through to the caller's "no such endpoint".
    SetHttpHeader(&extra_headers, "allow", "GET, HEAD");
    SetHttpHeader(&extra_headers, "content-type", "text/plain; charset=utf-8");
    return Answered(response->SendResponse(405, std::move(extra_headers),
                                           "Use GET to read action schemas"));
  }
  absl::StatusOr<std::string> body =
      options.handler(named, QueryOfPath(request.path));
  if (!body.ok()) {
    // The describer's own code and message: an unknown action is its NotFound,
    // which is a 404 saying which name was not found.
    SetHttpHeader(&extra_headers, "content-type", "text/plain; charset=utf-8");
    return Answered(response->SendResponse(
        StatusCodeToHttp(body.status().code()), std::move(extra_headers),
        std::string(body.status().message())));
  }
  SetHttpHeader(&extra_headers, "content-type",
                "application/json; charset=utf-8");
  return Answered(
      response->SendResponse(200, std::move(extra_headers), std::move(*body)));
}

}  // namespace a11::net

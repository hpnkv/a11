// Copyright 2026 The A11 Authors.

#include "a11/net/http/fetch.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/net/http/url.h"
#include "a11/net/http2.h"
#include "a11/status.h"

namespace a11::net {
namespace {

constexpr std::string_view kDefaultUserAgent = "a11-net/1";

/**
 * What a fetch is willing to hold in flight per response, unless told
 * otherwise.
 *
 * Larger than the transport's general default because fetching is bulk by
 * nature: it sets the HTTP/2 receive window too, and a window smaller than the
 * path's bandwidth-delay product caps a download well below the link. 16 MiB
 * covers a gigabit path at 100 ms.
 */
constexpr size_t kBulkTransferBufferSize = 16 * 1024 * 1024;

bool IsRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308;
}

/**
 * 301, 302 and 303 are rewritten to a bodyless GET, which is what every client
 * does and what RFC 9110 sanctions for the first two; 307 and 308 exist
 * precisely to preserve the method, so they keep it.
 */
bool RedirectRewritesToGet(int status) {
  return status == 301 || status == 302 || status == 303;
}

absl::Status ResponseStatus(const ParsedUrl& url,
                            const HttpResponseHead& head) {
  return MakeStatus(
      StatusCodeFromHttp(head.status),
      absl::StrCat("HTTP ", head.status, " for ", url.ToString()),
      nlohmann::json{{"url", url.ToString()}, {"http_status", head.status}});
}

std::uint64_t ContentLengthOf(const HttpResponseHead& head) {
  const std::optional<std::string> value =
      GetHttpHeader(head.headers, "content-length");
  std::uint64_t length = 0;
  if (value.has_value() && absl::SimpleAtoi(*value, &length)) {
    return length;
  }
  return 0;
}

/**
 * The whole operation, on a fiber. Both public entry points funnel through
 * here: Fetch() supplies a sink that appends to its response body, so there is
 * one implementation of connect/redirect/read rather than two.
 */
absl::StatusOr<HttpResponseHead> RunFetch(std::string url_text,
                                          FetchOptions options,
                                          const FetchSink& sink,
                                          const OnFetchProgress& on_progress,
                                          absl::Time deadline) {
  ABSL_ASSIGN_OR_RETURN(ParsedUrl target, ParseUrl(url_text));
  if (target.scheme != "http" && target.scheme != "https") {
    return absl::InvalidArgumentError(
        absl::StrCat("fetch needs an http or https URL, got: ", target.scheme));
  }

  // A bulk transfer wants a receive window big enough for the path's
  // bandwidth-delay product, and the window is tied to what we will buffer (see
  // StreamWindowSize). Preserve an explicit caller-provided value.
  if (options.transport.max_buffered_response_bytes ==
      Http2Options{}.max_buffered_response_bytes) {
    options.transport.max_buffered_response_bytes = kBulkTransferBufferSize;
  }

  std::string method = options.method;
  std::string body = options.body;
  std::shared_ptr<Http2Client> client;
  // Every hop gets its own connection and the previous one is closed.
  const auto close_client = [&client] {
    if (client != nullptr) {
      (void)client->Close();
      client.reset();
    }
  };

  for (int followed = 0;; ++followed) {
    Http2Options transport = options.transport;
    transport.tls.enabled = target.secure();
    transport.deadline = deadline;

    close_client();
    ABSL_ASSIGN_OR_RETURN(
        client, Http2Client::Connect(target.host, target.port, transport)
                    .Await(deadline));

    HttpHeaders headers = options.headers;
    NormalizeHttpHeaders(&headers);
    if (options.default_user_agent &&
        !GetHttpHeader(headers, "user-agent").has_value()) {
      SetHttpHeader(&headers, "user-agent", std::string(kDefaultUserAgent));
    }

    ABSL_ASSIGN_OR_RETURN(std::shared_ptr<Http2ResponseStream> stream,
                          client->RequestStream(method, target.target(),
                                                headers, body, target.scheme));
    ABSL_ASSIGN_OR_RETURN(HttpResponseHead head,
                          stream->Headers().Await(deadline));

    if (IsRedirect(head.status) && options.max_redirects > 0) {
      const std::optional<std::string> location =
          GetHttpHeader(head.headers, "location");
      if (location.has_value()) {
        if (followed >= options.max_redirects) {
          (void)stream->Cancel(
              absl::ResourceExhaustedError("too many redirects"));
          close_client();
          return MakeStatus(
              absl::StatusCode::kResourceExhausted,
              absl::StrCat("fetch followed ", options.max_redirects,
                           " redirects without reaching a final response"),
              nlohmann::json{{"url", url_text},
                             {"last_url", target.ToString()}});
        }
        // The body of a redirect is of no interest, and reading it would only
        // delay the hop.
        (void)stream->Cancel(absl::CancelledError("following redirect"));
        ABSL_ASSIGN_OR_RETURN(target, ResolveReference(target, *location));
        if (target.scheme != "http" && target.scheme != "https") {
          close_client();
          return absl::InvalidArgumentError(absl::StrCat(
              "redirect left http(s) for scheme: ", target.scheme));
        }
        if (RedirectRewritesToGet(head.status)) {
          method = "GET";
          body.clear();
        }
        continue;
      }
      // A 3xx with no Location is not a redirect we can follow; treat it as the
      // response it is.
    }

    if (head.status >= 400) {
      (void)stream->Cancel(absl::CancelledError("error response"));
      close_client();
      return ResponseStatus(target, head);
    }

    const std::uint64_t total = ContentLengthOf(head);
    std::uint64_t done = 0;
    if (on_progress) {
      on_progress(0, total);
    }
    while (true) {
      ABSL_ASSIGN_OR_RETURN(std::optional<std::string> chunk,
                            stream->Read().Await(deadline));
      if (!chunk.has_value()) {
        break;
      }
      if (absl::Status written = sink(*chunk); !written.ok()) {
        (void)stream->Cancel(written);
        close_client();
        return written;
      }
      done += chunk->size();
      if (on_progress) {
        on_progress(done, total);
      }
    }
    ABSL_RETURN_IF_ERROR(stream->Done().Await(deadline).status());
    close_client();
    return head;
  }
}

absl::Time DeadlineFor(const FetchOptions& options) {
  const absl::Time from_timeout = options.timeout == absl::InfiniteDuration()
                                      ? absl::InfiniteFuture()
                                      : absl::Now() + options.timeout;
  return std::min(from_timeout, options.transport.deadline);
}

}  // namespace

absl::Status FetchOptions::Validate() const {
  if (method.empty()) {
    return absl::InvalidArgumentError("FetchOptions.method must not be empty");
  }
  if (max_redirects < 0) {
    return absl::InvalidArgumentError(
        "FetchOptions.max_redirects must not be negative");
  }
  if (timeout <= absl::ZeroDuration()) {
    return absl::InvalidArgumentError("FetchOptions.timeout must be positive");
  }
  return transport.Validate();
}

namespace internal {

absl::StatusOr<HttpResponseHead> FetchBlocking(
    std::string url, FetchOptions options, const FetchSink& sink,
    const OnFetchProgress& on_progress) {
  if (sink == nullptr) {
    return absl::InvalidArgumentError("fetch needs a sink");
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  const absl::Time deadline = DeadlineFor(options);
  return RunFetch(std::move(url), std::move(options), sink, on_progress,
                  deadline);
}

}  // namespace internal

a11::Future<HttpResponse> Fetch(std::string url, FetchOptions options) {
  if (const absl::Status valid = options.Validate(); !valid.ok()) {
    return a11::FailedFuture<HttpResponse>(valid);
  }
  const size_t limit = options.transport.max_response_body_size;
  return a11::Submit<HttpResponse>(
      [url = std::move(url), options = std::move(options),
       limit]() mutable -> absl::StatusOr<HttpResponse> {
        HttpResponse response;
        FetchSink sink = [&response,
                          limit](std::string_view chunk) -> absl::Status {
          if (response.body.size() + chunk.size() > limit) {
            return absl::OutOfRangeError(
                "HTTP response exceeds max_response_body_size");
          }
          response.body.append(chunk);
          return absl::OkStatus();
        };
        ABSL_ASSIGN_OR_RETURN(
            response.head,
            internal::FetchBlocking(std::move(url), std::move(options), sink));
        return response;
      });
}

a11::Future<HttpResponseHead> FetchToSink(std::string url, FetchSink sink,
                                          FetchOptions options,
                                          OnFetchProgress on_progress) {
  return a11::Submit<HttpResponseHead>(
      [url = std::move(url), options = std::move(options),
       sink = std::move(sink), on_progress = std::move(on_progress)]() mutable
          -> absl::StatusOr<HttpResponseHead> {
        return internal::FetchBlocking(std::move(url), std::move(options), sink,
                                       on_progress);
      });
}

}  // namespace a11::net

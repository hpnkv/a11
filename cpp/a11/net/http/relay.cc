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

#include "a11/net/http/relay.h"

#include <string_view>
#include <utility>

#include <absl/container/flat_hash_set.h>
#include <absl/log/log.h>
#include <absl/status/status_macros.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_split.h>

#include "a11/concurrency/executor.h"
#include "a11/net/http/connection_pool.h"
#include "a11/net/http/url.h"
#include "a11/net/internal/exception_guarded_callbacks.h"

namespace a11::net {
namespace {
HttpHeaders EndToEnd(const HttpHeaders& headers, bool request) {
  absl::flat_hash_set<std::string> hop = {"connection",
                                          "keep-alive",
                                          "proxy-connection",
                                          "transfer-encoding",
                                          "upgrade",
                                          "te",
                                          "trailer",
                                          "proxy-authenticate",
                                          "proxy-authorization"};
  for (const auto& [name, value] : headers) {
    if (absl::EqualsIgnoreCase(name, "connection")) {
      for (std::string_view token : absl::StrSplit(value, ',')) {
        hop.insert(absl::AsciiStrToLower(absl::StripAsciiWhitespace(token)));
      }
    }
  }
  HttpHeaders result;
  for (const auto& [name, value] : headers) {
    const auto lower = absl::AsciiStrToLower(name);
    if (hop.find(lower) != hop.end() || absl::StartsWith(lower, "x-a11-") ||
        (request && (lower == "host" || lower == "content-length"))) {
      continue;
    }
    result.emplace_back(name, value);
  }
  return result;
}

template <typename Stream>
absl::Status Pump(const std::shared_ptr<Stream>& upstream,
                  const std::shared_ptr<Http2ResponseWriter>& writer,
                  absl::Time deadline, HttpRelayObserver observer,
                  size_t max_observed_bytes) {
  writer->Done().OnReady(
      [weak = std::weak_ptr<Stream>(upstream)](const auto& done) {
        if (!done.ok()) {
          if (auto stream = weak.lock(); stream != nullptr) {
            (void)stream->Cancel(done.status());
          }
        }
      });
  HttpResponseHead observed_head;
  size_t observed = 0;
  auto observe = [&](std::string_view event, std::string_view data) {
    if (observer == nullptr) {
      return;
    }
    if (const auto status = observer(event, observed_head, data);
        !status.ok()) {
      LOG(WARNING) << "HTTP relay observer disabled: " << status;
      observer = {};
    }
  };
  const auto run = [&]() -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(auto head, upstream->Headers().Await(deadline));
    observed_head = head;
    observe("headers", {});
    ABSL_RETURN_IF_ERROR(
        writer->SendHeaders(head.status, EndToEnd(head.headers, false)));
    for (;;) {
      ABSL_ASSIGN_OR_RETURN(auto chunk, upstream->Read().Await(deadline));
      if (!chunk.has_value()) {
        break;
      }
      if (observer != nullptr) {
        if (chunk->size() > max_observed_bytes - observed) {
          observe("limit", {});
          observer = {};
        } else {
          observed += chunk->size();
          observe("body", *chunk);
        }
      }
      ABSL_RETURN_IF_ERROR(writer->Write(std::move(*chunk)));
    }
    ABSL_ASSIGN_OR_RETURN(auto trailers, upstream->Trailers().Await(deadline));
    ABSL_RETURN_IF_ERROR(writer->FinishWithTrailers(EndToEnd(trailers, false)));
    const auto delivered = writer->Done().Await(deadline).status();
    if (delivered.ok()) {
      observe("complete", {});
    }
    return delivered;
  };
  const auto status = run();
  if (!status.ok()) {
    observe("error", {});
    (void)upstream->Cancel(status);
    (void)writer->Abort(status);
  }
  return status;
}
}  // namespace

a11::Task RelayHttp(std::string target, HttpRequest request,
                    std::shared_ptr<Http2ResponseWriter> writer,
                    Http2Options options, absl::Time deadline,
                    HttpRelayObserver observer, size_t max_observed_bytes) {
  observer = internal::GuardRelayObserver(std::move(observer));
  return a11::SubmitTask([target = std::move(target),
                          request = std::move(request),
                          writer = std::move(writer), options,
                          observer = std::move(observer), max_observed_bytes,
                          deadline]() mutable -> absl::Status {
    if (writer == nullptr) {
      return absl::InvalidArgumentError("Relay needs a response writer");
    }
    ABSL_ASSIGN_OR_RETURN(auto url, ParseUrl(target));
    if (url.scheme != "http" && url.scheme != "https") {
      return absl::InvalidArgumentError("Relay needs an HTTP or HTTPS URL");
    }
    options.tls.enabled = url.secure();
    ABSL_ASSIGN_OR_RETURN(
        auto lease,
        HttpConnectionPool::Shared()->Acquire(url, options).Await(deadline));
    if (writer->Done().IsReady()) {
      return absl::CancelledError("Downstream closed before upstream request");
    }
    auto headers = EndToEnd(request.headers, true);
    if (request.body_stream == nullptr) {
      ABSL_ASSIGN_OR_RETURN(
          auto stream, lease.client()->RequestStream(
                           request.method, url.target(), std::move(headers),
                           std::move(request.body), url.scheme));
      return Pump(stream, writer, deadline, std::move(observer),
                  max_observed_bytes);
    }
    ABSL_ASSIGN_OR_RETURN(auto stream, lease.client()->RequestStreamingBody(
                                           request.method, url.target(),
                                           std::move(headers), url.scheme));
    auto upload = a11::SubmitTask(
        [body = request.body_stream, stream, deadline]() -> absl::Status {
          for (;;) {
            auto next = body->Read().Await(deadline);
            if (!next.ok()) {
              (void)stream->Abort(next.status());
              return next.status();
            }
            if (!next->has_value()) {
              break;
            }
            auto status = stream->Write(std::move(**next));
            if (!status.ok()) {
              (void)body->Cancel(status);
              return status;
            }
          }
          return stream->Finish();
        });
    const auto status = Pump(stream->response(), writer, deadline,
                             std::move(observer), max_observed_bytes);
    (void)request.body_stream->Cancel(
        absl::CancelledError("Relay response ended"));
    (void)stream->Abort(absl::CancelledError("Relay response ended"));
    const auto sent = upload.Await(deadline);
    return status.ok() ? sent.status() : status;
  });
}
}  // namespace a11::net

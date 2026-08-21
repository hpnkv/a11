// Copyright 2026 The A11 Authors.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/future.h"
#include "a11/net/http/fetch.h"
#include "a11/net/http2.h"

namespace a11::net {
namespace {

absl::Time Soon() {
  return absl::Now() + absl::Seconds(10);
}

/**
 * A server covering the shapes the fetch helpers have to get right: a plain
 * body, a redirect chain, a redirect loop, a 3xx without a Location, and the
 * error codes. It records the paths it served so a test can assert that a cache
 * hit did *not* reach the network.
 */
class FetchTestServer {
 public:
  FetchTestServer() {
    auto server = Http2Server::Create(
        "127.0.0.1", 0,
        [this](
            const HttpRequest& request,
            const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
          requests_.push_back(absl::StrCat(request.method, " ", request.path));
          const std::string& path = request.path;
          absl::Status status;
          if (path == "/body") {
            status = response->SendResponse(
                200, {{"content-type", "text/plain"}}, "the-body");
          } else if (path == "/hop-1") {
            status = response->SendResponse(302, {{"location", "/hop-2"}}, "");
          } else if (path == "/hop-2") {
            // Absolute, and to the same origin, so ResolveReference's absolute
            // branch is exercised too.
            status = response->SendResponse(
                302,
                {{"location",
                  absl::StrCat("http://127.0.0.1:", port(), "/body")}},
                "");
          } else if (path == "/loop") {
            status = response->SendResponse(302, {{"location", "/loop"}}, "");
          } else if (path == "/see-other") {
            status =
                response->SendResponse(303, {{"location", "/echo-method"}}, "");
          } else if (path == "/keep-method") {
            status =
                response->SendResponse(307, {{"location", "/echo-method"}}, "");
          } else if (path == "/echo-method") {
            status = response->SendResponse(
                200, {}, absl::StrCat(request.method, ":", request.body));
          } else if (path == "/no-location") {
            status = response->SendResponse(302, {}, "nowhere");
          } else if (path == "/missing") {
            status = response->SendResponse(404, {}, "not here");
          } else if (path == "/broken") {
            status = response->SendResponse(500, {}, "boom");
          } else if (path == "/chunks") {
            status = response->SendHeaders(200, {{"content-length", "9"}});
            if (status.ok()) {
              status = response->Write("abc");
            }
            if (status.ok()) {
              status = response->Write("def");
            }
            if (status.ok()) {
              status = response->Write("ghi");
            }
            if (status.ok()) {
              status = response->Finish();
            }
          } else {
            status = response->SendResponse(400, {}, "unknown path");
          }
          return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
        });
    server_ = std::move(server);
  }

  [[nodiscard]] bool ok() const { return server_.ok(); }

  [[nodiscard]] std::uint16_t port() const { return (*server_)->port(); }

  [[nodiscard]] std::string Url(std::string_view path) const {
    return absl::StrCat("http://127.0.0.1:", port(), path);
  }

  [[nodiscard]] const std::vector<std::string>& requests() const {
    return requests_;
  }

 private:
  absl::StatusOr<std::shared_ptr<Http2Server>> server_;
  std::vector<std::string> requests_;
};

TEST(HttpFetchTest, FetchesABufferedBody) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  const auto response = Fetch(server.Url("/body")).Await(Soon());
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->head.status, 200);
  EXPECT_EQ(response->body, "the-body");
}

TEST(HttpFetchTest, MapsAnErrorResponseOntoAStatus) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  const auto missing = Fetch(server.Url("/missing")).Await(Soon());
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.status().code(), absl::StatusCode::kNotFound);
  // The message names the URL, because "404" alone does not say what for.
  EXPECT_NE(missing.status().message().find("/missing"), std::string::npos);

  const auto broken = Fetch(server.Url("/broken")).Await(Soon());
  ASSERT_FALSE(broken.ok());
  EXPECT_EQ(broken.status().code(), absl::StatusCode::kInternal);
}

TEST(HttpFetchTest, FollowsARedirectChain) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  const auto response = Fetch(server.Url("/hop-1")).Await(Soon());
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->body, "the-body");
  EXPECT_EQ(server.requests(), (std::vector<std::string>{
                                   "GET /hop-1", "GET /hop-2", "GET /body"}));
}

TEST(HttpFetchTest, StopsAfterMaxRedirects) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  FetchOptions options;
  options.max_redirects = 3;
  const auto response = Fetch(server.Url("/loop"), options).Await(Soon());
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.status().code(), absl::StatusCode::kResourceExhausted);
  // Four attempts: the original plus the three it was allowed to follow.
  EXPECT_EQ(server.requests().size(), 4u);
}

TEST(HttpFetchTest, ZeroMaxRedirectsReturnsTheRedirectItself) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  FetchOptions options;
  options.max_redirects = 0;
  const auto response = Fetch(server.Url("/hop-1"), options).Await(Soon());
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->head.status, 302);
  EXPECT_EQ(GetHttpHeader(response->head.headers, "location"), "/hop-2");
}

TEST(HttpFetchTest, ARedirectWithoutALocationIsTheResponse) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  const auto response = Fetch(server.Url("/no-location")).Await(Soon());
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->head.status, 302);
  EXPECT_EQ(response->body, "nowhere");
}

TEST(HttpFetchTest, RewritesTheMethodOn303ButNotOn307) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  FetchOptions post;
  post.method = "POST";
  post.body = "payload";

  // 303 means "go look over there instead", so the body does not follow.
  const auto see_other = Fetch(server.Url("/see-other"), post).Await(Soon());
  ASSERT_TRUE(see_other.ok()) << see_other.status();
  EXPECT_EQ(see_other->body, "GET:");

  // 307 exists to preserve the method and body.
  const auto kept = Fetch(server.Url("/keep-method"), post).Await(Soon());
  ASSERT_TRUE(kept.ok()) << kept.status();
  EXPECT_EQ(kept->body, "POST:payload");
}

TEST(HttpFetchTest, StreamsToASinkAndReportsProgress) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  std::string collected;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
  const auto head = FetchToSink(
                        server.Url("/chunks"),
                        [&collected](std::string_view chunk) -> absl::Status {
                          collected.append(chunk);
                          return absl::OkStatus();
                        },
                        {},
                        [&progress](std::uint64_t done, std::uint64_t total) {
                          progress.emplace_back(done, total);
                        })
                        .Await(Soon());
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);
  EXPECT_EQ(collected, "abcdefghi");
  // An initial call so a caller can draw an empty bar, then one per chunk, with
  // the total taken from Content-Length.
  ASSERT_FALSE(progress.empty());
  EXPECT_EQ(progress.front().first, 0u);
  EXPECT_EQ(progress.back(),
            std::make_pair(std::uint64_t{9}, std::uint64_t{9}));
}

TEST(HttpFetchTest, ASinkFailureFailsTheFetch) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  const auto head =
      FetchToSink(server.Url("/chunks"), [](std::string_view) -> absl::Status {
        return absl::UnavailableError("disk full");
      }).Await(Soon());
  ASSERT_FALSE(head.ok());
  EXPECT_EQ(head.status().code(), absl::StatusCode::kUnavailable);
  EXPECT_EQ(head.status().message(), "disk full");
}

TEST(HttpFetchTest, EnforcesTheBufferedBodyLimit) {
  FetchTestServer server;
  ASSERT_TRUE(server.ok());

  FetchOptions options;
  options.transport.max_response_body_size = 4;
  const auto response = Fetch(server.Url("/body"), options).Await(Soon());
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.status().code(), absl::StatusCode::kOutOfRange);
}

TEST(HttpFetchTest, ABodyLargerThanTheBufferStreamsInsteadOfFailing) {
  // A reader slower than the link is ordinary, and on a fast path it is the
  // common case. The transport answers by pausing the socket -- closing the TCP
  // window back to the peer -- rather than by failing the transfer, which is
  // what it used to do once the response buffer filled.
  constexpr size_t kBodySize = 8 * 1024 * 1024;
  const std::string body(kBodySize, 'x');
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [&body](
          const HttpRequest&,
          const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
        const absl::Status status = response->SendResponse(
            200, {{"content-length", absl::StrCat(body.size())}}, body);
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();

  FetchOptions options;
  // Deliberately far smaller than the body, so the buffer bound is crossed many
  // times over the course of the transfer.
  options.transport.max_buffered_response_bytes = 256 * 1024;
  options.transport.max_response_body_size = 2 * kBodySize;

  std::uint64_t received = 0;
  const auto head =
      FetchToSink(
          absl::StrCat("http://127.0.0.1:", (*server)->port(), "/bulk"),
          [&received](std::string_view chunk) -> absl::Status {
            received += chunk.size();
            return absl::OkStatus();
          },
          options)
          .Await(absl::Now() + absl::Seconds(60));
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);
  EXPECT_EQ(received, kBodySize);
}

TEST(HttpFetchTest, RejectsUrlsAndOptionsItCannotUse) {
  EXPECT_EQ(Fetch("not-a-url").Await(Soon()).status().code(),
            absl::StatusCode::kInvalidArgument);
  // wss parses, but it is not something to fetch.
  EXPECT_EQ(Fetch("wss://example.com/x").Await(Soon()).status().code(),
            absl::StatusCode::kInvalidArgument);

  FetchOptions bad;
  bad.max_redirects = -1;
  EXPECT_EQ(Fetch("http://example.com/x", bad).Await(Soon()).status().code(),
            absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace a11::net

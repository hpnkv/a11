// Copyright 2026 The A11 Authors.

#include "a11/net/http2.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"

namespace a11::net {
namespace {

std::string TestDataPath(std::string_view name) {
  return ((std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() /
           std::filesystem::path(__FILE__))
              .parent_path() /
          "testdata" / name)
      .string();
}

TEST(Http2Test, MultiplexesBufferedAndStreamingResponses) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest& request,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
        if (request.path == "/stream") {
          absl::Status status = response->SendHeaders(
              200, {{"content-type", "application/octet-stream"},
                    {"x-repeat", "first"},
                    {"x-repeat", "second"}});
          if (!status.ok()) {
            return a11::FailedTask(status);
          }
          status = response->Write("first-");
          if (!status.ok()) {
            return a11::FailedTask(status);
          }
          status = response->Write("second");
          if (!status.ok()) {
            return a11::FailedTask(status);
          }
          status = response->Finish();
          return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
        }
        HttpHeaders headers{{"content-type", "text/plain"}};
        absl::Status status = response->SendResponse(
            201, std::move(headers), request.method + ":" + request.body);
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();
  ASSERT_NE((*server)->port(), 0);

  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  auto first = (*client)->Request("POST", "/echo", {}, "payload");
  auto second = (*client)->Request("GET", "/stream");
  auto first_response = first.Await(absl::Now() + absl::Seconds(5));
  auto second_response = second.Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(first_response.ok()) << first_response.status();
  ASSERT_TRUE(second_response.ok()) << second_response.status();
  EXPECT_EQ(first_response->head.status, 201);
  EXPECT_EQ(first_response->body, "POST:payload");
  EXPECT_EQ(second_response->head.status, 200);
  EXPECT_EQ(second_response->body, "first-second");
  EXPECT_EQ(std::count_if(
                second_response->head.headers.begin(),
                second_response->head.headers.end(),
                [](const auto& header) { return header.first == "x-repeat"; }),
            2);

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http2Test, DeliversTrailersAfterTheBody) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest& /*request*/,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
        absl::Status status = response->SendHeaders(
            200, {{"content-type", "text/plain"}, {"trailer", "x-digest"}});
        if (!status.ok()) {
          return a11::FailedTask(status);
        }
        status = response->Write("weighed-and-measured");
        if (!status.ok()) {
          return a11::FailedTask(status);
        }
        // The trailer section a checksum belongs in: it cannot be sent with the
        // headers because it is not known until the body has been produced,
        // which is the whole reason the protocol has trailers.
        status = response->FinishWithTrailers(
            {{"x-digest", "20"}, {"x-repeat", "one"}, {"x-repeat", "two"}});
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  auto stream = (*client)->RequestStream("GET", "/trailed");
  ASSERT_TRUE(stream.ok()) << stream.status();
  auto head = (*stream)->Headers().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);

  std::string body;
  while (true) {
    auto chunk = (*stream)->Read().Await(absl::Now() + absl::Seconds(5));
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;
    }
    body += **chunk;
  }
  EXPECT_EQ(body, "weighed-and-measured");

  auto trailers = (*stream)->Trailers().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(trailers.ok()) << trailers.status();
  EXPECT_EQ(GetHttpHeader(*trailers, "x-digest"), "20");
  // Repeats survive, as they do in the header block: the trailer section is an
  // ordered field list and not a map.
  EXPECT_EQ(std::count_if(
                trailers->begin(), trailers->end(),
                [](const auto& field) { return field.first == "x-repeat"; }),
            2);
  // Pseudo-headers are never part of a trailer section.
  EXPECT_EQ(GetHttpHeader(*trailers, ":status"), std::nullopt);

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http2Test, ReportsAnEmptyTrailerSectionWhenThePeerSendsNone) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest&,
         const std::shared_ptr<Http2ResponseWriter>& response) {
        const absl::Status status = response->SendResponse(200, {}, "plain");
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  auto stream = (*client)->RequestStream("GET", "/plain");
  ASSERT_TRUE(stream.ok()) << stream.status();
  ASSERT_TRUE((*stream)->Done().Await(absl::Now() + absl::Seconds(5)).ok());
  // Resolving empty rather than never resolving is what lets a reader await
  // trailers unconditionally.
  auto trailers = (*stream)->Trailers().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(trailers.ok()) << trailers.status();
  EXPECT_TRUE(trailers->empty());
  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http2Test, StreamsARequestBodyWrittenAfterTheHeaders) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest& request,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
        // The server sees one ordinary request with a complete body, however
        // many DATA frames it arrived in.
        const absl::Status status = response->SendResponse(
            200, {}, absl::StrCat(request.method, ":", request.body));
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  // No content-length: the length is not known when the headers go out, which
  // is the point of a streamed body.
  auto upload = (*client)->RequestStreamingBody(
      "PUT", "/upload", {{"content-type", "application/octet-stream"}});
  ASSERT_TRUE(upload.ok()) << upload.status();
  EXPECT_TRUE((*upload)->Write("one-").ok());
  EXPECT_TRUE((*upload)->Write("two-").ok());
  EXPECT_TRUE((*upload)->Write("three").ok());
  EXPECT_TRUE((*upload)->Finish().ok());

  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  auto head = (*upload)->Headers().Await(deadline);
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);
  std::string body;
  while (true) {
    auto chunk = (*upload)->Read().Await(deadline);
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;
    }
    body += **chunk;
  }
  EXPECT_EQ(body, "PUT:one-two-three");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

// A server that answers /index with a page and pushes /style.css alongside it,
// the shape server push exists for.
absl::StatusOr<std::shared_ptr<Http2Server>> PushingServer() {
  return Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest&,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
        // The promise goes out before the response it accompanies is finished;
        // after that the protocol has nowhere to put it.
        //
        // A push that the client refuses -- because it disabled push, or reset
        // the promised stream -- must not take the response down with it, so
        // neither failure here is fatal. That is what a real server does too.
        if (absl::StatusOr<std::shared_ptr<Http2ResponseWriter>> pushed =
                response->PushPromise("GET", "/style.css",
                                      {{"accept", "text/css"}});
            pushed.ok()) {
          (void)(*pushed)->SendResponse(200, {{"content-type", "text/css"}},
                                        "body{}");
        }
        const absl::Status status = response->SendResponse(
            200, {{"content-type", "text/html"}}, "<link href=style.css>");
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
}

TEST(Http2Test, DeliversAPushedResponseOnTheAssociatedStream) {
  auto server = PushingServer();
  ASSERT_TRUE(server.ok()) << server.status();
  Http2Options options;
  options.enable_push = true;
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port(), options)
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  auto stream = (*client)->RequestStream("GET", "/index");
  ASSERT_TRUE(stream.ok()) << stream.status();

  auto promised = (*stream)->NextPush().Await(deadline);
  ASSERT_TRUE(promised.ok()) << promised.status();
  ASSERT_TRUE(promised->has_value());
  // The promise describes a request the client never made.
  EXPECT_EQ((*promised)->method, "GET");
  EXPECT_EQ((*promised)->path, "/style.css");
  EXPECT_EQ((*promised)->scheme, "http");
  EXPECT_EQ(GetHttpHeader((*promised)->headers, "accept"), "text/css");

  // And its response reads exactly like any other.
  auto pushed_head = (*promised)->response->Headers().Await(deadline);
  ASSERT_TRUE(pushed_head.ok()) << pushed_head.status();
  EXPECT_EQ(pushed_head->status, 200);
  EXPECT_EQ(GetHttpHeader(pushed_head->headers, "content-type"), "text/css");
  std::string pushed_body;
  while (true) {
    auto chunk = (*promised)->response->Read().Await(deadline);
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;
    }
    pushed_body += **chunk;
  }
  EXPECT_EQ(pushed_body, "body{}");

  std::string body;
  while (true) {
    auto chunk = (*stream)->Read().Await(deadline);
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;
    }
    body += **chunk;
  }
  EXPECT_EQ(body, "<link href=style.css>");

  // The associated response has ended, so no further promise can arrive on it.
  auto after = (*stream)->NextPush().Await(deadline);
  ASSERT_TRUE(after.ok()) << after.status();
  EXPECT_FALSE(after->has_value());

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http2Test, DeliversNoPushWhenTheClientDidNotEnableIt) {
  auto server = PushingServer();
  ASSERT_TRUE(server.ok()) << server.status();
  // The default: push off and advertised as off, so nothing this side did not
  // ask for is ever delivered to it. The server may still try -- a SETTINGS
  // frame and a request can cross -- and the promised stream is then reset
  // without the response it came with noticing.
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  auto stream = (*client)->RequestStream("GET", "/index");
  ASSERT_TRUE(stream.ok()) << stream.status();
  std::string body;
  while (true) {
    auto chunk = (*stream)->Read().Await(deadline);
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;
    }
    body += **chunk;
  }
  EXPECT_EQ(body, "<link href=style.css>");
  auto promised = (*stream)->NextPush().Await(deadline);
  ASSERT_TRUE(promised.ok()) << promised.status();
  EXPECT_FALSE(promised->has_value());

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http2Test, CancellingAPushedResponseRefusesIt) {
  auto server = PushingServer();
  ASSERT_TRUE(server.ok()) << server.status();
  Http2Options options;
  options.enable_push = true;
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port(), options)
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  auto stream = (*client)->RequestStream("GET", "/index");
  ASSERT_TRUE(stream.ok()) << stream.status();
  auto promised = (*stream)->NextPush().Await(deadline);
  ASSERT_TRUE(promised.ok()) << promised.status();
  ASSERT_TRUE(promised->has_value());

  // Refusing a push is Cancel() on its response: this is how a client that
  // already has the resource cached declines to receive it again.
  EXPECT_TRUE((*promised)
                  ->response->Cancel(absl::CancelledError("already cached"))
                  .ok());

  // Refusing the push leaves the response it came with alone.
  std::string body;
  while (true) {
    auto chunk = (*stream)->Read().Await(deadline);
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;
    }
    body += **chunk;
  }
  EXPECT_EQ(body, "<link href=style.css>");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http2Test, PropagatesHandlerStatusAsHttpResponse) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest&, const std::shared_ptr<Http2ResponseWriter>&) {
        return a11::FailedTask(absl::PermissionDeniedError("not allowed"));
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  auto response = (*client)
                      ->Request("GET", "/denied")
                      .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->head.status, 403);
  EXPECT_EQ(response->body, "not allowed");
  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http2Test, ExtendedConnectCarriesDuplexData) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](HttpRequest request,
         std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
        if (request.protocol != "echo" || request.body_stream == nullptr) {
          return a11::FailedTask(
              absl::InvalidArgumentError("expected an echo CONNECT stream"));
        }
        return a11::Submit<a11::Unit>(
            [body = std::move(request.body_stream),
             response = std::move(response)]() -> absl::StatusOr<a11::Unit> {
              absl::Status status = response->SendHeaders(200);
              if (!status.ok()) {
                return status;
              }
              auto incoming =
                  body->Read().Await(absl::Now() + absl::Seconds(5));
              if (!incoming.ok()) {
                return incoming.status();
              }
              if (!incoming->has_value()) {
                return absl::DataLossError("CONNECT body ended before data");
              }
              status = response->Write(absl::StrCat("echo:", **incoming));
              if (!status.ok()) {
                return status;
              }
              auto end = body->Read().Await(absl::Now() + absl::Seconds(5));
              if (!end.ok()) {
                return end.status();
              }
              if (end->has_value()) {
                return absl::DataLossError("unexpected second CONNECT chunk");
              }
              status = response->Finish();
              if (!status.ok()) {
                return status;
              }
              return a11::Unit{};
            });
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  auto stream = (*client)->ExtendedConnect("echo", "/duplex");
  ASSERT_TRUE(stream.ok()) << stream.status();
  auto head = (*stream)->Headers().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);
  ASSERT_TRUE((*stream)->Write("ping").ok());
  auto echoed = (*stream)->Read().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(echoed.ok()) << echoed.status();
  ASSERT_TRUE(echoed->has_value());
  EXPECT_EQ(**echoed, "echo:ping");
  ASSERT_TRUE((*stream)->Finish().ok());
  auto end = (*stream)->Read().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(end.ok()) << end.status();
  EXPECT_FALSE(end->has_value());
  EXPECT_TRUE((*stream)->Done().Await(absl::Now() + absl::Seconds(5)).ok());
  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http2Test, NegotiatesTlsWithH2AlpnAndVerifiesPeer) {
  Http2Options server_options;
  server_options.tls.enabled = true;
  server_options.tls.certificate_pem_file = TestDataPath("localhost-cert.pem");
  server_options.tls.key_pem_file = TestDataPath("localhost-key.pem");
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest& request,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
        if (request.scheme != "https") {
          return a11::FailedTask(
              absl::DataLossError("TLS request did not use https"));
        }
        absl::Status status = response->SendResponse(
            200, {{"content-type", "text/plain"}}, "secure-h2");
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      },
      server_options);
  ASSERT_TRUE(server.ok()) << server.status();
  EXPECT_TRUE((*server)->secure());

  Http2Options client_options;
  client_options.tls.enabled = true;
  client_options.tls.ca_certificate_pem_file =
      TestDataPath("localhost-cert.pem");
  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), client_options)
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  EXPECT_TRUE((*client)->secure());
  auto response = (*client)
                      ->Request("GET", "/secure")
                      .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->head.status, 200);
  EXPECT_EQ(response->body, "secure-h2");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

}  // namespace
}  // namespace a11::net

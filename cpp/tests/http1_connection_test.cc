// Copyright 2026 The A11 Authors.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"

namespace a11::net {
namespace {

std::string TestDataPath(std::string_view name) {
  return ((std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() /
           std::filesystem::path(__FILE__))
              .parent_path() /
          "testdata" / name)
      .string();
}

// Client options that force the native client onto HTTP/1.1. The cleartext
// server (all protocols enabled) sniffs the request line and accepts it over
// an Http1Connection.
Http2Options Http1ClientOptions() {
  Http2Options options;
  options.client_preference = Http2Options::ProtocolPreference::kHttp11;
  return options;
}

TEST(Http1ConnectionTest, BufferedRequestResponseRoundTrip) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](HttpRequest request,
         std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
        HttpHeaders headers{{"content-type", "text/plain"}};
        absl::Status status = response->SendResponse(
            201, std::move(headers), request.method + ":" + request.body);
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();

  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), Http1ClientOptions())
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  auto response = (*client)
                      ->Request("POST", "/echo", {}, "payload")
                      .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->head.status, 201);
  EXPECT_EQ(response->body, "POST:payload");
  EXPECT_EQ(GetHttpHeader(response->head.headers, "content-type"),
            "text/plain");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http1ConnectionTest, ChunkedStreamingResponse) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](HttpRequest request,
         std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
        EXPECT_EQ(request.path, "/stream");
        // No Content-Length -> chunked transfer-encoding on the wire.
        absl::Status status =
            response->SendHeaders(200, {{"content-type", "text/event-stream"}});
        if (!status.ok()) return a11::FailedTask(status);
        status = response->Write("data: one\n\n");
        if (!status.ok()) return a11::FailedTask(status);
        status = response->Write("data: two\n\n");
        if (!status.ok()) return a11::FailedTask(status);
        status = response->Finish();
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();

  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), Http1ClientOptions())
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  auto stream = (*client)->RequestStream("GET", "/stream",
                                         {{"accept", "text/event-stream"}});
  ASSERT_TRUE(stream.ok()) << stream.status();

  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  auto head = (*stream)->Headers().Await(deadline);
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);
  EXPECT_EQ(GetHttpHeader(head->headers, "content-type"), "text/event-stream");

  // The client de-chunks; the reader observes the plaintext event stream.
  std::string body;
  while (true) {
    auto chunk = (*stream)->Read().Await(deadline);
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) break;
    body += **chunk;
  }
  EXPECT_EQ(body, "data: one\n\ndata: two\n\n");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http1ConnectionTest, StreamsAChunkedRequestBody) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](HttpRequest request,
         std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
        EXPECT_EQ(GetHttpHeader(request.headers, "transfer-encoding"),
                  "chunked");
        const absl::Status status =
            response->SendResponse(200, {}, absl::StrCat("got:", request.body));
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), Http1ClientOptions())
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  auto upload = (*client)->RequestStreamingBody("POST", "/upload");
  ASSERT_TRUE(upload.ok()) << upload.status();
  EXPECT_TRUE((*upload)->Write("alpha").ok());
  EXPECT_TRUE((*upload)->Write("beta").ok());
  EXPECT_TRUE((*upload)->Finish().ok());

  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  auto head = (*upload)->Headers().Await(deadline);
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);
  std::string body;
  while (true) {
    auto chunk = (*upload)->Read().Await(deadline);
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) break;
    body += **chunk;
  }
  EXPECT_EQ(body, "got:alphabeta");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http1ConnectionTest, RejectsContentLengthOnAStreamedRequestBody) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](HttpRequest, std::shared_ptr<Http2ResponseWriter> response) {
        const absl::Status status = response->SendResponse(200, {}, "");
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), Http1ClientOptions())
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  // The two framings are mutually exclusive; saying both is a caller error, not
  // something to quietly pick a winner for.
  auto upload = (*client)->RequestStreamingBody("POST", "/upload",
                                                {{"content-length", "5"}});
  EXPECT_TRUE(absl::IsInvalidArgument(upload.status())) << upload.status();
  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http1ConnectionTest, DeliversChunkedTrailers) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](HttpRequest, std::shared_ptr<Http2ResponseWriter> response)
          -> a11::Task {
        absl::Status status = response->SendHeaders(
            200, {{"content-type", "text/plain"}, {"trailer", "x-digest"}});
        if (!status.ok()) return a11::FailedTask(status);
        status = response->Write("counted");
        if (!status.ok()) return a11::FailedTask(status);
        status = response->FinishWithTrailers({{"x-digest", "7"}});
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();

  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), Http1ClientOptions())
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  auto stream = (*client)->RequestStream("GET", "/trailed");
  ASSERT_TRUE(stream.ok()) << stream.status();

  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  std::string body;
  while (true) {
    auto chunk = (*stream)->Read().Await(deadline);
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) break;
    body += **chunk;
  }
  EXPECT_EQ(body, "counted");
  auto trailers = (*stream)->Trailers().Await(deadline);
  ASSERT_TRUE(trailers.ok()) << trailers.status();
  EXPECT_EQ(GetHttpHeader(*trailers, "x-digest"), "7");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http1ConnectionTest, RejectsMultipleConcurrentRequests) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](HttpRequest, std::shared_ptr<Http2ResponseWriter> response)
          -> a11::Task {
        // Never finish: keep the single exchange occupied.
        (void)response->SendHeaders(200, {{"content-type", "text/plain"}});
        return a11::ReadyTask();
      });
  ASSERT_TRUE(server.ok()) << server.status();

  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), Http1ClientOptions())
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  auto first = (*client)->RequestStream("GET", "/a");
  ASSERT_TRUE(first.ok()) << first.status();
  // HTTP/1.1 carries one request per connection; a second is rejected.
  auto second = (*client)->RequestStream("GET", "/b");
  EXPECT_FALSE(second.ok());

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http1ConnectionTest, NegotiatesHttp1OverTlsViaAlpn) {
  Http2Options server_options;
  server_options.tls.enabled = true;
  server_options.tls.certificate_pem_file = TestDataPath("localhost-cert.pem");
  server_options.tls.key_pem_file = TestDataPath("localhost-key.pem");
  server_options.enable_h2 = false;    // Serve HTTP/1.1 over TLS.
  server_options.enable_http1 = true;
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](HttpRequest request,
         std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
        EXPECT_EQ(request.scheme, "https");
        absl::Status status = response->SendResponse(
            200, {{"content-type", "text/plain"}}, "secure-http1");
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      },
      server_options);
  ASSERT_TRUE(server.ok()) << server.status();

  Http2Options client_options;
  client_options.tls.enabled = true;
  client_options.tls.ca_certificate_pem_file =
      TestDataPath("localhost-cert.pem");
  client_options.client_preference = Http2Options::ProtocolPreference::kHttp11;
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
  EXPECT_EQ(response->body, "secure-http1");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

// A cleartext client with an explicit HTTP/1.1 preference reaches a server that
// serves only HTTP/1.1 -- the server's preface sniff accepts it directly. (This
// is the recommended way to pin a cleartext protocol; the h2c client publishes
// readiness optimistically, so h2c-vs-http1 mismatches surface on the first
// request rather than at connect, which client_allow_downgrade cannot preempt.)
TEST(Http1ConnectionTest, CleartextHttp1OnlyServerServesExplicitHttp1Client) {
  Http2Options server_options;
  server_options.enable_h2c = false;
  server_options.enable_http1 = true;
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](HttpRequest request,
         std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
        absl::Status status = response->SendResponse(
            200, {{"content-type", "text/plain"}}, "http1:" + request.body);
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      },
      server_options);
  ASSERT_TRUE(server.ok()) << server.status();

  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), Http1ClientOptions())
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  auto response = (*client)
                      ->Request("POST", "/x", {}, "hi")
                      .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->body, "http1:hi");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

}  // namespace
}  // namespace a11::net

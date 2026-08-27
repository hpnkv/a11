// Copyright 2026 The A11 Authors.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/executor.h"
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
      [](const HttpRequest& request,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
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
      [](const HttpRequest& request,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
        EXPECT_EQ(request.path, "/stream");
        // No Content-Length -> chunked transfer-encoding on the wire.
        absl::Status status =
            response->SendHeaders(200, {{"content-type", "text/event-stream"}});
        if (!status.ok()) {
          return a11::FailedTask(status);
        }
        status = response->Write("data: one\n\n");
        if (!status.ok()) {
          return a11::FailedTask(status);
        }
        status = response->Write("data: two\n\n");
        if (!status.ok()) {
          return a11::FailedTask(status);
        }
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
    if (!chunk->has_value()) {
      break;
    }
    body += **chunk;
  }
  EXPECT_EQ(body, "data: one\n\ndata: two\n\n");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http1ConnectionTest, StreamsAChunkedRequestBody) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest& request,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
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
    if (!chunk->has_value()) {
      break;
    }
    body += **chunk;
  }
  EXPECT_EQ(body, "got:alphabeta");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(Http1ConnectionTest, RejectsContentLengthOnAStreamedRequestBody) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest&,
         const std::shared_ptr<Http2ResponseWriter>& response) {
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
      [](const HttpRequest&,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
        absl::Status status = response->SendHeaders(
            200, {{"content-type", "text/plain"}, {"trailer", "x-digest"}});
        if (!status.ok()) {
          return a11::FailedTask(status);
        }
        status = response->Write("counted");
        if (!status.ok()) {
          return a11::FailedTask(status);
        }
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
    if (!chunk->has_value()) {
      break;
    }
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
      [](const HttpRequest&,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
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
  server_options.enable_h2 = false;  // Serve HTTP/1.1 over TLS.
  server_options.enable_http1 = true;
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest& request,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
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
// serves only HTTP/1.1 -- the server's preface sniff accepts it directly.
TEST(Http1ConnectionTest, CleartextHttp1OnlyServerServesExplicitHttp1Client) {
  Http2Options server_options;
  server_options.enable_h2c = false;
  server_options.enable_http1 = true;
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest& request,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
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

// Http2Options::stream_request_body on an ordinary chunked POST: the handler
// runs while the body is still arriving, which is what an open-ended upload
// needs and what the SSE transport's streamed outbound direction is built on.
TEST(Http1ConnectionTest, StreamsAnAcceptedRequestBodyToTheHandler) {
  auto first_chunk_seen = std::make_shared<a11::Promise<a11::Unit>>();
  a11::Task first_chunk_arrived = first_chunk_seen->future();

  Http2Options server_options;
  server_options.stream_request_body =
      [](std::string_view method, std::string_view path, const HttpHeaders&) {
        return method == "POST" && path == "/upload";
      };
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [first_chunk_seen](
          HttpRequest request,
          std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
        return a11::SubmitTask([first_chunk_seen, request = std::move(request),
                                response = std::move(
                                    response)]() mutable -> absl::Status {
          if (request.body_stream == nullptr) {
            return absl::FailedPreconditionError("body was not streamed");
          }
          if (!request.body.empty()) {
            return absl::FailedPreconditionError(
                "a streamed body must not also be buffered");
          }
          std::string body;
          bool first = true;
          while (true) {
            ABSL_ASSIGN_OR_RETURN(std::optional<std::string> chunk,
                                  request.body_stream->Read().Await(
                                      absl::Now() + absl::Seconds(5)));
            if (!chunk.has_value()) {
              break;
            }
            if (first) {
              first = false;
              (void)first_chunk_seen->SetValue(a11::Unit{});
            }
            body.append(*chunk);
          }
          return response->SendResponse(200, {}, absl::StrCat("got:", body));
        });
      },
      server_options);
  ASSERT_TRUE(server.ok()) << server.status();

  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), Http1ClientOptions())
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  auto upload = (*client)->RequestStreamingBody("POST", "/upload");
  ASSERT_TRUE(upload.ok()) << upload.status();

  ASSERT_TRUE((*upload)->Write("first").ok());
  // The handler has the first chunk before the body ends: proof it was
  // dispatched
  // on the headers rather than at END_STREAM.
  ASSERT_TRUE(first_chunk_arrived.Await(absl::Now() + absl::Seconds(5)).ok());
  ASSERT_TRUE((*upload)->Write("-second").ok());
  ASSERT_TRUE((*upload)->Finish().ok());

  auto head = (*upload)->Headers().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);
  std::string body;
  while (true) {
    auto chunk = (*upload)->Read().Await(absl::Now() + absl::Seconds(5));
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;
    }
    body.append(**chunk);
  }
  EXPECT_EQ(body, "got:first-second");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

// The same, over HTTP/2 -- where the body is DATA frames rather than a chunked
// body, and the decision is made in the frame callback instead of the parser.
TEST(Http1ConnectionTest, StreamsAnAcceptedRequestBodyOverHttp2) {
  auto first_chunk_seen = std::make_shared<a11::Promise<a11::Unit>>();
  a11::Task first_chunk_arrived = first_chunk_seen->future();

  Http2Options server_options;
  server_options.enable_http1 = false;
  server_options.stream_request_body =
      [](std::string_view, std::string_view path, const HttpHeaders& headers) {
        return path == "/upload" &&
               GetHttpHeader(headers, "x-stream-me").has_value();
      };
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [first_chunk_seen](
          HttpRequest request,
          std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
        return a11::SubmitTask([first_chunk_seen, request = std::move(request),
                                response = std::move(
                                    response)]() mutable -> absl::Status {
          if (request.body_stream == nullptr) {
            return response->SendResponse(
                200, {}, absl::StrCat("buffered:", request.body));
          }
          std::string body;
          bool first = true;
          while (true) {
            ABSL_ASSIGN_OR_RETURN(std::optional<std::string> chunk,
                                  request.body_stream->Read().Await(
                                      absl::Now() + absl::Seconds(5)));
            if (!chunk.has_value()) {
              break;
            }
            if (first) {
              first = false;
              (void)first_chunk_seen->SetValue(a11::Unit{});
            }
            body.append(*chunk);
          }
          return response->SendResponse(200, {}, absl::StrCat("got:", body));
        });
      },
      server_options);
  ASSERT_TRUE(server.ok()) << server.status();

  Http2Options client_options;
  client_options.client_preference = Http2Options::ProtocolPreference::kHttp2;
  auto client =
      Http2Client::Connect("127.0.0.1", (*server)->port(), client_options)
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  auto upload = (*client)->RequestStreamingBody("POST", "/upload",
                                                {{"x-stream-me", "yes"}});
  ASSERT_TRUE(upload.ok()) << upload.status();
  ASSERT_TRUE((*upload)->Write("first").ok());
  ASSERT_TRUE(first_chunk_arrived.Await(absl::Now() + absl::Seconds(5)).ok());
  ASSERT_TRUE((*upload)->Write("-second").ok());
  ASSERT_TRUE((*upload)->Finish().ok());

  auto head = (*upload)->Headers().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);
  std::string body;
  while (true) {
    auto chunk = (*upload)->Read().Await(absl::Now() + absl::Seconds(5));
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;
    }
    body.append(**chunk);
  }
  EXPECT_EQ(body, "got:first-second");

  // A request the predicate declines -- same path, no x-stream-me -- is still
  // buffered whole and dispatched at its end.
  auto buffered = (*client)
                      ->Request("POST", "/upload", {}, "plain")
                      .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(buffered.ok()) << buffered.status();
  EXPECT_EQ(buffered->head.status, 200);
  EXPECT_EQ(buffered->body, "buffered:plain");

  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

}  // namespace
}  // namespace a11::net

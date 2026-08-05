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
      [](HttpRequest request,
         std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
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

TEST(Http2Test, PropagatesHandlerStatusAsHttpResponse) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0, [](HttpRequest, std::shared_ptr<Http2ResponseWriter>) {
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
      [](HttpRequest request,
         std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
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

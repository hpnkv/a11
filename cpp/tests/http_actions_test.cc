// Copyright 2026 The A11 Authors.

#include "sdk/http/actions/http_actions.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/net/http2.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"

namespace a11::sdk::http {
namespace {

using ::a11::actions::Action;
using ::a11::actions::ActionRegistry;
using ::a11::net::GetHttpHeader;
using ::a11::net::Http2ResponseWriter;
using ::a11::net::Http2Server;
using ::a11::net::HttpRequest;
using ::a11::nodes::AsyncNode;

constexpr absl::Duration kPatience = absl::Seconds(10);

/**
 * A server covering the shapes these actions have to get right: a plain body, a
 * trailer section, a redirect chain, repeated header fields, an error document,
 * a streamed upload, a server push, and the three payload shapes web-fetch
 * decodes.
 */
class ActionTestServer {
 public:
  ActionTestServer() {
    auto server = Http2Server::Create(
        "127.0.0.1", 0,
        [this](HttpRequest request,
               std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
          const absl::Status status = Serve(request, response);
          return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
        });
    server_ = server.ok() ? *server : nullptr;
  }

  ~ActionTestServer() {
    if (server_ != nullptr) {
      (void)server_->Stop();
    }
  }

  [[nodiscard]] bool ok() const { return server_ != nullptr; }
  [[nodiscard]] std::string url(std::string_view path) const {
    return absl::StrCat("http://127.0.0.1:", server_->port(), path);
  }
  /// The headers the server last saw, for asserting what was actually sent.
  [[nodiscard]] const a11::net::HttpHeaders& seen_headers() const {
    return seen_headers_;
  }
  [[nodiscard]] const std::string& seen_body() const { return seen_body_; }
  [[nodiscard]] const std::string& seen_method() const { return seen_method_; }

 private:
  absl::Status Serve(const HttpRequest& request,
                     const std::shared_ptr<Http2ResponseWriter>& response) {
    seen_headers_ = request.headers;
    seen_body_ = request.body;
    seen_method_ = request.method;
    const std::string& path = request.path;

    if (path == "/plain") {
      return response->SendResponse(
          200, {{"content-type", "text/plain"}, {"x-tag", "one"},
                {"x-tag", "two"}},
          "a plain body");
    }
    if (path == "/trailed") {
      ABSL_RETURN_IF_ERROR(response->SendHeaders(
          200, {{"content-type", "text/plain"}, {"trailer", "x-digest"}}));
      ABSL_RETURN_IF_ERROR(response->Write("first "));
      ABSL_RETURN_IF_ERROR(response->Write("second"));
      return response->FinishWithTrailers({{"x-digest", "12"}});
    }
    if (path == "/hop-1") {
      return response->SendResponse(302, {{"location", "/hop-2"}}, "");
    }
    if (path == "/hop-2") {
      return response->SendResponse(302, {{"location", "/plain"}}, "");
    }
    if (path == "/loop") {
      return response->SendResponse(302, {{"location", "/loop"}}, "");
    }
    if (path == "/see-other") {
      return response->SendResponse(303, {{"location", "/echo"}}, "");
    }
    if (path == "/echo") {
      return response->SendResponse(
          200, {{"content-type", "text/plain"}},
          absl::StrCat(request.method, ":", request.body));
    }
    if (path == "/missing") {
      return response->SendResponse(404, {{"content-type", "text/plain"}},
                                    "no such thing");
    }
    if (path == "/json") {
      return response->SendResponse(200, {{"content-type", "application/json"}},
                                    R"({"name": "a11", "count": 2})");
    }
    if (path == "/array") {
      return response->SendResponse(200, {{"content-type", "application/json"}},
                                    R"([{"n": 1}, {"n": 2}, {"n": 3}])");
    }
    if (path == "/ndjson") {
      return response->SendResponse(
          200, {{"content-type", "application/x-ndjson"}},
          "{\"n\": 1}\n{\"n\": 2}\n{\"n\": 3}\n");
    }
    if (path == "/events") {
      ABSL_RETURN_IF_ERROR(
          response->SendHeaders(200, {{"content-type", "text/event-stream"}}));
      ABSL_RETURN_IF_ERROR(
          response->Write("event: tick\ndata: {\"n\": 1}\nid: 1\n\n"));
      ABSL_RETURN_IF_ERROR(response->Write("data: {\"n\": 2}\n\n"));
      // A comment, which a real server sends to keep the connection alive.
      ABSL_RETURN_IF_ERROR(response->Write(": keep-alive\n\n"));
      ABSL_RETURN_IF_ERROR(response->Write("data: bare text\n\n"));
      return response->Finish();
    }
    if (path == "/pushing") {
      if (absl::StatusOr<std::shared_ptr<Http2ResponseWriter>> pushed =
              response->PushPromise("GET", "/style.css");
          pushed.ok()) {
        (void)(*pushed)->SendResponse(200, {{"content-type", "text/css"}},
                                      "body{}");
      }
      return response->SendResponse(200, {{"content-type", "text/html"}},
                                    "<html>");
    }
    return response->SendResponse(404, {}, "unknown path");
  }

  std::shared_ptr<Http2Server> server_;
  a11::net::HttpHeaders seen_headers_;
  std::string seen_body_;
  std::string seen_method_;
};

// --- Reading an action's ports ------------------------------------------------

/// Writes one JSON document to a unary input port and ends it.
absl::Status PutJson(const std::shared_ptr<Action>& action,
                     std::string_view port, const nlohmann::json& value) {
  data::Chunk chunk;
  chunk.metadata =
      data::ChunkMetadata{.mimetype = std::string(data::kJsonMimetype)};
  chunk.data = value.dump();
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> node,
                        action->GetInput(std::string(port), false));
  return node->PutChunk(std::move(chunk), std::nullopt, /*final=*/true)
      .Await()
      .status();
}

absl::Status PutBody(const std::shared_ptr<Action>& action,
                     const std::vector<std::string>& pieces) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> node,
                        action->GetInput("request_body", false));
  for (const std::string& piece : pieces) {
    data::Chunk chunk;
    chunk.metadata = data::ChunkMetadata{
        .mimetype = std::string("application/octet-stream")};
    chunk.data = piece;
    ABSL_RETURN_IF_ERROR(
        node->PutChunk(std::move(chunk), std::nullopt, false).Await().status());
  }
  return node->Finalize({.wait = true, .close = false}).Await().status();
}

/// A ready-to-run make_http_request with every input port fed. Every port is
/// written exactly once, because a unary port's end is established by the first
/// final write and a second one is rejected.
std::shared_ptr<Action> MakeRequest(std::string id, std::string url,
                                    nlohmann::json options = {},
                                    std::string method = {},
                                    std::vector<std::string> body = {}) {
  absl::StatusOr<std::shared_ptr<Action>> created = Action::Create(
      MakeHttpRequestSchema(), std::move(id), MakeHttpRequestHandler());
  if (!created.ok()) {
    return nullptr;
  }
  const std::shared_ptr<Action> action = *created;
  if (!PutJson(action, "url", url).ok()) {
    return nullptr;
  }
  if (!PutJson(action, "method", method.empty() ? nlohmann::json()
                                                : nlohmann::json(method))
           .ok()) {
    return nullptr;
  }
  if (!PutJson(action, "options",
               options.is_null() ? nlohmann::json::object() : options)
           .ok()) {
    return nullptr;
  }
  if (!PutBody(action, body).ok()) {
    return nullptr;
  }
  return action;
}

/// Reads every value on a port, as raw payloads.
std::vector<std::string> ReadAll(const std::shared_ptr<Action>& action,
                                 std::string_view port) {
  std::vector<std::string> values;
  absl::StatusOr<std::shared_ptr<AsyncNode>> node =
      action->GetOutput(std::string(port));
  if (!node.ok()) {
    return values;
  }
  while (true) {
    absl::StatusOr<std::optional<data::Chunk>> chunk =
        (*node)->NextChunk(kPatience).Await();
    if (!chunk.ok() || !chunk->has_value()) {
      break;
    }
    if ((*chunk)->IsNull()) {
      continue;
    }
    values.push_back((*chunk)->data);
  }
  return values;
}

/// Reads a unary port as JSON, or nullopt when it closed empty.
std::optional<nlohmann::json> ReadOne(const std::shared_ptr<Action>& action,
                                      std::string_view port) {
  const std::vector<std::string> values = ReadAll(action, port);
  if (values.empty()) {
    return std::nullopt;
  }
  return nlohmann::json::parse(values.front(), nullptr, false);
}

std::string Concat(const std::vector<std::string>& pieces) {
  std::string joined;
  for (const std::string& piece : pieces) {
    joined += piece;
  }
  return joined;
}

// --- Schemas -----------------------------------------------------------------

TEST(HttpActionsTest, SchemasValidate) {
  EXPECT_TRUE(MakeHttpRequestSchema().Validate().ok());
  EXPECT_TRUE(WebFetchSchema().Validate().ok());
}

TEST(HttpActionsTest, RegistersBothActions) {
  ActionRegistry registry;
  ASSERT_TRUE(RegisterHttpActions(registry).ok());
  EXPECT_TRUE(registry.IsRegistered(kMakeHttpRequestAction));
  EXPECT_TRUE(registry.IsRegistered(kWebFetchAction));
  EXPECT_TRUE(RegisterHttpActions(registry).ok());
}

TEST(HttpActionsTest, MakeHttpRequestDeclaresAPortPerConcern) {
  const a11::actions::ActionSchema schema = MakeHttpRequestSchema();
  for (const std::string_view name :
       {"status_code", "headers", "fields", "body", "trailers", "redirects",
        "pushes", "connection"}) {
    EXPECT_TRUE(schema.outputs.contains(std::string(name))) << name;
  }
  EXPECT_TRUE(schema.headers.contains("x-a11-deadline"));
}

// --- make_http_request -------------------------------------------------------

TEST(HttpActionsTest, SeparatesStatusHeadersAndBody) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("plain", server.url("/plain"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();

  EXPECT_EQ(ReadOne(action, "status_code"), 200);
  const std::optional<nlohmann::json> headers = ReadOne(action, "headers");
  ASSERT_TRUE(headers.has_value());
  EXPECT_EQ((*headers)["content-type"], "text/plain");
  // A repeated field is joined in the map...
  EXPECT_EQ((*headers)["x-tag"], "one, two");
  EXPECT_EQ(Concat(ReadAll(action, "body")), "a plain body");

  // ...and intact in the field list, which is what that port is for.
  const std::vector<std::string> fields = ReadAll(action, "fields");
  int tags = 0;
  for (const std::string& field : fields) {
    if (nlohmann::json::parse(field).at(0) == "x-tag") {
      ++tags;
    }
  }
  EXPECT_EQ(tags, 2);

  const std::optional<nlohmann::json> connection =
      ReadOne(action, "connection");
  ASSERT_TRUE(connection.has_value());
  EXPECT_EQ((*connection)["http_version"], "2");
  EXPECT_EQ((*connection)["secure"], false);
  EXPECT_EQ((*connection)["url"], server.url("/plain"));
}

TEST(HttpActionsTest, DeliversTrailersOnTheirOwnPort) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("trailed", server.url("/trailed"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();

  EXPECT_EQ(Concat(ReadAll(action, "body")), "first second");
  const std::optional<nlohmann::json> trailers = ReadOne(action, "trailers");
  ASSERT_TRUE(trailers.has_value());
  // The value a checksum travels in: unknowable when the headers were sent.
  EXPECT_EQ((*trailers)["x-digest"], "12");
}

TEST(HttpActionsTest, ReportsAnEmptyTrailerObjectWhenThereAreNone) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("no-trailers", server.url("/plain"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  const std::optional<nlohmann::json> trailers = ReadOne(action, "trailers");
  ASSERT_TRUE(trailers.has_value());
  EXPECT_TRUE(trailers->is_object());
  EXPECT_TRUE(trailers->empty());
}

TEST(HttpActionsTest, ReportsTheRedirectChainItFollowed) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("hops", server.url("/hop-1"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();

  EXPECT_EQ(ReadOne(action, "status_code"), 200);
  EXPECT_EQ(Concat(ReadAll(action, "body")), "a plain body");
  // Where it actually went, which a single `Response` object cannot tell you.
  const std::vector<std::string> hops = ReadAll(action, "redirects");
  ASSERT_EQ(hops.size(), 2u);
  EXPECT_EQ(nlohmann::json::parse(hops[0])["location"], "/hop-2");
  EXPECT_EQ(nlohmann::json::parse(hops[1])["location"], "/plain");
  // And the URL the response actually came from.
  EXPECT_EQ((*ReadOne(action, "connection"))["url"], server.url("/plain"));
}

TEST(HttpActionsTest, GivesUpAfterTooManyRedirects) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action = MakeRequest(
      "loop", server.url("/loop"), nlohmann::json{{"max_redirects", 2}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  EXPECT_EQ(action->Wait(kPatience).Await().status().code(),
            absl::StatusCode::kResourceExhausted);
}

TEST(HttpActionsTest, RewritesASeeOtherToABodylessGet) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("see-other", server.url("/see-other"), {}, "POST",
                  {"payload"});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  // 303 means "go and GET this instead", so the method and body do not survive.
  EXPECT_EQ(Concat(ReadAll(action, "body")), "GET:");
}

TEST(HttpActionsTest, DeliversAnErrorResponseRatherThanFailing) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("missing", server.url("/missing"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  // A 404 is a response, not a failure of the request: the server was reached
  // and answered. Failing here would make the `status` port pointless in exactly
  // the cases a caller cares about, and would throw away the error document --
  // and a caller that does want a failure can get one from `status`, whereas the
  // reverse is not recoverable. The action only fails when there is no response
  // at all.
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  EXPECT_EQ(ReadOne(action, "status_code"), 404);
  EXPECT_EQ(Concat(ReadAll(action, "body")), "no such thing");
}

TEST(HttpActionsTest, FailsWhenThereIsNoResponseAtAll) {
  // Nothing listens on port 1, so there is no status to report.
  const std::shared_ptr<Action> action =
      MakeRequest("refused", "http://127.0.0.1:1/nothing");
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  EXPECT_FALSE(action->Wait(kPatience).Await().ok());
}

TEST(HttpActionsTest, SendsActionHeadersAsRequestHeaders) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("headers", server.url("/plain"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->SetHeader("accept", "text/plain").ok());
  ASSERT_TRUE(action->SetHeader("authorization", "Bearer sekrit").ok());
  // A11's own headers are the action's business, not the peer's.
  ASSERT_TRUE(action->SetHeader("x-a11-deadline", "").ok());
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();

  EXPECT_EQ(GetHttpHeader(server.seen_headers(), "accept"), "text/plain");
  EXPECT_EQ(GetHttpHeader(server.seen_headers(), "authorization"),
            "Bearer sekrit");
  EXPECT_EQ(GetHttpHeader(server.seen_headers(), "x-a11-deadline"),
            std::nullopt);
  // And a default user-agent, since none was given.
  EXPECT_TRUE(GetHttpHeader(server.seen_headers(), "user-agent").has_value());
}

TEST(HttpActionsTest, OptionsHeadersBeatActionHeaders) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  // A computed value beats an inherited one, and this is also the way to send a
  // literal x-a11- header to a peer that wants one.
  const std::shared_ptr<Action> action = MakeRequest(
      "override", server.url("/plain"),
      nlohmann::json{{"headers", {{"accept", "application/json"},
                                  {"x-a11-passthrough", "kept"}}}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->SetHeader("accept", "text/plain").ok());
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  EXPECT_EQ(GetHttpHeader(server.seen_headers(), "accept"), "application/json");
  EXPECT_EQ(GetHttpHeader(server.seen_headers(), "x-a11-passthrough"), "kept");
}

TEST(HttpActionsTest, BuffersARequestBodyWithAContentLength) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("upload", server.url("/echo"), {}, "POST",
                  {"one-", "two-", "three"});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  EXPECT_EQ(Concat(ReadAll(action, "body")), "POST:one-two-three");
  EXPECT_EQ(GetHttpHeader(server.seen_headers(), "content-length"), "13");
}

TEST(HttpActionsTest, StreamsARequestBodyWithoutAContentLength) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  absl::StatusOr<std::shared_ptr<Action>> created = Action::Create(
      MakeHttpRequestSchema(), "streamed", MakeHttpRequestHandler());
  ASSERT_TRUE(created.ok()) << created.status();
  const std::shared_ptr<Action> action = *created;

  const auto put_json = [&action](std::string_view port,
                                  const nlohmann::json& value) {
    data::Chunk chunk;
    chunk.metadata =
        data::ChunkMetadata{.mimetype = std::string(data::kJsonMimetype)};
    chunk.data = value.dump();
    ASSERT_TRUE((*action->GetInput(std::string(port), false))
                    ->PutChunk(std::move(chunk), std::nullopt, true)
                    .Await()
                    .ok());
  };
  put_json("url", server.url("/echo"));
  put_json("method", "POST");
  put_json("options", nlohmann::json{{"request_body", "stream"}});

  const std::shared_ptr<AsyncNode> body = *action->GetInput("request_body", false);
  ASSERT_TRUE(action->Run().ok());
  // Written *after* the request is under way, which is the point: the length is
  // not known when the headers go out.
  for (const std::string_view piece : {"alpha", "beta", "gamma"}) {
    data::Chunk chunk;
    chunk.metadata = data::ChunkMetadata{
        .mimetype = std::string("application/octet-stream")};
    chunk.data = std::string(piece);
    ASSERT_TRUE(body->PutChunk(std::move(chunk), std::nullopt, false)
                    .Await()
                    .ok());
  }
  ASSERT_TRUE(body->Finalize({.wait = true}).Await().ok());

  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  EXPECT_EQ(Concat(ReadAll(action, "body")), "POST:alphabetagamma");
  EXPECT_EQ(GetHttpHeader(server.seen_headers(), "content-length"),
            std::nullopt);
}

TEST(HttpActionsTest, OmitClosesPortsWithoutWritingThem) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action = MakeRequest(
      "omitting", server.url("/plain"),
      nlohmann::json{{"omit", {"fields", "redirects", "pushes", "connection",
                               "trailers"}}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  // What was asked for is there...
  EXPECT_EQ(ReadOne(action, "status_code"), 200);
  EXPECT_EQ(Concat(ReadAll(action, "body")), "a plain body");
  // ...and what was not is closed rather than left hanging, so a reader that
  // drains everything still finishes.
  EXPECT_TRUE(ReadAll(action, "fields").empty());
  EXPECT_TRUE(ReadAll(action, "trailers").empty());
  EXPECT_TRUE(ReadAll(action, "connection").empty());
}

TEST(HttpActionsTest, DeliversAPushedResponseWithItsBodyOnANode) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("pushed", server.url("/pushing"),
                  nlohmann::json{{"accept_pushes", true}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();

  EXPECT_EQ(Concat(ReadAll(action, "body")), "<html>");
  const std::vector<std::string> pushes = ReadAll(action, "pushes");
  ASSERT_EQ(pushes.size(), 1u);
  const nlohmann::json record = nlohmann::json::parse(pushes.front());
  EXPECT_EQ(record["method"], "GET");
  EXPECT_EQ(record["path"], "/style.css");
  EXPECT_EQ(record["status"], 200);
  EXPECT_EQ(record["headers"]["content-type"], "text/css");

  // The pushed body is a node of its own, named in the record. A port cannot
  // interleave several bodies; a node per push is how A11 says this.
  const std::shared_ptr<nodes::NodeMap> map = action->GetNodeMap();
  ASSERT_NE(map, nullptr);
  absl::StatusOr<std::shared_ptr<AsyncNode>> body =
      map->Get(record["body"].get<std::string>());
  ASSERT_TRUE(body.ok()) << body.status();
  std::string pushed_body;
  while (true) {
    absl::StatusOr<std::optional<data::Chunk>> chunk =
        (*body)->NextChunk(kPatience).Await();
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;
    }
    if (!(*chunk)->IsNull()) {
      pushed_body += (*chunk)->data;
    }
  }
  EXPECT_EQ(pushed_body, "body{}");
}

TEST(HttpActionsTest, DeliversNoPushesWhenNotAskedFor) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeRequest("unpushed", server.url("/pushing"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  EXPECT_EQ(Concat(ReadAll(action, "body")), "<html>");
  EXPECT_TRUE(ReadAll(action, "pushes").empty());
}

TEST(HttpActionsTest, RejectsANonHttpUrlAndAPassedDeadline) {
  const std::shared_ptr<Action> bad_scheme =
      MakeRequest("scheme", "ftp://example.test/file");
  ASSERT_NE(bad_scheme, nullptr);
  ASSERT_TRUE(bad_scheme->Run().ok());
  EXPECT_EQ(bad_scheme->Wait(kPatience).Await().status().code(),
            absl::StatusCode::kInvalidArgument);

  const std::shared_ptr<Action> late =
      MakeRequest("late", "http://127.0.0.1:1/nothing");
  ASSERT_NE(late, nullptr);
  ASSERT_TRUE(late->SetHeader("x-a11-deadline", "1").ok());
  ASSERT_TRUE(late->Run().ok());
  EXPECT_EQ(late->Wait(kPatience).Await().status().code(),
            absl::StatusCode::kDeadlineExceeded);
}

TEST(HttpActionsTest, RejectsUnusableOptions) {
  const std::shared_ptr<Action> action =
      MakeRequest("bad-options", "http://127.0.0.1:1/nothing",
                  nlohmann::json{{"request_body", "telepathy"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  EXPECT_EQ(action->Wait(kPatience).Await().status().code(),
            absl::StatusCode::kInvalidArgument);
}

// --- web-fetch ---------------------------------------------------------------

std::shared_ptr<Action> MakeFetch(std::string id, std::string url,
                                  nlohmann::json options = {}) {
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(WebFetchSchema(), std::move(id), WebFetchHandler());
  if (!created.ok()) {
    return nullptr;
  }
  const std::shared_ptr<Action> action = *created;
  if (!PutJson(action, "url", url).ok() ||
      !PutJson(action, "method", nlohmann::json()).ok() ||
      !PutJson(action, "options",
               options.is_null() ? nlohmann::json::object() : options)
           .ok() ||
      !PutBody(action, {}).ok()) {
    return nullptr;
  }
  return action;
}

TEST(HttpActionsTest, WebFetchHandsBackTextAndJson) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeFetch("fetch-json", server.url("/json"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();

  EXPECT_EQ(ReadOne(action, "status_code"), 200);
  EXPECT_EQ(ReadOne(action, "ok"), true);
  EXPECT_EQ(ReadOne(action, "text"), R"({"name": "a11", "count": 2})");
  const std::optional<nlohmann::json> parsed = ReadOne(action, "json");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ((*parsed)["name"], "a11");
  EXPECT_EQ((*parsed)["count"], 2);
}

TEST(HttpActionsTest, WebFetchClosesJsonEmptyForAPageThatIsNotJson) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeFetch("fetch-text", server.url("/plain"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  EXPECT_EQ(ReadOne(action, "text"), "a plain body");
  // "It is not JSON" is an answer, not a failure.
  EXPECT_TRUE(ReadAll(action, "json").empty());
  EXPECT_TRUE(ReadAll(action, "items").empty());
}

TEST(HttpActionsTest, WebFetchReportsAnErrorResponseAsData) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeFetch("fetch-404", server.url("/missing"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  // Unlike make_http_request: the fetch succeeds and says the response did not.
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  EXPECT_EQ(ReadOne(action, "status_code"), 404);
  EXPECT_EQ(ReadOne(action, "ok"), false);
  EXPECT_EQ(ReadOne(action, "text"), "no such thing");
}

TEST(HttpActionsTest, WebFetchStreamsTheElementsOfAJsonArray) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeFetch("fetch-array", server.url("/array"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  const std::vector<std::string> items = ReadAll(action, "items");
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(nlohmann::json::parse(items[0])["n"], 1);
  EXPECT_EQ(nlohmann::json::parse(items[2])["n"], 3);
}

TEST(HttpActionsTest, WebFetchStreamsNdjsonLines) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeFetch("fetch-ndjson", server.url("/ndjson"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  const std::vector<std::string> items = ReadAll(action, "items");
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(nlohmann::json::parse(items[1])["n"], 2);
  // NDJSON is not a JSON document, so there is nothing on `json`.
  EXPECT_TRUE(ReadAll(action, "json").empty());
}

TEST(HttpActionsTest, WebFetchDecodesServerSentEvents) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeFetch("fetch-sse", server.url("/events"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();

  const std::vector<std::string> items = ReadAll(action, "items");
  ASSERT_EQ(items.size(), 3u) << Concat(items);
  const nlohmann::json first = nlohmann::json::parse(items[0]);
  EXPECT_EQ(first["event"], "tick");
  EXPECT_EQ(first["id"], "1");
  // The data is usually JSON, so it is parsed rather than handed back as text
  // for the caller to parse again.
  EXPECT_EQ(first["json"]["n"], 1);
  // An event without an `event:` field is a "message", per the format.
  EXPECT_EQ(nlohmann::json::parse(items[1])["event"], "message");
  // And data that is not JSON is still delivered, as data.
  const nlohmann::json third = nlohmann::json::parse(items[2]);
  EXPECT_EQ(third["data"], "bare text");
  EXPECT_FALSE(third.contains("json"));
}

TEST(HttpActionsTest, WebFetchFollowsRedirects) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeFetch("fetch-hops", server.url("/hop-1"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  EXPECT_EQ(ReadOne(action, "status_code"), 200);
  EXPECT_EQ(ReadOne(action, "text"), "a plain body");
}

TEST(HttpActionsTest, WebFetchStreamsTheBodyForACallerThatWantsBytes) {
  ActionTestServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<Action> action =
      MakeFetch("fetch-bytes", server.url("/plain"),
                nlohmann::json{{"omit", {"text", "json", "items"}}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok())
      << action->Wait(kPatience).Await().status();
  EXPECT_EQ(Concat(ReadAll(action, "body")), "a plain body");
  EXPECT_TRUE(ReadAll(action, "text").empty());
}

}  // namespace
}  // namespace a11::sdk::http

// Copyright 2026 The A11 Authors.

#include "a11/net/internal/http1_codec.h"

#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace a11::net::internal {
namespace {

TEST(Http1CodecTest, FindsHeaderBlockEndWithCrlf) {
  // Index points just past the terminating CRLFCRLF (the start of "BODY").
  EXPECT_EQ(FindHeaderBlockEnd("GET / HTTP/1.1\r\nHost: x\r\n\r\nBODY"), 27u);
  EXPECT_FALSE(FindHeaderBlockEnd("GET / HTTP/1.1\r\nHost: x\r\n").has_value());
}

TEST(Http1CodecTest, ParsesRequestHead) {
  const auto head = ParseRequestHead(
      "post /streams/9/message HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Content-Type: application/json\r\n"
      "X-A11-Http-Authorization: Bearer abc\r\n");
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->method, "POST");  // Upper-cased.
  EXPECT_EQ(head->target, "/streams/9/message");
  EXPECT_EQ(head->version, "HTTP/1.1");
  EXPECT_EQ(GetHttpHeader(head->headers, "host"), "example.com");
  EXPECT_EQ(GetHttpHeader(head->headers, "content-type"), "application/json");
  EXPECT_EQ(GetHttpHeader(head->headers, "x-a11-http-authorization"),
            "Bearer abc");
}

TEST(Http1CodecTest, ParsesResponseHead) {
  const auto head = ParseResponseHead(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\n");
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 200);
  EXPECT_EQ(head->reason, "OK");
  EXPECT_EQ(GetHttpHeader(head->headers, "content-type"), "text/event-stream");
}

TEST(Http1CodecTest, ParsesSwitchingProtocolsResponse) {
  const auto head = ParseResponseHead(
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n");
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(head->status, 101);
  EXPECT_EQ(head->reason, "Switching Protocols");
}

TEST(Http1CodecTest, RejectsMalformedRequestLine) {
  EXPECT_FALSE(ParseRequestHead("GET /\r\n").ok());
  EXPECT_FALSE(ParseRequestHead("GET / HTTP/9.9\r\n").ok());
  EXPECT_FALSE(ParseRequestHead("GET / HTTP/1.1\r\nbadheader\r\n").ok());
}

TEST(Http1CodecTest, PlansBodyFraming) {
  auto plan = PlanRequestBody({{"content-length", "42"}});
  ASSERT_TRUE(plan.ok());
  EXPECT_EQ(plan->framing, BodyFraming::kContentLength);
  EXPECT_EQ(plan->content_length, 42u);

  plan = PlanRequestBody({{"transfer-encoding", "chunked"}});
  ASSERT_TRUE(plan.ok());
  EXPECT_EQ(plan->framing, BodyFraming::kChunked);

  plan = PlanRequestBody({{"host", "x"}});
  ASSERT_TRUE(plan.ok());
  EXPECT_EQ(plan->framing, BodyFraming::kNone);
}

TEST(Http1CodecTest, PlansResponseBodyWithoutBodyStatuses) {
  auto plan = PlanResponseBody("GET", 204, {});
  ASSERT_TRUE(plan.ok());
  EXPECT_EQ(plan->framing, BodyFraming::kNone);

  plan = PlanResponseBody("HEAD", 200, {{"content-length", "10"}});
  ASSERT_TRUE(plan.ok());
  EXPECT_EQ(plan->framing, BodyFraming::kNone);

  plan = PlanResponseBody("GET", 200, {});
  ASSERT_TRUE(plan.ok());
  EXPECT_EQ(plan->framing, BodyFraming::kUntilClose);
}

TEST(Http1CodecTest, EncodesChunks) {
  EXPECT_EQ(EncodeChunk("hello"), "5\r\nhello\r\n");
  EXPECT_EQ(EncodeChunk(""), "");
  EXPECT_EQ(EncodeLastChunk(), "0\r\n\r\n");
  // 16 bytes -> hex "10".
  EXPECT_EQ(EncodeChunk(std::string(16, 'a')).substr(0, 4), "10\r\n");
}

TEST(Http1CodecTest, DecodesChunkedBodyWhole) {
  ChunkedDecoder decoder;
  std::string out;
  bool complete = false;
  ASSERT_TRUE(
      decoder.Feed("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", &out, &complete)
          .ok());
  EXPECT_TRUE(complete);
  EXPECT_EQ(out, "hello world");
}

TEST(Http1CodecTest, DecodesChunkedBodyBytewise) {
  const std::string wire = "3\r\nabc\r\n1\r\nd\r\n0\r\n\r\n";
  ChunkedDecoder decoder;
  std::string out;
  bool complete = false;
  for (char character : wire) {
    ASSERT_TRUE(
        decoder.Feed(std::string_view(&character, 1), &out, &complete).ok());
  }
  EXPECT_TRUE(complete);
  EXPECT_EQ(out, "abcd");
}

TEST(Http1CodecTest, DecodesChunkedWithExtensionsAndTrailers) {
  ChunkedDecoder decoder;
  std::string out;
  bool complete = false;
  ASSERT_TRUE(decoder
                  .Feed("4;name=value\r\ndata\r\n0\r\nTrailer: v\r\n\r\n", &out,
                        &complete)
                  .ok());
  EXPECT_TRUE(complete);
  EXPECT_EQ(out, "data");
  // The trailer section is kept, not skipped: it is the only place a checksum
  // computed while streaming can travel.
  ASSERT_EQ(decoder.trailers().size(), 1u);
  EXPECT_EQ(decoder.trailers().front().first, "trailer");
  EXPECT_EQ(decoder.trailers().front().second, "v");
}

TEST(Http1CodecTest, DecodesTrailerSectionSplitAcrossFeeds) {
  ChunkedDecoder decoder;
  std::string out;
  bool complete = false;
  // A trailer line arriving in pieces is the ordinary case on a slow link.
  ASSERT_TRUE(decoder.Feed("2\r\nhi\r\n0\r\nx-dig", &out, &complete).ok());
  EXPECT_FALSE(complete);
  ASSERT_TRUE(decoder.Feed("est: abc\r\nx-count: 2\r\n\r\n", &out, &complete)
                  .ok());
  EXPECT_TRUE(complete);
  EXPECT_EQ(out, "hi");
  EXPECT_EQ(GetHttpHeader(decoder.trailers(), "x-digest"), "abc");
  EXPECT_EQ(GetHttpHeader(decoder.trailers(), "x-count"), "2");
}

TEST(Http1CodecTest, EncodesALastChunkWithTrailers) {
  EXPECT_EQ(EncodeLastChunk(), "0\r\n\r\n");
  EXPECT_EQ(EncodeLastChunk({{"x-digest", "abc"}}),
            "0\r\nx-digest: abc\r\n\r\n");
}

TEST(Http1CodecTest, RejectsBadChunkSize) {
  ChunkedDecoder decoder;
  std::string out;
  bool complete = false;
  EXPECT_FALSE(decoder.Feed("zz\r\n", &out, &complete).ok());
}

TEST(Http1CodecTest, SerializesRequestAndResponse) {
  EXPECT_EQ(SerializeRequest("GET", "/connect",
                             {{"host", "h"}, {"accept", "text/event-stream"}}),
            "GET /connect HTTP/1.1\r\nhost: h\r\naccept: text/event-stream\r\n"
            "\r\n");
  EXPECT_EQ(SerializeResponse(200, {{"content-type", "text/event-stream"}}),
            "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\n\r\n");
  EXPECT_EQ(SerializeResponse(101, {{"upgrade", "websocket"}}),
            "HTTP/1.1 101 Switching Protocols\r\nupgrade: websocket\r\n\r\n");
}

TEST(Http1CodecTest, ComputesWebSocketAccept) {
  // The canonical example from RFC 6455 section 1.3.
  EXPECT_EQ(ComputeWebSocketAccept("dGhlIHNhbXBsZSBub25jZQ=="),
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(Http1CodecTest, GeneratesDistinctWebSocketKeys) {
  const std::string first = GenerateWebSocketKey();
  const std::string second = GenerateWebSocketKey();
  // 16 bytes base64-encode to 24 characters.
  EXPECT_EQ(first.size(), 24u);
  EXPECT_NE(first, second);
}

}  // namespace
}  // namespace a11::net::internal

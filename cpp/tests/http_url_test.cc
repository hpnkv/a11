// Copyright 2026 The A11 Authors.

#include <string>

#include <gtest/gtest.h>

#include "a11/net/http/url.h"

namespace a11::net {
namespace {

TEST(HttpUrlTest, ParsesSchemeHostPortAndTarget) {
  const auto url = ParseUrl("https://example.com:8443/v1/models?full=1#frag");
  ASSERT_TRUE(url.ok()) << url.status();
  EXPECT_EQ(url->scheme, "https");
  EXPECT_EQ(url->host, "example.com");
  EXPECT_EQ(url->port, 8443);
  EXPECT_EQ(url->path, "/v1/models");
  // The query survives; the fragment never travels to a server.
  EXPECT_EQ(url->query, "full=1");
  EXPECT_TRUE(url->secure());
  EXPECT_EQ(url->target(), "/v1/models?full=1");
  EXPECT_EQ(url->authority(), "example.com:8443");
}

TEST(HttpUrlTest, AppliesTheSchemeDefaultPortAndOmitsItFromTheAuthority) {
  const auto plain = ParseUrl("http://example.com/a");
  ASSERT_TRUE(plain.ok()) << plain.status();
  EXPECT_EQ(plain->port, 80);
  EXPECT_FALSE(plain->secure());
  // A default port is not worth spelling out in a Host header.
  EXPECT_EQ(plain->authority(), "example.com");

  const auto secure = ParseUrl("wss://example.com/socket");
  ASSERT_TRUE(secure.ok()) << secure.status();
  EXPECT_EQ(secure->port, 443);
  EXPECT_TRUE(secure->secure());
}

TEST(HttpUrlTest, GivesAPathlessUrlTheRoot) {
  const auto bare = ParseUrl("http://example.com");
  ASSERT_TRUE(bare.ok()) << bare.status();
  EXPECT_EQ(bare->path, "");
  EXPECT_EQ(bare->target(), "/");

  // A query with no path still has to produce a well-formed target.
  const auto queried = ParseUrl("http://example.com?a=b");
  ASSERT_TRUE(queried.ok()) << queried.status();
  EXPECT_EQ(queried->target(), "/?a=b");
}

TEST(HttpUrlTest, HandlesIpV6LiteralsWithAndWithoutAPort) {
  const auto ported = ParseUrl("http://[::1]:9000/x");
  ASSERT_TRUE(ported.ok()) << ported.status();
  EXPECT_EQ(ported->host, "::1");
  EXPECT_EQ(ported->port, 9000);
  EXPECT_EQ(ported->authority(), "[::1]:9000");

  // The colons inside the brackets must not be read as a port separator.
  const auto bare = ParseUrl("http://[2001:db8::1]/x");
  ASSERT_TRUE(bare.ok()) << bare.status();
  EXPECT_EQ(bare->host, "2001:db8::1");
  EXPECT_EQ(bare->port, 80);
  EXPECT_EQ(bare->authority(), "[2001:db8::1]");
}

TEST(HttpUrlTest, LowercasesTheScheme) {
  const auto url = ParseUrl("HTTPS://Example.COM/Path");
  ASSERT_TRUE(url.ok()) << url.status();
  EXPECT_EQ(url->scheme, "https");
  // The host keeps its case: it is compared case-insensitively elsewhere, and
  // rewriting it would change what a certificate is checked against.
  EXPECT_EQ(url->host, "Example.COM");
  EXPECT_EQ(url->path, "/Path");
}

TEST(HttpUrlTest, RejectsUrlsItCannotDial) {
  EXPECT_FALSE(ParseUrl("example.com/a").ok());        // no scheme
  EXPECT_FALSE(ParseUrl("ftp://example.com/a").ok());  // unsupported scheme
  EXPECT_FALSE(ParseUrl("http:///a").ok());            // no host
  EXPECT_FALSE(ParseUrl("http://example.com:0/a").ok());
  EXPECT_FALSE(ParseUrl("http://example.com:99999/a").ok());
  EXPECT_FALSE(ParseUrl("http://example.com:port/a").ok());
  EXPECT_FALSE(ParseUrl("http://[::1/a").ok());  // unterminated literal
}

TEST(HttpUrlTest, RoundTripsThroughToString) {
  for (const std::string text :
       {"http://example.com/a/b?c=d", "https://example.com:8443/x",
        "http://[::1]:9000/y?z=1"}) {
    const auto url = ParseUrl(text);
    ASSERT_TRUE(url.ok()) << url.status();
    EXPECT_EQ(url->ToString(), text);
  }
}

TEST(HttpUrlTest, ResolvesTheRedirectFormsThatOccurInPractice) {
  const auto base = ParseUrl("https://host.example/a/b/page?q=1");
  ASSERT_TRUE(base.ok()) << base.status();

  const auto absolute = ResolveReference(*base, "http://other.example/z");
  ASSERT_TRUE(absolute.ok()) << absolute.status();
  EXPECT_EQ(absolute->ToString(), "http://other.example/z");

  // Protocol-relative keeps the base scheme.
  const auto protocol_relative = ResolveReference(*base, "//cdn.example/z");
  ASSERT_TRUE(protocol_relative.ok()) << protocol_relative.status();
  EXPECT_EQ(protocol_relative->ToString(), "https://cdn.example/z");

  const auto rooted = ResolveReference(*base, "/root/z");
  ASSERT_TRUE(rooted.ok()) << rooted.status();
  EXPECT_EQ(rooted->ToString(), "https://host.example/root/z");

  // A bare reference resolves against the base's directory, and drops the
  // base's query.
  const auto relative = ResolveReference(*base, "sibling");
  ASSERT_TRUE(relative.ok()) << relative.status();
  EXPECT_EQ(relative->ToString(), "https://host.example/a/b/sibling");

  const auto query_only = ResolveReference(*base, "?q=2");
  ASSERT_TRUE(query_only.ok()) << query_only.status();
  EXPECT_EQ(query_only->ToString(), "https://host.example/a/b/page?q=2");

  EXPECT_FALSE(ResolveReference(*base, "").ok());
}

TEST(HttpUrlTest, ResolvedReferencesCarryTheirOwnQueryAndDropFragments) {
  const auto base = ParseUrl("https://host.example/a/page?keep=me");
  ASSERT_TRUE(base.ok()) << base.status();

  const auto resolved = ResolveReference(*base, "/next?fresh=1#ignored");
  ASSERT_TRUE(resolved.ok()) << resolved.status();
  EXPECT_EQ(resolved->query, "fresh=1");
  EXPECT_EQ(resolved->ToString(), "https://host.example/next?fresh=1");
}

}  // namespace
}  // namespace a11::net

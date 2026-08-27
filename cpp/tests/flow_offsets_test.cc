// Copyright 2026 The A11 Authors.

#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/flow/offsets.h"
#include "a11/flow/service.h"

namespace a11::flow {
namespace {

/// A flow containing a non-ASCII character for offset conversion tests.
///
/// `§` is two bytes of UTF-8 and one UTF-16 unit; the emoji is four bytes and
/// *two* UTF-16 units, so a conversion that merely counted code points would
/// get
/// the second one wrong and pass on the first.
constexpr std::string_view kSource =
    "flow marked {\n"
    "  in  q: string\n"
    "  out o: string\n"
    "  describe \"a § and an 🙂 walk in\"\n"
    "  q -> o\n"
    "}\n";

TEST(FlowOffsets, ConvertsBothWaysAroundOutsideAscii) {
  const TextIndex index{std::string(kSource)};
  const size_t marker = kSource.find("§");
  // Everything before the first wide character agrees, which is why reading one
  // basis as the other survives a test suite of ASCII files.
  EXPECT_EQ(index.Utf16Of(marker), marker);
  // Two bytes, one unit.
  EXPECT_EQ(index.Utf16Of(marker + 2), marker + 1);
  const size_t emoji = kSource.find("🙂");
  // Four bytes, two units: one short of what a code-point count would say.
  EXPECT_EQ(index.Utf16Of(emoji + 4), index.Utf16Of(emoji) + 2);
  // And back again, for every offset in the document.
  for (size_t at = 0; at <= kSource.size(); ++at) {
    if (at < kSource.size() &&
        (static_cast<unsigned char>(kSource[at]) & 0xC0) == 0x80) {
      continue;  // Not a character boundary, so not an offset anything reports.
    }
    EXPECT_EQ(index.ByteOf(index.Utf16Of(at)), at) << "at byte " << at;
  }
}

TEST(FlowOffsets, LinesAndColumnsAreTheProtocolsNotTheLanguages) {
  const TextIndex index{std::string(kSource)};
  const size_t emoji = kSource.find("🙂");
  const auto [line, character] = index.PositionOf(emoji);
  EXPECT_EQ(line, 3);
  EXPECT_EQ(index.ByteOfPosition(line, character), emoji);
  // Past the end of a line stays on that line rather than wandering onto the
  // next one.
  EXPECT_EQ(index.ByteOfPosition(0, 999), kSource.find('\n'));
  EXPECT_EQ(index.ByteOfPosition(-1, 0), 0u);
}

TEST(FlowOffsets, AnEmptyDocumentIsStillIndexable) {
  const TextIndex index("");
  EXPECT_EQ(index.Utf16Of(0), 0u);
  EXPECT_EQ(index.Utf16Of(50), 0u);
  EXPECT_EQ(index.ByteOf(50), 0u);
  EXPECT_EQ(index.PositionOf(0), std::make_pair(0, 0));
}

/// One request to the service, at a basis.
nlohmann::json Ask(std::string_view method, std::string_view offsets) {
  nlohmann::json request{{"method", std::string(method)},
                         {"source", std::string(kSource)}};
  if (!offsets.empty()) {
    request["offsets"] = std::string(offsets);
  }
  return Handle(request);
}

TEST(FlowOffsets, TheServiceAnswersInTheBasisTheClientAskedFor) {
  const nlohmann::json bytes = Ask("tokens", "");
  const nlohmann::json units = Ask("tokens", "utf16");
  ASSERT_TRUE(bytes.at("ok").get<bool>());
  ASSERT_TRUE(units.at("ok").get<bool>());
  const nlohmann::json& left = bytes.at("result").at("tokens");
  const nlohmann::json& right = units.at("result").at("tokens");
  ASSERT_EQ(left.size(), right.size());
  const TextIndex index{std::string(kSource)};
  bool differed = false;
  for (size_t at = 0; at < left.size(); ++at) {
    // The same tokens, said in two arithmetics: the answer is a rebasing and
    // not
    // a different classification.
    EXPECT_EQ(left[at].at("kind"), right[at].at("kind"));
    const auto from = left[at].at("start").get<size_t>();
    EXPECT_EQ(right[at].at("start").get<size_t>(), index.Utf16Of(from));
    if (right[at].at("start") != left[at].at("start")) {
      differed = true;
    }
  }
  // If nothing moved the test is not testing anything.
  EXPECT_TRUE(differed);
}

TEST(FlowOffsets, ACaretConvertsTheOtherWayRound) {
  // The caret an editor reports is in *its* units, so it has to become a byte
  // offset before the language reads the word in front of it. Right after the
  // `q` of `q -> o`, counted the way a JVM buffer counts.
  const TextIndex index{std::string(kSource)};
  const size_t after = kSource.find("q -> o") + 1;
  nlohmann::json request{{"method", "complete"},
                         {"source", std::string(kSource)},
                         {"offsets", "utf16"},
                         {"offset", index.Utf16Of(after)}};
  const nlohmann::json answer = Handle(request);
  ASSERT_TRUE(answer.at("ok").get<bool>()) << answer.dump();
  EXPECT_EQ(answer.at("result").at("prefix"), "q");
}

TEST(FlowOffsets, ABasisTheServiceDoesNotSpeakIsSaidSoRatherThanGuessed) {
  const nlohmann::json answer = Ask("tokens", "utf8");
  ASSERT_FALSE(answer.at("ok").get<bool>());
  EXPECT_NE(answer.at("error").at("message").get<std::string>().find("utf8"),
            std::string::npos);
}

TEST(FlowOffsets, ACaretIntoAProposalIsNotADocumentOffset) {
  // `caret` counts into the text the proposal inserts. Rebasing it against the
  // document would be invalid, so the conversion rule leaves it unchanged.
  nlohmann::json answer{{"caret", 4}, {"start", 4}};
  const TextIndex index{std::string(kSource)};
  const size_t moved = index.Utf16Of(4);
  RebaseToUtf16(answer, index);
  EXPECT_EQ(answer.at("caret").get<size_t>(), 4u);
  EXPECT_EQ(answer.at("start").get<size_t>(), moved);
}

}  // namespace
}  // namespace a11::flow

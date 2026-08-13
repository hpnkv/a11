// Copyright 2026 The A11 Authors.

#include "a11/flow/diagnostic.h"

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <gtest/gtest.h>

namespace a11::flow {
namespace {

TEST(FlowLineIndex, ReportsOneBasedLinesAndColumns) {
  // The convention `a11.flow.lexer` has always reported and FlowSyntaxError has
  // always printed: both start at one.
  const std::string source = "flow t {\n  in q: string\n}\n";
  const LineIndex index(source);

  EXPECT_EQ(index.At(0).line, 1);
  EXPECT_EQ(index.At(0).column, 1);
  EXPECT_EQ(index.At(4).line, 1);
  EXPECT_EQ(index.At(4).column, 5);
  // The newline belongs to the line it ends.
  EXPECT_EQ(index.At(8).line, 1);
  EXPECT_EQ(index.At(9).line, 2);
  EXPECT_EQ(index.At(9).column, 1);
  EXPECT_EQ(index.At(11).column, 3);
  EXPECT_EQ(index.LineCount(), 4u);
}

TEST(FlowLineIndex, ClampsPastTheEnd) {
  const std::string source = "flow t {}";
  const LineIndex index(source);
  const Position past = index.At(source.size() + 100);
  EXPECT_EQ(past.offset, source.size());
  EXPECT_EQ(past.line, 1);
  EXPECT_EQ(past.column, static_cast<int>(source.size()) + 1);
}

TEST(FlowLineIndex, HandlesAnEmptySource) {
  const LineIndex index("");
  EXPECT_EQ(index.LineCount(), 1u);
  EXPECT_EQ(index.At(0).line, 1);
  EXPECT_EQ(index.At(0).column, 1);
  EXPECT_EQ(index.LineStart(1), 0u);
}

TEST(FlowLineIndex, LineStartsAreWhereTheLinesStart) {
  const std::string source = "a\nbb\n\nccc";
  const LineIndex index(source);
  EXPECT_EQ(index.LineStart(1), 0u);
  EXPECT_EQ(index.LineStart(2), 2u);
  EXPECT_EQ(index.LineStart(3), 5u);
  EXPECT_EQ(index.LineStart(4), 6u);
  // Past the last line, the end of the source rather than a wild offset.
  EXPECT_EQ(index.LineStart(99), source.size());
}

TEST(FlowDiagnosticCodes, AreSortedUniqueAndWellFormed) {
  // The table is the published contract, and a listing of it has to be stable:
  // sorted so the output does not depend on the order somebody added rows in,
  // unique so a lookup means one thing.
  absl::flat_hash_set<std::string_view> seen;
  std::string_view previous;
  for (const CodeInfo& info : KnownCodes()) {
    EXPECT_FALSE(info.code.empty());
    EXPECT_TRUE(info.code.starts_with("flow.")) << info.code;
    // `flow.<family-ish>.<what>`: three dotted parts, all lower case.
    EXPECT_EQ(std::count(info.code.begin(), info.code.end(), '.'), 2)
        << info.code;
    for (const char letter : info.code) {
      EXPECT_TRUE(letter == '.' || letter == '-' ||
                  (letter >= 'a' && letter <= 'z'))
          << info.code;
    }
    EXPECT_FALSE(info.summary.empty()) << info.code;
    EXPECT_TRUE(info.summary.ends_with(".")) << info.code;
    if (!previous.empty()) EXPECT_LT(previous, info.code);
    EXPECT_TRUE(seen.insert(info.code).second) << info.code;
    previous = info.code;
  }
  EXPECT_GT(KnownCodes().size(), 30u);
}

TEST(FlowDiagnosticCodes, TheSecondPartNamesTheFamily) {
  // Not enforced by construction, so it is enforced here: a reader who sees
  // `flow.unused.header` should be able to guess which inspection owns it.
  for (const CodeInfo& info : KnownCodes()) {
    const size_t first = info.code.find('.');
    const size_t second = info.code.find('.', first + 1);
    const std::string_view middle =
        info.code.substr(first + 1, second - first - 1);
    EXPECT_EQ(middle, FamilyName(info.family)) << info.code;
  }
}

TEST(FlowDiagnosticCodes, AreFoundByCode) {
  const CodeInfo* found = FindCode("flow.unused.try-status");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->family, Family::kUnused);
  EXPECT_EQ(found->severity, Severity::kWeakWarning);
  EXPECT_EQ(FindCode("flow.nothing.here"), nullptr);
  EXPECT_EQ(FindCode(""), nullptr);
}

TEST(FlowDiagnosticNames, RoundTrip) {
  for (const Severity severity :
       {Severity::kError, Severity::kWarning, Severity::kWeakWarning,
        Severity::kInformation}) {
    EXPECT_EQ(SeverityFromName(SeverityName(severity)), severity);
  }
  for (const Family family :
       {Family::kSyntax, Family::kForm, Family::kName, Family::kSequence,
        Family::kBarrier, Family::kUnused}) {
    EXPECT_EQ(FamilyFromName(FamilyName(family)), family);
  }
  // An unknown name is the safe end of each scale rather than an error: a newer
  // producer must not make an older reader throw.
  EXPECT_EQ(SeverityFromName("whatever"), Severity::kError);
  EXPECT_EQ(FamilyFromName("whatever"), Family::kSyntax);
}

TEST(FlowSortDiagnostics, OrdersByPositionThenCode) {
  std::vector<Diagnostic> diagnostics;
  Diagnostic later;
  later.code = "flow.name.unknown";
  later.range.start.offset = 20;
  Diagnostic earlier;
  earlier.code = "flow.form.unknown-stage";
  earlier.range.start.offset = 5;
  Diagnostic sameSpotLaterCode;
  sameSpotLaterCode.code = "flow.name.taken";
  sameSpotLaterCode.range.start.offset = 5;
  diagnostics = {later, sameSpotLaterCode, earlier};

  SortDiagnostics(diagnostics);

  EXPECT_EQ(diagnostics[0].code, "flow.form.unknown-stage");
  EXPECT_EQ(diagnostics[1].code, "flow.name.taken");
  EXPECT_EQ(diagnostics[2].code, "flow.name.unknown");
}

}  // namespace
}  // namespace a11::flow

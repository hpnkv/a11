// Copyright 2026 The A11 Authors.

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/flow/catalogue.h"
#include "a11/flow/discover.h"
#include "a11/flow/navigate.h"

namespace a11::flow {
namespace {

using discover::Language;

/// The fixtures: one file per language, plus a header for the C++ one.
///
/// <repo>/cpp/tests/flow_discover_test.cc -> <repo>/testdata/flow/discover
std::filesystem::path Fixtures() {
  return std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() / "testdata" /
         "flow" / "discover";
}

const catalogue::ActionInfo* absl_nullable Find(
    const catalogue::Catalogue& found, std::string_view name) {
  return found.Action(name);
}

std::vector<std::string> PortNames(
    const std::vector<catalogue::PortInfo>& ports) {
  std::vector<std::string> names;
  names.reserve(ports.size());
  for (const catalogue::PortInfo& port : ports) {
    names.push_back(port.name);
  }
  return names;
}

const catalogue::PortInfo* absl_nullable Port(
    const std::vector<catalogue::PortInfo>& ports, std::string_view name) {
  for (const catalogue::PortInfo& port : ports) {
    if (port.name == name) {
      return &port;
    }
  }
  return nullptr;
}

/// Every action the fixtures declare, read once for the whole suite.
const catalogue::Catalogue& Scanned() {
  static const catalogue::Catalogue* found = [] {
    const std::vector<std::string> roots = {Fixtures().string()};
    return new catalogue::Catalogue(discover::Discover(roots).found);
  }();
  return *found;
}

TEST(FlowDiscover, ReadsTheExtensionsItKnowsAndNoOthers) {
  EXPECT_EQ(discover::LanguageOf("a/b.py"), Language::kPython);
  EXPECT_EQ(discover::LanguageOf("a/b.cc"), Language::kCpp);
  EXPECT_EQ(discover::LanguageOf("a/b.h"), Language::kCpp);
  EXPECT_EQ(discover::LanguageOf("a/b.ts"), Language::kTypeScript);
  EXPECT_EQ(discover::LanguageOf("a/b.mjs"), Language::kTypeScript);
  // Not a language this reads: a flow, a document, a name with no extension.
  EXPECT_FALSE(discover::LanguageOf("a/b.flow").has_value());
  EXPECT_FALSE(discover::LanguageOf("README").has_value());
  EXPECT_FALSE(discover::LanguageOf("a/b.md").has_value());
}

TEST(FlowDiscover, FindsAPythonSchemaWholeWithItsPortsAndItsOrigin) {
  const catalogue::ActionInfo* simple = Find(Scanned(), "simple");
  ASSERT_NE(simple, nullptr);
  EXPECT_EQ(simple->description, "Return the input unchanged.");
  EXPECT_EQ(PortNames(simple->inputs), std::vector<std::string>{"text"});
  EXPECT_EQ(PortNames(simple->outputs), std::vector<std::string>{"out"});

  const catalogue::PortInfo* text = Port(simple->inputs, "text");
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(text->type, "text/plain");
  EXPECT_TRUE(text->required);
  EXPECT_TRUE(text->unary);

  // The origin makes an action declared in a project file a
  // "go to declaration" target for the first time, and the position has to be
  // the declaration rather than the top of the file.
  ASSERT_TRUE(simple->origin.has_value());
  EXPECT_EQ(std::filesystem::path(simple->origin->file).filename(),
            "schemas.py");
  EXPECT_GT(simple->origin->line, 1);
  EXPECT_GT(simple->origin->column, 0);
}

TEST(FlowDiscover, JoinsProseWrittenAcrossLinesAndGivesBackTheIndentation) {
  // Every real description outgrows its line, and each language has its own way
  // of saying so: adjacent literals in Python and C++, a `+` in TypeScript. All
  // three mean one string, and a reader shown half of one would be misled.
  for (const std::string_view name : {"prose", "ts-prose"}) {
    const catalogue::ActionInfo* action = Find(Scanned(), name);
    ASSERT_NE(action, nullptr) << name;
    EXPECT_NE(action->description.find("written as two literals"),
              std::string::npos)
        << name << ": " << action->description;
    EXPECT_NE(action->description.find("which is one string."),
              std::string::npos)
        << name << ": " << action->description;
  }

  // A `"""..."""` description gives back the indentation the source put in
  // front of it, as the Flow parser does for its own: the text is what a reader
  // is shown, and twelve spaces of Python indentation are not part of it.
  const catalogue::ActionInfo* prose = Find(Scanned(), "prose");
  ASSERT_NE(prose, nullptr);
  const catalogue::PortInfo* question = Port(prose->inputs, "question");
  ASSERT_NE(question, nullptr);
  EXPECT_EQ(question->description,
            "What to find out.\n\nA second paragraph, indented in the source "
            "and not in the text.");
}

TEST(FlowDiscover, ResolvesANameOrAPortKeyBoundToAConstantOfTheSameFile) {
  // Without this the C++ side finds almost nothing, since nearly every action
  // there names itself with a `constexpr std::string_view`, and the Python side
  // offers a port called `NARRATION_PORT` -- a port that does not exist.
  EXPECT_NE(Find(Scanned(), "reads-its-name-from-a-constant"), nullptr);
  EXPECT_NE(Find(Scanned(), "ts-reads-its-name-from-a-constant"), nullptr);

  // A Python dict key is a variable; a TypeScript property is a literal name
  // unless it is written `[NAME]`. Both spellings reach the same port.
  for (const std::string_view name : {"prose", "ts-prose"}) {
    const catalogue::ActionInfo* action = Find(Scanned(), name);
    ASSERT_NE(action, nullptr) << name;
    EXPECT_NE(Port(action->outputs, "narration"), nullptr)
        << name << " lost the port its constant named";
    EXPECT_EQ(Port(action->outputs, "NARRATION_PORT"), nullptr)
        << name << " offers a port named after the constant rather than by it";
  }
}

TEST(FlowDiscover, ReadsACppSchemaAssembledStatementByStatement) {
  // The C++ shape, and the hard one: not a constructor call with literal
  // arguments but a local variable filled in a statement at a time, named by a
  // constant from the header beside the file.
  const catalogue::ActionInfo* action = Find(Scanned(), "cpp-assembled");
  ASSERT_NE(action, nullptr);
  EXPECT_NE(action->description.find("Assembled statement by statement"),
            std::string::npos);
  ASSERT_TRUE(action->origin.has_value());
  EXPECT_EQ(std::filesystem::path(action->origin->file).filename(),
            "schemas.cc");

  // The ports the helper writes positionally: name, type, description, then the
  // two flags in that order.
  const catalogue::PortInfo* body = Port(action->outputs, "body");
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->type, "application/octet-stream");
  EXPECT_EQ(body->description, "Response body chunks, in order.");
  EXPECT_FALSE(body->required);
  EXPECT_FALSE(body->unary) << "the second flag is `unary`";

  const catalogue::PortInfo* url = Port(action->inputs, "url");
  ASSERT_NE(url, nullptr);
  EXPECT_TRUE(url->required) << "the first flag is `required`";

  // A header is a port of its own kind, and is read the same way.
  EXPECT_NE(Port(action->headers, "x-a11-deadline"), nullptr);

  // A schema whose name is a literal written inline is the easy case, and it
  // still has to work.
  EXPECT_NE(Find(Scanned(), "cpp-inline"), nullptr);
}

TEST(FlowDiscover, DegradesRatherThanGuessingWhereItCannotRead) {
  // Three limits, each pinned here rather than left to be discovered.
  const catalogue::ActionInfo* action = Find(Scanned(), "cpp-assembled");
  ASSERT_NE(action, nullptr);

  // A type that is a call rather than a literal: the port is found, its
  // description is found, and its type is left empty.
  const catalogue::PortInfo* headers = Port(action->outputs, "headers");
  ASSERT_NE(headers, nullptr);
  EXPECT_EQ(headers->type, "") << "a type computed by a call cannot be read";
  EXPECT_FALSE(headers->description.empty());

  // A port added by a helper the schema was *passed to*: not found, because
  // following it means following a call across functions.
  EXPECT_EQ(Port(action->inputs, "settings"), nullptr);

  // A name built at run time: the whole entry is dropped, because nothing can
  // look up an action with no name and half an entry is worse than none.
  for (const catalogue::ActionInfo& one : Scanned().actions()) {
    EXPECT_EQ(one.name.find("computed-"), std::string::npos)
        << one.name << " was read from an f-string";
  }
  EXPECT_EQ(Find(Scanned(), "cpp-nameless"), nullptr);
}

TEST(FlowDiscover, ReadsNothingOutOfACommentOrAString) {
  // The cheapest way to a wrong answer: `ActionSchema` written in a comment
  // that shows how to use one, or in a docstring that mentions it. Both are in
  // the fixtures, and neither is a declaration.
  for (const std::string_view name :
       {"in-a-comment", "ts-in-a-comment", "cpp-in-a-comment"}) {
    EXPECT_EQ(Find(Scanned(), name), nullptr) << name << " came from a comment";
  }
}

TEST(FlowDiscover, ReadsOneDocumentWithoutOpeningIt) {
  // What a host with a file open calls on a save: the text is in hand, and the
  // path is recorded rather than read.
  const std::string source =
      "import a11\n"
      "S = a11.ActionSchema(name=\"live\", description=\"Read from a "
      "buffer.\")\n";
  const catalogue::Catalogue found =
      discover::DiscoverInSource(source, "untitled.py", Language::kPython);
  const catalogue::ActionInfo* action = found.Action("live");
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->description, "Read from a buffer.");
  ASSERT_TRUE(action->origin.has_value());
  EXPECT_EQ(action->origin->file, "untitled.py");
  EXPECT_EQ(action->origin->line, 2);
}

TEST(FlowDiscover, SaysWhatItDidNotRead) {
  // A cap that applied itself silently would make a half-read tree look like a
  // project with two actions in it.
  discover::Options options = discover::Options::Default();
  options.max_files = 1;
  const discover::Result stopped =
      discover::Discover({Fixtures().string()}, options);
  EXPECT_TRUE(stopped.reached_file_limit);

  options = discover::Options::Default();
  options.max_file_bytes = 10;
  const discover::Result skipped =
      discover::Discover({Fixtures().string()}, options);
  EXPECT_FALSE(skipped.too_large.empty());
  EXPECT_TRUE(skipped.found.Empty());
}

TEST(FlowDiscover, AScannedActionHoversAsItsDescriptionAndSaysWhereItIs) {
  // The end of the chain, and what all of this was for: a flow names an action
  // this project declares, and the editor can say what it does and where it
  // was written. Before a scan, hovering that word answered "action name".
  const std::string source =
      "flow use {\n"
      "  in text: string required\n"
      "  out out: string\n"
      "  s = run simple(text: text)\n"
      "  s.out -> out\n"
      "}\n";
  const Description about = Describe(source, source.find("simple"), Scanned());
  ASSERT_TRUE(about.found);
  EXPECT_EQ(about.text, "simple");
  EXPECT_NE(about.markdown.find("Return the input unchanged."),
            std::string::npos);
  EXPECT_NE(about.markdown.find("**Inputs**"), std::string::npos);
  EXPECT_NE(about.markdown.find("Declared in"), std::string::npos)
      << about.markdown;

  // `has_definition` stays about *this* document, and the origin is the other
  // answer: a frontend that folded them into one would put the caret at line 12
  // of the wrong file.
  EXPECT_FALSE(about.has_definition);
  ASSERT_TRUE(about.origin.has_value());
  EXPECT_EQ(std::filesystem::path(about.origin->file).filename(), "schemas.py");

  // An action the catalogue does not have still hovers, and has nowhere to go.
  const Description unknown =
      Describe(source, source.find("simple"), catalogue::Catalogue());
  EXPECT_TRUE(unknown.found);
  EXPECT_FALSE(unknown.origin.has_value());
}

TEST(FlowCatalogue, CarriesAnOriginThroughJsonAndAMerge) {
  const catalogue::Catalogue& scanned = Scanned();
  const catalogue::Catalogue round_tripped =
      catalogue::Catalogue::FromJson(scanned.ToJson());
  const catalogue::ActionInfo* before = scanned.Action("simple");
  const catalogue::ActionInfo* after = round_tripped.Action("simple");
  ASSERT_NE(before, nullptr);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(before->origin, after->origin);

  // A merge replaces a whole entry, so an entry from a live registry does not
  // inherit a scanned one's origin: half a description from each side would be
  // a third thing that is true of neither.
  const catalogue::Catalogue live = catalogue::Catalogue::FromJson(
      nlohmann::json::parse(R"({"actions": [{"name": "simple",
                                             "description": "From a registry."}]})"));
  // Held by name, not chained: `Action` points into the catalogue, and a
  // pointer
  // into a temporary would dangle at the semicolon.
  const catalogue::Catalogue merged = scanned.MergedWith(live);
  const catalogue::ActionInfo* entry = merged.Action("simple");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->description, "From a registry.");
  EXPECT_FALSE(entry->origin.has_value());
}

}  // namespace
}  // namespace a11::flow

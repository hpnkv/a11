// Copyright 2026 The A11 Authors.

#include "a11/flow/navigate.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_join.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/flow/catalogue.h"
#include "a11/flow/complete.h"

namespace a11::flow {
namespace {

constexpr std::string_view kSource = R"(struct Source {
  describe "Where an answer came from."

  id:  string required "Stable id."
  url: string required
}

flow research {
  describe "Look something up."

  in  question: string required
  out answer:   string
  out found:    Source stream

  pages = node()
  hits = run web-search(query: question)
  hits.results | map Source{"id": it.id, "url": it.url} -> found
  hits.results | map it.title -> pages
  pages | join ", " -> answer
}
)";

/// A catalogue with one action and one type, as a frontend would send.
catalogue::Catalogue Known() {
  return catalogue::Catalogue::FromJson(nlohmann::json::parse(R"({
    "actions": [{
      "name": "web-search",
      "description": "Search the web. Returns a page of results.",
      "inputs": [{"name": "query", "type": "str", "required": true,
                  "description": "What to look for."}],
      "outputs": [{"name": "results", "type": "dict", "unary": false}]
    }],
    "types": [{
      "tag": "a11.sdk.AudioBuffer",
      "description": "A block of samples.",
      "fields": [{"name": "rate", "type": "integer", "required": true},
                 {"name": "samples", "type": "bytes"}]
    }]
  })"));
}

size_t At(std::string_view needle) { return kSource.find(needle); }

TEST(FlowNavigate, ListsWhatADocumentDeclaresNestedAsItIsWritten) {
  const std::vector<DocumentSymbol> symbols = Symbols(kSource);
  ASSERT_EQ(symbols.size(), 2u);

  EXPECT_EQ(symbols[0].name, "Source");
  EXPECT_EQ(symbols[0].kind, SymbolClass::kDto);
  EXPECT_EQ(symbols[0].detail, "Where an answer came from.");
  std::vector<std::string> fields;
  for (const DocumentSymbol& child : symbols[0].children) {
    EXPECT_EQ(child.kind, SymbolClass::kField);
    fields.push_back(child.name);
  }
  EXPECT_EQ(absl::StrJoin(fields, ","), "id,url");

  EXPECT_EQ(symbols[1].name, "research");
  EXPECT_EQ(symbols[1].kind, SymbolClass::kFlow);
  // The ports first, with the type they declared, then everything else the flow
  // bound -- which is what a reader jumping around a flow wants to see.
  std::vector<std::string> named;
  for (const DocumentSymbol& child : symbols[1].children) {
    named.push_back(absl::StrCat(SymbolClassName(child.kind), ":", child.name));
  }
  EXPECT_EQ(absl::StrJoin(named, " "),
            "port:question port:answer port:found node:pages call:hits");

  // The selection is the name and the range is the whole construct, so
  // "go to symbol" puts the caret on the word and "select symbol" takes the
  // block.
  EXPECT_LT(symbols[1].selection.start.offset, symbols[1].selection.end.offset);
  EXPECT_LE(symbols[1].range.start.offset, symbols[1].selection.start.offset);
}

TEST(FlowNavigate, SaysWhatIsUnderTheCaretAndWhereItCameFrom) {
  const Description port = Describe(kSource, At("question: string"));
  EXPECT_TRUE(port.found);
  EXPECT_EQ(port.kind, SymbolClass::kPort);
  EXPECT_NE(port.summary.find("input-port"), std::string::npos) << port.summary;
  EXPECT_TRUE(port.has_definition);

  // A node used far from where it was made: the definition is where it was.
  const Description node = Describe(kSource, At("pages | join"));
  EXPECT_TRUE(node.has_definition);
  EXPECT_EQ(node.definition.start.offset, At("pages = node()"));

  // Whitespace describes nothing, and saying so is not a failure.
  EXPECT_FALSE(Describe(kSource, kSource.find("\n\n")).found);
}

TEST(FlowNavigate, AShapeHoversAsItsFields) {
  const Description about = Describe(kSource, At("Source{"));
  ASSERT_TRUE(about.found);
  EXPECT_EQ(about.kind, SymbolClass::kDto);
  EXPECT_NE(about.markdown.find("**Fields**"), std::string::npos);
  EXPECT_NE(about.markdown.find("`id`: string *(required)* — Stable id."),
            std::string::npos)
      << about.markdown;
  // Declared here, so there is somewhere to go.
  EXPECT_TRUE(about.has_definition);
}

TEST(FlowNavigate, ANameIsReadInTheFlowItIsWrittenIn) {
  // Two flows may each declare `in q`, and they are two different ports. The
  // answer used to be whichever flow came first in the file, because a symbol
  // was matched by name against every flow the caret was merely *after* -- so a
  // hover in the second flow described the first one's port and offered to
  // navigate into it.
  constexpr std::string_view kTwo = R"(flow one {
  in  q: string required "The first one's."
  out a: string
  q -> a
}

flow two {
  in  q: string required "The second one's."
  out a: string
  q -> a
}
)";
  const size_t second = kTwo.find("flow two");
  const Description here = Describe(kTwo, kTwo.find("q -> a", second));
  ASSERT_TRUE(here.found);
  EXPECT_NE(here.summary.find("of `two`"), std::string::npos) << here.summary;
  EXPECT_GT(here.definition.start.offset, second);
  EXPECT_EQ(here.detail, "The second one's.");
}

TEST(FlowNavigate, ADefinitionIsTheNameAndNotTheWordThatDeclaresIt) {
  // What "go to declaration" jumps to, and where a hover says the thing came
  // from. Landing on `struct` or `flow` is landing one word to the left of what
  // was asked about -- and, in an editor whose PSI is a token stream, on a
  // keyword whose own hover then answers the wrong question entirely.
  const Description shape = Describe(kSource, At("Source{"));
  ASSERT_TRUE(shape.has_definition);
  EXPECT_EQ(shape.definition.start.offset, At("Source {"));

  const Description flow = Describe(kSource, At("research"));
  ASSERT_TRUE(flow.found);
  EXPECT_EQ(flow.kind, SymbolClass::kFlow);
  ASSERT_TRUE(flow.has_definition);
  EXPECT_EQ(flow.definition.start.offset, At("research"));
}

TEST(FlowNavigate, AnActionHoversAsItsDescriptionAndItsPorts) {
  // The thing a flow author most often has to leave the file to find out. With
  // nothing to go on the answer is only what the word is; with a catalogue it
  // is the action.
  const Description bare = Describe(kSource, At("web-search"),
                                    catalogue::Catalogue());
  EXPECT_TRUE(bare.found);
  EXPECT_EQ(bare.markdown.find("**Inputs**"), std::string::npos);

  const Description about = Describe(kSource, At("web-search"), Known());
  ASSERT_TRUE(about.found);
  EXPECT_NE(about.markdown.find("Search the web."), std::string::npos);
  EXPECT_NE(about.markdown.find("**Inputs**"), std::string::npos);
  EXPECT_NE(about.markdown.find("`query`: str *(required)* — What to look for."),
            std::string::npos)
      << about.markdown;
  EXPECT_NE(about.markdown.find("`results`: dict stream"), std::string::npos);
  // An action is not in this document, so there is nowhere in it to go.
  EXPECT_FALSE(about.has_definition);
}

TEST(FlowNavigate, ARegisteredTypeReadsTheWayAShapeDoes) {
  // Both are records with described fields, and the catalogue records them the
  // same way -- so hover, completion and everything else is one code path.
  const std::string source =
      "flow f {\n  in a: a11.sdk.AudioBuffer required\n  out b: string\n"
      "  a.rate | text -> b\n}\n";
  const Description about =
      Describe(source, source.find("a11.sdk.AudioBuffer"), Known());
  ASSERT_TRUE(about.found);
  EXPECT_EQ(about.text, "a11.sdk.AudioBuffer");
  EXPECT_NE(about.markdown.find("A block of samples."), std::string::npos);
  EXPECT_NE(about.markdown.find("`rate`: integer *(required)*"),
            std::string::npos)
      << about.markdown;
  // A caret anywhere in the dotted name describes the whole type, not the one
  // word it happens to be in.
  EXPECT_EQ(Describe(source, source.find("AudioBuffer"), Known()).text,
            "a11.sdk.AudioBuffer");
}

TEST(FlowCatalogue, MergesWhatAFrontendSentOverWhatIsEmbedded) {
  const catalogue::Catalogue mine = Known();
  const catalogue::Catalogue theirs =
      catalogue::Catalogue::FromJson(nlohmann::json::parse(R"({
        "actions": [{"name": "web-search", "description": "Theirs."},
                    {"name": "other", "description": "New."}]
      })"));
  const catalogue::Catalogue merged = mine.MergedWith(theirs);

  // A name given twice takes the later description, whole: half a description
  // from each side would be a third thing that is true of neither.
  ASSERT_NE(merged.Action("web-search"), nullptr);
  EXPECT_EQ(merged.Action("web-search")->description, "Theirs.");
  EXPECT_TRUE(merged.Action("web-search")->inputs.empty());
  // A name only one side has stays.
  EXPECT_NE(merged.Action("other"), nullptr);
  EXPECT_NE(merged.Type("a11.sdk.AudioBuffer"), nullptr);
}

TEST(FlowCatalogue, ReadsWhatItWritesAndToleratesRubbish) {
  const catalogue::Catalogue mine = Known();
  const catalogue::Catalogue again =
      catalogue::Catalogue::FromJson(mine.ToJson());
  ASSERT_NE(again.Action("web-search"), nullptr);
  EXPECT_EQ(again.Action("web-search")->inputs.size(), 1u);
  EXPECT_TRUE(again.Action("web-search")->inputs[0].required);
  EXPECT_FALSE(again.Action("web-search")->outputs[0].unary);
  ASSERT_NE(again.Type("a11.sdk.AudioBuffer"), nullptr);
  EXPECT_EQ(again.Type("a11.sdk.AudioBuffer")->shape.fields.size(), 2u);
  // The shape holds bytes, which is what stops `| json` on a value of it.
  EXPECT_TRUE(again.Type("a11.sdk.AudioBuffer")->shape.binary);

  // This arrives from a frontend that may be older or newer than the tool
  // reading it, so a bad entry is skipped rather than refusing the lot.
  const catalogue::Catalogue rubbish =
      catalogue::Catalogue::FromJson(nlohmann::json::parse(
          R"({"actions": [7, {"description": "no name"}, {"name": "ok"}],
              "types": "not a list"})"));
  EXPECT_EQ(rubbish.actions().size(), 1u);
  EXPECT_TRUE(rubbish.types().empty());
  EXPECT_TRUE(catalogue::Catalogue::FromJson(nlohmann::json(7)).Empty());
}

TEST(FlowCatalogue, TheEmbeddedSnapshotIsThereAndUsable) {
  // What a standalone tool knows with nothing configured. Generated from the
  // live registries by `scripts/generate_flow_catalogue.py`; the check that
  // regenerates it is what keeps it honest.
  const catalogue::Catalogue& builtin = catalogue::Catalogue::Builtin();
  EXPECT_FALSE(builtin.Empty());
  ASSERT_NE(builtin.Action("make_http_request"), nullptr);
  const catalogue::ActionInfo& http = *builtin.Action("make_http_request");
  EXPECT_FALSE(http.description.empty());
  EXPECT_NE(http.Port("url", syntax::PortDirection::kInput), nullptr);
  EXPECT_NE(http.Port("body", syntax::PortDirection::kOutput), nullptr);
  EXPECT_TRUE(http.Port("url", syntax::PortDirection::kInput)->required);
}

TEST(FlowCatalogue, CompletionOffersAnActionsPortsAndItsName) {
  const std::string source =
      "flow f {\n  in u: string required\n  out t: string\n"
      "  page = run web-search()\n}\n";
  std::vector<std::string> names;
  for (const Proposal& proposal :
       CompleteAt(source, source.find("web-search()") + 11, Known()).proposals) {
    names.push_back(proposal.name);
  }
  EXPECT_EQ(absl::StrJoin(names, ","), "query");

  names.clear();
  for (const Proposal& proposal :
       CompleteAt(source, source.find("run ") + 4, Known()).proposals) {
    names.push_back(proposal.name);
  }
  EXPECT_NE(std::find(names.begin(), names.end(), "web-search"), names.end());
}

TEST(FlowNavigate, AStageHoversAsReference) {
  // What the hover used to say about a stage was "stage, takes number", which
  // tells a reader nothing the line in front of them did not.
  const Description about = Describe(kSource, At("| map Source") + 2, Known());
  ASSERT_TRUE(about.found);
  EXPECT_NE(about.summary.find("a pipeline stage"), std::string::npos)
      << about.summary;
  EXPECT_NE(about.markdown.find("**Takes:** an expression"), std::string::npos)
      << about.markdown;
  EXPECT_NE(about.markdown.find("**Example:**"), std::string::npos);
  // Reference, not a gloss: what it does with a shape is the thing worth saying.
  EXPECT_NE(about.markdown.find("map Shape"), std::string::npos)
      << about.markdown;
  // A stage that takes nothing prints no "Takes" line at all.
  const std::string collected = StageMarkdown("collect");
  EXPECT_EQ(collected.find("**Takes:**"), std::string::npos) << collected;
  // Either case, as the language allows, shown as it was written.
  EXPECT_NE(StageMarkdown("TRUNCATE").find("`TRUNCATE`"), std::string::npos);
}

TEST(FlowNavigate, AWordThatIsBothAStageAndAFunctionAnswersForWhereItStands) {
  // `| text` re-writes every value of a stream; `text(x)` re-writes one value.
  // Which one a hover is about is the position, and the highlighter has already
  // decided that -- so this is the one place the two must not be confused.
  constexpr std::string_view kBoth = R"(flow f {
  in  q: string stream
  out a: string
  q | text -> a
  let one = text(q)
}
)";
  const Description staged = Describe(kBoth, kBoth.find("| text") + 2);
  ASSERT_TRUE(staged.found);
  EXPECT_NE(staged.summary.find("a pipeline stage"), std::string::npos)
      << staged.summary;
  EXPECT_NE(staged.summary.find("stream"), std::string::npos) << staged.summary;

  const Description called = Describe(kBoth, kBoth.find("text(q)"));
  ASSERT_TRUE(called.found);
  EXPECT_NE(called.summary.find("a built-in function"), std::string::npos)
      << called.summary;

  // And neither of them writes `--` at a reader.
  for (const std::string& text :
       {staged.summary, staged.markdown, called.summary, called.markdown}) {
    EXPECT_EQ(text.find("--"), std::string::npos) << text;
  }
}

}  // namespace
}  // namespace a11::flow

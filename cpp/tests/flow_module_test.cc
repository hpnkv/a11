// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// What the native runtime is *for*: running flows.

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <absl/strings/str_cat.h>
#include <gtest/gtest.h>

#include "a11/flow/complete.h"
#include "a11/flow/contract.h"
#include "a11/flow/document.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/file_modules.h"
#include "a11/flow/inspect.h"
#include "a11/flow/navigate.h"
#include "a11/flow/service.h"

namespace a11::flow {
namespace {
TEST(FlowModule, DirectImportsBindContractsAndTypes) {
  const ModuleSources sources{{"lib", R"(
struct Report exported { text: string }
flow render exported { out value: string
 "ok" -> value
}
)"}};
  auto linked = LinkModules(R"(
declare flow render { out value: string }
import "lib" as lib
from lib import render, Report as ImportedReport
flow main { out value: string
 (run render()).value -> value
}
)",
                            "root", sources);
  auto resolved = Resolve(linked.source, linked.parsed, true);
  ASSERT_FALSE(resolved.HasErrors())
      << DiagnosticsToJsonValue("root", resolved.diagnostics);
  EXPECT_NE(linked.CatalogueAt(0).Action("render"), nullptr);
  EXPECT_NE(linked.CatalogueAt(0).Type("ImportedReport"), nullptr);
}

TEST(FlowModule, LocalDeclarationsAndStructsHaveLexicalScope) {
  auto linked = LinkModules(R"(
flow first {
 in request: Request
 declare flow external { in request: Request }
 struct Request { name: string }
 run external(request: request)
}
flow second {
 struct Request { count: int }
 in request: Request
 declare flow external { in request: Request }
 run external(request: request)
}
)",
                            "root");
  auto resolved = Resolve(linked.source, linked.parsed, true);
  ASSERT_FALSE(resolved.HasErrors())
      << DiagnosticsToJsonValue("root", resolved.diagnostics);
  ASSERT_EQ(resolved.program.dtos.size(), 2u);
  EXPECT_NE(resolved.program.dtos[0].name, resolved.program.dtos[1].name);
  EXPECT_EQ(linked.CatalogueAt(0).Action("external"), nullptr);
  EXPECT_EQ(linked.CatalogueAt(0).Type("Request"), nullptr);
  EXPECT_NE(
      linked.CatalogueAt(linked.source.find("run external")).Action("external"),
      nullptr);
}

TEST(FlowModule, LocalDeclarationsConstrainDirectImports) {
  const std::string source = R"(
flow main {
 declare flow render { out value: string stream }
 import "lib" as lib
from lib import render
 (run render()).value -> _
}
)";
  auto linked =
      LinkModules(source, "root",
                  {{"lib", "flow render exported { out value: int stream }"}});
  auto resolved = Resolve(linked.source, linked.parsed);
  linked.Locate(resolved.diagnostics);
  bool conflict = false;
  for (const auto& diagnostic : resolved.diagnostics) {
    if (diagnostic.code == "flow.form.declaration-schema-mismatch" &&
        diagnostic.source == "root") {
      conflict = true;
      EXPECT_FALSE(diagnostic.related.empty());
    }
  }
  EXPECT_TRUE(conflict) << DiagnosticsToJsonValue("root", resolved.diagnostics);
}

TEST(FlowModule, RejectsNestedExecutableDefinitionsAndDeclarationBodies) {
  for (const auto* source : {"flow outer { flow inner {} }",
                             "declare flow outer { declare flow inner {} }",
                             "declare flow outer { struct Inner {} }",
                             "flow outer { declare flow inner exported {} }",
                             "flow outer { struct Inner exported {} }"}) {
    EXPECT_TRUE(Parse(source).HasErrors()) << source;
  }
}

TEST(FlowModule, DirectImportSelectionCompletesOnlyPublicSymbols) {
  for (const auto* source :
       {"import \"lib\" as lib\nfrom lib import ",
        "flow main {\n import \"lib\" as lib\nfrom lib import "}) {
    const auto answer = Handle({{"method", "complete"},
                                {"source", source},
                                {"offset", std::string_view(source).size()},
                                {"path", "root"},
                                {"modules",
                                 {{"lib",
                                   "flow work exported {}\nflow hidden "
                                   "{}\nstruct Report exported {}"}}}});
    bool work = false, report = false;
    for (const auto& proposal : answer["result"]["proposals"]) {
      work |= proposal["name"] == "work";
      report |= proposal["name"] == "Report";
      EXPECT_TRUE(proposal["name"] == "work" || proposal["name"] == "Report")
          << answer;
    }
    EXPECT_TRUE(work) << answer;
    EXPECT_TRUE(report) << answer;
  }
}

TEST(FlowModule, MultipleDirectContractsMergeAndBodiesAreAmbiguousAtUse) {
  ModuleSources sources{
      {"one", "declare flow work exported { out value: number }"},
      {"two", "flow work exported { out value: int\n 42 -> value }"}};
  const std::string source =
      "import \"one\" as one\nfrom one import work\nimport \"two\" as "
      "two\nfrom two import work\nflow main { (run "
      "work()).value -> _ }";
  auto linked = LinkModules(source, "root", sources);
  auto resolved = Resolve(linked.source, linked.parsed);
  ASSERT_FALSE(resolved.HasErrors())
      << DiagnosticsToJsonValue("root", resolved.diagnostics);
  const auto catalogue = linked.CatalogueAt(source.find("run work"));
  ASSERT_NE(catalogue.Action("work"), nullptr);
  ASSERT_EQ(catalogue.Action("work")->outputs.size(), 1u);
  EXPECT_EQ(catalogue.Action("work")->outputs.front().type, "int");
  sources["one"] =
      "flow work exported { out value: string\n \"other\" -> value }";
  auto ambiguous = LinkModules(source, "root", sources);
  auto rejected = Resolve(ambiguous.source, ambiguous.parsed);
  EXPECT_TRUE(rejected.HasErrors());
}

TEST(FlowModule, NestedSymbolsAppearInDocumentOutlineAndNavigateLocally) {
  const std::string source = R"(flow main {
  declare flow work { out value: string }
  struct Report { value: string }
  run work()
})";
  const auto symbols = Symbols(source);
  ASSERT_EQ(symbols.size(), 1u);
  bool callable = false, shape = false;
  for (const auto& child : symbols.front().children) {
    callable |= child.name == "work" && child.kind == SymbolClass::kFlow;
    shape |= child.name == "Report" && child.kind == SymbolClass::kDto;
  }
  EXPECT_TRUE(callable);
  EXPECT_TRUE(shape);
  const auto definition = Handle({{"method", "definition"},
                                  {"source", source},
                                  {"path", "root"},
                                  {"offset", source.rfind("work()") + 1}});
  EXPECT_TRUE(definition["result"]["found"].get<bool>()) << definition;
}

TEST(FlowModule, EditsOneDirectSymbolWithoutChangingOtherSelections) {
  ModuleDocument document("root",
                          "import \"lib\" as lib\nfrom lib import work, Report "
                          "as R\nflow main { run work() }\n");
  ASSERT_TRUE(document.RenameImport("work", "render").ok());
  EXPECT_NE(document.source().find("work as render, Report as R"),
            std::string::npos);
  EXPECT_NE(document.source().find("run render()"), std::string::npos);
  ASSERT_TRUE(document.UpsertImport("lib", "render", {}, "other").ok());
  EXPECT_NE(document.source().find("other as render, Report as R"),
            std::string::npos);
  ASSERT_TRUE(document.RemoveImport("R").ok());
  EXPECT_EQ(document.source().find("Report"), std::string::npos);
  EXPECT_FALSE(Parse(document.source()).HasErrors());
}

TEST(FlowModule, GraphRenamePreservesDirectImportAliasesAndContracts) {
  ModuleDocument library(
      "lib", "flow render exported { out text: string\n \"ok\" -> text }");
  ModuleSources sources{{"root",
                         "import \"lib\" as lib\nfrom lib import render\nflow "
                         "main {\n declare flow render { out "
                         "text: string }\n (run render()).text -> _ }"}};
  const auto renamed =
      library.RenameInGraph(library.Symbols().front(), "paint", sources);
  ASSERT_TRUE(renamed.ok()) << renamed.status();
  EXPECT_NE(renamed->sources.at("root").find("paint as render"),
            std::string::npos);
  EXPECT_NE(renamed->sources.at("root").find("run render()"),
            std::string::npos);
  EXPECT_NE(renamed->sources.at("root").find("declare flow render"),
            std::string::npos);
  auto linked =
      LinkModules(renamed->sources.at("root"), "root", renamed->sources);
  EXPECT_FALSE(Resolve(linked.source, linked.parsed).HasErrors());
}

TEST(FlowModule, ResolvesCyclesAndKeepsPrivateDependencies) {
  ModuleSources sources{{"lib.flow", R"(
import "root.flow" as root
flow hidden { out value: int
  42 -> value
}
flow public exported { out value: int
  (run hidden()).value -> value
}
)"}};
  auto linked = LinkModules(
      "import \"lib.flow\" as lib\nflow f { out value: int\n (run "
      "lib::public()).value -> value }",
      "root.flow", sources);
  auto resolved = Resolve(linked.source, linked.parsed, true);
  ASSERT_FALSE(resolved.HasErrors())
      << DiagnosticsToJsonValue("root.flow", resolved.diagnostics);
  EXPECT_EQ(linked.documents.size(), 2u);
  const auto catalogue = linked.CatalogueAt(0);
  EXPECT_NE(catalogue.Action("lib::public"), nullptr);
  EXPECT_EQ(catalogue.Action("lib::hidden"), nullptr);
}

TEST(FlowModule, PrivateMembersAndPrivateExportedTypesAreErrors) {
  const ModuleSources sources{
      {"lib",
       "struct Hidden { value: int }\nflow private { }\nflow public exported { "
       "out value: Hidden }"}};
  auto linked = LinkModules(
      "import \"lib\" as lib\nflow f { run lib::private() }", "root", sources);
  auto resolved = Resolve(linked.source, linked.parsed);
  bool private_member = false, private_type = false;
  for (const auto& diagnostic : resolved.diagnostics) {
    private_member |= diagnostic.code == "flow.name.private-member";
    private_type |= diagnostic.code == "flow.form.private-type-exposed";
  }
  EXPECT_TRUE(private_member);
  EXPECT_TRUE(private_type);
}

TEST(FlowModule, ImportedCallTargetsCompleteAfterNamespaceDots) {
  const std::string source =
      "flow main {\n import \"lib\" as nested\n (run "
      "nested::package::work()).result -> _\n}";
  for (const auto* prefix : {"nested::", "nested::package::"}) {
    const auto offset = source.find(prefix) + std::string_view(prefix).size();
    const auto answer = Handle(
        {{"method", "complete"},
         {"source", source},
         {"path", "root"},
         {"offset", offset},
         {"modules",
          {{"lib",
            "flow hidden {}\nmod package exported { flow work exported { "
            "out result: string\n \"ok\" -> result } }"}}}});
    const auto expected =
        std::string_view(prefix) == "nested::" ? "package::work" : "work";
    bool found = false;
    for (const auto& proposal : answer["result"]["proposals"]) {
      found |= proposal["name"] == expected;
      EXPECT_NE(proposal["name"], "hidden");
    }
    EXPECT_TRUE(found) << answer;
  }
}

TEST(FlowModule, IncompleteCallsOfferOnlyExportedModuleSymbols) {
  const std::string source =
      "declare flow test { out output: string stream }\nflow main {\n"
      " import \"nested\" as nested\n t = run nested::\n}";
  for (const bool exported : {false, true}) {
    const auto answer = Handle(
        {{"method", "complete"},
         {"source", source},
         {"path", "root"},
         {"offset", source.find("nested::\n") + 8},
         {"modules",
          {{"nested", absl::StrCat("flow test", exported ? " exported" : "",
                                   " { out output: integer stream }")}}}});
    bool found = false;
    for (const auto& proposal : answer["result"]["proposals"]) {
      found |= proposal["name"] == "test";
    }
    EXPECT_EQ(found, exported) << answer;
  }
}

TEST(FlowModule, EmptyPrivateNamespaceDoesNotLeakShadowedExports) {
  const std::string source =
      "import \"outer\" as nested\nflow main {\n import \"inner\" as nested\n"
      " t = run nested::\n}";
  const auto answer = Handle(
      {{"method", "complete"},
       {"source", source},
       {"path", "root"},
       {"offset", source.find("nested::\n") + 8},
       {"modules",
        {{"outer", "flow visible exported {}"}, {"inner", "flow hidden {}"}}}});
  EXPECT_TRUE(answer["result"]["proposals"].empty()) << answer;
}

TEST(FlowModule, ImportedStructTypesCompleteInInterfacesAndStructFields) {
  for (const auto* declaration : {"struct Wrapper", "struct Wrapper exported",
                                  "flow main", "declare flow work"}) {
    const bool shape = std::string_view(declaration).starts_with("struct");
    const auto source =
        absl::StrCat("import \"nested\" as nested\n", declaration, " {\n ",
                     shape ? "row" : "in row", ": nested::\n}");
    const auto answer = Handle({{"method", "complete"},
                                {"source", source},
                                {"path", "root"},
                                {"offset", source.find("nested::\n") + 8},
                                {"modules",
                                 {{"nested",
                                   "struct Row exported { value: string }\n"
                                   "struct Hidden { value: string }\n"
                                   "flow action exported {}"}}}});
    const auto& proposals = answer["result"]["proposals"];
    ASSERT_EQ(proposals.size(), 1u) << answer;
    EXPECT_EQ(proposals[0]["name"], "Row") << answer;
    EXPECT_EQ(proposals[0]["kind"], "type") << answer;
  }
}

TEST(FlowModule, PrivateMemberDiagnosticsIdentifyExistingDeclarations) {
  const std::string source =
      "import \"nested\" as nested\nflow main {\n in row: nested::Row\n run "
      "nested::test()\n}";
  const std::string dependency = "struct Row { value: string }\nflow test {}";
  auto linked = LinkModules(source, "root", {{"nested", dependency}});
  auto resolved = Resolve(linked.source, linked.parsed);
  linked.Locate(resolved.diagnostics);
  size_t count = 0;
  for (const auto& diagnostic : resolved.diagnostics) {
    EXPECT_NE(diagnostic.code, "flow.name.unknown");
    if (diagnostic.code != "flow.name.private-member") {
      continue;
    }
    ++count;
    EXPECT_NE(diagnostic.message.find("exists"), std::string::npos);
    EXPECT_NE(diagnostic.message.find("private"), std::string::npos);
    EXPECT_NE(diagnostic.message.find("exported"), std::string::npos);
    ASSERT_EQ(diagnostic.related.size(), 1u);
    EXPECT_EQ(diagnostic.related[0].source, "nested");
    const auto& range = diagnostic.related[0].range;
    const auto name = dependency.substr(range.start.offset,
                                        range.end.offset - range.start.offset);
    EXPECT_TRUE(name == "test" || name == "Row");
    const auto& use = diagnostic.range;
    EXPECT_TRUE(
        source.substr(use.start.offset, use.end.offset - use.start.offset)
            .starts_with("nested::"));
  }
  EXPECT_EQ(count, 2u);
}

TEST(FlowModule, DeclarationBodyDiagnosticExplainsTheRepair) {
  const auto parsed = Parse("declare flow test {\n a = take (1)\n}");
  ASSERT_TRUE(parsed.HasErrors());
  bool found = false;
  for (const auto& diagnostic : parsed.diagnostics) {
    if (diagnostic.code != "flow.form.declaration-body") {
      continue;
    }
    found = true;
    EXPECT_TRUE(
        diagnostic.message.starts_with("A flow declaration must contain only"));
    EXPECT_NE(diagnostic.message.find("Remove this statement"),
              std::string::npos);
    EXPECT_NE(diagnostic.message.find("remove 'declare'"), std::string::npos);
  }
  EXPECT_TRUE(found);
}

TEST(FlowModule, RootInterfacesDoNotRequireExportAttributes) {
  for (const auto* attribute : {"", " exported"}) {
    auto linked = LinkModules(
        absl::StrCat("struct Report exported { title: string }\nflow main",
                     attribute,
                     " { out report: Report\n Report{title: \"ready\"} -> "
                     "report }"),
        "root");
    const auto resolved = Resolve(linked.source, linked.parsed);
    EXPECT_FALSE(resolved.HasErrors())
        << DiagnosticsToJsonValue("root", resolved.diagnostics);
  }
}

TEST(FlowModule, RootVisibilityDoesNotGrantExportsToImportersInACycle) {
  auto linked =
      LinkModules("import \"library\" as lib\nflow root_helper {}", "root",
                  {{"library",
                    "import \"root\" as root\nflow work exported { run "
                    "root::root_helper() }"}});
  const auto resolved = Resolve(linked.source, linked.parsed);
  EXPECT_TRUE(
      std::any_of(resolved.diagnostics.begin(), resolved.diagnostics.end(),
                  [](const auto& diagnostic) {
                    return diagnostic.code == "flow.name.private-member";
                  }));
  auto exposed = LinkModules(
      "import \"library\" as lib\nstruct Hidden { title: string }\n"
      "flow work exported { out value: Hidden }",
      "root", {{"library", "import \"root\" as root\nflow main {}"}});
  const auto exposed_result = Resolve(exposed.source, exposed.parsed);
  EXPECT_TRUE(
      std::any_of(exposed_result.diagnostics.begin(),
                  exposed_result.diagnostics.end(), [](const auto& diagnostic) {
                    return diagnostic.code == "flow.form.private-type-exposed";
                  }));
}

TEST(FlowModule, CompatibleContractsConjoinMembersAndRequiredness) {
  const auto parsed = Parse(R"(
declare flow work { in data: number
  out one: string
}
declare flow work { in data: int required
  out two: bool
}
)");
  const auto resolved = Resolve("", parsed);
  ASSERT_FALSE(resolved.HasErrors());
  const auto* interface = resolved.program.Flow("work");
  ASSERT_NE(interface, nullptr);
  EXPECT_EQ(interface->ports.size(), 3u);
  const auto* data = interface->Port("data", syntax::PortDirection::kInput);
  ASSERT_NE(data, nullptr);
  EXPECT_TRUE(data->required);
  EXPECT_EQ(data->declared, "int");
}

TEST(FlowModule, OpaqueComparisonsStayUnprovenOffline) {
  const std::string source =
      "declare flow work { in value: a11::UnknownOne }\ndeclare flow work { in "
      "value: a11::UnknownTwo }";
  auto linked = LinkModules(source, "root");
  const auto resolved = Resolve(linked.source, linked.parsed);
  EXPECT_FALSE(resolved.HasErrors())
      << DiagnosticsToJsonValue("", resolved.diagnostics);
  bool unproven = false;
  for (const auto& diagnostic : resolved.diagnostics) {
    unproven |= diagnostic.code == "flow.sequence.schema-refinement-unproven";
  }
  EXPECT_TRUE(unproven);
}

TEST(FlowModule, EffectiveSchemaHoverRetainsEveryModuleContributor) {
  const auto linked = LinkModules(
      "import \"one\" as one\nimport \"two\" as two\n"
      "from one import work\nfrom two import work\nflow main {}",
      "root",
      {{"one", "declare flow work exported { in value: number }"},
       {"two", "declare flow work exported { in value: int required }"}});
  const auto catalogue = linked.CatalogueAt(0);
  const auto* action = catalogue.Action("work");
  ASSERT_NE(action, nullptr);
  ASSERT_EQ(action->schema_origins.size(), 2u);
  EXPECT_EQ(action->inputs.front().type, "int");
  const auto markdown = ActionMarkdown(*action);
  EXPECT_NE(markdown.find("one:1"), std::string::npos);
  EXPECT_NE(markdown.find("two:1"), std::string::npos);
  const auto restored = catalogue::Catalogue::FromJson(catalogue.ToJson());
  ASSERT_NE(restored.Action("work"), nullptr);
  EXPECT_EQ(restored.Action("work")->schema_origins, action->schema_origins);
}

TEST(FlowModule, ImplementationCannotWeakenRequirednessOrTypes) {
  for (const auto& definition :
       {"flow work { in value: object }", "flow work { in value: int }"}) {
    const auto source = absl::StrCat(
        "declare flow work { in value: int required }\n", definition);
    EXPECT_TRUE(Resolve(source, Parse(source)).HasErrors());
  }
}

TEST(FlowModule, EqualStructsAreRepeatedSchemas) {
  const std::string source =
      "struct Value { value: integer }\nstruct Value { value: integer }";
  EXPECT_FALSE(Resolve(source, Parse(source)).HasErrors());
  const std::string conflict =
      "struct Value { value: integer }\nstruct Value { value: string }";
  EXPECT_TRUE(Resolve(conflict, Parse(conflict)).HasErrors());
}

TEST(FlowModule, RedundantGroupingFixesPreserveTheRequiredPair) {
  const std::string source =
      "flow f { out value: int\n n = take (((1)))\n n -> value }";
  auto parsed = Parse(source);
  auto resolved = Resolve(source, parsed);
  const auto diagnostics = Inspect(source, parsed, resolved);
  ModuleDocument document("root", source);
  std::vector<Edit> edits;
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == "flow.sequence.redundant-parentheses") {
      for (const auto& fix : diagnostic.fixes) {
        edits.insert(edits.end(), fix.edits.begin(), fix.edits.end());
      }
    }
  }
  EXPECT_FALSE(edits.empty());
  ASSERT_TRUE(document.Apply(0, edits).ok());
  EXPECT_EQ(document.source().find("((1))"), std::string::npos);
  const std::string required =
      "flow helper { out value: int\n 1 -> value }\nflow f { out value: int\n "
      "(run helper()).value -> value }";
  const auto required_parsed = Parse(required);
  const auto required_resolved = Resolve(required, required_parsed);
  for (const auto& diagnostic :
       Inspect(required, required_parsed, required_resolved)) {
    EXPECT_NE(diagnostic.code, "flow.sequence.redundant-parentheses");
  }
}

TEST(FlowDocument, TransactionsPreserveUnrelatedCommentsAndRejectStaleHandles) {
  ModuleDocument document(
      "root",
      "# preserved\nflow first { }\n\n# second stays\nflow second { }\n");
  auto symbols = document.Symbols();
  ASSERT_EQ(symbols.size(), 2u);
  ASSERT_TRUE(document.SetExported(symbols[0], true).ok());
  EXPECT_NE(document.source().find("# second stays\nflow second { }"),
            std::string::npos);
  EXPECT_FALSE(document.Remove(symbols[0]).ok());
  const auto before = document.source();
  EXPECT_FALSE(
      document.Apply(document.revision(), {{0, 5, ""}, {1, 2, ""}}).ok());
  EXPECT_EQ(document.source(), before);
}

TEST(FlowDocument, TypedExpressionsEmitRequiredGrouping) {
  auto argument = Expression::Reference("input");
  ASSERT_TRUE(argument.ok());
  auto called = Expression::Invoke("work", {{"value", *argument}});
  ASSERT_TRUE(called.ok());
  auto selected = called->Field("result");
  ASSERT_TRUE(selected.ok());
  auto destination = Expression::Reference("output");
  ASSERT_TRUE(destination.ok());
  FlowBuilder builder("f");
  ASSERT_TRUE(builder.AddPipe(*selected, *destination).ok());
  auto emitted = builder.Emit();
  ASSERT_TRUE(emitted.ok()) << emitted.status();
  EXPECT_NE(emitted->find(").result"), std::string::npos);
  EXPECT_FALSE(Parse(*emitted).HasErrors());
}

TEST(FlowDocument, DeclarationSynchronizationIsIdempotentAndOwned) {
  FlowBuilder declaration("work", true);
  ModuleDocument document("root");
  ASSERT_TRUE(document.Insert(declaration).ok());
  const auto symbol = document.Symbols().front();
  auto synchronized =
      document.UpsertDeclaration(declaration.interface(), {}, symbol);
  ASSERT_TRUE(synchronized.ok()) << synchronized.status();
  EXPECT_TRUE(synchronized->edits.empty());
  EXPECT_EQ(document.revision(), 1u);
  EXPECT_EQ(document.Symbols().size(), 1u);
}

TEST(FlowDocument, SynchronizationRetainsCommentsAndUnchangedTokens) {
  ModuleDocument document(
      "root",
      "declare flow work {\n  # keep this\n  in value: object # caller "
      "supplied\n  out result: string\n}\n");
  FlowBuilder schema("work", true);
  syntax::TypeExpression integer;
  integer.name = "integer";
  syntax::TypeExpression string;
  string.name = "string";
  ASSERT_TRUE(schema.AddPort("value", integer).ok());
  ASSERT_TRUE(
      schema.AddPort("result", string, syntax::PortDirection::kOutput).ok());
  auto change = document.UpsertDeclaration(schema.interface(), {},
                                           document.Symbols().front());
  ASSERT_TRUE(change.ok()) << change.status();
  EXPECT_NE(document.source().find("# keep this"), std::string::npos);
  EXPECT_NE(document.source().find("# caller supplied"), std::string::npos);
  EXPECT_NE(document.source().find("out result: string"), std::string::npos);
  auto again = document.UpsertDeclaration(schema.interface(), {},
                                          document.Symbols().front());
  ASSERT_TRUE(again.ok()) << again.status();
  EXPECT_TRUE(again->edits.empty());
}

TEST(FlowDocument, AliasRenameRespectsLexicalShadowing) {
  ModuleDocument document("root", R"(import "one" as lib
flow first { run lib::work() }
flow second { import "two" as lib
  run lib::work()
}
)");
  auto change = document.RenameImport("lib", "tools");
  ASSERT_TRUE(change.ok()) << change.status();
  EXPECT_NE(document.source().find("run tools::work()"), std::string::npos);
  EXPECT_NE(document.source().find("run lib::work()"), std::string::npos);
}

TEST(FlowModule, AnalysisUsesTheStrictestContextContract) {
  const auto context =
      catalogue::Catalogue::Of(
          {catalogue::ActionInfo{
              .name = "work",
              .inputs = {{"value", "integer", "", true, true}}}})
          .SchemaProgram();
  const std::string source = "declare flow work { in value: number }";
  const auto resolved = Resolve(source, Parse(source), false, &context);
  ASSERT_FALSE(resolved.HasErrors())
      << DiagnosticsToJsonValue("", resolved.diagnostics);
  const auto* port = resolved.program.Flow("work")->Port(
      "value", syntax::PortDirection::kInput);
  ASSERT_NE(port, nullptr);
  EXPECT_EQ(port->declared, "integer");
  EXPECT_TRUE(port->required);
}

TEST(FlowModule, AnonymousResultsHavePortAndFieldCompletionAndNavigation) {
  const std::string prefix =
      "struct Row { title: string }\ndeclare flow work { out result: Row "
      "}\nflow f { x = ";
  for (const auto& suffix : {"(run work()).", "(run work()).result."}) {
    const auto source = prefix + suffix;
    const auto result = CompleteAt(source, source.size());
    bool found = false;
    for (const auto& proposal : result.proposals) {
      found |= proposal.name ==
               (std::string(suffix).ends_with("result.") ? "title" : "result");
    }
    EXPECT_TRUE(found) << source;
  }
  const auto source = prefix + "(run work()).result.title\n}";
  const auto about = Describe(source, source.find(".title") + 2);
  EXPECT_EQ(about.kind, SymbolClass::kField);
  EXPECT_TRUE(about.has_definition);
}

TEST(FlowModule, ScalarRvalueStreamTipsAreOptionalEdits) {
  const std::string source =
      "declare flow work { out result: string stream }\nflow f { out result: "
      "object\n {\"text\": (run work()).result} -> result\n}";
  auto resolved = Resolve(source, Parse(source));
  ASSERT_FALSE(resolved.HasErrors())
      << DiagnosticsToJsonValue("", resolved.diagnostics);
  bool found = false;
  for (const auto& diagnostic : resolved.diagnostics) {
    if (diagnostic.code != "flow.sequence.scalar-rvalue-read") {
      continue;
    }
    found = true;
    EXPECT_EQ(diagnostic.severity, Severity::kInformation);
    ASSERT_FALSE(diagnostic.fixes.empty());
    EXPECT_FALSE(diagnostic.fixes.front().automatic);
  }
  EXPECT_TRUE(found);
}

TEST(FlowModule, RootSourceAndIdentityBudgetsApplyBeforeParsing) {
  ModuleLimits limits;
  limits.max_source_bytes = 4;
  const auto linked = LinkModules("flow f { }", "root", {}, {}, limits);
  EXPECT_TRUE(linked.parsed.HasErrors());
  EXPECT_TRUE(linked.parsed.flows.empty());
}

TEST(FlowDocument, GraphRenameUpdatesImportersAndRetainsCompiledSources) {
  ModuleDocument library(
      "lib", "flow work exported { out result: int\n 1 -> result }");
  ModuleSources modules{{"root",
                         "import \"lib\" as tools\nflow f { out result: int\n "
                         "(run tools::work()).result -> result }"}};
  const auto before = library.source();
  auto change =
      library.RenameInGraph(library.Symbols().front(), "render", modules);
  ASSERT_TRUE(change.ok()) << change.status();
  EXPECT_EQ(library.source(), before);
  EXPECT_NE(change->sources.at("root").find("tools::render"),
            std::string::npos);
  auto linked =
      LinkModules(change->sources.at("root"), "root", change->sources);
  EXPECT_FALSE(Resolve(linked.source, linked.parsed).HasErrors());
}

TEST(FlowModule, InvalidExpressionCategoriesHaveSpecificDiagnostics) {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"flow f { out result: object\n {\"handle\": run work()} -> result }",
       "flow.form.handle-escape"},
      {"flow f { result = take ((fail unavailable)) }",
       "flow.form.statement-as-value"},
      {"flow f { result = take (wait first of 1, 2) }",
       "flow.form.not-waitable"},
      {"flow f { job = run work() with \"x\": job.out }",
       "flow.barrier.expression-cycle"},
      {"flow f { if true { import \"lib\" as lib } }",
       "flow.form.import-context"},
  };
  for (const auto& [source, expected] : cases) {
    const auto parsed = Parse(source);
    const auto resolved = Resolve(source, parsed);
    bool found = false;
    for (const auto& diagnostic : resolved.diagnostics) {
      found |= diagnostic.code == expected;
    }
    EXPECT_TRUE(found) << expected << " "
                       << DiagnosticsToJsonValue("", resolved.diagnostics);
  }
}

TEST(FlowModule, FlatExpressionAndPrefixChainsHaveDepthLimits) {
  for (const auto& operation : {std::string("not "), std::string("wait ")}) {
    std::string source = "flow f { result = ";
    for (int index = 0; index < 1000; ++index) {
      source += operation;
    }
    source += "x\n}";
    const auto parsed = Parse(source);
    bool found = false;
    for (const auto& diagnostic : parsed.diagnostics) {
      found |= diagnostic.code == "flow.syntax.nesting-too-deep";
    }
    EXPECT_TRUE(found);
  }
  std::string source = "flow f { result = take (1)";
  for (int index = 0; index < 1000; ++index) {
    source += " + 1";
  }
  source += "\n}";
  EXPECT_TRUE(Parse(source).HasErrors());
}

TEST(FlowDocument, VisibilityAttributesDoNotConfuseKeywordNames) {
  ModuleDocument document(
      "root", "flow exported { }\nstruct exported { value: string }\n");
  ASSERT_TRUE(document.SetExported(document.Symbols().front(), true).ok());
  EXPECT_NE(document.source().find("flow exported exported"),
            std::string::npos);
  ASSERT_TRUE(document.SetExported(document.Symbols().front(), false).ok());
  EXPECT_EQ(document.Symbols().front().name, "exported");
}

TEST(FlowModule, QualifiedNameGrowthIsBudgeted) {
  ModuleLimits limits;
  limits.max_linked_name_bytes = 1;
  const auto linked =
      LinkModules("import \"lib\" as lib\nflow f { run lib::work() }", "root",
                  {{"lib", "flow work exported {}"}}, {}, limits);
  EXPECT_TRUE(linked.parsed.HasErrors());
}

TEST(FlowModule, IntegerLiteralsAreExactAndCountsAreChecked) {
  const auto parsed = Parse("flow f { value = 9223372036854775807 }");
  ASSERT_FALSE(parsed.HasErrors());
  const auto* value =
      syntax::As<syntax::Bind>(parsed.flows.front()->body.front().get());
  ASSERT_NE(value, nullptr);
  const auto constant = syntax::ConstantValue(value->value.get());
  ASSERT_TRUE(constant.has_value());
  EXPECT_EQ(constant->integer, 9223372036854775807LL);
  EXPECT_TRUE(
      Parse("flow f { value = take (9223372036854775808) }").HasErrors());
  EXPECT_TRUE(
      Parse("flow f { repeat max 9223372036854775807 { } }").HasErrors());
}

TEST(FlowModule, EqualStructsIgnoreDescriptionsFieldOrderAndBuiltinAliases) {
  const std::string source = R"(struct Row { a: int "first"
 b: string }
struct Row exported { b: string "second"
 a: int }
)";
  const auto resolved = Resolve(source, Parse(source));
  ASSERT_FALSE(resolved.HasErrors())
      << DiagnosticsToJsonValue("", resolved.diagnostics);
  EXPECT_TRUE(resolved.program.Dto("Row")->exported);
}

TEST(FlowModule, HostFileResolverReadsUnsavedSourceWithoutAFile) {
  const auto root = FileModuleIdentity("/tmp/a11-flow-root-not-on-disk.flow");
  const auto library =
      FileModuleIdentity("/tmp/a11-flow-library-not-on-disk.flow");
  auto resolver =
      FileModuleResolver(1024, {{library, "flow work exported {}"}});
  const auto resolved = resolver("a11-flow-library-not-on-disk.flow", root);
  ASSERT_TRUE(resolved.ok()) << resolved.status();
  EXPECT_EQ(resolved->identity, library);
  EXPECT_EQ(resolved->source, "flow work exported {}");
}

TEST(FlowModule, FilesystemImportsRequireExplicitApprovedRoots) {
  auto resolver = FileModuleResolver();
  for (const auto* path : {"/etc/passwd", "/missing/never-existed.flow"}) {
    const auto result = resolver(path, "/root.flow");
    ASSERT_FALSE(result.ok());
    EXPECT_TRUE(absl::IsPermissionDenied(result.status()));
    EXPECT_NE(result.status().message().find("imports are disallowed"),
              std::string_view::npos);
  }
  auto confined = FileModuleResolver(1024, {}, {"/approved"});
  const auto existing = confined("/etc/passwd", "/approved/root.flow");
  const auto missing = confined("/never-existed.flow", "/approved/root.flow");
  EXPECT_EQ(existing.status(), missing.status());
  EXPECT_EQ(confined("../etc/passwd", "/approved/root.flow").status(),
            existing.status());
  EXPECT_EQ(confined("/approved-other/file", "/approved/root.flow").status(),
            existing.status());
}

TEST(FlowModule, ApprovedImportsRejectSymlinksAndEnforceFileSize) {
  const auto base =
      std::filesystem::canonical(std::filesystem::temp_directory_path()) /
      absl::StrCat("a11-flow-roots-", ::getpid());
  std::filesystem::create_directories(base / "approved");

  struct Cleanup {
    std::filesystem::path path;

    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{base};

  const auto approved = base / "approved";
  std::ofstream(approved / "library.flow") << "flow work exported {}";
  std::ofstream(base / "secret.flow") << "secret";
  std::filesystem::create_symlink(base / "secret.flow", approved / "link.flow");
  std::filesystem::create_directory_symlink(base, approved / "escape");
  auto resolver = FileModuleResolver(1024, {}, {approved.string()});
  const auto source =
      resolver("library.flow", (approved / "root.flow").string());
  ASSERT_TRUE(source.ok()) << source.status();
  EXPECT_EQ(source->source, "flow work exported {}");
  EXPECT_FALSE(resolver("link.flow", (approved / "root.flow").string()).ok());
  EXPECT_FALSE(
      resolver("escape/secret.flow", (approved / "root.flow").string()).ok());
  EXPECT_TRUE(absl::IsResourceExhausted(
      FileModuleResolver(1, {}, {approved.string()})(
          "library.flow", (approved / "root.flow").string())
          .status()));
}

TEST(FlowModule, CompilationBudgetsBoundParsingAndReexports) {
  CompilationLimits limits;
  limits.max_tokens = 4;
  EXPECT_TRUE(Parse("flow sample { value = 1 }", limits).HasErrors());
  limits = {};
  limits.max_nesting = 2;
  EXPECT_TRUE(Parse("mod a { mod b { mod c { flow sample {} } } }", limits)
                  .HasErrors());
  limits = {};
  limits.max_duration_ms = 0;
  auto stopped = LinkModules("flow sample {}", "root", {}, {}, limits);
  ASSERT_TRUE(stopped.parsed.HasErrors());
  EXPECT_EQ(stopped.parsed.FirstError()->code, "flow.form.resource-limit");
  limits = {};
  limits.max_work = 10;
  stopped = LinkModules("import \"a\"\nfrom a import * exported", "root",
                        {{"a", "import \"root\"\nfrom root import * exported"}},
                        {}, limits);
  EXPECT_TRUE(stopped.parsed.HasErrors());
}

TEST(FlowModule, LongReexportChainsConvergeWithoutAnArbitraryPassLimit) {
  std::string source = "flow original {}\n";
  for (int index = 50; index > 0; --index) {
    source += absl::StrCat("export alias", index - 1, " as alias", index, "\n");
  }
  source += "export original as alias0\n";
  const auto linked = LinkModules(source, "root");
  ASSERT_FALSE(linked.parsed.HasErrors())
      << DiagnosticsToJsonValue("root", linked.parsed.diagnostics);
  EXPECT_NE(linked.CatalogueAt(0).Action("alias50"), nullptr);
}
}  // namespace

TEST(FlowModule, PublicInterfacesUseTheirOwnModuleVisibility) {
  auto good = LinkModules(R"(
mod internal {
  struct Result exported { value: string }
  declare flow work exported { out result: Result }
}
flow main {}
)",
                          "root");
  EXPECT_FALSE(Resolve(good.source, good.parsed).HasErrors());
  auto bad = LinkModules(R"(
mod internal {
  struct Result exported { value: string }
  declare flow work exported { out result: Result }
}
export internal::work
flow main {}
)",
                         "root");
  EXPECT_TRUE(Resolve(bad.source, bad.parsed).HasErrors());
}

TEST(FlowDocument, ModuleScopedImportRenamingKeepsOtherScopesIntact) {
  ModuleDocument document("root", R"(
mod first {
  import "one.flow" as lib
  flow work { run lib::action() }
}
mod second {
  import "two.flow" as lib
  flow work { run lib::action() }
}
)");
  const auto symbols = document.Symbols();
  const auto scope =
      std::find_if(symbols.begin(), symbols.end(), [](const auto& symbol) {
        return symbol.kind == "module" && symbol.name == "first";
      });
  ASSERT_NE(scope, symbols.end());
  ASSERT_TRUE(document.RenameImport("lib", "tools", *scope).ok());
  EXPECT_NE(document.source().find("tools::action"), std::string::npos);
  EXPECT_NE(document.source().find("lib::action"), std::string::npos);
}

TEST(FlowDocument, PipelineInvocationBuilderUsesOrdinaryCallOptions) {
  auto input = Expression::Reference("rows");
  ASSERT_TRUE(input.ok());
  CallOptions options;
  options.remote = true;
  options.timeout = absl::Seconds(2);
  auto result = input->PipelineInvoke("tools::clean", {}, options);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_NE(result->source().find("| call tools::clean() timeout 2s"),
            std::string::npos);
}

TEST(FlowModule, NestedPipelineInvocationsRespectTheParsingDepthBudget) {
  std::string source = "flow main { 1";
  for (int index = 0; index < 1000; ++index) {
    source += " | run f(config: 1";
  }
  source += std::string(1000, ')') + " -> _\n}";
  CompilationLimits limits;
  limits.max_nesting = 8;
  const auto parsed = Parse(source, limits);
  ASSERT_TRUE(parsed.HasErrors());
  EXPECT_TRUE(std::any_of(
      parsed.diagnostics.begin(), parsed.diagnostics.end(),
      [](const auto& d) { return d.code == "flow.syntax.nesting-too-deep"; }));
}

TEST(FlowDocument, ScopeBarrierBuilderAndSubjectNamedAllRemainDistinct) {
  auto barrier = Statement::WaitAll();
  ASSERT_TRUE(barrier.ok());
  EXPECT_EQ(barrier->source(), "wait all");
  auto named = Expression::Reference("all");
  ASSERT_TRUE(named.ok());
  auto wait = Statement::Wait(*named);
  ASSERT_TRUE(wait.ok());
  EXPECT_EQ(wait->source(), "wait (all)");
}

}  // namespace a11::flow

// Copyright 2026 The A11 Authors.

// `a11-flow`: the Flow language as a small standalone tool.
//
// The point of it is what it does *not* link. `a11::flow_lang` depends on the
// standard library, Abseil and nlohmann and nothing else -- no OpenSSL, no libuv,
// no nghttp2, no PortAudio -- so this binary is a few megabytes, builds on
// anything with a C++20 compiler, and can be bundled in an editor extension for a
// platform the full A11 runtime is not built for.
//
// Everything it prints is the same envelope `a11 flow` prints through the Python
// bindings, because both are thin frontends over the same library: an editor may
// speak to whichever is available and read the same answers.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <nlohmann/json.hpp>

#include "a11/flow/complete.h"
#include "a11/flow/diagnostic.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/format.h"
#include "a11/flow/generate.h"
#include "a11/flow/highlight.h"
#include "a11/flow/lexer.h"
#include "a11/flow/parser.h"
#include "a11/flow/plan.h"
#include "a11/flow/resolve.h"
#include "a11/flow/schema.h"
#include "a11/flow/tool/lsp.h"
#include "a11/flow/service.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow::tool {
namespace {

/// Standard input, spelled the way every other tool spells it.
constexpr std::string_view kStdin = "-";

/// What `--format` accepts.
enum class Output { kText, kJson, kSarif };

/// One invocation's options, as read off the command line.
struct Options {
  std::string command;
  std::vector<std::string> files;
  Output format = Output::kText;
  bool quiet = false;
  bool check = false;
  bool in_place = false;
  bool generate = false;
  long long offset = -1;
  int line = 0;
  int column = 0;
  std::string target = "sublime";
  std::string root = ".";
  std::string protocol = "json";
  /// Which shape `schema` is asked about; every one of them when empty.
  std::string dto;
  /// What could not be read on the command line itself.
  std::string complaint;
};

void PrintUsage() {
  std::cout << R"(a11-flow -- read, check and format Flow programs.

Usage: a11-flow <command> [options] [file...]

A file of `-` is standard input, so a flow can be piped in from anywhere.

Commands:
  check FILE...        every problem in each file; exits 1 if any is an error
  fmt FILE...          format: -i rewrites, --check reports what would change
  highlight FILE       what each token means, for a syntax highlighter
  parse FILE           the syntax tree, and everything wrong with the file
  describe FILE        the resolved plan: shapes, ports, headers, node maps, steps
  schema FILE          the JSON Schema a shape describes: --struct Name for one
  complete FILE        what may be written at --offset N or --line L --column C
  codes                every diagnostic code the language publishes
  vocabulary           every word the language gives meaning to
  syntax               an editor definition, generated: --check or --generate
  serve                answer requests on stdin: --protocol json|lsp
  version              what this is

Options:
  --format text|json|sarif   for people, for toolchains, for code scanning
  --quiet                    say nothing about the files that are fine
  --check                    report rather than write; exits 1 if it would
  -i, --in-place             rewrite each file
  --offset N                 a byte offset, for `complete`
  --line L --column C        the same position, 1-based, for `complete`
  --target NAME              which editor definition: )"
            << absl::StrJoin(
                   [] {
                     std::vector<std::string_view> names;
                     for (const SyntaxTarget target : SyntaxTargets()) {
                       names.push_back(SyntaxTargetName(target));
                     }
                     return names;
                   }(),
                   ", ")
            << R"(
  --root DIR                 the checkout an editor definition lives in
  --protocol json|lsp        what `serve` speaks

`serve --protocol lsp` speaks the Language Server Protocol over stdio, which is
what an editor with a language-server client wants. `--protocol json` answers one
request per line with one answer per line, for a host where LSP is awkward:

  {"id":1,"method":"check","source":"flow t { }"}

Methods:
)";
  for (const std::string_view method : Methods()) {
    const size_t padding = method.size() < 12 ? 12 - method.size() : 1;
    std::cout << "  " << method << std::string(padding, ' ')
              << MethodSummary(method) << "\n";
  }
}

/// The text of a file, or `nullopt` with the reason on `stderr`.
bool ReadFile(std::string_view path, std::string& text, std::string& reason) {
  if (path == kStdin) {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    text = buffer.str();
    return true;
  }
  std::ifstream file(std::string(path), std::ios::binary);
  if (!file) {
    reason = std::strerror(errno);
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  text = buffer.str();
  return true;
}

bool WriteFile(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << text;
  return file.good();
}

/// The name a diagnostic is reported against: nothing, for standard input.
std::string Reported(std::string_view path) {
  return path == kStdin ? std::string() : std::string(path);
}

void Emit(const nlohmann::json& payload) {
  std::cout << payload.dump(2) << "\n";
}

/// Everything wrong with one document, both passes, in source order.
std::vector<Diagnostic> Problems(std::string_view source) {
  const nlohmann::json answer = Handle(
      nlohmann::json{{"method", "check"}, {"source", std::string(source)}});
  std::vector<Diagnostic> found;
  for (const nlohmann::json& entry : answer.at("result").at("diagnostics")) {
    found.push_back(DiagnosticFromJsonValue(entry));
  }
  return found;
}

size_t CountErrors(const std::vector<Diagnostic>& found) {
  size_t errors = 0;
  for (const Diagnostic& diagnostic : found) {
    if (diagnostic.severity == Severity::kError) ++errors;
  }
  return errors;
}

// --- The commands ------------------------------------------------------------

int Check(const Options& options) {
  int exit_code = 0;
  for (const std::string& path : options.files) {
    std::string source;
    std::string reason;
    if (!ReadFile(path, source, reason)) {
      std::cerr << path << ": cannot read: " << reason << "\n";
      exit_code = 2;
      continue;
    }
    const std::vector<Diagnostic> found = Problems(source);
    if (CountErrors(found) > 0) exit_code = std::max(exit_code, 1);
    switch (options.format) {
      case Output::kJson:
        Emit(DiagnosticsToJsonValue(path, found));
        break;
      case Output::kSarif:
        Emit(DiagnosticsToSarifValue(path, found));
        break;
      case Output::kText:
        for (const Diagnostic& diagnostic : found) {
          std::cout << DiagnosticToText(Reported(path), diagnostic) << "\n";
        }
        if (found.empty() && !options.quiet) {
          std::cout << path << ": no problems found\n";
        }
        break;
    }
  }
  return exit_code;
}

int Fmt(const Options& options) {
  int exit_code = 0;
  for (const std::string& path : options.files) {
    std::string source;
    std::string reason;
    if (!ReadFile(path, source, reason)) {
      std::cerr << path << ": cannot read: " << reason << "\n";
      exit_code = 2;
      continue;
    }
    const FormatResult result = Format(source);
    bool refused = false;
    for (const Diagnostic& diagnostic : result.diagnostics) {
      if (diagnostic.severity != Severity::kError) continue;
      // A file that will not parse is left exactly as it is, and the problem is
      // what gets reported: half-formatting somebody's file is how a formatter
      // loses their work.
      std::cerr << DiagnosticToText(Reported(path), diagnostic) << "\n";
      refused = true;
    }
    if (refused) {
      exit_code = 2;
      continue;
    }
    if (options.format != Output::kText) {
      nlohmann::json payload = FormatToJsonValue(result);
      payload["source"] = path;
      Emit(payload);
      if (result.changed) exit_code = std::max(exit_code, 1);
      continue;
    }
    if (options.check) {
      if (result.changed) {
        std::cout << path << ": would be reformatted\n";
        exit_code = std::max(exit_code, 1);
      } else if (!options.quiet) {
        std::cout << path << ": already formatted\n";
      }
      continue;
    }
    if (options.in_place) {
      if (path == kStdin) {
        std::cerr << "-i cannot rewrite standard input\n";
        return 2;
      }
      if (result.changed) {
        if (!WriteFile(path, result.formatted)) {
          std::cerr << path << ": cannot write\n";
          return 2;
        }
        std::cout << path << ": reformatted\n";
        exit_code = std::max(exit_code, 1);
      } else if (!options.quiet) {
        std::cout << path << ": already formatted\n";
      }
      continue;
    }
    std::cout << result.formatted;
  }
  return exit_code;
}

int Highlight(const Options& options) {
  std::string source;
  std::string reason;
  const std::string& path = options.files.front();
  if (!ReadFile(path, source, reason)) {
    std::cerr << path << ": cannot read: " << reason << "\n";
    return 2;
  }
  if (options.format != Output::kText) {
    Emit(TokensToJsonValue(path, source));
    return 0;
  }
  const LexResult lexed = Lex(source, LexOptions{.keep_comments = true});
  std::vector<SemanticToken> semantic = flow::Highlight(lexed.tokens);
  // The same two passes the JSON path takes: what a token means lexically, and
  // then the one thing only the resolver can say.
  RefinePorts(source, semantic);
  for (const SemanticToken& token : semantic) {
    const std::string_view text(source.data() + token.start,
                                token.end - token.start);
    if (text == "\n") continue;
    const std::string kind(SemanticKindName(token.kind));
    std::printf("%4d:%-4d %-20s %.*s\n", token.line, token.column, kind.c_str(),
                static_cast<int>(text.size()), text.data());
  }
  return 0;
}

/// One syntax node and everything under it, indented: how the parser read it.
void Outline(const nlohmann::json& node, int depth) {
  if (!node.is_object()) return;
  const nlohmann::json at = node.value("at", nlohmann::json::object());
  std::string label = node.value("kind", std::string("?"));
  for (const std::string_view key :
       {"name", "action", "mode", "op", "variable", "alias", "direction"}) {
    const auto found = node.find(key);
    if (found != node.end() && found->is_string() &&
        !found->get_ref<const std::string&>().empty()) {
      absl::StrAppend(&label, " ", found->get_ref<const std::string&>());
    }
  }
  std::printf("  %4d:%-4d %s%s\n", at.value("line", 1), at.value("column", 1),
              std::string(static_cast<size_t>(depth) * 2, ' ').c_str(),
              label.c_str());
  // In reading order rather than the envelope's, which is sorted by key and so
  // puts a flow's body in front of its ports.
  static constexpr std::array kOrder = {
      std::string_view("ports"),    std::string_view("headers"),
      std::string_view("type"),     std::string_view("condition"),
      std::string_view("source"),   std::string_view("pipeline"),
      std::string_view("subject"),  std::string_view("target"),
      std::string_view("value"),    std::string_view("start"),
      std::string_view("stages"),   std::string_view("args"),
      std::string_view("modifiers"), std::string_view("targets"),
      std::string_view("then_body"), std::string_view("else_body"),
      std::string_view("body"),
  };
  std::vector<std::string> keys;
  for (const auto& [key, value] : node.items()) {
    if (key != "kind" && key != "at") keys.push_back(key);
  }
  const auto rank = [](const std::string& key) {
    for (size_t index = 0; index < kOrder.size(); ++index) {
      if (kOrder[index] == key) return index;
    }
    return kOrder.size();
  };
  std::stable_sort(keys.begin(), keys.end(),
                   [&](const std::string& left, const std::string& right) {
                     const size_t first = rank(left);
                     const size_t second = rank(right);
                     return first != second ? first < second : left < right;
                   });
  for (const std::string& key : keys) {
    const nlohmann::json& value = node.at(key);
    if (value.is_object() && value.contains("kind")) {
      Outline(value, depth + 1);
    } else if (value.is_array()) {
      for (const nlohmann::json& item : value) {
        if (item.is_object() && item.contains("kind")) {
          Outline(item, depth + 1);
        } else if (item.is_array() && item.size() == 2) {
          // A named child: an object's key, a call's port.
          Outline(item[1], depth + 1);
        }
      }
    }
  }
}

int Parse(const Options& options) {
  std::string source;
  std::string reason;
  const std::string& path = options.files.front();
  if (!ReadFile(path, source, reason)) {
    std::cerr << path << ": cannot read: " << reason << "\n";
    return 2;
  }
  const ParseResult parsed = flow::Parse(source);
  if (options.format != Output::kText) {
    Emit(SyntaxToJsonValue(path, parsed));
  } else {
    const nlohmann::json payload = SyntaxToJsonValue(path, parsed);
    for (const nlohmann::json& flow : payload.at("flows")) Outline(flow, 0);
    for (const Diagnostic& diagnostic : parsed.diagnostics) {
      std::cerr << DiagnosticToText(Reported(path), diagnostic) << "\n";
    }
  }
  // The tree is printed either way: a file with a mistake in it still has one,
  // which is the whole point of a recovering parser.
  return parsed.HasErrors() ? 1 : 0;
}

int Describe(const Options& options) {
  std::string source;
  std::string reason;
  const std::string& path = options.files.front();
  if (!ReadFile(path, source, reason)) {
    std::cerr << path << ": cannot read: " << reason << "\n";
    return 2;
  }
  const ParseResult parsed = flow::Parse(source);
  const ResolveResult resolved = Resolve(source, parsed);
  if (options.format != Output::kText) {
    Emit(PlanToJsonValue(path, resolved.program));
    return resolved.HasErrors() ? 1 : 0;
  }
  if (resolved.HasErrors()) {
    for (const Diagnostic& diagnostic : resolved.diagnostics) {
      if (diagnostic.severity == Severity::kError) {
        std::cerr << DiagnosticToText(Reported(path), diagnostic) << "\n";
      }
    }
    return 1;
  }
  for (const DtoPlan& dto : resolved.program.dtos) {
    std::cout << "struct " << dto.name << (dto.binary ? "  (holds bytes)" : "")
              << "\n";
    if (!dto.description.empty()) std::cout << "  " << dto.description << "\n";
    for (const FieldPlan& field : dto.fields) {
      std::printf("  field  %s: %s%s\n", field.name.c_str(),
                  field.declared.empty() ? field.type.c_str()
                                         : field.declared.c_str(),
                  field.required ? " (required)" : "");
    }
  }
  for (const FlowPlan& flow : resolved.program.flows) {
    std::cout << "flow " << flow.name << "\n";
    if (!flow.description.empty()) std::cout << "  " << flow.description << "\n";
    for (const syntax::PortDirection direction :
         {syntax::PortDirection::kInput, syntax::PortDirection::kOutput}) {
      const std::string word =
          direction == syntax::PortDirection::kInput ? "input" : "output";
      for (const PortPlan& port : flow.ports) {
        if (port.direction != direction) continue;
        std::printf("  %-6s %s: %s (%s%s)\n", word.c_str(), port.name.c_str(),
                    port.declared.empty() ? port.type.c_str()
                                          : port.declared.c_str(),
                    port.unary ? "one value" : "stream",
                    port.required ? ", required" : "");
      }
    }
    for (const HeaderPlan& header : flow.headers) {
      std::cout << "  header " << header.name << "\n";
    }
    for (const std::string& map : flow.node_maps) {
      std::cout << "  nodes  " << map << "\n";
    }
    const nlohmann::json plan = PlanToJsonValue(path, resolved.program);
    for (const nlohmann::json& described : plan.at("flows")) {
      if (described.value("flow", std::string()) != flow.name) continue;
      for (const nlohmann::json& step : described.at("steps")) {
        const std::string kind = step.value("step", std::string("?"));
        std::printf("  %-6s %s\n", kind.c_str(),
                    step.value("label", std::string("?")).c_str());
      }
    }
  }
  return 0;
}

/// `a11-flow schema FILE [--struct Name]`: the JSON Schema a shape describes.
///
/// The half of the shape/schema translation somebody runs by hand: a `struct` is
/// handed to whatever speaks schemas -- a model's structured-output mode, an
/// OpenAPI document, a validator elsewhere -- and this is how it gets out. The
/// other direction is `serve`'s `shapes` method, which takes a schema and gives
/// back Flow source.
int Schema(const Options& options) {
  std::string source;
  std::string reason;
  const std::string& path = options.files.front();
  if (!ReadFile(path, source, reason)) {
    std::cerr << path << ": cannot read: " << reason << "\n";
    return 2;
  }
  const ParseResult parsed = flow::Parse(source);
  const ResolveResult resolved = Resolve(source, parsed);
  if (resolved.HasErrors()) {
    for (const Diagnostic& diagnostic : resolved.diagnostics) {
      if (diagnostic.severity == Severity::kError) {
        std::cerr << DiagnosticToText(Reported(path), diagnostic) << "\n";
      }
    }
    return 1;
  }
  nlohmann::json schemas = nlohmann::json::object();
  for (const DtoPlan& dto : resolved.program.dtos) {
    if (!options.dto.empty() && dto.name != options.dto) continue;
    schemas[dto.name] = DtoToJsonSchema(dto, resolved.program);
  }
  if (schemas.empty()) {
    std::cerr << path << ": declares no shape"
              << (options.dto.empty() ? "" : absl::StrCat(" '", options.dto, "'"))
              << ".\n";
    return 1;
  }
  // One shape asked for is that shape's schema; the whole file is an object of
  // them, keyed by name -- so a script can ask for one and get something it can
  // hand straight over.
  Emit(schemas.size() == 1 && !options.dto.empty() ? schemas.begin().value()
                                                   : schemas);
  return 0;
}

int Complete(const Options& options) {
  std::string source;
  std::string reason;
  const std::string& path = options.files.front();
  if (!ReadFile(path, source, reason)) {
    std::cerr << path << ": cannot read: " << reason << "\n";
    return 2;
  }
  size_t offset = 0;
  if (options.offset >= 0) {
    offset = static_cast<size_t>(options.offset);
  } else if (options.line > 0) {
    const LineIndex lines(source);
    offset = lines.LineStart(options.line);
    // A column counts characters, so it is walked rather than added.
    int column = 1;
    while (offset < source.size() && column < options.column &&
           source[offset] != '\n') {
      const unsigned char lead = static_cast<unsigned char>(source[offset]);
      size_t width = 1;
      if (lead >= 0xF0) {
        width = 4;
      } else if (lead >= 0xE0) {
        width = 3;
      } else if (lead >= 0xC0) {
        width = 2;
      }
      offset += width;
      ++column;
    }
  } else {
    std::cerr << "complete takes --offset N, or --line L and --column C\n";
    return 2;
  }
  const CompleteResult completed = CompleteAt(source, offset);
  if (options.format != Output::kText) {
    Emit(CompletionsToJsonValue(completed));
    return 0;
  }
  for (const Proposal& proposal : completed.proposals) {
    const std::string kind(ProposalKindName(proposal.kind));
    std::printf("%-13s %s%s%s\n", kind.c_str(), proposal.name.c_str(),
                proposal.tail.c_str(),
                proposal.type.empty() ? ""
                                      : absl::StrCat(": ", proposal.type).c_str());
  }
  return 0;
}

int Codes(const Options& options) {
  if (options.format != Output::kText) {
    Emit(CodesToJsonValue());
    return 0;
  }
  size_t width = 0;
  for (const CodeInfo& info : KnownCodes()) {
    width = std::max(width, info.code.size());
  }
  for (const CodeInfo& info : KnownCodes()) {
    std::printf("%-*s  %-12s  %.*s\n", static_cast<int>(width),
                std::string(info.code).c_str(),
                std::string(SeverityName(info.severity)).c_str(),
                static_cast<int>(info.summary.size()), info.summary.data());
  }
  return 0;
}

int Vocabulary(const Options& options) {
  const nlohmann::json table = VocabularyToJsonValue();
  if (options.format != Output::kText) {
    Emit(table);
    return 0;
  }
  for (const auto& [key, words] : table.items()) {
    if (!words.is_array()) continue;
    std::vector<std::string> list;
    for (const nlohmann::json& word : words) list.push_back(word);
    std::printf("%-18s %s\n", key.c_str(), absl::StrJoin(list, " ").c_str());
  }
  return 0;
}

int Syntax(const Options& options) {
  SyntaxTarget target = SyntaxTarget::kSublime;
  if (!SyntaxTargetFromName(options.target, target)) {
    std::cerr << "No editor definition is generated for " << options.target
              << "\n";
    return 2;
  }
  const std::string generated = GenerateSyntax(target);
  const std::string path =
      absl::StrCat(options.root, "/", SyntaxTargetPath(target));
  if (options.generate) {
    if (!WriteFile(path, generated)) {
      std::cerr << path << ": cannot write\n";
      return 2;
    }
    std::cout << path << ": generated\n";
    return 0;
  }
  // The default is the check, because that is what CI runs and what a person
  // asking "is this up to date" means.
  std::string existing;
  std::string reason;
  if (!ReadFile(path, existing, reason)) {
    std::cerr << path << ": cannot read: " << reason << "\n";
    return 2;
  }
  if (existing == generated) {
    if (!options.quiet) std::cout << path << ": up to date\n";
    return 0;
  }
  std::cerr << path
            << ": out of date -- run `a11 flow syntax --target "
            << SyntaxTargetName(target) << " --generate`\n";
  return 1;
}

int Serve(const Options& options) {
  if (options.protocol == "lsp") return RunLsp(std::cin, std::cout);
  if (options.protocol != "json") {
    std::cerr << "serve speaks json or lsp, not " << options.protocol << "\n";
    return 2;
  }
  // One request per line, one answer per line. A source with line breaks in it
  // is a JSON string with `\n` in it, so a line is always a whole request --
  // which is what makes this usable from a shell, a test, or a plugin with a
  // pipe and no framing library.
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
    if (request.is_discarded()) {
      std::cout << nlohmann::json{
                       {"ok", false},
                       {"error",
                        nlohmann::json{{"message", "That is not JSON."}}}}
                       .dump()
                << "\n";
      std::cout.flush();
      continue;
    }
    std::cout << Handle(request).dump() << "\n";
    std::cout.flush();
  }
  return 0;
}

// --- The command line --------------------------------------------------------

Options ReadOptions(int argc, char** argv) {
  Options options;
  std::vector<std::string> arguments(argv + 1, argv + argc);
  for (size_t at = 0; at < arguments.size(); ++at) {
    const std::string& argument = arguments[at];
    const auto value = [&](std::string_view name) -> std::string {
      if (at + 1 >= arguments.size()) {
        options.complaint = absl::StrCat(name, " takes a value.");
        return "";
      }
      return arguments[++at];
    };
    if (argument == "--format") {
      const std::string named = value("--format");
      if (named == "text") {
        options.format = Output::kText;
      } else if (named == "json") {
        options.format = Output::kJson;
      } else if (named == "sarif") {
        options.format = Output::kSarif;
      } else if (options.complaint.empty()) {
        options.complaint = absl::StrCat("--format is text, json or sarif, not ",
                                         named, ".");
      }
    } else if (argument == "--quiet" || argument == "-q") {
      options.quiet = true;
    } else if (argument == "--check") {
      options.check = true;
    } else if (argument == "--generate") {
      options.generate = true;
    } else if (argument == "-i" || argument == "--in-place") {
      options.in_place = true;
    } else if (argument == "--offset") {
      if (!absl::SimpleAtoi(value("--offset"), &options.offset)) {
        options.complaint = "--offset is a number of bytes.";
      }
    } else if (argument == "--line") {
      if (!absl::SimpleAtoi(value("--line"), &options.line)) {
        options.complaint = "--line is a line number.";
      }
    } else if (argument == "--column") {
      if (!absl::SimpleAtoi(value("--column"), &options.column)) {
        options.complaint = "--column is a column number.";
      }
    } else if (argument == "--struct") {
      options.dto = value("--struct");
    } else if (argument == "--target") {
      options.target = value("--target");
    } else if (argument == "--root") {
      options.root = value("--root");
    } else if (argument == "--protocol") {
      options.protocol = value("--protocol");
    } else if (argument == "--help" || argument == "-h") {
      options.command = "help";
    } else if (argument == "--version") {
      options.command = "version";
    } else if (argument.size() > 1 && argument[0] == '-' && argument != kStdin) {
      options.complaint = absl::StrCat(argument, " is not an option this takes.");
    } else if (options.command.empty()) {
      options.command = argument;
    } else {
      options.files.push_back(argument);
    }
    if (!options.complaint.empty()) return options;
  }
  return options;
}

int Main(int argc, char** argv) {
  const Options options = ReadOptions(argc, argv);
  if (!options.complaint.empty()) {
    std::cerr << options.complaint << "\n";
    return 2;
  }
  const std::string& command = options.command;
  if (command.empty() || command == "help") {
    PrintUsage();
    return command.empty() ? 2 : 0;
  }
  if (command == "version") {
    // The formats are the contract, so they are what a version means here.
    std::cout << "a11-flow, speaking " << kDiagnosticsFormat << " "
              << kTokensFormat << " " << kSyntaxFormat << " " << kPlanFormat
              << " " << kFormatFormat << " " << kCompletionsFormat << " "
              << kCodesFormat << " " << kVocabularyFormat << "\n";
    return 0;
  }
  const bool wants_file =
      command == "check" || command == "fmt" || command == "highlight" ||
      command == "parse" || command == "describe" || command == "complete" ||
      command == "schema";
  if (wants_file && options.files.empty()) {
    std::cerr << command << " takes a file, or `-` for standard input\n";
    return 2;
  }
  if (command == "check") return Check(options);
  if (command == "fmt") return Fmt(options);
  if (command == "highlight") return Highlight(options);
  if (command == "parse") return Parse(options);
  if (command == "describe") return Describe(options);
  if (command == "schema") return Schema(options);
  if (command == "complete") return Complete(options);
  if (command == "codes") return Codes(options);
  if (command == "vocabulary") return Vocabulary(options);
  if (command == "syntax") return Syntax(options);
  if (command == "serve") return Serve(options);
  std::cerr << command << " is not a command. Try `a11-flow help`.\n";
  return 2;
}

}  // namespace
}  // namespace a11::flow::tool

int main(int argc, char** argv) { return a11::flow::tool::Main(argc, argv); }

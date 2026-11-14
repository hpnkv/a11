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

#include "a11/flow/tool/lsp.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_join.h>
#include <nlohmann/json.hpp>

#include "a11/flow/catalogue.h"
#include "a11/flow/complete.h"
#include "a11/flow/diagnostic.h"
#include "a11/flow/discover.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/file_modules.h"
#include "a11/flow/highlight.h"
#include "a11/flow/lexer.h"
#include "a11/flow/navigate.h"
#include "a11/flow/offsets.h"
#include "a11/flow/service.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow::tool {
namespace {

/// Open source text and its client version, indexed by UTF-16 position.
class Document : public TextIndex {
 public:
  Document() = default;

  explicit Document(std::string text) : TextIndex(std::move(text)) {}

  [[nodiscard]] int Version() const { return version_; }

  void SetVersion(int version) { version_ = version; }

 private:
  int version_ = 0;
};

// --- Semantic tokens ---------------------------------------------------------

/// Standard semantic-token names advertised to LSP clients.
constexpr std::array kTokenTypes = {
    std::string_view("comment"),
    std::string_view("string"),
    std::string_view("number"),
    std::string_view("keyword"),
    std::string_view("function"),
    std::string_view("type"),
    std::string_view("enumMember"),
    std::string_view("operator"),
    std::string_view("namespace"),
    std::string_view("property"),
    std::string_view("variable"),
    // Ports use the standard semantic-token type for parameters.
    std::string_view("parameter"),
    std::string_view("stringEscape"),
};

/// Maps a semantic kind to the legend, or `-1` when it has no LSP token.
int TokenTypeOf(SemanticKind kind) {
  switch (kind) {
    case SemanticKind::kComment:
      return 0;
    case SemanticKind::kString:
      return 1;
    case SemanticKind::kStringEscape:
      return 12;
    case SemanticKind::kNumber:
    case SemanticKind::kDuration:
      return 2;
    case SemanticKind::kBuiltinType:
    case SemanticKind::kDeclarationKeyword:
    case SemanticKind::kStatementKeyword:
    case SemanticKind::kModifierKeyword:
    case SemanticKind::kConstant:
      return 3;
    case SemanticKind::kStage:
    case SemanticKind::kBuiltin:
    case SemanticKind::kFlowName:
    case SemanticKind::kActionName:
      return 4;
    case SemanticKind::kType:
      return 5;
    case SemanticKind::kStatusCode:
    case SemanticKind::kLogLevel:
      return 6;
    case SemanticKind::kWordOperator:
    case SemanticKind::kFlowOperator:
    case SemanticKind::kOperator:
      return 7;
    case SemanticKind::kNamespaceName:
    case SemanticKind::kNodeMapName:
      return 8;
    case SemanticKind::kMember:
      return 9;
    case SemanticKind::kIdentifier:
      return 10;
    case SemanticKind::kPortName:
      return 11;
    case SemanticKind::kBrace:
    case SemanticKind::kParenthesis:
    case SemanticKind::kBracket:
    case SemanticKind::kPunctuation:
    case SemanticKind::kBad:
      return -1;
  }
  return -1;
}

/// Delta-encoded semantic tokens, split at line boundaries.
nlohmann::json SemanticTokenData(const Document& document) {
  const LexResult lexed =
      Lex(document.Text(), LexOptions{.keep_comments = true});
  std::vector<SemanticToken> semantic = Highlight(lexed.tokens);
  RefinePorts(document.Text(), semantic);
  nlohmann::json data = nlohmann::json::array();
  int last_line = 0;
  int last_character = 0;
  const std::string& text = document.Text();
  for (const SemanticToken& token : semantic) {
    const int type = TokenTypeOf(token.kind);
    if (type < 0) {
      continue;
    }
    size_t start = token.start;
    while (start < token.end) {
      size_t stop = text.find('\n', start);
      if (stop == std::string::npos || stop > token.end) {
        stop = token.end;
      }
      if (stop > start) {
        const auto [line, character] = document.PositionOf(start);
        const auto [end_line, end_character] = document.PositionOf(stop);
        const int length = end_line == line ? end_character - character : 0;
        if (length > 0) {
          data.push_back(line - last_line);
          data.push_back(line == last_line ? character - last_character
                                           : character);
          data.push_back(length);
          data.push_back(type);
          data.push_back(0);
          last_line = line;
          last_character = character;
        }
      }
      start = stop + 1;
    }
  }
  return data;
}

// --- Completions -------------------------------------------------------------

/// LSP completion item kind for a Flow proposal.
int CompletionItemKind(ProposalKind kind) {
  switch (kind) {
    case ProposalKind::kStatement:
    case ProposalKind::kDeclaration:
    case ProposalKind::kModifier:
    case ProposalKind::kPortModifier:
      return 14;  // Keyword
    case ProposalKind::kStage:
    case ProposalKind::kFunction:
      return 3;  // Function
    case ProposalKind::kType:
      return 25;  // TypeParameter
    case ProposalKind::kStatusCode:
    case ProposalKind::kLogLevel:
      return 20;  // EnumMember
    case ProposalKind::kConstant:
      return 21;  // Constant
    case ProposalKind::kFlow:
      return 2;  // Method
    case ProposalKind::kPort:
      return 5;  // Field
    case ProposalKind::kNode:
    case ProposalKind::kNodeMap:
      return 9;  // Module
    case ProposalKind::kCall:
      return 6;  // Variable
    case ProposalKind::kBarrier:
    case ProposalKind::kVariable:
    case ProposalKind::kHeader:
      return 6;  // Variable
    case ProposalKind::kField:
      return 10;  // Property
  }
  return 1;
}

/// Escaped LSP snippet with `$0` at the proposal's caret.
std::string Snippet(const Proposal& proposal) {
  std::string out;
  const std::string& insert = proposal.insert;
  const size_t caret =
      proposal.caret < 0 ? insert.size() : static_cast<size_t>(proposal.caret);
  for (size_t at = 0; at <= insert.size(); ++at) {
    if (at == caret) {
      absl::StrAppend(&out, "$0");
    }
    if (at == insert.size()) {
      break;
    }
    const char letter = insert[at];
    if (letter == '$' || letter == '}' || letter == '\\') {
      out.push_back('\\');
    }
    out.push_back(letter);
  }
  return out;
}

// --- The loop ----------------------------------------------------------------

class Server {
 public:
  Server(std::istream& in, std::ostream& out,
         std::vector<std::string> source_roots, CompilationLimits limits)
      : in_(in),
        out_(out),
        source_roots_(std::move(source_roots)),
        limits_(limits) {}

  int Run() {
    std::string body;
    while (Read(body)) {
      nlohmann::json message;
      message = nlohmann::json::parse(body, nullptr, false);
      if (message.is_discarded()) {
        continue;
      }
      Dispatch(message);
      if (exited_) {
        return 0;
      }
    }
    return shutdown_ ? 0 : 1;
  }

 private:
  ModuleSources Sources() const {
    ModuleSources sources;
    for (const auto& [uri, document] : documents_) {
      sources.emplace(FileModuleIdentity(uri), document.Text());
    }
    return sources;
  }

  nlohmann::json HostHandle(nlohmann::json request) const {
    auto sources = Sources();
    const auto path = request.find("path");
    if (path != request.end() && path->is_string()) {
      request["path"] = FileModuleIdentity(path->get<std::string>());
    }
    return flow::Handle(request,
                        FileModuleResolver(limits_.max_source_bytes,
                                           std::move(sources), source_roots_),
                        limits_);
  }

  struct Analysis {
    CompilationBudget budget;
    LinkedModule linked;
    catalogue::Catalogue known;

    Analysis(const Document& document, const std::string& uri, size_t offset,
             const ModuleSources& sources,
             const std::vector<std::string>& roots,
             const catalogue::Catalogue& registry, CompilationLimits limits)
        : budget(limits),
          linked(LinkModules(
              document.Text(), FileModuleIdentity(uri), sources,
              FileModuleResolver(limits.max_source_bytes, sources, roots),
              limits, &budget)),
          known(registry.MergedWith(
              linked.CatalogueAt(offset, true, limits, &budget))) {}
  };

  /// One message, headers and all. False at end of stream.
  bool Read(std::string& body) {
    size_t length = 0;
    std::string line;
    bool saw_header = false;
    while (std::getline(in_, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty()) {
        if (!saw_header) {
          continue;  // A stray blank line before any header.
        }
        break;
      }
      saw_header = true;
      const size_t colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      const std::string name = absl::AsciiStrToLower(
          absl::StripAsciiWhitespace(std::string_view(line).substr(0, colon)));
      const std::string_view value =
          absl::StripAsciiWhitespace(std::string_view(line).substr(colon + 1));
      if (name == "content-length") {
        length = static_cast<size_t>(
            std::strtoull(std::string(value).c_str(), nullptr, 10));
      }
    }
    if (!saw_header) {
      return false;
    }
    body.assign(length, '\0');
    in_.read(body.data(), static_cast<std::streamsize>(length));
    return in_.gcount() == static_cast<std::streamsize>(length);
  }

  void Send(const nlohmann::json& message) {
    const std::string payload = message.dump();
    out_ << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
    out_.flush();
  }

  void Answer(const nlohmann::json& request, nlohmann::json result) {
    if (!request.contains("id")) {
      return;  // A notification wants no answer.
    }
    Send(nlohmann::json{{"jsonrpc", "2.0"},
                        {"id", request.at("id")},
                        {"result", std::move(result)}});
  }

  void Notify(std::string_view method, nlohmann::json params) {
    Send(nlohmann::json{{"jsonrpc", "2.0"},
                        {"method", std::string(method)},
                        {"params", std::move(params)}});
  }

  [[nodiscard]] const Document* Find(const nlohmann::json& params) const {
    const std::string uri = Uri(params);
    const auto found = documents_.find(uri);
    return found == documents_.end() ? nullptr : &found->second;
  }

  static std::string Uri(const nlohmann::json& params) {
    if (!params.is_object()) {
      return "";
    }
    const auto document = params.find("textDocument");
    if (document == params.end() || !document->is_object()) {
      return "";
    }
    return document->value("uri", std::string());
  }

  /// Converts a path to a `file://` URI, preserving path separators.
  /// Other bytes outside the unreserved set are percent-encoded.
  static std::string FileUri(std::string_view path) {
    std::string out = "file://";
    if (!path.empty() && path.front() != '/') {
      // Relative paths resolve against the process working directory.
      std::error_code error;
      const std::filesystem::path absolute =
          std::filesystem::absolute(std::filesystem::path(path), error);
      if (!error) {
        return FileUri(absolute.string());
      }
    }
    for (const char c : path) {
      const auto byte = static_cast<unsigned char>(c);
      const bool plain = absl::ascii_isalnum(byte) || c == '/' || c == '-' ||
                         c == '_' || c == '.' || c == '~';
      if (plain) {
        out.push_back(c);
      } else {
        absl::StrAppend(&out, absl::StrFormat("%%%02X", byte));
      }
    }
    return out;
  }

  /// Converts a 1-based source position to a zero-width LSP range.
  /// The column is used without UTF-16 conversion when source text is absent.
  static nlohmann::json PositionAt(int line, int column) {
    const int zero_line = line > 0 ? line - 1 : 0;
    const int zero_column = column > 0 ? column - 1 : 0;
    nlohmann::json position{{"line", zero_line}, {"character", zero_column}};
    return nlohmann::json{{"start", position}, {"end", position}};
  }

  static nlohmann::json RangeOf(const Document& document, const Range& range) {
    const auto [start_line, start_character] =
        document.PositionOf(range.start.offset);
    const auto [end_line, end_character] =
        document.PositionOf(range.end.offset);
    return nlohmann::json{
        {"start",
         nlohmann::json{{"line", start_line}, {"character", start_character}}},
        {"end",
         nlohmann::json{{"line", end_line}, {"character", end_character}}},
    };
  }

  [[nodiscard]] nlohmann::json EditOf(const Document& document,
                                      const Edit& edit) const {
    Range range;
    range.start.offset = edit.start;
    range.end.offset = edit.end;
    return nlohmann::json{{"range", RangeOf(document, range)},
                          {"newText", edit.text}};
  }

  /// Publishes document diagnostics with fixes in their `data` fields.
  void Publish(const std::string& uri, const Document& document) {
    const nlohmann::json answer =
        HostHandle(nlohmann::json{{"method", "check"},
                                  {"source", document.Text()},
                                  {"path", uri},
                                  {"offsets", "utf16"}});
    nlohmann::json groups = nlohmann::json::object();
    groups[uri] = nlohmann::json::array();
    const auto range = [](const nlohmann::json& positions) {
      const auto position = [](const nlohmann::json& one) {
        return nlohmann::json{
            {"line", one.value("line", 1) - 1},
            {"character",
             one.value("utf16_column", one.value("column", 1)) - 1}};
      };
      return nlohmann::json{{"start", position(positions.at("start"))},
                            {"end", position(positions.at("end"))}};
    };
    if (answer.value("ok", false)) {
      for (const nlohmann::json& entry :
           answer.at("result").at("diagnostics")) {
        const Diagnostic diagnostic = DiagnosticFromJsonValue(entry);
        const std::string target =
            diagnostic.source.empty() ||
                    diagnostic.source == FileModuleIdentity(uri)
                ? uri
                : FileUri(diagnostic.source);
        if (!groups.contains(target)) {
          groups[target] = nlohmann::json::array();
        }
        nlohmann::json written{
            {"range", range(entry.at("range"))},
            {"severity", Severity(diagnostic.severity)},
            {"code", diagnostic.code},
            {"source", "a11-flow"},
            {"message", diagnostic.message},
        };
        if (!diagnostic.related.empty()) {
          written["relatedInformation"] = nlohmann::json::array();
          for (const auto& related : entry.at("related")) {
            written["relatedInformation"].push_back(
                {{"location",
                  {{"uri", FileUri(related.value("source", diagnostic.source))},
                   {"range", range(related.at("range"))}}},
                 {"message", related.at("message")}});
          }
        }
        if (!diagnostic.fixes.empty() && target == uri) {
          nlohmann::json fixes = nlohmann::json::array();
          for (const Fix& fix : diagnostic.fixes) {
            nlohmann::json edits = nlohmann::json::array();
            for (const Edit& edit : fix.edits) {
              Edit bytes = edit;
              bytes.start = document.ByteOf(edit.start);
              bytes.end = document.ByteOf(edit.end);
              edits.push_back(EditOf(document, bytes));
            }
            fixes.push_back(nlohmann::json{{"label", fix.label},
                                           {"edits", edits},
                                           {"automatic", fix.automatic}});
          }
          written["data"] = nlohmann::json{{"fixes", fixes}};
        }
        groups[target].push_back(std::move(written));
      }
    }
    auto old = publications_.find(uri);
    std::vector<std::string> targets;
    if (old != publications_.end()) {
      for (const auto& item : old->second.items()) {
        targets.push_back(item.key());
      }
    }
    for (const auto& item : groups.items()) {
      targets.push_back(item.key());
    }
    publications_.insert_or_assign(uri, std::move(groups));
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (const auto& target : targets) {
      auto list = nlohmann::json::array();
      for (const auto& [owner, contribution] : publications_) {
        if (contribution.contains(target)) {
          for (const auto& diagnostic : contribution.at(target)) {
            list.push_back(diagnostic);
          }
        }
      }
      nlohmann::json params{{"uri", target}, {"diagnostics", std::move(list)}};
      if (target == uri && document.Version() != 0) {
        params["version"] = document.Version();
      }
      Notify("textDocument/publishDiagnostics", std::move(params));
    }
  }

  /// Maps Flow severities to LSP severity numbers.
  static int Severity(flow::Severity severity) {
    switch (severity) {
      case flow::Severity::kError:
        return 1;
      case flow::Severity::kWarning:
        return 2;
      case flow::Severity::kWeakWarning:
        return 3;  // Information: a hint is hidden by default in most clients.
      case flow::Severity::kInformation:
        return 4;
    }
    return 1;
  }

  void Dispatch(const nlohmann::json& message) {
    const std::string method = message.value("method", std::string());
    const nlohmann::json params = message.contains("params")
                                      ? message.at("params")
                                      : nlohmann::json::object();

    if (method == "initialize") {
      ReadClientCapabilities(message.value("params", nlohmann::json::object()));
      Answer(message, Capabilities());
      return;
    }
    if (method == "shutdown") {
      shutdown_ = true;
      Answer(message, nlohmann::json());
      return;
    }
    if (method == "exit") {
      exited_ = true;
      return;
    }
    if (method == "textDocument/didOpen") {
      const std::string uri = Uri(params);
      const auto document = params.find("textDocument");
      std::string text;
      int version = 0;
      if (document != params.end() && document->is_object()) {
        text = document->value("text", std::string());
        version = document->value("version", 0);
      }
      Document& held = documents_[uri] = Document(std::move(text));
      held.SetVersion(version);
      for (const auto& [open_uri, open_document] : documents_) {
        Publish(open_uri, open_document);
      }
      return;
    }
    if (method == "textDocument/didChange") {
      const std::string uri = Uri(params);
      // The advertised synchronization mode sends complete document text.
      const auto changes = params.find("contentChanges");
      if (changes != params.end() && changes->is_array() && !changes->empty()) {
        const nlohmann::json& last = changes->back();
        Document held(last.value("text", std::string()));
        const auto document = params.find("textDocument");
        if (document != params.end() && document->is_object()) {
          held.SetVersion(document->value("version", 0));
        }
        documents_[uri] = std::move(held);
      }
      for (const auto& [open_uri, open_document] : documents_) {
        Publish(open_uri, open_document);
      }
      return;
    }
    if (method == "textDocument/didSave") {
      const auto found = documents_.find(Uri(params));
      if (found != documents_.end()) {
        Publish(found->first, found->second);
      }
      return;
    }
    if (method == "textDocument/didClose") {
      const std::string uri = Uri(params);
      documents_.erase(uri);
      const auto previous = publications_.find(uri);
      if (previous != publications_.end()) {
        const auto targets = previous->second;
        publications_.erase(previous);
        for (const auto& target : targets.items()) {
          Notify("textDocument/publishDiagnostics",
                 nlohmann::json{{"uri", target.key()},
                                {"diagnostics", nlohmann::json::array()}});
        }
      }
      for (const auto& [open_uri, open_document] : documents_) {
        Publish(open_uri, open_document);
      }
      bool retained = false;
      for (const auto& [owner, contribution] : publications_) {
        retained |= contribution.contains(uri);
      }
      if (!retained) {
        Notify("textDocument/publishDiagnostics",
               nlohmann::json{{"uri", uri},
                              {"diagnostics", nlohmann::json::array()}});
      }
      return;
    }
    if (method == "textDocument/semanticTokens/full") {
      const Document* document = Find(params);
      if (document == nullptr) {
        Answer(message, nlohmann::json{{"data", nlohmann::json::array()}});
        return;
      }
      Answer(message, nlohmann::json{{"data", SemanticTokenData(*document)}});
      return;
    }
    if (method == "textDocument/formatting") {
      const Document* document = Find(params);
      if (document == nullptr) {
        Answer(message, nlohmann::json::array());
        return;
      }
      const nlohmann::json answer = HostHandle(
          nlohmann::json{{"method", "format"}, {"source", document->Text()}});
      nlohmann::json edits = nlohmann::json::array();
      if (answer.value("ok", false)) {
        for (const nlohmann::json& edit : answer.at("result").at("edits")) {
          Edit one;
          one.start = edit.value("start", static_cast<size_t>(0));
          one.end = edit.value("end", static_cast<size_t>(0));
          one.text = edit.value("text", std::string());
          edits.push_back(EditOf(*document, one));
        }
      }
      Answer(message, std::move(edits));
      return;
    }
    if (method == "textDocument/completion") {
      Complete(message, params);
      return;
    }
    if (method == "textDocument/codeAction") {
      CodeActions(message, params);
      return;
    }
    if (method == "textDocument/hover") {
      Hover(message, params);
      return;
    }
    if (method == "textDocument/documentSymbol") {
      DocumentSymbols(message, params);
      return;
    }
    if (method == "textDocument/definition" ||
        method == "textDocument/declaration" ||
        method == "textDocument/typeDefinition") {
      // Definition, declaration, and type definition share name resolution.
      Definition(message, params);
      return;
    }
    if (method == "a11flow/setContext") {
      SetContext(params);
      for (const auto& [uri, document] : documents_) {
        Publish(uri, document);
      }
      return;
    }
    if (method == "a11flow/scan") {
      Scan(message, params);
      return;
    }
    if (method == "a11flow/relay") {
      nlohmann::json relayed =
          params.is_object() ? params : nlohmann::json::object();
      if (!relayed.contains("context") && !known_.Empty()) {
        // Include this session's action and type catalogue.
        nlohmann::json context = known_.ToJson();
        context["replace"] = true;
        relayed["context"] = std::move(context);
      }
      Answer(message, HostHandle(relayed));
      return;
    }
    if (message.contains("id")) {
      Answer(message, nlohmann::json());
    }
  }

  [[nodiscard]] size_t OffsetIn(const Document& document,
                                const nlohmann::json& params,
                                std::string_view key = "position") const {
    const auto position = params.find(key);
    if (position == params.end() || !position->is_object()) {
      return 0;
    }
    return document.ByteOfPosition(position->value("line", 0),
                                   position->value("character", 0));
  }

  void Complete(const nlohmann::json& message, const nlohmann::json& params) {
    const Document* document = Find(params);
    if (document == nullptr) {
      Answer(message, nlohmann::json::array());
      return;
    }
    const size_t offset = OffsetIn(*document, params);
    Analysis analysis(*document, Uri(params), offset, Sources(), source_roots_,
                      known_, limits_);
    const CompleteResult completed =
        CompleteAt(document->Text(), offset, analysis.known, limits_,
                   &analysis.budget, &analysis.linked);
    Range replaced;
    replaced.start.offset = std::min(completed.prefix_start, offset);
    replaced.end.offset = offset;
    nlohmann::json items = nlohmann::json::array();
    for (const Proposal& proposal : completed.proposals) {
      replaced.start.offset = proposal.replace_start == std::string::npos
                                  ? std::min(completed.prefix_start, offset)
                                  : std::min(proposal.replace_start, offset);
      nlohmann::json item{
          {"label", proposal.name},
          {"kind", CompletionItemKind(proposal.kind)},
          {"textEdit", nlohmann::json{{"range", RangeOf(*document, replaced)},
                                      {"newText", Snippet(proposal)}}},
          {"insertTextFormat", 2},  // Snippet: the caret goes where `$0` is.
          {"filterText",
           proposal.filter.empty() ? proposal.name : proposal.filter},
          // Preserve proposal order in the client's completion list.
          {"sortText", absl::StrCat(SortKey(items.size()))},
      };
      std::string detail = proposal.type;
      if (label_details_) {
        if (!proposal.tail.empty()) {
          item["labelDetails"] = nlohmann::json{{"detail", proposal.tail}};
        }
      } else if (!proposal.tail.empty()) {
        // Remove the tail's leading space when no type precedes it.
        absl::StrAppend(&detail, proposal.tail);
        if (!detail.empty() && detail.front() == ' ') {
          detail.erase(0, 1);
        }
      }
      if (!detail.empty()) {
        item["detail"] = detail;
      }
      if (!proposal.documentation.empty()) {
        item["documentation"] = nlohmann::json{
            {"kind", "markdown"}, {"value", proposal.documentation}};
      }
      items.push_back(std::move(item));
    }
    Answer(message, nlohmann::json{{"isIncomplete", false}, {"items", items}});
  }

  /// Zero-padded sort key preserving proposal order.
  static std::string SortKey(size_t index) {
    std::string key = std::to_string(index);
    return std::string(4 - std::min<size_t>(4, key.size()), '0') + key;
  }

  void CodeActions(const nlohmann::json& message,
                   const nlohmann::json& params) {
    const Document* document = Find(params);
    nlohmann::json actions = nlohmann::json::array();
    if (document == nullptr) {
      Answer(message, std::move(actions));
      return;
    }
    const std::string uri = Uri(params);
    const auto context = params.find("context");
    if (context != params.end() && context->is_object()) {
      const auto diagnostics = context->find("diagnostics");
      if (diagnostics != context->end() && diagnostics->is_array()) {
        for (const nlohmann::json& diagnostic : *diagnostics) {
          // Apply the edits carried by each diagnostic fix.
          const auto data = diagnostic.find("data");
          if (data == diagnostic.end() || !data->is_object()) {
            continue;
          }
          const auto fixes = data->find("fixes");
          if (fixes == data->end() || !fixes->is_array()) {
            continue;
          }
          for (const nlohmann::json& fix : *fixes) {
            nlohmann::json edits = fix.value("edits", nlohmann::json::array());
            actions.push_back(nlohmann::json{
                {"title", fix.value("label", std::string("Fix"))},
                {"kind", "quickfix"},
                {"isPreferred", fix.value("automatic", true)},
                {"diagnostics", nlohmann::json::array({diagnostic})},
                {"edit",
                 nlohmann::json{
                     {"changes", nlohmann::json{{uri, std::move(edits)}}}}},
            });
          }
        }
      }
    }
    Answer(message, std::move(actions));
  }

  void Hover(const nlohmann::json& message, const nlohmann::json& params) {
    const Document* document = Find(params);
    if (document == nullptr) {
      Answer(message, nlohmann::json());
      return;
    }
    const auto offset = OffsetIn(*document, params);
    Analysis analysis(*document, Uri(params), offset, Sources(), source_roots_,
                      known_, limits_);
    const Description about =
        Describe(document->Text(), offset, analysis.known, limits_,
                 &analysis.budget, &analysis.linked);
    if (!about.found || about.markdown.empty()) {
      Answer(message, nlohmann::json());
      return;
    }
    Answer(message, nlohmann::json{
                        {"contents", nlohmann::json{{"kind", "markdown"},
                                                    {"value", about.markdown}}},
                        {"range", RangeOf(*document, about.range)}});
  }

  void DocumentSymbols(const nlohmann::json& message,
                       const nlohmann::json& params) {
    const Document* document = Find(params);
    nlohmann::json symbols = nlohmann::json::array();
    if (document == nullptr) {
      Answer(message, std::move(symbols));
      return;
    }
    for (const DocumentSymbol& symbol : Symbols(document->Text(), limits_)) {
      symbols.push_back(SymbolJson(*document, symbol));
    }
    Answer(message, std::move(symbols));
  }

  void Definition(const nlohmann::json& message, const nlohmann::json& params) {
    const Document* document = Find(params);
    if (document == nullptr) {
      Answer(message, nlohmann::json());
      return;
    }
    const auto offset = OffsetIn(*document, params);
    Analysis analysis(*document, Uri(params), offset, Sources(), source_roots_,
                      known_, limits_);
    const Description about =
        Describe(document->Text(), offset, analysis.known, limits_,
                 &analysis.budget, &analysis.linked);
    if (about.has_definition) {
      Answer(message,
             nlohmann::json{{"uri", Uri(params)},
                            {"range", RangeOf(*document, about.definition)}});
      return;
    }
    // Project actions may resolve to declarations outside this document.
    if (about.origin.has_value()) {
      auto range = PositionAt(about.origin->line, about.origin->column);
      const auto identity = FileModuleIdentity(about.origin->file);
      auto source = FileModuleResolver(limits_.max_source_bytes, Sources(),
                                       source_roots_)(identity, "<flow>");
      if (source.ok()) {
        const TextIndex index(source->source);
        const auto position = index.PositionOf(index.ByteOfSourcePosition(
            about.origin->line, about.origin->column));
        range = nlohmann::json{
            {"start",
             {{"line", position.first}, {"character", position.second}}},
            {"end",
             {{"line", position.first}, {"character", position.second}}}};
      }
      Answer(message, nlohmann::json{{"uri", FileUri(about.origin->file)},
                                     {"range", range}});
      return;
    }
    Answer(message, nlohmann::json());
  }

  /// One symbol, as the protocol's `DocumentSymbol`.
  static nlohmann::json SymbolJson(const Document& document,
                                   const DocumentSymbol& symbol) {
    nlohmann::json children = nlohmann::json::array();
    for (const DocumentSymbol& child : symbol.children) {
      children.push_back(SymbolJson(document, child));
    }
    nlohmann::json value{
        {"name", symbol.name},
        {"kind", SymbolItemKind(symbol.kind)},
        {"range", RangeOf(document, symbol.range)},
        {"selectionRange", RangeOf(document, symbol.selection)}};
    if (!symbol.detail.empty()) {
      value["detail"] = symbol.detail;
    }
    if (!children.empty()) {
      value["children"] = std::move(children);
    }
    return value;
  }

  /// The protocol's symbol kinds, by number.
  static int SymbolItemKind(SymbolClass kind) {
    switch (kind) {
      case SymbolClass::kModule:
        return 2;
      case SymbolClass::kFlow:
        return 12;  // Function
      case SymbolClass::kDto:
        return 23;  // Struct
      case SymbolClass::kField:
        return 8;  // Field
      case SymbolClass::kPort:
        return 8;  // Field
      case SymbolClass::kHeader:
        return 20;  // Key
      case SymbolClass::kNodeMap:
        return 2;  // Module
      case SymbolClass::kNode:
        return 13;  // Variable
      case SymbolClass::kCall:
        return 6;  // Method
      case SymbolClass::kBarrier:
        return 13;  // Variable
      case SymbolClass::kVariable:
        return 13;  // Variable
      case SymbolClass::kExternal:
        return 13;  // Variable
    }
    return 13;
  }

  /// What the client said it can render, for the fields that are opt-in.
  void ReadClientCapabilities(const nlohmann::json& params) {
    label_details_ = false;
    if (!params.is_object()) {
      return;
    }
    const auto capabilities = params.find("capabilities");
    if (capabilities == params.end() || !capabilities->is_object()) {
      return;
    }
    const auto document = capabilities->find("textDocument");
    if (document == capabilities->end() || !document->is_object()) {
      return;
    }
    const auto completion = document->find("completion");
    if (completion == document->end() || !completion->is_object()) {
      return;
    }
    const auto item = completion->find("completionItem");
    if (item == completion->end() || !item->is_object()) {
      return;
    }
    label_details_ = item->value("labelDetailsSupport", false);
  }

  /// Scans project action declarations and merges them into session context.
  /// Returns scan counts and limit flags, then republishes open diagnostics.
  void Scan(const nlohmann::json& message, const nlohmann::json& params) {
    std::vector<std::string> roots;
    if (params.is_object()) {
      if (const auto paths = params.find("paths");
          paths != params.end() && paths->is_array()) {
        for (const nlohmann::json& one : *paths) {
          if (one.is_string()) {
            roots.push_back(one.get<std::string>());
          }
        }
      }
    }
    if (roots.empty()) {
      if (message.contains("id")) {
        Answer(message, nlohmann::json{{"actions", 0},
                                       {"error", "no paths were given"}});
      }
      return;
    }
    const discover::Result found = discover::Discover(roots);
    known_ = known_.MergedWith(found.found);
    for (const auto& [uri, document] : documents_) {
      Publish(uri, document);
    }
    if (message.contains("id")) {
      Answer(message,
             nlohmann::json{{"actions", found.found.actions().size()},
                            {"files_read", found.files_read},
                            {"reached_file_limit", found.reached_file_limit},
                            {"too_large", found.too_large}});
    }
  }

  /// Sets the action and type catalogue for subsequent session requests.
  void SetContext(const nlohmann::json& params) {
    if (!params.is_object()) {
      return;
    }
    auto given = catalogue::Catalogue::FromJson(params);
    known_ = params.value("replace", false)
                 ? std::move(given)
                 : catalogue::Catalogue::Builtin().MergedWith(given);
  }

  static nlohmann::json Capabilities() {
    nlohmann::json types = nlohmann::json::array();
    for (const std::string_view type : kTokenTypes) {
      types.push_back(type);
    }
    return nlohmann::json{
        {"capabilities",
         nlohmann::json{
             // Full sync: see `didChange`.
             {"textDocumentSync",
              nlohmann::json{
                  {"openClose", true}, {"change", 1}, {"save", true}}},
             {"documentFormattingProvider", true},
             {"documentSymbolProvider", true},
             {"definitionProvider", true},
             {"declarationProvider", true},
             {"typeDefinitionProvider", true},
             {"hoverProvider", true},
             {"codeActionProvider",
              nlohmann::json{
                  {"codeActionKinds", nlohmann::json::array({"quickfix"})}}},
             {"completionProvider",
              nlohmann::json{
                  {"triggerCharacters",
                   // Argument delimiters trigger port completions.
                   nlohmann::json::array({".", "|", ":", ">", " ", "(", ","})},
                  {"resolveProvider", false}}},
             {"semanticTokensProvider",
              nlohmann::json{
                  {"legend",
                   nlohmann::json{{"tokenTypes", types},
                                  {"tokenModifiers", nlohmann::json::array()}}},
                  {"full", true}}},
         }},
        {"serverInfo", nlohmann::json{{"name", "a11-flow"}, {"version", "1"}}},
    };
  }

  std::istream& in_;
  std::ostream& out_;
  std::vector<std::string> source_roots_;
  CompilationLimits limits_;
  absl::flat_hash_map<std::string, Document> documents_;
  absl::flat_hash_map<std::string, nlohmann::json> publications_;
  /// Action and type metadata retained for this session.
  catalogue::Catalogue known_ = catalogue::Catalogue::Builtin();
  bool shutdown_ = false;
  bool exited_ = false;
  /// Whether completion rows support `labelDetails`.
  /// Without it, descriptions appear in the standard `detail` field.
  bool label_details_ = false;
};

}  // namespace

int RunLsp(std::istream& in, std::ostream& out,
           std::vector<std::string> source_roots, CompilationLimits limits) {
  return Server(in, out, std::move(source_roots), limits).Run();
}

}  // namespace a11::flow::tool

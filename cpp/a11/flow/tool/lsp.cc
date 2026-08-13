// Copyright 2026 The A11 Authors.

#include "a11/flow/tool/lsp.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <nlohmann/json.hpp>

#include "a11/flow/complete.h"
#include "a11/flow/diagnostic.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/highlight.h"
#include "a11/flow/lexer.h"
#include "a11/flow/offsets.h"
#include "a11/flow/service.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow::tool {
namespace {

/// One open document, and the version the client last told us about.
///
/// The offset arithmetic lives in [TextIndex] rather than here. It used to live
/// here, and that was the bug: the JSON protocol had no conversion at all, so the
/// IntelliJ plugin read byte offsets as if they were the UTF-16 offsets its
/// document buffer is indexed by, and everything after the first non-ASCII
/// character in a file was coloured in the wrong place. One conversion, both
/// protocols.
class Document : public TextIndex {
 public:
  Document() = default;
  explicit Document(std::string text) : TextIndex(std::move(text)) {}

  int Version() const { return version_; }
  void SetVersion(int version) { version_ = version; }

 private:
  int version_ = 0;
};

// --- Semantic tokens ---------------------------------------------------------

/// The legend, in the standard names, so a client with no A11-specific theme
/// still colours a flow sensibly.
constexpr std::array kTokenTypes = {
    std::string_view("comment"),  std::string_view("string"),
    std::string_view("number"),   std::string_view("keyword"),
    std::string_view("function"), std::string_view("type"),
    std::string_view("enumMember"), std::string_view("operator"),
    std::string_view("namespace"), std::string_view("property"),
    std::string_view("variable"),
};

/// Which legend entry a meaning maps to, or `-1` for the ones a semantic
/// highlighter leaves alone -- punctuation is the lexer's business and every
/// client already draws it.
int TokenTypeOf(SemanticKind kind) {
  switch (kind) {
    case SemanticKind::kComment:
      return 0;
    case SemanticKind::kString:
      return 1;
    case SemanticKind::kNumber:
    case SemanticKind::kDuration:
      return 2;
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
      return 6;
    case SemanticKind::kWordOperator:
    case SemanticKind::kFlowOperator:
    case SemanticKind::kOperator:
      return 7;
    case SemanticKind::kNodeMapName:
      return 8;
    case SemanticKind::kMember:
      return 9;
    case SemanticKind::kIdentifier:
      return 10;
    case SemanticKind::kBrace:
    case SemanticKind::kParenthesis:
    case SemanticKind::kBracket:
    case SemanticKind::kPunctuation:
    case SemanticKind::kBad:
      return -1;
  }
  return -1;
}

/// The delta-encoded token data the protocol asks for.
///
/// A token may not span a line in this encoding, and two things in this language
/// do -- a `"""` string and nothing else, in practice -- so one that does is cut
/// at each break rather than dropped.
nlohmann::json SemanticTokenData(const Document& document) {
  const LexResult lexed =
      Lex(document.Text(), LexOptions{.keep_comments = true});
  nlohmann::json data = nlohmann::json::array();
  int last_line = 0;
  int last_character = 0;
  const std::string& text = document.Text();
  for (const SemanticToken& token : Highlight(lexed.tokens)) {
    const int type = TokenTypeOf(token.kind);
    if (type < 0) continue;
    size_t start = token.start;
    while (start < token.end) {
      size_t stop = text.find('\n', start);
      if (stop == std::string::npos || stop > token.end) stop = token.end;
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

/// The protocol's item kinds, by number, for the kinds this language has.
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

/// Snippet text with the caret where the proposal wants it.
std::string Snippet(const Proposal& proposal) {
  std::string out;
  const std::string& insert = proposal.insert;
  const size_t caret = proposal.caret < 0
                           ? insert.size()
                           : static_cast<size_t>(proposal.caret);
  for (size_t at = 0; at <= insert.size(); ++at) {
    if (at == caret) absl::StrAppend(&out, "$0");
    if (at == insert.size()) break;
    const char letter = insert[at];
    if (letter == '$' || letter == '}' || letter == '\\') out.push_back('\\');
    out.push_back(letter);
  }
  return out;
}

// --- Hover -------------------------------------------------------------------

/// What the word under the caret is, in one line.
///
/// Read off the classifier rather than a table of prose: the honest answer to
/// "what is this" in a language this small is what it *means here*, which is the
/// judgement the highlighter already makes.
std::string HoverText(const Document& document, size_t offset) {
  const LexResult lexed =
      Lex(document.Text(), LexOptions{.keep_comments = true});
  const std::vector<SemanticToken> semantic = Highlight(lexed.tokens);
  for (size_t index = 0; index < semantic.size(); ++index) {
    const SemanticToken& token = semantic[index];
    if (offset < token.start || offset >= token.end) continue;
    const std::string_view text(document.Text().data() + token.start,
                                token.end - token.start);
    const std::string word = vocabulary::Canonical(text);
    std::string what(SemanticKindName(token.kind));
    for (char& letter : what) {
      if (letter == '-') letter = ' ';
    }
    std::string detail;
    if (token.kind == SemanticKind::kStage) {
      const auto takes = vocabulary::StageTakes(word);
      if (takes.has_value()) {
        detail = absl::StrCat("takes ",
                              vocabulary::StageArgumentName(*takes));
      }
    } else if (token.kind == SemanticKind::kStatusCode) {
      detail = "one of Abseil's canonical status codes";
    } else if (token.kind == SemanticKind::kBuiltin) {
      detail = "one of the language's fixed functions";
    }
    return detail.empty() ? absl::StrCat("`", text, "` -- ", what)
                          : absl::StrCat("`", text, "` -- ", what, ", ", detail);
  }
  return "";
}

// --- The loop ----------------------------------------------------------------

class Server {
 public:
  Server(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

  int Run() {
    std::string body;
    while (Read(body)) {
      nlohmann::json message;
      // A client that sent something unreadable is a client with a bug, and
      // there is nobody to tell: no id, no method, nothing to answer.
      message = nlohmann::json::parse(body, nullptr, false);
      if (message.is_discarded()) continue;
      Dispatch(message);
      if (exited_) return 0;
    }
    // The stream ended. A client that meant to stop said `shutdown` first.
    return shutdown_ ? 0 : 1;
  }

 private:
  /// One message, headers and all. False at end of stream.
  bool Read(std::string& body) {
    size_t length = 0;
    std::string line;
    bool saw_header = false;
    while (std::getline(in_, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) {
        if (!saw_header) continue;  // A stray blank line before any header.
        break;
      }
      saw_header = true;
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      const std::string name =
          absl::AsciiStrToLower(absl::StripAsciiWhitespace(
              std::string_view(line).substr(0, colon)));
      const std::string_view value =
          absl::StripAsciiWhitespace(std::string_view(line).substr(colon + 1));
      if (name == "content-length") {
        length = static_cast<size_t>(std::strtoull(std::string(value).c_str(),
                                                   nullptr, 10));
      }
    }
    if (!saw_header) return false;
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
    if (!request.contains("id")) return;  // A notification wants no answer.
    Send(nlohmann::json{{"jsonrpc", "2.0"},
                        {"id", request.at("id")},
                        {"result", std::move(result)}});
  }

  void Notify(std::string_view method, nlohmann::json params) {
    Send(nlohmann::json{{"jsonrpc", "2.0"},
                        {"method", std::string(method)},
                        {"params", std::move(params)}});
  }

  const Document* Find(const nlohmann::json& params) const {
    const std::string uri = Uri(params);
    const auto found = documents_.find(uri);
    return found == documents_.end() ? nullptr : &found->second;
  }

  static std::string Uri(const nlohmann::json& params) {
    if (!params.is_object()) return "";
    const auto document = params.find("textDocument");
    if (document == params.end() || !document->is_object()) return "";
    return document->value("uri", std::string());
  }

  nlohmann::json RangeOf(const Document& document, const Range& range) const {
    const auto [start_line, start_character] =
        document.PositionOf(range.start.offset);
    const auto [end_line, end_character] = document.PositionOf(range.end.offset);
    return nlohmann::json{
        {"start", nlohmann::json{{"line", start_line},
                                 {"character", start_character}}},
        {"end", nlohmann::json{{"line", end_line},
                               {"character", end_character}}},
    };
  }

  nlohmann::json EditOf(const Document& document, const Edit& edit) const {
    Range range;
    range.start.offset = edit.start;
    range.end.offset = edit.end;
    return nlohmann::json{{"range", RangeOf(document, range)},
                          {"newText", edit.text}};
  }

  /// The diagnostics of one document, in the protocol's shape.
  ///
  /// `data` carries the fixes back with the diagnostic, so `codeAction` needs no
  /// second analysis of the document to know what Alt+Enter offers: the fix was
  /// worked out where the problem was found.
  void Publish(const std::string& uri, const Document& document) {
    const nlohmann::json answer = Handle(nlohmann::json{
        {"method", "check"}, {"source", document.Text()}, {"path", uri}});
    nlohmann::json list = nlohmann::json::array();
    if (answer.value("ok", false)) {
      for (const nlohmann::json& entry :
           answer.at("result").at("diagnostics")) {
        const Diagnostic diagnostic = DiagnosticFromJsonValue(entry);
        nlohmann::json written{
            {"range", RangeOf(document, diagnostic.range)},
            {"severity", Severity(diagnostic.severity)},
            {"code", diagnostic.code},
            {"source", "a11-flow"},
            {"message", diagnostic.message},
        };
        if (!diagnostic.fixes.empty()) {
          nlohmann::json fixes = nlohmann::json::array();
          for (const Fix& fix : diagnostic.fixes) {
            nlohmann::json edits = nlohmann::json::array();
            for (const Edit& edit : fix.edits) {
              edits.push_back(EditOf(document, edit));
            }
            fixes.push_back(
                nlohmann::json{{"label", fix.label}, {"edits", edits}});
          }
          written["data"] = nlohmann::json{{"fixes", fixes}};
        }
        list.push_back(std::move(written));
      }
    }
    nlohmann::json params{{"uri", uri}, {"diagnostics", list}};
    if (document.Version() != 0) params["version"] = document.Version();
    Notify("textDocument/publishDiagnostics", std::move(params));
  }

  /// The protocol's severities, which are the language's four in its three.
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
    const nlohmann::json params =
        message.contains("params") ? message.at("params") : nlohmann::json::object();

    if (method == "initialize") {
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
      Publish(uri, held);
      return;
    }
    if (method == "textDocument/didChange") {
      const std::string uri = Uri(params);
      // Full sync only, which is what the capabilities ask for: a whole document
      // is what every method here takes, and stitching incremental changes back
      // together would be a second copy of the document's state to get wrong.
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
      const auto found = documents_.find(uri);
      if (found != documents_.end()) Publish(uri, found->second);
      return;
    }
    if (method == "textDocument/didSave") {
      const auto found = documents_.find(Uri(params));
      if (found != documents_.end()) Publish(found->first, found->second);
      return;
    }
    if (method == "textDocument/didClose") {
      const std::string uri = Uri(params);
      documents_.erase(uri);
      // An empty list is how a server says "nothing is wrong here any more",
      // which is what closing a file means.
      Notify("textDocument/publishDiagnostics",
             nlohmann::json{{"uri", uri},
                            {"diagnostics", nlohmann::json::array()}});
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
      const nlohmann::json answer = Handle(nlohmann::json{
          {"method", "format"}, {"source", document->Text()}});
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
    // Anything else: an unknown request still needs an answer, or a client waits
    // for one for ever.
    if (message.contains("id")) Answer(message, nlohmann::json());
  }

  size_t OffsetIn(const Document& document, const nlohmann::json& params,
                  std::string_view key = "position") const {
    const auto position = params.find(key);
    if (position == params.end() || !position->is_object()) return 0;
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
    const CompleteResult completed = CompleteAt(document->Text(), offset);
    Range replaced;
    replaced.start.offset = completed.prefix_start;
    replaced.end.offset = offset;
    const nlohmann::json range = RangeOf(*document, replaced);
    nlohmann::json items = nlohmann::json::array();
    for (const Proposal& proposal : completed.proposals) {
      nlohmann::json item{
          {"label", proposal.name},
          {"kind", CompletionItemKind(proposal.kind)},
          {"textEdit",
           nlohmann::json{{"range", range}, {"newText", Snippet(proposal)}}},
          {"insertTextFormat", 2},  // Snippet: the caret goes where `$0` is.
          {"filterText", proposal.name},
          // The order the language proposes them in is the order they should be
          // offered in, so every item sorts by its position in the list rather
          // than alphabetically.
          {"sortText", absl::StrCat(SortKey(items.size()))},
      };
      if (!proposal.type.empty()) item["detail"] = proposal.type;
      if (!proposal.tail.empty()) {
        item["labelDetails"] = nlohmann::json{{"detail", proposal.tail}};
      }
      items.push_back(std::move(item));
    }
    Answer(message, nlohmann::json{{"isIncomplete", false}, {"items", items}});
  }

  /// A sort key that keeps the language's order: zero-padded, so `10` follows
  /// `9` rather than `1`.
  static std::string SortKey(size_t index) {
    std::string key = std::to_string(index);
    return std::string(4 - std::min<size_t>(4, key.size()), '0') + key;
  }

  void CodeActions(const nlohmann::json& message, const nlohmann::json& params) {
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
          // The fixes travelled with the diagnostic, so this is a translation
          // rather than another analysis -- and a fix an editor applies is
          // exactly the edits the language wrote down.
          const auto data = diagnostic.find("data");
          if (data == diagnostic.end() || !data->is_object()) continue;
          const auto fixes = data->find("fixes");
          if (fixes == data->end() || !fixes->is_array()) continue;
          for (const nlohmann::json& fix : *fixes) {
            nlohmann::json edits = fix.value("edits", nlohmann::json::array());
            actions.push_back(nlohmann::json{
                {"title", fix.value("label", std::string("Fix"))},
                {"kind", "quickfix"},
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
    const std::string text = HoverText(*document, OffsetIn(*document, params));
    if (text.empty()) {
      Answer(message, nlohmann::json());
      return;
    }
    Answer(message,
           nlohmann::json{{"contents", nlohmann::json{{"kind", "markdown"},
                                                      {"value", text}}}});
  }

  static nlohmann::json Capabilities() {
    nlohmann::json types = nlohmann::json::array();
    for (const std::string_view type : kTokenTypes) types.push_back(type);
    return nlohmann::json{
        {"capabilities",
         nlohmann::json{
             // Full sync: see `didChange`.
             {"textDocumentSync", nlohmann::json{{"openClose", true},
                                                 {"change", 1},
                                                 {"save", true}}},
             {"documentFormattingProvider", true},
             {"hoverProvider", true},
             {"codeActionProvider", nlohmann::json{{"codeActionKinds",
                                                    nlohmann::json::array(
                                                        {"quickfix"})}}},
             {"completionProvider",
              nlohmann::json{{"triggerCharacters",
                              nlohmann::json::array({".", "|", ":", ">", " "})},
                             {"resolveProvider", false}}},
             {"semanticTokensProvider",
              nlohmann::json{
                  {"legend", nlohmann::json{{"tokenTypes", types},
                                            {"tokenModifiers",
                                             nlohmann::json::array()}}},
                  {"full", true}}},
         }},
        {"serverInfo",
         nlohmann::json{{"name", "a11-flow"}, {"version", "1"}}},
    };
  }

  std::istream& in_;
  std::ostream& out_;
  absl::flat_hash_map<std::string, Document> documents_;
  bool shutdown_ = false;
  bool exited_ = false;
};

}  // namespace

int RunLsp(std::istream& in, std::ostream& out) {
  return Server(in, out).Run();
}

}  // namespace a11::flow::tool

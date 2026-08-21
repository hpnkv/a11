// Copyright 2026 The A11 Authors.

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
#include "a11/flow/highlight.h"
#include "a11/flow/lexer.h"
#include "a11/flow/navigate.h"
#include "a11/flow/offsets.h"
#include "a11/flow/service.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow::tool {
namespace {

/// One open document, and the version the client last told us about.
///
/// TextIndex converts between UTF-8 byte offsets and the protocol's UTF-16
/// positions.
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

/// The legend, in the standard names, so a client with no A11-specific theme
/// still colours a flow sensibly.
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
    case SemanticKind::kLogLevel:
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

/// The delta-encoded token data the protocol asks for.
///
/// Splits multiline tokens at line boundaries as required by the protocol.
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

/// Snippet text with the caret where the proposal wants it.
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
  Server(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

  int Run() {
    std::string body;
    while (Read(body)) {
      nlohmann::json message;
      // A client that sent something unreadable is a client with a bug, and
      // there is nobody to tell: no id, no method, nothing to answer.
      message = nlohmann::json::parse(body, nullptr, false);
      if (message.is_discarded()) {
        continue;
      }
      Dispatch(message);
      if (exited_) {
        return 0;
      }
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

  /// A filesystem path as a `file://` URI.
  ///
  /// Percent-encodes everything outside the unreserved set, which is what a path
  /// with a space in it needs. The separators stay separators: a URI whose
  /// slashes were escaped would name one file called `a/b`.
  static std::string FileUri(std::string_view path) {
    std::string out = "file://";
    if (!path.empty() && path.front() != '/') {
      // A relative path is relative to wherever the tool was started, which is
      // the project root for every host that starts one. Making it absolute here
      // would guess at a directory this process may not be in.
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

  /// A zero-width range at a 1-based line and column, as the protocol counts
  /// them: 0-based, and a column in UTF-16 units.
  ///
  /// The column is used as given rather than converted, because converting it
  /// would mean reading a file this process was never asked to open. A caret one
  /// unit out on a line with prose before the declaration is a smaller wrong
  /// than opening the wrong file, and every host scrolls to the line.
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
    if (document.Version() != 0) {
      params["version"] = document.Version();
    }
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
      if (found != documents_.end()) {
        Publish(uri, found->second);
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
      const nlohmann::json answer = Handle(
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
      // All three ask the same question of a language this size: where was this
      // name bound. Answering them alike is what makes every one of an editor's
      // three navigation gestures land somewhere useful.
      Definition(message, params);
      return;
    }
    if (method == "a11flow/setContext") {
      SetContext(params);
      // A notification: no `id`, so no answer, and the documents already open
      // are re-checked because what the world contains may change what is wrong
      // with them.
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
      // The JSON service, reached through the LSP connection: `{"method":
      // "check", "source": ".."}` answered exactly as `serve --protocol json`
      // answers it.
      //
      // For the questions the protocol has no place for, and there is one that
      // matters: a flow written inside a *string literal* of another language is
      // not a document the server has, so it is asked about as text. Without this
      // a client would have to run a second process for it, and the two would
      // then disagree about the world the moment one of them was sent a context.
      nlohmann::json relayed =
          params.is_object() ? params : nlohmann::json::object();
      if (!relayed.contains("context") && !known_.Empty()) {
        // What this session knows, so a fragment is checked against the same
        // world its file's `.flow` siblings are.
        nlohmann::json context = known_.ToJson();
        context["replace"] = true;
        relayed["context"] = std::move(context);
      }
      Answer(message, Handle(relayed));
      return;
    }
    // Anything else: an unknown request still needs an answer, or a client waits
    // for one for ever.
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
    // Against the world this session knows, which is the whole point of knowing
    // it. Hover and go-to-declaration were given `known_` and this was not, so an
    // action the project declares hovered with its description and had somewhere
    // to go, and was the one thing never *offered* -- which is the case that
    // matters most, since a name you have to know already is a name you did not
    // need completing.
    const CompleteResult completed =
        CompleteAt(document->Text(), offset, known_);
    // What taking a proposal replaces: the partial word, and nothing else.
    //
    // Clamped rather than trusted. Every other answer in this adapter is read-only
    // -- a colour, a message, a hover -- and this one *edits the document*, so the
    // cost of the language being wrong about the range is somebody's file rather
    // than a wrong colour. It was wrong once, and a completion taken where nothing
    // had been typed replaced everything before the caret.
    Range replaced;
    replaced.start.offset = std::min(completed.prefix_start, offset);
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
      // The type beside the name, and what the thing is *for* after it. A client
      // that renders `labelDetails` gets the two apart, which reads better; one
      // that does not gets both in `detail`, because a description that only
      // appears for some clients is a description somebody does not see.
      std::string detail = proposal.type;
      if (label_details_) {
        if (!proposal.tail.empty()) {
          item["labelDetails"] = nlohmann::json{{"detail", proposal.tail}};
        }
      } else if (!proposal.tail.empty()) {
        // The tail already opens with the space that separates it from whatever
        // it follows, so nothing is added here; with no type in front of it that
        // space would be a leading one, and is taken off.
        absl::StrAppend(&detail, proposal.tail);
        if (!detail.empty() && detail.front() == ' ') {
          detail.erase(0, 1);
        }
      }
      if (!detail.empty()) {
        item["detail"] = detail;
      }
      // The reference text beside the list, which is the same text a hover over
      // the finished word gives. The language works it out for every proposal it
      // has one for; dropping it here was why this client's popup was empty
      // where the IntelliJ one, reading the same field off the JSON protocol,
      // was not.
      if (!proposal.documentation.empty()) {
        item["documentation"] = nlohmann::json{
            {"kind", "markdown"}, {"value", proposal.documentation}};
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
          // The fixes travelled with the diagnostic, so this is a translation
          // rather than another analysis -- and a fix an editor applies is
          // exactly the edits the language wrote down.
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
    // The language decides what is under the caret -- it is name resolution, and
    // this adapter has no business doing it a second time. All that happens here
    // is the translation into what the protocol calls a hover.
    const Description about =
        Describe(document->Text(), OffsetIn(*document, params), known_);
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
    for (const DocumentSymbol& symbol : Symbols(document->Text())) {
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
    const Description about =
        Describe(document->Text(), OffsetIn(*document, params), known_);
    if (about.has_definition) {
      Answer(message,
             nlohmann::json{{"uri", Uri(params)},
                            {"range", RangeOf(*document, about.definition)}});
      return;
    }
    // A name this document did not declare may still have somewhere to go: an
    // action declared in a project file that something scanned. This is the one
    // answer that leaves the document, and it is why an `ActionSchema` written
    // in a `.py` two directories away is now a jump rather than a search.
    if (about.origin.has_value()) {
      Answer(message,
             nlohmann::json{{"uri", FileUri(about.origin->file)},
                            {"range", PositionAt(about.origin->line,
                                                 about.origin->column)}});
      return;
    }
    // A word that is not a name at all -- a stage, a keyword -- has no location.
    // Null rather than an empty list, which is what a client reads as "nowhere
    // to go".
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

  /// `a11flow/scan`: read the project's own source for the actions it declares,
  /// and fold what is found into the world these documents are read against.
  ///
  /// A *request* rather than a notification, unlike `setContext`, because a
  /// client wants to know what was found: how many actions, and whether a cap
  /// stopped the walk. It behaves like a `setContext` besides -- the result is
  /// merged over what is already known, and every open document is re-checked,
  /// since an action that exists now may make a flow that named it correct.
  ///
  /// Merged rather than replacing, so a host may scan *and* send a live registry
  /// and get both.
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

  /// `a11flow/setContext`: what the world outside these documents contains.
  ///
  /// Stateful, and deliberately so. A client that knows which action registry an
  /// inline flow is attached to says it once, and every completion and hover for
  /// the rest of the session sees it -- rather than repeating a catalogue of a
  /// hundred actions on every keystroke.
  void SetContext(const nlohmann::json& params) {
    if (!params.is_object()) {
      return;
    }
    const catalogue::Catalogue given = catalogue::Catalogue::FromJson(params);
    known_ = params.value("replace", false)
                 ? given
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
                   // `(` and `,` are what open an argument list and
                   // what separate one argument from the next, so both
                   // are positions where the answer is "these ports".
                   // Without them, typing `run act(` asked for nothing
                   // and the ports of the action being called -- the
                   // thing hardest to remember -- were the one list
                   // that needed Ctrl+Space to see.
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
  absl::flat_hash_map<std::string, Document> documents_;
  /// What the world outside these documents contains, for as long as this
  /// session lasts: see [SetContext].
  catalogue::Catalogue known_ = catalogue::Catalogue::Builtin();
  bool shutdown_ = false;
  bool exited_ = false;
  /// Whether the client said it renders `labelDetails`.
  ///
  /// It is an opt-in capability, and a client that did not ask for it is entitled
  /// to ignore the field -- which would drop a port's description out of the row
  /// and leave the list saying only the port's name. So where it is absent the
  /// same text goes in `detail`, which every client has always shown.
  bool label_details_ = false;
};

}  // namespace

int RunLsp(std::istream& in, std::ostream& out) {
  return Server(in, out).Run();
}

}  // namespace a11::flow::tool

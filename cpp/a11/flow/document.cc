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

#include "a11/flow/document.h"

#include <algorithm>
#include <tuple>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status_macros.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <cmath>

#include "a11/flow/emit_json.h"
#include "a11/flow/format.h"
#include "a11/flow/inspect.h"
#include "a11/flow/lexer.h"
#include "a11/flow/schema.h"
#include "a11/utf8.h"

namespace a11::flow {
namespace {
absl::Status Name(std::string_view name, bool dotted = false) {
  auto lexed = Lex(name);
  bool word = true;
  for (const auto& token : lexed.tokens) {
    if (token.kind == TokenKind::kEnd) {
      break;
    }
    if (word ? token.kind != TokenKind::kWord
             : (!dotted || (token.kind != TokenKind::kDot &&
                            token.kind != TokenKind::kScope))) {
      return absl::InvalidArgumentError(
          "A name must contain words separated by namespace or projection "
          "tokens.");
    }
    word = !word;
  }
  if (name.empty() || word || !lexed.diagnostics.empty()) {
    return absl::InvalidArgumentError("Expected a name.");
  }
  return absl::OkStatus();
}

std::string Quote(std::string_view value) {
  return nlohmann::json(value).dump();
}

absl::Status Parsed(std::string_view source) {
  auto parsed = Parse(source);
  if (auto* error = parsed.FirstError(); error != nullptr) {
    return absl::InvalidArgumentError(error->message);
  }
  return absl::OkStatus();
}

// Match source tokens so schema maintenance retains comments and whitespace.
absl::StatusOr<std::vector<Edit>> MaintainTokens(std::string_view original,
                                                 std::string_view generated,
                                                 size_t base) {
  const auto significant = [](std::string_view text) {
    std::vector<Token> result;
    for (const auto& token :
         Lex(text, LexOptions{.keep_comments = true}).tokens) {
      if (token.kind != TokenKind::kComment &&
          token.kind != TokenKind::kNewline && token.kind != TokenKind::kEnd) {
        result.push_back(token);
      }
    }
    return result;
  };
  const auto before = significant(original), after = significant(generated);
  if (before.size() > 2048 || after.size() > 2048) {
    return absl::ResourceExhaustedError(
        "A declaration is too large for a source-preserving synchronization.");
  }
  const size_t width = after.size() + 1;
  std::vector<uint16_t> lengths((before.size() + 1) * width, 0);
  const auto same = [&](size_t a, size_t b) {
    return before[a].kind == after[b].kind && before[a].text == after[b].text;
  };
  for (size_t i = before.size(); i-- > 0;) {
    for (size_t j = after.size(); j-- > 0;) {
      lengths[i * width + j] = same(i, j)
                                   ? 1 + lengths[(i + 1) * width + j + 1]
                                   : std::max(lengths[(i + 1) * width + j],
                                              lengths[i * width + j + 1]);
    }
  }
  std::vector<Edit> edits;
  size_t i = 0, j = 0;
  while (i < before.size() || j < after.size()) {
    if (i < before.size() && j < after.size() && same(i, j)) {
      ++i;
      ++j;
      continue;
    }
    const size_t from_i = i, from_j = j;
    while (i < before.size() || j < after.size()) {
      if (i < before.size() && j < after.size() && same(i, j)) {
        break;
      }
      if (j < after.size() &&
          (i == before.size() ||
           lengths[i * width + j + 1] > lengths[(i + 1) * width + j])) {
        ++j;
      } else {
        ++i;
      }
    }
    std::string replacement;
    if (j > from_j) {
      replacement = generated.substr(after[from_j].start,
                                     after[j - 1].end - after[from_j].start);
      const size_t prior = from_j == 0 ? 0 : after[from_j - 1].end;
      if (generated.substr(prior, after[from_j].start - prior).find('\n') !=
          std::string_view::npos) {
        replacement.insert(0, "\n  ");
      } else {
        replacement.insert(0, " ");
      }
      replacement += " ";
      if (j < after.size() &&
          generated.substr(after[j - 1].end, after[j].start - after[j - 1].end)
                  .find('\n') != std::string_view::npos) {
        replacement += "\n  ";
      }
    }
    if (i > from_i) {
      edits.push_back({base + before[from_i].start, base + before[from_i].end,
                       replacement});
      for (size_t k = from_i + 1; k < i; ++k) {
        edits.push_back({base + before[k].start, base + before[k].end, ""});
      }
    } else {
      const size_t offset =
          i < before.size() ? before[i].start : original.size();
      edits.push_back({base + offset, base + offset, replacement});
    }
  }
  return edits;
}

absl::StatusOr<std::string> Interface(const FlowPlan& plan, bool declaration) {
  if (!utf8::IsValid(plan.name) || !utf8::IsValid(plan.description)) {
    return absl::InvalidArgumentError(
        "Flow names and descriptions must contain valid UTF-8.");
  }
  if (plan.name.empty()) {
    return absl::InvalidArgumentError("A constructed flow requires a name.");
  }
  std::string text = absl::StrCat(declaration ? "declare flow " : "flow ",
                                  syntax::ActionSource(plan.name),
                                  plan.exported ? " exported" : "", " {\n");
  if (!plan.description.empty()) {
    absl::StrAppend(&text, "  describe ", Quote(plan.description), "\n");
  }
  for (const auto& port : plan.ports) {
    ABSL_RETURN_IF_ERROR(Name(port.name));
    if (!utf8::IsValid(port.description) || !utf8::IsValid(port.declared) ||
        !utf8::IsValid(port.type)) {
      return absl::InvalidArgumentError("Port text must contain valid UTF-8.");
    }
    std::string type_source = syntax::SourceTypeName(
        port.declared.empty() ? port.type : port.declared);
    if (!port.constraints.empty() && port.declared == "any") {
      return absl::FailedPreconditionError(
          "An unresolved payload conjunction has no lossless Flow spelling.");
    }
    if (port.syntax_type.has_value() &&
        port.syntax_type->ToString() == port.declared) {
      ABSL_ASSIGN_OR_RETURN(type_source, port.syntax_type->ToSource());
    }
    absl::StrAppend(
        &text,
        port.direction == syntax::PortDirection::kInput ? "  in " : "  out ",
        port.name, ": ", type_source, port.unary ? "" : " stream",
        port.required ? " required" : "");
    if (!port.description.empty()) {
      absl::StrAppend(&text, " ", Quote(port.description));
    }
    text += '\n';
  }
  for (const auto& header : plan.headers) {
    if (!utf8::IsValid(header.name) || !utf8::IsValid(header.description)) {
      return absl::InvalidArgumentError(
          "Header text must contain valid UTF-8.");
    }
    if (header.has_default) {
      ABSL_RETURN_IF_ERROR(
          Expression::Literal(ConstantToJsonValue(header.default_value))
              .status());
    }
    ABSL_RETURN_IF_ERROR(
        Name(header.alias.empty() ? header.name : header.alias));
    absl::StrAppend(&text, "  header ", Quote(header.name), " as ",
                    header.alias.empty() ? header.name : header.alias);
    if (header.has_default) {
      absl::StrAppend(&text, " default ",
                      ConstantToJsonValue(header.default_value).dump());
    }
    if (!header.description.empty()) {
      absl::StrAppend(&text, " ", Quote(header.description));
    }
    text += '\n';
  }
  return text;
}
}  // namespace

Expression Expression::Take(bool optional) const {
  return Expression(
      absl::StrCat("(", optional ? "try take (" : "take (", source_, "))"));
}

absl::StatusOr<Expression> Expression::Transfer(const Expression& destination,
                                                bool tolerant) const {
  auto source = absl::StrCat("(", tolerant ? "try " : "", source_, " -> ",
                             destination.source(), ")");
  ABSL_RETURN_IF_ERROR(Parsed(absl::StrCat("flow { sink = ", source, "\n}")));
  return Expression(std::move(source));
}

absl::StatusOr<Expression> Expression::Reference(std::string name) {
  ABSL_RETURN_IF_ERROR(Name(name, true));
  return Expression(std::move(name));
}

absl::StatusOr<Expression> Expression::Literal(nlohmann::json value) {
  if (value.is_discarded() || value.is_binary()) {
    return absl::InvalidArgumentError("Expected a Flow literal.");
  }
  std::vector<std::pair<const nlohmann::json*, size_t>> pending{{&value, 0}};
  while (!pending.empty()) {
    const auto [item, depth] = pending.back();
    pending.pop_back();
    if (depth > 128 || item->is_discarded() || item->is_binary()) {
      return absl::InvalidArgumentError(
          "Literal nesting or representation is unsupported.");
    }
    if (item->is_string() &&
        !utf8::IsValid(item->get_ref<const std::string&>())) {
      return absl::InvalidArgumentError(
          "A literal string must contain valid UTF-8.");
    }
    if (item->is_number_float() && !std::isfinite(item->get<double>())) {
      return absl::InvalidArgumentError("A literal number must be finite.");
    }
    if (item->is_object()) {
      for (const auto& child : item->items()) {
        if (!utf8::IsValid(child.key())) {
          return absl::InvalidArgumentError(
              "A literal key must contain valid UTF-8.");
        }
        pending.push_back({&child.value(), depth + 1});
      }
    }
    if (item->is_array()) {
      for (const auto& child : *item) {
        pending.push_back({&child, depth + 1});
      }
    }
  }
  std::string text = value.dump();
  ABSL_RETURN_IF_ERROR(Parsed(absl::StrCat("flow { value = ", text, "\n}")));
  return Expression(std::move(text));
}

absl::StatusOr<Expression> Expression::Invoke(
    std::string action,
    std::vector<std::pair<std::string, Expression>> arguments, bool remote,
    bool tolerant, std::string node_map) {
  if (!utf8::IsValid(action)) {
    return absl::InvalidArgumentError(
        "An action name must contain valid UTF-8.");
  }
  std::string text =
      absl::StrCat(tolerant ? "try " : "", remote ? "call " : "run ",
                   syntax::ActionSource(action), "(");
  bool first = true;
  for (const auto& [name, value] : arguments) {
    ABSL_RETURN_IF_ERROR(Name(name));
    if (!first) {
      text += ", ";
    }
    first = false;
    absl::StrAppend(&text, name, ": ", value.source());
  }
  text += ')';
  if (!node_map.empty()) {
    ABSL_RETURN_IF_ERROR(Name(node_map));
    absl::StrAppend(&text, " via ", node_map);
  }
  return Expression(absl::StrCat("(", text, ")"));
}

absl::StatusOr<Expression> Expression::InvokeWith(
    std::string action,
    std::vector<std::pair<std::string, Expression>> arguments,
    const CallOptions& options) {
  ABSL_ASSIGN_OR_RETURN(
      auto invocation,
      Invoke(std::move(action), std::move(arguments), options.remote,
             options.tolerant, options.node_map));
  std::string source = invocation.source();
  source.pop_back();
  if (options.tee) {
    source += " tee";
  }
  if (options.timeout.has_value()) {
    if (*options.timeout <= absl::ZeroDuration()) {
      return absl::InvalidArgumentError("A call timeout must be positive.");
    }
    absl::StrAppend(&source, " timeout ",
                    absl::FormatDuration(*options.timeout));
  }
  if (options.deadline.has_value()) {
    absl::StrAppend(&source, " deadline ", options.deadline->source());
  }
  if (options.id.has_value()) {
    absl::StrAppend(&source, " id ", options.id->source());
  }
  if (!options.after.empty()) {
    source += " after ";
    for (size_t index = 0; index < options.after.size(); ++index) {
      absl::StrAppend(&source, index == 0 ? "" : ", ",
                      options.after[index].source());
    }
  }
  if (!options.headers.empty()) {
    source += " with ";
    for (size_t index = 0; index < options.headers.size(); ++index) {
      const auto& [name, value] = options.headers[index];
      if (!utf8::IsValid(name)) {
        return absl::InvalidArgumentError("Header names require valid UTF-8.");
      }
      absl::StrAppend(&source, index == 0 ? "" : ", ", Quote(name), ": ",
                      value.source());
    }
  }
  if (!options.forward.empty()) {
    source += " forward headers ";
    for (size_t index = 0; index < options.forward.size(); ++index) {
      if (!utf8::IsValid(options.forward[index])) {
        return absl::InvalidArgumentError(
            "Header patterns require valid UTF-8.");
      }
      absl::StrAppend(&source, index == 0 ? "" : ", ",
                      Quote(options.forward[index]));
    }
  }
  source += ')';
  ABSL_RETURN_IF_ERROR(Parsed(absl::StrCat("flow { value = ", source, "\n}")));
  return Expression(std::move(source));
}

absl::StatusOr<Expression> Expression::Object(
    std::vector<std::pair<std::string, Expression>> fields) {
  std::vector<std::string> parts;
  for (const auto& [name, value] : fields) {
    if (!utf8::IsValid(name)) {
      return absl::InvalidArgumentError("Object keys require valid UTF-8.");
    }
    parts.push_back(absl::StrCat(Quote(name), ": ", value.source()));
  }
  return Expression(absl::StrCat("{", absl::StrJoin(parts, ", "), "}"));
}

absl::StatusOr<Expression> Expression::Array(std::vector<Expression> items) {
  std::vector<std::string> parts;
  for (const auto& item : items) {
    parts.push_back(item.source());
  }
  return Expression(absl::StrCat("[", absl::StrJoin(parts, ", "), "]"));
}

absl::StatusOr<Expression> Expression::Builtin(
    std::string name, std::vector<Expression> arguments) {
  if (name.starts_with("std::")) {
    name.erase(0, 5);
  }
  if (!vocabulary::Builtins().contains(name)) {
    return absl::InvalidArgumentError("Unknown Flow builtin.");
  }
  std::vector<std::string> parts;
  for (const auto& item : arguments) {
    parts.push_back(item.source());
  }
  std::string source = absl::StrCat(syntax::SourceFunctionName(name), "(",
                                    absl::StrJoin(parts, ", "), ")");
  ABSL_RETURN_IF_ERROR(Parsed(absl::StrCat("flow { value = ", source, "\n}")));
  return Expression(std::move(source));
}

absl::StatusOr<Expression> Expression::Node(std::optional<Expression> id,
                                            std::string node_map) {
  std::string source =
      absl::StrCat("node(", id.has_value() ? id->source() : "", ")");
  if (!node_map.empty()) {
    ABSL_RETURN_IF_ERROR(Name(node_map));
    source = absl::StrCat("(", source, " in ", node_map, ")");
  }
  return Expression(std::move(source));
}

absl::StatusOr<Expression> Expression::Completion(std::string operation,
                                                  const Expression& subject) {
  if (operation != "wait" && operation != "close" && operation != "status") {
    return absl::InvalidArgumentError("Expected wait, close, or status.");
  }
  const auto target =
      operation == "wait" && vocabulary::Canonical(subject.source()) == "all"
          ? absl::StrCat("(", subject.source(), ")")
          : subject.source();
  return Expression(absl::StrCat("(", operation, " ", target, ")"));
}

absl::StatusOr<Expression> Expression::Cast(
    const syntax::TypeExpression& type) const {
  ABSL_ASSIGN_OR_RETURN(auto source, type.ToSource());
  return Expression(absl::StrCat("(", source_, " as ", source, ")"));
}

absl::StatusOr<Expression> Expression::Index(const Expression& index) const {
  return Expression(absl::StrCat(source_, "[", index.source(), "]"));
}

absl::StatusOr<Expression> Expression::Field(std::string name) const {
  ABSL_RETURN_IF_ERROR(Name(name));
  return Expression(absl::StrCat(source_, ".", name));
}

absl::StatusOr<Expression> Expression::Pipeline(
    std::string stage, std::optional<Expression> argument) const {
  const auto spec = vocabulary::StageTakes(stage);
  if (!spec.has_value()) {
    return absl::InvalidArgumentError("Unknown pipeline stage.");
  }
  std::string text = absl::StrCat("(", source_, " | ", stage);
  if (argument.has_value()) {
    absl::StrAppend(&text, " ", argument->source());
  }
  text += ')';
  ABSL_RETURN_IF_ERROR(Parsed(absl::StrCat("flow { value = ", text, "\n}")));
  return Expression(std::move(text));
}

absl::StatusOr<Expression> Expression::PipelineInvoke(
    std::string action,
    std::vector<std::pair<std::string, Expression>> arguments,
    const CallOptions& options) const {
  ABSL_ASSIGN_OR_RETURN(
      auto invocation,
      InvokeWith(std::move(action), std::move(arguments), options));
  const auto& called = invocation.source();
  std::string source =
      absl::StrCat("(", source_, " | ",
                   std::string_view(called).substr(1, called.size() - 2), ")");
  ABSL_RETURN_IF_ERROR(Parsed(absl::StrCat("flow { value = ", source, "\n}")));
  return Expression(std::move(source));
}

absl::StatusOr<Statement> Statement::Validated(std::string source) {
  ABSL_RETURN_IF_ERROR(Parsed(absl::StrCat("flow {\n", source, "\n}")));
  return Statement(std::move(source));
}

std::string Statement::Body(const std::vector<Statement>& statements) {
  std::string source = "{\n";
  for (const auto& statement : statements) {
    absl::StrAppend(&source, statement.source(), "\n");
  }
  return source + "}";
}

absl::StatusOr<Statement> Statement::Initialize(
    std::string name, const Expression& value,
    std::optional<syntax::TypeExpression> type) {
  ABSL_RETURN_IF_ERROR(Name(name));
  std::string prefix;
  if (type.has_value()) {
    ABSL_ASSIGN_OR_RETURN(prefix, type->ToSource());
    prefix += " ";
  }
  return Validated(absl::StrCat(prefix, name, " = ", value.source()));
}

absl::StatusOr<Statement> Statement::Pipe(const Expression& value,
                                          const Expression& destination) {
  return Validated(absl::StrCat(value.source(), " -> ", destination.source()));
}

absl::StatusOr<Statement> Statement::Wait(const Expression& subject,
                                          bool close) {
  const auto target = !close && vocabulary::Canonical(subject.source()) == "all"
                          ? absl::StrCat("(", subject.source(), ")")
                          : subject.source();
  return Validated(absl::StrCat(close ? "close " : "wait ", target));
}

absl::StatusOr<Statement> Statement::WaitAll() {
  return Validated("wait all");
}

absl::StatusOr<Statement> Statement::Cancel(const Expression& subject) {
  return Validated(absl::StrCat("cancel ", subject.source()));
}

absl::StatusOr<Statement> Statement::Fail(const Expression& code,
                                          std::optional<Expression> message) {
  return Validated(
      absl::StrCat("fail ", code.source(),
                   message.has_value() ? " " + message->source() : ""));
}

absl::StatusOr<Statement> Statement::If(
    const Expression& condition, const std::vector<Statement>& then_body,
    const std::vector<Statement>& else_body) {
  std::string source =
      absl::StrCat("if ", condition.source(), " ", Body(then_body));
  if (!else_body.empty()) {
    absl::StrAppend(&source, " else ", Body(else_body));
  }
  return Validated(std::move(source));
}

absl::StatusOr<Statement> Statement::IfSome(
    std::string name, const Expression& option,
    const std::vector<Statement>& then_body,
    const std::vector<Statement>& else_body) {
  ABSL_RETURN_IF_ERROR(Name(name));
  auto source =
      absl::StrCat("if ", name, " = ", option.source(), " ", Body(then_body));
  if (!else_body.empty()) {
    absl::StrAppend(&source, " else ", Body(else_body));
  }
  return Validated(std::move(source));
}

absl::StatusOr<Statement> Statement::For(std::string variable,
                                         const Expression& source,
                                         const std::vector<Statement>& body,
                                         int parallel) {
  ABSL_RETURN_IF_ERROR(Name(variable));
  if (parallel < 1) {
    return absl::InvalidArgumentError("Parallelism must be positive.");
  }
  return Validated(absl::StrCat("for ", variable, " in ", source.source(),
                                " parallel ", parallel, " ", Body(body)));
}

absl::StatusOr<Statement> Statement::After(
    const std::vector<Expression>& subjects, const std::vector<Statement>& body,
    bool tolerant) {
  if (subjects.empty()) {
    return absl::InvalidArgumentError(
        "An after block requires at least one subject.");
  }
  std::string source = tolerant ? "try after " : "after ";
  for (size_t index = 0; index < subjects.size(); ++index) {
    if (index != 0) {
      source += ", ";
    }
    source += subjects[index].source();
  }
  return Validated(absl::StrCat(source, " ", Body(body)));
}

absl::StatusOr<Statement> Statement::Block(const std::vector<Statement>& body,
                                           bool tolerant) {
  return Validated(absl::StrCat(tolerant ? "try " : "", Body(body)));
}

absl::Status FlowBuilder::AddStatement(const Statement& statement) {
  if (plan_.interface_only) {
    return absl::InvalidArgumentError(
        "An interface contains no executable statements.");
  }
  statements_.push_back(statement.source());
  return absl::OkStatus();
}

FlowBuilder::FlowBuilder(std::string name, bool interface_only) {
  plan_.name = std::move(name);
  plan_.interface_only = interface_only;
}

absl::Status FlowBuilder::AddPort(std::string name,
                                  const syntax::TypeExpression& type,
                                  syntax::PortDirection direction, bool unary,
                                  bool required, std::string description) {
  ABSL_RETURN_IF_ERROR(Name(name));
  ABSL_RETURN_IF_ERROR(type.ToSource().status());
  PortPlan port;
  port.name = std::move(name);
  port.syntax_type = type;
  port.type = type.ToString();
  port.declared = port.type;
  port.direction = direction;
  port.unary = unary;
  port.required = required;
  port.description = std::move(description);
  plan_.ports.push_back(std::move(port));
  return absl::OkStatus();
}

absl::Status FlowBuilder::AddInitialize(
    std::string name, const Expression& value,
    std::optional<syntax::TypeExpression> type) {
  ABSL_ASSIGN_OR_RETURN(
      auto statement,
      Statement::Initialize(std::move(name), value, std::move(type)));
  return AddStatement(statement);
}

absl::Status FlowBuilder::AddPipe(const Expression& value,
                                  const Expression& destination) {
  if (plan_.interface_only) {
    return absl::InvalidArgumentError("An interface has no statements.");
  }
  statements_.push_back(
      absl::StrCat(value.source(), " -> ", destination.source()));
  return absl::OkStatus();
}

absl::Status FlowBuilder::AddImport(std::string reference, std::string alias,
                                    std::string symbol) {
  ABSL_RETURN_IF_ERROR(Name(alias));
  if (plan_.interface_only) {
    return absl::InvalidArgumentError("An interface has no imports.");
  }
  if (!utf8::IsValid(reference)) {
    return absl::InvalidArgumentError(
        "An import reference must contain valid UTF-8.");
  }
  syntax::Import item;
  item.source = std::move(reference);
  if (!symbol.empty()) {
    ABSL_RETURN_IF_ERROR(Name(symbol, true));
    item.selective = true;
    item.namespace_source = true;
    item.symbols.push_back({syntax::Word{symbol, {}, {}},
                            syntax::Word{alias, {}, {}}, symbol != alias});
  } else {
    item.alias.text = std::move(alias);
  }
  imports_.push_back(std::move(item));
  return absl::OkStatus();
}

absl::Status FlowBuilder::AddDeclaration(const FlowPlan& interface) {
  if (plan_.interface_only || interface.exported) {
    return absl::InvalidArgumentError(
        "Local declarations require an implemented flow and cannot be "
        "exported.");
  }
  ABSL_ASSIGN_OR_RETURN(auto text, DeclarationSource(interface));
  statements_.push_back(std::move(text));
  return absl::OkStatus();
}

absl::Status FlowBuilder::AddStruct(const DtoPlan& shape) {
  if (plan_.interface_only || shape.exported) {
    return absl::InvalidArgumentError(
        "Local structs require an implemented flow and cannot be exported.");
  }
  StructBuilder builder(shape.name);
  builder.shape() = shape;
  ABSL_ASSIGN_OR_RETURN(auto text, builder.Emit());
  statements_.push_back(std::move(text));
  return absl::OkStatus();
}

absl::StatusOr<std::string> FlowBuilder::Emit() const {
  ABSL_ASSIGN_OR_RETURN(std::string text,
                        Interface(plan_, plan_.interface_only));
  for (const auto& item : imports_) {
    if (item.selective) {
      const auto& symbol = item.symbols.front();
      absl::StrAppend(&text, "  from ", item.source, " import ",
                      symbol.name.text, symbol.explicit_alias ? " as " : "",
                      symbol.explicit_alias ? symbol.alias.text : "", "\n");
    } else {
      absl::StrAppend(&text, "  import ", Quote(item.source), " as ",
                      item.alias.text, "\n");
    }
  }
  for (const auto& statement : statements_) {
    absl::StrAppend(&text, "  ", statement, "\n");
  }
  text += "}\n";
  ABSL_RETURN_IF_ERROR(Parsed(text));
  return Format(text).formatted;
}

absl::StatusOr<std::string> DeclarationSource(const FlowPlan& plan) {
  ABSL_ASSIGN_OR_RETURN(std::string text, Interface(plan, true));
  text += "}\n";
  ABSL_RETURN_IF_ERROR(Parsed(text));
  return Format(text).formatted;
}

absl::Status StructBuilder::AddField(std::string name,
                                     const syntax::TypeExpression& type,
                                     bool required,
                                     std::optional<Expression> default_value,
                                     std::string description) {
  ABSL_RETURN_IF_ERROR(Name(name));
  ABSL_RETURN_IF_ERROR(type.ToSource().status());
  if (!utf8::IsValid(description)) {
    return absl::InvalidArgumentError(
        "A description must contain valid UTF-8.");
  }
  FieldPlan field;
  field.name = std::move(name);
  field.type = type.name;
  field.declared = type.ToString();
  field.syntax_type = type;
  field.required = required;
  field.description = std::move(description);
  if (default_value.has_value()) {
    const auto parsed =
        Parse(absl::StrCat("flow { value = ", default_value->source(), "\n}"));
    if (parsed.HasErrors() || parsed.flows.empty() ||
        parsed.flows.front()->body.empty()) {
      return absl::InvalidArgumentError("A field default must be a constant.");
    }
    const auto* binding =
        syntax::As<syntax::Bind>(parsed.flows.front()->body.front().get());
    const auto constant = binding == nullptr
                              ? std::optional<syntax::Constant>{}
                              : syntax::ConstantValue(binding->value.get());
    if (!constant.has_value()) {
      return absl::InvalidArgumentError("A field default must be a constant.");
    }
    field.has_default = true;
    field.default_value = *constant;
  }
  plan_.fields.push_back(std::move(field));
  return absl::OkStatus();
}

absl::StatusOr<std::string> StructBuilder::Emit() const {
  ABSL_RETURN_IF_ERROR(Name(plan_.name));
  if (!utf8::IsValid(plan_.description)) {
    return absl::InvalidArgumentError(
        "A description must contain valid UTF-8.");
  }
  for (const auto& field : plan_.fields) {
    ABSL_RETURN_IF_ERROR(Name(field.name));
    if (!utf8::IsValid(field.description) || !utf8::IsValid(field.pattern)) {
      return absl::InvalidArgumentError("Field text must contain valid UTF-8.");
    }
    if (!utf8::IsValid(field.declared) || !utf8::IsValid(field.type)) {
      return absl::InvalidArgumentError("Field types require valid UTF-8.");
    }
    if (field.has_default) {
      ABSL_RETURN_IF_ERROR(
          Expression::Literal(ConstantToJsonValue(field.default_value))
              .status());
    }
    for (const auto& allowed : field.enumeration) {
      ABSL_RETURN_IF_ERROR(
          Expression::Literal(ConstantToJsonValue(allowed)).status());
    }
    if (field.range.has_minimum) {
      ABSL_RETURN_IF_ERROR(
          Expression::Literal(ConstantToJsonValue(field.range.minimum))
              .status());
    }
    if (field.range.has_maximum) {
      ABSL_RETURN_IF_ERROR(
          Expression::Literal(ConstantToJsonValue(field.range.maximum))
              .status());
    }
    if (field.syntax_type.has_value()) {
      ABSL_RETURN_IF_ERROR(field.syntax_type->ToSource().status());
    }
  }
  auto source = DtoToFlow(plan_);
  ABSL_RETURN_IF_ERROR(Parsed(source));
  return source;
}

ModuleDocument::ModuleDocument(std::string identity, std::string source)
    : identity_(std::move(identity)), source_(std::move(source)) {}

std::vector<DocumentSymbolHandle> ModuleDocument::Symbols() const {
  auto parsed = Parse(source_);
  std::vector<DocumentSymbolHandle> symbols;
  absl::flat_hash_map<std::string, size_t> counts;
  const auto add = [&](std::string name, std::string kind,
                       syntax::Location span) {
    symbols.push_back({identity_, revision_, name, kind,
                       counts[absl::StrCat(kind, ":", name)]++, span.start,
                       span.end});
  };
  for (const auto& module : parsed.modules) {
    const auto text = std::string_view(source_).substr(
        module.extent.start, module.extent.end - module.extent.start);
    if (text.starts_with("mod") || text.starts_with("MOD")) {
      add(module.name.text, "module", module.extent);
    }
  }
  for (const auto& flow : parsed.flows) {
    add(flow->name.text, flow->interface_only ? "declaration" : "flow",
        flow->extent);
  }
  for (const auto& dto : parsed.dtos) {
    add(dto->name.text, "struct", dto->extent);
  }
  std::sort(symbols.begin(), symbols.end(),
            [](const auto& a, const auto& b) { return a.start < b.start; });
  return symbols;
}

absl::Status ModuleDocument::Validate(
    const DocumentSymbolHandle& symbol) const {
  if (symbol.document != identity_ || symbol.revision != revision_) {
    return absl::FailedPreconditionError(
        "The symbol handle belongs to a different document revision.");
  }
  for (const auto& current : Symbols()) {
    if (current.start == symbol.start && current.end == symbol.end &&
        current.kind == symbol.kind && current.name == symbol.name &&
        current.occurrence == symbol.occurrence) {
      return absl::OkStatus();
    }
  }
  return absl::NotFoundError("The symbol no longer exists.");
}

absl::StatusOr<DocumentChange> ModuleDocument::Apply(uint64_t revision,
                                                     std::vector<Edit> edits) {
  if (revision != revision_) {
    return absl::FailedPreconditionError("The document revision is stale.");
  }
  std::sort(edits.begin(), edits.end(),
            [](const Edit& a, const Edit& b) { return a.start < b.start; });
  size_t end = 0;
  bool previous = false;
  for (const auto& edit : edits) {
    if (edit.start > edit.end || edit.end > source_.size() ||
        edit.start < end ||
        (previous && edit.start == end && edit.start == edit.end)) {
      return absl::InvalidArgumentError(
          "Source edits overlap or exceed the document.");
    }
    previous = true;
    end = edit.end;
  }
  for (const auto& edit : edits) {
    if (!utf8::IsValid(edit.text) ||
        (edit.start < source_.size() &&
         utf8::IsContinuation(source_[edit.start])) ||
        (edit.end < source_.size() &&
         utf8::IsContinuation(source_[edit.end]))) {
      return absl::InvalidArgumentError(
          "An edit must preserve UTF-8 character boundaries.");
    }
  }
  std::string next = source_;
  for (auto i = edits.rbegin(); i != edits.rend(); ++i) {
    next.replace(i->start, i->end - i->start, i->text);
  }
  if (!next.empty()) {
    ABSL_RETURN_IF_ERROR(Parsed(next));
  }
  if (next != source_) {
    source_ = std::move(next);
    ++revision_;
  }
  return DocumentChange{revision_, std::move(edits), Check()};
}

absl::StatusOr<DocumentChange> ModuleDocument::Insert(const FlowBuilder& flow) {
  ABSL_ASSIGN_OR_RETURN(auto text, flow.Emit());
  return Apply(
      revision_,
      {{source_.size(), source_.size(),
        absl::StrCat(source_.empty() || source_.back() == '\n' ? "" : "\n",
                     text)}});
}

absl::StatusOr<DocumentChange> ModuleDocument::InsertStruct(
    const DtoPlan& shape) {
  StructBuilder builder(shape.name);
  builder.shape() = shape;
  ABSL_ASSIGN_OR_RETURN(auto text, builder.Emit());
  return Apply(
      revision_,
      {{source_.size(), source_.size(),
        absl::StrCat(source_.empty() || source_.back() == '\n' ? "" : "\n",
                     text)}});
}

absl::StatusOr<DocumentChange> ModuleDocument::UpsertDeclaration(
    const FlowPlan& interface, const std::vector<DtoPlan>& types,
    std::optional<DocumentSymbolHandle> symbol) {
  if (!interface.interface_only) {
    return absl::InvalidArgumentError("Expected an interface declaration.");
  }
  FlowPlan updated = interface;
  size_t start = source_.size(), end = start;
  const auto parsed = Parse(source_);
  if (symbol.has_value()) {
    ABSL_RETURN_IF_ERROR(Validate(*symbol));
    if (symbol->kind != "declaration" || symbol->name != interface.name) {
      return absl::InvalidArgumentError(
          "Upsert requires the owned interface declaration of this action.");
    }
    start = symbol->start;
    end = symbol->end;
    for (const auto& flow : parsed.flows) {
      if (flow->extent.start == start) {
        updated.exported = flow->exported;
      }
    }
  }
  ABSL_ASSIGN_OR_RETURN(auto text, DeclarationSource(updated));
  if (symbol.has_value() && !text.empty() && text.back() == '\n') {
    text.pop_back();
  }
  std::string shapes;
  const auto resolved = Resolve(source_, parsed);
  for (const auto& type : types) {
    if (const auto* existing = resolved.program.Dto(type.name);
        existing != nullptr) {
      auto a = DtoToJsonValue(*existing), b = DtoToJsonValue(type);
      a.erase("exported");
      b.erase("exported");
      if (a != b) {
        return absl::FailedPreconditionError(
            "A generated struct conflicts with an existing contribution; "
            "update its owned symbol explicitly.");
      }
    } else {
      StructBuilder builder(type.name);
      builder.shape() = type;
      ABSL_ASSIGN_OR_RETURN(auto source, builder.Emit());
      shapes += source + "\n";
    }
  }
  std::vector<Edit> edits;
  if (!shapes.empty()) {
    if (start == source_.size()) {
      text = shapes + text;
    } else {
      edits.push_back({source_.size(), source_.size(), "\n" + shapes});
    }
  }
  if (start == source_.size() && !source_.empty() && source_.back() != '\n') {
    text.insert(0, "\n");
  }
  if (symbol.has_value()) {
    ABSL_ASSIGN_OR_RETURN(
        auto maintained,
        MaintainTokens(std::string_view(source_).substr(start, end - start),
                       text, start));
    edits.insert(edits.end(), maintained.begin(), maintained.end());
  } else if (std::string_view(source_).substr(start, end - start) != text) {
    edits.push_back({start, end, std::move(text)});
  }
  return Apply(revision_, std::move(edits));
}

absl::StatusOr<DocumentChange> ModuleDocument::Update(
    const DocumentSymbolHandle& symbol, const FlowBuilder& flow) {
  ABSL_RETURN_IF_ERROR(Validate(symbol));
  if ((symbol.kind != "flow" && symbol.kind != "declaration") ||
      (symbol.kind == "declaration") != flow.interface().interface_only ||
      symbol.name != flow.interface().name) {
    return absl::InvalidArgumentError(
        "An update retains symbol name and declaration kind.");
  }
  auto kept = flow;
  auto parsed = Parse(source_);
  for (const auto& item : parsed.flows) {
    if (item->extent.start == symbol.start) {
      kept.interface().exported = item->exported;
    }
  }
  ABSL_ASSIGN_OR_RETURN(auto text, kept.Emit());
  if (!text.empty() && text.back() == '\n') {
    text.pop_back();
  }
  ABSL_ASSIGN_OR_RETURN(
      auto edits, MaintainTokens(std::string_view(source_).substr(
                                     symbol.start, symbol.end - symbol.start),
                                 text, symbol.start));
  return Apply(revision_, std::move(edits));
}

absl::StatusOr<DocumentChange> ModuleDocument::UpdateStruct(
    const DocumentSymbolHandle& symbol, const DtoPlan& shape) {
  ABSL_RETURN_IF_ERROR(Validate(symbol));
  if (symbol.kind != "struct" || symbol.name != shape.name) {
    return absl::InvalidArgumentError(
        "A struct update retains its name and kind.");
  }
  StructBuilder builder(shape.name);
  builder.shape() = shape;
  const auto parsed = Parse(source_);
  for (const auto& item : parsed.dtos) {
    if (item->extent.start == symbol.start) {
      builder.shape().exported = item->exported;
    }
  }
  ABSL_ASSIGN_OR_RETURN(auto text, builder.Emit());
  ABSL_ASSIGN_OR_RETURN(
      auto edits, MaintainTokens(std::string_view(source_).substr(
                                     symbol.start, symbol.end - symbol.start),
                                 text, symbol.start));
  return Apply(revision_, std::move(edits));
}

absl::StatusOr<DocumentChange> ModuleDocument::Remove(
    const DocumentSymbolHandle& symbol) {
  ABSL_RETURN_IF_ERROR(Validate(symbol));
  return Apply(revision_, {{symbol.start, symbol.end, ""}});
}

absl::StatusOr<DocumentChange> ModuleDocument::SetExported(
    const DocumentSymbolHandle& symbol, bool value) {
  ABSL_RETURN_IF_ERROR(Validate(symbol));
  const auto parsed = Parse(source_);
  size_t name_end = symbol.start;
  for (const auto& flow : parsed.flows) {
    if (flow->extent.start == symbol.start) {
      name_end = flow->name.location.end;
    }
  }
  for (const auto& shape : parsed.dtos) {
    if (shape->extent.start == symbol.start) {
      name_end = shape->name.location.end;
    }
  }
  auto lexed = Lex(std::string_view(source_).substr(symbol.start,
                                                    symbol.end - symbol.start));
  for (const auto& token : lexed.tokens) {
    if (symbol.start + token.start >= name_end && token.IsWord() &&
        vocabulary::Canonical(token.text) == "exported") {
      return value ? Apply(revision_, {})
                   : Apply(revision_, {{symbol.start + token.start,
                                        symbol.start + token.end, ""}});
    }
    if (token.kind == TokenKind::kLeftBrace) {
      return value
                 ? Apply(revision_, {{symbol.start + token.start,
                                      symbol.start + token.start, "exported "}})
                 : Apply(revision_, {});
    }
  }
  return absl::InvalidArgumentError("The declaration has no body.");
}

absl::StatusOr<DocumentChange> ModuleDocument::UpsertImport(
    std::string reference, std::string alias,
    std::optional<DocumentSymbolHandle> scope, std::string symbol) {
  ABSL_RETURN_IF_ERROR(Name(alias));
  if (!symbol.empty()) {
    ABSL_RETURN_IF_ERROR(Name(symbol, true));
  }
  auto parsed = Parse(source_);
  const std::vector<syntax::Import>* imports = &parsed.imports;
  size_t at = 0;
  if (scope.has_value()) {
    ABSL_RETURN_IF_ERROR(Validate(*scope));
    if (scope->kind != "flow" && scope->kind != "module") {
      return absl::InvalidArgumentError("Imports scope to a flow or a module.");
    }
    for (const auto& flow : parsed.flows) {
      if (flow->extent.start == scope->start) {
        imports = &flow->imports;
      }
    }
    auto lexed = Lex(std::string_view(source_).substr(
        scope->start, scope->end - scope->start));
    for (const auto& token : lexed.tokens) {
      if (token.kind == TokenKind::kLeftBrace) {
        at = scope->start + token.end;
        break;
      }
    }
  }
  if (!utf8::IsValid(reference)) {
    return absl::InvalidArgumentError(
        "An import reference must contain valid UTF-8.");
  }
  if (scope.has_value() && scope->kind == "module") {
    const auto local = std::find_if(
        parsed.modules.begin(), parsed.modules.end(), [&](const auto& module) {
          return module.extent.start == scope->start;
        });
    if (local != parsed.modules.end()) {
      at = source_.find('{', local->extent.start) + 1;
    }
  }
  const auto text =
      symbol.empty() ? absl::StrCat("import ", Quote(reference), " as ", alias)
                     : absl::StrCat("from ", reference, " import ", symbol,
                                    symbol == alias ? "" : " as ",
                                    symbol == alias ? "" : alias);
  for (const auto& item : *imports) {
    if (scope.has_value() && scope->kind == "module" &&
        item.scope != scope->name) {
      continue;
    }
    if (!scope.has_value() && !item.scope.empty()) {
      continue;
    }
    if (item.alias.text == alias) {
      return Apply(revision_, {{item.extent.start, item.extent.end, text}});
    }
    for (const auto& selected : item.symbols) {
      if (selected.alias.text == alias) {
        if (item.symbols.size() == 1) {
          return Apply(revision_, {{item.extent.start, item.extent.end, text}});
        }
        if (item.source != reference || symbol.empty()) {
          return absl::InvalidArgumentError(
              "Remove the selected symbol before changing its source or import "
              "kind.");
        }
        return Apply(
            revision_,
            {{selected.name.location.start, selected.alias.location.end,
              absl::StrCat(symbol, symbol == alias ? "" : " as ",
                           symbol == alias ? "" : alias)}});
      }
    }
  }
  return Apply(
      revision_,
      {{at, at, absl::StrCat(scope.has_value() ? "\n  " : "", text, "\n")}});
}

absl::StatusOr<DocumentChange> ModuleDocument::RemoveImport(
    std::string alias, std::optional<DocumentSymbolHandle> scope) {
  auto parsed = Parse(source_);
  const std::vector<syntax::Import>* imports = &parsed.imports;
  if (scope.has_value()) {
    ABSL_RETURN_IF_ERROR(Validate(*scope));
    for (const auto& flow : parsed.flows) {
      if (flow->extent.start == scope->start) {
        imports = &flow->imports;
      }
    }
  }
  for (const auto& item : *imports) {
    if (item.alias.text == alias) {
      return Apply(revision_, {{item.extent.start, item.extent.end, ""}});
    }
    for (size_t i = 0; i < item.symbols.size(); ++i) {
      if (item.symbols[i].alias.text != alias) {
        continue;
      }
      if (item.symbols.size() == 1) {
        return Apply(revision_, {{item.extent.start, item.extent.end, ""}});
      }
      const auto start = i == 0 ? item.symbols[i].name.location.start
                                : item.symbols[i - 1].alias.location.end;
      const auto end = i == 0 ? item.symbols[i + 1].name.location.start
                              : item.symbols[i].alias.location.end;
      return Apply(revision_, {{start, end, ""}});
    }
  }
  return absl::NotFoundError("No import under that alias.");
}

absl::StatusOr<DocumentChange> ModuleDocument::Rename(
    const DocumentSymbolHandle& symbol, std::string name) {
  ABSL_RETURN_IF_ERROR(Validate(symbol));
  ABSL_RETURN_IF_ERROR(Name(name));
  if (symbol.kind == "declaration") {
    return absl::InvalidArgumentError(
        "Renaming an external contract requires an explicit contract change.");
  }
  const auto parsed = Parse(source_);
  std::vector<Edit> edits;
  for (const auto& flow : parsed.flows) {
    if (flow->extent.start == symbol.start) {
      edits.push_back(
          {flow->name.location.start, flow->name.location.end, name});
    }
    syntax::VisitSubtree(*flow, [&](const syntax::Node& node) {
      if (const auto* call = syntax::As<syntax::CallExpression>(&node);
          call != nullptr && symbol.kind == "flow" &&
          call->action == symbol.name) {
        edits.push_back({call->action_location.start, call->action_location.end,
                         syntax::ActionSource(name)});
      }
    });
  }
  if (symbol.kind == "struct") {
    const auto change_type = [&](const auto& self,
                                 const syntax::TypeExpression& type) -> void {
      if (!type.quoted && type.name == symbol.name) {
        edits.push_back({type.location.start,
                         type.location.start + symbol.name.size(), name});
      }
      for (const auto& parameter : type.parameters) {
        self(self, parameter);
      }
    };
    for (const auto& dto : parsed.dtos) {
      if (dto->extent.start == symbol.start) {
        edits.push_back(
            {dto->name.location.start, dto->name.location.end, name});
      }
      for (const auto& field : dto->fields) {
        change_type(change_type, field->type);
      }
    }
    for (const auto& flow : parsed.flows) {
      syntax::VisitSubtree(*flow, [&](const syntax::Node& node) {
        if (const auto* port = syntax::As<syntax::PortDeclaration>(&node);
            port != nullptr) {
          change_type(change_type, port->type);
        }
        if (const auto* value = syntax::As<syntax::TypedValue>(&node);
            value != nullptr) {
          change_type(change_type, value->type);
        }
      });
    }
  }
  return Apply(revision_, std::move(edits));
}

absl::StatusOr<ModuleGraphChange> ModuleDocument::RenameInGraph(
    const DocumentSymbolHandle& symbol, std::string name,
    const ModuleSources& sources) const {
  ABSL_RETURN_IF_ERROR(Validate(symbol));
  ABSL_RETURN_IF_ERROR(Name(name));
  if (symbol.kind != "flow" && symbol.kind != "struct") {
    return absl::InvalidArgumentError(
        "External declarations require an explicit contract change.");
  }
  ModuleGraphChange change;
  change.sources = sources;
  change.sources.insert_or_assign(identity_, source_);
  const bool type = symbol.kind == "struct";
  for (const auto& [root, source] : change.sources) {
    auto linked = LinkModules(source, root, change.sources);
    if (linked.parsed.HasErrors()) {
      return absl::FailedPreconditionError(
          "The supplied graph must link before it can be renamed.");
    }
    std::string target;
    for (const auto& doc : linked.documents) {
      if (doc.identity != identity_) {
        continue;
      }
      if (type) {
        for (const auto& shape : linked.parsed.dtos) {
          if (shape->extent.start == doc.start + symbol.start) {
            target = shape->name.text;
          }
        }
      } else {
        for (const auto& flow : linked.parsed.flows) {
          if (flow->extent.start == doc.start + symbol.start) {
            target = flow->name.text;
          }
        }
      }
    }
    if (target.empty()) {
      continue;
    }
    const auto add = [&](syntax::Location location, std::string replacement) {
      for (const auto& doc : linked.documents) {
        if (doc.identity != root || location.start < doc.start ||
            location.end > doc.end) {
          continue;
        }
        change.edits[root].push_back({location.start - doc.start,
                                      location.end - doc.start,
                                      std::move(replacement)});
        return;
      }
    };

    struct DirectAlias {
      std::string name;
      size_t start;
      size_t end;
    };

    std::vector<DirectAlias> direct_aliases;
    const auto raw = Parse(source);
    const auto selections = [&](const std::vector<syntax::Import>& imports,
                                size_t start, size_t end, bool scoped) {
      for (const auto& import : imports) {
        for (const auto& selected : import.symbols) {
          const auto bound = std::find_if(
              linked.members.begin(), linked.members.end(),
              [&](const auto& member) {
                return !member.selection && member.scoped == scoped &&
                       member.name == selected.alias.text &&
                       member.target == target && member.scope_start == start &&
                       member.scope_end == end;
              });
          if (bound == linked.members.end()) {
            continue;
          }
          add(selected.name.location,
              absl::StrCat(name, selected.explicit_alias ? "" : " as ",
                           selected.explicit_alias ? "" : selected.alias.text));
          direct_aliases.push_back({selected.alias.text, start, end});
        }
      }
    };
    selections(raw.imports, 0, source.size(), false);
    for (const auto& flow : raw.flows) {
      selections(flow->imports, flow->extent.start, flow->extent.end, true);
    }
    const auto direct = [&](std::string_view spelling,
                            const syntax::Location& location) {
      return std::any_of(
          direct_aliases.begin(), direct_aliases.end(), [&](const auto& alias) {
            return spelling == alias.name && alias.start <= location.start &&
                   location.end <= alias.end;
          });
    };
    const auto change_type =
        [&](const auto& self,
            const syntax::TypeExpression& expression) -> void {
      if (!expression.quoted && expression.name == target) {
        for (const auto& doc : linked.documents) {
          if (doc.identity != root || expression.location.start < doc.start ||
              expression.location.end > doc.end) {
            continue;
          }
          const auto tokens = Lex(
              std::string_view(linked.source)
                  .substr(expression.location.start,
                          expression.location.end - expression.location.start));
          size_t last = expression.location.start;
          for (const auto& token : tokens.tokens) {
            if (token.kind == TokenKind::kLeftBracket) {
              break;
            }
            if (token.IsWord()) {
              last = expression.location.start + token.end;
            }
          }
          const auto spelling = std::string_view(linked.source)
                                    .substr(expression.location.start,
                                            last - expression.location.start);
          if (direct(spelling, expression.location)) {
            continue;
          }
          if (last >= expression.location.start + symbol.name.size()) {
            add({last - symbol.name.size(), last, 1, 1}, name);
          }
        }
      }
      for (const auto& parameter : expression.parameters) {
        self(self, parameter);
      }
    };
    for (const auto& flow : linked.parsed.flows) {
      if (!type && flow->name.text == target &&
          !direct(
              std::string_view(linked.source)
                  .substr(flow->name.location.start,
                          flow->name.location.end - flow->name.location.start),
              flow->name.location)) {
        add(flow->name.location, syntax::ActionSource(name));
      }
      syntax::VisitSubtree(*flow, [&](const syntax::Node& node) {
        if (!type) {
          if (const auto* call = syntax::As<syntax::CallExpression>(&node);
              call != nullptr && call->action == target) {
            const auto raw = std::string_view(linked.source)
                                 .substr(call->action_location.start,
                                         call->action_location.end -
                                             call->action_location.start);
            const auto lexed = Lex(raw);
            std::string spelling;
            for (const auto& token : lexed.tokens) {
              if (token.kind == TokenKind::kString) {
                const auto value =
                    nlohmann::json::parse(token.text, nullptr, false);
                if (value.is_string()) {
                  spelling = value.get<std::string>();
                }
              } else if (token.IsWord() || token.kind == TokenKind::kScope) {
                spelling += token.text;
              }
            }
            if (direct(spelling, call->action_location)) {
              return;
            }
            if (spelling.ends_with(symbol.name)) {
              spelling.replace(spelling.size() - symbol.name.size(),
                               symbol.name.size(), name);
            }
            add(call->action_location, Quote(spelling));
          }
        } else {
          if (const auto* port = syntax::As<syntax::PortDeclaration>(&node);
              port != nullptr) {
            change_type(change_type, port->type);
          }
          if (const auto* value = syntax::As<syntax::TypedValue>(&node);
              value != nullptr) {
            change_type(change_type, value->type);
          }
        }
      });
    }
    if (type) {
      for (const auto& shape : linked.parsed.dtos) {
        if (shape->name.text == target) {
          add(shape->name.location, name);
        }
        for (const auto& field : shape->fields) {
          change_type(change_type, field->type);
        }
      }
    }
  }
  for (auto& [identity, edits] : change.edits) {
    std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) {
      return std::tie(a.start, a.end, a.text) <
             std::tie(b.start, b.end, b.text);
    });
    edits.erase(std::unique(edits.begin(), edits.end(),
                            [](const auto& a, const auto& b) {
                              return a.start == b.start && a.end == b.end &&
                                     a.text == b.text;
                            }),
                edits.end());
    ModuleDocument validation(identity, change.sources.at(identity));
    ABSL_ASSIGN_OR_RETURN(auto result, validation.Apply(0, edits));
    change.sources[identity] = validation.source();
  }
  Diagnostic boundary;
  boundary.code = "flow.sequence.rename-graph-boundary";
  boundary.severity = Severity::kInformation;
  boundary.family = Family::kSequence;
  boundary.source = identity_;
  boundary.message =
      "Rename edits cover the supplied module graph. Update external users of "
      "an exported API separately; source aliases retain their wire identity.";
  change.diagnostics.push_back(std::move(boundary));
  return change;
}

absl::StatusOr<DocumentChange> ModuleDocument::RenameImport(
    std::string alias, std::string name,
    std::optional<DocumentSymbolHandle> scope) {
  ABSL_RETURN_IF_ERROR(Name(name));
  const auto parsed = Parse(source_);
  const std::vector<syntax::Import>* imports = &parsed.imports;
  const syntax::FlowDeclaration* owner = nullptr;
  if (scope.has_value()) {
    ABSL_RETURN_IF_ERROR(Validate(*scope));
    if (scope->kind != "flow" && scope->kind != "module") {
      return absl::InvalidArgumentError(
          "Imports belong to implemented flows or modules.");
    }
    for (const auto& flow : parsed.flows) {
      if (flow->extent.start == scope->start) {
        owner = flow.get();
        imports = &flow->imports;
      }
    }
  }
  const syntax::Import* target = nullptr;
  const syntax::ImportedSymbol* selected = nullptr;
  const std::string module_scope =
      scope.has_value() && scope->kind == "module" ? scope->name : "";
  for (const auto& import : *imports) {
    if (owner == nullptr && import.scope != module_scope) {
      continue;
    }
    for (const auto& symbol : import.symbols) {
      if (symbol.alias.text == name && name != alias) {
        return absl::AlreadyExistsError("The new local name is already bound.");
      }
      if (symbol.alias.text == alias) {
        target = &import;
        selected = &symbol;
      }
    }
    if (import.alias.text == name && name != alias) {
      return absl::AlreadyExistsError(
          "The new namespace alias is already bound.");
    }
    if (import.alias.text == alias) {
      target = &import;
    }
  }
  if (target == nullptr) {
    return absl::NotFoundError("No import under that alias.");
  }
  if (alias == name) {
    return Apply(revision_, {});
  }
  std::vector<Edit> edits;
  if (selected != nullptr) {
    edits.push_back(selected->explicit_alias
                        ? Edit{selected->alias.location.start,
                               selected->alias.location.end, name}
                        : Edit{selected->name.location.end,
                               selected->name.location.end,
                               absl::StrCat(" as ", name)});
  } else {
    edits.push_back(target->explicit_alias
                        ? Edit{target->alias.location.start,
                               target->alias.location.end, name}
                        : Edit{target->source_location.end,
                               target->source_location.end,
                               absl::StrCat(" as ", name)});
  }
  const auto matches = [&](std::string_view value) {
    return selected != nullptr
               ? value == alias
               : value == alias || value.starts_with(alias + "::");
  };
  const auto change_type = [&](const auto& self,
                               const syntax::TypeExpression& type) -> void {
    if (!type.quoted && matches(type.name)) {
      edits.push_back(
          {type.location.start, type.location.start + alias.size(), name});
    }
    for (const auto& parameter : type.parameters) {
      self(self, parameter);
    }
  };
  const auto visible_in = [&](std::string_view local) {
    if (!module_scope.empty() && local != module_scope &&
        !local.starts_with(module_scope + "::")) {
      return false;
    }
    for (const auto& item : parsed.imports) {
      if (item.scope == module_scope ||
          (local != item.scope && !local.starts_with(item.scope + "::"))) {
        continue;
      }
      if (item.alias.text == alias) {
        return false;
      }
      for (const auto& selected : item.symbols) {
        if (selected.alias.text == alias) {
          return false;
        }
      }
    }
    return true;
  };
  for (const auto& flow : parsed.flows) {
    if (!visible_in(flow->scope)) {
      continue;
    }
    if (owner != nullptr && flow.get() != owner) {
      continue;
    }
    if (owner == nullptr) {
      bool shadowed = false;
      for (const auto& import : flow->imports) {
        shadowed |= import.alias.text == alias;
        for (const auto& symbol : import.symbols) {
          shadowed |= symbol.alias.text == alias;
        }
      }
      if (shadowed) {
        continue;
      }
    }
    syntax::VisitSubtree(*flow, [&](const syntax::Node& node) {
      const auto* function = syntax::As<syntax::Builtin>(&node);
      const auto* zip = syntax::As<syntax::Zip>(&node);
      const auto qualified = function != nullptr ? function->name
                             : zip != nullptr    ? zip->name
                                                 : "";
      if (!qualified.empty() && matches(qualified)) {
        edits.push_back(
            {node.location.start, node.location.start + alias.size(), name});
      }
      if (const auto* call = syntax::As<syntax::CallExpression>(&node);
          call != nullptr && matches(call->action)) {
        auto action = name + call->action.substr(alias.size());
        edits.push_back({call->action_location.start, call->action_location.end,
                         syntax::ActionSource(action)});
      }
      if (const auto* port = syntax::As<syntax::PortDeclaration>(&node);
          port != nullptr) {
        change_type(change_type, port->type);
      }
      if (const auto* value = syntax::As<syntax::TypedValue>(&node);
          value != nullptr) {
        change_type(change_type, value->type);
      }
    });
  }
  if (owner == nullptr) {
    for (const auto& shape : parsed.dtos) {
      if (!visible_in(shape->scope)) {
        continue;
      }
      for (const auto& field : shape->fields) {
        change_type(change_type, field->type);
      }
    }
  }
  for (const auto& item : parsed.exports) {
    if (owner == nullptr && visible_in(item.scope) &&
        matches(item.symbol.text)) {
      edits.push_back({item.symbol.location.start,
                       item.symbol.location.start + alias.size(), name});
    }
  }
  return Apply(revision_, std::move(edits));
}

absl::StatusOr<FlowPlan> ModuleDocument::EffectiveSchema(
    std::string name, const ModuleSources& sources, ModuleResolver resolver,
    CompilationLimits limits) const {
  CompilationBudget budget(limits);
  auto linked = Link(sources, std::move(resolver), limits, &budget);
  const auto resolved =
      Resolve(linked.source, linked.parsed, false, nullptr, limits, &budget);
  if (const auto* error = resolved.FirstError(); error != nullptr) {
    return absl::FailedPreconditionError(error->message);
  }
  const FlowPlan* schema = resolved.program.Flow(name);
  {
    for (const auto& member : linked.members) {
      if (!member.selection && !member.type && !member.scoped &&
          member.name == name) {
        schema = resolved.program.Flow(member.target);
        break;
      }
    }
  }
  if (schema == nullptr) {
    return absl::NotFoundError("No callable under that name.");
  }
  return *schema;
}

LinkedModule ModuleDocument::Link(const ModuleSources& sources,
                                  ModuleResolver resolver,
                                  CompilationLimits limits,
                                  CompilationBudget* budget) const {
  return LinkModules(source_, identity_, sources, std::move(resolver), limits,
                     budget);
}

std::vector<Diagnostic> ModuleDocument::Check(const ModuleSources& sources,
                                              ModuleResolver resolver,
                                              CompilationLimits limits) const {
  if (source_.empty()) {
    return {};
  }
  CompilationBudget budget(limits);
  auto linked = Link(sources, std::move(resolver), limits, &budget);
  auto resolved =
      Resolve(linked.source, linked.parsed, false, nullptr, limits, &budget);
  for (auto& diagnostic : Inspect(linked.source, linked.parsed, resolved)) {
    resolved.diagnostics.push_back(std::move(diagnostic));
  }
  linked.Locate(resolved.diagnostics);
  SortDiagnostics(resolved.diagnostics);
  return std::move(resolved.diagnostics);
}
}  // namespace a11::flow

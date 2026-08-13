// Copyright 2026 The A11 Authors.

#include "a11/flow/generate.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_replace.h>
#include <absl/types/span.h>

#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

/// The statements that have a rule of their own in the grammar file.
///
/// `run`, `call` and `try` are followed by the name of an action, which is
/// coloured as one; `nodes` is followed by the name of a map. Each needs a
/// context that pushes, so none of them belongs in the flat list of statement
/// keywords -- and saying so here is what keeps a statement added to the
/// language out of the wrong rule.
constexpr std::array kOwnRule = {
    std::string_view("run"),
    std::string_view("call"),
    std::string_view("try"),
    std::string_view("nodes"),
};

/// The words that may follow a `=`, which is what makes the name before it a
/// bound step rather than a comparison. The statements that produce a value,
/// plus `node`, which is the declaration that does.
constexpr std::array kBindingVerbs = {
    std::string_view("run"),   std::string_view("call"),
    std::string_view("try"),   std::string_view("node"),
    std::string_view("wait"),  std::string_view("drain"),
};

/// `else` opens a block and so is written with the statements; `it` is a value
/// rather than a literal and is coloured as one. Named because both are taken
/// *out* of a table below.
constexpr std::string_view kElse = "else";
constexpr std::string_view kIt = "it";

/// A word in upper case, which is the other spelling of every keyword.
std::string Shout(std::string_view word) {
  std::string shouted(word);
  for (char& letter : shouted) {
    if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 32);
  }
  return shouted;
}

/// `a|b|A|B`: an alternation of words in both spellings, on one line.
std::string Inline(absl::Span<const std::string_view> words) {
  std::vector<std::string> parts;
  parts.reserve(words.size() * 2);
  for (const std::string_view word : words) parts.emplace_back(word);
  for (const std::string_view word : words) parts.push_back(Shout(word));
  return absl::StrJoin(parts, "|");
}

/// The same alternation wrapped over lines, each continuation starting with the
/// `|` -- which is how a long list of words stays readable in a YAML pattern.
///
/// `indent` is the whitespace each line after the first begins with; the first
/// line carries none, because it is placed after whatever opened it.
std::string Wrapped(absl::Span<const std::string_view> words,
                    std::string_view indent, size_t width = 70) {
  std::vector<std::string> parts;
  parts.reserve(words.size() * 2);
  for (const std::string_view word : words) parts.emplace_back(word);
  for (const std::string_view word : words) parts.push_back(Shout(word));
  std::string out;
  std::string line;
  for (size_t index = 0; index < parts.size(); ++index) {
    const std::string piece =
        index == 0 ? parts[index] : absl::StrCat("|", parts[index]);
    if (!line.empty() && indent.size() + line.size() + piece.size() > width) {
      absl::StrAppend(&out, line, "\n", indent);
      line.clear();
    }
    absl::StrAppend(&line, piece);
  }
  absl::StrAppend(&out, line);
  return out;
}

/// The words of a table that are not in `without`, in the table's order.
std::vector<std::string_view> Except(absl::Span<const std::string_view> words,
                                     absl::Span<const std::string_view> without) {
  std::vector<std::string_view> kept;
  for (const std::string_view word : words) {
    if (std::find(without.begin(), without.end(), word) == without.end()) {
      kept.push_back(word);
    }
  }
  return kept;
}

/// The words of a table, in the table's order, with any two-word entry split.
std::vector<std::string_view> Split(absl::Span<const std::string_view> words) {
  std::vector<std::string_view> parts;
  for (const std::string_view word : words) {
    size_t start = 0;
    while (start <= word.size()) {
      const size_t space = word.find(' ', start);
      const size_t end = space == std::string_view::npos ? word.size() : space;
      if (end > start) parts.push_back(word.substr(start, end - start));
      if (space == std::string_view::npos) break;
      start = space + 1;
    }
  }
  return parts;
}

/// The keyword statements: everything that opens a statement without taking a
/// name after it, plus the `else` that continues an `if`.
std::vector<std::string_view> StatementKeywords() {
  std::vector<std::string_view> words =
      Except(vocabulary::OrderedStatements(), kOwnRule);
  words.push_back("else");
  return words;
}

/// The duration units, longest spelling first: `250ms` is milliseconds, and a
/// regex that offered `m` before `ms` would read it as metres.
std::vector<std::string_view> DurationUnits() {
  std::vector<std::string_view> units;
  for (const std::string_view unit : vocabulary::DurationUnits()) {
    units.push_back(unit);
  }
  std::stable_sort(units.begin(), units.end(),
                   [](std::string_view left, std::string_view right) {
                     return left.size() > right.size();
                   });
  return units;
}

/// The word-shaped operators an expression may use.
///
/// `in` is both a port direction and the membership operator, so it is spelled
/// out here rather than taken from `OperatorWords` -- which is the set of words
/// that are *only* operators, and is what the highlighter consults in an order
/// where a declaration wins.
std::vector<std::string_view> OperatorWords() {
  std::vector<std::string_view> words;
  for (const std::string_view word : {"and", "or", "not"}) {
    if (vocabulary::OperatorWords().contains(word)) words.push_back(word);
  }
  words.push_back("in");
  return words;
}

/// One rule per port modifier, each scoped by the name it is.
std::string PortModifierRules() {
  std::string out;
  for (const std::string_view modifier : vocabulary::OrderedPortModifiers()) {
    absl::StrAppend(&out, "            - match: '\\b(", modifier, "|",
                    Shout(modifier), "){{kw_boundary}}'\n",
                    "              scope: keyword.modifier.", modifier,
                    ".a11flow\n");
  }
  // Trailing newline belongs to the template, not to the last rule.
  if (!out.empty()) out.pop_back();
  return out;
}

/// One rule per field modifier, each scoped by the name it is.
///
/// `one of` is two words with a space between them, so its pattern says so; the
/// scope it takes is the first word, since a scope name cannot hold one.
std::string FieldModifierRules() {
  std::string out;
  for (const std::string_view modifier : vocabulary::OrderedFieldModifiers()) {
    const size_t space = modifier.find(' ');
    const std::string pattern =
        space == std::string_view::npos
            ? absl::StrCat(modifier, "|", Shout(modifier))
            : absl::StrCat(modifier.substr(0, space), "\\s+",
                           modifier.substr(space + 1), "|",
                           Shout(modifier.substr(0, space)), "\\s+",
                           Shout(modifier.substr(space + 1)));
    const std::string_view scope =
        space == std::string_view::npos ? modifier : modifier.substr(0, space);
    absl::StrAppend(&out, "            - match: '\\b(", pattern,
                    "){{kw_boundary}}'\n", "              scope: keyword.modifier.",
                    scope, ".a11flow\n");
  }
  if (!out.empty()) out.pop_back();
  return out;
}

/// The Sublime grammar, with `@NAME@` where a list of words goes.
///
/// The structure is hand-written because it is a judgement about the language --
/// that `run` pushes a context expecting an action name, that a bare `then` is a
/// stage only with an operand after it -- and the words are not, because they are
/// a table that already exists.
constexpr std::string_view kSublimeTemplate = R"(%YAML 1.2
---
# Syntax highlighting for the A11 Flow language (see cpp/a11/flow/ in the A11
# repo).
#
# GENERATED FILE -- do not edit it by hand. It is written by
# `a11 flow syntax --target sublime --generate` (or `a11-flow syntax ...`) from
# the language's own word tables, and `--check` holds it to being up to date. A
# word added to the language reaches this file by running the generator; edited
# here, it would be overwritten and the drift would be silent.
#
# Install: copy this directory's files into
#   ~/Library/Application Support/Sublime Text/Packages/A11 Flow/   (macOS)
#   %APPDATA%\Sublime Text\Packages\A11 Flow\                       (Windows)
#   ~/.config/sublime-text/Packages/A11 Flow/                       (Linux)
#
# Every keyword may be written in lower case or UPPER CASE, but not Mixed, which
# is exactly what the patterns below accept: a word matches as a keyword only if
# it is uniformly cased, so `For` highlights as a name just as the compiler reads
# it as one.
name: A11 Flow
file_extensions: [flow]
scope: source.a11flow

variables:
  # A name: letters, digits, underscores, and dashes between word characters.
  name: '[A-Za-z_$][A-Za-z0-9_$]*(?:-[A-Za-z0-9_$]+)*'
  # `word` written all lower or all upper, and not part of a longer name.
  kw_boundary: '(?![A-Za-z0-9_$-])'

contexts:
  main:
    - include: comments
    - include: struct-declaration
    - include: flow-declaration
    - include: statements

  comments:
    - match: '#'
      scope: punctuation.definition.comment.a11flow
      push:
        - meta_scope: comment.line.number-sign.a11flow
        - match: $\n?
          pop: true

  # --- flow declarations ----------------------------------------------------

  flow-declaration:
    - match: '\b(@FLOW@){{kw_boundary}}'
      scope: keyword.declaration.flow.a11flow
      push:
        - meta_scope: meta.flow.a11flow
        - match: '{{name}}'
          scope: entity.name.function.flow.a11flow
          pop: true
        - match: '"'
          scope: punctuation.definition.string.begin.a11flow
          set:
            - meta_scope: entity.name.function.flow.a11flow
            - match: '"'
              scope: punctuation.definition.string.end.a11flow
              pop: true
        - match: '(?=\S)'
          pop: true

  # --- shape declarations -----------------------------------------------------

  # `struct Name { field: type ... }`. The name is a type everywhere else in the
  # file, so it is scoped as one here; the body is fields and nothing else,
  # which is why it is its own context rather than a use of `statements`.
  struct-declaration:
    - match: '\b(@STRUCT@){{kw_boundary}}'
      scope: keyword.declaration.struct.a11flow
      push:
        - meta_scope: meta.struct.a11flow
        - match: '{{name}}'
          scope: entity.name.type.struct.a11flow
          set:
            - match: '\{'
              scope: punctuation.section.block.begin.a11flow
              set:
                - meta_scope: meta.struct.body.a11flow
                - match: '\}'
                  scope: punctuation.section.block.end.a11flow
                  pop: true
                - include: comments
                - include: describe-declaration
                - include: field-declaration
            - match: '(?=\S)'
              pop: true
        - match: '(?=\S)'
          pop: true

  field-declaration:
    - match: '({{name}})(?=\s*:)'
      scope: variable.parameter.field.a11flow
      push:
        - meta_scope: meta.field.a11flow
        - match: ':'
          scope: punctuation.separator.a11flow
          set:
            - meta_scope: meta.field.a11flow
            # What a field says about itself after its type: whether it has to
            # be given, and what bounds the values it may hold.
@FIELD_MODIFIERS@
            - include: port-types
            - include: strings
            - include: literals
            # `1..200`: the range between two bounds.
            - match: '\.\.'
              scope: keyword.operator.range.a11flow
            - match: '[\[\]]'
              scope: punctuation.section.brackets.a11flow
            - match: ','
              scope: punctuation.separator.comma.a11flow
            - match: '$\n?'
              pop: true
        - match: '$\n?'
          pop: true

  # --- declarations inside a flow -------------------------------------------

  port-declaration:
    - match: '\b(@DIRECTIONS@){{kw_boundary}}(?=\s+{{name}}\s*:)'
      scope: keyword.declaration.port.a11flow
      push:
        - meta_scope: meta.port.a11flow
        - match: '{{name}}'
          scope: variable.parameter.port.a11flow
        - match: ':'
          scope: punctuation.separator.a11flow
          set:
            - meta_scope: meta.port.a11flow
            # What a port says about itself after its type: how many values it
            # carries, and whether it has to be given one.
@PORT_MODIFIERS@
            - include: port-types
            - include: strings
            # The brackets a generic type says what it holds in:
            # `list[string]`, `list[a11.NodeFragment]`.
            - match: '[\[\]]'
              scope: punctuation.section.brackets.a11flow
            - match: ','
              scope: punctuation.separator.comma.a11flow
            - match: '$\n?'
              pop: true
        - match: '$\n?'
          pop: true

  port-types:
    # A type a serialisation registry knows by tag, written unquoted and dotted
    # exactly as it was registered: `a11.sdk.AudioBuffer`. First, so a tag whose
    # first part happens to be a built-in name is still read as one tag.
    - match: '{{name}}(?:\.{{name}})+'
      scope: storage.type.tag.a11flow
    - match: |-
        (?x) \b (
          @TYPES@
        ) {{kw_boundary}}
      scope: storage.type.a11flow

  header-declaration:
    - match: '\b(@HEADER@){{kw_boundary}}'
      scope: keyword.declaration.header.a11flow
      push:
        - match: '\b(@HEADER_WORDS@){{kw_boundary}}'
          scope: keyword.other.a11flow
        - include: strings
        - include: literals
        - match: '{{name}}'
          scope: variable.other.header.a11flow
        - match: '$\n?'
          pop: true

  describe-declaration:
    - match: '\b(@DESCRIBE@){{kw_boundary}}'
      scope: keyword.other.describe.a11flow

  # --- statements ------------------------------------------------------------

  statements:
    - include: comments
    - include: port-declaration
    - include: header-declaration
    - include: describe-declaration
    - include: bindings
    - include: statement-keywords
    - include: call
    - include: node-expression
    - include: node-map
    - include: stages
    - include: bare-stages
    - include: operators
    - include: strings
    - include: literals
    - include: builtins
    - include: status-codes
    - include: expression-keywords
    - include: names

  bindings:
    # `x = call ...`, `x = node ...`, `x = wait ...`: the name being bound.
    - match: '({{name}})\s*(=)(?=\s*(?:@BINDING_VERBS@){{kw_boundary}})'
      captures:
        1: entity.name.constant.step.a11flow
        2: keyword.operator.assignment.a11flow
    # `state <- source`: what a repeat carries.
    - match: '({{name}})\s*(<-)'
      captures:
        1: variable.other.carry.a11flow
        2: keyword.operator.assignment.carry.a11flow

  statement-keywords:
    - match: |-
        (?x) \b (
          @STATEMENTS@
        ) {{kw_boundary}}
      scope: keyword.control.a11flow
    - match: '\b(@CLAUSES@){{kw_boundary}}'
      scope: keyword.other.a11flow

  node-map:
    - match: '\b(@NODES@){{kw_boundary}}'
      scope: keyword.declaration.nodes.a11flow
      push:
        - match: '{{name}}'
          scope: entity.name.namespace.nodemap.a11flow
          pop: true
        - match: '(?=\S)'
          pop: true

  node-expression:
    # Making a node takes parentheses -- `node()`, `node(id)` -- so the word is
    # the keyword only where one opens, and a port called `node` is a name.
    - match: '\b(@NODE@){{kw_boundary}}(?=\s*\()'
      scope: keyword.declaration.node.a11flow

  call:
    - match: '\b(@TRY@){{kw_boundary}}'
      scope: keyword.control.try.a11flow
    - match: '\b(@VERBS@){{kw_boundary}}'
      scope: keyword.other.call.a11flow
      push:
        - match: '({{name}})(?:(\.)({{name}}))*'
          scope: entity.name.function.action.a11flow
          pop: true
        - match: '"'
          scope: punctuation.definition.string.begin.a11flow
          set:
            - meta_scope: entity.name.function.action.a11flow
            - match: '"'
              scope: punctuation.definition.string.end.a11flow
              pop: true
        - match: '(?=\S)'
          pop: true
    - include: call-modifiers

  call-modifiers:
    # `headers` is only ever the second word of `forward headers "x-name"`.
    - match: |-
        (?x) \b (
          @MODIFIERS@
        ) {{kw_boundary}}
      scope: storage.modifier.a11flow

  # --- pipelines -------------------------------------------------------------

  stages:
    - match: '(\|)\s*'
      captures:
        1: keyword.operator.pipe.a11flow
      push:
        - match: |-
            (?x) \b (
              @STAGES@
            ) {{kw_boundary}}
          scope: support.function.stage.a11flow
          pop: true
        - match: '(?=\S)'
          pop: true

  # The stages that may be written without their leading `|` read as words
  # joining two things -- `history then asked`, `hits where it.ok`. Both take an
  # operand, which is what tells the stage from a port of the same name: a bare
  # one at the end of a statement, or in front of a `->`, is a name.
  bare-stages:
    - match: '\b(@BARE_STAGES@){{kw_boundary}}(?=[ \t]+(?:[A-Za-z_$"(\[{]|[0-9]|-[0-9]))'
      scope: support.function.stage.a11flow

  operators:
    - match: '->'
      scope: keyword.operator.into.a11flow
    - match: '<-'
      scope: keyword.operator.assignment.carry.a11flow
    - match: '=='
      scope: keyword.operator.comparison.a11flow
    - match: '!='
      scope: keyword.operator.comparison.a11flow
    - match: '<=|>=|<|>'
      scope: keyword.operator.comparison.a11flow
    - match: '='
      scope: keyword.operator.assignment.a11flow
    - match: '\|'
      scope: keyword.operator.pipe.a11flow
    - match: '[{}]'
      scope: punctuation.section.block.a11flow
    - match: '[()]'
      scope: punctuation.section.group.a11flow
    - match: '[\[\]]'
      scope: punctuation.section.brackets.a11flow
    - match: ':'
      scope: punctuation.separator.a11flow
    - match: ','
      scope: punctuation.separator.comma.a11flow
    - match: '\.'
      scope: punctuation.accessor.a11flow

  # --- expressions -----------------------------------------------------------

  expression-keywords:
    # `expr as TYPE`, and the type it names -- which may be a registry tag,
    # so it is read by shape rather than looked up in a list.
    - match: '\b(@AS@){{kw_boundary}}'
      scope: keyword.operator.cast.a11flow
      push:
        - match: '{{name}}(?:\.{{name}})*(?:\s*\[[^\]\n]*\])?'
          scope: storage.type.a11flow
          pop: true
        - include: strings
        - match: '(?=\S)'
          pop: true
    # `a11.sdk.Interaction{...}`: a value of a named type.
    - match: '{{name}}(?:\.{{name}})+(?=\s*\{)'
      scope: storage.type.a11flow
    - match: '\b(@SOURCE_WORDS@){{kw_boundary}}'
      scope: keyword.other.status.a11flow
    - match: '\b(@IT@){{kw_boundary}}'
      scope: variable.language.it.a11flow
    - match: '\b(@OPERATOR_WORDS@){{kw_boundary}}'
      scope: keyword.operator.logical.a11flow

  builtins:
    - match: |-
        (?x) \b (
          @BUILTINS@
        ) {{kw_boundary}} (?=\s*\()
      scope: support.function.builtin.a11flow

  status-codes:
    - match: |-
        (?x) \b (
          @STATUS_CODES@
        ) {{kw_boundary}}
      scope: constant.language.status-code.a11flow

  literals:
    - match: '\b(@CONSTANTS@){{kw_boundary}}'
      scope: constant.language.a11flow
    # A number with a duration unit: 500ns, 250ms, 30s, 5m, 1h. Longest first,
    # so `250ms` is milliseconds rather than a number of metres.
    - match: '\b-?[0-9]+(?:\.[0-9]+)?(@DURATION_UNITS@){{kw_boundary}}'
      scope: constant.numeric.duration.a11flow
    - match: '\b-?[0-9]+(?:\.[0-9]+)?\b'
      scope: constant.numeric.a11flow

  strings:
    # `"""..."""` first, so three quotes are not read as an empty string and a
    # quote. A line break inside one is content, which is the whole point of it.
    - match: '"""'
      scope: punctuation.definition.string.begin.a11flow
      push:
        - meta_scope: string.quoted.double.block.a11flow
        - match: '\\(?:[nrt"\\]|.)'
          scope: constant.character.escape.a11flow
        - match: '"""'
          scope: punctuation.definition.string.end.a11flow
          pop: true
    - match: '"'
      scope: punctuation.definition.string.begin.a11flow
      push:
        - meta_scope: string.quoted.double.a11flow
        - match: '\\(?:[nrt"\\]|.)'
          scope: constant.character.escape.a11flow
        - match: '"'
          scope: punctuation.definition.string.end.a11flow
          pop: true
        - match: '$\n?'
          scope: invalid.illegal.unterminated-string.a11flow
          pop: true

  names:
    # `x.port` -- a call's port, a node's id, a field of a value.
    - match: '({{name}})\s*(?=\.)'
      captures:
        1: variable.other.step.a11flow
    - match: '(?<=\.)({{name}})'
      captures:
        1: variable.other.member.a11flow
    - match: '{{name}}'
      scope: variable.other.a11flow
)";

/// One word as an alternation of its two spellings: `flow|FLOW`.
std::string One(std::string_view word) {
  const std::array<std::string_view, 1> only = {word};
  return Inline(absl::MakeConstSpan(only.data(), only.size()));
}

std::string Sublime() {
  const std::vector<std::string_view> statements = StatementKeywords();
  const std::vector<std::string_view> clauses =
      Except(vocabulary::OrderedClauseWords(), absl::MakeConstSpan(
                                                   &kElse, 1));
  const std::vector<std::string_view> modifiers =
      Split(vocabulary::OrderedModifiers());
  const std::vector<std::string_view> constants =
      Except(std::vector<std::string_view>{"true", "false", "null", "it"},
             absl::MakeConstSpan(&kIt, 1));
  const std::vector<std::string_view> units = DurationUnits();
  const std::vector<std::string_view> operators = OperatorWords();
  std::vector<std::string_view> bare;
  for (const std::string_view stage : vocabulary::Stages()) {
    if (vocabulary::BareStages().contains(stage)) bare.push_back(stage);
  }
  std::vector<std::string_view> sources;
  for (const std::string_view word : vocabulary::SourceWords()) {
    sources.push_back(word);
  }
  std::sort(sources.begin(), sources.end());

  return absl::StrReplaceAll(
      kSublimeTemplate,
      {
          {"@FLOW@", One("flow")},
          {"@STRUCT@", One("struct")},
          {"@DIRECTIONS@", Inline(std::vector<std::string_view>{"in", "out"})},
          {"@PORT_MODIFIERS@", PortModifierRules()},
          {"@FIELD_MODIFIERS@", FieldModifierRules()},
          {"@TYPES@", Wrapped(vocabulary::OrderedTypeNames(), "          ")},
          {"@HEADER@", One("header")},
          {"@HEADER_WORDS@",
           Inline(std::vector<std::string_view>{"as", "default"})},
          {"@DESCRIBE@", One("describe")},
          {"@BINDING_VERBS@",
           Inline(absl::MakeConstSpan(kBindingVerbs.data(),
                                      kBindingVerbs.size()))},
          {"@STATEMENTS@", Wrapped(statements, "          ")},
          {"@CLAUSES@", Inline(clauses)},
          {"@NODES@", One("nodes")},
          {"@NODE@", One("node")},
          {"@TRY@", One("try")},
          {"@VERBS@", Inline(std::vector<std::string_view>{"run", "call"})},
          {"@MODIFIERS@", Wrapped(modifiers, "          ")},
          {"@STAGES@", Wrapped(vocabulary::Stages(), "              ")},
          {"@BARE_STAGES@", Inline(bare)},
          {"@AS@", One("as")},
          {"@SOURCE_WORDS@", Inline(sources)},
          {"@IT@", One("it")},
          {"@OPERATOR_WORDS@", Inline(operators)},
          {"@BUILTINS@", Wrapped(vocabulary::OrderedBuiltins(), "          ")},
          {"@STATUS_CODES@", Wrapped(vocabulary::StatusCodes(), "          ")},
          {"@CONSTANTS@", Inline(constants)},
          {"@DURATION_UNITS@", absl::StrJoin(units, "|")},
      });
}

}  // namespace

absl::Span<const SyntaxTarget> SyntaxTargets() {
  static constexpr std::array kTargets = {SyntaxTarget::kSublime};
  return absl::MakeConstSpan(kTargets.data(), kTargets.size());
}

std::string_view SyntaxTargetName(SyntaxTarget target) {
  switch (target) {
    case SyntaxTarget::kSublime:
      return "sublime";
  }
  return "sublime";
}

bool SyntaxTargetFromName(std::string_view name, SyntaxTarget& target) {
  for (const SyntaxTarget candidate : SyntaxTargets()) {
    if (SyntaxTargetName(candidate) == name) {
      target = candidate;
      return true;
    }
  }
  return false;
}

std::string_view SyntaxTargetPath(SyntaxTarget target) {
  switch (target) {
    case SyntaxTarget::kSublime:
      return "editors/sublime-text/A11 Flow.sublime-syntax";
  }
  return "";
}

std::string GenerateSyntax(SyntaxTarget target) {
  switch (target) {
    case SyntaxTarget::kSublime:
      return Sublime();
  }
  return "";
}

}  // namespace a11::flow

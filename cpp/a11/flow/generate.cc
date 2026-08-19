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

/// `"a", "b", "c"`: the words as Python arguments, wrapped over lines.
///
/// Only the lower-case spelling is written. The generated `_keywords` makes the
/// shouted one, which keeps the file a list of the language's words rather than
/// a list of each of them twice.
std::string Quoted(absl::Span<const std::string_view> words,
                   std::string_view indent, size_t width = 74) {
  std::string out;
  std::string line;
  for (size_t index = 0; index < words.size(); ++index) {
    const std::string piece = absl::StrCat(
        "\"", words[index], "\"", index + 1 == words.size() ? "" : ",");
    if (!line.empty() &&
        indent.size() + line.size() + 1 + piece.size() > width) {
      absl::StrAppend(&out, line, "\n", indent);
      line.clear();
    }
    if (!line.empty()) absl::StrAppend(&line, " ");
    absl::StrAppend(&line, piece);
  }
  absl::StrAppend(&out, line);
  return out;
}

/// One word, quoted, for a rule that names a single keyword.
std::string QuotedOne(std::string_view word) {
  const std::array<std::string_view, 1> only = {word};
  return Quoted(absl::MakeConstSpan(only.data(), only.size()), "");
}

/// `_keywords("a", "b")`, on one line where it fits and wrapped where it does
/// not -- which is what keeps a two-word table from being written out as a
/// three-line call, and a table that grows past the margin from running off it.
std::string Call(absl::Span<const std::string_view> words,
                 std::string_view assigned_to) {
  const std::string one_line =
      absl::StrCat("_keywords(", Quoted(words, "", 1000), ")");
  if (assigned_to.size() + one_line.size() <= 79) return one_line;
  return absl::StrCat("_keywords(\n    ", Quoted(words, "    "), "\n)");
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

/// The Pygments lexer, with `@NAME@` where a list of words goes.
///
/// The same split as the Sublime template: the states and what pushes what are a
/// judgement about the language and are written here, and every word in them
/// comes from `vocabulary`. What differs is the audience -- this one colours
/// prose about flows rather than a file being edited, so it stops at what a
/// reader of a documentation page sees and leaves the error states out.
constexpr std::string_view kPygmentsTemplate = R"PY(# Copyright 2026 The A11 Authors.

"""Syntax highlighting for the A11 Flow language, as a Pygments lexer.

GENERATED FILE -- do not edit it by hand. It is written by
``a11 flow syntax --target pygments --generate`` (or ``a11-flow syntax ...``)
from the language's own word tables, and ``--check`` holds it to being up to
date. A
word added to the language reaches this file by running the generator; edited
here, it would be overwritten and the drift would be silent.

This is what colours the flows in A11's own documentation: MkDocs highlights a
fenced block with Pygments, and ``doc/hooks/flow_highlighting.py`` registers
this lexer under the alias ``a11flow``. Nothing about it is specific to that,
though -- it is an ordinary Pygments lexer, so Sphinx, ``pygmentize`` and a
static site of
your own can all read a ``.flow`` file through it.

Every keyword may be written in lower case or UPPER CASE, but not Mixed, which
is what ``_keywords`` builds the two spellings of: ``For`` highlights as a name
just as the compiler reads it as one.
"""

from pygments.lexer import RegexLexer, bygroups, default, include
from pygments.token import (
    Comment,
    Keyword,
    Name,
    Number,
    Operator,
    Punctuation,
    String,
    Text,
    Whitespace,
)

__all__ = ["A11FlowLexer"]

#: A name: letters, digits, underscores, and dashes between word characters.
NAME = r"[A-Za-z_$][A-Za-z0-9_$]*(?:-[A-Za-z0-9_$]+)*"

#: What must follow a keyword for it to be one, rather than the first part of a
#: longer name: ``in`` is a direction and ``inputs`` is not.
BOUNDARY = r"(?![A-Za-z0-9_$-])"

#: A dotted name -- a tag a serialisation registry knows a type by, or the type
#: of a value being built: ``a11.sdk.AudioBuffer``.
DOTTED = NAME + r"(?:\." + NAME + r")"


def _keywords(*names):
    """``flow|FLOW``: the two spellings of each word, as one alternation.

    A two-word entry keeps its space as ``\\s+``, so ``one of`` matches however
    it is spaced.
    """
    spellings = [*names, *(name.upper() for name in names)]
    return "|".join(name.replace(" ", r"\s+") for name in spellings)


def _group(alternation):
    """A whole-word group around an alternation: what a rule matches."""
    return r"\b(" + alternation + r")" + BOUNDARY


def _word(*names):
    """The pattern for the keywords named, in either spelling."""
    return _group(_keywords(*names))


#: The directions a port is declared in.
DIRECTIONS = @DIRECTIONS@

#: What a port says about itself after its type.
PORT_MODIFIERS = @PORT_MODIFIERS@

#: What a ``struct`` field says about itself after its type.
FIELD_MODIFIERS = @FIELD_MODIFIERS@

#: The built-in port type names.
TYPES = @TYPES@

#: What may follow a ``header``.
HEADER_WORDS = @HEADER_WORDS@

#: The statements that may follow a ``=``, which is what makes the name before
#: it a bound step rather than one side of a comparison.
BINDING_VERBS = @BINDING_VERBS@

#: Words that open a statement.
STATEMENTS = @STATEMENTS@

#: Words that stand inside a statement without opening one.
CLAUSES = @CLAUSES@

#: What may follow a call's closing parenthesis.
MODIFIERS = @MODIFIERS@

#: Every pipeline stage.
STAGES = @STAGES@

#: The stages that may be written without their leading ``|``.
BARE_STAGES = @BARE_STAGES@

#: Words that open a pipeline source rather than naming one.
SOURCE_WORDS = @SOURCE_WORDS@

#: Operators that are words.
OPERATOR_WORDS = @OPERATOR_WORDS@

#: The language's fixed function set. No user code, ever: a flow stays data.
BUILTINS = @BUILTINS@

#: The canonical status codes, which is what ``fail`` names.
STATUS_CODES = @STATUS_CODES@

#: Literals that are words.
CONSTANTS = @CONSTANTS@

#: Duration suffixes a number may carry, longest spelling first: a pattern that
#: offered ``m`` before ``ms`` would read ``250ms`` as a number of metres.
DURATION_UNITS = r"@DURATION_UNITS@"


class A11FlowLexer(RegexLexer):
    """Lexer for the A11 Flow language.

    A word of Flow means what its position says it means -- there are no
    reserved words -- so the states below are mostly about position: a stage
    only follows a ``|``, a type only follows a port's ``:``, a function is only
    a function where it is called, and whatever follows a ``.`` is a member
    however it is spelled.
    """

    name = "A11 Flow"
    url = "https://github.com/hpnkv/a11"
    aliases = ["a11flow", "a11-flow"]
    filenames = ["*.flow"]
    mimetypes = ["text/x-a11flow"]

    tokens = {
        "root": [
            (r"[^\S\n]+", Whitespace),
            (r"\n", Whitespace),
            include("comment"),
            include("string"),
            (_word(@FLOW@), Keyword.Declaration, "flow-name"),
            (_word(@STRUCT@), Keyword.Declaration, "struct-name"),
            # A port, told from the `in` of `x in y` by what follows it.
            (
                _group(DIRECTIONS) + r"(?=\s+" + NAME + r"\s*:)",
                Keyword.Declaration,
                "port",
            ),
            (_word(@HEADER@), Keyword.Declaration, "header"),
            (_word(@DESCRIBE@), Keyword.Declaration),
            (_word(@NODES@), Keyword.Declaration, "node-map"),
            # Making a node takes parentheses -- `node()`, `node(id)` -- so the
            # word is the keyword only where one opens, and a port called `node`
            # is a name.
            (_word(@NODE@) + r"(?=\s*\()", Keyword.Declaration),
            # `x = run ...`: the name before the `=` is the step being bound,
            # and a step is coloured the way it is coloured where it is used
            # again -- `mic` and the `mic` of `mic.audio` are the same thing.
            (
                r"(" + NAME + r")(\s*)(=)(?=\s*(?:" + BINDING_VERBS + r")"
                + BOUNDARY + r")",
                bygroups(Name.Variable, Whitespace, Operator),
            ),
            # `state <- source`: what a repeat carries.
            (
                r"(" + NAME + r")(\s*)(<-)",
                bygroups(Name.Variable, Whitespace, Operator),
            ),
            (_word(@TRY@), Keyword),
            (_word(@VERBS@), Keyword, "action-name"),
            # `via scratch` names a node map, so it is coloured as one -- the
            # same name the `nodes` that declared it was given.
            (
                r"(" + _keywords(@VIA@) + r")" + BOUNDARY + r"([^\S\n]+)("
                + NAME + r")",
                bygroups(Keyword.Reserved, Whitespace, Name.Namespace),
            ),
            (_group(MODIFIERS), Keyword.Reserved),
            (_group(STATEMENTS), Keyword),
            (_group(CLAUSES), Keyword),
            # A stage, which is what a word after a `|` is. The gap may hold a
            # line break: a long pipeline is written one stage to a line.
            (
                r"(\|)(\s*)(" + STAGES + r")" + BOUNDARY,
                bygroups(Operator, Whitespace, Name.Builtin.Pseudo),
            ),
            # The two stages that may be written without their `|` read as words
            # joining two things -- `history then asked`, `hits where it.ok`.
            # Both take an operand, which is what tells the stage from a port of
            # the same name.
            (
                _group(BARE_STAGES)
                + r"(?=[^\S\n]+(?:[A-Za-z_$\"(\[{]|[0-9]|-[0-9]))",
                Name.Builtin.Pseudo,
            ),
            (_word(@AS@), Keyword, "cast"),
            # `a11.sdk.Interaction{...}`: a value of a named type.
            (DOTTED + r"+(?=\s*\{)", Keyword.Type),
            (_group(SOURCE_WORDS), Keyword),
            (_word(@IT@), Name.Builtin.Pseudo),
            (_group(OPERATOR_WORDS), Operator.Word),
            (_group(BUILTINS) + r"(?=\s*\()", Name.Builtin),
            (_group(STATUS_CODES), Name.Constant),
            include("literal"),
            include("operator"),
            include("name"),
            (r".", Text),
        ],
        "comment": [
            (r"#[^\n]*", Comment.Single),
        ],
        "string": [
            # `"""..."""` first, so three quotes are not read as
            # an empty string and a quote. A line break inside one is content,
            # which is the whole point of it.
            (r'"""', String, "block-string"),
            (r'"', String, "quoted-string"),
        ],
        "block-string": [
            (r'\\.', String.Escape),
            (r'"""', String, "#pop"),
            (r'[^\\"]+', String),
            (r'"', String),
        ],
        "quoted-string": [
            (r'\\.', String.Escape),
            (r'"', String, "#pop"),
            (r'[^\\"\n]+', String),
            (r"\n", Whitespace, "#pop"),
        ],
        "flow-name": [
            (r"[^\S\n]+", Whitespace),
            (NAME, Name.Function, "#pop"),
            (r'"', String, ("#pop", "quoted-string")),
            default("#pop"),
        ],
        "struct-name": [
            (r"[^\S\n]+", Whitespace),
            (NAME, Name.Class, ("#pop", "struct-body")),
            default("#pop"),
        ],
        # A struct's body is fields and nothing else, which is why it is its own
        # state rather than a use of `root`.
        "struct-body": [
            (r"[^\S\n]+", Whitespace),
            (r"\n", Whitespace),
            (r"\{", Punctuation),
            (r"\}", Punctuation, "#pop"),
            include("comment"),
            (_word(@DESCRIBE@), Keyword.Declaration),
            include("string"),
            (
                r"(" + NAME + r")(\s*)(:)",
                bygroups(Name.Attribute, Whitespace, Punctuation),
                "field-type",
            ),
            (r".", Text),
        ],
        "field-type": [
            (r"[^\S\n]+", Whitespace),
            (_group(FIELD_MODIFIERS), Keyword.Pseudo),
            include("type"),
            include("string"),
            include("literal"),
            # `1..200`: the range between two bounds.
            (r"\.\.", Operator),
            (r"[\[\],]", Punctuation),
            (r"\n", Whitespace, "#pop"),
            (r".", Text),
        ],
        "port": [
            (r"[^\S\n]+", Whitespace),
            (NAME, Name.Attribute),
            (r":", Punctuation, ("#pop", "port-type")),
            default("#pop"),
        ],
        "port-type": [
            (r"[^\S\n]+", Whitespace),
            (_group(PORT_MODIFIERS), Keyword.Pseudo),
            include("type"),
            include("string"),
            # The brackets a generic type says what it holds in.
            (r"[\[\],]", Punctuation),
            (r"\n", Whitespace, "#pop"),
            (r".", Text),
        ],
        "header": [
            (r"[^\S\n]+", Whitespace),
            (_group(HEADER_WORDS), Keyword),
            include("string"),
            include("literal"),
            (NAME, Name.Variable),
            (r"\n", Whitespace, "#pop"),
            (r".", Text),
        ],
        "node-map": [
            (r"[^\S\n]+", Whitespace),
            (NAME, Name.Namespace, "#pop"),
            default("#pop"),
        ],
        "action-name": [
            (r"[^\S\n]+", Whitespace),
            (NAME + r"(?:\." + NAME + r")*", Name.Function, "#pop"),
            (r'"', String, ("#pop", "quoted-string")),
            default("#pop"),
        ],
        # `expr as TYPE`, and the type it names -- which may be a registry tag,
        # so it is read by shape rather than looked up in a list.
        "cast": [
            (r"[^\S\n]+", Whitespace),
            (
                NAME + r"(?:\." + NAME + r")*(?:\s*\[[^\]\n]*\])?",
                Keyword.Type,
                "#pop",
            ),
            (r'"', String, ("#pop", "quoted-string")),
            default("#pop"),
        ],
        "type": [
            # A tag first, so one whose first part happens to be a built-in name
            # is still read as the whole tag.
            (DOTTED + r"+", Keyword.Type),
            (_group(TYPES), Keyword.Type),
        ],
        "literal": [
            (_group(CONSTANTS), Keyword.Constant),
            (r"-?\d+(?:\.\d+)?(?:" + DURATION_UNITS + r")" + BOUNDARY, Number),
            (r"-?\d+(?:\.\d+)?", Number),
        ],
        "operator": [
            (r"->|<-", Operator),
            (r"==|!=|<=|>=", Operator),
            (r"\.\.", Operator),
            (r"[<>|=+*/%-]", Operator),
            (r"[{}()\[\]]", Punctuation),
            (r"[:,.]", Punctuation),
        ],
        "name": [
            # `x.port` -- a call's port, a node's id, a field of a value.
            (NAME + r"(?=\s*\.)", Name.Variable),
            (r"(?<=\.)" + NAME, Name.Attribute),
            (NAME, Name),
        ],
    }
)PY";

std::string Pygments() {
  const std::vector<std::string_view> statements = StatementKeywords();
  const std::vector<std::string_view> clauses =
      Except(vocabulary::OrderedClauseWords(), absl::MakeConstSpan(&kElse, 1));
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
      kPygmentsTemplate,
      {
          {"@FLOW@", QuotedOne("flow")},
          {"@STRUCT@", QuotedOne("struct")},
          {"@DIRECTIONS@",
           Call(std::vector<std::string_view>{"in", "out"}, "DIRECTIONS = ")},
          {"@PORT_MODIFIERS@", Call(vocabulary::OrderedPortModifiers(),
                                    "PORT_MODIFIERS = ")},
          {"@FIELD_MODIFIERS@", Call(vocabulary::OrderedFieldModifiers(),
                                     "FIELD_MODIFIERS = ")},
          {"@TYPES@", Call(vocabulary::OrderedTypeNames(), "TYPES = ")},
          {"@HEADER@", QuotedOne("header")},
          {"@HEADER_WORDS@",
           Call(std::vector<std::string_view>{"as", "default"},
                "HEADER_WORDS = ")},
          {"@DESCRIBE@", QuotedOne("describe")},
          {"@BINDING_VERBS@",
           Call(absl::MakeConstSpan(kBindingVerbs.data(), kBindingVerbs.size()),
                "BINDING_VERBS = ")},
          {"@STATEMENTS@", Call(statements, "STATEMENTS = ")},
          {"@CLAUSES@", Call(clauses, "CLAUSES = ")},
          {"@NODES@", QuotedOne("nodes")},
          {"@NODE@", QuotedOne("node")},
          {"@TRY@", QuotedOne("try")},
          {"@VERBS@",
           Quoted(std::vector<std::string_view>{"run", "call"}, "")},
          {"@VIA@", QuotedOne("via")},
          {"@MODIFIERS@", Call(modifiers, "MODIFIERS = ")},
          {"@STAGES@", Call(vocabulary::Stages(), "STAGES = ")},
          {"@BARE_STAGES@", Call(bare, "BARE_STAGES = ")},
          {"@AS@", QuotedOne("as")},
          {"@SOURCE_WORDS@", Call(sources, "SOURCE_WORDS = ")},
          {"@IT@", QuotedOne("it")},
          {"@OPERATOR_WORDS@", Call(operators, "OPERATOR_WORDS = ")},
          {"@BUILTINS@", Call(vocabulary::OrderedBuiltins(), "BUILTINS = ")},
          {"@STATUS_CODES@",
           Call(vocabulary::StatusCodes(), "STATUS_CODES = ")},
          {"@CONSTANTS@", Call(constants, "CONSTANTS = ")},
          {"@DURATION_UNITS@", absl::StrJoin(units, "|")},
      });
}

/// The TextMate grammar VSCode reads, with `@NAME@` where a word list goes.
///
/// **What this is for, and what it is not.** A VSCode extension with a language
/// server gets its real colours from *semantic tokens* -- the language's own
/// judgement about every token, the same `flow.tokens/v1` the IntelliJ plugin
/// draws from -- so this is what colours a `.flow` in the moment before the
/// server has answered, what colours one when no `a11-flow` binary exists for the
/// platform, and what colours a fragment inside a string. That is a real job and
/// a limited one: words, strings, numbers and marks.
///
/// So it deliberately does not try to be a parser. A TextMate grammar has
/// begin/end contexts and no name resolution, and a grammar that guessed at the
/// difference between a port and a node would be a second, worse implementation
/// of something the server does properly. Where a distinction needs the resolver,
/// this leaves the word plain and the semantic tokens paint over it.
///
/// The `@KEYWORDS@` lists carry both spellings, since every keyword may be
/// written in lower case or shouted.
constexpr std::string_view kVsCodeTemplate = R"JSON({
  "$schema": "https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json",
  "name": "A11 Flow",
  "scopeName": "source.a11flow",
  "//": "Generated by `a11 flow syntax --target vscode`. Do not edit: the word lists come from the language's own tables, and an edit here is a copy that falls behind. Semantic tokens from `a11-flow serve --protocol lsp` refine everything below.",
  "patterns": [{"include": "#flow"}],
  "repository": {
    "flow": {
      "patterns": [
        {"include": "#comment"},
        {"include": "#string"},
        {"include": "#duration"},
        {"include": "#number"},
        {"include": "#declaration-name"},
        {"include": "#stage"},
        {"include": "#builtin"},
        {"include": "#port-type"},
        {"include": "#declaration"},
        {"include": "#statement"},
        {"include": "#modifier"},
        {"include": "#status-code"},
        {"include": "#constant"},
        {"include": "#word-operator"},
        {"include": "#member"},
        {"include": "#flow-operator"},
        {"include": "#operator"},
        {"include": "#punctuation"}
      ]
    },
    "comment": {
      "name": "comment.line.number-sign.a11flow",
      "match": "#.*$"
    },
    "string": {
      "patterns": [
        {
          "//": "A tripled quote holds line breaks and gives back its indentation, which is how every description longer than a line is written.",
          "name": "string.quoted.triple.a11flow",
          "begin": "\"\"\"",
          "end": "\"\"\""
        },
        {
          "name": "string.quoted.double.a11flow",
          "begin": "\"",
          "end": "\"",
          "patterns": [{"name": "constant.character.escape.a11flow", "match": "\\\\."}]
        }
      ]
    },
    "duration": {
      "//": "Longest unit first: a pattern offering `m` before `ms` reads `250ms` as metres.",
      "name": "constant.numeric.duration.a11flow",
      "match": "\\b[0-9]+(?:\\.[0-9]+)?(?:@DURATION_UNITS@)\\b"
    },
    "number": {
      "name": "constant.numeric.a11flow",
      "match": "\\b[0-9]+(?:\\.[0-9]+)?\\b"
    },
    "declaration-name": {
      "//": "The name a `flow` or a `struct` declaration gives, which is what a caller dispatches.",
      "match": "\\b(@FLOW_OR_STRUCT@)\\s+([A-Za-z_][A-Za-z0-9_-]*)",
      "captures": {
        "1": {"name": "keyword.declaration.a11flow"},
        "2": {"name": "entity.name.type.a11flow"}
      }
    },
    "stage": {
      "//": "A word is a stage after a `|`, and the same word may be a function elsewhere.",
      "match": "(\\|)\\s*(@STAGES@)\\b",
      "captures": {
        "1": {"name": "keyword.operator.pipe.a11flow"},
        "2": {"name": "support.function.stage.a11flow"}
      }
    },
    "builtin": {
      "//": "A function only where it is called, which is what tells `text(it)` from the port type of the same name.",
      "match": "\\b(@BUILTINS@)(?=\\s*\\()",
      "name": "support.function.builtin.a11flow"
    },
    "port-type": {
      "//": "Past a port's or a field's `:` the rest of it is the type, dots and brackets included, so a registry tag needs no list to be in.",
      "begin": "\\b(@DIRECTIONS@)\\s+([A-Za-z_][A-Za-z0-9_-]*)\\s*(:)",
      "beginCaptures": {
        "1": {"name": "keyword.declaration.a11flow"},
        "2": {"name": "variable.parameter.a11flow"},
        "3": {"name": "punctuation.separator.a11flow"}
      },
      "end": "$",
      "patterns": [
        {"include": "#string"},
        {
          "match": "\\b(@PORT_AND_FIELD_MODIFIERS@)\\b",
          "name": "keyword.modifier.a11flow"
        },
        {
          "match": "\\b[A-Za-z_][A-Za-z0-9_.]*\\b",
          "name": "storage.type.a11flow"
        },
        {"include": "#punctuation"}
      ]
    },
    "declaration": {
      "match": "\\b(@DECLARATIONS@)\\b",
      "name": "keyword.declaration.a11flow"
    },
    "statement": {
      "match": "\\b(@STATEMENTS@)\\b",
      "name": "keyword.control.a11flow"
    },
    "modifier": {
      "match": "\\b(@MODIFIERS@)\\b",
      "name": "keyword.other.modifier.a11flow"
    },
    "status-code": {
      "match": "\\b(@STATUS_CODES@)\\b",
      "name": "constant.other.status.a11flow"
    },
    "constant": {
      "match": "\\b(@CONSTANTS@)\\b",
      "name": "constant.language.a11flow"
    },
    "word-operator": {
      "match": "\\b(@OPERATOR_WORDS@)\\b",
      "name": "keyword.operator.word.a11flow"
    },
    "member": {
      "//": "Whatever follows a `.` is a member however it is spelled, so `page.text` reads as a port rather than as the stage of the same name.",
      "match": "(?<=\\.)([A-Za-z_][A-Za-z0-9_-]*)",
      "name": "variable.other.member.a11flow"
    },
    "flow-operator": {
      "//": "Where a stream is going, which is the distinction worth its own colour.",
      "match": "->|<-|\\|",
      "name": "keyword.operator.flow.a11flow"
    },
    "operator": {
      "match": "==|!=|<=|>=|\\.\\.\\.|\\.\\.|[<>+=-]",
      "name": "keyword.operator.a11flow"
    },
    "punctuation": {
      "patterns": [
        {"match": "[{}]", "name": "punctuation.section.block.a11flow"},
        {"match": "[()]", "name": "punctuation.section.group.a11flow"},
        {"match": "[\\[\\]]", "name": "punctuation.section.brackets.a11flow"},
        {"match": "[:,.]", "name": "punctuation.separator.a11flow"}
      ]
    }
  }
}
)JSON";

/// The injection grammar: the same words, inside a host language's strings.
///
/// **Why a separate file.** VSCode injects one grammar into another by
/// `injectTo` on the injected grammar, not by a rule in the host's, so this is a
/// grammar of its own. And it answers a different question first: whether a
/// string is a flow at all. The rule is the one the IntelliJ injector uses -- a
/// string whose first real word is `flow`, followed by a name and a `{` -- so the
/// same fragment is recognised in both editors and prose merely mentioning a flow
/// is recognised in neither.
///
/// Only the multi-line quotes. A flow is at least a `flow x { .. }`, which does
/// not fit on one line of anybody's string, so injecting into every `"..."` in
/// every Python file would be a lot of matching for a case that does not arise.
constexpr std::string_view kVsCodeInjectionTemplate = R"JSON({
  "$schema": "https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json",
  "name": "A11 Flow in a string",
  "scopeName": "a11flow.injection",
  "//": "Generated by `a11 flow syntax --target vscode-injection`. Do not edit. Colours a flow written inside a host language's string literal, which is where most of them live: `flow.loads(\"\"\"...\"\"\")`. The decision is the content's, not the file's -- a string whose first real word is `flow NAME {` is one.",
  "injectionSelector": "L:string.quoted.docstring, L:string.quoted.multi, L:string.template, L:source.python string.quoted, L:source.ts, L:source.js, L:source.java, L:source.kotlin, L:source.go",
  "patterns": [{"include": "#fragment"}],
  "repository": {
    "fragment": {
      "patterns": [
        {
          "//": "A Python `\"\"\"..\"\"\"` or `'''..'''` holding a flow.",
          "begin": "(\"\"\"|''')(?=\\s*(?:#[^\\n]*\\n\\s*)*@FLOW@\\s+[A-Za-z_][A-Za-z0-9_-]*\\s*\\{)",
          "beginCaptures": {"1": {"name": "punctuation.definition.string.begin.a11flow"}},
          "end": "\\1",
          "endCaptures": {"0": {"name": "punctuation.definition.string.end.a11flow"}},
          "contentName": "meta.embedded.block.a11flow",
          "patterns": [{"include": "source.a11flow#flow"}]
        },
        {
          "//": "A TypeScript template literal holding one.",
          "begin": "(`)(?=\\s*(?:#[^\\n]*\\n\\s*)*@FLOW@\\s+[A-Za-z_][A-Za-z0-9_-]*\\s*\\{)",
          "beginCaptures": {"1": {"name": "punctuation.definition.string.begin.a11flow"}},
          "end": "`",
          "endCaptures": {"0": {"name": "punctuation.definition.string.end.a11flow"}},
          "contentName": "meta.embedded.block.a11flow",
          "patterns": [{"include": "source.a11flow#flow"}]
        }
      ]
    }
  }
}
)JSON";

std::string VsCode() {
  const std::vector<std::string_view> units = DurationUnits();
  // Every declaration word, and the two spellings of each. `in`/`out` are in the
  // list as well as in the port-type rule above it: the rule that pushes wins by
  // being tried first, and a bare `in` is still the declaration word.
  const std::vector<std::string_view> declarations =
      Split(vocabulary::OrderedDeclarations());
  std::vector<std::string_view> statements;
  for (const std::string_view word : vocabulary::OrderedStatements()) {
    statements.push_back(word);
  }
  for (const std::string_view word : vocabulary::OrderedClauseWords()) {
    statements.push_back(word);
  }
  for (const std::string_view word : vocabulary::SourceWords()) {
    statements.push_back(word);
  }
  std::sort(statements.begin(), statements.end());
  statements.erase(std::unique(statements.begin(), statements.end()),
                   statements.end());

  const std::vector<std::string_view> modifiers =
      Split(vocabulary::OrderedModifiers());
  std::vector<std::string_view> port_and_field =
      Split(vocabulary::OrderedPortModifiers());
  for (const std::string_view word : Split(vocabulary::OrderedFieldModifiers())) {
    if (std::find(port_and_field.begin(), port_and_field.end(), word) ==
        port_and_field.end()) {
      port_and_field.push_back(word);
    }
  }
  std::vector<std::string_view> constants;
  for (const std::string_view word : vocabulary::ConstantWords()) {
    constants.push_back(word);
  }
  std::sort(constants.begin(), constants.end());

  return absl::StrReplaceAll(
      kVsCodeTemplate,
      {
          {"@DURATION_UNITS@", absl::StrJoin(units, "|")},
          {"@FLOW_OR_STRUCT@",
           Inline(std::vector<std::string_view>{"flow", "struct"})},
          {"@DIRECTIONS@", Inline(std::vector<std::string_view>{"in", "out"})},
          {"@STAGES@", Inline(vocabulary::Stages())},
          {"@BUILTINS@", Inline(vocabulary::OrderedBuiltins())},
          {"@PORT_AND_FIELD_MODIFIERS@", Inline(port_and_field)},
          {"@DECLARATIONS@", Inline(declarations)},
          {"@STATEMENTS@", Inline(statements)},
          {"@MODIFIERS@", Inline(modifiers)},
          {"@STATUS_CODES@", Inline(vocabulary::StatusCodes())},
          {"@CONSTANTS@", Inline(constants)},
          {"@OPERATOR_WORDS@", Inline(OperatorWords())},
      });
}

std::string VsCodeInjection() {
  return absl::StrReplaceAll(kVsCodeInjectionTemplate,
                             {{"@FLOW@", One("flow")}});
}

}  // namespace

absl::Span<const SyntaxTarget> SyntaxTargets() {
  static constexpr std::array kTargets = {
      SyntaxTarget::kSublime, SyntaxTarget::kPygments, SyntaxTarget::kVsCode,
      SyntaxTarget::kVsCodeInjection};
  return absl::MakeConstSpan(kTargets.data(), kTargets.size());
}

std::string_view SyntaxTargetName(SyntaxTarget target) {
  switch (target) {
    case SyntaxTarget::kSublime:
      return "sublime";
    case SyntaxTarget::kPygments:
      return "pygments";
    case SyntaxTarget::kVsCode:
      return "vscode";
    case SyntaxTarget::kVsCodeInjection:
      return "vscode-injection";
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
    case SyntaxTarget::kPygments:
      return "editors/pygments/a11flow_lexer.py";
    case SyntaxTarget::kVsCode:
      return "editors/vscode/a11flow.tmLanguage.json";
    case SyntaxTarget::kVsCodeInjection:
      return "editors/vscode/a11flow-injection.tmLanguage.json";
  }
  return "";
}

std::string GenerateSyntax(SyntaxTarget target) {
  switch (target) {
    case SyntaxTarget::kSublime:
      return Sublime();
    case SyntaxTarget::kPygments:
      return Pygments();
    case SyntaxTarget::kVsCode:
      return VsCode();
    case SyntaxTarget::kVsCodeInjection:
      return VsCodeInjection();
  }
  return "";
}

}  // namespace a11::flow

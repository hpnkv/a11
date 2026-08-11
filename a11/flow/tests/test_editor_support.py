"""The editor syntax definitions have to know what the language knows.

A highlighter is a second, hand-written copy of the language's vocabulary, and a
second copy drifts. These tests read the word lists straight out of the editor
definitions -- the Sublime Text syntax under ``editors/`` and the IntelliJ
plugin's ``FlowVocabulary.kt`` -- and check them against the tables the parser
and resolver actually use, so adding a stage or a status code fails here until
every editor has been told.
"""

import pathlib
import re

import pytest

from a11.flow import plan
from a11.flow.lexer import DURATION_UNITS
from a11.flow.parser import (
    BARE_STAGES,
    BUILTINS,
    DECLARATION_WORDS,
    MODIFIER_WORDS,
    SOURCE_WORDS,
    STAGES,
    STATEMENT_WORDS,
)

ROOT = pathlib.Path(__file__).parents[3]

SUBLIME = ROOT / "editors" / "sublime-text" / "A11 Flow.sublime-syntax"

INTELLIJ = (
    ROOT
    / "intellij-plugin"
    / "src"
    / "main"
    / "kotlin"
    / "dev"
    / "curiositystack"
    / "a11"
    / "clion"
    / "flow"
    / "FlowVocabulary.kt"
)


@pytest.fixture(scope="module")
def sublime_syntax() -> str:
    if not SUBLIME.is_file():
        pytest.skip(f"{SUBLIME.name} is not in this checkout")
    return SUBLIME.read_text(encoding="utf-8")


def words_in(source: str, context: str) -> set[str]:
    """Every lower-case word listed under one context of the syntax file.

    The definitions spell each list twice, once per case, because a keyword is
    only a keyword when it is uniformly cased. Only the lower-case spellings are
    compared here; `test_every_keyword_is_offered_in_both_cases` checks that the
    upper-case half agrees.
    """
    block = _context_block(source, context)
    found: set[str] = set()
    for word in re.findall(r"[A-Za-z_][A-Za-z0-9_-]*", block):
        if word.islower() and word not in _YAML_NOISE:
            found.add(word)
    return found


def _context_block(source: str, context: str) -> str:
    lines = source.splitlines()
    start = next(
        (
            index
            for index, line in enumerate(lines)
            if line.strip() == f"{context}:"
        ),
        None,
    )
    assert start is not None, f"no {context!r} context in the syntax file"
    indent = len(lines[start]) - len(lines[start].lstrip())
    block: list[str] = []
    for line in lines[start + 1 :]:
        if line.strip() and (len(line) - len(line.lstrip())) <= indent:
            break
        block.append(line)
    return "\n".join(block)


#: Words that are part of the syntax-definition format rather than the language.
_YAML_NOISE = {
    "match",
    "scope",
    "captures",
    "push",
    "pop",
    "set",
    "include",
    "meta",
    "kw",
    "boundary",
    "name",
    "true",
    "x",
    "a11flow",
    "keyword",
    "support",
    "function",
    "stage",
    "constant",
    "language",
    "status",
    "code",
    "storage",
    "modifier",
    "control",
    "other",
    "declaration",
    "variable",
    "entity",
    "punctuation",
    "operator",
    "type",
    "builtin",
    "logical",
    "try",
    "call",
}


def test_the_stages_are_all_highlighted(sublime_syntax: str):
    listed = words_in(sublime_syntax, "stages")
    assert set(STAGES) <= listed, set(STAGES) - listed


def test_the_stages_that_go_without_a_pipe_are_highlighted_there_too(
    sublime_syntax: str,
):
    """A stage written without its `|` is still a stage to look at."""
    listed = words_in(sublime_syntax, "bare-stages")
    assert set(BARE_STAGES) <= listed, set(BARE_STAGES) - listed
    assert set(BARE_STAGES) <= set(STAGES)


def test_the_builtin_functions_are_all_highlighted(sublime_syntax: str):
    listed = words_in(sublime_syntax, "builtins")
    assert set(BUILTINS) <= listed, set(BUILTINS) - listed


def test_the_status_codes_are_all_highlighted(sublime_syntax: str):
    listed = words_in(sublime_syntax, "status-codes")
    assert set(plan.STATUS_CODES) <= listed, set(plan.STATUS_CODES) - listed


def test_the_port_types_are_all_highlighted(sublime_syntax: str):
    listed = words_in(sublime_syntax, "port-types")
    assert set(plan.TYPE_NAMES) <= listed, set(plan.TYPE_NAMES) - listed


def test_the_statement_and_modifier_words_are_all_highlighted(
    sublime_syntax: str,
):
    listed = set(re.findall(r"[a-z][a-z0-9_-]*", sublime_syntax))
    expected = (
        STATEMENT_WORDS | MODIFIER_WORDS | SOURCE_WORDS | DECLARATION_WORDS
    )
    missing = expected - listed
    assert not missing, missing


def test_every_keyword_is_offered_in_both_cases(sublime_syntax: str):
    """A keyword the language accepts in upper case must highlight in it too."""
    for context, words in (
        ("stages", set(STAGES)),
        ("builtins", set(BUILTINS)),
        ("status-codes", set(plan.STATUS_CODES)),
        ("port-types", set(plan.TYPE_NAMES)),
    ):
        block = _context_block(sublime_syntax, context)
        shouted = {
            word for word in re.findall(r"[A-Z][A-Z0-9_-]*", block)
        }
        missing = {word.upper() for word in words} - shouted
        assert not missing, f"{context}: {missing}"


def test_the_flow_files_in_the_examples_are_the_ones_highlighted():
    """The extension the syntax claims is the one the examples use."""
    assert 'file_extensions: [flow]' in SUBLIME.read_text(encoding="utf-8")
    examples = ROOT / "examples" / "003-flow-dsl"
    assert list(examples.glob("*.flow"))


# --- The IntelliJ plugin ------------------------------------------------------


@pytest.fixture(scope="module")
def intellij_vocabulary() -> dict[str, set[str]]:
    """The `setOf(...)` tables the plugin's lexer classifies words with."""
    if not INTELLIJ.is_file():
        pytest.skip(f"{INTELLIJ.name} is not in this checkout")
    source = INTELLIJ.read_text(encoding="utf-8")
    found: dict[str, set[str]] = {}
    for name, body in re.findall(
        r"val\s+([A-Z_]+)\s*=\s*setOf\((.*?)\)", source, re.S
    ):
        found[name] = set(re.findall(r'"([^"]+)"', body))
    assert found, "no word lists in FlowVocabulary.kt"
    return found


@pytest.mark.parametrize(
    ("table", "expected"),
    [
        ("STAGE_WORDS", set(STAGES)),
        ("BARE_STAGE_WORDS", set(BARE_STAGES)),
        ("BUILTIN_WORDS", set(BUILTINS)),
        ("TYPE_WORDS", set(plan.TYPE_NAMES)),
        ("STATUS_CODES", set(plan.STATUS_CODES)),
        ("MODIFIER_WORDS", set(MODIFIER_WORDS)),
        ("DURATION_UNITS", set(DURATION_UNITS)),
    ],
)
def test_intellij_knows_exactly_these_words(
    intellij_vocabulary, table: str, expected: set[str]
):
    """Tables where the editor knows the language's words and no others."""
    assert intellij_vocabulary[table] == expected


def test_intellij_knows_every_word_that_opens_something(intellij_vocabulary):
    """Every word the parser gives a position to is a keyword to the editor.

    Which of the two buckets it lands in is the editor's business -- `nodes`
    declares a node map and reads like a declaration, while the parser files it
    with the statements it opens -- so the check is against both. The editor
    also knows a few the parser never tables, like ``parallel`` and ``else``,
    which it reads in place.
    """
    known = (
        intellij_vocabulary["DECLARATION_WORDS"]
        | intellij_vocabulary["STATEMENT_WORDS"]
    )
    expected = (
        set(DECLARATION_WORDS) | set(STATEMENT_WORDS) | set(SOURCE_WORDS)
    )
    assert expected <= known, expected - known


def test_intellij_canonicalises_case_the_way_the_compiler_does():
    """The plugin's `canonical` has to be the compiler's rule, not a variant."""
    source = INTELLIJ.read_text(encoding="utf-8")
    assert "isUpperCase" in source and "isLowerCase" in source
    assert "lowercase()" in source


def test_the_plugin_registers_the_language_id_the_docs_tell_people_to_use():
    """`# language=A11Flow` only works if that is the id that was registered."""
    plugin_xml = (
        ROOT
        / "intellij-plugin"
        / "src"
        / "main"
        / "resources"
        / "META-INF"
        / "plugin.xml"
    )
    if not plugin_xml.is_file():
        pytest.skip("the IntelliJ plugin is not in this checkout")
    registered = plugin_xml.read_text(encoding="utf-8")
    assert 'language="A11Flow"' in registered
    assert 'extensions="flow"' in registered
    # Flows in string literals are what the injector is for.
    assert "FlowInjector" in registered

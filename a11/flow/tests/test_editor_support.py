"""The editors do not know the language: they ask it.

This file used to be long. It read the word lists out of the Sublime syntax and
out of the plugin's `FlowVocabulary.kt` and compared them with the parser's own
tables, because both were hand-written copies and a copy drifts. Neither is a
copy any more -- the Sublime grammar is generated from the one table
(`a11 flow syntax`), and the plugin runs `a11-flow` and asks -- so what is left to
check is that this stays true.
"""

import pathlib
import re

import pytest

ROOT = pathlib.Path(__file__).parents[3]

SUBLIME = ROOT / "editors" / "sublime-text" / "A11 Flow.sublime-syntax"

PYGMENTS = ROOT / "editors" / "pygments" / "a11flow_lexer.py"

PLUGIN = ROOT / "intellij-plugin"

FLOW_PLUGIN_SOURCE = (
    PLUGIN / "src" / "main" / "kotlin" / "dev" / "curiositystack" / "a11"
    / "clion" / "flow"
)


def test_the_sublime_syntax_is_what_the_language_generates():
    """The check `a11 flow syntax` runs, from here, over the checked-in file.

    A generated file that nobody regenerated is the only way the last static
    grammar can fall behind, so this is the drift test that replaced a dozen
    word-list comparisons.
    """
    from a11._native import flow as native_flow

    if not SUBLIME.is_file():
        pytest.skip(f"{SUBLIME.name} is not in this checkout")
    generated = native_flow.syntax("sublime")
    assert generated["path"] == "editors/sublime-text/A11 Flow.sublime-syntax"
    assert SUBLIME.read_text(encoding="utf-8") == generated["text"], (
        "the Sublime syntax is out of date -- run"
        " `a11 flow syntax --target sublime --generate`"
    )


def test_the_pygments_lexer_is_what_the_language_generates():
    """The same check for the lexer that colours the documentation.

    A page of prose is the one surface where a word the highlighter has never
    heard of looks like ordinary text rather than like a mistake, so nothing but
    this notices when the lexer falls behind.
    """
    from a11._native import flow as native_flow

    if not PYGMENTS.is_file():
        pytest.skip(f"{PYGMENTS.name} is not in this checkout")
    generated = native_flow.syntax("pygments")
    assert generated["path"] == "editors/pygments/a11flow_lexer.py"
    assert PYGMENTS.read_text(encoding="utf-8") == generated["text"], (
        "the Pygments lexer is out of date -- run"
        " `a11 flow syntax --target pygments --generate`"
    )


def test_the_pygments_lexer_colours_every_flow_in_the_repository():
    """It is a static grammar, so this is what says it still reads the language.

    The staleness check above compares it with what the generator writes; this
    runs it. A rule that stopped matching -- a state that no longer pops, a
    pattern the language outgrew -- leaves `Error` tokens behind, and every
    ``.flow`` file in the checkout is the corpus that finds them.
    """
    pytest.importorskip("pygments")
    if not PYGMENTS.is_file():
        pytest.skip(f"{PYGMENTS.name} is not in this checkout")

    import importlib.util

    from pygments.token import Error

    spec = importlib.util.spec_from_file_location("a11flow_lexer", PYGMENTS)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    lexer = module.A11FlowLexer()

    flows = [
        path
        for path in ROOT.glob("**/*.flow")
        if "build" not in path.parts and ".venv" not in path.parts
    ]
    assert flows, "no .flow files to read"
    for path in flows:
        source = path.read_text(encoding="utf-8")
        unknown = [
            text for kind, text in lexer.get_tokens(source) if kind is Error
        ]
        assert not unknown, f"{path.name}: the lexer chokes on {unknown[:5]}"


def test_the_generated_syntax_says_it_is_generated():
    """Anybody who opens one has to be told not to edit it."""
    for generated in (SUBLIME, PYGMENTS):
        if not generated.is_file():
            pytest.skip(f"{generated.name} is not in this checkout")
        assert "GENERATED FILE" in generated.read_text(encoding="utf-8")
    text = SUBLIME.read_text(encoding="utf-8")
    # And the extension it claims is the one the examples use.
    assert "file_extensions: [flow]" in text
    assert list((ROOT / "examples" / "003-flow-dsl").glob("*.flow"))


def test_the_plugin_keeps_no_language_knowledge_of_its_own():
    """The deletion that was the point of the port, held in place.

    A lexer, a parser, a resolver, an inspector and five word lists lived in
    Kotlin, and every word the language gained had to be taught to them. If any of
    it comes back, this fails: whatever an editor needs to know about the language
    it asks `a11-flow` for.
    """
    if not FLOW_PLUGIN_SOURCE.is_dir():
        pytest.skip("the IntelliJ plugin is not in this checkout")
    assert not (FLOW_PLUGIN_SOURCE / "FlowVocabulary.kt").exists()
    assert not (FLOW_PLUGIN_SOURCE / "analysis").exists()

    # No word lists anywhere in it either: the words are the language's.
    from a11._native import flow as native_flow

    stages = set(native_flow.vocabulary()["stages"])
    for source in FLOW_PLUGIN_SOURCE.glob("*.kt"):
        text = source.read_text(encoding="utf-8")
        quoted = set(re.findall(r'"([a-z][a-z0-9_-]*)"', text))
        # A handful of stage names would be a table; the semantic *kinds* the
        # payload uses are not stage names, so this is a real signal.
        assert len(quoted & stages) < 3, (
            f"{source.name} looks like it carries a list of stages"
        )


def test_python_keeps_no_language_implementation_of_its_own():
    """The other half of the same deletion.

    A lexer, a parser, a syntax tree, a resolver, a value model and a runtime
    lived in `a11/flow/`, and while they did, every language change had to be
    made twice and three parity harnesses existed to notice when it had not
    been. They are gone: what is left in `a11/flow/` is glue over
    `a11._native.flow`.
    """
    package = ROOT / "a11" / "flow"
    for gone in ("lexer.py", "parser.py", "syntax.py", "values.py"):
        assert not (package / gone).exists(), f"a11/flow/{gone} is back"
    for gone in (
        "test_native_parity.py",
        "test_parser_parity.py",
        "test_resolve_parity.py",
    ):
        assert not (package / "tests" / gone).exists(), (
            f"{gone} is back, which means a second implementation is too"
        )
    # And the glue is glue: it asks the native module rather than deciding.
    for module in ("plan.py", "runtime.py"):
        text = (package / module).read_text(encoding="utf-8")
        assert "_native" in text, f"a11/flow/{module} stopped asking natively"


def test_the_plugin_registers_the_language_id_the_docs_tell_people_to_use():
    """`# language=A11Flow` only works if that is the id that was registered."""
    plugin_xml = PLUGIN / "src" / "main" / "resources" / "META-INF" / "plugin.xml"
    if not plugin_xml.is_file():
        pytest.skip("the IntelliJ plugin is not in this checkout")
    registered = plugin_xml.read_text(encoding="utf-8")
    assert 'language="A11Flow"' in registered
    assert 'extensions="flow"' in registered
    # Flows in string literals are what the injector is for.
    assert "multiHostInjector" in registered
    # And the language is asked rather than reimplemented: one annotator over the
    # tool, no inspections over a resolver of the plugin's own.
    assert "externalAnnotator" in registered
    assert "localInspection" not in registered

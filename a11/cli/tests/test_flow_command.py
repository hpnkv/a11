# Copyright 2026 The A11 Authors.

"""``a11 flow``: the command every external toolchain drives the language through.

Exit codes and output shapes are the contract here -- a pre-commit hook reads the
exit code, a CI annotator reads the SARIF, an editor reads the JSON envelope -- so
they are checked directly rather than through whichever engine happens to be
behind them.
"""

from __future__ import annotations

import asyncio
import contextlib
import io
import json
import pathlib
import sys

import pytest

from a11.cli.app import build_parser
from a11.cli.commands import COMMANDS
from a11.flow import diagnostics as diag

ROOT = pathlib.Path(__file__).parents[3]
EXAMPLES = ROOT / "examples" / "003-flow-dsl"

GOOD = """
flow shout {
  in  words:   string stream
  out loudest: string

  say = run text-upper(text: words)
  say.upper | first 1 -> loudest
}
"""

BROKEN = """
flow shout {
  in  words:   string stream
  out loudest: string

  words | flatten -> loudest
}
"""


def run(*argv: str, stdin: str | None = None) -> int:
    """Drive the CLI the way `main` does, without its process-level setup.

    ``stdin`` stands in for a pipe, which is how the subcommands that read `-`
    and the one that answers requests are driven.
    """
    parser = build_parser(COMMANDS)
    args = parser.parse_args(argv)
    if stdin is None:
        return asyncio.run(args._command.run(args))
    with _standard_input(stdin):
        return asyncio.run(args._command.run(args))


@contextlib.contextmanager
def _standard_input(text: str):
    """`sys.stdin` as this text, for as long as the block runs."""
    held = sys.stdin
    sys.stdin = io.StringIO(text)
    try:
        yield
    finally:
        sys.stdin = held


@pytest.fixture
def flow_file(tmp_path: pathlib.Path):
    def write(source: str, name: str = "t.flow") -> pathlib.Path:
        path = tmp_path / name
        path.write_text(source, encoding="utf-8")
        return path

    return write


def test_the_command_is_registered_under_its_own_name():
    assert any(command.name == "flow" for command in COMMANDS)


def test_a_flow_that_compiles_is_reported_as_fine(flow_file, capsys):
    assert run("flow", "check", str(flow_file(GOOD))) == 0
    assert "no problems found" in capsys.readouterr().out


def test_quiet_says_nothing_about_the_files_that_are_fine(flow_file, capsys):
    assert run("flow", "check", "--quiet", str(flow_file(GOOD))) == 0
    assert capsys.readouterr().out == ""


def test_a_flow_that_does_not_compile_exits_one(flow_file, capsys):
    assert run("flow", "check", str(flow_file(BROKEN))) == 1
    printed = capsys.readouterr().out
    # path:line:column: severity: message [code] -- the shape editors parse.
    assert ":6:11: error:" in printed
    assert "flatten" in printed
    # The precise code, from the native parser: a stage the language does not
    # have is not merely "unexpected text".
    assert "[flow.form.unknown-stage]" in printed


def test_a_file_that_cannot_be_read_exits_two(capsys):
    assert run("flow", "check", "/nonexistent/x.flow") == 2
    assert "cannot read" in capsys.readouterr().err


def test_the_json_envelope_is_the_documented_one(flow_file, capsys):
    assert run("flow", "check", "--format", "json", str(flow_file(BROKEN))) == 1
    envelope = json.loads(capsys.readouterr().out)
    assert envelope["format"] == diag.DIAGNOSTICS_FORMAT
    assert envelope["counts"]["error"] == 1
    found = envelope["diagnostics"][0]
    assert found["severity"] == "error"
    assert found["family"] == "form"
    assert found["range"]["start"]["line"] == 6
    # Offsets place the problem without needing the file again.
    text = pathlib.Path(envelope["source"]).read_text(encoding="utf-8")
    start = found["range"]["start"]["offset"]
    assert text[start : start + 7] == "flatten"


def test_sarif_is_a_log_a_scanner_can_read(flow_file, capsys):
    assert run("flow", "check", "--format", "sarif", str(flow_file(BROKEN))) == 1
    log = json.loads(capsys.readouterr().out)
    assert log["version"] == "2.1.0"
    run_entry = log["runs"][0]
    assert run_entry["results"][0]["level"] == "error"
    assert len(run_entry["tool"]["driver"]["rules"]) == len(diag.known_codes())


def test_several_files_are_all_checked_and_the_worst_wins(flow_file, capsys):
    good = flow_file(GOOD, "good.flow")
    broken = flow_file(BROKEN, "broken.flow")
    assert run("flow", "check", str(good), str(broken)) == 1
    printed = capsys.readouterr().out
    assert "good.flow: no problems found" in printed
    assert "broken.flow:6:11: error:" in printed


def test_standard_input_is_a_file_called_dash(capsys):
    assert run("flow", "check", "-", stdin=BROKEN) == 1
    printed = capsys.readouterr().out
    # No path to print, so the line starts at the position.
    assert printed.startswith("6:11: error:")


def test_the_shipped_flows_all_check_clean(capsys):
    files = sorted(str(path) for path in EXAMPLES.glob("*.flow"))
    assert files, "no example flows in the repository"
    assert run("flow", "check", "--quiet", *files) == 0
    assert capsys.readouterr().out == ""


def test_describe_prints_the_ports_and_steps_it_resolved(capsys):
    assert run("flow", "describe", str(EXAMPLES / "research.flow")) == 0
    printed = capsys.readouterr().out
    assert "flow research" in printed
    assert "input  question: string (one value, required)" in printed
    assert "output sources: string (stream)" in printed
    assert "nodes  fetched" in printed


def test_describe_as_json_is_the_plan_envelope(capsys):
    assert run("flow", "describe", "--format", "json", str(EXAMPLES / "research.flow")) == 0
    envelope = json.loads(capsys.readouterr().out)
    assert envelope["format"] == diag.PLAN_FORMAT
    assert envelope["flows"][0]["flow"] == "research"


def test_describe_of_a_broken_flow_reports_it_and_exits_one(flow_file, capsys):
    assert run("flow", "describe", str(flow_file(BROKEN))) == 1
    assert "error:" in capsys.readouterr().err


def test_check_reports_what_a_flow_does_that_it_did_not_mean_to(flow_file, capsys):
    """The half of `check` that is not the compiler.

    Every one of these compiles and runs, so the exit code stays 0: they are the
    things a reader would point at, and a build that failed on them would be a
    build nobody could green.
    """
    source = """
flow careless {
  in  words:   string stream
  out loudest: string
  out forgotten: string

  say = try run text-upper(text: words)
  say.upper | collect | drop 2 -> loudest
}
"""
    assert run("flow", "check", str(flow_file(source))) == 0
    printed = capsys.readouterr().out
    assert "[flow.unused.try-status]" in printed
    assert "[flow.unused.output-port]" in printed
    assert "[flow.sequence.impossible]" in printed
    # A warning is not an error, and the line says which it is.
    assert "error:" not in printed


def test_check_counts_the_severities_for_a_gate(flow_file, capsys):
    source = """
flow careless {
  in  words:   string stream
  out loudest: string

  say = try run text-upper(text: words)
  say.upper | first 1 -> loudest
}
"""
    assert run("flow", "check", "--format", "json", str(flow_file(source))) == 0
    envelope = json.loads(capsys.readouterr().out)
    assert envelope["counts"] == {
        "error": 0,
        "warning": 0,
        "weak-warning": 1,
        "information": 0,
    }
    found = envelope["diagnostics"][0]
    assert found["code"] == "flow.unused.try-status"
    assert found["family"] == "unused"
    assert found["flow"] == "careless"


def test_parse_prints_the_tree_it_read(capsys):
    assert run("flow", "parse", str(EXAMPLES / "research.flow")) == 0
    printed = capsys.readouterr().out
    assert "flow research" in printed
    assert "port question inputs" in printed
    assert "stage truncate" in printed


def test_parse_of_a_broken_flow_still_gives_a_tree(flow_file, capsys):
    """The difference the recovering parser exists for.

    A file with a mistake in it exits 1 and *still* prints what was read -- which
    is what an editor needs, and what the Python reference could never give.
    """
    assert run("flow", "parse", "--format", "json", str(flow_file(BROKEN))) == 1
    envelope = json.loads(capsys.readouterr().out)
    assert envelope["format"] == diag.SYNTAX_FORMAT
    assert envelope["diagnostics"][0]["code"] == "flow.form.unknown-stage"
    flow = envelope["flows"][0]
    assert flow["name"] == "shout"
    # Both ports and the statement that has the mistake in it.
    assert len(flow["ports"]) == 2
    assert [one["kind"] for one in flow["body"]] == ["pipe"]


UNFORMATTED = "flow shout{\nin words:string stream\nout loudest:string\nwords|first 1->loudest\n}\n"

FORMATTED = (
    "flow shout {\n"
    "  in  words:   string stream\n"
    "  out loudest: string\n"
    "  words | first 1 -> loudest\n"
    "}\n"
)


def test_fmt_prints_the_formatted_file(flow_file, capsys):
    assert run("flow", "fmt", str(flow_file(UNFORMATTED))) == 0
    assert capsys.readouterr().out == FORMATTED


def test_fmt_in_place_rewrites_the_file_and_says_so(flow_file, capsys):
    path = flow_file(UNFORMATTED)
    assert run("flow", "fmt", "-i", str(path)) == 1
    assert path.read_text(encoding="utf-8") == FORMATTED
    assert "reformatted" in capsys.readouterr().out
    # And again, now that there is nothing to do.
    assert run("flow", "fmt", "-i", str(path)) == 0


def test_fmt_check_says_what_would_change_without_writing_it(flow_file, capsys):
    path = flow_file(UNFORMATTED)
    assert run("flow", "fmt", "--check", str(path)) == 1
    assert "would be reformatted" in capsys.readouterr().out
    assert path.read_text(encoding="utf-8") == UNFORMATTED


def test_fmt_leaves_a_file_it_cannot_parse_alone(flow_file, capsys):
    path = flow_file(BROKEN)
    assert run("flow", "fmt", "-i", str(path)) == 2
    assert path.read_text(encoding="utf-8") == BROKEN
    assert "error:" in capsys.readouterr().err


def test_fmt_as_json_is_the_format_envelope(flow_file, capsys):
    assert run("flow", "fmt", "--format", "json", str(flow_file(UNFORMATTED))) == 1
    envelope = json.loads(capsys.readouterr().out)
    assert envelope["format"] == "flow.format/v1"
    assert envelope["formatted"] == FORMATTED
    assert envelope["changed"] is True
    # One edit, trimmed to what differs, so an editor keeps the cursor.
    edit = envelope["edits"][0]
    applied = (
        UNFORMATTED[: edit["start"]] + edit["text"] + UNFORMATTED[edit["end"] :]
    )
    assert applied == FORMATTED


def test_every_flow_this_repository_ships_is_formatted():
    """The corpus is the style's own documentation, so it is held to it."""
    files = sorted(str(path) for path in EXAMPLES.glob("*.flow"))
    files += sorted(str(path) for path in (ROOT / "scripts").glob("*.flow"))
    assert run("flow", "fmt", "--check", "--quiet", *files) == 0


def test_codes_lists_the_published_table(capsys):
    assert run("flow", "codes") == 0
    printed = capsys.readouterr().out
    assert "flow.unused.try-status" in printed
    assert len(printed.strip().splitlines()) == len(diag.known_codes())


def test_codes_as_json_is_the_codes_envelope(capsys):
    assert run("flow", "codes", "--format", "json") == 0
    envelope = json.loads(capsys.readouterr().out)
    assert envelope["format"] == diag.CODES_FORMAT
    assert len(envelope["codes"]) == len(diag.known_codes())


def test_a_bare_flow_command_prints_help(capsys):
    assert run("flow") == 2
    assert "subcommand" in capsys.readouterr().out


def test_highlight_labels_every_token_with_what_it_means(capsys):
    """The one classifier, through the CLI: an editor reads this, not its own."""
    assert run("flow", "highlight", str(EXAMPLES / "research.flow")) == 0
    printed = capsys.readouterr().out
    assert "declaration-keyword  flow" in printed
    assert "flow-name            research" in printed
    # A word is a stage after a `|` and a type past a port's `:`.
    assert "stage                truncate" in printed
    assert "type                 string" in printed


def test_highlight_as_json_is_the_tokens_envelope(capsys):
    assert run("flow", "highlight", "--format", "json", str(EXAMPLES / "research.flow")) == 0
    envelope = json.loads(capsys.readouterr().out)
    assert envelope["format"] == diag.TOKENS_FORMAT
    source = (EXAMPLES / "research.flow").read_text(encoding="utf-8")
    # Offsets tile the source, which is what lets a client colour from this alone.
    for token in envelope["tokens"]:
        assert token["start"] < token["end"] <= len(source)


def test_syntax_says_the_generated_definitions_are_current(capsys):
    """The check that replaces a drift test per editor.

    The definition is generated from the native word tables, so this is a diff
    rather than a judgement: a word the language gains and nobody regenerated is
    an out-of-date file, wherever that editor lives.
    """
    assert run("flow", "syntax", "--root", str(ROOT)) == 0
    assert "up to date" in capsys.readouterr().out


def test_syntax_reports_a_definition_that_has_fallen_behind(tmp_path, capsys):
    """One that is not what the language would write fails, and says what to run."""
    stale = tmp_path / "editors" / "sublime-text"
    stale.mkdir(parents=True)
    (stale / "A11 Flow.sublime-syntax").write_text(
        "match: first|last\n", encoding="utf-8"
    )
    assert run("flow", "syntax", "--target", "sublime", "--root", str(tmp_path)) == 1
    reported = capsys.readouterr().err
    assert "out of date" in reported
    assert "--generate" in reported


def test_syntax_generate_writes_what_the_language_says(tmp_path, capsys):
    assert run(
        "flow", "syntax", "--target", "sublime", "--generate", "--root", str(tmp_path)
    ) == 0
    written = (tmp_path / "editors" / "sublime-text" / "A11 Flow.sublime-syntax")
    assert written.is_file()
    # Every stage, in both spellings, and the marker that says not to edit it.
    text = written.read_text(encoding="utf-8")
    assert "GENERATED FILE" in text
    assert "truncate" in text and "TRUNCATE" in text
    # And running the check against what was just written passes.
    assert run("flow", "syntax", "--target", "sublime", "--root", str(tmp_path)) == 0


def test_complete_offers_what_the_language_allows_there(capsys):
    """The one completion, through the command line."""
    source = "flow t {\n  in q: string\n  q | "
    assert run("flow", "complete", "-", "--offset", str(len(source)),
               stdin=source) == 0
    printed = capsys.readouterr().out
    assert "stage" in printed
    assert "truncate" in printed
    # Nothing that is not a stage.
    assert "len" not in printed


def test_complete_as_json_is_the_completions_envelope(capsys):
    source = "flow t {\n  in q: string\n  q | trun"
    assert run("flow", "complete", "-", "--format", "json",
               "--offset", str(len(source)), stdin=source) == 0
    envelope = json.loads(capsys.readouterr().out)
    assert envelope["format"] == "flow.completions/v1"
    assert envelope["prefix"] == "trun"
    assert any(one["name"] == "truncate" for one in envelope["proposals"])


def test_serve_answers_one_request_per_line(capsys):
    """The protocol every frontend talks, over a pipe."""
    requests = (
        '{"id": 1, "method": "check", "source": "flow t { }"}\n'
        '{"id": 2, "method": "nonsense"}\n'
    )
    assert run("flow", "serve", stdin=requests) == 0
    answers = [json.loads(line) for line in capsys.readouterr().out.splitlines()]
    assert answers[0]["id"] == 1
    assert answers[0]["ok"] is True
    assert answers[0]["result"]["format"] == diag.DIAGNOSTICS_FORMAT
    # A method that does not exist is a bad request, not a crash.
    assert answers[1]["ok"] is False
    assert "not a method" in answers[1]["error"]["message"]


def test_run_runs_a_flow_and_prints_its_ports(capsys):
    source = (
        "flow shout {\n"
        "  in  words: string stream required\n"
        "  out loud:  string\n"
        "  words | collect | map upper(join(it, \", \")) -> loud\n"
        "}\n"
    )
    assert run("flow", "run", "-", "--input", 'words=["hi","there"]',
               stdin=source) == 0
    assert "loud: HI, THERE" in capsys.readouterr().out


def test_run_of_a_flow_that_is_not_there_exits_two(capsys):
    assert run("flow", "run", "-", "--flow", "missing",
               stdin="flow t { }\n") == 2
    assert "no flow named" in capsys.readouterr().err

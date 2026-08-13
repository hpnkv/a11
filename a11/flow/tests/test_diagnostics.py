# Copyright 2026 The A11 Authors.

"""The diagnostic shape and the formats external toolchains read.

These are contract tests: the envelopes are what an editor, a CI annotator and a
future VSCode extension parse, so their field names and their meanings are pinned
here and by `testdata/flow/codes.json`. The C++ half of the same contract is
checked by `cpp/tests/flow_emit_json_test.cc`, against the same golden table.
"""

from __future__ import annotations

import json
import pathlib

import dataclasses

import pytest

from a11.flow import diagnostics as diag

ROOT = pathlib.Path(__file__).parents[3]
CODES_JSON = ROOT / "testdata" / "flow" / "codes.json"


def one(code: str = "flow.unused.header") -> diag.Diagnostic:
    return diag.Diagnostic(
        code=code,
        message="Nothing uses 'deadline'.",
        range=diag.Range(diag.Position(40, 3, 12), diag.Position(48, 3, 20)),
        severity=diag.Severity.WEAK_WARNING,
        family=diag.Family.UNUSED,
        flow="research",
        fixes=(diag.Fix("Remove the header declaration", (diag.Edit(30, 60, ""),)),),
    )


# --- the line index -----------------------------------------------------------


def test_lines_and_columns_start_at_one():
    """The convention the lexer has always reported and errors have printed."""
    index = diag.LineIndex("flow t {\n  in q: string\n}\n")
    assert (index.at(0).line, index.at(0).column) == (1, 1)
    assert (index.at(4).line, index.at(4).column) == (1, 5)
    # The newline belongs to the line it ends.
    assert index.at(8).line == 1
    assert (index.at(9).line, index.at(9).column) == (2, 1)
    assert index.at(11).column == 3
    assert index.line_count == 4


def test_the_index_clamps_past_the_end():
    source = "flow t {}"
    index = diag.LineIndex(source)
    past = index.at(len(source) + 100)
    assert past.offset == len(source)
    assert past.line == 1


def test_an_offset_and_a_line_column_are_the_same_place():
    source = "flow t {\n  in q: string\n  q | text -> a\n}\n"
    index = diag.LineIndex(source)
    for offset in range(len(source) + 1):
        position = index.at(offset)
        assert index.offset_of(position.line, position.column) == offset


def test_the_index_counts_bytes_and_says_so():
    """The distinction every ASCII test file hides, and the one that mattered.

    An offset in this language is a byte offset; a Python ``str`` is indexed by
    code point. They agree until the first character outside ASCII, at which point
    an index built over the string disagrees with the native compiler about where
    every later token is -- which is exactly how the IntelliJ plugin came to colour
    `suggest-fixes.flow` a column to the left of itself.
    """
    source = 'flow t {\n  describe "a \u00a7 and an \U0001f642"\n  in q: string\n}\n'
    index = diag.LineIndex(source)
    assert index.length == len(source.encode("utf-8")) > len(source)
    # The `in` of line three, found the way the compiler reports it.
    offset = index.offset_of(3, 3)
    assert source.encode("utf-8")[offset : offset + 2] == b"in"
    # And back again, in the units each side counts in: bytes for the offset,
    # code points for the column.
    position = index.at(offset)
    assert (position.line, position.column) == (3, 3)
    assert index.characters_of(offset) < offset


def test_an_offset_and_a_line_column_agree_outside_ascii_too():
    source = 'flow t {\n  describe "\u00a7\U0001f642"\n  q | text -> a\n}\n'
    index = diag.LineIndex(source)
    data = source.encode("utf-8")
    for offset in range(len(data) + 1):
        # Only character boundaries are positions anything reports.
        if offset < len(data) and data[offset] & 0xC0 == 0x80:
            continue
        position = index.at(offset)
        assert index.offset_of(position.line, position.column) == offset


def test_the_index_agrees_with_the_lexer_about_where_a_token_is():
    """What makes a compiler error placeable: the lexer reports line and column,
    and everything else works in offsets."""
    from a11._native import flow as native

    source = "flow t {\n  in q: string\n  q | truncate 20 -> a\n}\n"
    index = diag.LineIndex(source)
    for token in native.tokenize(source)["tokens"]:
        if token["kind"] in ("newline", "end"):
            continue
        offset = index.offset_of(token["line"], token["column"])
        assert source[offset : offset + len(token["text"])] == token["text"]


# --- the published code table -------------------------------------------------


def test_every_code_is_published_with_a_meaning():
    codes = diag.known_codes()
    assert len(codes) > 30
    for entry in codes:
        assert entry.code.startswith("flow.")
        assert entry.code.count(".") == 2
        assert entry.summary.endswith(".")
        # The middle part of the code names the family, so a reader can guess
        # which inspection owns a code without a lookup.
        assert entry.code.split(".")[1] == str(entry.family)


def test_the_code_table_is_the_generated_one():
    """Python reads the C++'s table; it never keeps a second copy of it."""
    table = json.loads(CODES_JSON.read_text(encoding="utf-8"))
    assert table["format"] == diag.CODES_FORMAT
    assert [entry["code"] for entry in table["codes"]] == [
        entry.code for entry in diag.known_codes()
    ]


def test_codes_are_sorted_and_unique():
    codes = [entry.code for entry in diag.known_codes()]
    assert codes == sorted(codes)
    assert len(set(codes)) == len(codes)


def test_a_code_is_found_by_its_name():
    found = diag.find_code("flow.unused.try-status")
    assert found is not None
    assert found.family is diag.Family.UNUSED
    assert found.severity is diag.Severity.WEAK_WARNING
    assert diag.find_code("flow.nothing.here") is None


# --- the envelopes ------------------------------------------------------------


def test_the_diagnostics_envelope_carries_offsets_and_positions():
    envelope = diag.diagnostics_envelope("research.flow", [one()])
    assert envelope["format"] == "flow.diagnostics/v1"
    assert envelope["source"] == "research.flow"

    first = envelope["diagnostics"][0]
    assert first["code"] == "flow.unused.header"
    assert first["severity"] == "weak-warning"
    assert first["family"] == "unused"
    assert first["flow"] == "research"
    # Both, always: an editor edits by offset, a person reads line and column.
    assert first["range"]["start"] == {"offset": 40, "line": 3, "column": 12}
    assert first["range"]["end"]["offset"] == 48
    assert first["fixes"][0]["label"] == "Remove the header declaration"
    assert first["fixes"][0]["edits"][0] == {"start": 30, "end": 60, "text": ""}


def test_the_envelope_counts_by_severity_so_a_gate_needs_no_walk():
    diagnostics = [
        one(),
        diag.Diagnostic(
            code="flow.name.unknown",
            message="Unknown name 'x'.",
            range=diag.Range(diag.Position(), diag.Position()),
        ),
    ]
    counts = diag.diagnostics_envelope("x.flow", diagnostics)["counts"]
    assert counts == {
        "error": 1,
        "warning": 0,
        "weak-warning": 1,
        "information": 0,
    }


def test_an_unknown_flow_is_absent_rather_than_empty():
    plain = dataclass_replace_flow(one(), "")
    assert "flow" not in plain.as_json()


def dataclass_replace_flow(diagnostic: diag.Diagnostic, flow: str) -> diag.Diagnostic:
    import dataclasses

    return dataclasses.replace(diagnostic, flow=flow)


def test_diagnostics_sort_by_position_then_code():
    late = dataclass_replace_range(one("flow.name.unknown"), 20)
    early = dataclass_replace_range(one("flow.form.unknown-stage"), 5)
    same_spot = dataclass_replace_range(one("flow.name.taken"), 5)
    order = [found.code for found in diag.sort_diagnostics([late, same_spot, early])]
    assert order == [
        "flow.form.unknown-stage",
        "flow.name.taken",
        "flow.name.unknown",
    ]


def dataclass_replace_range(
    diagnostic: diag.Diagnostic, offset: int
) -> diag.Diagnostic:
    import dataclasses

    return dataclasses.replace(
        diagnostic,
        range=diag.Range(diag.Position(offset, 1, offset + 1), diag.Position(offset)),
    )


def test_the_text_line_reads_like_every_other_compiler():
    assert one().as_text("research.flow") == (
        "research.flow:3:12: weak-warning: Nothing uses 'deadline'."
        " [flow.unused.header]"
    )
    # Standard input has no path to print.
    assert one().as_text().startswith("3:12: weak-warning:")


def test_sarif_documents_every_rule_it_can_reference():
    log = diag.sarif_log("research.flow", [one()])
    assert log["version"] == "2.1.0"
    driver = log["runs"][0]["tool"]["driver"]
    assert driver["name"] == "a11 flow"
    assert len(driver["rules"]) == len(diag.known_codes())

    result = log["runs"][0]["results"][0]
    assert result["ruleId"] == "flow.unused.header"
    # SARIF has three levels; a weak warning is the one that does not fail a
    # build.
    assert result["level"] == "note"
    region = result["locations"][0]["physicalLocation"]["region"]
    assert region["startLine"] == 3
    assert region["startColumn"] == 12
    # SARIF counts `charOffset` in characters and a diagnostic carries bytes, so
    # without the text there is nothing to convert with and the two offset fields
    # are left out rather than written wrongly. Line and column are what an
    # annotator actually places a comment with.
    assert "charOffset" not in region
    assert "charLength" not in region


def test_sarif_offsets_are_characters_when_the_text_is_there():
    # A source whose bytes and characters disagree, which is the only case where
    # this distinction is visible -- and the case every ASCII test file hides.
    source = 'flow t {\n  describe "§"\n  header "deadline"\n}\n'
    index = diag.LineIndex(source)
    at = source.encode("utf-8").index(b'"deadline"')
    diagnostic = dataclasses.replace(one(), range=index.between(at, at + 10))
    region = diag.sarif_log("t.flow", [diagnostic], index)["runs"][0]["results"][0][
        "locations"
    ][0]["physicalLocation"]["region"]
    assert region["charOffset"] == source.index('"deadline"')
    assert region["charLength"] == 10
    # And it really is a conversion: the byte offset is further along.
    assert diagnostic.range.start.offset > region["charOffset"]


@pytest.mark.parametrize("severity", list(diag.Severity))
def test_every_severity_maps_to_a_sarif_level(severity: diag.Severity):
    assert diag._sarif_level(severity) in ("error", "warning", "note")

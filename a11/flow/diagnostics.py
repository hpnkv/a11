# Copyright 2026 The A11 Authors.

"""Flow diagnostics, and the formats external tools read them in.

One problem found in a flow is a [Diagnostic][a11.flow.diagnostics.Diagnostic]:
a stable code, a severity, a family, a range that carries both byte offsets and
line/column, a message, and any fix that is a single obvious edit. Everything
that reports on a flow -- the CLI, an editor, a CI job -- renders that one shape,
so the wording of a language problem lives in one place.

The formats are versioned and additive: a reader checks ``format`` and ignores
fields it does not know, and a new field is not a version change. They are
*produced* by `cpp/a11/flow/emit_json.{h,cc}` -- one writer per envelope, so the
payload `a11 flow` prints and the payload the standalone `a11-flow` prints cannot
differ. What lives here is the reading half: the dataclasses a Python caller wants
and the text and SARIF renderings the CLI prints. `testdata/flow/codes.json` is
generated from the C++ table and read back here, so the two cannot drift about
what a code means.
"""

from __future__ import annotations

import bisect
import dataclasses
import enum
import functools
import json
import pathlib
from typing import Any, Iterable, Sequence

#: The envelope versions. Bumped only when a field changes meaning or goes away.
DIAGNOSTICS_FORMAT = "flow.diagnostics/v1"
CODES_FORMAT = "flow.codes/v1"
TOKENS_FORMAT = "flow.tokens/v1"
PLAN_FORMAT = "flow.plan/v1"
SYNTAX_FORMAT = "flow.syntax/v1"

#: The generated table of every code the language publishes.
_CODES_PATH = (
    pathlib.Path(__file__).resolve().parents[2] / "testdata" / "flow" / "codes.json"
)


class FlowSyntaxError(Exception):
    """A Flow source file that could not be read.

    What [a11.flow.loads][] raises: the strict door onto an engine that
    otherwise recovers and reports everything. It carries the position, so an
    author -- or a model writing a flow -- is told exactly where the problem is,
    and it is built from the first ``error``
    [Diagnostic][a11.flow.diagnostics.Diagnostic] rather than from a second
    opinion about what is wrong.
    """

    def __init__(
        self, message: str, line: int, column: int, source_name: str = ""
    ):
        self.message = message
        self.line = line
        self.column = column
        self.source_name = source_name
        location = f"{source_name}:" if source_name else ""
        super().__init__(f"{location}{line}:{column}: {message}")

    def to_status(self):
        """The A11 status a caller sees when a flow will not compile."""
        from a11.status import Status, StatusCode

        return Status(code=StatusCode.INVALID_ARGUMENT, message=str(self))


class Severity(enum.StrEnum):
    """How much a diagnostic matters.

    The distinction that earns its keep is between "this cannot work" and "this
    does nothing": the first stops a flow from compiling, the second is the
    greyed-out unused symbol every editor already knows how to show.
    """

    ERROR = "error"
    WARNING = "warning"
    WEAK_WARNING = "weak-warning"
    INFORMATION = "information"


class Family(enum.StrEnum):
    """The kind of problem, which is the grouping a reader thinks in.

    An editor turns each of these into one switchable inspection and a CI job can
    gate on some and not others, which is why the family travels in the output
    rather than being inferred from the code.
    """

    SYNTAX = "syntax"
    FORM = "form"
    NAME = "name"
    SEQUENCE = "sequence"
    BARRIER = "barrier"
    UNUSED = "unused"


@dataclasses.dataclass(frozen=True, slots=True)
class Position:
    """One place in the source: the byte offset, and the line and column at it.

    All three travel because each consumer wants a different one -- offsets to
    edit with, line and column to read -- and computing one from the other needs
    the source text, which a diagnostic that has travelled as JSON no longer has.
    Lines and columns are 1-based, as the lexer has always reported them.
    """

    offset: int = 0
    line: int = 1
    column: int = 1

    def as_json(self) -> dict[str, int]:
        return {"offset": self.offset, "line": self.line, "column": self.column}


@dataclasses.dataclass(frozen=True, slots=True)
class Range:
    """Half-open span of source, ``[start, end)``."""

    start: Position
    end: Position

    def as_json(self) -> dict[str, Any]:
        return {"start": self.start.as_json(), "end": self.end.as_json()}


@dataclasses.dataclass(frozen=True, slots=True)
class Edit:
    """One replacement of a span of source. Empty ``text`` is a deletion."""

    start: int
    end: int
    text: str = ""

    def as_json(self) -> dict[str, Any]:
        return {"start": self.start, "end": self.end, "text": self.text}


@dataclasses.dataclass(frozen=True, slots=True)
class Fix:
    """Edits that would fix a diagnostic, where one obvious edit exists.

    A frontend applies these blind -- it never re-derives what the fix should be
    -- so a fix that guessed would be a fix that corrupted somebody's file.
    """

    label: str
    edits: tuple[Edit, ...] = ()

    def as_json(self) -> dict[str, Any]:
        return {"label": self.label, "edits": [edit.as_json() for edit in self.edits]}


@dataclasses.dataclass(frozen=True, slots=True)
class Diagnostic:
    """One problem found in a flow."""

    code: str
    message: str
    range: Range
    severity: Severity = Severity.ERROR
    family: Family = Family.SYNTAX
    #: The flow it is in, where the text got far enough to say.
    flow: str = ""
    fixes: tuple[Fix, ...] = ()

    def as_json(self) -> dict[str, Any]:
        value: dict[str, Any] = {
            "code": self.code,
            "severity": str(self.severity),
            "family": str(self.family),
            "message": self.message,
            "range": self.range.as_json(),
            "fixes": [fix.as_json() for fix in self.fixes],
        }
        # Absent rather than empty: the flow is unknown when the text did not get
        # far enough to name one, and "" would read as a flow called nothing.
        if self.flow:
            value["flow"] = self.flow
        return value

    def as_text(self, source: str = "") -> str:
        """The line editors and compilers have printed for decades."""
        where = f"{source}:" if source else ""
        return (
            f"{where}{self.range.start.line}:{self.range.start.column}: "
            f"{self.severity}: {self.message} [{self.code}]"
        )

    @classmethod
    def from_payload(cls, value: dict[str, Any]) -> "Diagnostic":
        """A diagnostic read back from the wire shape `as_json` writes.

        What the native engine hands over, and what a frontend reading the JSON
        envelope -- a CI script, the IDE plugin -- turns back into an object.
        Unknown fields are ignored and missing ones take their defaults, so a newer
        producer never breaks an older reader.
        """

        def position(payload: Any) -> Position:
            payload = payload if isinstance(payload, dict) else {}
            return Position(
                offset=payload.get("offset", 0),
                line=payload.get("line", 1),
                column=payload.get("column", 1),
            )

        span = value.get("range") or {}
        return cls(
            code=value.get("code", ""),
            message=value.get("message", ""),
            range=Range(position(span.get("start")), position(span.get("end"))),
            severity=Severity(value.get("severity", "error")),
            family=Family(value.get("family", "syntax")),
            flow=value.get("flow", ""),
            fixes=tuple(
                Fix(
                    label=fix.get("label", ""),
                    edits=tuple(
                        Edit(
                            start=edit.get("start", 0),
                            end=edit.get("end", edit.get("start", 0)),
                            text=edit.get("text", ""),
                        )
                        for edit in fix.get("edits", ())
                    ),
                )
                for fix in value.get("fixes", ())
            ),
        )


@dataclasses.dataclass(frozen=True, slots=True)
class CodeInfo:
    """What a diagnostic code means, from the generated table."""

    code: str
    family: Family
    severity: Severity
    summary: str


@functools.cache
def known_codes() -> tuple[CodeInfo, ...]:
    """Every diagnostic code the language publishes, sorted by code.

    Read from `testdata/flow/codes.json`, which the C++ table generates: this is
    a reader of that contract, never a second copy of it.
    """
    table = json.loads(_CODES_PATH.read_text(encoding="utf-8"))
    return tuple(
        CodeInfo(
            code=entry["code"],
            family=Family(entry["family"]),
            severity=Severity(entry["severity"]),
            summary=entry["summary"],
        )
        for entry in table["codes"]
    )


def find_code(code: str) -> CodeInfo | None:
    """The entry for ``code``, or ``None`` if nothing publishes it."""
    return next((entry for entry in known_codes() if entry.code == code), None)


class LineIndex:
    """Line and column lookup over one source text.

    Built once per file and shared by everything that reports a position, so a
    diagnostic never costs a scan of the source to locate.

    **Over the source's bytes, not its characters.** An offset in this language is
    a byte offset -- that is what the lexer reads, what a diagnostic carries and
    what an edit is applied in -- while a Python `str` is indexed by code point and
    a `§` is one of those and two bytes. Indexing the string would agree with the
    native compiler on every ASCII file and disagree on the first one with prose in
    it, so the bytes are what is indexed and `characters_of` is how a consumer that
    genuinely wants a character count asks for one.
    """

    __slots__ = ("_data", "_length", "_line_starts")

    def __init__(self, source: str) -> None:
        self._data = source.encode("utf-8")
        self._length = len(self._data)
        starts = [0]
        start = self._data.find(b"\n")
        while start != -1:
            starts.append(start + 1)
            start = self._data.find(b"\n", start + 1)
        self._line_starts = starts

    @property
    def length(self) -> int:
        """How many bytes the source is, which is what an offset is bounded by."""
        return self._length

    def at(self, offset: int) -> Position:
        """The position at a byte offset, clamped to the end of the source."""
        clamped = max(0, min(offset, self._length))
        index = bisect.bisect_right(self._line_starts, clamped) - 1
        return Position(
            offset=clamped,
            line=index + 1,
            # Code points, which is what the native compiler counts a column in.
            column=self._characters(self._line_starts[index], clamped) + 1,
        )

    def between(self, start: int, end: int) -> Range:
        """A range from two byte offsets."""
        return Range(start=self.at(start), end=self.at(max(start, end)))

    def offset_of(self, line: int, column: int) -> int:
        """The byte offset of a 1-based line and column, clamped to the source.

        The inverse of [at][a11.flow.diagnostics.LineIndex.at], for the errors
        the compiler reports by line and column rather than by offset.
        """
        if line <= 1:
            start = 0
        elif line - 1 >= len(self._line_starts):
            return self._length
        else:
            start = self._line_starts[line - 1]
        end = (
            self._line_starts[line] - 1
            if 0 < line < len(self._line_starts)
            else self._length
        )
        # Walk the column in code points, because that is the unit it is counted
        # in; a byte walk would land inside a character.
        at = start
        for _ in range(max(column, 1) - 1):
            if at >= end:
                break
            at += 1
            while at < end and self._data[at] & 0xC0 == 0x80:
                at += 1
        return max(0, min(at, self._length))

    def characters_of(self, offset: int) -> int:
        """How many characters of the source precede a byte offset.

        For the one consumer that is specified in characters rather than bytes:
        SARIF's `charOffset`.
        """
        return self._characters(0, max(0, min(offset, self._length)))

    def _characters(self, start: int, end: int) -> int:
        return sum(1 for byte in self._data[start:end] if byte & 0xC0 != 0x80)

    @property
    def line_count(self) -> int:
        return len(self._line_starts)


def sort_diagnostics(diagnostics: Iterable[Diagnostic]) -> list[Diagnostic]:
    """The order every frontend presents them in: by position, then by code."""
    return sorted(
        diagnostics,
        key=lambda found: (found.range.start.offset, found.code, found.message),
    )


def diagnostics_envelope(
    source: str, diagnostics: Sequence[Diagnostic]
) -> dict[str, Any]:
    """The ``flow.diagnostics/v1`` envelope.

    ``counts`` is there so a gate can be written without walking the list.
    """
    counts = {severity.value: 0 for severity in Severity}
    for diagnostic in diagnostics:
        counts[str(diagnostic.severity)] += 1
    return {
        "format": DIAGNOSTICS_FORMAT,
        "source": source,
        "diagnostics": [diagnostic.as_json() for diagnostic in diagnostics],
        "counts": counts,
    }


def codes_envelope() -> dict[str, Any]:
    """The published code table, as ``flow.codes/v1``."""
    return {
        "format": CODES_FORMAT,
        "codes": [
            {
                "code": entry.code,
                "family": str(entry.family),
                "severity": str(entry.severity),
                "summary": entry.summary,
            }
            for entry in known_codes()
        ],
    }


def sarif_log(
    source: str,
    diagnostics: Sequence[Diagnostic],
    index: LineIndex | None = None,
) -> dict[str, Any]:
    """A SARIF 2.1.0 log for one file's diagnostics.

    SARIF is what code-scanning services and CI annotators already read, so
    emitting it means a flow's problems show up in a pull request without anybody
    writing a converter. Every rule in the log is documented, because the rule
    list is the published code table.

    ``index`` is the text the diagnostics are about, and it is worth passing:
    SARIF specifies ``charOffset`` in *characters* while a diagnostic carries
    bytes, and the two differ from the first non-ASCII character in the file. With
    no index the two offset fields are left out rather than written wrongly --
    ``startLine``/``startColumn`` are what an annotator actually places a comment
    with, and a field that is absent is better than one that is off by three.
    """
    def region(diagnostic: Diagnostic) -> dict[str, Any]:
        placed: dict[str, Any] = {
            "startLine": diagnostic.range.start.line,
            "startColumn": diagnostic.range.start.column,
            "endLine": diagnostic.range.end.line,
            "endColumn": diagnostic.range.end.column,
        }
        if index is not None:
            start = index.characters_of(diagnostic.range.start.offset)
            placed["charOffset"] = start
            placed["charLength"] = (
                index.characters_of(diagnostic.range.end.offset) - start
            )
        return placed

    return {
        "$schema": "https://json.schemastore.org/sarif-2.1.0.json",
        "version": "2.1.0",
        "runs": [
            {
                "tool": {
                    "driver": {
                        "name": "a11 flow",
                        "informationUri": "https://github.com/hpnkv/a11",
                        "rules": [
                            {
                                "id": entry.code,
                                "name": entry.code,
                                "shortDescription": {"text": entry.summary},
                                "defaultConfiguration": {
                                    "level": _sarif_level(entry.severity)
                                },
                                "properties": {
                                    "family": str(entry.family),
                                    "tags": ["a11-flow"],
                                },
                            }
                            for entry in known_codes()
                        ],
                    }
                },
                "results": [
                    {
                        "ruleId": diagnostic.code,
                        "level": _sarif_level(diagnostic.severity),
                        "message": {"text": diagnostic.message},
                        "locations": [
                            {
                                "physicalLocation": {
                                    "artifactLocation": {"uri": source},
                                    "region": region(diagnostic),
                                }
                            }
                        ],
                    }
                    for diagnostic in diagnostics
                ],
            }
        ],
    }


def _sarif_level(severity: Severity) -> str:
    """SARIF has three levels and no weak warning, so both shades of "this does
    nothing" land on ``note``."""
    if severity is Severity.ERROR:
        return "error"
    if severity is Severity.WARNING:
        return "warning"
    return "note"


__all__ = [
    "CODES_FORMAT",
    "DIAGNOSTICS_FORMAT",
    "PLAN_FORMAT",
    "SYNTAX_FORMAT",
    "TOKENS_FORMAT",
    "CodeInfo",
    "Diagnostic",
    "Edit",
    "Family",
    "Fix",
    "FlowSyntaxError",
    "LineIndex",
    "Position",
    "Range",
    "Severity",
    "codes_envelope",
    "diagnostics_envelope",
    "find_code",
    "known_codes",
    "sarif_log",
    "sort_diagnostics",
]

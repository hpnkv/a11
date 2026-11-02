# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Read and render Flow diagnostics in Python.

A [Diagnostic][a11.flow.diagnostics.Diagnostic] carries a stable code,
severity, family, source range, message, and optional source edits. Ranges
contain byte offsets for edits and line and column positions for display.

Native Flow analysis produces versioned, additive envelopes. Readers check
the ``format`` field and ignore unknown fields. This module supplies Python
value types and text and SARIF renderers. The code catalogue is generated from
the native table into `testdata/flow/codes.json`.
"""

from __future__ import annotations

import bisect
import dataclasses
import enum
import functools
from typing import Any, Iterable, Sequence

#: Envelope versions change when a field changes meaning or is removed.
DIAGNOSTICS_FORMAT = "flow.diagnostics/v1"
CODES_FORMAT = "flow.codes/v1"
TOKENS_FORMAT = "flow.tokens/v1"
PLAN_FORMAT = "flow.plan/v1"
SYNTAX_FORMAT = "flow.syntax/v1"


class FlowSyntaxError(Exception):
    """Report the first compilation error from a strict Flow entry point.

    [a11.flow.loads][] raises this exception with the first native error
    diagnostic's message, line, and column. Editor entry points continue after
    recoverable errors and return the full diagnostic list.
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
        """Return an ``INVALID_ARGUMENT`` status with the source location."""
        from a11.status import Status, StatusCode

        return Status(code=StatusCode.INVALID_ARGUMENT, message=str(self))


class Severity(enum.StrEnum):
    """Classify whether a diagnostic blocks compilation or reports a finding.

    ``ERROR`` blocks compilation. ``WARNING`` reports a possible behavior
    mismatch. ``WEAK_WARNING`` marks an ineffective source element.
    ``INFORMATION`` supplies a non-blocking finding.
    """

    ERROR = "error"
    WARNING = "warning"
    WEAK_WARNING = "weak-warning"
    INFORMATION = "information"


class Family(enum.StrEnum):
    """Group diagnostics by syntax, form, name, sequence, barrier, or use.

    Editors can expose families as separate inspections. Build gates can select
    families without parsing diagnostic codes.
    """

    SYNTAX = "syntax"
    FORM = "form"
    NAME = "name"
    SEQUENCE = "sequence"
    BARRIER = "barrier"
    UNUSED = "unused"


@dataclasses.dataclass(frozen=True, slots=True)
class Position:
    """Locate source by byte offset and 1-based line and column.

    Byte offsets support edits. Line and column remain available after
    serialization removes access to the original source text.
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
    """Carry source edits supplied by native analysis for a diagnostic.

    Editors apply ``edits`` in source byte coordinates. ``automatic`` marks a
    repair that can be applied without further user input.
    """

    label: str
    edits: tuple[Edit, ...] = ()
    automatic: bool = True

    def as_json(self) -> dict[str, Any]:
        value = {
            "label": self.label,
            "edits": [edit.as_json() for edit in self.edits],
        }
        if not self.automatic:
            value["automatic"] = False
        return value


@dataclasses.dataclass(frozen=True, slots=True)
class RelatedDiagnostic:
    """Another source range contributing to the same diagnostic."""

    source: str
    range: Range
    message: str

    def as_json(self) -> dict[str, Any]:
        return {
            "source": self.source,
            "range": self.range.as_json(),
            "message": self.message,
        }


@dataclasses.dataclass(frozen=True, slots=True)
class Diagnostic:
    """Represent one Flow error or non-blocking finding."""

    code: str
    message: str
    range: Range
    severity: Severity = Severity.ERROR
    family: Family = Family.SYNTAX
    #: Enclosing flow name, when parsing or resolution identifies one.
    flow: str = ""
    fixes: tuple[Fix, ...] = ()
    source: str = ""
    related: tuple[RelatedDiagnostic, ...] = ()

    def as_json(self) -> dict[str, Any]:
        value: dict[str, Any] = {
            "code": self.code,
            "severity": str(self.severity),
            "family": str(self.family),
            "message": self.message,
            "range": self.range.as_json(),
            "fixes": [fix.as_json() for fix in self.fixes],
        }
        # Omit the field when parsing did not identify a flow. An empty string
        # would identify a flow whose name is empty.
        if self.flow:
            value["flow"] = self.flow
        if self.source:
            value["source"] = self.source
        if self.related:
            value["related"] = [item.as_json() for item in self.related]
        return value

    def as_text(self, source: str = "") -> str:
        """Render the location, severity, message, and code on one line."""
        source = self.source or source
        if source == "-":
            source = ""
        where = f"{source}:" if source else ""
        return (
            f"{where}{self.range.start.line}:{self.range.start.column}: "
            f"{self.severity}: {self.message} [{self.code}]"
        )

    @classmethod
    def from_payload(cls, value: dict[str, Any]) -> "Diagnostic":
        """Read a diagnostic from the JSON shape written by ``as_json``.

        Unknown fields are ignored. Missing fields use defaults so older
        readers can accept additive native output.
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
            source=value.get("source", ""),
            related=tuple(
                RelatedDiagnostic(
                    source=item.get("source", ""),
                    range=Range(
                        position(item.get("range", {}).get("start")),
                        position(item.get("range", {}).get("end")),
                    ),
                    message=item.get("message", ""),
                )
                for item in value.get("related", ())
            ),
            fixes=tuple(
                Fix(
                    label=fix.get("label", ""),
                    automatic=fix.get("automatic", True),
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
    """Describe a stable native diagnostic code."""

    code: str
    family: Family
    severity: Severity
    summary: str


@functools.cache
def known_codes() -> tuple[CodeInfo, ...]:
    """Return the native diagnostic catalogue sorted by code.

    The same table generates `testdata/flow/codes.json`.
    """
    from a11._native import flow as native

    return tuple(
        CodeInfo(
            code=entry["code"],
            family=Family(entry["family"]),
            severity=Severity(entry["severity"]),
            summary=entry["summary"],
        )
        for entry in native.codes()
    )


def find_code(code: str) -> CodeInfo | None:
    """Return the entry for ``code``, or ``None`` for an unknown code."""
    return next((entry for entry in known_codes() if entry.code == code), None)


class LineIndex:
    """Convert UTF-8 byte offsets to source lines and columns.

    Construct once per source document and reuse for diagnostic locations.
    Offsets use UTF-8 bytes; columns count Unicode code points. For example,
    `§` occupies two bytes and one column. ``characters_of`` supplies the
    character count required by SARIF.
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
        """The byte length used to bound offsets."""
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
        """Count Unicode code points before a byte offset.

        SARIF uses this count for ``charOffset``.
        """
        return self._characters(0, max(0, min(offset, self._length)))

    def _characters(self, start: int, end: int) -> int:
        return sum(1 for byte in self._data[start:end] if byte & 0xC0 != 0x80)

    @property
    def line_count(self) -> int:
        return len(self._line_starts)


def sort_diagnostics(diagnostics: Iterable[Diagnostic]) -> list[Diagnostic]:
    """Sort by byte position, code, and message."""
    return sorted(
        diagnostics,
        key=lambda found: (found.range.start.offset, found.code, found.message),
    )


def diagnostics_envelope(
    source: str, diagnostics: Sequence[Diagnostic]
) -> dict[str, Any]:
    """Build a ``flow.diagnostics/v1`` envelope with severity counts.

    ``counts`` supports build gates without scanning the diagnostic list.
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
    """Build a ``flow.codes/v1`` envelope from the native catalogue."""
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
    """Build a SARIF 2.1.0 log for one source file.

    Code-scanning services and CI annotators consume SARIF directly. Every rule
    in the log is documented from the published code table.

    ``index`` maps diagnostic byte offsets to SARIF character offsets. When it
    is absent, the log retains line and column locations and omits character
    offsets. Non-ASCII characters require this conversion.
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
    """Map weak warnings and information to SARIF's ``note`` level."""
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

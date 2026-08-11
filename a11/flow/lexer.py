"""The Flow language tokenizer.

Flow source is line-oriented and brace-delimited, with `#` comments. The lexer
is deliberately small: identifiers, strings, numbers, durations, and a fixed set
of punctuation. There are no keywords -- every bare word is an identifier, and
the parser decides which ones are significant where. That is what lets a port be
called ``max`` or an action ``for-each`` without the language fighting back.

Every word that *is* significant may be written in lower case or upper case --
``for`` or ``FOR``, ``stream`` or ``STREAM`` -- because a flow is as often
generated as typed, and both conventions are in use. Mixed case is not a
keyword: ``For`` is a name, which keeps the rule easy to state and easy to see.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from a11 import timing
from a11.status import Status, StatusCode

#: Multi-character punctuation, longest first so `->` wins over `-`.
_PUNCTUATION = (
    "->",
    "<-",
    "==",
    "!=",
    "<=",
    ">=",
    "{",
    "}",
    "(",
    ")",
    "+",
    "-",
    "[",
    "]",
    ":",
    ",",
    "|",
    "=",
    "<",
    ">",
)

#: Duration suffixes a number may carry, and what each is in seconds. The same
#: units a duration is *formatted* in, so what a flow reads back it can write.
DURATION_UNITS = {
    "ns": 1e-9,
    "us": 1e-6,
    "ms": 0.001,
    "s": 1.0,
    "m": 60.0,
    "h": 3600.0,
}


@dataclass(frozen=True)
class Token:
    """One lexed token, with the position used in every error message."""

    kind: str
    text: str
    value: Any
    line: int
    column: int

    def __str__(self) -> str:
        return f"{self.kind} {self.text!r} at {self.line}:{self.column}"


class FlowSyntaxError(Exception):
    """A Flow source file that could not be read.

    Raised while lexing, parsing or resolving; carries the position so an author
    -- or a model writing a flow -- gets told exactly where the problem is.
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

    def to_status(self) -> Status:
        """The A11 status a caller sees when a flow will not compile."""
        return Status(code=StatusCode.INVALID_ARGUMENT, message=str(self))


def canonical(word: str) -> str:
    """The lower-case form of a uniformly-cased word, for keyword matching.

    ``FOR`` and ``for`` both canonicalise to ``for``; ``For`` is left alone and
    so never matches a keyword. Identifiers keep the spelling they were written
    with -- this is only ever consulted when deciding whether a word is
    significant.
    """
    return word.lower() if word.isupper() else word


def _is_ident_start(char: str) -> bool:
    return char.isalpha() or char in "_$"


def _is_ident_part(char: str) -> bool:
    return char.isalnum() or char in "_$"


def tokenize(source: str, source_name: str = "") -> list[Token]:
    """Turn Flow source into tokens, ending with a single ``end`` token.

    Raises:
        FlowSyntaxError: On an unterminated string, an unknown character, or a
            malformed number or duration.
    """
    tokens: list[Token] = []
    index = 0
    line = 1
    line_start = 0
    length = len(source)

    def column(at: int) -> int:
        return at - line_start + 1

    def fail(message: str, at: int) -> FlowSyntaxError:
        return FlowSyntaxError(message, line, column(at), source_name)

    while index < length:
        char = source[index]

        if char == "\n":
            # A statement ends at the end of its line. The parser skips these
            # wherever a line break cannot mean that -- inside brackets, between
            # a flow's statements, and before a continuing `|` or `->`.
            if tokens and tokens[-1].kind != "newline":
                tokens.append(
                    Token("newline", "\\n", None, line, column(index))
                )
            index += 1
            line += 1
            line_start = index
            continue
        if char in " \t\r":
            index += 1
            continue
        if char == "#":
            while index < length and source[index] != "\n":
                index += 1
            continue

        start = index

        if char == '"':
            index += 1
            pieces: list[str] = []
            while True:
                if index >= length or source[index] == "\n":
                    raise fail("Unterminated string.", start)
                current = source[index]
                if current == '"':
                    index += 1
                    break
                if current == "\\":
                    index += 1
                    if index >= length:
                        raise fail("Unterminated escape.", start)
                    escape = source[index]
                    pieces.append(
                        {
                            "n": "\n",
                            "t": "\t",
                            "r": "\r",
                            '"': '"',
                            "\\": "\\",
                        }.get(escape, escape)
                    )
                    index += 1
                    continue
                pieces.append(current)
                index += 1
            text = "".join(pieces)
            tokens.append(
                Token("string", source[start:index], text, line, column(start))
            )
            continue

        if char.isdigit() or (
            char == "-"
            and index + 1 < length
            and source[index + 1].isdigit()
        ):
            index += 1
            while index < length and (
                source[index].isdigit() or source[index] == "."
            ):
                index += 1
            number_text = source[start:index]
            unit_start = index
            while index < length and source[index].isalpha():
                index += 1
            unit = source[unit_start:index]
            try:
                number = (
                    float(number_text)
                    if "." in number_text
                    else int(number_text)
                )
            except ValueError as error:
                raise fail(f"Bad number {number_text!r}.", start) from error
            if unit:
                if unit not in DURATION_UNITS:
                    raise fail(
                        f"Unknown duration unit {unit!r} "
                        f"(use {', '.join(sorted(DURATION_UNITS))}).",
                        unit_start,
                    )
                seconds = float(number) * DURATION_UNITS[unit]
                tokens.append(
                    Token(
                        "duration",
                        source[start:index],
                        timing.Duration.seconds(seconds),
                        line,
                        column(start),
                    )
                )
            else:
                tokens.append(
                    Token(
                        "number",
                        number_text,
                        number,
                        line,
                        column(start),
                    )
                )
            continue

        if _is_ident_start(char):
            index += 1
            while index < length:
                if _is_ident_part(source[index]):
                    index += 1
                    continue
                # A dash continues an identifier only when a word follows, so
                # `drain-and-close` is one name while `a -> b` is a pipe.
                if (
                    source[index] == "-"
                    and index + 1 < length
                    and _is_ident_part(source[index + 1])
                ):
                    index += 2
                    continue
                break
            text = source[start:index]
            tokens.append(Token("word", text, text, line, column(start)))
            continue

        if char == ".":
            index += 1
            tokens.append(Token(".", ".", ".", line, column(start)))
            continue

        for punctuation in _PUNCTUATION:
            if source.startswith(punctuation, index):
                index += len(punctuation)
                tokens.append(
                    Token(
                        punctuation,
                        punctuation,
                        punctuation,
                        line,
                        column(start),
                    )
                )
                break
        else:
            raise fail(f"Unexpected character {char!r}.", start)

    while tokens and tokens[-1].kind == "newline":
        tokens.pop()
    tokens.append(Token("end", "", None, line, column(index)))
    return tokens


__all__ = [
    "DURATION_UNITS",
    "FlowSyntaxError",
    "Token",
    "canonical",
    "tokenize",
]

# Copyright 2026 The A11 Authors.

"""Reading a duration off the command line, the way a flow reads one."""

from __future__ import annotations

import argparse


def duration_seconds(text: str) -> float:
    """A duration written the way a flow writes one, as seconds.

    Read with the language's **own lexer** rather than a second parser here, so
    `--timeout` accepts exactly what a duration literal in a `.flow` file takes
    and exactly what `a11-flow-run --timeout` takes. A CLI flag that agreed
    with neither is how `--timeout 30s` came to be rejected by the frontend that
    claims to be the same interpreter.

    A bare number is seconds, which is the one form the lexer reads as a number
    rather than a duration. Consecutive durations add up, so `1m30s` is ninety
    seconds -- the compound form the C++ parser also takes.
    """
    from a11._native import flow as native

    read = native.tokenize(text)
    tokens = [one for one in read["tokens"] if one["kind"] != "end"]
    if not read["diagnostics"] and tokens:
        if all(one["kind"] == "duration" for one in tokens):
            return sum(one["value"].float_seconds() for one in tokens)
        if len(tokens) == 1 and tokens[0]["kind"] == "number":
            return float(tokens[0]["value"])
    raise argparse.ArgumentTypeError(
        f"{text!r} is not a duration; write one as 30s, 250ms, 1m30s, or a"
        " number of seconds"
    )


__all__ = ["duration_seconds"]

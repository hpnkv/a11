# Copyright 2026 The A11 Authors.

"""Run a flow on uvloop.

The loop is a process-wide choice, so each case runs in a subprocess that
installs uvloop before A11 sees a loop at all. uvloop is optional and `uv sync`
removes it, so a run without it skips.

The shape matches `bench/suites/flow.py`'s `pipe_throughput`: a piped flow
started repeatedly through `invoke`.
"""

from __future__ import annotations

import subprocess
import sys
import textwrap

import pytest

#: Subprocess deadline for the uvloop flow cases.
_TIMEOUT_SECONDS = 120

_PREAMBLE = """
import uvloop
import a11
from a11 import flow as flow_lang

_PIPED = '''
flow piped {
  in items: string stream required
  out result: string stream
  items | map it -> result
}
'''

program = flow_lang.compile_source(_PIPED, "test")
plan = program["piped"]

async def run_rounds(rounds, values):
    seen = 0
    for _ in range(rounds):
        out = await flow_lang.invoke(plan, items=values)
        seen += len(out["result"])
    return seen
"""


def _run(body: str) -> str:
    """Run ``body`` after the preamble in a fresh interpreter."""
    source = textwrap.dedent(_PREAMBLE) + textwrap.dedent(body)
    finished = subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        timeout=_TIMEOUT_SECONDS,
    )
    assert finished.returncode == 0, finished.stderr
    return finished.stdout.strip()


pytest.importorskip("uvloop", reason="uvloop is not a project dependency")


def test_a_piped_flow_runs_to_completion_on_uvloop() -> None:
    values = [f"value-{index}" for index in range(16)]
    output = _run(
        f"print(uvloop.run(run_rounds(40, {values!r})))",
    )
    assert output == "640"


def test_many_short_flows_run_to_completion_on_uvloop() -> None:
    """Repeated short flows complete on the same uvloop."""
    output = _run("print(uvloop.run(run_rounds(200, ['one'])))")
    assert output == "200"

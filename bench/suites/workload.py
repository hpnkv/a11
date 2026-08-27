# Copyright 2026 The A11 Authors.

"""Measure A11 overhead in representative agent workloads.

The suite holds the expensive backend work constant and measures two common
workloads:

* **`interact_with_llm`** with a fake provider registered in place of a real
  one. No network and no model, so what remains is exactly A11: routing the
  call, four output ports, and one chunk per token through a node. Swept
  against token count, because a turn that streams two thousand tokens pays the
  per-token cost two thousand times and nothing else changes.
* **the bash tool** against a real `bash`:
* a process, a command, and output lines
  returned over a port. Results vary by output-line count.

Both report A11's share as a fraction. A tool call where A11 is 4% of the wall
clock is not worth optimising; one where it is 70% is.
"""

from __future__ import annotations

import asyncio
import functools
import shutil
import subprocess
import sys
import time
import types

import a11
from bench.harness import Result, benchmark, latency

SUITE = "workload"


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 4)


# --------------------------------------------------------------------------
# interact_with_llm, with the model taken out
# --------------------------------------------------------------------------


def _install_fake_provider(tokens: int, thoughts: int = 0):
    """Register a provider that streams `tokens` chunks and nothing else.

    Returns the provider key. The handler performs the minimum provider work:
    write tokens, write one interaction, and close every port. This isolates
    routing and port overhead.
    """
    from a11.sdk import interact_with_llm as illm
    from a11.sdk.llm import Interaction

    async def handler(action) -> None:
        for index in range(tokens):
            await action["text_output"].put(f"tok{index} ")
        for index in range(thoughts):
            await action["thoughts"].put(f"thought{index}")
        await action["new_interactions"].put(Interaction(model="bench"))
        for name in ("event_stream", "text_output", "thoughts"):
            await action[name].finalize()
        await action["new_interactions"].finalize()

    module_name = f"a11.sdk._bench_provider_{tokens}_{thoughts}"
    module = types.ModuleType(module_name)
    module.handler = handler
    sys.modules[module_name] = module
    illm._PROVIDERS["bench"] = illm._Provider(module_name, "handler", "bench")
    return "bench"


async def _one_turn(read: str = "text_output") -> int:
    from a11.sdk.interact_with_llm import (
        INTERACT_WITH_LLM_SCHEMA,
        interact_with_llm,
    )
    from a11.sdk.llm import LlmHeaders

    action = a11.Action(INTERACT_WITH_LLM_SCHEMA).bind_handler(
        interact_with_llm
    )
    action.set_header(LlmHeaders.PROVIDER.value, b"bench")
    action = action.run()
    await action["interactions"].finalize(
        a11.to_chunk({
            "role": "user",
            "content": [{"type": "text", "text": "hi"}],
        })
    )
    await action["config"].finalize()
    await action["tools"].finalize()
    seen = 0
    async for _value in action[read]:
        seen += 1
    return seen


@benchmark(SUITE, "llm_turn")
async def llm_turn(scale: float) -> list[Result]:
    """One `interact_with_llm` turn with a fake provider, by token count.

    The per-token slope is the number that matters: subtract the fixed cost of
    a turn and what is left is what A11 charges to move one token from a
    provider handler to a consumer. Put it beside a real model's inter-token
    time (tens of milliseconds for a large model, low single-digit
    milliseconds for a small local one) to see whether it is visible at all.
    """
    results = []
    for tokens in (1, 32, 512):
        _install_fake_provider(tokens)
        iterations = _scaled(60 if tokens < 512 else 20, scale)
        metrics = await latency(
            lambda _index: _one_turn(),
            iterations=iterations,
            warmup=5,
        )
        metrics["items_per_s"] = metrics["ops_per_s"] * tokens
        results.append(
            Result(SUITE, "interact_with_llm", metrics, {"tokens": tokens})
        )

    fixed = results[0].metrics["p50_us"]
    slope = (results[-1].metrics["p50_us"] - fixed) / (512 - 1)
    results[0].note = f"fixed cost of a turn: {fixed:.0f}us"
    results[-1].note = (
        f"{slope:.0f}us per streamed token above the fixed cost -- compare"
        " against the model's own inter-token time"
    )
    return results


@benchmark(SUITE, "llm_turn_fanout")
async def llm_turn_fanout(scale: float) -> list[Result]:
    """Concurrent turns: how many conversations one process can stream at once.

    A gateway's real load is many turns at once, each slow because the model is
    slow. The fake provider removes the model, so what this measures is how
    many *A11-side* turns can be in flight before the process is the limit.
    """
    _install_fake_provider(64)
    results = []
    for in_flight in (1, 8, 32):
        total = _scaled(48, scale)
        started = time.perf_counter_ns()
        remaining = total
        while remaining > 0:
            batch = min(in_flight, remaining)
            await asyncio.gather(*(_one_turn() for _ in range(batch)))
            remaining -= batch
        elapsed = (time.perf_counter_ns() - started) / 1e9
        results.append(
            Result(
                SUITE,
                "turns_in_flight",
                {
                    "ops_per_s": total / elapsed,
                    "items_per_s": total * 64 / elapsed,
                    "elapsed_s": elapsed,
                },
                {"in_flight": in_flight, "tokens": 64},
            )
        )
    return results


# --------------------------------------------------------------------------
# The bash tool, against a real shell
# --------------------------------------------------------------------------


def _bash_registry(manager):
    from a11.sdk import bash

    registry = a11.ActionRegistry()
    for schema, handler in bash.SHELL_ACTIONS:
        registry.register(
            schema.name, schema, functools.partial(handler, manager=manager)
        )
    return registry


async def _drive(registry, name, *, headers=None, command=None):
    action = registry.make_action(name)
    for key, value in (headers or {}).items():
        action.set_header(key, value.encode())
    action.run()
    if command is not None:
        await action["command"].put(command, final=True)
    # Closed rather than finalized: `command` marked its own value final.
    for input_name in action.get_schema().inputs:
        await action[input_name].close()
    await asyncio.wait_for(action.wait(), timeout=60)
    return action


async def _lines(action, port: str) -> int:
    seen = 0
    while await action[port].next_object(str) is not None:
        seen += 1
    return seen


@benchmark(SUITE, "bash_tool")
async def bash_tool(scale: float) -> list[Result]:
    """`shell_execute` against a real bash, by lines of output.

    Two costs are tangled here on purpose, and the sweep separates them: the
    fixed cost of a tool call (the action, the shell round trip, the sentinel
    protocol) and the marginal cost of a line of output (one chunk through one
    port). The second is A11's; the first is mostly the shell's.

    Also reported: the same commands run through `subprocess` with no A11 at
    all, which is the floor a tool call could ever reach.
    """
    if shutil.which("bash") is None:
        from bench.harness import Skip

        raise Skip("bash is not on PATH")

    from a11.sdk.bash import SHELL_ID_HEADER
    from a11.sdk.bash.manager import ShellManager

    manager = ShellManager()
    registry = _bash_registry(manager)
    results = []
    try:
        started = await _drive(registry, "shell_start")
        shell_id = await started["shell_id"].next_object(str)

        for lines in (1, 20, 500):
            command = f"seq 1 {lines}"
            iterations = _scaled(30 if lines < 500 else 12, scale)

            async def one(_index, c=command, s=shell_id) -> None:
                action = await _drive(
                    registry,
                    "shell_execute",
                    headers={SHELL_ID_HEADER: s},
                    command=c,
                )
                await _lines(action, "output_lines")

            metrics = await latency(one, iterations=iterations, warmup=3)
            metrics["items_per_s"] = metrics["ops_per_s"] * lines
            results.append(
                Result(SUITE, "shell_execute", metrics, {"lines": lines})
            )

        # The floor: the same command, no A11, no persistent shell.
        floor = await latency(
            lambda _index: asyncio.to_thread(
                subprocess.run,
                ["bash", "-c", "seq 1 20"],
                capture_output=True,
            ),
            iterations=_scaled(20, scale),
            warmup=3,
        )
        results.append(
            Result(
                SUITE,
                "subprocess_floor",
                floor,
                {"lines": 20},
                note="bash -c through subprocess, for scale",
            )
        )
        fixed = results[0].metrics["p50_us"]
        per_line = (results[2].metrics["p50_us"] - fixed) / 499
        results[
            2
        ].note = f"{per_line:.0f}us per output line above the fixed cost"

        await _drive(
            registry, "shell_exit", headers={SHELL_ID_HEADER: shell_id}
        )
    finally:
        for entry in list(manager._shells.values()):
            entry.shell.terminate()
            entry.shell.kill()
        manager._shells.clear()
    return results

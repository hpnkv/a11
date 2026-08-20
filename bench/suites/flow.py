# Copyright 2026 The A11 Authors.

"""Flow: the language's tooling latency, and the runtime's per-step cost.

Flow is measured in two entirely separate places, because two entirely
different people are waiting on it.

**The editor.** `check`, `tokens`, `parse`, `format` and `complete` run on
keystrokes. What matters is p99 latency on a document of realistic size, not
throughput: a 40ms p99 on `check` is a language server that feels broken, and
nobody will report it as a performance bug -- they will just stop using the
plugin. These run against `examples/005-http/http.flow` (a real, commented,
four-flow document) and against synthetic documents an order of magnitude
larger, so the *shape* of the curve against document size is visible. A parser
that is quadratic in flow count is fine at four flows and not at four hundred.

**The runtime.** A flow is an action whose steps are actions, so the number
that matters is what wrapping a call in a flow *adds*. Measured as the same
work done three ways -- the action called directly, the action called by a
one-step flow, and a flow with several steps -- so the per-step overhead falls
out of the differences rather than being asserted.

Also here: pipe throughput (values per second through `|` stages, which is the
streaming path), and flow compilation, which a gateway pays once per flow at
startup and again whenever a flow is edited.
"""

from __future__ import annotations

import asyncio
import os
import time

import a11
from a11 import flow as flow_lang
from bench.harness import (
    Result,
    benchmark,
    latency,
    latency_sync,
    memory_slope,
    throughput_sync,
)

SUITE = "flow"

#: The worked example the suite reads as its "real document".
_EXAMPLE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "examples",
    "005-http",
    "http.flow",
)


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 8)


def _example_source() -> str:
    with open(_EXAMPLE) as handle:
        return handle.read()


def _grown(source: str, copies: int) -> str:
    """`copies` renamed copies of a document, to grow it without changing it.

    Renaming rather than repeating: a duplicate flow name is a resolver error,
    and an error path is not what the editor benchmarks are about.
    """
    parts = []
    for index in range(copies):
        renamed = source
        for name in (
            "read-a-page",
            "only-if-json",
            "inspect-a-url",
            "send-a-stream",
        ):
            renamed = renamed.replace(f"flow {name} ", f"flow {name}-{index} ")
        parts.append(renamed)
    return "\n".join(parts)


_DOCUMENTS = None


def _documents() -> list[tuple[str, str]]:
    global _DOCUMENTS
    if _DOCUMENTS is None:
        source = _example_source()
        _DOCUMENTS = [
            ("1x", source),
            ("10x", _grown(source, 10)),
            ("50x", _grown(source, 50)),
        ]
    return _DOCUMENTS


# --------------------------------------------------------------------------
# Language tooling
# --------------------------------------------------------------------------


@benchmark(SUITE, "editor_latency")
async def editor_latency(scale: float) -> list[Result]:
    """The methods an editor calls on a keystroke, per document size."""
    results = []
    for method in ("tokens", "parse", "check", "format"):
        for label, source in _documents():
            iterations = _scaled(
                400 if label == "1x" else (60 if label == "10x" else 15),
                scale,
            )
            payload = {"method": method, "source": source}
            metrics = latency_sync(
                lambda _i, p=payload: flow_lang.request(p),
                iterations=iterations,
                warmup=max(iterations // 10, 2),
            )
            metrics["mib_per_s"] = (
                metrics["ops_per_s"] * len(source) / (1024 * 1024)
            )
            results.append(
                Result(
                    SUITE,
                    method,
                    metrics,
                    {"doc": label, "bytes": len(source)},
                )
            )
    return results


@benchmark(SUITE, "completion_latency")
async def completion_latency(scale: float) -> list[Result]:
    """`complete` at a live offset -- the slowest thing an editor asks for.

    Offsets are swept across the document rather than fixed, because
    completion cost depends on what is in scope where the caret is, and one
    lucky offset would flatter it.
    """
    results = []
    for label, source in _documents():
        offsets = [
            len(source) * step // 12 for step in range(1, 12)
        ]
        iterations = _scaled(200 if label == "1x" else 40, scale)
        metrics = latency_sync(
            lambda index, s=source, o=offsets: flow_lang.request(
                {
                    "method": "complete",
                    "source": s,
                    "offset": o[index % len(o)],
                }
            ),
            iterations=iterations,
            warmup=max(iterations // 10, 2),
        )
        results.append(
            Result(
                SUITE,
                "complete",
                metrics,
                {"doc": label, "bytes": len(source)},
            )
        )
    return results


@benchmark(SUITE, "compile")
async def compile_program(scale: float) -> list[Result]:
    """Source to a runnable `Program`, and what one costs to keep."""
    results = []
    for label, source in _documents():
        iterations = _scaled(200 if label == "1x" else 30, scale)
        metrics = throughput_sync(
            lambda _i, s=source: flow_lang.compile_source(s, "bench"),
            iterations=iterations,
            warmup=max(iterations // 10, 2),
        )
        metrics["p50_us"] = metrics["ns_per_op"] / 1000
        metrics["mib_per_s"] = (
            metrics["ops_per_s"] * len(source) / (1024 * 1024)
        )
        results.append(
            Result(
                SUITE,
                "compile_source",
                metrics,
                {"doc": label, "bytes": len(source)},
            )
        )

    source = _documents()[0][1]
    slope, trail = await memory_slope(
        lambda count: [
            flow_lang.compile_source(source, "bench") for _ in range(count)
        ],
        counts=[_scaled(200, scale)] * 6,
    )
    results.append(
        Result(
            SUITE,
            "program_resident",
            {"bytes_each": slope},
            {"doc": "1x"},
            note=f"a compiled 4-flow program held in memory. {trail}",
        )
    )
    return results


# --------------------------------------------------------------------------
# Runtime
# --------------------------------------------------------------------------


_ECHO = a11.ActionSchema(
    name="echo",
    description="Echo the input back.",
    inputs={
        "text": a11.ActionPortSchema(
            name="text", type="text/plain", unary=True, required=True
        )
    },
    outputs={"out": a11.ActionPortSchema(name="out", type="text/plain")},
)


async def _echo(action: a11.Action) -> None:
    text = await action["text"].consume(str)
    await action["out"].finalize(text)


def _runtime_registry() -> a11.ActionRegistry:
    registry = a11.ActionRegistry()
    registry.register("echo", _ECHO, _echo)
    return registry


def _chain_source(steps: int) -> str:
    """A flow that threads one value through `steps` sequential `run`s."""
    lines = [
        f"flow chain{steps} {{",
        "  in text: string required",
        "  out result: string",
    ]
    previous = "text"
    for index in range(steps):
        lines.append(f"  s{index} = run echo(text: {previous})")
        previous = f"s{index}.out"
    lines.append(f"  {previous} -> result")
    lines.append("}")
    return "\n".join(lines)


_PIPE_SOURCE = """
flow piped {
  in items: string stream required
  out result: string stream
  items | map it -> result
}
"""


@benchmark(SUITE, "step_overhead")
async def step_overhead(scale: float) -> list[Result]:
    """A bare action, then the same action inside a flow, then a chain.

    The first two differ by exactly one flow: its own action, its ports, and
    the fibre that copies one node into another. The chain says whether the
    second, third and tenth step cost the same as the first.
    """
    registry = _runtime_registry()
    iterations = _scaled(200, scale)
    results = []

    async def bare(_index) -> None:
        call = a11.Action(_ECHO, handler=_echo)
        call.run()
        await call["text"].finalize("payload")
        await call["out"].consume(str)
        await call.wait(a11.Duration.seconds(30))

    results.append(
        Result(
            SUITE,
            "action_direct",
            await latency(bare, iterations=iterations, warmup=10),
            {"steps": 0},
            note="no flow involved -- the baseline the flow is charged against",
        )
    )

    for steps in (1, 2, 8):
        program = flow_lang.compile_source(_chain_source(steps), "bench")
        plan = program[f"chain{steps}"]
        metrics = await latency(
            lambda _index, p=plan: flow_lang.invoke(
                p, text="payload", registry=registry
            ),
            iterations=iterations,
            warmup=10,
        )
        metrics["steps_per_s"] = metrics["ops_per_s"] * steps
        results.append(
            Result(SUITE, "flow_run", metrics, {"steps": steps})
        )

    baseline = results[0].metrics["p50_us"]
    one_step = results[1].metrics["p50_us"]
    eight = results[3].metrics["p50_us"]
    results[1].note = (
        f"{one_step - baseline:.0f}us over the bare action -- the whole cost"
        " of expressing it as a flow"
    )
    results[3].note = (
        f"{(eight - one_step) / 7:.0f}us per additional step"
    )
    return results


@benchmark(SUITE, "pipe_throughput")
async def pipe_throughput(scale: float) -> list[Result]:
    """Values per second through a `|` stage: the streaming path.

    A pipe is a fibre copying one node into another as values arrive, so this
    is the closest thing Flow has to a per-value cost. Compare it against
    `nodes/drain`: a pipe that costs much more than a node read is spending it
    somewhere worth finding.
    """
    registry = _runtime_registry()
    program = flow_lang.compile_source(_PIPE_SOURCE, "bench")
    plan = program["piped"]
    results = []
    for count in (16, 256, 4096):
        values = [f"value-{index}" for index in range(count)]
        iterations = _scaled(60 if count <= 256 else 10, scale)
        started = time.perf_counter_ns()
        for _ in range(iterations):
            out = await flow_lang.invoke(plan, items=values, registry=registry)
        elapsed = (time.perf_counter_ns() - started) / 1e9
        moved = iterations * count
        results.append(
            Result(
                SUITE,
                "pipe_values",
                {
                    "ops_per_s": iterations / elapsed,
                    "items_per_s": moved / elapsed,
                    "p50_us": elapsed / iterations * 1e6,
                    "elapsed_s": elapsed,
                },
                {"values": count},
                note=f"{len(out['result'])} values out per run",
            )
        )
    return results


@benchmark(SUITE, "concurrent_flows")
async def concurrent_flows(scale: float) -> list[Result]:
    """Whole flows run concurrently: what a gateway serving flows delivers."""
    registry = _runtime_registry()
    program = flow_lang.compile_source(_chain_source(1), "bench")
    plan = program["chain1"]
    results = []
    for in_flight in (1, 8, 32):
        total = _scaled(120, scale)
        started = time.perf_counter_ns()
        remaining = total
        while remaining > 0:
            batch = min(in_flight, remaining)
            await asyncio.gather(
                *(
                    flow_lang.invoke(plan, text="payload", registry=registry)
                    for _ in range(batch)
                )
            )
            remaining -= batch
        elapsed = (time.perf_counter_ns() - started) / 1e9
        results.append(
            Result(
                SUITE,
                "flows_in_flight",
                {"ops_per_s": total / elapsed, "elapsed_s": elapsed},
                {"in_flight": in_flight},
            )
        )
    return results

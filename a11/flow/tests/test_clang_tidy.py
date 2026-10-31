"""Exercise the reusable clang-tidy Flow without requiring LLVM."""

import asyncio
import json
import os
from pathlib import Path

import pytest

import a11
from a11 import flow, timing
from a11.flow.runtime import start
from a11.status import StatusException


SOURCE = Path(__file__).parents[3] / ".a11/flows/clang_tidy.flow"
SCHEMA = a11.ActionSchema.model_validate({
    "name": "run_command",
    "inputs": {
        "command": {"type": str, "unary": True},
        "timeout_seconds": {"type": int, "unary": True},
    },
    "outputs": {
        "output_lines": {"type": str},
        "result": {"type": dict, "unary": True},
    },
})


async def invoke(inputs, handler):
    registry = a11.ActionRegistry()
    registry.register("run_command", SCHEMA, handler)
    program = flow.load(SOURCE)
    program.register_all(registry)
    return await program["clang_tidy"].invoke(
        inputs, registry=registry, timeout=timing.Duration.seconds(15)
    )


@pytest.mark.asyncio
async def test_checks_start_before_source_stream_closes():
    started = asyncio.Event()
    commands = []

    async def run_command(action):
        commands.append(await action["command"].consume(str))
        started.set()
        await action["output_lines"].finalize()
        await action["result"].finalize({"exit_code": 0})

    registry = a11.ActionRegistry()
    registry.register("run_command", SCHEMA, run_command)
    program = flow.load(SOURCE)
    program.register_all(registry)
    running = await start(
        program["clang_tidy"],
        registry=registry,
        open_inputs=["sources"],
        timeout=timing.Duration.seconds(15),
    )
    sources = running.inputs["sources"]
    try:
        await (await sources.put("first.cc"))
        await asyncio.wait_for(started.wait(), timeout=5)
        await (await sources.put("second.cc"))
    finally:
        await sources.finalize()
    result = await running.collect()
    assert result["result"] == {"exit_code": 0}
    assert commands == ["'clang-tidy' './first.cc'", "'clang-tidy' './second.cc'"]


def test_sources_is_a_required_string_stream():
    source = flow.load(SOURCE)["clang_tidy"].schema.inputs["sources"]
    assert source.typeinfo is str
    assert not source.unary
    assert source.required


@pytest.mark.asyncio
@pytest.mark.parametrize("limit", [1, 2, 3])
@pytest.mark.parametrize("custom", [False, True])
@pytest.mark.parametrize("failure", [False, True])
async def test_paths_and_concurrency(tmp_path, limit, custom, failure):
    executable = tmp_path / ("tidy ' tool" if custom else "clang-tidy")
    events = tmp_path / "events.jsonl"
    executable.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, sys, time\n"
        "flags = os.O_WRONLY | os.O_CREAT | os.O_APPEND\n"
        f"fd = os.open({str(events)!r}, flags, 0o600)\n"
        "def emit(event):\n"
        "    data = [event, os.getpid(), sys.argv[1:]]\n"
        "    record = json.dumps(data) + chr(10)\n"
        "    os.write(fd, record.encode())\n"
        "emit('start')\n"
        "time.sleep(0.15)\n"
        "emit('end')\n"
        "print('checked')\n"
        "sys.exit(1 if sys.argv[-1].endswith('bad.cc') else 0)\n"
    )
    executable.chmod(0o755)
    sources = [
        "space name.cc",
        "quote'file.cc",
        "$(touch INJECTED).cc",
        "line\nbreak.cc",
        "-option.cc",
        str(tmp_path / "absolute.cc"),
        "bad.cc" if failure else "good.cc",
    ]
    inputs = {"sources": sources, "options": {"max_concurrency": limit}}
    if custom:
        inputs["options"].update(clang_tidy=str(executable), build_dir="build ' dir")

    async def run_command(action):
        command = await action["command"].consume(str)
        assert await action["timeout_seconds"].consume(int) == 600
        process = await asyncio.create_subprocess_exec(
            "bash",
            "--noprofile",
            "--norc",
            "-c",
            command,
            cwd=tmp_path,
            env={**os.environ, "PATH": f"{tmp_path}:{os.environ['PATH']}"},
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        output, _ = await process.communicate()
        for line in output.decode().splitlines():
            await (await action["output_lines"].put(line))
        await action["output_lines"].finalize()
        await action["result"].finalize({"exit_code": process.returncode})

    result = await invoke(inputs, run_command)
    assert (result["result"]["exit_code"] != 0) == failure
    assert result["diagnostics"].count("checked") == len(sources)
    assert not (tmp_path / "INJECTED").exists()
    running = set()
    peak = 0
    seen = []
    for event, pid, args in map(json.loads, events.read_text().splitlines()):
        if event == "start":
            running.add(pid)
            seen.append(args)
            peak = max(peak, len(running))
        else:
            running.remove(pid)
    assert not running
    assert peak == limit
    prefix = ["-p", "build ' dir"] if custom else []
    assert sorted(seen) == sorted(
        prefix + [path if path.startswith("/") else "./" + path]
        for path in sources
    )


@pytest.mark.asyncio
@pytest.mark.parametrize("options", [None, {}])
async def test_default_options(options):
    inputs = {"sources": [f"source{i}.cc" for i in range(5)]}
    if options is not None:
        inputs["options"] = options
    active = 0
    peak = 0
    commands = []

    async def run_command(action):
        nonlocal active, peak
        commands.append(await action["command"].consume(str))
        active += 1
        peak = max(peak, active)
        await asyncio.sleep(0.02)
        active -= 1
        await action["output_lines"].finalize()
        await action["result"].finalize({"exit_code": 0})

    result = await invoke(inputs, run_command)
    assert result["result"] == {"exit_code": 0}
    assert peak == 4
    assert sorted(commands) == [
        f"'clang-tidy' './source{i}.cc'" for i in range(5)
    ]


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "overrides",
    [
        {"options": {"max_concurrency": 0}},
        {"options": {"max_concurrency": -1}},
        {"sources": []},
        {"sources": [""]},
        {"sources": ["a\0.cc"]},
        {"options": {"clang_tidy": ""}},
        {"options": {"build_dir": ""}},
    ],
)
async def test_invalid_input_never_runs_command(overrides):
    async def unexpected(action):
        pytest.fail("Invalid input reached run_command")

    with pytest.raises(StatusException) as error:
        await invoke(
            {"sources": ["a.cc"], "options": {"max_concurrency": 2}, **overrides}, unexpected
        )
    assert error.value.status.code == a11.StatusCode.INVALID_ARGUMENT

# Copyright 2026 The A11 Authors.

"""Offline tests for the Codex CLI interaction provider."""

import asyncio
import json
from pathlib import Path

import pytest

import a11
from a11.sdk import llm
from a11.sdk.openai import interact_with_codex as mod
from a11.sdk.openai.interact_with_codex_schema import (
    CreateCodexSessionConfig,
    INTERACT_WITH_CODEX_SCHEMA,
    THREAD_ID_METADATA_KEY,
)


async def _run(script, monkeypatch, *, interactions=None, tools=()):
    calls = []

    async def fake_run(*args):
        calls.append(args)
        return script[len(calls) - 1]

    monkeypatch.setattr(mod, "_run_codex", fake_run)
    registry = a11.ActionRegistry()

    schema = a11.ActionSchema(
        name="lookup",
        inputs={"key": a11.ActionPortSchema("key", "text/plain")},
        outputs={"value": a11.ActionPortSchema("value", "text/plain")},
    )

    async def lookup(action):
        key = await action["key"].consume(str)
        await action["value"].finalize(f"value of {key}")

    registry.register("lookup", schema, lookup)
    action = a11.Action(INTERACT_WITH_CODEX_SCHEMA).bind_handler(
        mod.interact_with_codex
    )
    if tools:
        action = action.bind_registry(registry).set_header(
            llm.LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"lookup"
        )
    action = action.run()
    text_task = asyncio.create_task(_collect(action["text_output"]))
    await action["interactions"].finalize(
        interactions
        or llm.Interaction(
            role=llm.Role.USER,
            content=[a11.to_chunk({"role": "user", "content": "hello"})],
        )
    )
    await action["config"].finalize(CreateCodexSessionConfig())
    for tool in tools:
        await action["tools"].put(tool)
    await action["tools"].finalize()
    produced = [value async for value in action["new_interactions"]]
    await action.wait()
    return await text_task, produced, calls


async def _collect(node):
    return [value async for value in node]


class _Sink:
    def __init__(self):
        self.values = []

    async def put(self, value):
        self.values.append(value)


class _Input:
    def __init__(self):
        self.data = b""

    def write(self, value):
        self.data += value

    async def drain(self):
        pass

    def close(self):
        pass


class _Process:
    def __init__(self, events):
        self.stdin = _Input()
        self.stdout = asyncio.StreamReader()
        self.stderr = asyncio.StreamReader()
        for event in events:
            self.stdout.feed_data(json.dumps(event).encode() + b"\n")
        self.stdout.feed_eof()
        self.stderr.feed_eof()
        self.returncode = None

    async def wait(self):
        self.returncode = 0
        return 0

    def terminate(self):
        self.returncode = -15


@pytest.mark.asyncio
async def test_jsonl_transport_streams_events_reasoning_and_usage(monkeypatch):
    process = _Process(
        [
            {"type": "thread.started", "thread_id": "thread-9"},
            {
                "type": "item.completed",
                "item": {"type": "reasoning", "text": "considering"},
            },
            {
                "type": "item.completed",
                "item": {"type": "agent_message", "text": "Answer."},
            },
            {
                "type": "turn.completed",
                "usage": {
                    "input_tokens": 5,
                    "cached_input_tokens": 2,
                    "output_tokens": 3,
                },
            },
        ]
    )
    command = []

    async def create(*args, **kwargs):
        command.extend(args)
        return process

    monkeypatch.setattr(asyncio, "create_subprocess_exec", create)
    action = {
        "event_stream": _Sink(),
        "thoughts": _Sink(),
    }
    text, thread_id, usage = await mod._run_codex(
        action,
        CreateCodexSessionConfig(),
        "gpt-test",
        "hello",
        None,
        None,
        {},
    )

    assert text == "Answer."
    assert thread_id == "thread-9"
    assert usage.total_tokens == 8
    assert action["thoughts"].values == ["considering"]
    assert process.stdin.data == b"hello"
    assert command[:3] == ["codex", "exec", "--json"]


def test_resume_uses_only_options_supported_by_the_subcommand():
    config = CreateCodexSessionConfig(
        cwd="/workspace",
        add_dirs=["/shared"],
        profile="work",
        sandbox="workspace-write",
    )

    options = mod._options(config, "gpt-test", None, resume=True)

    assert "--json" in options
    assert "--model" in options
    assert "--cd" not in options
    assert "--add-dir" not in options
    assert "--profile" not in options
    assert "--sandbox" not in options


def test_recorded_thread_resumes_only_interactions_it_has_not_seen():
    answered = llm.Interaction(
        role=llm.Role.ASSISTANT,
        content=[a11.to_chunk({"role": "assistant", "content": "Earlier."})],
        backend_specific_metadata={
            llm.BACKEND_METADATA_KEY: b"codex",
            THREAD_ID_METADATA_KEY: b"thread-10",
        },
    )
    follow_up = llm.Interaction(
        previous_interaction_id=answered.id,
        role=llm.Role.USER,
        content=[a11.to_chunk({"role": "user", "content": "Now what?"})],
    )

    thread_id, index = mod._latest_thread([answered, follow_up])
    prompt = mod._build_prompt([answered, follow_up][index + 1 :], thread_id)

    assert thread_id == "thread-10"
    assert prompt == "Now what?"


def test_inline_images_become_codex_image_arguments(tmp_path):
    interaction = llm.Interaction(
        role=llm.Role.USER,
        content=[
            a11.to_chunk(
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": "inspect this"},
                        {
                            "type": "image",
                            "data": "QUJD",
                            "mime_type": "image/png",
                        },
                    ],
                }
            )
        ],
    )

    paths = mod._write_prompt_images([interaction], str(tmp_path))

    assert len(paths) == 1
    assert paths[0].endswith(".png")
    assert Path(paths[0]).read_bytes() == b"ABC"


@pytest.mark.asyncio
async def test_final_message_records_thread_for_the_next_turn(monkeypatch):
    text, produced, _ = await _run(
        [("Finished.", "thread-7", llm.UsageMetadata(total_tokens=12))],
        monkeypatch,
    )

    assert text == ["Finished."]
    assert produced[0].backend_specific_metadata["backend"] == b"codex"
    assert (
        produced[0].backend_specific_metadata[THREAD_ID_METADATA_KEY]
        == b"thread-7"
    )
    assert produced[0].usage_metadata.total_tokens == 12


@pytest.mark.asyncio
async def test_a11_tool_call_executes_then_resumes_codex(monkeypatch):
    definition = {
        "name": "lookup",
        "description": "look up a key",
        "input_schema": {
            "type": "object",
            "properties": {"key": {"type": "string"}},
            "required": ["key"],
        },
    }
    text, produced, calls = await _run(
        [
            (
                '{"type":"tool_call","name":"lookup","arguments":{"key":"x"}}',
                "thread-8",
                None,
            ),
            ('{"type":"response","response":"Done."}', "thread-8", None),
        ],
        monkeypatch,
        tools=[definition],
    )

    assert text == ["Done."]
    assert len(produced) == 3
    assert produced[0].action_calls[0].name == "lookup"
    assert produced[1].action_outputs
    assert calls[1][4] == "thread-8"

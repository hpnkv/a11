# Copyright 2026 The A11 Authors.

"""Offline tests for the Claude Code handler.

These drive `interact_with_claude_code` against a fake `ClaudeSDKClient` that
replays a scripted message sequence, so no `claude` binary, subscription or
network is needed. The tool bridge is exercised for real: the fake calls the
`SdkMcpTool` the handler built, which runs the registry action through the
shared runner.
"""

import asyncio

import pytest

import a11
from a11.sdk.llm import Interaction, LlmHeaders, Role
from a11.status import StatusCode, StatusException

cc = pytest.importorskip("claude_agent_sdk")

from a11.sdk.anthropic import interact_with_claude_code as mod
from a11.sdk.anthropic.interact_with_claude_code_schema import (
    ClaudeCodeHeaders,
    CreateSessionConfig,
    INTERACT_WITH_CLAUDE_CODE_SCHEMA,
)


# -- a trivial registry tool the model can call -------------------------------

_GET_INFO = a11.ActionSchema(
    name="get_info",
    inputs={"path": a11.ActionPortSchema("path", "text/plain", required=True)},
    outputs={"result": a11.ActionPortSchema("result", "text/plain")},
)


async def _get_info(action: a11.Action):
    path = ""
    async for value in action["path"]:
        path = value
    await action["result"].finalize(f"listing of {path}")


_TOOL_DEF = {
    "name": "get_info",
    "description": "get info about a path",
    "input_schema": {
        "type": "object",
        "properties": {"path": {"type": "string"}},
        "required": ["path"],
    },
}


# -- fakes --------------------------------------------------------------------


class _FakeClient:
    """Replays one scripted message list in place of the Claude Code CLI."""

    last: "_FakeClient | None" = None

    def __init__(self, options, script, connect_error=None):
        self.options = options
        self._script = script
        self._connect_error = connect_error
        self.prompt: list[dict] = []
        self.disconnected = False
        _FakeClient.last = self

    async def connect(self, prompt=None):
        if self._connect_error is not None:
            raise self._connect_error
        if prompt is not None:
            self.prompt = [message async for message in prompt]

    async def receive_response(self):
        for message in await self._script():
            yield message

    async def disconnect(self):
        self.disconnected = True


def _result(session_id="sess-1"):
    return cc.ResultMessage(
        subtype="success",
        duration_ms=1,
        duration_api_ms=1,
        is_error=False,
        num_turns=1,
        session_id=session_id,
    )


def _text_delta(text, session_id="sess-1"):
    return cc.StreamEvent(
        uuid="stream-1",
        session_id=session_id,
        event={
            "type": "content_block_delta",
            "delta": {"type": "text_delta", "text": text},
        },
    )


def _thinking_delta(text, session_id="sess-1"):
    return cc.StreamEvent(
        uuid="stream-2",
        session_id=session_id,
        event={
            "type": "content_block_delta",
            "delta": {"type": "thinking_delta", "thinking": text},
        },
    )


async def _run(
    script,
    monkeypatch,
    *,
    read="text_output",
    interactions=None,
    config=None,
    headers=None,
    connect_error=None,
    captured_tools=None,
):
    """Run one turn against ``script`` and read values from ``read``.

    Mirrors how the CLI drives the action: a reader runs concurrently while the
    caller feeds inputs and drains ``new_interactions``.
    """
    if captured_tools is not None:
        real_sdk_tool = mod._sdk_tool

        def spy(action, registry, definitions, logs):
            tools = [
                real_sdk_tool(action, registry, definition, logs)
                for definition in definitions
            ]
            captured_tools.extend(tools)
            return cc.create_sdk_mcp_server(
                name=mod.MCP_SERVER_NAME, tools=tools
            )

        monkeypatch.setattr(mod, "_build_tool_server", spy)

    monkeypatch.setattr(
        mod,
        "_new_client",
        lambda options: _FakeClient(options, script, connect_error),
    )

    registry = a11.ActionRegistry()
    registry.register("get_info", _GET_INFO, _get_info)

    action = (
        a11.Action(INTERACT_WITH_CLAUDE_CODE_SCHEMA)
        .bind_handler(mod.interact_with_claude_code)
        .bind_registry(registry)
        .set_header(LlmHeaders.MODEL.value, b"claude-sonnet-4-6")
        .set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"get_info")
    )
    for name, value in (headers or {}).items():
        action = action.set_header(name, value)
    action = action.run()

    collected: list = []

    async def pump():
        async for value in action[read]:
            collected.append(value)

    reader = asyncio.create_task(pump())

    if interactions is None:
        interactions = [
            Interaction(
                role=Role.USER,
                content=[a11.to_chunk({"role": "user", "content": "hi"})],
            )
        ]
    for interaction in interactions[:-1]:
        await action["interactions"].put(interaction)
    await action["interactions"].finalize(interactions[-1])

    if config is None:
        await action["config"].finalize()
    else:
        await action["config"].finalize(config)

    tools = action["tools"]
    await tools.put(_TOOL_DEF)
    await tools.finalize()

    new_interactions = []
    async for interaction in action["new_interactions"]:
        new_interactions.append(interaction)

    await reader
    await action.wait()
    return collected, new_interactions


# -- tests --------------------------------------------------------------------


@pytest.mark.asyncio
async def test_partial_messages_stream_text(monkeypatch):
    """Token deltas reach `text_output`, and the message becomes one turn."""

    async def script():
        return [
            _text_delta("Hel"),
            _text_delta("lo."),
            cc.AssistantMessage(
                content=[cc.TextBlock(text="Hello.")],
                model="claude-sonnet-4-6",
                session_id="sess-1",
            ),
            _result(),
        ]

    text, new_interactions = await _run(script, monkeypatch)

    assert "".join(text) == "Hello."
    assert len(new_interactions) == 1
    message = a11.from_chunk(new_interactions[0].content[0])
    assert message == {
        "role": "assistant",
        "content": [{"type": "text", "text": "Hello."}],
    }
    metadata = new_interactions[0].backend_specific_metadata
    assert metadata["backend"] == b"claude_code"
    assert metadata[mod.SESSION_ID_METADATA_KEY] == b"sess-1"


@pytest.mark.asyncio
async def test_thinking_deltas_reach_thoughts(monkeypatch):
    async def script():
        return [
            _thinking_delta("weighing it up"),
            _text_delta("Done."),
            cc.AssistantMessage(
                content=[cc.TextBlock(text="Done.")],
                model="claude-sonnet-4-6",
                session_id="sess-1",
            ),
            _result(),
        ]

    thoughts, _ = await _run(script, monkeypatch, read="thoughts")

    assert "".join(thoughts) == "weighing it up"


@pytest.mark.asyncio
async def test_subagent_deltas_stay_out_of_the_answer(monkeypatch):
    async def script():
        nested = _text_delta("scratch work")
        nested.parent_tool_use_id = "toolu_sub"
        return [
            nested,
            _text_delta("Answer."),
            cc.AssistantMessage(
                content=[cc.TextBlock(text="Answer.")],
                model="claude-sonnet-4-6",
                session_id="sess-1",
            ),
            _result(),
        ]

    text, _ = await _run(script, monkeypatch)

    assert "".join(text) == "Answer."


@pytest.mark.asyncio
async def test_tool_call_runs_the_registry_action(monkeypatch):
    """The bridged MCP tool runs the action and returns its output."""
    tools: list = []

    async def script():
        result = await tools[0].handler({"path": "~"})
        text = result["content"][0]["text"]
        assert "is_error" not in result
        return [
            cc.AssistantMessage(
                content=[
                    cc.ToolUseBlock(
                        id="toolu_1",
                        name="mcp__a11__get_info",
                        input={"path": "~"},
                    )
                ],
                model="claude-sonnet-4-6",
                session_id="sess-1",
            ),
            cc.UserMessage(
                content=[
                    cc.ToolResultBlock(tool_use_id="toolu_1", content=text)
                ]
            ),
            _text_delta("You have one folder."),
            cc.AssistantMessage(
                content=[cc.TextBlock(text="You have one folder.")],
                model="claude-sonnet-4-6",
                session_id="sess-1",
            ),
            _result(),
        ]

    text, new_interactions = await _run(
        script, monkeypatch, captured_tools=tools
    )

    assert [tool.name for tool in tools] == ["get_info"]
    assert "".join(text) == "You have one folder."
    assert len(new_interactions) == 3

    call = a11.from_chunk(new_interactions[0].content[0])["content"][0]
    # The MCP prefix is the transport's, not the action's name.
    assert call == {
        "type": "tool_use",
        "id": "toolu_1",
        "name": "get_info",
        "input": {"path": "~"},
    }

    result = a11.from_chunk(new_interactions[1].content[0])["content"][0]
    assert result["type"] == "tool_result"
    # The action's outputs, keyed by port, as every backend encodes them.
    assert result["content"] == '{"result":"listing of ~"}'

    # The SDK ran the call, so the interaction records it without offering it
    # to the caller to run again.
    assert not any(
        interaction.action_calls for interaction in new_interactions
    )


@pytest.mark.asyncio
async def test_tool_failure_is_reported_to_the_model(monkeypatch):
    tools: list = []

    async def script():
        result = await tools[0].handler({"nonexistent": 1})
        assert result["is_error"] is True
        return [
            _text_delta("That failed."),
            cc.AssistantMessage(
                content=[cc.TextBlock(text="That failed.")],
                model="claude-sonnet-4-6",
                session_id="sess-1",
            ),
            _result(),
        ]

    text, _ = await _run(script, monkeypatch, captured_tools=tools)

    assert "".join(text) == "That failed."


@pytest.mark.asyncio
async def test_the_turn_reports_the_result_usage(monkeypatch):
    """The closing result's counts land on the turn's last assistant message.

    An `AssistantMessage` carries a snapshot taken as it begins — the same few
    output tokens for every message, and no thinking tokens — so only the
    `ResultMessage` totals become `usage_metadata`.
    """
    snapshot = {"input_tokens": 142, "output_tokens": 3}

    async def script():
        return [
            cc.AssistantMessage(
                content=[cc.TextBlock(text="One moment.")],
                model="claude-sonnet-4-6",
                usage=snapshot,
                session_id="sess-1",
            ),
            cc.AssistantMessage(
                content=[cc.TextBlock(text="Three.")],
                model="claude-sonnet-4-6",
                usage=snapshot,
                session_id="sess-1",
            ),
            cc.ResultMessage(
                subtype="success",
                duration_ms=1200,
                duration_api_ms=900,
                is_error=False,
                num_turns=1,
                session_id="sess-1",
                total_cost_usd=0.0004,
                usage={
                    "input_tokens": 142,
                    "output_tokens": 57,
                    "cache_read_input_tokens": 0,
                    "cache_creation_input_tokens": 0,
                    "output_tokens_details": {"thinking_tokens": 43},
                },
            ),
        ]

    _, new_interactions = await _run(script, monkeypatch)

    assert len(new_interactions) == 2
    assert new_interactions[0].usage_metadata is None

    usage = new_interactions[-1].usage_metadata
    assert usage.input_tokens == 142
    assert usage.output_tokens == 57
    assert usage.reasoning_tokens == 43
    assert usage.total_tokens == 199

    metadata = new_interactions[-1].backend_specific_metadata
    assert metadata["total_cost_usd"] == b"0.0004"
    # The per-message snapshot stays, unpromoted.
    assert b"142" in metadata["message_usage"]


@pytest.mark.asyncio
async def test_builtin_tools_are_off_by_default(monkeypatch):
    async def script():
        return [_result()]

    await _run(script, monkeypatch)

    options = _FakeClient.last.options
    assert options.tools == []
    assert options.strict_mcp_config is True
    assert options.include_partial_messages is True
    assert options.setting_sources == []
    # The gate is the session's only approval point; an `allowed_tools` entry
    # would approve a call before the gate could refuse anything else.
    assert options.allowed_tools == []


@pytest.mark.asyncio
async def test_builtin_tools_switch_on(monkeypatch):
    async def script():
        return [_result()]

    await _run(
        script,
        monkeypatch,
        config=CreateSessionConfig(
            builtin_tools=True, permission_mode="acceptEdits"
        ),
    )

    options = _FakeClient.last.options
    assert options.tools == {"type": "preset", "preset": "claude_code"}
    assert options.permission_mode == "acceptEdits"


@pytest.mark.asyncio
async def test_builtin_tools_accept_a_subset(monkeypatch):
    async def script():
        return [_result()]

    await _run(
        script,
        monkeypatch,
        config=CreateSessionConfig(builtin_tools=["Read", "Grep"]),
    )

    assert _FakeClient.last.options.tools == ["Read", "Grep"]


@pytest.mark.asyncio
async def test_permission_gate_denies_an_unlisted_tool(monkeypatch):
    """Claude Code consults this for MCP tools; its own are permitted by being
    enabled, so the gate is the backstop for anything the turn did not offer."""

    async def script():
        return [_result()]

    await _run(script, monkeypatch)

    gate = _FakeClient.last.options.can_use_tool
    allowed = await gate("mcp__a11__get_info", {"path": "~"}, None)
    denied = await gate("Bash", {"command": "rm -rf /"}, None)
    assert allowed.behavior == "allow"
    assert denied.behavior == "deny"
    assert "permission_mode" in denied.message


@pytest.mark.asyncio
async def test_system_instructions_are_the_prompt(monkeypatch):
    async def script():
        return [_result()]

    await _run(
        script,
        monkeypatch,
        interactions=[
            Interaction(
                role=Role.USER,
                content=[a11.to_chunk({"role": "user", "content": "hi"})],
                system_instructions=[a11.to_chunk("Be terse.")],
            )
        ],
    )

    assert _FakeClient.last.options.system_prompt == "Be terse."


@pytest.mark.asyncio
async def test_preset_header_appends_the_instructions(monkeypatch):
    async def script():
        return [_result()]

    await _run(
        script,
        monkeypatch,
        headers={ClaudeCodeHeaders.SYSTEM_PRESET.value: b"claude_code"},
        interactions=[
            Interaction(
                role=Role.USER,
                content=[a11.to_chunk({"role": "user", "content": "hi"})],
                system_instructions=[a11.to_chunk("Be terse.")],
            )
        ],
    )

    assert _FakeClient.last.options.system_prompt == {
        "type": "preset",
        "preset": "claude_code",
        "append": "Be terse.",
    }


@pytest.mark.asyncio
async def test_history_resumes_the_recorded_session(monkeypatch):
    """A history this provider produced continues its own session."""

    async def script():
        return [
            cc.AssistantMessage(
                content=[cc.TextBlock(text="Still here.")],
                model="claude-sonnet-4-6",
                session_id="sess-7",
            ),
            _result("sess-7"),
        ]

    first = Interaction(
        role=Role.USER,
        content=[a11.to_chunk({"role": "user", "content": "hello"})],
    )
    answered = Interaction(
        previous_interaction_id=first.id,
        role=Role.ASSISTANT,
        content=[
            a11.to_chunk(
                {
                    "role": "assistant",
                    "content": [{"type": "text", "text": "hi there"}],
                }
            )
        ],
        backend_specific_metadata={
            "backend": b"claude_code",
            mod.SESSION_ID_METADATA_KEY: b"sess-7",
        },
    )
    follow_up = Interaction(
        previous_interaction_id=answered.id,
        role=Role.USER,
        content=[a11.to_chunk({"role": "user", "content": "and now?"})],
    )

    await _run(
        script, monkeypatch, interactions=[first, answered, follow_up]
    )

    client = _FakeClient.last
    assert client.options.resume == "sess-7"
    # Only the turn the resumed session has not seen is sent.
    assert client.prompt[0]["message"]["content"] == "and now?"


@pytest.mark.asyncio
async def test_history_without_a_session_is_replayed(monkeypatch):
    async def script():
        return [_result()]

    first = Interaction(
        role=Role.USER,
        content=[a11.to_chunk({"role": "user", "content": "hello"})],
    )
    answered = Interaction(
        previous_interaction_id=first.id,
        role=Role.ASSISTANT,
        content=[
            a11.to_chunk(
                {
                    "role": "assistant",
                    "content": [{"type": "text", "text": "hi there"}],
                }
            )
        ],
    )
    follow_up = Interaction(
        previous_interaction_id=answered.id,
        role=Role.USER,
        content=[a11.to_chunk({"role": "user", "content": "and now?"})],
    )

    await _run(
        script, monkeypatch, interactions=[first, answered, follow_up]
    )

    client = _FakeClient.last
    assert client.options.resume is None
    prompt = client.prompt[0]["message"]["content"]
    assert "User: hello" in prompt
    assert "Assistant: hi there" in prompt
    assert prompt.endswith("and now?")


@pytest.mark.asyncio
async def test_a_resumed_session_needs_a_new_turn(monkeypatch):
    """A history ending at the recorded session leaves nothing to send."""

    async def script():
        return [_result()]

    first = Interaction(
        role=Role.USER,
        content=[a11.to_chunk({"role": "user", "content": "hello"})],
    )
    answered = Interaction(
        previous_interaction_id=first.id,
        role=Role.ASSISTANT,
        content=[
            a11.to_chunk(
                {
                    "role": "assistant",
                    "content": [{"type": "text", "text": "hi there"}],
                }
            )
        ],
        backend_specific_metadata={
            "backend": b"claude_code",
            mod.SESSION_ID_METADATA_KEY: b"sess-7",
        },
    )

    with pytest.raises(StatusException) as caught:
        await _run(script, monkeypatch, interactions=[first, answered])

    assert caught.value.status.code == StatusCode.INVALID_ARGUMENT


@pytest.mark.asyncio
async def test_missing_cli_is_a_precondition(monkeypatch):
    async def script():
        return []

    with pytest.raises(StatusException) as caught:
        await _run(
            script,
            monkeypatch,
            connect_error=cc.CLINotFoundError("no claude on PATH"),
        )

    assert caught.value.status.code == StatusCode.FAILED_PRECONDITION
    assert "npm install" in caught.value.status.message


@pytest.mark.asyncio
async def test_authentication_failure_is_unauthenticated(monkeypatch):
    async def script():
        return [
            cc.AssistantMessage(
                content=[],
                model="claude-sonnet-4-6",
                error="authentication_failed",
                session_id="sess-1",
            ),
            _result(),
        ]

    with pytest.raises(StatusException) as caught:
        await _run(script, monkeypatch)

    assert caught.value.status.code == StatusCode.UNAUTHENTICATED
    assert "claude" in caught.value.status.message


@pytest.mark.asyncio
async def test_failed_result_ends_the_turn(monkeypatch):
    async def script():
        return [
            cc.ResultMessage(
                subtype="error_during_execution",
                duration_ms=1,
                duration_api_ms=1,
                is_error=True,
                num_turns=1,
                session_id="sess-1",
                errors=["the session ran out of turns"],
            )
        ]

    with pytest.raises(StatusException) as caught:
        await _run(script, monkeypatch)

    assert caught.value.status.code == StatusCode.INTERNAL
    assert "ran out of turns" in caught.value.status.message


@pytest.mark.asyncio
async def test_the_client_is_always_disconnected(monkeypatch):
    async def script():
        return [_result()]

    await _run(script, monkeypatch)

    assert _FakeClient.last.disconnected is True

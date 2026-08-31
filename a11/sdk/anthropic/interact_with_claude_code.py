# Copyright 2026 The A11 Authors.

"""Drive one conversational turn through a Claude Code subscription.

The credential is the one the `claude` CLI holds, so this provider reads no API
key. `claude_agent_sdk` owns the agent loop, which inverts the arrangement the
other providers use: A11 registry actions are published to the SDK as an
in-process MCP server and the handler consumes the resulting message stream
rather than running tool calls between rounds itself.
"""

from __future__ import annotations

import dataclasses
import json
import traceback
import uuid
from typing import Any, AsyncIterator

from absl import logging

import a11
import claude_agent_sdk as cc

from a11.status import Status, StatusCode, StatusException

from a11.sdk import llm
from a11.sdk.anthropic.interact_with_claude_code_schema import (
    CLAUDE_CODE_PRESET,
    ClaudeCodeHeaders,
    CreateSessionConfig,
)
from a11.sdk.anthropic.messages import Conversation, to_normalized
from a11.sdk.llm_tools import runner


llm.register_interaction_normalizer(llm.Backend.CLAUDE_CODE, to_normalized)


#: MCP server name the registry's actions are published under.
MCP_SERVER_NAME = "a11"

_TOOL_PREFIX = f"mcp__{MCP_SERVER_NAME}__"

#: Interaction metadata key holding the Claude Code session to resume.
SESSION_ID_METADATA_KEY = "session_id"

_NATIVE_BACKENDS = (llm.Backend.CLAUDE, llm.Backend.CLAUDE_CODE)


def _new_client(options: cc.ClaudeAgentOptions) -> cc.ClaudeSDKClient:
    """The SDK client for one session."""
    return cc.ClaudeSDKClient(options=options)


# --- tool bridge -------------------------------------------------------------


def _qualified_tool_name(name: str) -> str:
    return f"{_TOOL_PREFIX}{name}"


def _action_name(tool_name: str) -> str:
    """The registry action behind an MCP tool name."""
    if tool_name.startswith(_TOOL_PREFIX):
        return tool_name[len(_TOOL_PREFIX) :]
    return tool_name


def _as_mcp_result(
    call_id: str, content: str, failure: str | None
) -> dict[str, Any]:
    """One action's outputs, or why it failed, as an MCP tool result."""
    result: dict[str, Any] = {"content": [{"type": "text", "text": content}]}
    if failure is not None:
        result["is_error"] = True
    return result


def _sdk_tool(
    action: a11.Action,
    registry: a11.ActionRegistry,
    definition: dict[str, Any],
    logs: dict[str, str],
) -> Any:
    """One registry action as an SDK MCP tool."""
    name = definition["name"]
    schema = definition.get("input_schema") or {
        "type": "object",
        "properties": {},
    }

    @cc.tool(name, definition.get("description", ""), schema)
    async def run(args: dict[str, Any]) -> dict[str, Any]:
        call = llm.ToolCall(
            name=name, id=f"toolu_{uuid.uuid4().hex}", params=args or {}
        )
        holder = llm.Interaction(
            role=llm.Role.ASSISTANT,
            created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
        )
        try:
            # Arguments the action's ports refuse are this call's failure, and
            # the model is owed a result it can read rather than a dead turn.
            await llm.add_tool_calls_to_interaction([call], holder, registry)
        except StatusException as error:
            return _as_mcp_result(call.id, str(error.status), str(error))
        executed = await runner.execute_actions_from_interaction(
            holder, action, registry
        )
        logs.update(executed.logs)
        results = await llm.build_tool_results(executed, _as_mcp_result)
        return results[0] if results else {"content": []}

    return run


def _build_tool_server(
    action: a11.Action,
    registry: a11.ActionRegistry,
    definitions: list[dict[str, Any]],
    logs: dict[str, str],
) -> Any:
    return cc.create_sdk_mcp_server(
        name=MCP_SERVER_NAME,
        tools=[
            _sdk_tool(action, registry, definition, logs)
            for definition in definitions
        ],
    )


def _permission_gate(allowed: set[str]):
    """Approve the allow-list's actions, and deny any other MCP tool.

    The A11 actions are not also named in `allowed_tools`: an entry there
    approves a call before the callback is consulted, leaving the callback
    nothing to refuse anything else with. A11 raises no interactive prompt, so
    refusal is the only answer available here.

    Claude Code consults this for MCP tools only. Its own tools are permitted
    by being enabled, so `builtin_tools` grants what it names and
    `disallowed_tools` is what takes a tool or a command shape back.
    """

    async def can_use_tool(
        tool_name: str, input_data: dict[str, Any], context: Any
    ):
        if tool_name in allowed:
            return cc.PermissionResultAllow(updated_input=input_data)
        return cc.PermissionResultDeny(
            message=(
                f"{tool_name} needs approval, which this session cannot ask"
                " for. Set permission_mode to approve it, or drop the call."
            )
        )

    return can_use_tool


# --- options -----------------------------------------------------------------


def _build_builtin_tools(config: CreateSessionConfig) -> Any:
    """Claude Code's own tools, off unless the config asks for them."""
    if config.builtin_tools is True:
        return {"type": "preset", "preset": "claude_code"}
    if config.builtin_tools:
        return list(config.builtin_tools)
    return []


def _build_system_prompt(system_prompt: str | None, preset: str | None) -> Any:
    """The session's system prompt, honouring the preset header."""
    if preset == CLAUDE_CODE_PRESET:
        prompt: dict[str, Any] = {"type": "preset", "preset": "claude_code"}
        if system_prompt:
            prompt["append"] = system_prompt
        return prompt
    return system_prompt


def _build_thinking(config: CreateSessionConfig) -> Any:
    if not config.thinking:
        return None
    thinking: dict[str, Any] = {"type": "adaptive"}
    if config.thinking_summaries:
        thinking["display"] = "summarized"
    return thinking


def _build_options(
    config: CreateSessionConfig,
    model: str | None,
    system_prompt: str | None,
    preset: str | None,
    server: Any,
    tool_names: list[str],
    resume: str | None,
) -> cc.ClaudeAgentOptions:
    return cc.ClaudeAgentOptions(
        tools=_build_builtin_tools(config),
        can_use_tool=_permission_gate(set(tool_names)),
        disallowed_tools=list(config.disallowed_tools),
        mcp_servers={MCP_SERVER_NAME: server} if server is not None else {},
        # Only the servers assembled here, so a project's `.mcp.json` cannot
        # add tools the allow-list never admitted.
        strict_mcp_config=True,
        system_prompt=_build_system_prompt(system_prompt, preset),
        model=model or None,
        fallback_model=config.fallback_model,
        permission_mode=config.permission_mode,
        max_turns=config.max_turns,
        max_budget_usd=config.max_budget_usd,
        cwd=config.cwd,
        add_dirs=list(config.add_dirs),
        # An empty list keeps the session independent of the host's on-disk
        # Claude Code settings; `None` would load all of them.
        setting_sources=(
            config.setting_sources if config.setting_sources is not None else []
        ),
        skills=config.skills,
        thinking=_build_thinking(config),
        effort=config.effort,
        resume=resume,
        fork_session=config.fork_session,
        cli_path=config.cli_path,
        # Partial messages are what make `text_output` a token stream rather
        # than one value per completed message.
        include_partial_messages=True,
    )


# --- prompt ------------------------------------------------------------------


def _message_text(message: dict[str, Any]) -> str:
    """The readable text of one Anthropic-shaped message."""
    content = message.get("content")
    if isinstance(content, str):
        return content
    parts: list[str] = []
    for block in content or []:
        if isinstance(block, str):
            parts.append(block)
            continue
        if block.get("type") == "text":
            parts.append(block.get("text", ""))
        elif block.get("type") == "tool_use":
            parts.append(f"[called {block.get('name', '')}]")
        elif block.get("type") == "tool_result":
            parts.append(
                f"[result] {llm.stringify_content(block.get('content'))}"
            )
    return "\n".join(part for part in parts if part)


def _resume_point(
    conversation: Conversation, config: CreateSessionConfig
) -> tuple[str | None, list[dict[str, Any]]]:
    """The session to resume, and the messages it has not seen.

    Claude Code keeps the transcript on its own side while A11 replays the
    whole history each turn. Where the history names a session this provider
    produced, that session is resumed and only the turns after it are sent.
    """
    if config.resume:
        return config.resume, list(conversation.messages)

    for index in range(len(conversation.interactions) - 1, -1, -1):
        interaction = conversation.interactions[index]
        backend = llm.interaction_backend(interaction)
        if backend != llm.Backend.CLAUDE_CODE:
            continue
        session_id = interaction.backend_specific_metadata.get(
            SESSION_ID_METADATA_KEY
        )
        if not session_id:
            continue
        if isinstance(session_id, bytes):
            session_id = session_id.decode()
        return session_id, list(conversation.messages[index + 1 :])

    return None, list(conversation.messages)


def _prompt_text(messages: list[dict[str, Any]]) -> str:
    """The pending turns as one user prompt.

    A single user message is sent as written. Anything longer is rendered as a
    labelled transcript so a history from another backend still reaches the
    model in order.
    """
    if not messages:
        return ""
    if len(messages) == 1 and messages[0].get("role") == "user":
        return _message_text(messages[0])

    lines = ["<conversation>"]
    for message in messages[:-1]:
        role = "Assistant" if message.get("role") == "assistant" else "User"
        lines.append(f"{role}: {_message_text(message)}")
    lines.append("</conversation>")
    lines.append(_message_text(messages[-1]))
    return "\n".join(lines)


async def _prompt_stream(text: str) -> AsyncIterator[dict[str, Any]]:
    """The turn's prompt, in the CLI's `stream-json` input shape."""
    yield {
        "type": "user",
        "session_id": "",
        "message": {"role": "user", "content": text},
        "parent_tool_use_id": None,
    }


# --- messages out ------------------------------------------------------------


def _block_dict(block: Any) -> dict[str, Any]:
    """One SDK content block in Anthropic wire shape."""
    if isinstance(block, cc.TextBlock):
        return {"type": "text", "text": block.text}
    if isinstance(block, cc.ThinkingBlock):
        return {
            "type": "thinking",
            "thinking": block.thinking,
            "signature": block.signature,
        }
    if isinstance(block, cc.ToolUseBlock):
        return {
            "type": "tool_use",
            "id": block.id,
            "name": _action_name(block.name),
            "input": block.input,
        }
    if isinstance(block, cc.ToolResultBlock):
        result: dict[str, Any] = {
            "type": "tool_result",
            "tool_use_id": block.tool_use_id,
            "content": block.content,
        }
        if block.is_error:
            result["is_error"] = True
        return result
    if dataclasses.is_dataclass(block):
        return {
            "type": _snake_case(type(block).__name__),
            **dataclasses.asdict(block),
        }
    return {"type": "text", "text": llm.stringify_content(block)}


def _snake_case(name: str) -> str:
    out: list[str] = []
    for index, character in enumerate(name):
        if character.isupper() and index:
            out.append("_")
        out.append(character.lower())
    return "".join(out)


def _as_event(message: Any) -> dict[str, Any]:
    """One SDK message as a JSON-encodable event."""
    payload: dict[str, Any] = {"type": _snake_case(type(message).__name__)}
    if dataclasses.is_dataclass(message):
        payload.update(dataclasses.asdict(message))
    return payload


def _build_usage_metadata(
    usage: dict[str, Any] | None,
) -> llm.UsageMetadata | None:
    """Map the CLI's usage dict onto the provider-independent model.

    Only a `ResultMessage`'s usage is passed here. The dict an
    `AssistantMessage` carries is a snapshot taken as the message begins: it
    reports the same few output tokens for every message in a turn and omits
    thinking tokens, so it is kept as raw metadata instead.
    """
    if not usage:
        return None

    input_tokens = usage.get("input_tokens")
    output_tokens = usage.get("output_tokens")
    cache_read = usage.get("cache_read_input_tokens")
    cache_write = usage.get("cache_creation_input_tokens")
    total = (input_tokens or 0) + (output_tokens or 0)
    total += (cache_read or 0) + (cache_write or 0)
    details = usage.get("output_tokens_details") or {}

    return llm.UsageMetadata(
        input_tokens=input_tokens,
        output_tokens=output_tokens,
        total_tokens=total or None,
        cached_input_tokens=cache_read,
        cache_write_tokens=cache_write,
        reasoning_tokens=details.get("thinking_tokens"),
    )


#: `ResultMessage` usage counters specific to Anthropic, which the shared
#: `UsageMetadata` has no place for.
_RESULT_USAGE_METADATA = (
    "service_tier",
    "inference_geo",
    "server_tool_use",
    "cache_creation",
    "iterations",
    "speed",
)


def _result_metadata(message: cc.ResultMessage) -> dict[str, Any]:
    """What a completed turn reports beyond its token counts."""
    extra: dict[str, Any] = {
        "stop_reason": message.stop_reason,
        "total_cost_usd": message.total_cost_usd,
        "duration_ms": message.duration_ms,
        "num_turns": message.num_turns,
    }
    for field in _RESULT_USAGE_METADATA:
        extra[field] = (message.usage or {}).get(field)
    return extra


def _backend_metadata(
    session_id: str | None, extra: dict[str, Any] | None = None
) -> dict[str, bytes]:
    metadata: dict[str, bytes] = {
        llm.BACKEND_METADATA_KEY: str(llm.Backend.CLAUDE_CODE).encode()
    }
    if session_id:
        metadata[SESSION_ID_METADATA_KEY] = session_id.encode()
    for field, value in (extra or {}).items():
        if value is not None:
            metadata[field] = llm.encode_backend_value(value)
    return metadata


def _tool_log_metadata(logs: dict[str, str]) -> dict[str, bytes]:
    """This round's tool narration as interaction metadata."""
    if not logs:
        return {}
    return {llm.TOOL_LOGS_METADATA_KEY: json.dumps(logs).encode()}


def _check_assistant_error(message: cc.AssistantMessage) -> None:
    if message.error is None:
        return
    if message.error == "authentication_failed":
        raise Status(
            code=StatusCode.UNAUTHENTICATED,
            message=(
                "The Claude Code CLI is not authenticated. Run `claude` and"
                " sign in to the subscription that should serve this session."
            ),
        ).to_exception()
    raise Status(
        code=StatusCode.INTERNAL,
        message=f"Claude Code reported {message.error}.",
    ).to_exception()


def _check_result(message: cc.ResultMessage) -> None:
    if not message.is_error:
        return
    errors = getattr(message, "errors", None) or []
    detail = "; ".join(errors) or message.subtype
    raise Status(
        code=StatusCode.INTERNAL,
        message=f"The Claude Code session ended with {detail}.",
    ).to_exception()


# --- handler -----------------------------------------------------------------


async def interact_with_claude_code(action: a11.Action):
    deadline = a11.get_deadline(action)

    def remaining_timeout():
        return max(deadline - a11.now(), a11.zero_duration())

    model = action.get_header(llm.LlmHeaders.MODEL.value, decode=True)
    preset = action.get_header(
        ClaudeCodeHeaders.SYSTEM_PRESET.value, decode=True
    )

    config = await action["config"].consume(
        CreateSessionConfig, timeout=remaining_timeout(), allow_none=True
    )
    if config is None:
        config = CreateSessionConfig()

    previous_interaction_id = ""
    conversation = Conversation(native_backends=_NATIVE_BACKENDS)
    async for interaction in action["interactions"]:
        interaction = conversation.feed_next_interaction(interaction)
        previous_interaction_id = interaction.id

    # Record the LLM span's model and input for tracing backends (e.g.
    # Langfuse). Guarded: tracing must never affect the interaction.
    if action.trace_id:
        try:
            action.set_span_name("Claude Code interaction")
            action.set_span_attribute("gen_ai.system", "claude_code")
            if model:
                action.set_span_attribute("gen_ai.request.model", model)
            action.set_span_input(
                [
                    {"role": message["role"], "content": message["content"]}
                    for message in conversation.messages
                ]
            )
        except Exception:
            logging.debug("failed to record LLM span input", exc_info=True)

    registry = action.get_registry()
    definitions = await runner.collect_tools(action, deadline)
    if definitions and registry is None:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Tools were offered without a registry to run them.",
        ).to_exception()

    logs: dict[str, str] = {}
    server = None
    tool_names: list[str] = []
    if definitions:
        server = _build_tool_server(action, registry, definitions, logs)
        tool_names = [
            _qualified_tool_name(definition["name"])
            for definition in definitions
        ]

    resume, pending = _resume_point(conversation, config)
    prompt = _prompt_text(pending)
    if not prompt:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="The turn carries no message the session has not seen.",
        ).to_exception()

    options = _build_options(
        config,
        model,
        conversation.system_prompt,
        preset,
        server,
        tool_names,
        resume,
    )

    client = _new_client(options)
    session_id = resume
    last_message: Any = None
    # The turn's token counts arrive with the closing `ResultMessage`, so the
    # assistant turn they belong to waits here for them.
    pending_assistant: llm.Interaction | None = None

    async def flush_assistant() -> None:
        nonlocal pending_assistant
        if pending_assistant is not None:
            await action["new_interactions"].put(pending_assistant)
            pending_assistant = None

    try:
        try:
            await client.connect(_prompt_stream(prompt))
        except cc.CLINotFoundError as exc:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=(
                    "The Claude Code CLI is not installed. Install it with"
                    f" `npm install -g @anthropic-ai/claude-code`: {exc}"
                ),
            ).to_exception() from exc

        async for message in client.receive_response():
            await action["event_stream"].put(_as_event(message))
            session_id = getattr(message, "session_id", None) or session_id

            if isinstance(message, cc.StreamEvent):
                # A sub-agent's deltas belong to its own trace, not to this
                # turn's visible answer.
                if message.parent_tool_use_id is not None:
                    continue
                event = message.event or {}
                if event.get("type") != "content_block_delta":
                    continue
                delta = event.get("delta") or {}
                if delta.get("type") == "text_delta" and delta.get("text"):
                    await action["text_output"].put(delta["text"])
                elif delta.get("type") == "thinking_delta" and delta.get(
                    "thinking"
                ):
                    await action["thoughts"].put(delta["thinking"])
                continue

            if isinstance(message, cc.AssistantMessage):
                _check_assistant_error(message)
                interaction = llm.Interaction(
                    previous_interaction_id=previous_interaction_id,
                    role=llm.Role.ASSISTANT,
                    created_at_millis=(
                        a11.now().nanoseconds_since_epoch // 1000000
                    ),
                    model=message.model,
                    content=[
                        a11.to_chunk(
                            {
                                "role": "assistant",
                                "content": [
                                    _block_dict(block)
                                    for block in message.content
                                ],
                            }
                        )
                    ],
                    backend_specific_metadata=_backend_metadata(
                        session_id,
                        {
                            "stop_reason": message.stop_reason,
                            "message_id": message.message_id,
                            "parent_tool_use_id": message.parent_tool_use_id,
                            "message_usage": message.usage,
                        },
                    ),
                )
                previous_interaction_id = interaction.id
                # The SDK already ran the calls in this message, so the
                # interaction records them without offering them again.
                await flush_assistant()
                pending_assistant = interaction
                continue

            if isinstance(message, cc.UserMessage):
                content = message.content
                blocks = (
                    [{"type": "text", "text": content}]
                    if isinstance(content, str)
                    else [_block_dict(block) for block in content]
                )
                interaction = llm.Interaction(
                    previous_interaction_id=previous_interaction_id,
                    role=llm.Role.USER,
                    created_at_millis=(
                        a11.now().nanoseconds_since_epoch // 1000000
                    ),
                    content=[
                        a11.to_chunk({"role": "user", "content": blocks})
                    ],
                    backend_specific_metadata=_backend_metadata(
                        session_id,
                        {"parent_tool_use_id": message.parent_tool_use_id},
                    )
                    # What the tools said to the user, kept beside their
                    # results rather than in them: metadata is the one part of
                    # an interaction no backend turns into provider content.
                    | _tool_log_metadata(logs),
                )
                previous_interaction_id = interaction.id
                await flush_assistant()
                await action["new_interactions"].put(interaction)
                logs.clear()
                continue

            if isinstance(message, cc.ResultMessage):
                _check_result(message)
                last_message = message
                if pending_assistant is not None:
                    pending_assistant.usage_metadata = _build_usage_metadata(
                        message.usage
                    )
                    pending_assistant.backend_specific_metadata.update(
                        _backend_metadata(session_id, _result_metadata(message))
                    )
                await flush_assistant()

        await flush_assistant()

        if action.trace_id and last_message is not None:
            try:
                action.set_span_output(_as_event(last_message))
            except Exception:
                logging.debug(
                    "failed to record LLM span output", exc_info=True
                )

    except StatusException:
        raise

    except cc.ClaudeSDKError as exc:
        raise Status(
            code=StatusCode.INTERNAL, message=str(exc)
        ).to_exception() from exc

    except Exception as e:
        tb = traceback.format_exc()
        raise Status(code=StatusCode.INTERNAL, message=tb).to_exception() from e

    else:
        await action["event_stream"].finalize()
        await action["text_output"].finalize()
        await action["thoughts"].finalize()
        await action["new_interactions"].finalize()

    finally:
        await client.disconnect()


__all__ = [
    "MCP_SERVER_NAME",
    "SESSION_ID_METADATA_KEY",
    "interact_with_claude_code",
]

# Copyright 2026 The A11 Authors.

import asyncio
import dataclasses
import traceback
from typing import Any

from absl import logging

import a11
import anthropic
import pydantic_core

from a11.status import Status, StatusCode, StatusException
from anthropic.lib.streaming._messages import accumulate_event

from a11.sdk.anthropic.client import get_anthropic_client
from a11.sdk.anthropic.interact_with_claude_schema import (
    CreateMessageConfig,
    DEFAULT_MODEL,
)
from a11.sdk import llm
from a11.sdk.anthropic.messages import Conversation, to_normalized
from a11.sdk.llm_tools import runner


llm.register_interaction_normalizer(llm.Backend.CLAUDE, to_normalized)


@dataclasses.dataclass
class _ToolCall(llm.ToolCall):
    """A tool call whose arguments arrive as a stream of JSON fragments."""

    partial_json: str = ""

    def apply_input_delta(self, partial_json: str) -> None:
        self.partial_json += partial_json

    async def finalize_params(self) -> None:
        if self.partial_json:
            self.params = await asyncio.to_thread(
                pydantic_core.from_json, self.partial_json
            )


async def _build_tool_results_from_outputs(
    executed: runner.ExecutedActions,
) -> list[dict[str, Any]]:
    """One `tool_result` per call: its outputs, or why it failed.

    A call that wrote an `image/*` output answers with a block list, since
    `tool_result.content` takes an image block beside its text.
    """

    def as_tool_result(
        call_id: str,
        content: str,
        failure: str | None,
        images: list[llm.NormalizedPart],
    ) -> dict[str, Any]:
        result: dict[str, Any] = {
            "type": "tool_result",
            "tool_use_id": call_id,
            "content": content,
        }
        if images:
            blocks: list[dict[str, Any]] = []
            if content:
                blocks.append({"type": "text", "text": content})
            blocks.extend(_image_block(image) for image in images)
            result["content"] = blocks
        if failure is not None:
            result["is_error"] = True
        return result

    return await llm.build_tool_results(executed, as_tool_result)


def _image_block(image: llm.NormalizedPart) -> dict[str, Any]:
    """One encoded frame as an Anthropic base64 image block."""
    return {
        "type": "image",
        "source": {
            "type": "base64",
            "media_type": image.mime_type or "application/octet-stream",
            "data": image.data or "",
        },
    }


def _build_usage_metadata(
    usage: anthropic.types.Usage | None,
) -> llm.UsageMetadata | None:
    """Map Anthropic's `Usage` onto the provider-independent `UsageMetadata`."""
    if usage is None:
        return None

    reasoning_tokens = None
    if usage.output_tokens_details is not None:
        reasoning_tokens = usage.output_tokens_details.thinking_tokens

    cache_read = usage.cache_read_input_tokens or 0
    cache_write = usage.cache_creation_input_tokens or 0
    # Anthropic reports `input_tokens` as the uncached remainder, so the full
    # token count for the interaction adds the cached-read and cache-write
    # input tokens on top of the (uncached) input and the output tokens.
    total_tokens = (
        usage.input_tokens + usage.output_tokens + cache_read + cache_write
    )

    return llm.UsageMetadata(
        input_tokens=usage.input_tokens,
        output_tokens=usage.output_tokens,
        total_tokens=total_tokens,
        cached_input_tokens=usage.cache_read_input_tokens,
        cache_write_tokens=usage.cache_creation_input_tokens,
        reasoning_tokens=reasoning_tokens,
    )


def _build_backend_specific_metadata(
    snapshot: anthropic.types.Message,
) -> dict[str, bytes]:
    """Collect Anthropic-specific fields that don't map onto shared models."""
    metadata: dict[str, bytes] = {
        llm.BACKEND_METADATA_KEY: str(llm.Backend.CLAUDE).encode()
    }

    for field in ("container", "stop_reason", "stop_details"):
        value = getattr(snapshot, field, None)
        if value is not None:
            metadata[field] = llm.encode_backend_value(value)

    # Usage counters that are specific to Anthropic and have no place in the
    # provider-independent `UsageMetadata`.
    usage = snapshot.usage
    if usage is not None:
        for field in (
            "service_tier",
            "inference_geo",
            "server_tool_use",
            "cache_creation",
        ):
            value = getattr(usage, field, None)
            if value is not None:
                metadata[field] = llm.encode_backend_value(value)

    return metadata


def _build_thinking(
    config: CreateMessageConfig, model: str, has_tools: bool
) -> Any:
    """Adaptive thinking config, guarded by model and tool constraints."""
    if not config.thinking or "haiku" in model or has_tools:
        return anthropic.Omit()
    thinking: dict[str, Any] = {"type": "adaptive"}
    if config.thinking_summaries:
        thinking["display"] = "summarized"
    return thinking


def _build_output_config(config: CreateMessageConfig, model: str) -> Any:
    """Output effort config, honoured only where the model supports it."""
    if config.effort is None or "haiku" in model:
        return anthropic.Omit()
    return {"effort": config.effort}


def _build_server_tools(config: CreateMessageConfig) -> list[dict[str, Any]]:
    """Claude's built-in, server-side tools enabled via config toggles."""
    tools: list[dict[str, Any]] = []
    if config.web_search:
        tools.append({"type": "web_search_20260209", "name": "web_search"})
    if config.web_fetch:
        tools.append({"type": "web_fetch_20260209", "name": "web_fetch"})
    if config.code_execution:
        tools.append(
            {"type": "code_execution_20260521", "name": "code_execution"}
        )
    return tools


async def interact_with_claude(action: a11.Action):
    deadline = a11.get_deadline(action)

    def remaining_timeout():
        return max(deadline - a11.now(), a11.zero_duration())

    api_key = action.get_header(llm.LlmHeaders.API_KEY.value, decode=True)
    if api_key is None:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="API key is required.",
        ).to_exception()
    api_key: str

    model: str = (
        action.get_header(llm.LlmHeaders.MODEL.value, decode=True)
        or DEFAULT_MODEL
    )

    config = await action["config"].consume(
        CreateMessageConfig, timeout=remaining_timeout(), allow_none=True
    )
    if config is None:
        config = CreateMessageConfig()

    previous_interaction_id = ""
    conversation = Conversation()
    async for interaction in action["interactions"]:
        interaction = conversation.feed_next_interaction(interaction)
        previous_interaction_id = interaction.id

    # Record the LLM span's model and input for tracing backends (e.g.
    # Langfuse). Guarded: tracing must never affect the interaction.
    if action.trace_id:
        try:
            action.set_span_name("Claude interaction")
            action.set_span_attribute("gen_ai.system", "anthropic")
            action.set_span_attribute("gen_ai.request.model", model)
            action.set_span_input(
                [
                    {"role": message["role"], "content": message["content"]}
                    for message in conversation.messages
                ]
            )
        except Exception:
            logging.debug("failed to record LLM span input", exc_info=True)

    client = get_anthropic_client(api_key)

    tools = await runner.collect_tools(action, deadline)
    tools.extend(_build_server_tools(config))

    thinking = _build_thinking(config, model, bool(tools))
    output_config = _build_output_config(config, model)

    try:
        failed_rounds = llm.FailedToolRounds()
        while True:
            snapshot = None
            try:
                messages = [
                    {"role": message["role"], "content": message["content"]}
                    for message in conversation.messages
                ]
                stream = await client.messages.create(
                    max_tokens=config.max_tokens,
                    messages=messages,
                    model=model,
                    cache_control={"type": "ephemeral", "ttl": "1h"},
                    system=conversation.system_prompt or anthropic.Omit(),
                    stream=True,
                    tool_choice=(
                        {"type": "auto"} if tools else anthropic.Omit()
                    ),
                    tools=tools or anthropic.Omit(),
                    thinking=thinking,
                    output_config=output_config,
                )
            except anthropic.APIError as exc:
                raise Status(
                    code=StatusCode.INTERNAL, message=str(exc)
                ).to_exception() from exc

            tool_calls: list[_ToolCall] = []
            pending_tool_calls: dict[int, _ToolCall] = {}

            async for event in stream:
                await action["event_stream"].put(event)
                snapshot = accumulate_event(
                    event=event, current_snapshot=snapshot
                )

                if event.type == "content_block_start":
                    if event.content_block.type == "tool_use":
                        pending_tool_calls[event.index] = _ToolCall(
                            name=event.content_block.name,
                            id=event.content_block.id,
                        )

                if event.type == "content_block_delta":
                    delta = event.delta

                    if delta.type == "input_json_delta":
                        pending_tool_calls[event.index].apply_input_delta(
                            delta.partial_json
                        )
                    elif delta.type == "text_delta":
                        if delta.text:
                            await action["text_output"].put(delta.text)
                    elif delta.type == "thinking_delta":
                        if delta.thinking:
                            await action["thoughts"].put(delta.thinking)

                if (
                    event.type == "content_block_stop"
                    and event.index in pending_tool_calls
                ):
                    tool_call = pending_tool_calls.pop(event.index)
                    await tool_call.finalize_params()
                    tool_calls.append(tool_call)

            if snapshot is None:
                raise Status(
                    code=StatusCode.DATA_LOSS,
                    message="No message could be accumulated.",
                ).to_exception()

            interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.ASSISTANT,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                model=snapshot.model,
                content=[
                    await asyncio.to_thread(
                        a11.to_chunk, snapshot.model_dump(exclude_none=True)
                    )
                ],
                backend_specific_metadata=_build_backend_specific_metadata(
                    snapshot
                ),
                usage_metadata=_build_usage_metadata(snapshot.usage),
            )
            previous_interaction_id = interaction.id
            rejected = await llm.add_tool_calls_to_interaction(
                tool_calls, interaction, action.get_registry()
            )

            interaction = conversation.feed_next_interaction(interaction)

            await action["new_interactions"].put(interaction)
            if not interaction.action_calls and not rejected:
                if action.trace_id:
                    try:
                        action.set_span_output(
                            snapshot.model_dump(exclude_none=True)
                        )
                    except Exception:
                        logging.debug(
                            "failed to record LLM span output", exc_info=True
                        )
                break

            executed = await runner.execute_actions_from_interaction(
                interaction, action, action.get_registry(), rejected=rejected
            )

            tool_output_interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.USER,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                action_outputs=executed.outputs,
                backend_specific_metadata={
                    llm.BACKEND_METADATA_KEY: str(llm.Backend.CLAUDE).encode(),
                    # What the tools said to the user, kept beside their
                    # results rather than in them: metadata is the one part of
                    # an interaction no backend turns into provider content.
                    **executed.log_metadata(),
                },
                content=[
                    a11.to_chunk(
                        {
                            "role": "user",
                            "content": await _build_tool_results_from_outputs(
                                executed
                            ),
                        }
                    )
                ],
            )
            previous_interaction_id = tool_output_interaction.id
            tool_output_interaction = conversation.feed_next_interaction(
                tool_output_interaction
            )

            await action["new_interactions"].put(tool_output_interaction)

            if not failed_rounds.record(executed):
                logging.warning(
                    "ending the conversation after %d rounds in which every"
                    " tool call failed",
                    failed_rounds.rounds,
                )
                break

    except StatusException:
        raise

    except Exception as e:
        tb = traceback.format_exc()
        raise Status(code=StatusCode.INTERNAL, message=tb).to_exception() from e

    else:
        await action["event_stream"].finalize()
        await action["text_output"].finalize()
        await action["thoughts"].finalize()
        await action["new_interactions"].finalize()

    finally:
        pass

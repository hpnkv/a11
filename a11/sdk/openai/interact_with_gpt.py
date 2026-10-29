# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Drive one conversational turn through the OpenAI API."""

import asyncio
import hashlib
import os
import traceback
import uuid
from collections.abc import AsyncIterator
from typing import Any

import openai
from absl import logging
from pydantic_core import from_json, to_json

import a11
from a11.sdk import llm
from a11.sdk.llm_context.runtime import (
    llm_context_handler,
    prepare_interaction_result,
    prepare_interaction_stream,
    prepare_messages,
)
from a11.sdk.llm_tools import runner
from a11.sdk.openai.interact_with_gpt_schema import (
    DEFAULT_MODEL,
    CreateChatCompletionConfig,
)
from a11.sdk.usage import UsageReporter, closing_stream, provider_event
from a11.sdk.usage import responses_usage as _responses_usage
from a11.sdk.vllm import interact_with_vllm as chat
from a11.status import Status, StatusCode, StatusException


def get_openai_client(
    base_url: str | None = None, api_key: str | None = None
) -> openai.AsyncOpenAI:
    """Return a cached OpenAI client for one endpoint and credential."""
    base_url = (base_url or os.environ.get("OPENAI_BASE_URL") or "").strip()
    api_key = api_key or os.environ.get("OPENAI_API_KEY")
    if not api_key:
        raise Status(
            code=StatusCode.UNAUTHENTICATED,
            message=(
                f"Set {llm.LlmHeaders.API_KEY.value} or OPENAI_API_KEY to use"
                " the OpenAI provider."
            ),
        ).to_exception()
    if not hasattr(get_openai_client, "_clients"):
        get_openai_client._clients = {}
    cache_key = hashlib.sha256(f"{base_url}\0{api_key}".encode()).hexdigest()
    if cache_key not in get_openai_client._clients:
        kwargs: dict[str, Any] = {"api_key": api_key}
        if base_url:
            kwargs["base_url"] = base_url
        get_openai_client._clients[cache_key] = openai.AsyncOpenAI(**kwargs)
    return get_openai_client._clients[cache_key]


def _build_request_options(
    config: CreateChatCompletionConfig,
) -> dict[str, Any]:
    options: dict[str, Any] = {}
    for field in (
        "max_completion_tokens",
        "temperature",
        "top_p",
        "presence_penalty",
        "frequency_penalty",
        "seed",
        "reasoning_effort",
        "service_tier",
    ):
        if (value := getattr(config, field)) is not None:
            options[field] = value
    if config.stop:
        options["stop"] = list(config.stop)
    if config.json_schema is not None:
        options["response_format"] = {
            "type": "json_schema",
            "json_schema": {
                "name": "response",
                "schema": config.json_schema,
                "strict": True,
            },
        }
    elif config.json_output:
        options["response_format"] = {"type": "json_object"}
    return options


def _responses_tools(tools: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Convert Chat-style function definitions to Responses tools."""
    converted = []
    for tool in tools:
        function = tool.get("function", {})
        converted.append(
            {
                "type": "function",
                "name": function.get("name", ""),
                "description": function.get("description", ""),
                "parameters": function.get("parameters", {"type": "object"}),
                "strict": function.get("strict", False),
            }
        )
    return converted


def _responses_input(messages: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Translate Chat history at the Responses request boundary.

    Text and image parts convert in new containers. Stored Chat messages,
    plain strings, and the function-call/result mapping retain their formats.
    """
    items = []
    for message in messages:
        role = message.get("role")
        if role == "tool":
            items.append(
                {
                    "type": "function_call_output",
                    "call_id": message.get("tool_call_id", ""),
                    "output": llm.stringify_content(message.get("content")),
                }
            )
            continue
        content = message.get("content")
        if isinstance(content, list):
            parts = []
            for part in content:
                if not isinstance(part, dict):
                    parts.append(part)
                elif part.get("type") == "text":
                    parts.append(
                        {
                            **part,
                            "type": (
                                "output_text"
                                if role == "assistant"
                                else "input_text"
                            ),
                        }
                    )
                elif part.get("type") == "image_url":
                    image = part.get("image_url")
                    converted = {**part, "type": "input_image"}
                    if isinstance(image, dict):
                        converted["image_url"] = image.get("url")
                        if "detail" in image:
                            converted["detail"] = image["detail"]
                    parts.append(converted)
                else:
                    parts.append(part)
            content = parts
        if content:
            items.append({"role": role, "content": content})
        for call in message.get("tool_calls") or []:
            function = call.get("function", {})
            items.append(
                {
                    "type": "function_call",
                    "call_id": call.get("id", ""),
                    "name": function.get("name", ""),
                    "arguments": function.get("arguments", "{}"),
                }
            )
    return items


def _responses_options(config: CreateChatCompletionConfig) -> dict[str, Any]:
    """Map shared GPT options onto their Responses API spellings."""
    options: dict[str, Any] = {}
    if config.max_completion_tokens is not None:
        options["max_output_tokens"] = config.max_completion_tokens
    for field in ("temperature", "top_p", "service_tier"):
        if (value := getattr(config, field)) is not None:
            options[field] = value
    if config.reasoning_effort not in {None, "none"}:
        options["reasoning"] = {
            "effort": config.reasoning_effort,
            "summary": "auto",
        }
    if config.json_schema is not None:
        options["text"] = {
            "format": {
                "type": "json_schema",
                "name": "response",
                "schema": config.json_schema,
                "strict": True,
            }
        }
    elif config.json_output:
        options["text"] = {"format": {"type": "json_object"}}
    return options


def _gpt_to_normalized(interaction: llm.Interaction) -> llm.NormalizedMessage:
    return chat._vllm_to_normalized(interaction)


llm.register_interaction_normalizer(
    llm.Backend.GPT,
    _gpt_to_normalized,
    replay_metadata_keys=("response_id", "finish_reason"),
)


def _api_status(exc: openai.APIError) -> Status:
    """Map OpenAI SDK failures onto actionable A11 status codes."""
    mappings = (
        (openai.AuthenticationError, StatusCode.UNAUTHENTICATED),
        (openai.PermissionDeniedError, StatusCode.PERMISSION_DENIED),
        (openai.NotFoundError, StatusCode.NOT_FOUND),
        (openai.RateLimitError, StatusCode.RESOURCE_EXHAUSTED),
        (openai.APITimeoutError, StatusCode.DEADLINE_EXCEEDED),
        (openai.APIConnectionError, StatusCode.UNAVAILABLE),
        (openai.BadRequestError, StatusCode.INVALID_ARGUMENT),
    )
    for error_type, code in mappings:
        if isinstance(exc, error_type):
            return Status(code=code, message=str(exc))
    return Status(code=StatusCode.INTERNAL, message=str(exc))


async def _raw_response_events(
    client: openai.AsyncOpenAI, request: dict[str, Any]
) -> AsyncIterator[dict[str, Any]]:
    """Decode Responses SSE without validating transient event shapes.

    Some OpenAI-compatible endpoints emit ``name: null`` on the redundant
    ``response.function_call_arguments.done`` event. The SDK's typed stream
    rejects that event before callers can ignore it, even though the complete
    function call follows in ``response.output_item.done``. Reading the same
    response as SSE keeps A11 tolerant of that harmless wire variation while
    still using the official client for transport, authentication, retries,
    deadlines, and HTTP error handling.
    """
    data_lines: list[str] = []
    response = client.responses.with_streaming_response.create(**request)
    async with response as raw:
        async for raw_line in raw.iter_lines():
            line = (
                raw_line.decode("utf-8", errors="replace")
                if isinstance(raw_line, bytes)
                else raw_line
            )
            if not line:
                if data_lines:
                    payload = "\n".join(data_lines)
                    data_lines.clear()
                    if payload != "[DONE]":
                        event = from_json(payload)
                        if isinstance(event, dict):
                            yield event
                continue
            if line.startswith("data:"):
                data_lines.append(line[5:].lstrip())
        if data_lines:
            payload = "\n".join(data_lines)
            if payload != "[DONE]":
                event = from_json(payload)
                if isinstance(event, dict):
                    yield event


async def _response_events(
    client: openai.AsyncOpenAI, request: dict[str, Any]
) -> AsyncIterator[Any]:
    """Use tolerant raw streaming, with a small seam for offline clients."""
    if hasattr(client.responses, "with_streaming_response"):
        async for event in _raw_response_events(client, request):
            yield event
        return
    stream = await client.responses.create(**request)
    async with closing_stream(stream):
        async for event in stream:
            yield event


def _event_value(source: Any, name: str, default: Any = None) -> Any:
    if isinstance(source, dict):
        return source.get(name, default)
    return getattr(source, name, default)


async def _run_responses_turn(
    action: a11.Action,
    output: llm.OrderedOutputStreams,
    client: openai.AsyncOpenAI,
    model: str,
    config: CreateChatCompletionConfig,
    conversation: chat.Conversation,
    previous_interaction_id: str,
    tools: list[dict[str, Any]],
) -> None:
    """Run tool-enabled GPT rounds on the Responses API."""
    response_input = _responses_input(conversation.messages)
    failed_rounds = llm.FailedToolRounds()
    while True:
        request: dict[str, Any] = {
            "model": model,
            "input": await prepare_messages(action, response_input),
            "tools": _responses_tools(tools),
            "tool_choice": "auto",
            "stream": True,
            **_responses_options(config),
        }
        if conversation.system_prompt:
            request["instructions"] = conversation.system_prompt
        if config.extra_body:
            request["extra_body"] = dict(config.extra_body)
        text_parts: list[str] = []
        tool_calls: list[llm.ToolCall] = []
        response = None
        reporter = UsageReporter(action, "gpt", model)
        await reporter.report(llm.UsageMetadata(), complete=False)
        try:
            async for event in _response_events(client, request):
                await action["event_stream"].put(provider_event(event))
                event_type = _event_value(event, "type", "")
                if event_type == "response.output_text.delta":
                    delta = str(_event_value(event, "delta", ""))
                    text_parts.append(delta)
                    await output.put(text=delta)
                elif event_type in {
                    "response.reasoning_summary_text.delta",
                    "response.reasoning_text.delta",
                }:
                    await output.put(
                        thought=str(_event_value(event, "delta", ""))
                    )
                elif event_type == "response.output_item.done":
                    item = _event_value(event, "item")
                    if _event_value(item, "type", "") == "function_call":
                        arguments = _event_value(item, "arguments", "{}")
                        try:
                            params = from_json(arguments)
                        except (TypeError, ValueError):
                            params = {}
                        tool_calls.append(
                            llm.ToolCall(
                                name=str(_event_value(item, "name", "")),
                                id=str(_event_value(item, "call_id", "")),
                                params=(
                                    params if isinstance(params, dict) else {}
                                ),
                            )
                        )
                elif event_type in {
                    "response.completed",
                    "response.incomplete",
                    "response.failed",
                }:
                    response = _event_value(event, "response")
                    await reporter.report(
                        _responses_usage(_event_value(response, "usage")),
                        _event_value(response, "usage"),
                        source_id=_event_value(response, "id"),
                    )
        except openai.APIError as exc:
            raise _api_status(exc).to_exception() from exc

        if response is None:
            raise Status(
                code=StatusCode.INTERNAL,
                message="OpenAI Responses stream ended without a response.",
            ).to_exception()
        if _event_value(response, "status", "completed") == "failed":
            error = _event_value(response, "error")
            raise Status(
                code=StatusCode.INTERNAL,
                message=str(_event_value(error, "message", error) or "failed"),
            ).to_exception()

        message = {
            "role": "assistant",
            "content": "".join(text_parts) or None,
            "tool_calls": [
                {
                    "id": call.id,
                    "type": "function",
                    "function": {
                        "name": call.name,
                        "arguments": to_json(call.params).decode(),
                    },
                }
                for call in tool_calls
            ],
        }
        interaction = llm.Interaction(
            previous_interaction_id=previous_interaction_id,
            role=llm.Role.ASSISTANT,
            created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
            model=str(_event_value(response, "model", model)),
            content=[await asyncio.to_thread(a11.to_chunk, message)],
            backend_specific_metadata={
                llm.BACKEND_METADATA_KEY: str(llm.Backend.GPT).encode(),
                "provider_usage": llm.encode_backend_value(
                    _event_value(response, "usage")
                ),
            },
            usage_metadata=reporter.usage,
        )
        previous_interaction_id = interaction.id
        rejected = await llm.add_tool_calls_to_interaction(
            tool_calls, interaction, action.get_registry()
        )
        interaction = conversation.feed_next_interaction(interaction)
        await action["new_interactions"].put(interaction)
        if not interaction.action_calls and not rejected:
            break

        executed = await runner.execute_actions_from_interaction(
            interaction,
            action,
            action.get_registry(),
            rejected=rejected,
        )
        tool_messages = await chat._build_tool_results_from_outputs(
            executed, action=action
        )
        result = llm.Interaction(
            previous_interaction_id=previous_interaction_id,
            role=llm.Role.USER,
            created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
            action_outputs=executed.outputs,
            backend_specific_metadata={
                llm.BACKEND_METADATA_KEY: str(llm.Backend.GPT).encode(),
                **executed.log_metadata(),
            },
            content=[a11.to_chunk({"messages": tool_messages})],
        )
        previous_interaction_id = result.id
        projected_result = await prepare_interaction_result(action, result)
        conversation.feed_next_interaction(projected_result)
        await action["new_interactions"].put(result)
        if projected_result is not result:
            tool_messages = chat._vllm_from_normalized(
                llm.normalize_interaction(projected_result)
            )
        response_output = _event_value(response, "output", [])
        response_input.extend(
            [
                (
                    item.model_dump(exclude_none=True)
                    if hasattr(item, "model_dump")
                    else item
                )
                for item in response_output
            ]
        )
        response_input.extend(
            _responses_input(
                [
                    message
                    for message in tool_messages
                    if isinstance(message, dict)
                ]
            )
        )
        if not failed_rounds.record(executed):
            break


@llm_context_handler
async def interact_with_gpt(action: a11.Action) -> None:
    """Run one OpenAI turn, including any registry-backed tool rounds."""
    output = llm.OrderedOutputStreams(action)
    deadline = a11.get_deadline(action)

    def remaining_timeout():
        return max(deadline - a11.now(), a11.zero_duration())

    config = await action["config"].consume(
        CreateChatCompletionConfig,
        timeout=remaining_timeout(),
        allow_none=True,
    )
    config = config or CreateChatCompletionConfig()
    model = (
        action.get_header(llm.LlmHeaders.MODEL.value, decode=True)
        or DEFAULT_MODEL
    )
    client = get_openai_client(
        action.get_header(llm.LlmHeaders.BASE_URL.value, decode=True),
        action.get_header(llm.LlmHeaders.API_KEY.value, decode=True),
    )

    previous_interaction_id = ""
    conversation = chat.Conversation(llm.Backend.GPT)
    async for interaction in prepare_interaction_stream(
        action,
        action["interactions"],
        stateful=bool(config.extra_body.get("previous_response_id")),
    ):
        interaction = conversation.feed_next_interaction(interaction)
        previous_interaction_id = interaction.id

    if action.trace_id:
        try:
            action.set_span_name("OpenAI interaction")
            action.set_span_attribute("gen_ai.system", "openai")
            action.set_span_attribute("gen_ai.request.model", model)
            action.set_span_input(conversation.messages)
        except Exception:
            logging.debug("failed to record LLM span input", exc_info=True)

    tools = chat._build_tools(await runner.collect_tools(action, deadline))
    if tools:
        try:
            await _run_responses_turn(
                action,
                output,
                client,
                model,
                config,
                conversation,
                previous_interaction_id,
                tools,
            )
        except StatusException:
            raise
        except Exception as exc:
            raise Status(
                code=StatusCode.INTERNAL, message=traceback.format_exc()
            ).to_exception() from exc
        await action["event_stream"].finalize()
        await action["thoughts"].finalize()
        await action["text_output"].finalize()
        await action["new_interactions"].finalize()
        return

    options = _build_request_options(config)
    prefix = f"call_{uuid.uuid4().hex[:12]}"
    next_call_id = 0
    try:
        failed_rounds = llm.FailedToolRounds()
        while True:
            messages: list[dict[str, Any]] = []
            if conversation.system_prompt:
                messages.append(
                    {
                        "role": "system",
                        "content": conversation.system_prompt,
                    }
                )
            messages.extend(conversation.messages)
            messages = await prepare_messages(action, messages)
            request: dict[str, Any] = {
                "model": model,
                "messages": messages,
                "stream": True,
                "stream_options": {"include_usage": True},
                **options,
            }
            if tools:
                request.update(tools=tools, tool_choice="auto")
            if config.extra_body:
                request["extra_body"] = dict(config.extra_body)
            reporter = UsageReporter(action, "gpt", model)
            await reporter.report(llm.UsageMetadata(), complete=False)
            try:
                stream = await client.chat.completions.create(**request)
            except openai.APIError as exc:
                raise _api_status(exc).to_exception() from exc

            accumulator = chat._StreamAccumulator(
                prefix, next_call_id, llm.Backend.GPT
            )
            async with closing_stream(stream):
                async for chunk in stream:
                    await action["event_stream"].put(provider_event(chunk))
                    text, reasoning = accumulator.add(chunk)
                    if chunk.usage is not None:
                        await reporter.report(
                            chat._build_usage_metadata(chunk.usage),
                            chunk.usage,
                            source_id=chunk.id,
                        )
                    await output.put(text=text, thought=reasoning)
            tool_calls = await accumulator.finalize()
            next_call_id += len(tool_calls)
            message = accumulator.message_dict()
            interaction = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.ASSISTANT,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                model=accumulator.model or model,
                content=[await asyncio.to_thread(a11.to_chunk, message)],
                backend_specific_metadata=accumulator.backend_specific_metadata(),
                usage_metadata=reporter.usage,
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
                        action.set_span_output(message)
                    except Exception:
                        logging.debug(
                            "failed to record LLM span output", exc_info=True
                        )
                break

            executed = await runner.execute_actions_from_interaction(
                interaction,
                action,
                action.get_registry(),
                rejected=rejected,
            )
            result = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.USER,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                action_outputs=executed.outputs,
                backend_specific_metadata={
                    llm.BACKEND_METADATA_KEY: str(llm.Backend.GPT).encode(),
                    **executed.log_metadata(),
                },
                content=[
                    a11.to_chunk(
                        {
                            "messages": (
                                await chat._build_tool_results_from_outputs(
                                    executed, action=action
                                )
                            )
                        }
                    )
                ],
            )
            previous_interaction_id = result.id
            conversation.feed_next_interaction(
                await prepare_interaction_result(action, result)
            )
            await action["new_interactions"].put(result)
            if not failed_rounds.record(executed):
                break
    except StatusException:
        raise
    except Exception as exc:
        raise Status(
            code=StatusCode.INTERNAL, message=traceback.format_exc()
        ).to_exception() from exc
    else:
        await action["event_stream"].finalize()
        await action["thoughts"].finalize()
        await action["text_output"].finalize()
        await action["new_interactions"].finalize()

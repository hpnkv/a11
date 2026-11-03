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

"""Run one A11 conversation turn through OpenRouter Chat Completions."""

import asyncio
import traceback
import uuid
from typing import Any

import openai
import pydantic_core
from absl import logging

import a11
from a11.sdk import llm
from a11.sdk.llm_context.runtime import (
    llm_context_handler,
    prepare_interaction_result,
    prepare_interaction_stream,
    prepare_messages,
)
from a11.sdk.llm_tools import runner
from a11.sdk.openrouter.client import get_openrouter_client
from a11.sdk.openrouter.interact_with_openrouter_schema import (
    DEFAULT_MODEL,
    CreateChatCompletionConfig,
)
from a11.sdk.usage import (
    UsageReporter,
    chat_usage,
    closing_stream,
    provider_event,
)
from a11.sdk.vllm import interact_with_vllm as chat
from a11.status import Status, StatusCode, StatusException


class Conversation(chat.Conversation):
    """Replay native OpenRouter messages and portable foreign interactions."""

    def __init__(self):
        super().__init__(backend=llm.Backend.OPENROUTER)

    @staticmethod
    def _native_messages(interaction: llm.Interaction) -> list[dict[str, Any]]:
        if interaction.role == llm.Role.SYSTEM:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Use system_instructions for an OpenRouter system prompt."
                ),
            ).to_exception()
        messages = chat.Conversation._native_messages(interaction)
        content = a11.from_chunk(interaction.content[0])
        if (
            interaction.role == llm.Role.ASSISTANT
            and isinstance(content, dict)
            and "messages" not in content
        ):
            if details := content.get("reasoning_details"):
                messages[0]["reasoning_details"] = details
        if isinstance(content, str) or (
            isinstance(content, dict) and "messages" not in content
        ):
            messages[0]["role"] = (
                "assistant"
                if interaction.role == llm.Role.ASSISTANT
                else "user"
            )
        if isinstance(content, dict) and isinstance(
            content.get("content"), list
        ):
            messages[0]["content"] = [
                (
                    chat._image_part(part["data"], part.get("mime_type"))
                    if isinstance(part, dict)
                    and part.get("type") == "image"
                    and part.get("data")
                    else part
                )
                for part in content["content"]
            ]
        if isinstance(content, dict) and content.get("images"):
            previous = messages[0].get("content")
            parts = (
                list(previous)
                if isinstance(previous, list)
                else ([{"type": "text", "text": previous}] if previous else [])
            )
            messages[0]["content"] = [*parts, *content["images"]]
        return messages


def _openrouter_to_normalized(
    interaction: llm.Interaction,
) -> llm.NormalizedMessage:
    message = chat._vllm_to_normalized(interaction)
    content = a11.from_chunk(interaction.content[0])
    if isinstance(content, dict):
        message.parts.extend(
            chat._normalized_content_parts(content.get("images"))
        )
    return message


llm.register_interaction_normalizer(
    llm.Backend.OPENROUTER,
    _openrouter_to_normalized,
    replay_metadata_keys=(
        "response_id",
        "finish_reason",
        "openrouter_metadata",
        "system_fingerprint",
    ),
)


def _plain(value: Any) -> Any:
    """Convert SDK extras into JSON values without losing unknown fields."""
    return pydantic_core.from_json(llm.encode_backend_value(value))


class _StreamAccumulator(chat._StreamAccumulator):
    """Retain OpenRouter tool ids, reasoning details, and routing metadata."""

    def __init__(self, used_call_ids: set[str]):
        super().__init__(uuid.uuid4().hex, backend=llm.Backend.OPENROUTER)
        self._used_call_ids = used_call_ids
        self._provider_call_ids: dict[int, str] = {}
        self._reasoning_details: list[Any] = []
        self._images: list[dict[str, Any]] = []
        self._openrouter_metadata: Any = None
        self._system_fingerprint: str | None = None

    def add(self, chunk: Any) -> tuple[str, str]:
        text, reasoning = super().add(chunk)
        if value := getattr(chunk, "openrouter_metadata", None):
            self._openrouter_metadata = _plain(value)
        if value := getattr(chunk, "system_fingerprint", None):
            self._system_fingerprint = value
        for choice in getattr(chunk, "choices", None) or []:
            delta = getattr(choice, "delta", None)
            if delta is None:
                continue
            details = getattr(delta, "reasoning_details", None)
            if details:
                self._reasoning_details.extend(_plain(details))
            if images := getattr(delta, "images", None):
                self._images.extend(_plain(images))
        return text, reasoning

    def _add_tool_call_delta(self, tool_call: Any) -> None:
        index = getattr(tool_call, "index", None)
        if index is None:
            index = len(self._pending)
        if provider_id := getattr(tool_call, "id", None):
            self._provider_call_ids[index] = provider_id
        super()._add_tool_call_delta(tool_call)

    async def finalize(self) -> list[llm.ToolCall]:
        self._tool_calls = []
        for index in sorted(self._pending):
            entry = self._pending[index]
            params = await asyncio.to_thread(
                chat._decode_arguments, entry["name"], entry["arguments"]
            )
            call_id = (
                self._provider_call_ids.get(index) or f"call_{uuid.uuid4().hex}"
            )
            if call_id in self._used_call_ids:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=f"OpenRouter repeated tool call id {call_id!r}.",
                ).to_exception()
            self._used_call_ids.add(call_id)
            self._tool_calls.append(
                llm.ToolCall(name=entry["name"], id=call_id, params=params)
            )
        return self._tool_calls

    def message_dict(self) -> dict[str, Any]:
        message = super().message_dict()
        if self._reasoning_details:
            message["reasoning_details"] = self._reasoning_details
        if self._images:
            message["images"] = self._images
        return message

    def backend_specific_metadata(self) -> dict[str, bytes]:
        metadata = super().backend_specific_metadata()
        if self._openrouter_metadata is not None:
            metadata["openrouter_metadata"] = llm.encode_backend_value(
                self._openrouter_metadata
            )
        if self._system_fingerprint:
            metadata["system_fingerprint"] = self._system_fingerprint.encode()
        return metadata


def _build_request_options(
    config: CreateChatCompletionConfig,
) -> dict[str, Any]:
    """Build standard Chat fields separately from OpenRouter extensions."""
    options: dict[str, Any] = {}
    for name in (
        "max_completion_tokens",
        "temperature",
        "top_p",
        "presence_penalty",
        "frequency_penalty",
        "seed",
        "user",
    ):
        if (value := getattr(config, name)) is not None:
            options[name] = value
    if config.stop:
        options["stop"] = list(config.stop)
    if config.json_schema is not None:
        options["response_format"] = {
            "type": "json_schema",
            "json_schema": {"name": "response", "schema": config.json_schema},
        }
    elif config.json_output:
        options["response_format"] = {"type": "json_object"}
    return options


def _build_extra_body(config: CreateChatCompletionConfig) -> dict[str, Any]:
    """Send caller metadata and request usage on every round."""
    body = dict(config.extra_body)
    for name in ("top_k", "min_p", "reasoning", "provider", "session_id"):
        if (value := getattr(config, name)) is not None:
            body[name] = value
    if config.models:
        body["models"] = list(config.models)
    if config.modalities:
        body["modalities"] = list(config.modalities)
    if config.metadata:
        body["metadata"] = dict(config.metadata)
    body["usage"] = {"include": True}
    return body


def _api_error(exc: openai.APIError) -> StatusException:
    if getattr(exc, "status_code", None) == 402:
        code = StatusCode.RESOURCE_EXHAUSTED
    elif isinstance(exc, openai.AuthenticationError):
        code = StatusCode.UNAUTHENTICATED
    elif isinstance(exc, openai.PermissionDeniedError):
        code = StatusCode.PERMISSION_DENIED
    elif isinstance(exc, openai.BadRequestError):
        code = StatusCode.INVALID_ARGUMENT
    elif isinstance(exc, openai.NotFoundError):
        code = StatusCode.NOT_FOUND
    elif isinstance(exc, openai.RateLimitError):
        code = StatusCode.RESOURCE_EXHAUSTED
    elif isinstance(exc, openai.APIConnectionError):
        code = StatusCode.UNAVAILABLE
    else:
        code = StatusCode.INTERNAL
    return Status(
        code=code, message=f"OpenRouter request failed: {exc}"
    ).to_exception()


@llm_context_handler
async def interact_with_openrouter(action: a11.Action) -> None:
    """Stream OpenRouter text, reasoning, tools, and per-request usage."""
    output = llm.OrderedOutputStreams(action)
    deadline = a11.get_deadline(action)

    def remaining_timeout():
        return max(deadline - a11.now(), a11.zero_duration())

    api_key = action.get_header(llm.LlmHeaders.API_KEY.value, decode=True)
    base_url = action.get_header(llm.LlmHeaders.BASE_URL.value, decode=True)
    model = (
        action.get_header(llm.LlmHeaders.MODEL.value, decode=True)
        or DEFAULT_MODEL
    )
    config = await action["config"].consume(
        CreateChatCompletionConfig, timeout=remaining_timeout(), allow_none=True
    )
    if config is None:
        config = CreateChatCompletionConfig()

    conversation = Conversation()
    used_call_ids: set[str] = set()
    previous_interaction_id = ""
    async for interaction in prepare_interaction_stream(
        action, action["interactions"]
    ):
        interaction = conversation.feed_next_interaction(interaction)
        previous_interaction_id = interaction.id
        used_call_ids.update(call.id for call in interaction.action_calls)

    client = get_openrouter_client(base_url, api_key)
    if action.trace_id:
        try:
            action.set_span_name("OpenRouter interaction")
            action.set_span_attribute("gen_ai.system", "openrouter")
            action.set_span_attribute("gen_ai.request.model", model)
            action.set_span_input(conversation.messages)
        except Exception:
            logging.debug("failed to record LLM span input", exc_info=True)

    tools = chat._build_tools(await runner.collect_tools(action, deadline))
    options = _build_request_options(config)
    extra_body = _build_extra_body(config)
    try:
        failed_rounds = llm.FailedToolRounds()
        round_index = 0
        while True:
            messages: list[dict[str, Any]] = []
            if conversation.system_prompt:
                messages.append(
                    {"role": "system", "content": conversation.system_prompt}
                )
            messages.extend(conversation.messages)
            messages = await prepare_messages(action, messages)

            request: dict[str, Any] = {
                "model": model,
                "messages": messages,
                "stream": True,
                "stream_options": {"include_usage": True},
                "extra_body": extra_body,
                **options,
            }
            if tools:
                request["tools"] = tools
                request["tool_choice"] = (
                    config.tool_choice if round_index == 0 else "auto"
                )
                if config.parallel_tool_calls is not None:
                    request["parallel_tool_calls"] = config.parallel_tool_calls

            reporter = UsageReporter(action, "openrouter", model)
            await reporter.report(llm.UsageMetadata(), complete=False)
            try:
                stream = await client.chat.completions.create(**request)
            except openai.APIError as exc:
                raise _api_error(exc) from exc

            accumulator = _StreamAccumulator(used_call_ids)
            async with closing_stream(stream):
                async for chunk in stream:
                    if error := getattr(chunk, "error", None):
                        raise Status(
                            code=StatusCode.INTERNAL,
                            message=f"OpenRouter stream error: {error}",
                        ).to_exception()
                    await action["event_stream"].put(provider_event(chunk))
                    text, reasoning = accumulator.add(chunk)
                    if (usage := getattr(chunk, "usage", None)) is not None:
                        await reporter.report(
                            chat_usage(usage),
                            usage,
                            source_id=getattr(chunk, "id", None),
                        )
                    await output.put(text=text, thought=reasoning)

            tool_calls = await accumulator.finalize()
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
            conversation.feed_next_interaction(interaction)
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
                interaction, action, action.get_registry(), rejected=rejected
            )
            tool_output = llm.Interaction(
                previous_interaction_id=previous_interaction_id,
                role=llm.Role.USER,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                action_outputs=executed.outputs,
                backend_specific_metadata={
                    llm.BACKEND_METADATA_KEY: (
                        str(llm.Backend.OPENROUTER).encode()
                    ),
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
            previous_interaction_id = tool_output.id
            conversation.feed_next_interaction(
                await prepare_interaction_result(action, tool_output)
            )
            await action["new_interactions"].put(tool_output)
            if not failed_rounds.record(executed):
                logging.warning(
                    "ending the conversation after %d failed tool rounds",
                    failed_rounds.rounds,
                )
                break
            round_index += 1
    except StatusException:
        raise
    except Exception as exc:
        raise Status(
            code=StatusCode.INTERNAL, message=traceback.format_exc()
        ).to_exception() from exc
    else:
        await action["event_stream"].finalize()
        await action["text_output"].finalize()
        await action["thoughts"].finalize()
        await action["new_interactions"].finalize()

# Copyright 2026 The A11 Authors.

"""Drive one conversational turn through the OpenAI API."""

import asyncio
import hashlib
import os
import traceback
import uuid
from typing import Any

from absl import logging

import a11
import openai

from a11.sdk import llm
from a11.sdk.llm_tools import runner
from a11.sdk.openai.interact_with_gpt_schema import (
    CreateChatCompletionConfig,
    DEFAULT_MODEL,
)
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


def _gpt_to_normalized(interaction: llm.Interaction) -> llm.NormalizedMessage:
    return chat._vllm_to_normalized(interaction)


llm.register_interaction_normalizer(llm.Backend.GPT, _gpt_to_normalized)


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


async def interact_with_gpt(action: a11.Action) -> None:
    """Run one OpenAI turn, including any registry-backed tool rounds."""
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
    async for interaction in action["interactions"]:
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
            try:
                stream = await client.chat.completions.create(**request)
            except openai.APIError as exc:
                raise _api_status(exc).to_exception() from exc

            accumulator = chat._StreamAccumulator(
                prefix, next_call_id, llm.Backend.GPT
            )
            async for chunk in stream:
                await action["event_stream"].put(chunk)
                text, reasoning = accumulator.add(chunk)
                if text:
                    await action["text_output"].put(text)
                if reasoning:
                    await action["thoughts"].put(reasoning)
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
                usage_metadata=chat._build_usage_metadata(accumulator.usage),
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
                                    executed
                                )
                            )
                        }
                    )
                ],
            )
            previous_interaction_id = result.id
            await action["new_interactions"].put(
                conversation.feed_next_interaction(result)
            )
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

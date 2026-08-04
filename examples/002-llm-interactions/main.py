import asyncio
import os
from typing import Sequence

import a11
from absl import app as absl_app
from absl import logging

from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import Interaction, LlmHeaders, Role
from a11.sdk.llm_tools import runner


async def get_weather(action: a11.Action):
    location = await action["location"].consume()
    await action["report"].put(f"It is 22°C and sunny in {location}.")
    await action["report"].drain_and_close()

    logging.info(
        "get_weather action %s called for location: %s",
        action.get_id(),
        location,
    )


GET_WEATHER_SCHEMA = a11.ActionSchema(
    name="get_weather",
    description="Get the current weather for a location.",
    inputs={
        "location": a11.ActionPortSchema(
            name="location",
            type="text/plain",
            typeinfo=str,
            description="The city and state, e.g. San Francisco, CA.",
            required=True,
        )
    },
    outputs={
        "report": a11.ActionPortSchema(
            name="report",
            type="text/plain",
            typeinfo=str,
            description="A short human-readable weather report.",
            required=True,
        )
    },
)


def _make_client_action_registry() -> a11.ActionRegistry:
    registry = a11.ActionRegistry()
    registry.register("get_weather", GET_WEATHER_SCHEMA, get_weather)
    return registry


# Everything the driver needs to know per backend now fits in a tiny table: the
# `interact_with_llm` action routes to the concrete backend by header and
# imports its SDK lazily, so there is no per-provider schema/handler/adapter to
# select here.
DEFAULT_MODEL: dict[str, str] = {
    "claude": "claude-sonnet-4-6",
    "gemini": "gemini-3.5-flash",
    "ollama": "deepseek-r1:8b",
}
API_KEY_ENV: dict[str, tuple[str, ...]] = {
    "claude": ("ANTHROPIC_API_KEY",),
    "gemini": ("GEMINI_API_KEY", "GOOGLE_API_KEY"),
}
# Providers reached over a base URL rather than a hosted API. Ollama runs
# locally, so it needs no API key — just where to find the server.
DEFAULT_BASE_URL: dict[str, str] = {
    "ollama": "http://localhost:11434",
}


def _api_key_for(provider: str) -> str:
    for env in API_KEY_ENV.get(provider, ()):
        if value := os.environ.get(env, ""):
            return value
    return ""


def _make_user_interaction(text: str) -> Interaction:
    """A backend-neutral user text message understood by every backend."""
    return Interaction(
        role=Role.USER,
        content=[
            a11.to_chunk(
                {
                    "role": "user",
                    "content": [{"type": "text", "text": text}],
                }
            )
        ],
    )


async def send_text_message(
    provider: str,
    model: str,
    text: str,
    previous_interactions: list[Interaction] | None = None,
    *,
    available_actions: a11.ActionRegistry | None = None,
) -> list[Interaction]:
    available_action_names = []
    if available_actions is not None:
        for action_name in available_actions.list_registered_actions():
            available_action_names.append(action_name)

    interact = (
        a11.Action(INTERACT_WITH_LLM_SCHEMA)
        .bind_handler(interact_with_llm)
        .bind_registry(available_actions)
        .set_header(LlmHeaders.PROVIDER.value, provider)
        .set_header(LlmHeaders.API_KEY.value, _api_key_for(provider))
        .set_header(LlmHeaders.MODEL.value, model)
        .set_header(
            LlmHeaders.BASE_URL.value, DEFAULT_BASE_URL.get(provider, "")
        )
        .set_header(
            LlmHeaders.ALLOWED_LLM_ACTIONS.value,
            ",".join(available_action_names),
        )
        .run()
    )

    previous_interactions = previous_interactions or []
    text_interaction = _make_user_interaction(text)

    # The assistant's visible text now arrives, already extracted, on the
    # `text_output` node — no need to parse the raw provider event stream.
    async def stream_text() -> None:
        async for chunk in interact["text_output"]:
            print(chunk, end="", flush=True)

    stream_task = asyncio.create_task(stream_text())

    async with (
        interact["interactions"] as interactions,
        interact["config"],
        interact["tools"] as tools,
    ):
        for interaction in previous_interactions:
            await interactions.put(interaction)
        await interactions.put_final(text_interaction)

        tool_definitions = runner.get_tool_definitions(
            available_actions, available_action_names
        )
        for tool in tool_definitions:
            await tools.put(tool)
        await tools.put_null_final()

    new_interactions = []
    try:
        async for interaction in interact["new_interactions"]:
            new_interactions.append(interaction)
        await stream_task
        return [text_interaction] + new_interactions

    finally:
        if not stream_task.done():
            stream_task.cancel()
            try:
                await stream_task
            except asyncio.CancelledError:
                pass


_HELP = (
    "Commands:\n"
    "  /model [claude|gemini|ollama] [model]  switch backend (and optionally"
    " model)\n"
    "  /exit, /quit                            leave\n"
)


def _handle_model_command(
    parts: list[str], provider: str, model: str
) -> tuple[str, str]:
    if len(parts) < 2 or parts[1] not in DEFAULT_MODEL:
        print(f"usage: /model [{'|'.join(DEFAULT_MODEL)}] [model]")
        return provider, model

    new_provider = parts[1]
    new_model = parts[2] if len(parts) > 2 else DEFAULT_MODEL[new_provider]
    # Only the hosted backends need a key; a keyless provider (e.g. Ollama) is
    # reached over its base URL instead.
    if new_provider in API_KEY_ENV and not _api_key_for(new_provider):
        print(
            f"warning: no API key set for {new_provider} (expected one of"
            f" {', '.join(API_KEY_ENV[new_provider])})."
        )
    print(f"switched to {new_provider} / {new_model}")
    return new_provider, new_model


async def main(_argv: Sequence[str]):
    interactions: list[Interaction] = []
    provider = "gemini"
    model = DEFAULT_MODEL[provider]
    registry = _make_client_action_registry()

    print(_HELP)
    print(f"backend: {provider} / {model}")

    while True:
        text = (await asyncio.to_thread(input, f"[{provider}] > ")).strip()
        if not text:
            continue
        if text.casefold() in ("/exit", "/quit"):
            break
        if text.casefold() in ("/help", "/?"):
            print(_HELP)
            continue
        if text.startswith("/model"):
            provider, model = _handle_model_command(
                text.split(), provider, model
            )
            continue

        print()
        new_interactions = await send_text_message(
            provider,
            model,
            text,
            interactions,
            available_actions=registry,
        )
        interactions.extend(new_interactions)
        print()
        print()


class GrayFormatter(logging.PythonFormatter):
    def format(self, record):
        return f"\033[90m{super().format(record)}\033[0m"


def sync_main(argv: Sequence[str]):
    logging.get_absl_handler().setFormatter(GrayFormatter())
    logging.use_absl_handler()
    logging.set_verbosity(logging.INFO)

    return asyncio.run(main(argv))


if __name__ == "__main__":
    absl_app.run(sync_main)

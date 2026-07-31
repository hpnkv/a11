import asyncio
import dataclasses
import os
from typing import Any, Awaitable, Callable, Sequence

import a11
from absl import app as absl_app
from absl import logging

from a11.sdk.anthropic.interact_with_claude import interact_with_claude
from a11.sdk.anthropic.interact_with_claude_schema import (
    ClaudeInteractionAdapter,
    CreateMessageConfig,
    INTERACT_WITH_CLAUDE_SCHEMA,
)
from a11.sdk.gemini.interact_with_gemini import interact_with_gemini
from a11.sdk.gemini.interact_with_gemini_schema import (
    CreateInteractionConfig,
    GeminiInteractionAdapter,
    INTERACT_WITH_GEMINI_SCHEMA,
)
from a11.sdk.llm import Interaction, InteractionAdapter, LlmHeaders, Role
from a11.sdk.llm_tools import runner
from a11.status import Status, StatusCode, StatusException


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


@dataclasses.dataclass(frozen=True)
class BackendSpec:
    """Everything that differs between the two interaction backends.

    A single conversation history (a flat list of `Interaction`s) is threaded
    across backends: each turn feeds the whole history into whichever backend
    is active, and the backends normalise any interactions the *other* produced.
    """

    name: str
    schema: a11.ActionSchema
    handler: Callable[[a11.Action], Awaitable[None]]
    adapter: type[InteractionAdapter]
    make_config: Callable[[], Any]
    default_model: str
    api_key_env: tuple[str, ...]


BACKENDS: dict[str, BackendSpec] = {
    "claude": BackendSpec(
        name="claude",
        schema=INTERACT_WITH_CLAUDE_SCHEMA,
        handler=interact_with_claude,
        adapter=ClaudeInteractionAdapter,
        make_config=lambda: CreateMessageConfig(max_tokens=1024),
        default_model="claude-sonnet-4-6",
        api_key_env=("ANTHROPIC_API_KEY",),
    ),
    "gemini": BackendSpec(
        name="gemini",
        schema=INTERACT_WITH_GEMINI_SCHEMA,
        handler=interact_with_gemini,
        adapter=GeminiInteractionAdapter,
        make_config=lambda: CreateInteractionConfig(max_output_tokens=1024),
        default_model="gemini-3.5-flash",
        api_key_env=("GEMINI_API_KEY", "GOOGLE_API_KEY"),
    ),
}


def _api_key_for(spec: BackendSpec) -> str:
    for env in spec.api_key_env:
        if value := os.environ.get(env, ""):
            return value
    return ""


async def _listen_to_events(
    node: a11.AsyncNode,
    on_event: Callable[[object], Awaitable[None]] | None = None,
    deadline: a11.Time = a11.infinite_future(),
):
    try:
        async for event in node.iter_with_deadline(deadline):
            if on_event is not None:
                await on_event(event)

    except StatusException:
        raise

    except Exception as e:
        raise Status(
            code=StatusCode.INTERNAL, message=str(e)
        ).to_exception() from e


async def log_events(event: object) -> None:
    logging.info("event: %s", event)


async def send_text_message(
    spec: BackendSpec,
    model: str,
    text: str,
    previous_interactions: list[Interaction] | None = None,
    *,
    on_event: Callable[[object], Awaitable[None]] | None = None,
    available_actions: a11.ActionRegistry | None = None,
) -> list[Interaction]:
    available_action_names = []
    if available_actions is not None:
        for action_name in available_actions.list_registered_actions():
            available_action_names.append(action_name)

    interact = (
        a11.Action(spec.schema)
        .bind_handler(spec.handler)
        .bind_registry(available_actions)
        .set_header(LlmHeaders.API_KEY.value, _api_key_for(spec))
        .set_header(LlmHeaders.MODEL.value, model)
        .set_header(
            LlmHeaders.ALLOWED_LLM_ACTIONS.value,
            ",".join(available_action_names),
        )
        .run()
    )

    previous_interactions = previous_interactions or []

    log_task = asyncio.create_task(
        _listen_to_events(interact["event_stream"], on_event)
    )

    async with (
        interact["interactions"] as interactions,
        interact["config"] as config_node,
        interact["tools"] as tools,
    ):
        await config_node.put_final(spec.make_config())

        for interaction in previous_interactions:
            await interactions.put(interaction)
        text_interaction = spec.adapter.make_text_message_interaction(
            text, "", Role.USER
        )
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
        return [text_interaction] + new_interactions

    finally:
        if not log_task.done():
            log_task.cancel()

        try:
            await log_task
        except asyncio.CancelledError:
            pass


_HELP = (
    "Commands:\n"
    "  /model [claude|gemini] [model]  switch backend (and optionally model)\n"
    "  /exit, /quit                     leave\n"
)


def _handle_model_command(
    parts: list[str], spec: BackendSpec, model: str
) -> tuple[BackendSpec, str]:
    if len(parts) < 2 or parts[1] not in BACKENDS:
        print(f"usage: /model [{'|'.join(BACKENDS)}] [model]")
        return spec, model

    new_spec = BACKENDS[parts[1]]
    new_model = parts[2] if len(parts) > 2 else new_spec.default_model
    if not _api_key_for(new_spec):
        print(
            f"warning: no API key set for {new_spec.name} (expected one of"
            f" {', '.join(new_spec.api_key_env)})."
        )
    print(f"switched to {new_spec.name} / {new_model}")
    return new_spec, new_model


async def main(_argv: Sequence[str]):
    interactions: list[Interaction] = []
    spec = BACKENDS["gemini"]
    model = spec.default_model
    registry = _make_client_action_registry()

    print(_HELP)
    print(f"backend: {spec.name} / {model}")

    while True:
        text = (await asyncio.to_thread(input, f"[{spec.name}] > ")).strip()
        if not text:
            continue
        if text.casefold() in ("/exit", "/quit"):
            break
        if text.casefold() in ("/help", "/?"):
            print(_HELP)
            continue
        if text.startswith("/model"):
            spec, model = _handle_model_command(text.split(), spec, model)
            continue

        print()
        new_interactions = await send_text_message(
            spec,
            model,
            text,
            interactions,
            on_event=log_events,
            available_actions=registry,
        )
        interactions.extend(new_interactions)
        print()
        print(spec.adapter(new_interactions[-1]).get_message_text())
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

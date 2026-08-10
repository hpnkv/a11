# Copyright 2026 The A11 Authors.

"""Driving one conversational turn against a gateway.

This is the loop that used to exist twice -- once in `a11 chat` and once in the
IntelliJ webview's ``runTurn`` -- and in the CLI's case did the LLM call
in-process rather than on a gateway at all. Everything that is not presentation
lives here: build the call, feed history and tools in, read the output ports,
fold what arrives into a `PresentationReducer`.

Two details are load-bearing rather than stylistic:

* The ``thoughts`` and ``new_interactions`` readers start **before** the
  ``text_output`` loop. Ports carry no global ordering guarantee, and a
  mid-turn tool run is announced on ``new_interactions`` while text is still
  streaming; draining text to completion first would show the tool run after
  the answer it preceded.
* ``await call.wait(...)`` is not optional. A turn that fails after its first
  token -- a provider error, a tool that raised, a cancelled stream -- reports
  it in the call's terminal status and nowhere else, so a driver that stops
  reading when the ports close would call that turn a success.
* **Every** output port is read, ``event_stream`` included, whether or not the
  caller wants its contents. A backend writes each raw provider chunk there, and
  an output port with no reader fills the session's per-stream buffer and then
  stops the backend mid-answer -- which presents as a turn that never finishes,
  intermittently, and more often on long answers. Draining is not optional
  either; only *caring* about the contents is.
"""

from __future__ import annotations

import asyncio
import dataclasses
from collections.abc import Callable, Sequence
from typing import Any

from a11 import observability, timing
import a11
from a11.client.connection import GatewayConnection
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import Interaction, LlmHeaders
from a11.sdk.presentation import PresentationReducer

#: Bound on one turn. Long, because a turn with several tool calls legitimately
#: takes minutes; finite, because the terminal status is the only failure report
#: and a turn that never terminates never produces one.
DEFAULT_TURN_TIMEOUT = timing.Duration.seconds(120)


@dataclasses.dataclass
class TurnConfig:
    """Which backend to run a turn against, and how."""

    provider: str
    model: str
    api_key: str = ""
    base_url: str = ""
    #: Regex patterns for the gateway-side actions this turn may use. A client
    #: that serves its own tools announces them instead (see
    #: `GatewayConnection.announce_tools`) and leaves this empty.
    allowed_actions: str = ""
    #: Applied last, so a user-supplied header wins over the defaults above.
    extra_headers: Sequence[tuple[str, str]] = ()
    traceparent: str | None = None
    timeout: timing.Duration = DEFAULT_TURN_TIMEOUT
    #: Called with each raw provider event. The port is drained regardless; this
    #: is only for a caller that wants to see them (``a11 chat -v``).
    on_event: Callable[[Any], None] | None = None


async def run_turn(
    connection: GatewayConnection,
    history: Sequence[Interaction],
    user_interaction: Interaction,
    tool_definitions: Sequence[dict],
    config: TurnConfig,
    reducer: PresentationReducer,
) -> list[Interaction]:
    """Run one turn on ``connection``, folding output into ``reducer``.

    Args:
        connection: A connected gateway.
        history: The conversation so far, replayed to the backend each turn.
        user_interaction: This turn's user message.
        tool_definitions: Tool definitions to offer the model.
        config: Backend selection and headers.
        reducer: Receives text, thoughts and interactions as they arrive.

    Returns:
        The interactions the turn produced, for the caller to append to its
        history. Only returned on success; a failure raises.

    Raises:
        StatusException: When the call fails, at any point in the turn.
    """
    call = (
        a11.Action(INTERACT_WITH_LLM_SCHEMA)
        .bind_node_map(connection.session.node_map)
        .bind_session(connection.session)
        .bind_stream(connection.stream)
        .set_header(LlmHeaders.PROVIDER.value, config.provider)
        .set_header(LlmHeaders.MODEL.value, config.model)
        .set_header(LlmHeaders.API_KEY.value, config.api_key)
        .set_header(LlmHeaders.BASE_URL.value, config.base_url)
        .set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, config.allowed_actions)
    )
    for key, value in config.extra_headers:
        call.set_header(key, value)
    observability.enable_tracing(
        call,
        traceparent=config.traceparent,
        baggage={"langfuse.trace.name": "a11_client_turn"},
    )
    await call.call()

    new_interactions: list[Interaction] = []

    async def read_thoughts() -> None:
        async for piece in call["thoughts"]:
            reducer.on_thought(piece)

    async def read_events() -> None:
        # Drained even when nobody is looking; see the module docstring.
        async for event in call["event_stream"]:
            if config.on_event is not None:
                config.on_event(event)

    async def read_interactions() -> None:
        async for interaction in call["new_interactions"]:
            new_interactions.append(interaction)
            reducer.on_interaction(interaction)

    # Started before the text loop; see the module docstring.
    readers = [
        asyncio.create_task(read_thoughts()),
        asyncio.create_task(read_interactions()),
        asyncio.create_task(read_events()),
    ]
    try:
        async with (
            call["interactions"] as interactions,
            call["config"],
            call["tools"] as tools,
        ):
            for interaction in history:
                await interactions.put(interaction)
            await interactions.put_final(user_interaction)
            # `config` is left empty and closed on block exit, so the backend
            # applies its own default request configuration.
            for definition in tool_definitions:
                await tools.put(definition)
            await tools.put_null_final()

        async for piece in call["text_output"]:
            reducer.on_text(piece)

        await asyncio.gather(*readers)
        await call.wait(config.timeout)
    finally:
        for reader in readers:
            if not reader.done():
                reader.cancel()
        await asyncio.gather(*readers, return_exceptions=True)
        reducer.end_turn()

    return new_interactions


__all__ = ["DEFAULT_TURN_TIMEOUT", "TurnConfig", "run_turn"]

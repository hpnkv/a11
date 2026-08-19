# Copyright 2026 The A11 Authors.

"""The two things the tool runner does besides running tools.

It keeps a tool's narration out of the result the model is shown and
hands it back as that call's log, and it offers the model the actions registered
on this side that the caller's allowed-tool patterns match.
"""

import pytest

import a11
from a11.actions import ActionPortSchema, ActionRegistry, ActionSchema
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import (
    Interaction,
    LlmHeaders,
    TOOL_LOGS_METADATA_KEY,
)
from a11.sdk.llm_tools import runner

_ECHO_SCHEMA = ActionSchema(
    name="echo_tool",
    description="Echo a word back.",
    inputs={
        "word": ActionPortSchema(
            "word", "text/plain", typeinfo=str, unary=True, required=True
        )
    },
    outputs={"echoed": ActionPortSchema("echoed", "text/plain", required=True)},
)


async def _echo(action: a11.Action) -> None:
    word = await action["word"].consume(str)
    await action["echoed"].put(word)
    await action["echoed"].drain_and_close()
    # Narration: no port declares it and nothing here closes it.
    await action.log(f"Echoed `{word}`.")
    # And one the runner must leave out, because it is A11's own bookkeeping
    # rather than something to show a person.
    await action.log("resolved the shell", internal=True)


def _registry() -> ActionRegistry:
    registry = ActionRegistry()
    registry.register(_ECHO_SCHEMA.name, _ECHO_SCHEMA, _echo)
    return registry


def _call(call_id: str, word: str) -> Interaction:
    """One interaction calling ``echo_tool``, as a backend would record it."""
    return Interaction(
        action_calls=[a11.ActionMessage(id=call_id, name=_ECHO_SCHEMA.name)],
        action_inputs={
            call_id: [
                a11.NodeFragment(
                    id="word", data=a11.to_chunk(word), continued=False
                )
            ]
        },
    )


@pytest.mark.asyncio
async def test_narration_is_taken_out_of_the_tool_result():
    registry = _registry()
    executed: list[runner.ExecutedActions] = []

    # `interact_with_llm`'s schema stands in for a backend's turn here; the
    # runner only needs a live action to nest the calls under.
    async def host_handler(action: a11.Action) -> None:
        executed.append(
            await runner.execute_actions_from_interaction(
                _call("call-1", "hello"), action, registry
            )
        )

    registry.register(
        INTERACT_WITH_LLM_SCHEMA.name, INTERACT_WITH_LLM_SCHEMA, host_handler
    )
    host = registry.make_action(INTERACT_WITH_LLM_SCHEMA.name)
    host.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"echo_tool")
    host.run()
    await host.wait()
    executed = executed[0]

    assert executed.errors == {}
    # The user-facing entry, and only it: "user facing" is the absence of the
    # internal flag, so the bookkeeping line the handler also wrote is left out.
    assert executed.logs == {"call-1": "Echoed `hello`."}
    # The result the model is handed carries the tool's own output and nothing
    # from the log port.
    result = {
        fragment.id: a11.from_chunk(fragment.get_chunk())
        for fragment in executed.outputs["call-1"]
    }
    assert result == {"echoed": "hello"}

    metadata = executed.log_metadata()
    assert TOOL_LOGS_METADATA_KEY in metadata
    assert b"Echoed" in metadata[TOOL_LOGS_METADATA_KEY]


@pytest.mark.asyncio
async def test_log_metadata_is_empty_when_no_tool_narrated_its_run():
    assert runner.ExecutedActions(outputs={}, errors={}).log_metadata() == {}


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "patterns,expected",
    [
        # The caller's own tool is offered; ours is not, nothing allows it.
        (b"caller_tool", ["caller_tool"]),
        # A pattern that matches ours adds it, without the caller having had to
        # describe a schema it does not own.
        (b"caller_tool,echo_.*", ["caller_tool", "echo_tool"]),
        # A tool the caller sent that no pattern matches is dropped.
        (b"echo_.*", ["echo_tool"]),
    ],
)
async def test_collect_tools_adds_the_registered_actions_the_caller_allows(
    patterns: bytes, expected: list[str]
):
    registry = _registry()
    collected: list[list[dict]] = []

    async def host_handler(action: a11.Action) -> None:
        collected.append(await runner.collect_tools(action))

    registry.register(
        INTERACT_WITH_LLM_SCHEMA.name, INTERACT_WITH_LLM_SCHEMA, host_handler
    )
    host = registry.make_action(INTERACT_WITH_LLM_SCHEMA.name)
    host.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, patterns)
    host.run()

    async with host["tools"] as tools:
        await tools.put_final(
            {
                "name": "caller_tool",
                "description": "A tool the caller serves itself.",
                "input_schema": {"type": "object", "properties": {}},
            }
        )
    async with host["interactions"] as interactions:
        await interactions.put_null_final()
    async with host["config"] as config:
        await config.put_null_final()
    await host.wait()

    assert [tool["name"] for tool in collected[0]] == expected

# Copyright 2026 The A11 Authors.

"""What the tool runner does besides running tools.

It keeps a tool's narration out of the result the model is shown and
hands it back as that call's log, it offers the model the actions registered
on this side that the caller's allowed-tool patterns match, and it honours a
schema that nominates one output as the whole result.
"""

import pytest

import a11
from a11.actions import ActionPortSchema, ActionRegistry, ActionSchema
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import (
    Interaction,
    LlmHeaders,
    TOOL_LOGS_METADATA_KEY,
    decode_action_output_fragments,
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
    await action["echoed"].finalize(word)
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
async def test_an_output_mapped_to_the_whole_result_is_not_wrapped():
    """`{"port": "$"}` exposes the port value without a one-key wrapper."""
    schema = ActionSchema(
        name="whole_tool",
        outputs={
            "payload": ActionPortSchema(
                "payload", "application/json", unary=True, required=True
            )
        },
        output_to_json_field={"payload": a11.ActionSchema.WHOLE_JSON},
    )

    async def handler(action: a11.Action) -> None:
        await action["payload"].finalize({"total": 7})

    registry = ActionRegistry()
    registry.register(schema.name, schema, handler)
    executed: list[runner.ExecutedActions] = []

    async def host_handler(action: a11.Action) -> None:
        interaction = Interaction(
            action_calls=[a11.ActionMessage(id="call-1", name=schema.name)]
        )
        executed.append(
            await runner.execute_actions_from_interaction(
                interaction, action, registry
            )
        )

    registry.register(
        INTERACT_WITH_LLM_SCHEMA.name, INTERACT_WITH_LLM_SCHEMA, host_handler
    )
    host = registry.make_action(INTERACT_WITH_LLM_SCHEMA.name)
    host.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"whole_tool")
    host.run()
    await host.wait()

    fragments = executed[0].outputs["call-1"]
    assert decode_action_output_fragments(fragments) == {"total": 7}


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

    await host["tools"].finalize({
        "name": "caller_tool",
        "description": "A tool the caller serves itself.",
        "input_schema": {"type": "object", "properties": {}},
    })
    await host["interactions"].finalize()
    await host["config"].finalize()
    await host.wait()

    assert [tool["name"] for tool in collected[0]] == expected


_QUIET_SCHEMA = ActionSchema(
    name="quiet_tool",
    description="Answers, narrates nothing, and leaves its log port open.",
    outputs={
        "listed": ActionPortSchema(
            "listed", "application/json", typeinfo=dict, required=True
        )
    },
)


@pytest.mark.asyncio
async def test_a_log_port_nobody_closes_is_drained_for_a_bounded_time():
    """The read that used to never return.

    `Action.log`'s contract is that nobody has to drain the log port and nobody
    has to close it. A handler in *this* process closes it by finishing, but a
    peer is under no such obligation -- the browser-tools guide's page left it
    open for every tool that answered without narrating -- and the runner read
    it against the turn's deadline alone. A turn carrying no `x-a11-deadline`
    header has `infinite_future()` for a deadline, so the read of a log that was
    never coming never returned: the tool ran, the page reported its result, and
    the model was never given it. From outside, a turn that stopped after its
    first tool call and never came back.

    Staged as the node itself, because that is the whole of the condition: a log
    port with no writer and no closer.
    """
    import asyncio
    import time

    node = a11.AsyncNode(a11.LocalChunkStore("a-log-nobody-closes"))
    started = time.monotonic()
    text = await asyncio.wait_for(
        runner.user_facing_log(
            node, runner._drain_timeout(a11.infinite_future())
        ),
        timeout=runner.DRAIN_AFTER_COMPLETION.float_seconds() + 20,
    )
    elapsed = time.monotonic() - started
    # No log, and no exception either: a port that did not end is not a failure
    # of the tool that owned it.
    assert text == ""
    # It waited, and then it stopped waiting. Both halves matter: a read that
    # returned at once would drop a log still in flight.
    assert elapsed >= runner.DRAIN_AFTER_COMPLETION.float_seconds() * 0.5
    assert elapsed < runner.DRAIN_AFTER_COMPLETION.float_seconds() + 10


@pytest.mark.asyncio
async def test_a_call_that_narrated_nothing_still_yields_a_result():
    """The ordinary shape of a read-only tool: it answers and says nothing."""
    import asyncio

    async def quiet(action: a11.Action) -> None:
        await action["listed"].put({"id": 0})
        await action["listed"].close()

    registry = ActionRegistry()
    registry.register(_QUIET_SCHEMA.name, _QUIET_SCHEMA, quiet)

    holder = registry.make_action(_QUIET_SCHEMA.name)
    holder.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS, _QUIET_SCHEMA.name)
    interaction = Interaction(
        action_calls=[a11.ActionMessage(id="c1", name=_QUIET_SCHEMA.name)]
    )

    # Bounded well above the drain window and well below "for ever": the point
    # is that the runner returns on its own.
    executed = await asyncio.wait_for(
        runner.execute_actions_from_interaction(interaction, holder, registry),
        timeout=runner.DRAIN_AFTER_COMPLETION.float_seconds() + 20,
    )

    assert executed.errors == {}
    assert "c1" in executed.outputs
    # Nothing narrated, so there is no log to show -- which is the case that
    # used to hang rather than a case that should fail.
    assert executed.logs == {}


@pytest.mark.asyncio
async def test_the_drain_window_is_bounded_whatever_the_turn_deadline_is():
    """State the bound, because the failure it prevents is a hang.

    A hang has no error message to assert on, so the regression test above can
    only prove that *this* handler finishes. This says why any handler does.
    """
    assert runner.DRAIN_AFTER_COMPLETION < a11.infinite_future() - a11.now()
    assert runner._drain_timeout(a11.infinite_future()) == (
        runner.DRAIN_AFTER_COMPLETION
    )
    # And the turn's deadline is still the ceiling when it is the tighter one.
    soon = a11.now() + a11.Duration.milliseconds(50)
    assert runner._drain_timeout(soon) < runner.DRAIN_AFTER_COMPLETION

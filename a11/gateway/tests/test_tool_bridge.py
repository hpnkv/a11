# Copyright 2026 The A11 Authors.

"""How a peer's tool descriptor becomes the two schemas the bridge uses.

The reverse-dispatch path itself needs a live peer on the other end of a stream;
what is checked here is the part that decides what the model may see: the local
schema the tool is registered under, and the wire schema it is called with.
"""

import a11
import pytest
from a11.gateway.tool_bridge import (
    REGISTER_TOOLS_SCHEMA,
    RemoteToolBridge,
    _BridgedTool,
    describe_tool,
)
from a11.sdk.llm import USER_FACING_LOG_PORT


def _descriptor(**overrides) -> dict:
    descriptor = {
        "name": "get_selection",
        "description": "The text selected in the active editor.",
        "inputs": [
            {
                "name": "request",
                "type": "application/json",
                "unary": True,
                "required": True,
            }
        ],
        "outputs": [
            {"name": "slice", "type": "application/json"},
            {
                "name": "user_log_for_run",
                "type": "text/plain",
                "user_facing": True,
            },
        ],
        "output_to_json_field": {"slice": "$"},
    }
    descriptor.update(overrides)
    return descriptor


def test_a_user_facing_output_is_renamed_locally_and_kept_on_the_wire():
    tool = _BridgedTool(_descriptor())

    # The peer's own name is what the peer writes, so the wire keeps it.
    assert set(tool.wire_schema.outputs) == {"slice", "user_log_for_run"}
    # Locally it lands on the canonical port, which is the one the tool runner
    # holds back from the model and files as the call's log.
    assert set(tool.schema.outputs) == {"slice", USER_FACING_LOG_PORT}
    assert tool.forwarded_outputs == {
        "slice": "slice",
        "user_log_for_run": USER_FACING_LOG_PORT,
    }
    assert tool.drained_ports == []
    # The peer's output-to-JSON mapping survives: which port is the whole result
    # is the tool's choice, not one assumed here.
    assert dict(tool.schema.output_to_json_field) == {"slice": "$"}


def test_extra_user_facing_outputs_are_dropped_but_still_drained():
    descriptor = _descriptor(
        outputs=[
            {"name": "slice", "type": "application/json"},
            {"name": "log_a", "type": "text/plain", "user_facing": True},
            {"name": "log_b", "type": "text/plain", "user_facing": True},
        ],
        output_to_json_field={},
    )
    tool = _BridgedTool(descriptor)

    assert set(tool.schema.outputs) == {"slice", USER_FACING_LOG_PORT}
    # Dropped from the model's view, but a port nobody reads stalls the peer
    # writing it, so it is still read on the wire.
    assert tool.drained_ports == ["log_b"]


def test_a_tool_without_a_log_is_left_alone():
    descriptor = _descriptor(
        outputs=[{"name": "slice", "type": "application/json"}],
        output_to_json_field={},
    )
    tool = _BridgedTool(descriptor)

    assert tool.log_port is None
    assert set(tool.schema.outputs) == {"slice"}
    assert isinstance(tool.schema, a11.ActionSchema)


async def _announce(registry: a11.ActionRegistry, *descriptors: dict) -> dict:
    """Run the bridge's announce handler against ``registry``, return its ok."""
    bridge = RemoteToolBridge()
    bridge.install(registry)
    action = a11.Action(REGISTER_TOOLS_SCHEMA).bind_handler(
        bridge._register_tools_handler
    )
    action.run()
    async with action["tools"] as tools:
        for descriptor in descriptors:
            await tools.put(descriptor)
        await tools.put_null_final()
    return await action["ok"].consume(dict)


@pytest.mark.asyncio
async def test_a_peer_tool_shadows_a_local_one_of_the_same_name():
    """A peer may announce a name this side already serves, and win.

    This is what lets `a11 chat` announce its own `shell_execute` to a gateway
    that serves one too: the model's calls must reach the *client's* shell, in
    the user's cwd. Registration replaces, and the registry is a per-connection
    copy, so the substitution is scoped to this peer.
    """
    registry = a11.ActionRegistry()
    local = a11.ActionSchema(
        name="get_selection",
        description="This side's own version.",
        outputs={
            "local_only": a11.ActionPortSchema(
                name="local_only", type="text/plain"
            )
        },
    )

    async def local_handler(action: a11.Action) -> None:  # pragma: no cover
        raise AssertionError("the peer's tool should have replaced this one")

    registry.register(local.name, local, local_handler)

    ok = await _announce(registry, _descriptor())

    assert ok["registered"] == ["get_selection"]
    # The peer's schema is the one registered now, not this side's.
    schema = registry.get_schema("get_selection")
    assert set(schema.outputs) == {"slice", USER_FACING_LOG_PORT}


@pytest.mark.asyncio
async def test_announcing_a_fresh_name_leaves_local_tools_registered():
    registry = a11.ActionRegistry()
    keep = a11.ActionSchema(name="shell_execute", description="Kept.")
    registry.register(keep.name, keep)

    ok = await _announce(registry, _descriptor())

    assert ok["registered"] == ["get_selection"]
    assert registry.is_registered("shell_execute")
    assert registry.is_registered("get_selection")


def test_describe_tool_round_trips_a_schema_into_a_callable_proxy():
    """The descriptor has to carry *ports*, not a JSON-Schema tool definition.

    Announcing the latter -- which is a different document, for a different port,
    aimed at the model rather than at the bridge -- yields a proxy with no inputs
    at all, and every tool call then fails with "unexpected input". This pins the
    shape so that cannot pass silently.
    """
    original = a11.ActionSchema(
        name="shell_execute",
        description="Run a command.",
        inputs={
            "command": a11.ActionPortSchema(
                name="command", type="text/plain", unary=True
            ),
            "parameters": a11.ActionPortSchema(
                name="parameters", type="application/json", unary=True
            ),
        },
        outputs={
            "output_lines": a11.ActionPortSchema(
                name="output_lines", type="text/plain"
            ),
            USER_FACING_LOG_PORT: a11.ActionPortSchema(
                name=USER_FACING_LOG_PORT, type="text/plain"
            ),
        },
    )

    descriptor = describe_tool(original)
    # A set: ActionSchema's ports are backed by a hash map, so descriptor order
    # is not stable between runs and nothing should depend on it.
    described = {entry["name"]: entry for entry in descriptor["inputs"]}
    assert set(described) == {"command", "parameters"}
    assert described["command"]["unary"] is True
    assert described["command"]["type"] == "text/plain"
    # The log port must be flagged, or the model would be shown a tool's
    # narration of itself as though it were the result.
    log = next(
        entry
        for entry in descriptor["outputs"]
        if entry["name"] == USER_FACING_LOG_PORT
    )
    assert log["user_facing"] is True

    # And the bridge must rebuild a schema the model's arguments can land on.
    rebuilt = _BridgedTool(descriptor)
    assert set(rebuilt.schema.inputs) == {"command", "parameters"}
    assert rebuilt.schema.inputs["command"].unary is True
    assert USER_FACING_LOG_PORT in rebuilt.schema.outputs


def test_a_tool_definition_is_not_a_descriptor():
    """The mistake this guards against, stated as a test.

    `get_tool_definitions` produces `{name, description, input_schema}` for the
    model; the bridge needs `{name, inputs, outputs}`. Handing it the former is
    accepted -- and produces a tool that cannot be called.
    """
    definition = {
        "name": "shell_execute",
        "description": "Run a command.",
        "input_schema": {"type": "object", "properties": {"command": {}}},
    }
    useless = _BridgedTool(definition)
    assert useless.schema.inputs == {}, (
        "a definition has no ports, which is exactly why it must not be"
        " announced in place of a descriptor"
    )

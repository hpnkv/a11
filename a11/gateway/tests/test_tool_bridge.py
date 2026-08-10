# Copyright 2026 The A11 Authors.

"""How a peer's tool descriptor becomes the two schemas the bridge uses.

The reverse-dispatch path itself needs a live peer on the other end of a stream;
what is checked here is the part that decides what the model may see: the local
schema the tool is registered under, and the wire schema it is called with.
"""

import a11
from a11.gateway.tool_bridge import _BridgedTool
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

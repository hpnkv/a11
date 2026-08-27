# Copyright 2026 The A11 Authors.

"""How a peer's actions become proxies the model can call.

The bridge calls ``__list_actions__`` on the peer and registers one
reverse-dispatch proxy for each returned schema.
"""

import asyncio

import a11
import pytest
from a11 import net
from a11.gateway.tool_bridge import RemoteToolBridge, _BridgedTool
from a11.service.session import Session


def _described(**overrides) -> dict:
    """One entry as `__list_actions__` produces it."""
    entry = {
        "name": "get_selection",
        "description": "The text selected in the active editor.",
        "runnable": True,
        "inputs": [
            {
                "name": "request",
                "type": "application/json",
                "unary": True,
                "required": True,
                "json_schema": {"type": "object"},
            }
        ],
        "outputs": [
            {"name": "slice", "type": "application/json", "unary": False}
        ],
        "output_to_json_field": {"slice": "$"},
    }
    entry.update(overrides)
    return entry


def test_a_descriptor_becomes_one_callable_schema():
    """Use one schema for local lookup and wire dispatch.

    A Flow ``call`` dispatches the registry schema's ports directly to the
    peer, so the local schema must preserve every remote port name. Narration
    uses the reserved log port and is not part of either schema.
    """
    tool = _BridgedTool(_described())

    assert set(tool.schema.inputs) == {"request"}
    assert set(tool.schema.outputs) == {"slice"}
    assert tool.schema.inputs["request"].unary is True
    assert tool.schema.inputs["request"].required is True
    # Not unary: a document that omitted `unary` would mean the opposite of this
    # to the flow catalogue's reader, which is why it is always written.
    assert tool.schema.outputs["slice"].unary is False
    # The peer's output-to-JSON mapping survives: which port is the whole result
    # is the tool's choice, not one assumed here.
    assert dict(tool.schema.output_to_json_field) == {"slice": "$"}
    # The port's value schema travels, which is what lets the model be shown a
    # remote tool's real argument types -- there is no Python type here to
    # derive one from.
    assert tool.schema.inputs["request"].json_schema


def test_an_older_clients_user_facing_flag_is_read_and_dropped():
    """Accept the legacy ``user_facing`` flag as an ignored field."""
    tool = _BridgedTool(
        _described(
            outputs=[
                {"name": "slice", "type": "application/json", "unary": False},
                {
                    "name": "user_log_for_run",
                    "type": "text/plain",
                    "unary": False,
                    "user_facing": True,
                },
            ],
            output_to_json_field={},
        )
    )

    assert set(tool.schema.outputs) == {"slice", "user_log_for_run"}


def test_a_nameless_descriptor_is_refused():
    with pytest.raises(Exception):
        _BridgedTool({"description": "no name"})


def test_a_tool_definition_is_not_a_descriptor():
    """Reject treating a model tool definition as an action descriptor.

    ``get_tool_definitions`` produces ``{name, description, input_schema}``
    for the model; an action descriptor carries ports. Passing the former here
    produces a tool with no ports and must be detectable before dispatch.
    """
    useless = _BridgedTool({
        "name": "shell_execute",
        "description": "Run a command.",
        "input_schema": {"type": "object", "properties": {"command": {}}},
    })
    assert useless.schema.inputs == {}


# --- registering what came back --------------------------------------------


def _register(registry: a11.ActionRegistry, *entries: dict) -> list[str]:
    """Feed the bridge some schemas the way `discover` would."""
    bridge = RemoteToolBridge()
    bridge.install(registry)
    return bridge.register_peer_schemas(list(entries))


def test_a_peer_tool_shadows_a_local_one_of_the_same_name():
    """A peer may serve a name this side also serves, and win.

    This is what lets `a11 chat` serve its own `shell_execute` to a gateway that
    serves one too: the model's calls must reach the *client's* shell, in the
    user's cwd. Registration replaces, and the registry is a per-connection
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

    assert _register(registry, _described()) == ["get_selection"]
    # The peer's schema is the one registered now, not this side's.
    assert set(registry.get_schema("get_selection").outputs) == {"slice"}


def test_a_fresh_name_leaves_local_tools_registered():
    registry = a11.ActionRegistry()
    keep = a11.ActionSchema(name="shell_execute", description="Kept.")
    registry.register(keep.name, keep)

    assert _register(registry, _described()) == ["get_selection"]
    assert registry.is_registered("shell_execute")
    assert registry.is_registered("get_selection")


def test_a_reserved_name_is_never_proxied():
    """A11's own actions are answered here already.

    `Register` refuses their names in any case, so proxying one would fail the
    whole batch over an entry that could not have been meant.
    """
    registry = a11.ActionRegistry()
    registered = _register(
        registry, _described(name="__ping"), _described(name="editor_open")
    )
    assert registered == ["editor_open"]


# --- the pull path -----------------------------------------------------------


@pytest.mark.asyncio
async def test_the_bridge_asks_the_peer_what_it_serves():
    """The migrated path: nothing announced, everything discovered."""
    peer_registry = a11.ActionRegistry()

    @peer_registry.action
    async def editor_open(path: str) -> str:
        """Open a file in the editor."""
        return f"opened {path}"

    # Something the peer only holds a schema for. It lives on some further peer,
    # so proxying it here would build a chain nobody asked for.
    peer_registry.register(
        "further_away",
        a11.ActionSchema(
            name="further_away",
            description="Not here either.",
            inputs={
                "q": a11.ActionPortSchema(
                    name="q", type="text/plain", typeinfo=str
                )
            },
        ),
    )

    gateway_side, client_side = net.create_in_process_wire_stream_pair()
    peer = Session(action_registry=peer_registry)
    await peer.add_stream(client_side, mode="accept")

    gateway_registry = a11.ActionRegistry()
    gateway_session = Session(action_registry=gateway_registry)
    await gateway_session.add_stream(gateway_side, mode="start")

    bridge = RemoteToolBridge()
    bridge.install(gateway_registry)
    bridge.bind_session(gateway_session, gateway_side)

    discovered = await asyncio.wait_for(bridge.discover(), timeout=10)

    assert discovered == ["editor_open"]
    assert gateway_registry.is_registered("editor_open")
    # Registered *with* a handler here -- the proxy -- so the tool runner can
    # call it even though the work happens on the peer.
    assert gateway_registry.get_handler("editor_open") is not None

    # Asked once. A second ask would build a second proxy for every tool.
    assert await asyncio.wait_for(bridge.discover(), timeout=10) == []


@pytest.mark.asyncio
async def test_the_bridge_finds_itself_on_its_session():
    """How `collect_tools` reaches the bridge for *this* connection."""
    registry = a11.ActionRegistry()
    session = Session(action_registry=registry)
    left, _right = net.create_in_process_wire_stream_pair()

    assert RemoteToolBridge.of(session) is None
    bridge = RemoteToolBridge()
    bridge.install(registry)
    bridge.bind_session(session, left)
    assert RemoteToolBridge.of(session) is bridge
    assert RemoteToolBridge.of(None) is None


@pytest.mark.asyncio
async def test_a_peer_that_will_not_answer_costs_the_turn_nothing():
    """Plenty of clients serve no tools; that is not a failure.

    The turn proceeds with the gateway's own actions rather than failing over a
    peer that could not be asked.
    """
    registry = a11.ActionRegistry()
    bridge = RemoteToolBridge()
    bridge.install(registry)
    # Never bound to a session, so there is nothing to ask.
    assert await asyncio.wait_for(bridge.discover(), timeout=10) == []

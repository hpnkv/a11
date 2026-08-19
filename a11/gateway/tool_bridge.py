# Copyright 2026 The A11 Authors.

"""Distributed tool bridge: run the model's tool calls back on the caller.

The gateway serves ``interact_with_llm``, but a client often owns tools the
gateway cannot: an IDE plugin's editor, index and refactorings live in the IDE.
When the model calls one of those, the tool runner resolves it against this
connection's registry, where the bridge has registered a *proxy* whose handler
reverse-dispatches the call over the same stream to the client, forwards the
tool's inputs and streams its outputs back — so the real handler runs where the
tool actually is, while the model sees one ordinary A11 action.

Handshake: the client calls the reserved ``__register_tools__`` action once per
connection, streaming a JSON descriptor of each tool's schema on the ``tools``
input. The bridge builds an :class:`a11.ActionSchema` per descriptor and
registers the proxy before the first chat turn. Registration is therefore
per-connection, which is why the gateway hands each stream its own registry.

Narration -- what a tool did, for the person watching, never part of the model's
tool result -- rides the reserved log port. A client that logs through its own
A11 runtime needs nothing from this module: the log port of the action the bridge
dispatched mirrors back like any other output, and the proxy re-emits it through
:meth:`a11.actions.action.Action.log` on the local action, which is where the
tool runner reads it.

A descriptor may also flag an output ``user_facing``, which is how a client that
declares its own narration port says so. Such a port is not part of the local
schema -- there is nothing to declare, the log port is not a schema port -- and
its values are re-emitted through ``log()`` as well, so both kinds of client end
up in one place. Retained for clients that have not moved to ``log()``.
"""

from __future__ import annotations

import asyncio
from typing import Any

from absl import logging

import a11
from a11 import _native
from a11.net.wire_stream import WireStream
from a11.service.session import Session
from a11.status import Status, StatusCode

#: Reserved action a client announces its own tools with, once per connection.
REGISTER_TOOLS_ACTION = "__register_tools__"

REGISTER_TOOLS_SCHEMA = a11.ActionSchema(
    name=REGISTER_TOOLS_ACTION,
    description="Announce the caller's tool schemas for reverse dispatch.",
    inputs={
        "tools": a11.ActionPortSchema(
            name="tools", type="application/json", typeinfo=list, required=True
        )
    },
    outputs={
        "ok": a11.ActionPortSchema(
            name="ok", type="application/json", typeinfo=dict, required=True
        )
    },
)


def describe_port(port: a11.ActionPortSchema, *, user_facing: bool) -> dict:
    """One port, as `_ports` reads it back."""
    described: dict[str, Any] = {
        "name": port.name,
        "type": port.type,
        "description": port.description,
        "required": bool(port.required),
        "unary": bool(port.unary),
    }
    if user_facing:
        described["user_facing"] = True
    return described


def describe_tool(schema: a11.ActionSchema) -> dict:
    """An ActionSchema as the descriptor `__register_tools__` expects.

    This is the *port* description the bridge rebuilds a callable schema from --
    not the JSON-Schema tool definition a model is shown, which is a different
    document produced by
    [get_tool_definitions][a11.sdk.llm_tools.runner.get_tool_definitions]. A
    client needs both, for different ports, and announcing the latter here
    silently yields a proxy with no inputs at all: the model's arguments then
    have nowhere to land and the call fails with "unexpected input".

    Mirrors what the IntelliJ plugin's Kotlin side sends, so both clients get
    proxies built the same way.
    """
    return {
        "name": schema.name,
        "description": schema.description,
        "inputs": [
            describe_port(port, user_facing=False)
            for port in schema.inputs.values()
        ],
        "outputs": [
            # Nothing is flagged: an A11 schema has no narration port to flag.
            # What a tool logs travels on the reserved log port, which is not in
            # the schema and so is not describable -- and does not need to be,
            # because the far side finds it in the same place on every action.
            describe_port(port, user_facing=False)
            for port in schema.outputs.values()
        ],
        "output_to_json_field": dict(schema.output_to_json_field),
    }


def _ports(
    entries: list[dict[str, Any]],
) -> dict[str, a11.ActionPortSchema]:
    result: dict[str, a11.ActionPortSchema] = {}
    for entry in entries or []:
        name = entry["name"]
        result[name] = a11.ActionPortSchema(
            name=name,
            type=entry.get("type", "application/json"),
            description=entry.get("description", ""),
            required=bool(entry.get("required", False)),
            unary=bool(entry.get("unary", False)),
        )
    return result


class _BridgedTool:
    """One remote tool: the schema it is called with here, and on the wire.

    The two are the same but for narration. A port the descriptor flagged
    ``user_facing`` exists on the wire, because the client writes it, and not
    locally, because narration is not a schema port here -- its values are
    re-emitted through :meth:`a11.actions.action.Action.log` instead. A client
    that has moved to ``log()`` flags nothing and the two schemas are identical.

    That identity is worth more than it looks. A **flow** does not go through the
    proxy's forwarding: a ``call`` step dispatches the ports of the schema in the
    registry -- the local one -- straight to the peer. When the local schema was
    the wire schema with a port renamed, a flow calling a bridged tool named a
    port the client did not have and the composition failed there. With narration
    off the schema entirely, there is nothing left to diverge.
    """

    def __init__(self, descriptor: dict[str, Any]) -> None:
        self.name: str = descriptor["name"]
        wire_outputs = _ports(descriptor.get("outputs", []))
        #: Wire ports the client narrates on. Logged rather than forwarded.
        self.log_ports: list[str] = [
            entry["name"]
            for entry in descriptor.get("outputs", []) or []
            if entry.get("user_facing")
        ]

        local_outputs = {
            name: port
            for name, port in wire_outputs.items()
            if name not in self.log_ports
        }

        inputs = _ports(descriptor.get("inputs", []))
        self.schema = a11.ActionSchema(
            name=self.name,
            description=descriptor.get("description", ""),
            inputs=inputs,
            outputs=local_outputs,
        )
        self.wire_schema = a11.ActionSchema(
            name=self.name,
            description=descriptor.get("description", ""),
            inputs=inputs,
            outputs=wire_outputs,
        )
        # The client's own output-to-JSON mapping: which port (if any) is the
        # whole result is the tool's choice, not a name assumed here.
        for output, field in (
            descriptor.get("output_to_json_field") or {}
        ).items():
            if output in self.schema.outputs:
                self.schema.map_output_to_json(output, field)
            if output in self.wire_schema.outputs:
                self.wire_schema.map_output_to_json(output, field)

    @property
    def forwarded_outputs(self) -> dict[str, str]:
        """Wire output port -> the local port it lands on.

        The identity on everything the local schema declares. Narration is not in
        it and is not forwarded: see :meth:`log_ports`.
        """
        return {
            name: name
            for name in self.wire_schema.outputs
            if name in self.schema.outputs
        }


class RemoteToolBridge:
    """Registers reverse-dispatch proxies for one connection's remote tools.

    One instance per stream: the tools belong to the peer that announced them,
    and so does the registry they are registered on.
    """

    def __init__(self) -> None:
        self._registry: a11.ActionRegistry | None = None
        self._session: Session | None = None
        self._stream: WireStream | None = None
        self._counter = 0

    def install(self, registry: a11.ActionRegistry) -> None:
        self._registry = registry
        registry.register(
            REGISTER_TOOLS_ACTION,
            REGISTER_TOOLS_SCHEMA,
            self._register_tools_handler,
        )

    def bind_session(self, session: Session, stream: WireStream) -> None:
        self._session = session
        self._stream = stream

    async def _register_tools_handler(self, action: a11.Action) -> None:
        if self._registry is None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="Tool bridge is not installed on a registry.",
            ).to_exception()

        registered: list[str] = []
        shadowed: list[str] = []
        async for descriptor in action["tools"]:
            tool = _BridgedTool(descriptor)
            # A peer announcing a name this side also serves shadows it:
            # `register` replaces, and the registry is a per-connection copy, so
            # no other session sees the substitution. That is the behaviour we
            # want — the peer asked for *its* tool to run the model's calls, and
            # a client whose whole point is its own shell (`a11 chat`) must be
            # able to announce `shell_execute` to a gateway that serves one too.
            # It is worth a log line because it is otherwise invisible.
            if self._registry.is_registered(tool.name):
                shadowed.append(tool.name)
            self._registry.register(
                tool.name, tool.schema, self._make_proxy(tool)
            )
            registered.append(tool.name)

        logging.info("registered %d remote tool(s)", len(registered))
        if shadowed:
            logging.info(
                "peer tools shadow local ones on this connection: %s",
                ", ".join(shadowed),
            )
        await action["ok"].put({"registered": registered}, final=True)
        await action["ok"].drain_and_close()

    def _make_proxy(self, tool: _BridgedTool):
        async def proxy(nested: a11.Action) -> None:
            # A11 handlers must return None; signal failure by raising a status
            # exception (returning a Status trips "async callback must return
            # None" in the native runtime).
            if self._session is None or self._stream is None:
                raise Status(
                    code=StatusCode.FAILED_PRECONDITION,
                    message="Tool bridge is not bound to a session.",
                ).to_exception()
            self._counter += 1
            # Register the outbound action's nodes in the session's node map so
            # the peer's streamed tool outputs route back to `remote`'s output
            # ports. Without the node map, response fragments have nowhere to go
            # and the forwarding below blocks forever. Mirrors the outbound-call
            # binding in a11/actions/tests/test_action.py.
            remote = (
                a11.Action(tool.wire_schema)
                .bind_node_map(self._session.node_map)
                .bind_session(self._session)
                .bind_stream(self._stream)
            )
            remote.set_id(f"{nested.get_id()}-remote-{self._counter}")
            await remote.call()

            # Forward the tool inputs the runner fed us to the peer, then stream
            # the peer's outputs back onto the nested action's outputs.
            #
            # Every port is pumped concurrently, and that is load-bearing rather
            # than an optimisation. A peer writes its ports in whatever order it
            # likes, `ActionSchema.inputs`/`outputs` do not preserve declaration
            # order, and the transport applies backpressure: draining one
            # port to completion before starting the next deadlocks the moment
            # the peer is busy filling a port this side has not begun reading. A
            # big result (a whole file, say) is exactly when that happens.
            await asyncio.gather(
                *[
                    _pump(nested.get_input(name), remote.get_input(name))
                    for name in tool.schema.inputs
                ]
            )
            await asyncio.gather(
                *[
                    _pump(
                        remote.get_output(wire),
                        nested.get_output(local),
                        rename=local if local != wire else None,
                    )
                    for wire, local in tool.forwarded_outputs.items()
                ],
                # Narration, from either kind of client, onto the local action's
                # log -- which is where the tool runner reads it, and the only
                # place it can be without becoming visible to the model. Read for
                # the same backpressure reason as everything else: a port nobody
                # reads stalls the peer writing it.
                _relog(remote.get_log_node(), nested, from_log_port=True),
                *[
                    _relog(remote.get_output(name), nested, from_log_port=False)
                    for name in tool.log_ports
                ],
            )

            await remote.wait()

        return proxy


async def _relog(
    src: a11.AsyncNode, action: a11.Action, *, from_log_port: bool
) -> None:
    """Re-emit what a peer narrated as this action's own log.

    ``from_log_port`` says the chunks are already log chunks, in which case the
    level, channel, location and internal flag they came with are passed back
    through -- a client's ``debug`` line must not arrive here as ``info``. A
    client's own declared narration port carries plain values instead, and those
    become ordinary user-facing entries.

    Best effort: a peer's narration is worth nothing next to its result, so a log
    that will not re-emit does not fail the call.
    """
    while True:
        chunk = await src.next_chunk()
        if chunk is None:
            break
        if chunk.is_null() or _native.is_status_chunk(chunk):
            continue
        try:
            if from_log_port:
                record = _native.log_record_from_chunk(chunk)
                await action.log(
                    chunk,
                    level=record["level"],
                    channel=record["channel"] or None,
                    internal=record["internal"],
                    # The peer's location, or none at all -- an empty file rather
                    # than None, so `log` does not helpfully stamp this module's
                    # own line onto somebody else's log.
                    file=record["file"],
                    lineno=record["lineno"],
                )
            else:
                await action.log(chunk, file="")
        except Exception:
            logging.warning("failed to re-emit a peer's log", exc_info=True)


async def _pump(
    src: Any, dst: a11.AsyncNode, rename: str | None = None
) -> None:
    """Copy every fragment from one node to another, then close the target.

    ``rename`` re-tags each fragment with the destination port's name, which the
    two differ in when a remote user-facing log lands on the canonical one.
    """
    while True:
        fragment = await src.next_fragment()
        if fragment is None:
            break
        if rename is not None:
            fragment.id = rename
        await dst.put_fragment(fragment)

    await dst.drain_and_close()


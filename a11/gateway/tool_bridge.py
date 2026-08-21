# Copyright 2026 The A11 Authors.

"""Distributed tool bridge: run the model's tool calls back on the caller.

The gateway serves ``interact_with_llm``, but a client often owns tools the
gateway cannot: an IDE plugin's editor, index and refactorings live in the IDE.
When the model calls one of those, the tool runner resolves it against this
connection's registry, where the bridge has registered a *proxy* whose handler
reverse-dispatches the call over the same stream to the client, forwards the
tool's inputs and streams its outputs back -- so the real handler runs where the
tool actually is, while the model sees one ordinary A11 action.

**How the bridge learns what the client has.** It asks. Every A11 peer answers
`__list_actions__`, so the bridge calls it on the client and builds a proxy per
answer -- no handshake to implement, no schema to hand-copy, and nothing for the
client to remember to do. The ask is **lazy**: it happens the first time a turn
needs tools, not when the connection opens. Two reasons, and both are hard
requirements rather than tuning. A connection's `on_connection` hook runs before
its session pumps messages, so an awaited round trip there deadlocks; and a
connection that never chats should not pay for a round trip it will not use.

There used to be a ``__register_tools__`` handshake for the other direction --
a client announcing its tools -- with its schema hand-copied into four
languages and its own port-descriptor vocabulary. It is gone: every A11
runtime now answers `__list_actions__` itself, so there is nothing to announce
and, more to the point, nothing to announce wrongly.

Narration -- what a tool did, for the person watching, never part of the model's
tool result -- rides the reserved log port. A client that logs through its own
A11 runtime needs nothing from this module: the log port of the action the bridge
dispatched mirrors back like any other output, and the proxy re-emits it through
:meth:`a11.actions.action.Action.log` on the local action, which is where the
tool runner reads it. There is no longer any way for a client to nominate one of
its *declared* outputs as narration, and nothing needs one: every action has a
log port, and no schema has to declare it.
"""

from __future__ import annotations

import asyncio
from typing import Any

from absl import logging

import a11
from a11 import _native, timing
from a11.actions import describe
from a11.net.wire_stream import WireStream
from a11.service.session import Session
from a11.status import Status, StatusCode

#: How long the bridge waits for a client to say what it serves.
DISCOVERY_TIMEOUT = timing.Duration.seconds(15)


class _BridgedTool:
    """One remote tool: the schema it is called with, here and on the wire.

    They are the same schema. They were once two, differing only in a port the
    client had flagged as narration, which existed here and not there; narration
    moved to the reserved log port, which no schema declares, and the two
    collapsed into one.

    That identity is worth more than it looks. A **flow** does not go through the
    proxy's forwarding: a ``call`` step dispatches the ports of the schema in the
    registry straight to the peer. When the local schema was the wire schema with
    a port renamed, a flow calling a bridged tool named a port the client did not
    have and the composition failed there.
    """

    def __init__(self, described: dict[str, Any]) -> None:
        self.name: str = described.get("name", "")
        if not self.name:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="A described tool must have a name.",
            ).to_exception()
        self.schema = describe.schema_from_json(described)


class RemoteToolBridge:
    """Registers reverse-dispatch proxies for one connection's remote tools.

    One instance per stream: the tools belong to the peer that announced them,
    and so does the registry they are registered on.
    """

    #: Attribute the bridge parks itself under on its session, so the tool
    #: runner can find it without a global.
    SESSION_ATTRIBUTE = "a11_tool_bridge"

    def __init__(self) -> None:
        self._registry: a11.ActionRegistry | None = None
        self._session: Session | None = None
        self._stream: WireStream | None = None
        self._counter = 0
        self._asked = False
        self._lock = asyncio.Lock()

    def install(self, registry: a11.ActionRegistry) -> None:
        """Bind this bridge to the registry its proxies go on.

        Nothing is registered here. The bridge's own entry point is
        `__list_actions__` on the *peer*, not an action it serves itself.
        """
        self._registry = registry

    def bind_session(self, session: Session, stream: WireStream) -> None:
        self._session = session
        self._stream = stream
        # Parked on the session so `collect_tools` can reach the bridge for
        # *this* connection. A module-level map keyed by session would be the
        # same thing with a lifetime bug attached.
        setattr(session, self.SESSION_ATTRIBUTE, self)

    @classmethod
    def of(cls, session: Session | None) -> RemoteToolBridge | None:
        """The bridge bound to ``session``, if it has one."""
        if session is None:
            return None
        found = getattr(session, cls.SESSION_ATTRIBUTE, None)
        return found if isinstance(found, cls) else None

    # --- learning what the peer has ----------------------------------------

    async def discover(
        self, *, timeout: timing.Duration | None = None
    ) -> list[str]:
        """Ask the peer what it serves, once, and proxy each answer.

        Idempotent and cheap after the first call: a connection asks once. A peer
        that cannot answer is not an error -- plenty of clients serve no tools at
        all, and the turn should proceed with the gateway's own.
        """
        async with self._lock:
            if self._asked:
                return []
            self._asked = True
            if self._registry is None or self._session is None:
                return []
            try:
                described = await self._ask(timeout or DISCOVERY_TIMEOUT)
            except Exception:
                logging.info(
                    "the peer did not say what it serves; using local tools"
                    " only",
                    exc_info=True,
                )
                return []
            return self.register_peer_schemas(described)

    async def _ask(self, timeout: timing.Duration) -> list[dict[str, Any]]:
        call = (
            a11.Action(describe.LIST_ACTIONS_SCHEMA)
            .bind_node_map(self._session.node_map)
            .bind_session(self._session)
            .bind_stream(self._stream)
        )
        await call.call()
        # Only what the peer can actually run: an action it holds a schema for
        # and no handler lives on some further peer, and proxying it here would
        # build a chain nobody asked for.
        await call["request"].finalize({"runnable_only": True})
        try:
            document = await call["actions"].consume(dict)
        except Exception:
            # The terminal status is where the real reason is; the empty read is
            # only its symptom.
            await call.wait(timeout)
            raise
        await call.wait(timeout)
        return describe.schemas_in_document(document)

    def register_peer_schemas(
        self, described: list[dict[str, Any]]
    ) -> list[str]:
        """Register a reverse-dispatch proxy per schema the peer sent.

        The one place a written schema becomes a callable action.
        """
        if self._registry is None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="Tool bridge is not installed on a registry.",
            ).to_exception()

        registered: list[str] = []
        shadowed: list[str] = []
        for entry in described:
            name = entry.get("name", "")
            if not name or describe.is_reserved_action(name):
                # A11's own actions are answered here already, and `Register`
                # refuses their names in any case.
                continue
            try:
                tool = _BridgedTool(entry)
            except Exception:
                logging.warning(
                    "could not read the peer's description of %r", name,
                    exc_info=True,
                )
                continue
            # A peer announcing a name this side also serves shadows it:
            # `register` replaces, and the registry is a per-connection copy, so
            # no other session sees the substitution. That is the behaviour we
            # want -- the peer asked for *its* tool to run the model's calls, and
            # a client whose whole point is its own shell (`a11 chat`) must be
            # able to serve `shell_execute` to a gateway that serves one too.
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
        return registered

    # --- running one of them ------------------------------------------------

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
                a11.Action(tool.schema)
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
                    _pump(remote.get_output(name), nested.get_output(name))
                    for name in tool.schema.outputs
                ],
                # Narration onto the local action's log -- which is where the
                # tool runner reads it, and the only place it can be without
                # becoming visible to the model. Read for the same backpressure
                # reason as everything else: a port nobody reads stalls the peer
                # writing it.
                _relog(remote.get_log_node(), nested),
            )

            await remote.wait()

        return proxy


async def _relog(src: a11.AsyncNode, action: a11.Action) -> None:
    """Re-emit what a peer narrated as this action's own log.

    The level, channel, location and internal flag come back through: a client's
    ``debug`` line must not arrive here as ``info``.

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
        except Exception:
            logging.warning("failed to re-emit a peer's log", exc_info=True)


async def _pump(src: Any, dst: a11.AsyncNode) -> None:
    """Copy every fragment from one node to another, then close the target.

    The copy carries the source's own finality, so the target is closed rather
    than finalized: a second final sequence would be invalid at the store.
    """
    while True:
        fragment = await src.next_fragment()
        if fragment is None:
            break
        await dst.put_fragment(fragment)

    await dst.close()

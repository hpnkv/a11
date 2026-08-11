"""Running a compiled Flow program on A11.

The runtime turns a [FlowPlan][a11.flow.plan.FlowPlan] into an action handler:
the flow becomes one action,
each `call` in it becomes a nested action on the same session, and each pipe
becomes a task copying one node into another as values arrive. Nothing is
buffered that A11 would not buffer anyway -- a pipe forwards a producer's own
chunks, with their mimetypes intact, and only decodes a value when a stage
actually looks at one.

Three behaviours are worth knowing about, because they are what a composition
needs and what hand-written glue usually gets wrong:

* **Every output is drained.** An output port of a called action that the flow
  does not read is read and discarded anyway, because an unread output stalls
  the action producing it. `skip` is the explicit spelling of the same thing.
* **A `run` step keeps its nodes off the wire.** A step that runs in this
  process is bound to no stream unless it asks for `tee`, so the intermediate
  streams between two steps of a composition are never replicated to the peer
  that dispatched it. A `nodes` block goes further and keeps them out of the
  session's node map entirely. A `call` step, by contrast, is exactly a step
  put on the stream this flow is attached to.
* **Inputs are closed.** A port the flow feeds is closed when its last writer
  finishes, and one it never feeds is closed immediately, so a callee waiting
  for end-of-input is never left waiting on a port the composition was never
  going to write.
"""

from __future__ import annotations

import asyncio
import contextlib
import fnmatch
import itertools
from collections.abc import AsyncIterator, Callable, Iterable, Mapping, Sequence
from typing import Any

from a11 import timing
from a11.actions.action import Action, ActionSchema
from a11.data import types
from a11.data.serialization import (
    MSGPACK_MIMETYPE,
    get_global_serialization_registry,
)
from a11.flow import plan as _plan
from a11.flow import values
from a11.flow.plan import (
    Body,
    CallPortRef,
    CallStep,
    CancelStep,
    CaptureStep,
    DerivedRef,
    Expr,
    ExprRef,
    FailStep,
    FlowPlan,
    FlowPortRef,
    ForEachStep,
    HeaderRef,
    IfStep,
    LocalNodeRef,
    NodeIdRef,
    PipeStep,
    Ref,
    RepeatStep,
    SkipStep,
    StatusRef,
    Step,
    WaitStep,
    status_code,
)
from a11.nodes.async_node import AsyncNode, NodeMap
from a11.status import Status, StatusCode, StatusException

#: How many values a pipe may run ahead of its reader. Small on purpose: A11's
#: own stores are the buffer, and a flow should not become a second one.
QUEUE_DEPTH = 8

def _fail(message: str, code: StatusCode = StatusCode.INVALID_ARGUMENT):
    return Status(code=code, message=message).to_exception()


# --- Items -------------------------------------------------------------------


class Item:
    """One value travelling through a pipe.

    An item read from a node keeps its native [Chunk][a11.data.types.Chunk], so
    a pipe that only moves values re-writes exactly the producer's bytes and
    mimetype and never pays for a round trip through Python. A stage that looks
    at the value decodes it once, and from then on the item carries the value.
    """

    __slots__ = ("chunk", "_value", "_decoded", "_registry")

    def __init__(
        self,
        chunk: types.Chunk | None = None,
        value: Any = None,
        decoded: bool = False,
        registry: Any = None,
    ) -> None:
        self.chunk = chunk
        self._value = value
        self._decoded = decoded
        self._registry = registry

    @classmethod
    def of(cls, value: Any) -> "Item":
        """An item holding a decoded value."""
        return cls(value=value, decoded=True)

    @property
    def mimetype(self) -> str:
        if self.chunk is None:
            return "application/json"
        return self.chunk.metadata.mimetype

    @property
    def registry(self) -> Any:
        """The registry this item was read with, or the process-wide one."""
        return self._registry or get_global_serialization_registry()

    async def value(self) -> Any:
        """The decoded value, deserialising the chunk the first time."""
        if not self._decoded:
            registry = self._registry or get_global_serialization_registry()
            chunk = self.chunk
            self._value = (
                None
                if chunk is None
                else await asyncio.to_thread(
                    registry.from_chunk, chunk, "", None
                )
            )
            self._decoded = True
        return self._value


def _base_mimetype(mimetype: str) -> str:
    """A mimetype without its parameters: ``application/x-msgpack;type=X``."""
    return mimetype.split(";", 1)[0].strip().lower()


async def _packed(item: Item) -> Item:
    """``item`` as MessagePack, or ``item`` itself when it already is one.

    An item read from a node keeps the producer's bytes, so a value that
    arrived packed is passed on untouched, tag and all. Anything else is
    decoded once and re-encoded, which is the only point at which a flow pays
    for the conversion.
    """
    if item.chunk is not None and _base_mimetype(item.mimetype) == (
        MSGPACK_MIMETYPE
    ):
        return item
    registry = item.registry
    value = await item.value()
    chunk = await asyncio.to_thread(registry.to_chunk, value, MSGPACK_MIMETYPE)
    return Item(chunk=chunk, value=value, decoded=True, registry=registry)


class _End:
    """The end of a subscription, carrying the producer's error if any."""

    __slots__ = ("error",)

    def __init__(self, error: BaseException | None = None) -> None:
        self.error = error


# --- Buses -------------------------------------------------------------------


class _Subscription:
    """One reader's view of a stream."""

    __slots__ = ("queue", "dropped", "_bus")

    def __init__(self, bus: "_Bus") -> None:
        self.queue: asyncio.Queue = asyncio.Queue(maxsize=QUEUE_DEPTH)
        self.dropped = False
        self._bus = bus

    async def __aiter__(self) -> AsyncIterator[Item]:
        self._bus.wanted()
        while True:
            item = await self.queue.get()
            if isinstance(item, _End):
                if item.error is not None:
                    raise item.error
                return
            yield item

    def close(self) -> None:
        """Stop reading.

        The producer is not cancelled -- it keeps going into a discarded queue.
        A node being read on the other end of it may be feeding an action that
        would stall if nobody drained it, and a `first 3` in a flow must not be
        able to wedge the step it is reading from.
        """
        if self.dropped:
            return
        self.dropped = True
        self._bus.wanted()
        self._bus.discard(self)


class _Bus:
    """A stream with a fixed number of readers, fed by one producer."""

    def __init__(
        self,
        label: str,
        produce: Callable[[], AsyncIterator[Item]],
        readers: int,
    ) -> None:
        self.label = label
        self.produce = produce
        self.subscriptions = [_Subscription(self) for _ in range(readers)]
        self._handed_out = 0
        self._discarding: list[asyncio.Task] = []
        self._demand = asyncio.Event()

    def wanted(self) -> None:
        """Somebody has started reading, so the producer may start."""
        self._demand.set()

    def take(self) -> _Subscription:
        if self._handed_out >= len(self.subscriptions):
            raise _fail(
                f"Internal flow error: {self.label} has more readers than the "
                "plan accounted for."
            )
        subscription = self.subscriptions[self._handed_out]
        self._handed_out += 1
        return subscription

    def discard(self, subscription: _Subscription) -> None:
        async def drain() -> None:
            while True:
                item = await subscription.queue.get()
                if isinstance(item, _End):
                    return

        self._discarding.append(asyncio.ensure_future(drain()))

    async def pump(self) -> None:
        # Nothing is read before something asks for it. Reading can *act* --
        # ending a node, waiting on a call -- and a step held back by `after`
        # must not have its reads run ahead of it.
        await self._demand.wait()
        error: BaseException | None = None
        try:
            async for item in self.produce():
                for subscription in self.subscriptions:
                    if not subscription.dropped:
                        await subscription.queue.put(item)
        except asyncio.CancelledError:
            for subscription in self.subscriptions:
                subscription.queue.put_nowait(_End(None))
            raise
        except BaseException as raised:  # noqa: BLE001 - relayed to readers
            error = raised
        for subscription in self.subscriptions:
            await subscription.queue.put(_End(error))
        if self._discarding:
            await asyncio.gather(*self._discarding, return_exceptions=True)


class _Lazy:
    """A single value computed the first time it is asked for, then shared.

    What a status is: reading one waits for its subject and may end a node, so
    it happens when a step asks, and once however many steps ask.
    """

    def __init__(self, produce: Callable[[], Any]) -> None:
        self._produce = produce
        self._value: asyncio.Future | None = None

    async def _get(self) -> Item:
        if self._value is None:
            self._value = asyncio.get_running_loop().create_future()
            try:
                self._value.set_result(await self._produce())
            except BaseException as raised:  # noqa: BLE001 - relayed to readers
                self._value.set_exception(raised)
        return await asyncio.shield(self._value)

    async def replay(self) -> AsyncIterator[Item]:
        yield await self._get()


class _Materialised:
    """A stream buffered once and replayed to every reader.

    This is what a ref read inside a loop or a branch becomes. The buffer is
    filled by a single reader of the underlying stream, and each pass of the
    loop iterates the buffer, so every pass sees the same values.
    """

    def __init__(self, subscription: _Subscription) -> None:
        self.subscription = subscription
        self.items: list[Item] = []
        self.ready = asyncio.Event()
        self.error: BaseException | None = None

    async def fill(self) -> None:
        try:
            async for item in self.subscription.__aiter__():
                self.items.append(item)
        except BaseException as raised:  # noqa: BLE001 - relayed to readers
            self.error = raised
        finally:
            self.ready.set()

    async def replay(self) -> AsyncIterator[Item]:
        await self.ready.wait()
        if self.error is not None:
            raise self.error
        for item in list(self.items):
            yield item


class _Preset:
    """A stream the runtime binds directly: a loop's value, index, or carry."""

    def __init__(self, items: Sequence[Item]) -> None:
        self.items = list(items)

    async def replay(self) -> AsyncIterator[Item]:
        for item in self.items:
            yield item


# --- Destinations ------------------------------------------------------------


class _Destination:
    """A node several steps may write, closed when the last of them is done."""

    def __init__(
        self,
        label: str,
        node: Callable[[], Any],
        writers: int,
        tolerant: bool = False,
    ) -> None:
        self.label = label
        self._node = node
        self._writers = writers
        #: Whether a failure writing here is this destination's own business.
        self.tolerant = tolerant
        self.finished = asyncio.Event()
        self._lock = asyncio.Lock()
        self._closed = False
        if writers <= 0:
            # Nothing here will write it, so nothing here is holding it open.
            self.finished.set()

    async def node(self) -> Any:
        """The node being written."""
        return await self._node()

    async def write(self, item: Item) -> None:
        """Append one value, as the producer wrote it wherever possible."""
        node = await self._node()
        # One writer at a time, so two steps sharing a destination append whole
        # values rather than interleaving halves of two.
        async with self._lock:
            try:
                if item.chunk is not None:
                    await (await node.put_chunk(item.chunk))
                else:
                    await (await node.put(await item.value()))
            except StatusException:
                # A `try call` that has already failed or been cancelled has
                # aborted its ports; feeding one is then not the flow's problem.
                if not self.tolerant:
                    raise

    @property
    def writers(self) -> int:
        """How many writers have still to finish."""
        return self._writers

    async def end(self) -> None:
        """Close the node now, whoever was writing it.

        What `drain` does to a node the flow does not write itself: a callee
        given a node to write does not close it -- it does not own it -- so the
        flow that lent it the node is the one that says when it is over.
        """
        async with self._lock:
            if self._closed:
                return
            self._closed = True
            self._writers = 0
            node = await self._node()
            try:
                await _close_node(node)
            except StatusException:
                if not self.tolerant:
                    raise
            finally:
                self.finished.set()

    async def release(self) -> None:
        """One writer is done; close the node when it was the last.

        The close writes the null final chunk that says the stream is over, so a
        reader waiting on a whole value -- a `one` port, or anything calling
        ``consume`` -- is told the value has ended rather than left waiting.
        """
        async with self._lock:
            self._writers -= 1
            if self._writers > 0 or self._closed:
                return
            self._closed = True
            node = await self._node()
            try:
                await _close_node(node)
            except StatusException:
                if not self.tolerant:
                    raise
            finally:
                self.finished.set()


# --- Static analysis ---------------------------------------------------------


class _Analysis:
    """Who reads and who writes each ref a body owns.

    Computed once per body and cached on it. The counts matter: a stream gets
    exactly as many readers as the plan says it has, and a node is closed
    exactly when the last of its writers finishes -- including a loop or a
    branch, which counts as one writer for as long as it runs.
    """

    def __init__(self, body: Body) -> None:
        self.body = body
        self.readers: dict[int, int] = {}
        self.materialise: set[int] = set()
        self.refs: dict[int, Ref] = {}
        self.writers: dict[int, int] = {}
        self.held: dict[int, tuple[Ref, ...]] = {}
        self.dest_refs: dict[int, Ref] = {}
        self.nodes: dict[int, Any] = {}
        self._compute()

    def _compute(self) -> None:
        body = self.body
        nested = _refs_used_in(body.nested_bodies())
        local: dict[int, int] = {}
        owned: dict[int, Ref] = {}

        def note(ref: Ref, count: int = 1) -> None:
            if ref.owner is body:
                owned[ref.uid] = ref
                local[ref.uid] = local.get(ref.uid, 0) + count

        for step in body.steps:
            for ref in step.sources():
                note(ref)
            for ref in step.value_sources():
                note(ref)

        # A node's id is computed once per pass, however many steps name the
        # node, so its readers are counted here rather than per mention.
        self.nodes = _node_refs_in(body)
        for node_ref in self.nodes.values():
            for id_ref in node_ref.id_refs():
                note(id_ref)

        for ref in nested.values():
            if ref.owner is body:
                owned.setdefault(ref.uid, ref)

        # A derived or computed stream reads its own upstream, so the upstream
        # needs a reader for it. A ref is always built from refs created before
        # it, so taking the highest id still to do settles the whole chain --
        # including the ones only reached through another derivation.
        settled: set[int] = set()
        while True:
            waiting = [uid for uid in owned if uid not in settled]
            if not waiting:
                break
            uid = max(waiting)
            settled.add(uid)
            ref = owned[uid]
            if local.get(uid, 0) == 0 and uid not in nested:
                continue
            for upstream in ref.upstreams():
                note(upstream)
            for value_ref in ref.value_refs():
                note(value_ref)

        for uid, ref in owned.items():
            self.refs[uid] = ref
            if uid in nested:
                self.materialise.add(uid)
                self.readers[uid] = 1
            else:
                self.readers[uid] = local.get(uid, 0)

        for step in body.steps:
            held: list[Ref] = []
            for ref in step.destinations():
                if ref.owner is body:
                    held.append(ref)
                self._count_writer(ref, body)
            for ref in step.observed():
                # Only a ref this flow could write becomes a destination; a
                # readable one is finished by being read to its end instead.
                if ref.owner is body and ref.writable:
                    self.dest_refs.setdefault(ref.uid, ref)
            nested_bodies = list(step.bodies())
            if nested_bodies:
                deep: list[Body] = []
                for nested_body in nested_bodies:
                    deep.append(nested_body)
                    deep.extend(nested_body.nested_bodies())
                for ref in _dests_written_in(deep):
                    if ref.owner is body:
                        held.append(ref)
                        self._count_writer(ref, body)
            self.held[id(step)] = tuple(held)

    def _count_writer(self, ref: Ref, body: Body) -> None:
        if ref.owner is not body:
            return
        self.dest_refs[ref.uid] = ref
        self.writers[ref.uid] = self.writers.get(ref.uid, 0) + 1


def _refs_used_in(bodies: Iterable[Body]) -> dict[int, Ref]:
    """Every ref these bodies read, following derivations to their source."""
    found: dict[int, Ref] = {}

    def walk(ref: Ref) -> None:
        if ref.uid in found:
            return
        found[ref.uid] = ref
        for upstream in ref.upstreams():
            walk(upstream)
        for value_ref in ref.value_refs():
            walk(value_ref)

    for body in bodies:
        for step in body.steps:
            for ref in step.sources():
                walk(ref)
            for ref in step.value_sources():
                walk(ref)
    return found


def _node_refs_in(body: Body) -> dict[int, Any]:
    """The nodes of its own that a body names, wherever it names them."""
    found: dict[int, Any] = {}

    def walk(ref: Ref) -> None:
        if isinstance(ref, LocalNodeRef) and ref.owner is body:
            found.setdefault(ref.uid, ref)
        if isinstance(ref, NodeIdRef):
            walk(ref.node)
        if isinstance(ref, StatusRef) and isinstance(ref.subject, Ref):
            walk(ref.subject)
        for upstream in ref.upstreams():
            walk(upstream)
        for value_ref in ref.value_refs():
            walk(value_ref)

    for step in body.steps:
        for ref in (
            *step.sources(),
            *step.value_sources(),
            *step.destinations(),
            *step.observed(),
        ):
            walk(ref)
    return found


def _dests_written_in(bodies: Iterable[Body]) -> list[Ref]:
    """Every ref the given bodies write, once each."""
    found: dict[int, Ref] = {}
    for body in bodies:
        for step in body.steps:
            for ref in step.destinations():
                found.setdefault(ref.uid, ref)
    return list(found.values())


def _analysis(body: Body) -> _Analysis:
    if body.analysis is None:
        body.analysis = _Analysis(body)
    return body.analysis


# --- Calls -------------------------------------------------------------------


class _CallHandle:
    """One instance of a `call` step: its action, its ports, its completion."""

    def __init__(self, step: CallStep, schema: ActionSchema) -> None:
        self.step = step
        self.schema = schema
        self.action: Action | None = None
        self.started: asyncio.Future[Action] = (
            asyncio.get_running_loop().create_future()
        )
        self.done = asyncio.Event()
        self.error: BaseException | None = None
        #: Input ports nobody writes, and output ports nobody reads: closed and
        #: drained for the callee once it has started.
        self.unclosed: list[AsyncNode] = []
        self.undrained: list[AsyncNode] = []

    async def node(self, name: str, direction: str) -> AsyncNode:
        action = await asyncio.shield(self.started)
        if direction == "inputs":
            return action.get_input(name, bind_stream=None)
        return action.get_output(name, bind_stream=None)

    async def outcome(self) -> Status:
        """How the call went, once it has gone."""
        await self.done.wait()
        if self.error is not None:
            return Status.from_exception(self.error)
        action = self.action
        return Status.ok() if action is None else action.get_status()


def _check_ports(step: CallStep, schema: ActionSchema) -> None:
    """Reject a port the target does not declare, before anything runs.

    The check cannot always happen while compiling -- an action's schema comes
    from the registry of whatever runtime dispatches the flow -- so it happens
    here, once, with the same wording the compiler would have used.
    """
    for (direction, name), ref in step.ports.items():
        declared = schema.inputs if direction == "inputs" else schema.outputs
        if name in declared:
            continue
        known = ", ".join(sorted(declared)) or "none"
        raise _fail(
            f"{step.action} has no {direction[:-1]} port {name!r} "
            f"(declared: {known}), but {ref.label} names one.",
            StatusCode.NOT_FOUND,
        )


# --- Scopes ------------------------------------------------------------------


class _Scope:
    """One running instance of a body: the flow's top level, or a loop pass."""

    def __init__(
        self,
        runner: "_Runner",
        body: Body,
        parent: "_Scope | None" = None,
        presets: Mapping[int, Sequence[Item]] | None = None,
    ) -> None:
        self.runner = runner
        self.body = body
        self.parent = parent
        self.analysis = _analysis(body)
        self.presets = {
            uid: _Preset(items) for uid, items in (presets or {}).items()
        }
        self.buses: dict[int, _Bus] = {}
        self.lazies: dict[int, _Lazy] = {}
        self.materialised: dict[int, _Materialised] = {}
        self.destinations: dict[int, _Destination] = {}
        self.calls: dict[int, _CallHandle] = {}
        self.events: dict[int, asyncio.Event] = {}
        self.captures: dict[str, Any] = {}
        self._expr_values: dict[tuple[int, int], Any] = {}
        self._nodes: dict[int, asyncio.Future[AsyncNode]] = {}
        self._unwritten: list[LocalNodeRef] = []

    # -- wiring ----------------------------------------------------------------

    def _prepare(self) -> None:
        """Create this pass's calls, streams and destinations, in ref order."""
        for step in self.body.steps:
            self.events[id(step)] = asyncio.Event()
            if isinstance(step, CallStep):
                schema = self.runner.schema_of(step)
                _check_ports(step, schema)
                self.calls[id(step)] = _CallHandle(step, schema)

        for uid in sorted(self.analysis.refs):
            readers = self.analysis.readers.get(uid, 0)
            if readers <= 0 or uid in self.presets:
                continue
            ref = self.analysis.refs[uid]
            if isinstance(ref, StatusRef):
                self.lazies[uid] = _Lazy(
                    lambda ref=ref: self._status_item(ref)
                )
                continue
            bus = _Bus(ref.label, lambda ref=ref: self._produce(ref), readers)
            self.buses[uid] = bus
            if uid in self.analysis.materialise:
                self.materialised[uid] = _Materialised(bus.take())

        # A node of the flow's own that nothing writes still has to end, or a
        # reader of it would wait for a value that was never coming.
        self._unwritten = [
            ref
            for uid, ref in self.analysis.nodes.items()
            if ref.id_expr is None
            and uid not in self.analysis.dest_refs
            and self.analysis.readers.get(uid, 0) > 0
        ]

        for uid, ref in self.analysis.dest_refs.items():
            writers = self.analysis.writers.get(uid, 0)
            self.destinations[uid] = _Destination(
                ref.label,
                lambda ref=ref: self._destination_node(ref),
                writers,
                tolerant=isinstance(ref, CallPortRef) and ref.call.tolerant,
            )

    async def _destination_node(self, ref: Ref) -> AsyncNode:
        if isinstance(ref, CallPortRef):
            return await self.call(ref.call).node(ref.name, ref.direction)
        if isinstance(ref, FlowPortRef):
            return self.runner.action.get_output(ref.name)
        if isinstance(ref, LocalNodeRef):
            return await self.local_node(ref)
        raise _fail(f"{ref.label} is not something a flow can write.")

    async def local_node(self, ref: LocalNodeRef) -> AsyncNode:
        """The node ``ref`` names, made once per pass through its body.

        A declaration with an id attaches to the node that names -- the one a
        caller passed in a header, say -- and one without makes a fresh node in
        the active node map, which is where a flow keeps a stream of its own.
        """
        scope = self._owner_scope(ref)
        pending = scope._nodes.get(ref.uid)
        if pending is None:
            pending = asyncio.get_running_loop().create_future()
            scope._nodes[ref.uid] = pending
            try:
                pending.set_result(await scope._make_node(ref))
            except BaseException as raised:  # noqa: BLE001 - relayed to readers
                pending.set_exception(raised)
        return await asyncio.shield(pending)

    async def _make_node(self, ref: LocalNodeRef) -> AsyncNode:
        node_map = (
            self.runner.node_map(ref.node_map)
            if ref.node_map is not None
            else self.runner.action.get_node_map()
        )
        if ref.id_expr is not None:
            named = await self.evaluate(ref.id_expr)
            node_id = values.as_text(
                named.get("id") if isinstance(named, Mapping) else named
            )
            if not node_id:
                raise _fail(
                    f"{ref.label} was given no node id to attach to."
                )
        else:
            node_id = self.runner.fresh_node_id(ref.name)
        return node_map.get(node_id)

    # -- lookups ---------------------------------------------------------------

    def call(self, step: CallStep) -> _CallHandle:
        scope: _Scope | None = self
        while scope is not None:
            handle = scope.calls.get(id(step))
            if handle is not None:
                return handle
            scope = scope.parent
        raise _fail(f"Internal flow error: {step.name} was never started.")

    def destination(self, ref: Ref) -> _Destination:
        scope: _Scope | None = self
        while scope is not None:
            found = scope.destinations.get(ref.uid)
            if found is not None:
                return found
            scope = scope.parent
        raise _fail(f"Internal flow error: nothing writes {ref.label}.")

    def subscribe(self, ref: Ref) -> AsyncIterator[Item]:
        """An independent view of ``ref``'s values for one reader."""
        scope = self._owner_scope(ref)
        preset = scope.presets.get(ref.uid)
        if preset is not None:
            return preset.replay()
        lazy = scope.lazies.get(ref.uid)
        if lazy is not None:
            return lazy.replay()
        buffered = scope.materialised.get(ref.uid)
        if buffered is not None:
            return buffered.replay()
        bus = scope.buses.get(ref.uid)
        if bus is None:
            raise _fail(
                f"Internal flow error: {ref.label} has no reader slot left."
            )
        return bus.take().__aiter__()

    def _owner_scope(self, ref: Ref) -> "_Scope":
        scope: _Scope | None = self
        while scope is not None:
            if scope.body is ref.owner:
                return scope
            scope = scope.parent
        raise _fail(
            f"Internal flow error: {ref.label} is not in scope here."
        )

    async def first_value(self, ref: Ref, site: int) -> Any:
        """The first value of ``ref``, read once per expression site."""
        key = (site, ref.uid)
        if key in self._expr_values:
            return self._expr_values[key]
        stream = self.subscribe(ref)
        value: Any = None
        async for item in stream:
            value = await item.value()
            break
        await _stop(stream)
        self._expr_values[key] = value
        return value

    async def evaluate(self, expr: Expr, it: Any = values.MISSING) -> Any:
        """Evaluate an expression here, reading each ref it mentions once."""
        captured = {
            ref.uid: await self.first_value(ref, expr.uid) for ref in expr.refs
        }
        return values.evaluate(expr.node, captured, it)

    # -- stream production -----------------------------------------------------

    def _produce(self, ref: Ref) -> AsyncIterator[Item]:
        stream = self._produce_all(ref)
        # One place, upstream of the bus that fans the stream out, so the
        # values `skip n` spoke for are the same ones every reader misses.
        return stream if ref.skip == 0 else _without_first(stream, ref.skip)

    def _produce_all(self, ref: Ref) -> AsyncIterator[Item]:
        """Everything ``ref`` has, before any counted `skip` is taken off."""
        if isinstance(ref, DerivedRef):
            return _apply_stage(self, ref)
        if isinstance(ref, ExprRef):
            return self._produce_expr(ref)
        if isinstance(ref, HeaderRef):
            return self._produce_header(ref)
        if isinstance(ref, NodeIdRef):
            return self._produce_node_id(ref)
        if isinstance(ref, LocalNodeRef):
            return _read_node(self.local_node(ref), tolerant=False)
        if isinstance(ref, CallPortRef):
            return _read_node(
                self.call(ref.call).node(ref.name, ref.direction),
                tolerant=ref.call.tolerant,
            )
        if isinstance(ref, FlowPortRef):
            return _read_node(
                _immediate(self.runner.action.get_input(ref.name)),
                tolerant=False,
            )
        raise _fail(f"{ref.label} is not something a flow can read.")

    async def _produce_expr(self, ref: ExprRef) -> AsyncIterator[Item]:
        yield Item.of(await self.evaluate(ref.expr))

    async def _produce_header(self, ref: HeaderRef) -> AsyncIterator[Item]:
        raw = self.runner.action.get_header(ref.name, decode=True)
        if raw is None:
            if ref.default is None:
                return
            yield Item.of(ref.default)
            return
        yield Item.of(raw)

    async def _produce_node_id(self, ref: NodeIdRef) -> AsyncIterator[Item]:
        yield Item.of((await self.local_node(ref.node)).get_id())

    async def _status_item(self, ref: StatusRef) -> Item:
        """One status, once the subject is finished: a synchronisation point."""
        subject = ref.subject
        if isinstance(subject, CallStep):
            return Item.of(_status_record(await self.call(subject).outcome()))
        if isinstance(subject, Ref):
            return Item.of(_status_record(await self._node_outcome(subject)))
        raise _fail(f"{ref.label} has no status to read.")

    async def _node_outcome(self, ref: Ref) -> Status:
        """A node's outcome: its writers are done, or its stream has ended.

        A node this flow writes is finished when the last writer has closed it,
        so the status is the one the node was closed or aborted with. One it
        only reads is finished when the stream ends, and the status is the
        reader's -- which is how an output cut off mid-stream gets noticed.
        """
        written = self._find_destination(ref)
        if written is not None:
            if written.writers <= 0:
                # Nothing in this flow writes it, so nothing in this flow will
                # close it either unless this barrier does.
                await written.end()
            await written.finished.wait()
            node = await written.node()
            aborted = node.get_writer_abort_status()
            return aborted if aborted is not None else node.get_writer_status()
        subscription = self.subscribe(ref)
        try:
            async for _ in subscription:
                pass
        except StatusException as raised:
            return raised.status
        node = await self._readable_node(ref)
        return Status.ok() if node is None else node.get_reader_status()

    async def _readable_node(self, ref: Ref) -> AsyncNode | None:
        if isinstance(ref, LocalNodeRef):
            return await self.local_node(ref)
        if isinstance(ref, CallPortRef):
            return await self.call(ref.call).node(ref.name, ref.direction)
        if isinstance(ref, FlowPortRef) and ref.direction == "inputs":
            return self.runner.action.get_input(ref.name)
        return None

    def _find_destination(self, ref: Ref) -> "_Destination | None":
        scope: _Scope | None = self
        while scope is not None:
            found = scope.destinations.get(ref.uid)
            if found is not None:
                return found
            scope = scope.parent
        return None

    # -- execution -------------------------------------------------------------

    async def run(self) -> None:
        self._prepare()
        async with asyncio.TaskGroup() as group:
            for ref in self._unwritten:
                group.create_task(self._end_unwritten(ref), name="end-node")
            for bus in self.buses.values():
                group.create_task(bus.pump(), name=f"bus:{bus.label}")
            for buffer in self.materialised.values():
                group.create_task(buffer.fill(), name="materialise")
            for step in self.body.steps:
                group.create_task(
                    self._run_step(step), name=f"{self.body.label}:{step.label}"
                )

    async def _end_unwritten(self, ref: LocalNodeRef) -> None:
        await _close_node(await self.local_node(ref))

    async def _run_step(self, step: Step) -> None:
        try:
            for dependency in step.after:
                await self.step_done(dependency)
            await self._execute(step)
        finally:
            with contextlib.suppress(Exception):
                for ref in self.analysis.held.get(id(step), ()):
                    await self.destination(ref).release()
            self.events[id(step)].set()

    async def step_done(self, step: Step) -> None:
        scope: _Scope | None = self
        while scope is not None:
            event = scope.events.get(id(step))
            if event is not None:
                await event.wait()
                return
            scope = scope.parent
        raise _fail(f"Internal flow error: {step.label} is not in scope.")

    async def _execute(self, step: Step) -> None:
        if isinstance(step, CallStep):
            await self.runner.run_call(self, step)
            return
        if isinstance(step, PipeStep):
            destination = self.destination(step.destination)
            async for item in self.subscribe(step.source):
                await destination.write(item)
            return
        if isinstance(step, SkipStep):
            if step.count is not None:
                # The values are already gone: the count was applied where the
                # stream is produced. Reading here would take a reader slot
                # this step was never counted for.
                return
            async for _ in self.subscribe(step.source):
                pass
            return
        if isinstance(step, CaptureStep):
            async for item in self.subscribe(step.source):
                self.captures[step.slot] = await item.value()
                break
            self.captures.setdefault(step.slot, None)
            return
        if isinstance(step, WaitStep):
            await self._wait(step)
            return
        if isinstance(step, CancelStep):
            handle = self.call(step.call)
            action = await asyncio.shield(handle.started)
            action.cancel()
            return
        if isinstance(step, FailStep):
            raise await self._failure(step)
        if isinstance(step, IfStep):
            taken = bool(await self.evaluate(step.condition))
            body = step.then_body if taken else step.else_body
            if body.steps:
                await _Scope(self.runner, body, parent=self).run()
            return
        if isinstance(step, ForEachStep):
            await self._run_for_each(step)
            return
        if isinstance(step, RepeatStep):
            await self._run_repeat(step)
            return
        raise _fail(f"Cannot run a {type(step).__name__}.")

    async def _wait(self, step: WaitStep) -> None:
        """Read the subject's status, and let a bad one through when it is ours.

        A subject a flow said it would handle -- a `try` step -- reports and the
        flow carries on. Anything else that finished badly ends the flow here,
        with the status it finished with rather than a new one.
        """
        reading = self._read_status(step)
        if step.timeout is None:
            record = await reading
        else:
            try:
                record = await asyncio.wait_for(
                    reading, step.timeout.float_seconds()
                )
            except TimeoutError:
                raise _fail(
                    f"Waiting for "
                    f"{_plan._subject_label(step.outcome.subject)} timed out "
                    f"after {step.timeout}.",
                    StatusCode.DEADLINE_EXCEEDED,
                ) from None
        if step.tolerant or not isinstance(record, Mapping):
            return
        if not record.get("ok", True):
            raise _status_of(record).to_exception()

    async def _read_status(self, step: WaitStep) -> Any:
        async for item in self.subscribe(step.outcome):
            return await item.value()
        return None

    async def _failure(self, step: FailStep) -> BaseException:
        """The status a ``fail`` statement raises."""
        code = (
            None if step.code is None else await self.evaluate(step.code)
        )
        message = (
            None if step.message is None else await self.evaluate(step.message)
        )
        if message is None and isinstance(code, Mapping):
            # `fail check` -- raise the status the flow recovered from.
            return _status_of(code).to_exception()
        if message is None and status_code(code) is None:
            code, message = None, code
        resolved = status_code(code) or StatusCode.INTERNAL
        if resolved is StatusCode.OK:
            resolved = StatusCode.INTERNAL
        text = values.as_text(message) if message is not None else ""
        return _fail(text or f"{self.runner.flow.name} failed.", resolved)

    async def _run_for_each(self, step: ForEachStep) -> None:
        counter = itertools.count()
        limit = asyncio.Semaphore(step.parallel)

        async def pass_over(item: Item, index: int, admitted: bool) -> None:
            try:
                await _Scope(
                    self.runner,
                    step.loop_body,
                    parent=self,
                    presets={
                        step.item.uid: [item],
                        step.index.uid: [Item.of(index)],
                    },
                ).run()
            finally:
                if admitted:
                    limit.release()

        async with asyncio.TaskGroup() as group:
            async for item in self.subscribe(step.source):
                index = next(counter)
                if step.parallel == 1:
                    await pass_over(item, index, False)
                    continue
                # Admission before the task, so a wide `parallel` does not turn
                # a long stream into a pile of pending passes.
                await limit.acquire()
                group.create_task(
                    pass_over(item, index, True),
                    name=f"{step.label}[{index}]",
                )

    async def _run_repeat(self, step: RepeatStep) -> None:
        carried: Any = step.start
        for index in range(step.max_iterations):
            scope = _Scope(
                self.runner,
                step.loop_body,
                parent=self,
                presets={
                    step.carry.uid: [Item.of(carried)],
                    step.index.uid: [Item.of(index)],
                },
            )
            await scope.run()
            if step.condition is not None:
                captured = {
                    ref.uid: scope.captures.get(f"condition:{ref.uid}")
                    for ref in step.condition.refs
                }
                holds = bool(
                    values.evaluate(step.condition.node, captured)
                )
                if holds is step.stop_when:
                    return
            if step.carry_source is not None:
                carried = scope.captures.get("carry")


# --- Stages ------------------------------------------------------------------


async def _apply_stage(scope: _Scope, ref: DerivedRef) -> AsyncIterator[Item]:
    """Read ``ref``'s source and reshape it with one stage."""
    stage = ref.stage
    name = stage.name
    argument = stage.arg
    source = scope.subscribe(ref.source)

    if name == "at":
        async for item in source:
            yield Item.of(values.lookup(await item.value(), argument))
        return
    if name == "map":
        async for item in source:
            yield Item.of(await scope.evaluate(argument, await item.value()))
        return
    if name == "where":
        async for item in source:
            if await scope.evaluate(argument, await item.value()):
                yield item
        return
    if name == "mime":
        async for item in source:
            if fnmatch.fnmatch(item.mimetype, argument):
                yield item
        return
    if name == "first":
        taken = 0
        if argument <= 0:
            return
        async for item in source:
            yield item
            taken += 1
            if taken >= argument:
                break
        await _stop(source)
        return
    if name == "last":
        tail: list[Item] = []
        async for item in source:
            tail.append(item)
            if len(tail) > argument:
                tail.pop(0)
        for item in tail:
            yield item
        return
    if name == "drop":
        seen = 0
        async for item in source:
            seen += 1
            if seen > argument:
                yield item
        return
    if name == "truncate":
        async for item in source:
            yield Item.of(values.truncate(await item.value(), argument))
        return
    if name == "batch":
        group: list[Any] = []
        async for item in source:
            group.append(await item.value())
            if len(group) >= argument:
                yield Item.of(group)
                group = []
        if group:
            yield Item.of(group)
        return
    if name == "then":
        # All of this one, then all of that one. Two writers to a node
        # interleave by arrival, which is fine for pages and wrong for a
        # conversation; this is how a flow says which comes first.
        async for item in source:
            yield item
        async for item in scope.subscribe(argument):
            yield item
        return
    if name == "group":
        # `batch`, closed by a question rather than by a count: values pile up
        # until one of them says the group is finished, and the group goes on
        # as a list. What is left when the stream ends goes too -- a partial
        # group is still what was said.
        gathered: list[Any] = []
        async for item in source:
            value = await item.value()
            gathered.append(value)
            if await scope.evaluate(argument, value):
                yield Item.of(gathered)
                gathered = []
        if gathered:
            yield Item.of(gathered)
        return
    if name == "collect":
        collected = [await item.value() async for item in source]
        yield Item.of(collected)
        return
    if name == "count":
        total = 0
        async for _ in source:
            total += 1
        yield Item.of(total)
        return
    if name == "distinct":
        seen_values: list[Any] = []
        async for item in source:
            value = await item.value()
            key = values.as_text(value)
            if key not in seen_values:
                seen_values.append(key)
                yield item
        return
    if name == "join":
        pieces = [values.as_text(await item.value()) async for item in source]
        yield Item.of(str(argument or "").join(pieces))
        return
    if name == "strformat":
        # The one-value shorthand: `| strformat "took {}"` is exactly
        # `| map strformat("took {}", it)`, which is the shape almost every use
        # of it has. The full builtin is there when more than one value goes in.
        async for item in source:
            yield Item.of(
                values.strformat(argument, [await item.value()])
            )
        return
    if name == "text":
        async for item in source:
            yield Item.of(values.as_text(await item.value()))
        return
    if name == "json":
        async for item in source:
            yield Item.of(values.as_json(await item.value()))
        return
    if name == "packb":
        async for item in source:
            yield await _packed(item)
        return
    raise _fail(f"Unknown stage {name!r}.")


# --- Node reading ------------------------------------------------------------


async def _immediate(value: Any) -> Any:
    return value


async def _stop(source: Any) -> None:
    """Stop reading a stream, however it is backed.

    A subscription is dropped rather than cancelled: whatever is producing it
    keeps going into a discarded buffer, so a `first 3` cannot wedge the action
    on the other end of the node it is reading.
    """
    closer = getattr(source, "close", None)
    if closer is not None:
        closer()
        return
    aclose = getattr(source, "aclose", None)
    if aclose is not None:
        await aclose()


async def _read_node(
    node_awaitable: Any, tolerant: bool
) -> AsyncIterator[Item]:
    """Read a node as items, each keeping the producer's own chunk.

    A null chunk is a marker rather than a value: a final one ends the node, and
    any other is skipped, which is how an empty stream stays empty instead of
    turning into a value nobody wrote.
    """
    node = await node_awaitable
    registry = node.serialization_registry
    try:
        while True:
            fragment = await node.next_fragment()
            if fragment is None:
                return
            chunk = fragment.get_chunk()
            if chunk.is_null():
                if not fragment.continued:
                    return
                continue
            yield Item(chunk=chunk, registry=registry)
    except StatusException:
        # The producer aborted the node. A `try call` says the composition
        # expects that and the stream simply ends; otherwise the call step is
        # the one that reports it, so there is no need to fail twice.
        if not tolerant:
            raise
        return


def _status_record(status: Status) -> dict[str, Any]:
    """A status as data: what a flow sees when it looks at an outcome."""
    return {
        "ok": status.is_ok(),
        "code": status.code.name,
        "number": int(status.code),
        "message": status.message,
    }


def _status_of(record: Mapping[str, Any]) -> Status:
    """The status a record like the one above describes."""
    code = status_code(record.get("code"))
    if code is None:
        code = status_code(record.get("number"))
    return Status(
        code=code or StatusCode.UNKNOWN,
        message=values.as_text(record.get("message", "")),
    )


# --- The runner --------------------------------------------------------------


class _Runner:
    """One execution of one flow, against the action it is running as.

    ``dispatch_stream`` is for a flow run *by a client* over a session it
    already has: the calls that belong to the peer are bound to that stream,
    and the flow's own action is not. Binding it to the flow itself would end
    the stream when the flow finishes, and the session could dispatch nothing
    afterwards -- see [invoke][a11.flow.runtime.invoke].
    """

    def __init__(
        self,
        flow: FlowPlan,
        action: Action,
        dispatch_stream: Any = None,
    ) -> None:
        self.flow = flow
        self.action = action
        self.registry = action.get_registry()
        self.program = flow.program
        self.dispatch_stream = dispatch_stream
        self._node_maps: dict[str, NodeMap] = {}
        self._schemas: dict[str, ActionSchema] = {}
        self._node_counts: dict[str, int] = {}

    async def run(self) -> None:
        await _Scope(self, self.flow.root).run()

    def node_map(self, name: str) -> NodeMap:
        """The temporary node map named by a `nodes` block.

        One map per name per execution. Nodes created in it are not in the
        session's node map, so a peer neither sees them nor receives their
        fragments -- which is the point of putting a step's traffic there.
        """
        existing = self._node_maps.get(name)
        if existing is None:
            existing = NodeMap()
            self._node_maps[name] = existing
        return existing

    def fresh_node_id(self, name: str) -> str:
        """An id for a node the flow declared, unique within this run.

        Named after the flow's own action and the name the flow gave it, so a
        node a peer does see is recognisable rather than a bare identifier.
        """
        count = self._node_counts.get(name, 0) + 1
        self._node_counts[name] = count
        suffix = name if count == 1 else f"{name}-{count}"
        return Action.make_node_id(self.action.id, suffix)

    def resolve(self, name: str) -> tuple[ActionSchema, Any]:
        """The schema and handler for a call target.

        The handler may be ``None``: an action registered for its schema alone
        is one whose ports are known here and whose *work* happens on the
        peer, and that is what a flow composing a gateway's actions from the
        outside registers. Such an action can only be `call`ed; what `run`
        needs is a handler, and saying `run` without one is an error rather
        than a quiet trip to the session.
        """
        if self.program is not None and name in self.program:
            target = self.program[name]
            if self.dispatch_stream is not None:
                # A flow of this program that this flow runs is still the same
                # client's, so its own `call` steps belong on the same stream.
                # It cannot inherit that: a `run` step's action is bound to no
                # stream precisely so its nodes stay off the wire.
                return target.schema, make_handler(target, self.dispatch_stream)
            return target.schema, target.handler
        registry = self.registry
        if registry is None or not registry.is_registered(name):
            known = (
                ", ".join(sorted(registry.list_registered_actions()))
                if registry is not None
                else "none"
            )
            raise _fail(
                f"{self.flow.name} calls {name!r}, which is not a flow in this "
                f"program and is not registered here (registered: {known}).",
                StatusCode.NOT_FOUND,
            )
        try:
            handler = registry.get_handler(name)
        except StatusException:
            # Registered without one. The registry reports that as an error
            # rather than as `None`, so asking is how it is found out.
            handler = None
        return registry.get_schema(name), handler

    def schema_of(self, step: CallStep) -> ActionSchema:
        cached = self._schemas.get(step.action)
        if cached is None:
            cached = self.resolve(step.action)[0]
            self._schemas[step.action] = cached
        return cached

    def _forwarded(self, step: CallStep) -> dict[str, bytes]:
        """The headers `forward headers` sends on to ``step``, as they arrived.

        A pattern matches the flow's own headers -- what its caller sent -- and
        `*` in one matches any run of characters, so
        ``forward headers "x-tenant-*"`` is a family rather than a list. Nothing
        is invented: a header that was not sent is simply not forwarded, because
        a composition should not fail over an optional one nobody supplied.

        A11 already gives a nested action every ``x-a11-`` header of its parent,
        so this is for the ones outside that prefix -- an ``authorization``, a
        tenant id -- and for saying out loud that a step is meant to see one.
        """
        if not step.forward_headers:
            return {}
        available = dict(self.action.headers)
        chosen: dict[str, bytes] = {}
        for pattern in step.forward_headers:
            folded = pattern.lower()
            for name, value in available.items():
                if fnmatch.fnmatchcase(name.lower(), folded):
                    chosen[name] = value
        return chosen

    async def run_call(self, scope: _Scope, step: CallStep) -> None:
        """Dispatch one call, feed it, drain it, and wait for it."""
        handle = scope.call(step)
        try:
            await self._start_call(scope, step, handle)
        except BaseException as raised:
            # A step that never started still has to say so. Everything wiring
            # itself to this call waits on `handle.started`, and
            # `_CallHandle.node` shields that wait -- deliberately, so a reader
            # is not cancelled out from under a call that is still starting --
            # which means an unresolved future here is a deadlock rather than a
            # cancellation. Failing it hands every waiter the same reason.
            if not handle.started.done():
                handle.started.set_exception(raised)
            handle.error = raised
            handle.done.set()
            raise
        await self._pump_call(step, handle)

    async def _start_call(
        self, scope: _Scope, step: CallStep, handle: _CallHandle
    ) -> None:
        """Build the nested action and its ports, and mark the call started."""
        schema, handler = self.resolve(step.action)
        mode = step.mode
        if mode == "run" and handler is None:
            raise _fail(
                f"{self.flow.name} says 'run {step.action}', but "
                f"{step.action} is registered here for its schema alone and "
                f"has no handler to run. Say 'call' to dispatch it on the "
                f"stream this flow is attached to.",
                StatusCode.FAILED_PRECONDITION,
            )

        nested = self.action.make_nested(schema)
        if step.node_map is not None:
            nested.bind_node_map(self.node_map(step.node_map))
        if mode == "call" and self.dispatch_stream is not None:
            # The flow is a client's, and this call is the peer's: give it the
            # stream the flow itself deliberately does not hold.
            nested.bind_stream(self.dispatch_stream)
        if mode == "run":
            nested.bind_handler(handler)
            if not step.tee:
                # Keep a local step's streams off the wire: the peer that
                # dispatched this flow asked for its outputs, not for every
                # intermediate node inside it.
                nested.bind_stream(None)
        # `forward headers` first, so an explicit `with` of the same name -- the
        # more specific of the two -- is the one that lands.
        for name, value in self._forwarded(step).items():
            nested.set_header(name, value)
        for name, expr in step.headers.items():
            value = await scope.evaluate(expr)
            nested.set_header(name, _header_bytes(value))
        if step.action_id is not None:
            nested.set_id(values.as_text(await scope.evaluate(step.action_id)))

        # Create every port node before the action starts, so a reader that
        # subscribes later still sees the whole stream from its beginning.
        written = {
            ref.name
            for ref in scope.analysis.dest_refs.values()
            if isinstance(ref, CallPortRef) and ref.call is step
        }
        unclosed: list[AsyncNode] = []
        for name, declared in schema.inputs.items():
            node = nested.get_input(name, bind_stream=None)
            if name not in written and not declared.autofills:
                unclosed.append(node)
        read = {
            ref.name
            for ref in scope.analysis.refs.values()
            if isinstance(ref, CallPortRef)
            and ref.call is step
            and ref.direction == "outputs"
            and scope.analysis.readers.get(ref.uid, 0) > 0
        }
        undrained = [
            nested.get_output(name, bind_stream=None)
            for name in schema.outputs
            if name not in read
        ]

        handle.action = nested
        handle.unclosed = unclosed
        handle.undrained = undrained
        handle.started.set_result(nested)

    async def _pump_call(self, step: CallStep, handle: _CallHandle) -> None:
        """Dispatch the started call, feed it, drain it, and wait for it."""
        nested = handle.action
        assert nested is not None
        mode = step.mode
        unclosed, undrained = handle.unclosed, handle.undrained
        try:
            if mode == "run":
                nested.run()
            else:
                try:
                    await nested.call()
                except StatusException as raised:
                    raise _fail(
                        f"{self.flow.name} could not call "
                        f"{step.action!r} on its session: "
                        f"{raised.status.message}",
                        raised.status.code,
                    ) from raised
            async with asyncio.TaskGroup() as group:
                for node in unclosed:
                    group.create_task(_close_node(node), name="close")
                for node in undrained:
                    group.create_task(
                        _drain(node, step.tolerant), name="auto-drain"
                    )
                group.create_task(self._await_call(handle, step), name="wait")
        finally:
            handle.done.set()
        if handle.error is not None and not step.tolerant:
            raise handle.error

    async def _await_call(self, handle: _CallHandle, step: CallStep) -> None:
        action = handle.action
        assert action is not None
        try:
            if step.timeout is None:
                await action.wait()
            else:
                await action.wait(step.timeout)
        except asyncio.CancelledError:
            # A `cancel` statement cancels the call, which cancels the wait for
            # it. That is this call's outcome, not the flow being cancelled --
            # unless the flow really is going away too.
            if action.cancelled() and not self.action.cancelled():
                handle.error = action.get_status().to_exception()
                return
            raise
        except BaseException as raised:  # noqa: BLE001 - reported by the caller
            handle.error = raised


async def _without_first(
    stream: AsyncIterator[Item], count: int
) -> AsyncIterator[Item]:
    """``stream`` with its first ``count`` values read and dropped."""
    seen = 0
    async for item in stream:
        seen += 1
        if seen > count:
            yield item


async def _close_node(node: AsyncNode) -> None:
    """End a node: mark the stream over, then close the write half."""
    await (await node.put_null_final())
    await node.drain_and_close()


async def _drain(node: AsyncNode, tolerant: bool) -> None:
    try:
        while await node.next_fragment() is not None:
            pass
    except StatusException:
        if not tolerant:
            raise


def _header_bytes(value: Any) -> bytes:
    if isinstance(value, bytes):
        return value
    return values.as_text(value).encode()


def make_handler(
    flow: FlowPlan, dispatch_stream: Any = None
) -> Callable[[Action], Any]:
    """The action handler that runs ``flow``.

    Registering this makes the composition an action like any other: a peer can
    dispatch it, another flow can call it, and an LLM can be offered it as a
    tool, without any of them knowing it is a composition.

    ``dispatch_stream`` is only for a flow a client runs itself over a session
    it already holds; [invoke][a11.flow.runtime.invoke] passes it, and nothing
    that registers a flow as an action needs it.
    """

    async def run_flow(action: Action) -> None:
        try:
            await _Runner(flow, action, dispatch_stream).run()
        except ExceptionGroup as group:
            raise _first_error(group) from None

    run_flow.__name__ = f"flow:{flow.name}"
    run_flow.__qualname__ = run_flow.__name__
    run_flow.__doc__ = flow.description or f"The {flow.name} flow."
    return run_flow


def _first_error(group: BaseException) -> BaseException:
    """The most useful single failure out of a nest of task-group errors."""
    if isinstance(group, ExceptionGroup):
        for error in group.exceptions:
            found = _first_error(error)
            if isinstance(found, StatusException):
                return found
        return _first_error(group.exceptions[0])
    return group


class Running:
    """A flow that has been started, and the output nodes it is filling.

    A flow's outputs are [AsyncNode][a11.nodes.async_node.AsyncNode]s like any
    other action's, so the honest way to read one is to read the node -- values
    arrive as the flow produces them. [invoke][a11.flow.runtime.invoke]'s dict
    of lists is a convenience on top of this for the callers that want the lot
    at the end (a tool call, a test, a script), not the other way round.
    """

    __slots__ = ("action", "outputs", "flow", "_timeout")

    def __init__(
        self,
        flow: FlowPlan,
        action: Action,
        outputs: Mapping[str, AsyncNode],
        timeout: timing.Duration | None,
    ) -> None:
        self.flow = flow
        #: The action the flow is running as.
        self.action = action
        #: Every declared output port, by name, live.
        self.outputs = dict(outputs)
        self._timeout = timeout

    def __getitem__(self, name: str) -> AsyncNode:
        return self.outputs[name]

    async def wait(self) -> None:
        """Wait for the flow to finish, raising whatever it finished with."""
        await self.action.wait(self._timeout)

    def publish(self, stream: Any, names: Iterable[str] | None = None) -> dict:
        """Mirror the named outputs to ``stream``, and say where they landed.

        Returns the node id of each published port, which is what a peer needs
        to read it: `NodeMap.get(id)` on the other side of the stream is the
        same node. The *action* is deliberately not bound to the stream -- one
        that is run locally and holds a stream ends it when it finishes, and
        the session would then be unable to dispatch anything else.
        """
        published: dict[str, str] = {}
        for name in self.outputs if names is None else names:
            node = self.outputs[name]
            node.attach_stream(stream)
            published[name] = node.get_id()
        return published

    async def collect(self) -> dict[str, Any]:
        """Wait for the flow, and gather every output port into a dict.

        One value for a `one` port, a list for a `many` one. Reading starts
        before the wait, because an output nobody reads stalls the flow filling
        it -- the same rule that applies to any action's ports.
        """
        collectors = {
            name: asyncio.ensure_future(_collect(node))
            for name, node in self.outputs.items()
        }
        try:
            await self.wait()
        except BaseException:
            for collector in collectors.values():
                collector.cancel()
            raise
        gathered: dict[str, Any] = {}
        for name, collector in collectors.items():
            values = await collector
            port = self.flow.outputs[name]
            gathered[name] = (
                (values[0] if values else None) if port.unary else values
            )
        return gathered


async def start(
    flow: FlowPlan,
    inputs: Mapping[str, Any] | None = None,
    *,
    registry: Any = None,
    session: Any = None,
    node_map: Any = None,
    stream: Any = None,
    dispatch_stream: Any = None,
    headers: Mapping[str, Any] | None = None,
    timeout: timing.Duration | None = None,
    action_id: str | None = None,
    **keyword_inputs: Any,
) -> Running:
    """Start ``flow``, feed it its inputs, and hand back its live outputs.

    The streaming entry point, and what [invoke][a11.flow.runtime.invoke] is
    built on. Everything is running when this returns: the inputs have been
    written and closed, and each output node is filling as the flow produces
    values. The caller reads the nodes, and calls
    [Running.wait][a11.flow.runtime.Running.wait] when it wants the flow's
    status.

    ``action_id`` names the flow's action, which fixes its output node ids at
    ``<action_id>#<port>`` -- what a caller that means to publish them to a
    peer wants, so both ends can work the ids out.

    See [invoke][a11.flow.runtime.invoke] for ``stream`` against
    ``dispatch_stream``.
    """
    inputs = {**(inputs or {}), **keyword_inputs}
    action = Action(
        flow.schema,
        handler=make_handler(flow, dispatch_stream=dispatch_stream),
        registry=registry,
        session=session,
        node_map=node_map,
        stream=stream,
    )
    if action_id is not None:
        action.set_id(action_id)
    for name, value in (headers or {}).items():
        action.set_header(name, _header_bytes(value))

    declared_inputs = flow.inputs
    for name in inputs:
        if name not in declared_inputs:
            known = ", ".join(sorted(declared_inputs)) or "none"
            raise _fail(
                f"{flow.name} has no input port {name!r} (declared: {known})."
            )

    # Created before the flow starts, so a reader that subscribes later still
    # sees the stream from its beginning.
    outputs = {
        name: action.get_output(name, bind_stream=False)
        for name in flow.outputs
    }
    running = Running(flow, action, outputs, timeout)

    action.run()
    for name, value in inputs.items():
        node = action.get_input(name, bind_stream=False)
        for one in value if isinstance(value, (list, tuple)) else [value]:
            await (await node.put(one))
        await (await node.put_null_final())
        await node.drain_and_close()
    for name in declared_inputs:
        if name not in inputs:
            node = action.get_input(name, bind_stream=False)
            await (await node.put_null_final())
            await node.drain_and_close()
    return running


async def invoke(
    flow: FlowPlan,
    inputs: Mapping[str, Any] | None = None,
    *,
    registry: Any = None,
    session: Any = None,
    node_map: Any = None,
    stream: Any = None,
    dispatch_stream: Any = None,
    headers: Mapping[str, Any] | None = None,
    timeout: timing.Duration | None = None,
    **keyword_inputs: Any,
) -> dict[str, Any]:
    """Run ``flow`` once, here, and return its outputs collected.

    The convenience path for tests, scripts, and tool calls: it runs the flow
    to completion and returns every output port keyed by name -- one value for
    a `one` port, a list for a `many` one. Inputs may be keywords or a mapping
    (which is what a port whose name collides with one of the options needs).
    ``registry``, ``session`` and ``node_map`` place the flow in an existing
    runtime, so its calls resolve and dispatch exactly as they would inside a
    server.

    Collecting is the convenience, not the mechanism: a flow's outputs are
    nodes, and [start][a11.flow.runtime.start] hands them over live for a
    caller that would rather read them as they fill.

    The two stream arguments are different questions, and a caller wants
    exactly one of them:

    * ``stream`` runs the flow **as though a peer had dispatched it** over that
      stream. The flow's own ports are mirrored to the peer, which is what a
      server's caller is waiting for.
    * ``dispatch_stream`` runs the flow **as the client's own**, and gives the
      stream only to the calls that go to the peer. The flow's ports stay here.
      This is what a client with a session of its own wants: an action that is
      run locally *and* holds a stream ends that stream when it finishes, after
      which the session can dispatch nothing -- so a client passing ``stream``
      would find its second flow unable to reach the peer at all.
    """
    running = await start(
        flow,
        inputs,
        registry=registry,
        session=session,
        node_map=node_map,
        stream=stream,
        dispatch_stream=dispatch_stream,
        headers=headers,
        timeout=timeout,
        **keyword_inputs,
    )
    return await running.collect()


async def _collect(node: AsyncNode) -> list[Any]:
    gathered: list[Any] = []
    async for value in node:
        gathered.append(value)
    return gathered


__all__ = ["Item", "Running", "invoke", "make_handler", "start"]

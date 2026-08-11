"""The resolved plan a Flow program compiles to.

The parser produces syntax; this module turns it into the graph the runtime
walks. Names are bound here -- a word becomes a port of the flow, a port of a
call, a loop variable, a header, or a node map -- and every stream in the
source becomes a [Ref][a11.flow.plan.Ref], every statement a
[Step][a11.flow.plan.Step].

Two rules are worth knowing, because they are what makes the graph predictable:

* **Steps run concurrently.** Order comes from the data, not from the order the
  statements were written in. A call is dispatched at once and its inputs stream
  in while it works. Where an order is genuinely needed, `after`, `wait` and
  `drain` say so.
* **A stream read inside a loop or branch is materialised.** The runtime
  buffers it once, in the scope that owns it, and replays the buffer to each
  reader. That is what lets every pass of a loop see the same outer value, and
  it is the one place the language trades streaming for repeatability.
"""

from __future__ import annotations

import itertools
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any

from a11 import timing
from a11.actions.action import ActionSchema
from a11.flow import syntax
from a11.flow.lexer import FlowSyntaxError, canonical
from a11.flow.parser import STAGES, parse
from a11.status import StatusCode

_uid = itertools.count(1)

#: Friendly type names, and the Python type each gives a port. The type drives
#: the JSON schema A11 shows an LLM, so a flow's ports describe themselves as
#: well as a hand-written action's do.
TYPE_NAMES: dict[str, type | None] = {
    "string": str,
    "text": str,
    "number": float,
    "integer": int,
    "int": int,
    "bool": bool,
    "boolean": bool,
    "object": dict,
    "json": dict,
    "list": list,
    "array": list,
    "bytes": bytes,
    "any": None,
}

#: How many type parameters a built-in type may be given: a list says what it
#: holds, an object what it maps. Everything else takes none, and a type the
#: registries know by tag -- ``a11.sdk.AudioBuffer`` -- is already concrete.
TYPE_PARAMETERS: dict[str, tuple[int, ...]] = {
    "list": (0, 1),
    "array": (0, 1),
    "object": (0, 2),
    "json": (0, 2),
}

#: The canonical status codes, by the name Abseil and gRPC give them. A flow may
#: write either case -- ``not_found`` or ``NOT_FOUND`` -- and a code computed at
#: runtime may be the number instead.
STATUS_CODES: dict[str, StatusCode] = {
    name.lower(): code for name, code in StatusCode.__members__.items()
}

#: The names ``fail`` accepts, in the case they are canonically spelled in.
FAIL_CODES = tuple(sorted(StatusCode.__members__))


#: The fields of a status record, which is what a flow sees when it looks at an
#: outcome. A reference ending in one of these reads the record rather than
#: naming a port -- ``status x.code`` is the code it finished with.
STATUS_FIELDS = ("ok", "code", "number", "message")


def status_code(value: Any) -> StatusCode | None:
    """The canonical code ``value`` names, by name or by number.

    Accepts either case of a canonical name (``data_loss``, ``DATA_LOSS``) and
    any number Abseil defines a code for, which is what lets a flow re-raise a
    status it was handed without knowing how it was spelled.
    """
    if isinstance(value, StatusCode):
        return value
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        try:
            return StatusCode(value)
        except ValueError:
            return None
    if isinstance(value, str):
        return STATUS_CODES.get(canonical(value.strip()).replace("-", "_"))
    return None


# --- Expressions -------------------------------------------------------------


@dataclass
class RefValue(syntax.Node):
    """A resolved name inside an expression: the ref's first value."""

    ref: "Ref"


class Expr:
    """One resolved expression, and the refs it reads.

    An expression is evaluated where it appears -- a condition once, a
    ``where``/``map`` stage once per value -- and each ref it mentions
    contributes its *first* value, read once and remembered for as long as the
    expression's step is running.
    """

    __slots__ = ("node", "refs", "uid")

    def __init__(self, node: syntax.Node, refs: Sequence["Ref"]) -> None:
        self.node = node
        self.refs = tuple(refs)
        self.uid = next(_uid)

    def describe(self) -> str:
        return unparse(self.node)

    def __repr__(self) -> str:
        return f"<Expr {self.describe()}>"


@dataclass(frozen=True)
class Stage:
    """One resolved pipeline stage."""

    name: str
    arg: Any = None

    def describe(self) -> Any:
        if isinstance(self.arg, Expr):
            return f"{self.name} {self.arg.describe()}"
        if isinstance(self.arg, Ref):
            return f"{self.name} {self.arg.label}"
        if self.arg in (None, ""):
            return self.name
        return f"{self.name} {self.arg!r}"

    @property
    def exprs(self) -> tuple[Expr, ...]:
        return (self.arg,) if isinstance(self.arg, Expr) else ()

    @property
    def streams(self) -> tuple["Ref", ...]:
        """The streams this stage reads besides the one it is applied to.

        Only `then` has one: the stage takes a stream rather than a value, and
        the plan has to know it is read so that whatever produces it is run and
        counted.
        """
        return (self.arg,) if isinstance(self.arg, Ref) else ()


# --- Refs --------------------------------------------------------------------


class Ref:
    """A stream in the plan: a port, a header, a constant, or a derivation."""

    __slots__ = ("uid", "owner", "label", "skip")

    def __init__(self, owner: "Body", label: str) -> None:
        self.uid = next(_uid)
        #: The body this ref belongs to, which is where it is materialised.
        self.owner = owner
        self.label = label
        #: How many of this stream's first values `skip n` has spoken for.
        #: Applied where the stream is produced, which is upstream of the
        #: fan-out, so it is the same values every reader does not see. Several
        #: `skip n` statements naming the same node add up -- the ref is one
        #: object however many times the flow mentions it.
        self.skip = 0

    @property
    def readable(self) -> bool:
        return True

    @property
    def writable(self) -> bool:
        return False

    def upstreams(self) -> tuple["Ref", ...]:
        """Refs read as streams to produce this one."""
        return ()

    def value_refs(self) -> tuple["Ref", ...]:
        """Refs read for their first value to produce this one."""
        return ()

    def describe(self) -> str:
        return self.label

    def __repr__(self) -> str:
        return f"<{type(self).__name__} {self.label}>"


class FlowPortRef(Ref):
    """One of the flow's own declared ports."""

    __slots__ = ("name", "direction", "unary")

    def __init__(
        self, owner: "Body", name: str, direction: str, unary: bool
    ) -> None:
        super().__init__(owner, name)
        self.name = name
        self.direction = direction
        self.unary = unary

    @property
    def readable(self) -> bool:
        return self.direction == "inputs"

    @property
    def writable(self) -> bool:
        return self.direction == "outputs"


class CallPortRef(Ref):
    """A port of an action the flow calls."""

    __slots__ = ("call", "name", "direction")

    def __init__(self, call: "CallStep", name: str, direction: str) -> None:
        super().__init__(call.body, f"{call.name}.{name}")
        self.call = call
        self.name = name
        self.direction = direction

    @property
    def readable(self) -> bool:
        return self.direction == "outputs"

    @property
    def writable(self) -> bool:
        return self.direction == "inputs"


class StatusRef(Ref):
    """The status of something, as a record: one value, once it is finished.

    Each value is ``{"ok": bool, "code": "NOT_FOUND", "number": 5, "message":
    str}`` -- data, so a flow can look at an outcome without any of the language
    knowing what an `a11.status.Status` is. The subject may be a call, a node or
    port, or a `wait`/`drain` barrier.

    Reading one is a synchronisation point: the subject has to finish first.
    That is deliberate, and it is what makes a recoverable failure
    recoverable: `try` keeps the flow alive, and this is how the flow finds
    out what happened and decides what to do about it.
    """

    __slots__ = ("subject",)

    def __init__(self, subject: Any) -> None:
        super().__init__(
            _subject_body(subject), f"status {_subject_label(subject)}"
        )
        self.subject = subject

    @property
    def call(self) -> "CallStep | None":
        """The call this is the status of, if it is a call's."""
        return self.subject if isinstance(self.subject, CallStep) else None

    def subject_node(self) -> "LocalNodeRef | None":
        """The node this is the status of, if it is a node's."""
        return self.subject if isinstance(self.subject, LocalNodeRef) else None


class LocalNodeRef(Ref):
    """A node of the flow's own: a stream it can write and read back.

    A flow makes one to fan several steps into one place, to keep a value it
    needs twice, or to hold something a peer will read. With an ``id``
    expression it attaches to a node somebody else named -- the node a caller
    passed in a header, say -- and writes there instead.

    The node lands in the contextually active node map, which is the enclosing
    `nodes` block's if there is one and the action's otherwise, unless the
    declaration names another.
    """

    __slots__ = ("name", "id_expr", "node_map")

    def __init__(
        self,
        owner: "Body",
        name: str,
        id_expr: "Expr | None",
        node_map: str | None,
    ) -> None:
        super().__init__(owner, name)
        self.name = name
        self.id_expr = id_expr
        self.node_map = node_map

    @property
    def writable(self) -> bool:
        return True

    def id_refs(self) -> tuple[Ref, ...]:
        """Refs the id expression reads; counted once per pass, not per use."""
        return self.id_expr.refs if self.id_expr is not None else ()


class NodeIdRef(Ref):
    """A node's id, as one value.

    What a flow hands to an action that expects to be told which node to write:
    a progress node in a header, or a node id in a request.
    """

    __slots__ = ("node",)

    def __init__(self, node: LocalNodeRef) -> None:
        super().__init__(node.owner, f"{node.label}.id")
        self.node = node


def _subject_body(subject: Any) -> "Body":
    if isinstance(subject, Ref):
        return subject.owner
    return subject.body


def _subject_label(subject: Any) -> str:
    if isinstance(subject, Ref):
        return subject.label
    return getattr(subject, "name", None) or subject.label


class HeaderRef(Ref):
    """One header of the call running this flow, as a single value."""

    __slots__ = ("name", "default")

    def __init__(self, owner: "Body", name: str, default: Any) -> None:
        super().__init__(owner, f'header "{name}"')
        self.name = name
        self.default = default


class ExprRef(Ref):
    """A stream of one value: an expression evaluated once."""

    __slots__ = ("expr",)

    def __init__(self, owner: "Body", expr: Expr) -> None:
        super().__init__(owner, expr.describe())
        self.expr = expr

    def value_refs(self) -> tuple[Ref, ...]:
        return self.expr.refs


class DerivedRef(Ref):
    """A stream with one stage applied to another stream."""

    __slots__ = ("source", "stage")

    def __init__(self, owner: "Body", source: Ref, stage: Stage) -> None:
        super().__init__(owner, f"{source.label} | {stage.describe()}")
        self.source = source
        self.stage = stage

    def upstreams(self) -> tuple[Ref, ...]:
        return (self.source, *self.stage.streams)

    def value_refs(self) -> tuple[Ref, ...]:
        return tuple(
            ref for expr in self.stage.exprs for ref in expr.refs
        )


class BoundRef(Ref):
    """A stream the runtime binds per pass: a loop's value, index, or carry."""

    __slots__ = ("role", "step")

    def __init__(self, owner: "Body", step: "Step", role: str) -> None:
        super().__init__(owner, role)
        self.role = role
        self.step = step


# --- Bodies and steps --------------------------------------------------------


class Body:
    """A block of steps: a flow's top level, or a loop or branch body."""

    __slots__ = ("label", "parent", "steps", "owner_step", "analysis")

    def __init__(
        self, label: str, parent: "Body | None" = None, owner_step: Any = None
    ) -> None:
        self.label = label
        self.parent = parent
        self.steps: list["Step"] = []
        self.owner_step = owner_step
        #: Cache for the runtime's static analysis of this body.
        self.analysis: Any = None

    def add(self, step: "Step") -> "Step":
        self.steps.append(step)
        return step

    def nested_bodies(self) -> list["Body"]:
        """Every body inside this one, at any depth."""
        found: list[Body] = []
        for step in self.steps:
            for body in step.bodies():
                found.append(body)
                found.extend(body.nested_bodies())
        return found

    def describe(self) -> list[dict[str, Any]]:
        return [step.describe() for step in self.steps]

    def __repr__(self) -> str:
        return f"<Body {self.label} ({len(self.steps)} steps)>"


class Step:
    """One resolved statement."""

    kind = "step"

    def __init__(self, body: Body, label: str) -> None:
        self.body = body
        self.label = label
        self.after: tuple["Step", ...] = ()

    def sources(self) -> tuple[Ref, ...]:
        """Refs this step reads as streams, one entry per independent read."""
        return ()

    def value_sources(self) -> tuple[Ref, ...]:
        """Refs this step reads for one value each."""
        return tuple(ref for expr in self.exprs() for ref in expr.refs)

    def exprs(self) -> tuple[Expr, ...]:
        """Expressions evaluated by this step."""
        return ()

    def destinations(self) -> tuple[Ref, ...]:
        """Refs this step writes, one entry per independent writer."""
        return ()

    def observed(self) -> tuple[Ref, ...]:
        """Written refs this step only watches, without writing them itself."""
        return ()

    def bodies(self) -> tuple[Body, ...]:
        """Bodies nested directly in this step."""
        return ()

    def describe(self) -> dict[str, Any]:
        described: dict[str, Any] = {"step": self.kind, "label": self.label}
        if self.after:
            described["after"] = [step.label for step in self.after]
        return described


class CallStep(Step):
    """A call to another action.

    ``mode`` is the verb the flow was written with, and it is the same
    distinction A11 draws everywhere else. ``"run"`` executes the handler
    registered here -- a handler that has to exist, or the step fails rather
    than quietly going elsewhere -- and keeps its ports off the session's wire
    unless ``tee`` asks for them, so an intermediate stream between two `run`
    steps is never replicated to a peer. ``"call"`` dispatches on the stream
    the flow is attached to, whether or not a handler for the action happens to
    be registered here as well.

    Either way the action's **schema** has to be registered here, because that
    is where the ports being wired up come from. A composition written against
    a peer's actions registers their schemas and no handlers -- which is both
    how it learns the port names and why every step of it is a ``call``.
    """

    kind = "call"

    def __init__(
        self,
        body: Body,
        name: str,
        action: str,
        *,
        mode: str,
        tee: bool = False,
        node_map: str | None = None,
        timeout: timing.Duration | None = None,
        tolerant: bool = False,
        headers: Mapping[str, Expr] | None = None,
        forward_headers: Sequence[str] = (),
        action_id: Expr | None = None,
        target_schema: ActionSchema | None = None,
    ) -> None:
        super().__init__(body, name)
        #: The binding name, and the label used in diagnostics.
        self.name = name
        self.action = action
        self.mode = mode
        #: A described step says which verb it was written with, the way a
        #: `wait`/`drain` step says which of those it was.
        self.kind = mode
        self.tee = tee
        self.node_map = node_map
        self.timeout = timeout
        self.tolerant = tolerant
        self.headers = dict(headers or {})
        #: Header names (or ``*`` patterns) `forward headers` sends on as they
        #: arrived, without the flow declaring a name for each.
        self.forward_headers = tuple(forward_headers)
        self.action_id = action_id
        self.target_schema = target_schema
        self.ports: dict[tuple[str, str], CallPortRef] = {}
        self.status_ref: StatusRef | None = None

    def port(self, name: str, direction: str) -> CallPortRef:
        existing = self.ports.get((direction, name))
        if existing is not None:
            return existing
        created = CallPortRef(self, name, direction)
        self.ports[(direction, name)] = created
        return created

    def status(self) -> StatusRef:
        """The status of this call, as one value once it has finished."""
        if self.status_ref is None:
            self.status_ref = StatusRef(self)
        return self.status_ref

    def exprs(self) -> tuple[Expr, ...]:
        found = list(self.headers.values())
        if self.action_id is not None:
            found.append(self.action_id)
        return tuple(found)

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        described.update(
            {
                "action": self.action,
                "mode": self.mode,
                "ports": sorted(
                    f"{direction}.{name}" for direction, name in self.ports
                ),
            }
        )
        if self.node_map:
            described["via"] = self.node_map
        if self.tee:
            described["tee"] = True
        if self.tolerant:
            described["try"] = True
        if self.headers:
            described["headers"] = {
                name: value.describe() for name, value in self.headers.items()
            }
        if self.forward_headers:
            described["forward"] = list(self.forward_headers)
        if self.timeout is not None:
            described["timeout"] = self.timeout.float_seconds()
        return described


class PipeStep(Step):
    """``source -> destination``."""

    kind = "pipe"

    def __init__(self, body: Body, source: Ref, destination: Ref) -> None:
        super().__init__(body, f"{source.label} -> {destination.label}")
        self.source = source
        self.destination = destination

    def sources(self) -> tuple[Ref, ...]:
        return (self.source,)

    def destinations(self) -> tuple[Ref, ...]:
        return (self.destination,)

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        described.update(
            {"from": self.source.describe(), "to": self.destination.describe()}
        )
        return described


class SkipStep(Step):
    """``skip source`` -- read to the end, keep nothing.

    With a ``count`` it is the other statement: not a reader at all, but the
    declaration that the source's first ``count`` values are nobody's. The
    count lives on the ref (see [Ref.skip][a11.flow.plan.Ref.skip]) and is
    applied where the stream is produced, so this step has nothing left to do
    at runtime and claims no reader slot.
    """

    kind = "skip"

    def __init__(self, body: Body, source: Ref, count: int | None = None):
        label = (
            f"skip {source.label}"
            if count is None
            else f"skip {count} of {source.label}"
        )
        super().__init__(body, label)
        self.source = source
        self.count = count

    def sources(self) -> tuple[Ref, ...]:
        return () if self.count is not None else (self.source,)

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        described["of"] = self.source.describe()
        if self.count is not None:
            described["count"] = self.count
        return described


class WaitStep(Step):
    """``wait subject`` -- hold until it is finished.

    The subject is a call, or a node this flow writes, or one of its ports. In
    every case the step reads the subject's [status][a11.flow.plan.StatusRef],
    which is why the same statement bound to a name is also the way to *get*
    that status: ``check = wait risky`` waits, and ``check.ok`` says how it
    went.

    A subject that finished badly fails the flow, unless it is a `try` step --
    those are the failures a flow said it would handle itself.
    """

    kind = "wait"

    def __init__(
        self,
        body: Body,
        outcome: StatusRef,
        timeout: timing.Duration | None,
        tolerant: bool = False,
        spelling: str = "wait",
    ) -> None:
        super().__init__(body, f"{spelling} {_subject_label(outcome.subject)}")
        self.outcome = outcome
        self.timeout = timeout
        #: Whether a bad status here is the flow's business or the subject's.
        self.tolerant = tolerant
        self.kind = spelling

    def sources(self) -> tuple[Ref, ...]:
        return (self.outcome,)

    def observed(self) -> tuple[Ref, ...]:
        subject = self.outcome.subject
        return (subject,) if isinstance(subject, Ref) else ()

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        described["of"] = _subject_label(self.outcome.subject)
        if self.timeout is not None:
            described["timeout"] = self.timeout.float_seconds()
        return described


class DrainStep(WaitStep):
    """``drain node`` -- its writers are done and its buffer has landed.

    The same barrier as `wait` on a node, spelled for the place it reads best:
    beside the port it is about.
    """

    def __init__(
        self, body: Body, outcome: StatusRef, timeout: timing.Duration | None
    ) -> None:
        super().__init__(body, outcome, timeout, spelling="drain")


class CancelStep(Step):
    """``cancel call`` -- ask the called action to stop."""

    kind = "cancel"

    def __init__(self, body: Body, call: CallStep) -> None:
        super().__init__(body, f"cancel {call.name}")
        self.call = call

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        described["of"] = self.call.name
        return described


class FailStep(Step):
    """``fail`` -- end the flow with a status.

    Both operands are expressions, so a status a flow recovered from can be
    raised again as it stands (``fail check``), given a better message
    (``fail check.code "could not reach the index"``), or replaced outright.
    """

    kind = "fail"

    def __init__(
        self, body: Body, code: "Expr | None", message: "Expr | None"
    ) -> None:
        super().__init__(body, "fail")
        self.code = code
        self.message = message

    def exprs(self) -> tuple["Expr", ...]:
        return tuple(
            expr for expr in (self.code, self.message) if expr is not None
        )

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        if self.code is not None:
            described["code"] = self.code.describe()
        if self.message is not None:
            described["message"] = self.message.describe()
        return described


class CaptureStep(Step):
    """Remember a stream's first value for the loop that owns this body."""

    kind = "capture"

    def __init__(self, body: Body, source: Ref, slot: str) -> None:
        super().__init__(body, f"capture {slot}")
        self.source = source
        self.slot = slot

    def sources(self) -> tuple[Ref, ...]:
        return (self.source,)

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        described.update({"slot": self.slot, "of": self.source.describe()})
        return described


class ForEachStep(Step):
    """``for value in stream { ... }``."""

    kind = "for"

    def __init__(
        self, body: Body, label: str, source: Ref, parallel: int
    ) -> None:
        super().__init__(body, label)
        self.source = source
        self.parallel = parallel
        self.loop_body = Body(f"{label}.body", parent=body, owner_step=self)
        self.item = BoundRef(self.loop_body, self, "item")
        self.index = BoundRef(self.loop_body, self, "index")

    def sources(self) -> tuple[Ref, ...]:
        return (self.source,)

    def bodies(self) -> tuple[Body, ...]:
        return (self.loop_body,)

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        described.update(
            {
                "over": self.source.describe(),
                "parallel": self.parallel,
                "body": self.loop_body.describe(),
            }
        )
        return described


class RepeatStep(Step):
    """``repeat carry = start max n { ... }``."""

    kind = "repeat"

    def __init__(
        self, body: Body, label: str, start: Any, max_iterations: int
    ) -> None:
        super().__init__(body, label)
        self.start = start
        self.max_iterations = max_iterations
        self.loop_body = Body(f"{label}.body", parent=body, owner_step=self)
        self.carry = BoundRef(self.loop_body, self, "carry")
        self.index = BoundRef(self.loop_body, self, "index")
        self.carry_source: Ref | None = None
        self.condition: Expr | None = None
        self.stop_when = True

    def bodies(self) -> tuple[Body, ...]:
        return (self.loop_body,)

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        described.update(
            {
                "start": self.start,
                "max": self.max_iterations,
                "carry": (
                    self.carry_source.describe() if self.carry_source else None
                ),
                "until" if self.stop_when else "while": (
                    self.condition.describe() if self.condition else None
                ),
                "body": self.loop_body.describe(),
            }
        )
        return described


class IfStep(Step):
    """``if condition { ... } else { ... }``."""

    kind = "if"

    def __init__(self, body: Body, label: str, condition: Expr) -> None:
        super().__init__(body, label)
        self.condition = condition
        self.then_body = Body(f"{label}.then", parent=body, owner_step=self)
        self.else_body = Body(f"{label}.else", parent=body, owner_step=self)

    def exprs(self) -> tuple[Expr, ...]:
        return (self.condition,)

    def bodies(self) -> tuple[Body, ...]:
        return (self.then_body, self.else_body)

    def describe(self) -> dict[str, Any]:
        described = super().describe()
        described.update(
            {
                "condition": self.condition.describe(),
                "then": self.then_body.describe(),
                "else": self.else_body.describe(),
            }
        )
        return described


# --- Flows and programs ------------------------------------------------------


@dataclass
class PortPlan:
    """A declared port of a flow."""

    name: str
    direction: str
    type: Any
    unary: bool
    required: bool
    description: str
    #: The type as the flow wrote it, parameters and all: ``list[string]``.
    declared: str = ""


class FlowPlan:
    """One compiled flow: an action schema, and the graph implementing it."""

    def __init__(
        self,
        name: str,
        description: str,
        ports: Sequence[PortPlan],
        headers: Mapping[str, dict[str, Any]],
        source_name: str = "",
    ) -> None:
        self.name = name
        self.description = description
        self.ports = list(ports)
        self.headers = dict(headers)
        self.source_name = source_name
        self.root = Body("root")
        self.node_maps: list[str] = []
        self.program: "Program | None" = None
        self._schema: ActionSchema | None = None

    @property
    def inputs(self) -> dict[str, PortPlan]:
        return {
            port.name: port for port in self.ports if port.direction == "inputs"
        }

    @property
    def outputs(self) -> dict[str, PortPlan]:
        return {
            port.name: port
            for port in self.ports
            if port.direction == "outputs"
        }

    @property
    def schema(self) -> ActionSchema:
        """The [ActionSchema][a11.actions.action.ActionSchema] this flow
        presents.

        A flow is an action: it has ports, headers and a name, so anything that
        can dispatch an action can dispatch a composition without being told it
        is one.
        """
        if self._schema is None:
            self._schema = ActionSchema.model_validate(
                {
                    "name": self.name,
                    "description": self.description,
                    "inputs": {
                        name: _port_spec(port)
                        for name, port in self.inputs.items()
                    },
                    "outputs": {
                        name: _port_spec(port)
                        for name, port in self.outputs.items()
                    },
                    "headers": {
                        name: {
                            "name": name,
                            "description": spec.get("description", ""),
                        }
                        for name, spec in self.headers.items()
                    },
                }
            )
        return self._schema

    @property
    def handler(self):
        """The action handler that runs this flow."""
        from a11.flow.runtime import make_handler

        return make_handler(self)

    def register(self, registry: Any, name: str | None = None) -> "FlowPlan":
        """Register this flow as an action in ``registry``."""
        registry.register(name or self.name, self.schema, self.handler)
        return self

    def action(self, **kwargs: Any) -> Any:
        """Build a standalone [Action][a11.actions.action.Action] for it."""
        from a11.actions.action import Action

        return Action(self.schema, handler=self.handler, **kwargs)

    async def invoke(
        self, inputs: Mapping[str, Any] | None = None, **kwargs: Any
    ) -> dict[str, Any]:
        """Run the flow once, here, and collect its outputs.

        See [a11.flow.runtime.invoke][] for what the keywords mean.
        """
        from a11.flow.runtime import invoke

        return await invoke(self, inputs, **kwargs)

    def describe(self) -> dict[str, Any]:
        """The whole composition as plain data."""
        return {
            "flow": self.name,
            "description": self.description,
            "inputs": {
                name: _describe_port(port) for name, port in self.inputs.items()
            },
            "outputs": {
                name: _describe_port(port)
                for name, port in self.outputs.items()
            },
            "headers": sorted(self.headers),
            "node_maps": list(self.node_maps),
            "steps": self.root.describe(),
        }

    def __repr__(self) -> str:
        return f"<FlowPlan {self.name} ({len(self.root.steps)} steps)>"


class Program:
    """The flows compiled from one Flow source file.

    A program is self-contained: its flows may call each other by name, and
    anything else they call is looked up in the action registry of whatever
    runtime dispatches them.
    """

    def __init__(
        self, flows: Sequence[FlowPlan], source_name: str = ""
    ) -> None:
        self.flows = {flow.name: flow for flow in flows}
        self.source_name = source_name
        for flow in flows:
            flow.program = self

    def __getitem__(self, name: str) -> FlowPlan:
        try:
            return self.flows[name]
        except KeyError:
            known = ", ".join(sorted(self.flows)) or "none"
            raise KeyError(
                f"No flow named {name!r} in "
                f"{self.source_name or 'this program'} (declared: {known})."
            ) from None

    def __contains__(self, name: str) -> bool:
        return name in self.flows

    def __iter__(self):
        return iter(self.flows.values())

    def __len__(self) -> int:
        return len(self.flows)

    @property
    def main(self) -> FlowPlan:
        """The first flow declared, which is the one a file is usually about."""
        return next(iter(self.flows.values()))

    def register_all(self, registry: Any) -> "Program":
        """Register every flow in ``registry``."""
        for flow in self.flows.values():
            flow.register(registry)
        return self

    def describe(self) -> dict[str, Any]:
        return {
            "source": self.source_name,
            "flows": [flow.describe() for flow in self.flows.values()],
        }

    def __repr__(self) -> str:
        return f"<Program {sorted(self.flows)}>"


def _port_spec(port: PortPlan) -> dict[str, Any]:
    spec: dict[str, Any] = {
        "name": port.name,
        "type": port.type,
        "unary": port.unary,
        "required": port.required,
    }
    if port.description:
        spec["description"] = port.description
    return spec


def _describe_port(port: PortPlan) -> dict[str, Any]:
    return {
        "type": port.declared
        or getattr(port.type, "__name__", port.type),
        "unary": port.unary,
        "required": port.required,
        "description": port.description,
    }


# --- Resolution --------------------------------------------------------------


@dataclass
class _Binding:
    """What a name means while resolving one flow."""

    kind: str
    value: Any = None


class _Names:
    """A lexical scope of names, chained to its parent."""

    def __init__(self, parent: "_Names | None" = None) -> None:
        self.parent = parent
        self.entries: dict[str, _Binding] = {}

    def define(self, name: str, binding: _Binding) -> None:
        self.entries[name] = binding

    def lookup(self, name: str) -> _Binding | None:
        scope: _Names | None = self
        while scope is not None:
            found = scope.entries.get(name)
            if found is not None:
                return found
            scope = scope.parent
        return None

    def known(self) -> list[str]:
        names: list[str] = []
        scope: _Names | None = self
        while scope is not None:
            names.extend(scope.entries)
            scope = scope.parent
        return sorted(set(names))


class _FlowResolver:
    """Turns one flow declaration into a [FlowPlan][a11.flow.plan.FlowPlan]."""

    def __init__(
        self,
        declaration: syntax.FlowDeclaration,
        known_flows: Mapping[str, FlowPlan],
        source_name: str,
    ) -> None:
        self.declaration = declaration
        self.known_flows = known_flows
        self.source_name = source_name
        self.labels: dict[str, int] = {}
        self.plan: FlowPlan | None = None
        #: The flow's own ports and headers, bound by `declare`.
        self.names: _Names | None = None
        self._node_map: str | None = None
        self._repeats: list[RepeatStep] = []

    def fail(self, message: str, node: syntax.Node) -> FlowSyntaxError:
        return FlowSyntaxError(
            message, node.token.line, node.token.column, self.source_name
        )

    def port_type(self, expression: syntax.TypeExpression) -> Any:
        """What a declared type gives a port.

        A built-in name gives the Python type behind it, which is what the
        JSON schema an LLM sees is derived from. A mimetype and a serialisation
        tag are carried through as they were written: the tag is the name the
        registries know the type by, and the module defining it may not even be
        imported at the time the flow is compiled.
        """
        name = expression.name
        declared = canonical(name)
        if declared in TYPE_NAMES:
            allowed = TYPE_PARAMETERS.get(declared, (0,))
            self.check_parameters(expression, allowed)
            return TYPE_NAMES[declared] or "application/json"
        if "/" in name:
            self.check_parameters(expression, (0,))
            return name
        # A dotted name is the tag a serialisation registry knows a type by.
        # An undotted one is nothing the language knows, and is far more often
        # a misspelt built-in than a tag somebody meant.
        if "." in name:
            self.check_parameters(expression, (0,))
            return name
        known = ", ".join(sorted(TYPE_NAMES))
        raise self.fail(
            f"Unknown port type {name!r} (known: {known}, a serialisation tag "
            "like 'a11.sdk.AudioBuffer', or a quoted mimetype).",
            expression,
        )

    def check_parameters(
        self, expression: syntax.TypeExpression, allowed: tuple[int, ...]
    ) -> None:
        """Check a type was given as many parameters as it takes."""
        for parameter in expression.parameters:
            self.port_type(parameter)
        if len(expression.parameters) in allowed:
            return
        counts = " or ".join(str(count) for count in allowed)
        raise self.fail(
            f"{expression.name!r} takes {counts} type parameter(s), but "
            f"{expression!s} gives {len(expression.parameters)}.",
            expression,
        )

    def label(self, base: str) -> str:
        count = self.labels.get(base, 0) + 1
        self.labels[base] = count
        return base if count == 1 else f"{base}#{count}"

    def declare(self) -> FlowPlan:
        """The flow's ports, headers and schema, without resolving its body.

        The first of two passes. A flow's schema is what a *sibling* calling it
        needs in order to have its port names checked, and a program is a set of
        flows rather than a sequence: which one is written first is the author's
        convenience, not a dependency order. So every flow declares itself
        before any of them is resolved, and [resolve][] then sees the whole
        program -- including the flow itself, which is how a recursive one is
        checked like any other call.
        """
        declaration = self.declaration
        ports: list[PortPlan] = []
        seen: set[tuple[str, str]] = set()
        for port in declaration.ports:
            if (port.direction, port.name) in seen:
                raise self.fail(
                    f"Port {port.name!r} is declared twice.", port
                )
            seen.add((port.direction, port.name))
            ports.append(
                PortPlan(
                    name=port.name,
                    direction=port.direction,
                    type=self.port_type(port.type),
                    declared=str(port.type),
                    unary=port.unary,
                    required=port.required,
                    description=port.description,
                )
            )
        headers = {
            header.name: {
                "alias": header.alias,
                "default": header.default,
                "description": header.description,
            }
            for header in declaration.headers
        }
        plan = FlowPlan(
            declaration.name,
            declaration.description,
            ports,
            headers,
            self.source_name,
        )
        self.plan = plan

        names = _Names()
        for port in ports:
            names.define(
                port.name,
                _Binding(
                    "port",
                    FlowPortRef(
                        plan.root, port.name, port.direction, port.unary
                    ),
                ),
            )
        for header in declaration.headers:
            names.define(
                header.alias,
                _Binding(
                    "ref",
                    HeaderRef(plan.root, header.name, header.default),
                ),
            )
        self.names = names
        plan.schema  # validate the schema eagerly
        return plan

    def resolve(self) -> FlowPlan:
        """Resolve the flow's body, with every sibling's schema known."""
        plan = self.plan
        names = self.names
        assert plan is not None and names is not None, "declare() comes first"
        self.resolve_body(self.declaration.body, plan.root, names)
        return plan

    # -- statements ------------------------------------------------------------

    def resolve_body(
        self, statements: Sequence[syntax.Node], body: Body, names: _Names
    ) -> None:
        for statement in statements:
            self.resolve_statement(statement, body, names)

    def resolve_statement(
        self, statement: syntax.Node, body: Body, names: _Names
    ) -> None:
        if isinstance(statement, syntax.Bind):
            if names.lookup(statement.name) is not None:
                raise self.fail(
                    f"{statement.name!r} is already taken in this scope.",
                    statement,
                )
            if isinstance(statement.value, syntax.CallExpression):
                call = self.resolve_call(
                    statement.value, body, names, statement.name
                )
                names.define(statement.name, _Binding("call", call))
                return
            if isinstance(statement.value, syntax.NodeExpression):
                names.define(
                    statement.name,
                    _Binding(
                        "node",
                        self.resolve_node(
                            statement.value, body, names, statement.name
                        ),
                    ),
                )
                return
            before = len(body.steps)
            self.resolve_statement(statement.value, body, names)
            names.define(
                statement.name, _Binding("step", body.steps[before])
            )
            return

        if isinstance(statement, syntax.CallStatement):
            self.resolve_call(
                statement.call,
                body,
                names,
                self.label(statement.call.action),
            )
            return

        if isinstance(statement, syntax.Pipe):
            source = self.resolve_pipeline(statement.pipeline, body, names)
            held = self.resolve_after(statement.after, statement, names, body)
            for target in statement.targets:
                destination = self.resolve_destination(target, body, names)
                step = body.add(PipeStep(body, source, destination))
                step.after = held
            return

        if isinstance(statement, syntax.Skip):
            source = self.resolve_pipeline(statement.pipeline, body, names)
            if statement.count is not None:
                # The count is the node's, not this statement's, so it is
                # recorded on the ref and every reader of it inherits it.
                # Only a stream that is genuinely read from somewhere has a
                # front to take values off; a loop variable or a status is
                # handed to its readers ready-made.
                if not isinstance(
                    source, (CallPortRef, LocalNodeRef, FlowPortRef)
                ) or not source.readable:
                    raise self.fail(
                        f"'skip {statement.count}' takes a port or a node, "
                        f"and {source.label} is not one. Use "
                        f"'| drop {statement.count}' to drop values from a "
                        f"pipeline instead.",
                        statement,
                    )
                source.skip += statement.count
            step = body.add(SkipStep(body, source, statement.count))
            step.after = self.resolve_after(
                statement.after, statement, names, body
            )
            return

        if isinstance(statement, syntax.Wait):
            outcome, tolerant = self.resolve_outcome(
                statement.subject, body, names
            )
            step = body.add(
                WaitStep(body, outcome, statement.timeout, tolerant)
            )
            step.after = self.resolve_after(
                statement.after, statement, names, body
            )
            return

        if isinstance(statement, syntax.Cancel):
            step = body.add(
                CancelStep(
                    body, self.expect_call(statement.name, statement, names)
                )
            )
            step.after = self.resolve_after(
                statement.after, statement, names, body
            )
            return

        if isinstance(statement, syntax.Drain):
            destination = self.resolve_destination(
                statement.target, body, names
            )
            step = body.add(DrainStep(body, StatusRef(destination), None))
            step.after = self.resolve_after(
                statement.after, statement, names, body
            )
            return

        if isinstance(statement, syntax.Fail):
            code = self.resolve_fail_code(statement.code, body, names)
            message = (
                None
                if statement.message is None
                else self.resolve_expr(statement.message, body, names)
            )
            step = body.add(FailStep(body, code, message))
            step.after = self.resolve_after(
                statement.after, statement, names, body
            )
            return

        if isinstance(statement, syntax.Nodes):
            if statement.name not in self.plan.node_maps:
                self.plan.node_maps.append(statement.name)
            # A `nodes` block is a scope for traffic, not for names: its steps
            # join the body around it, so what it calls stays nameable after it.
            names.define(statement.name, _Binding("nodemap", statement.name))
            if not statement.body:
                return
            previous = self._node_map
            self._node_map = statement.name
            try:
                self.resolve_body(statement.body, body, names)
            finally:
                self._node_map = previous
            return

        if isinstance(statement, syntax.ForEach):
            source = self.resolve_pipeline(statement.pipeline, body, names)
            step = ForEachStep(
                body, self.label("for"), source, statement.parallel
            )
            body.add(step)
            inner = _Names(names)
            inner.define(statement.variable, _Binding("ref", step.item))
            inner.define("index", _Binding("ref", step.index))
            self.resolve_body(statement.body, step.loop_body, inner)
            return

        if isinstance(statement, syntax.Repeat):
            start = None
            if statement.start is not None:
                start = self.constant(statement.start, names)
            step = RepeatStep(
                body,
                self.label("repeat"),
                start,
                statement.max_iterations,
            )
            body.add(step)
            inner = _Names(names)
            if statement.variable is not None:
                inner.define(statement.variable, _Binding("ref", step.carry))
                inner.define(
                    "__carry__", _Binding("carry-name", statement.variable)
                )
            inner.define("index", _Binding("ref", step.index))
            self._repeats.append(step)
            try:
                self.resolve_body(statement.body, step.loop_body, inner)
            finally:
                self._repeats.pop()
            return

        if isinstance(statement, syntax.Carry):
            repeats = self._repeats
            if not repeats:
                raise self.fail(
                    "'<-' carries a value into the next pass of a 'repeat', "
                    "and there is no repeat here.",
                    statement,
                )
            step = repeats[-1]
            expected = names.lookup("__carry__")
            if expected is None or expected.value != statement.name:
                raise self.fail(
                    f"This repeat carries "
                    f"{(expected.value if expected else 'nothing')!r}, "
                    f"not {statement.name!r}.",
                    statement,
                )
            if step.carry_source is not None:
                raise self.fail(
                    f"{statement.name!r} is already carried.", statement
                )
            source = self.resolve_pipeline(statement.pipeline, body, names)
            step.carry_source = source
            body.add(CaptureStep(body, source, "carry"))
            return

        if isinstance(statement, syntax.Until):
            repeats = self._repeats
            if not repeats:
                raise self.fail(
                    "'until'/'while' ends a 'repeat', and there is no repeat "
                    "here.",
                    statement,
                )
            step = repeats[-1]
            if step.condition is not None:
                raise self.fail(
                    f"{step.label} already has a stop condition.", statement
                )
            condition = self.resolve_expr(statement.condition, body, names)
            step.condition = condition
            step.stop_when = statement.stop_when
            for ref in condition.refs:
                body.add(
                    CaptureStep(body, ref, f"condition:{ref.uid}")
                )
            return

        if isinstance(statement, syntax.If):
            condition = self.resolve_expr(statement.condition, body, names)
            step = IfStep(body, self.label("if"), condition)
            body.add(step)
            self.resolve_body(
                statement.then_body, step.then_body, _Names(names)
            )
            self.resolve_body(
                statement.else_body, step.else_body, _Names(names)
            )
            return

        raise self.fail(
            f"Cannot run a {type(statement).__name__} here.", statement
        )

    def resolve_node(
        self,
        expression: syntax.NodeExpression,
        body: Body,
        names: _Names,
        name: str,
    ) -> LocalNodeRef:
        """Bind ``name`` to a node of this flow's own."""
        node_map = expression.node_map or self._node_map
        if node_map is not None:
            binding = names.lookup(node_map)
            if binding is None or binding.kind != "nodemap":
                raise self.fail(
                    f"Unknown node map {node_map!r}; declare it with "
                    f"'nodes {node_map}'.",
                    expression,
                )
            if node_map not in self.plan.node_maps:
                self.plan.node_maps.append(node_map)
        id_expr = (
            None
            if expression.id is None
            else self.resolve_expr(expression.id, body, names)
        )
        return LocalNodeRef(body, name, id_expr, node_map)

    def resolve_fail_code(
        self, expression: syntax.Node | None, body: Body, names: _Names
    ) -> "Expr | None":
        """A ``fail`` code: a canonical name, a number, or an expression."""
        if expression is None:
            return None
        if isinstance(expression, syntax.Name):
            code = status_code(expression.name)
            if code is not None:
                return Expr(
                    syntax.Literal(token=expression.token, value=int(code)), ()
                )
            binding = names.lookup(expression.name)
            if binding is None:
                raise self.fail(
                    f"Unknown status code {expression.name!r} "
                    f"(known: {', '.join(FAIL_CODES)}, either case, or a "
                    "number).",
                    expression,
                )
        return self.resolve_expr(expression, body, names)

    def resolve_outcome(
        self, expression: syntax.Node, body: Body, names: _Names
    ) -> tuple[StatusRef, bool]:
        """The status of what ``expression`` names, and whether it forgives one.

        The longest prefix of the reference that names something with a status
        wins, and anything left over reads into the record: ``status x.ok`` is
        the call's status asked whether it is ok, and ``status x.out`` is that
        port's.
        """
        path: list[str] = []
        cursor = expression
        # A trailing field of the record belongs to the record, not to a port of
        # that name: `status x.code` is what x finished with.
        while (
            isinstance(cursor, syntax.Attr)
            and cursor.name in STATUS_FIELDS
            and self.try_subject(cursor.base, names) is not None
        ):
            path.append(cursor.name)
            cursor = cursor.base
        while True:
            subject = self.try_subject(cursor, names)
            if subject is not None:
                break
            if isinstance(cursor, syntax.Attr):
                path.append(cursor.name)
                cursor = cursor.base
                continue
            if isinstance(cursor, syntax.Name):
                if names.lookup(cursor.name) is None:
                    raise self.fail(
                        f"Unknown name {cursor.name!r} "
                        f"(known: {', '.join(names.known())}).",
                        expression,
                    )
                raise self.fail(
                    f"{cursor.name!r} has no status: that belongs to a call, "
                    "a node, a port, or a barrier.",
                    expression,
                )
            raise self.fail(
                "A status belongs to a call, a node, a port, or a barrier.",
                expression,
            )
        outcome = StatusRef(subject)
        tolerant = isinstance(subject, CallStep) and subject.tolerant
        if isinstance(subject, WaitStep):
            outcome = subject.outcome
            tolerant = subject.tolerant
        for name in reversed(path):
            outcome = DerivedRef(body, outcome, Stage("at", name))  # type: ignore[assignment]
        return outcome, tolerant

    def try_subject(self, expression: syntax.Node, names: _Names) -> Any:
        """What ``expression`` names, if it is something with a status."""
        if isinstance(expression, syntax.Name):
            binding = names.lookup(expression.name)
            if binding is None:
                return None
            if binding.kind in ("call", "step", "node"):
                return binding.value
            if binding.kind == "port":
                return binding.value
            return None
        if isinstance(expression, syntax.Attr) and isinstance(
            expression.base, syntax.Name
        ):
            binding = names.lookup(expression.base.name)
            if binding is not None and binding.kind == "call":
                call = binding.value
                if expression.name == "status":
                    return call
                schema = call.target_schema
                if schema is None or expression.name in schema.outputs:
                    return call.port(expression.name, "outputs")
                if expression.name in schema.inputs:
                    return call.port(expression.name, "inputs")
        return None

    def resolve_after(
        self,
        after: Sequence[str],
        node: syntax.Node,
        names: _Names,
        body: "Body",
    ) -> tuple[Step, ...]:
        """The steps a statement must wait for, from its ``after`` names.

        A name may be a step -- a call, a bound `wait`/`drain` -- or it may be
        a **port or node**, in which case the statement waits for that stream
        to be finished. The second reading is the one an author reaches for
        without thinking: "stop the microphone once we have a sentence" is
        `after sentence`, and having to bind a `wait` to a name first was
        ceremony standing in front of an obvious meaning.
        """
        held: list[Step] = []
        for name in after:
            binding = names.lookup(name)
            if binding is not None and binding.kind in ("call", "step"):
                held.append(binding.value)
                continue
            held.append(self.barrier_for(name, node, names, body))
        return tuple(held)

    def barrier_for(
        self, name: str, node: syntax.Node, names: _Names, body: "Body"
    ) -> Step:
        """A `wait` step on the stream ``name`` refers to, made on the spot.

        This is what `after some-port` compiles to. It is the same step a
        written-out `x = wait some-port` would have made, so the two spellings
        cannot drift apart.
        """
        outcome, tolerant = self.resolve_outcome(
            syntax.Name(token=node.token, name=name), body, names
        )
        return body.add(WaitStep(body, outcome, None, tolerant))

    def step_outcome(self, step: Step, node: syntax.Node) -> StatusRef:
        """The status a named barrier waited for."""
        if isinstance(step, WaitStep):
            return step.outcome
        raise self.fail(
            f"{step.label!r} is not something with a status to read.", node
        )

    def expect_step(self, name: str, node: syntax.Node, names: _Names) -> Step:
        binding = names.lookup(name)
        if binding is None:
            raise self.fail(
                f"Unknown name {name!r} (known: {', '.join(names.known())}).",
                node,
            )
        if binding.kind not in ("call", "step"):
            raise self.fail(
                f"{name!r} is not a step to wait for.", node
            )
        return binding.value

    def expect_call(
        self, name: str, node: syntax.Node, names: _Names
    ) -> CallStep:
        binding = names.lookup(name)
        if binding is None:
            raise self.fail(
                f"Unknown name {name!r} (known: {', '.join(names.known())}).",
                node,
            )
        if binding.kind != "call":
            raise self.fail(f"{name!r} is not a call.", node)
        return binding.value

    # -- calls -----------------------------------------------------------------

    def resolve_call(
        self,
        expression: syntax.CallExpression,
        body: Body,
        names: _Names,
        name: str,
    ) -> CallStep:
        modifiers = expression.modifiers
        node_map = modifiers.node_map or self._node_map
        if node_map is not None:
            binding = names.lookup(node_map)
            if binding is None or binding.kind != "nodemap":
                raise self.fail(
                    f"Unknown node map {node_map!r}; declare it with "
                    f"'nodes {node_map} {{ ... }}'.",
                    expression,
                )
        target_schema = None
        known = self.known_flows.get(expression.action)
        if known is not None:
            target_schema = known.schema
        step = CallStep(
            body,
            name,
            expression.action,
            mode=expression.mode,
            tee=modifiers.tee,
            node_map=node_map,
            timeout=modifiers.timeout,
            tolerant=expression.tolerant,
            headers={
                header: self.resolve_expr(value, body, names)
                for header, value in modifiers.headers
            },
            forward_headers=modifiers.forward,
            action_id=(
                self.resolve_expr(modifiers.action_id, body, names)
                if modifiers.action_id is not None
                else None
            ),
            target_schema=target_schema,
        )
        body.add(step)
        step.after = self.resolve_after(
            modifiers.after, expression, names, body
        )
        for port_name, pipeline in expression.args:
            destination = self.call_port(
                step, port_name, "inputs", expression
            )
            source = self.resolve_pipeline(pipeline, body, names)
            body.add(PipeStep(body, source, destination))
        return step

    def call_port(
        self,
        call: CallStep,
        name: str,
        direction: str,
        node: syntax.Node,
    ) -> CallPortRef:
        schema = call.target_schema
        if schema is not None:
            declared = (
                schema.inputs if direction == "inputs" else schema.outputs
            )
            if name not in declared:
                known = ", ".join(sorted(declared)) or "none"
                raise self.fail(
                    f"{call.action} has no {direction[:-1]} port {name!r} "
                    f"(declared: {known}).",
                    node,
                )
        return call.port(name, direction)

    # -- pipelines and refs ----------------------------------------------------

    def resolve_pipeline(
        self, pipeline: syntax.Pipeline, body: Body, names: _Names
    ) -> Ref:
        ref = self.resolve_source(pipeline.source, body, names)
        for stage in pipeline.stages:
            argument: Any = stage.arg
            if STAGES.get(stage.name) == "stream":
                # `then` takes a stream to read after this one, so its
                # argument resolves the way a pipeline's source does.
                argument = self.resolve_source(argument, body, names)
            elif isinstance(argument, syntax.Node):
                argument = self.resolve_expr(
                    argument, body, names, allow_it=True
                )
            ref = DerivedRef(body, ref, Stage(stage.name, argument))
        return ref

    def resolve_source(
        self, expression: syntax.Node, body: Body, names: _Names
    ) -> Ref:
        """A pipeline source: a stream, a path over a stream, or one value."""
        stream = self.as_stream(expression, body, names)
        if stream is not None:
            return stream
        return ExprRef(body, self.resolve_expr(expression, body, names))

    def as_stream(
        self, expression: syntax.Node, body: Body, names: _Names
    ) -> Ref | None:
        """Resolve a name or path rooted at one, as a stream; else ``None``."""
        if isinstance(expression, syntax.PipelineValue):
            return self.resolve_pipeline(expression.pipeline, body, names)
        if isinstance(expression, syntax.Outcome):
            return self.resolve_outcome(expression.subject, body, names)[0]
        if isinstance(expression, syntax.Name):
            binding = names.lookup(expression.name)
            if binding is None:
                raise self.fail(
                    f"Unknown name {expression.name!r} "
                    f"(known: {', '.join(names.known())}).",
                    expression,
                )
            if binding.kind == "call":
                raise self.fail(
                    f"{expression.name!r} is a call; name one of its ports, "
                    f"like {expression.name}.output.",
                    expression,
                )
            if binding.kind == "nodemap":
                raise self.fail(
                    f"{expression.name!r} is a node map, not a stream.",
                    expression,
                )
            if binding.kind == "step":
                # A named barrier reads as the status it waited for.
                return self.step_outcome(binding.value, expression)
            ref = binding.value
            if not ref.readable:
                raise self.fail(
                    f"{expression.name!r} is written by this flow, not read.",
                    expression,
                )
            return ref
        if isinstance(expression, syntax.Attr):
            named = self.attr_stream(expression, body, names)
            if named is not None:
                return named
            inner = self.as_stream(expression.base, body, names)
            if inner is None:
                return None
            return DerivedRef(
                body, inner, Stage("at", expression.name)
            )
        if isinstance(expression, syntax.Index):
            inner = self.as_stream(expression.base, body, names)
            if inner is None:
                return None
            index = self.constant(expression.index, names)
            return DerivedRef(body, inner, Stage("at", index))
        return None

    def attr_stream(
        self, expression: syntax.Attr, body: Body, names: _Names
    ) -> Ref | None:
        """``x.y`` where ``x`` is a call, a node, or a barrier -- not a value.

        These are streams in their own right rather than a field of whatever the
        base carries, and both a pipeline source and an expression have to agree
        about that: ``seen.id`` is the node's id, never the ``id`` field of the
        first thing written to it.
        """
        base = expression.base
        if not isinstance(base, syntax.Name):
            return None
        binding = names.lookup(base.name)
        if binding is None:
            return None
        if binding.kind == "call":
            call = binding.value
            if expression.name == "status":
                return call.status()
            return self.call_port(call, expression.name, "outputs", expression)
        if binding.kind == "node" and expression.name == "id":
            return NodeIdRef(binding.value)
        if binding.kind == "step":
            return DerivedRef(
                body,
                self.step_outcome(binding.value, expression),
                Stage("at", expression.name),
            )
        return None

    def resolve_destination(
        self, expression: syntax.Node, body: Body, names: _Names
    ) -> Ref:
        if isinstance(expression, syntax.Name):
            binding = names.lookup(expression.name)
            if binding is None:
                raise self.fail(
                    f"Unknown destination {expression.name!r} "
                    f"(known: {', '.join(names.known())}).",
                    expression,
                )
            if binding.kind == "call":
                raise self.fail(
                    f"{expression.name!r} is a call; name the port to write, "
                    f"like {expression.name}.input.",
                    expression,
                )
            if binding.kind == "step":
                raise self.fail(
                    f"{expression.name!r} is a barrier, not somewhere to "
                    "write.",
                    expression,
                )
            ref = binding.value
            if not isinstance(ref, Ref) or not ref.writable:
                raise self.fail(
                    f"{expression.name!r} cannot be written by this flow "
                    "(an 'in' port and a call's output are read, not "
                    "written).",
                    expression,
                )
            return ref
        if isinstance(expression, syntax.Attr) and isinstance(
            expression.base, syntax.Name
        ):
            binding = names.lookup(expression.base.name)
            if binding is None:
                raise self.fail(
                    f"Unknown destination {expression.base.name!r}.",
                    expression,
                )
            if binding.kind != "call":
                raise self.fail(
                    f"{expression.base.name!r} is not a call, so "
                    f"{expression.name!r} is not a port to write.",
                    expression,
                )
            return self.call_port(
                binding.value, expression.name, "inputs", expression
            )
        raise self.fail(
            "A destination is an 'out' port or a call's input port.",
            expression,
        )

    # -- expressions -----------------------------------------------------------

    def resolve_expr(
        self,
        expression: syntax.Node,
        body: Body,
        names: _Names,
        allow_it: bool = False,
    ) -> Expr:
        refs: list[Ref] = []
        node = self.rewrite(expression, body, names, refs, allow_it)
        return Expr(node, refs)

    def rewrite(
        self,
        expression: syntax.Node,
        body: Body,
        names: _Names,
        refs: list[Ref],
        allow_it: bool,
    ) -> syntax.Node:
        """Bind names inside an expression, collecting the refs it reads."""

        def remember(ref: Ref) -> syntax.Node:
            if all(existing is not ref for existing in refs):
                refs.append(ref)
            return RefValue(token=expression.token, ref=ref)

        if isinstance(expression, syntax.It):
            if not allow_it:
                raise self.fail(
                    "'it' names the value a 'where' or 'map' stage is looking "
                    "at, and there is none here.",
                    expression,
                )
            return expression
        if isinstance(expression, syntax.Literal):
            return expression
        if isinstance(expression, syntax.ListLiteral):
            return syntax.ListLiteral(
                token=expression.token,
                items=[
                    self.rewrite(item, body, names, refs, allow_it)
                    for item in expression.items
                ],
            )
        if isinstance(expression, syntax.ObjectLiteral):
            return syntax.ObjectLiteral(
                token=expression.token,
                pairs=[
                    (key, self.rewrite(value, body, names, refs, allow_it))
                    for key, value in expression.pairs
                ],
            )
        if isinstance(expression, syntax.Builtin):
            return syntax.Builtin(
                token=expression.token,
                name=expression.name,
                args=[
                    self.rewrite(argument, body, names, refs, allow_it)
                    for argument in expression.args
                ],
            )
        if isinstance(expression, syntax.TypedValue):
            # The type is checked as far as it can be here -- a built-in name
            # is either known or misspelt -- and a tag is left to the runtime,
            # which is the only place that knows what has been registered.
            if not _is_tag(expression.type):
                self.port_type(expression.type)
            return syntax.TypedValue(
                token=expression.token,
                type=expression.type,
                value=self.rewrite(
                    expression.value, body, names, refs, allow_it
                ),
            )
        if isinstance(expression, syntax.Unary):
            return syntax.Unary(
                token=expression.token,
                op=expression.op,
                operand=self.rewrite(
                    expression.operand, body, names, refs, allow_it
                ),
            )
        if isinstance(expression, syntax.Binary):
            return syntax.Binary(
                token=expression.token,
                op=expression.op,
                left=self.rewrite(
                    expression.left, body, names, refs, allow_it
                ),
                right=self.rewrite(
                    expression.right, body, names, refs, allow_it
                ),
            )
        if isinstance(expression, syntax.PipelineValue):
            return remember(
                self.resolve_pipeline(expression.pipeline, body, names)
            )
        if isinstance(expression, syntax.Name):
            return remember(self.as_stream(expression, body, names))
        if isinstance(expression, syntax.Attr):
            named = self.attr_stream(expression, body, names)
            if named is not None:
                return remember(named)
            return syntax.Attr(
                token=expression.token,
                base=self.rewrite(
                    expression.base, body, names, refs, allow_it
                ),
                name=expression.name,
            )
        if isinstance(expression, syntax.Index):
            return syntax.Index(
                token=expression.token,
                base=self.rewrite(
                    expression.base, body, names, refs, allow_it
                ),
                index=self.rewrite(
                    expression.index, body, names, refs, allow_it
                ),
            )
        raise self.fail(
            f"Cannot use a {type(expression).__name__} as a value.", expression
        )

    def constant(self, expression: syntax.Node, names: _Names) -> Any:
        del names
        from a11.flow.parser import constant_value

        found, value = constant_value(expression)
        if not found:
            raise self.fail(
                "Expected a constant value here (a literal, list or object).",
                expression,
            )
        return value


def _is_tag(type_expression: syntax.TypeExpression) -> bool:
    """Whether a type names something only the runtime can resolve."""
    return "." in type_expression.name or "/" in type_expression.name


def unparse(node: syntax.Node) -> str:
    """Render a resolved expression back into something close to its source."""
    if isinstance(node, RefValue):
        return node.ref.label
    if isinstance(node, syntax.It):
        return "it"
    if isinstance(node, syntax.Literal):
        return repr(node.value)
    if isinstance(node, syntax.ListLiteral):
        return "[" + ", ".join(unparse(item) for item in node.items) + "]"
    if isinstance(node, syntax.ObjectLiteral):
        inner = ", ".join(
            f"{key!r}: {unparse(value)}" for key, value in node.pairs
        )
        return "{" + inner + "}"
    if isinstance(node, syntax.Builtin):
        return (
            f"{node.name}("
            + ", ".join(unparse(argument) for argument in node.args)
            + ")"
        )
    if isinstance(node, syntax.TypedValue):
        return f"{unparse(node.value)} as {node.type}"
    if isinstance(node, syntax.Unary):
        return f"{node.op} {unparse(node.operand)}"
    if isinstance(node, syntax.Binary):
        return f"({unparse(node.left)} {node.op} {unparse(node.right)})"
    if isinstance(node, syntax.Attr):
        return f"{unparse(node.base)}.{node.name}"
    if isinstance(node, syntax.Index):
        return f"{unparse(node.base)}[{unparse(node.index)}]"
    if isinstance(node, syntax.Name):
        return node.name
    return type(node).__name__


def compile_source(source: str, source_name: str = "") -> Program:
    """Compile Flow source into a [Program][a11.flow.plan.Program].

    Raises:
        FlowSyntaxError: On any lexical, grammatical or naming problem, with the
            line and column it was found at.
    """
    declarations = parse(source, source_name)
    flows: dict[str, FlowPlan] = {}
    resolvers: list[_FlowResolver] = []
    # Two passes over the same dict of flows: every declaration first, so a
    # flow calling one written later in the file -- or calling itself -- has
    # its ports checked against the real thing rather than going unchecked
    # until it runs.
    for declaration in declarations:
        if declaration.name in flows:
            raise FlowSyntaxError(
                f"Flow {declaration.name!r} is declared twice.",
                declaration.token.line,
                declaration.token.column,
                source_name,
            )
        resolver = _FlowResolver(declaration, flows, source_name)
        flows[declaration.name] = resolver.declare()
        resolvers.append(resolver)
    for resolver in resolvers:
        resolver.resolve()
    return Program(list(flows.values()), source_name)


__all__ = [
    "BoundRef",
    "Body",
    "CallPortRef",
    "CallStep",
    "CancelStep",
    "CaptureStep",
    "DerivedRef",
    "DrainStep",
    "Expr",
    "ExprRef",
    "FAIL_CODES",
    "LocalNodeRef",
    "NodeIdRef",
    "STATUS_CODES",
    "STATUS_FIELDS",
    "FailStep",
    "FlowPlan",
    "FlowPortRef",
    "ForEachStep",
    "HeaderRef",
    "IfStep",
    "PipeStep",
    "PortPlan",
    "Program",
    "Ref",
    "RefValue",
    "RepeatStep",
    "SkipStep",
    "Stage",
    "StatusRef",
    "Step",
    "TYPE_NAMES",
    "WaitStep",
    "compile_source",
    "status_code",
    "unparse",
]

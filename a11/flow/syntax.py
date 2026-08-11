"""The Flow language syntax tree.

One dataclass per construct, each carrying the token it started at so the
resolver can point at the offending line. Nothing here knows about A11: these
are the shapes the parser produces, and [a11.flow.plan][] turns them into a
runnable plan.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from a11 import timing
from a11.flow.lexer import Token


@dataclass
class Node:
    """Base of every syntax node; ``token`` is where it started."""

    token: Token


# --- Expressions -------------------------------------------------------------


@dataclass
class Literal(Node):
    """A number, string, boolean, null, list, or object literal."""

    value: Any


@dataclass
class ListLiteral(Node):
    """``[a, b, c]``."""

    items: list["Node"]


@dataclass
class ObjectLiteral(Node):
    """``{ "key": expr, ... }``."""

    pairs: list[tuple[str, "Node"]]


@dataclass
class It(Node):
    """``it`` -- the value a ``where``/``map`` stage is looking at."""


@dataclass
class Name(Node):
    """A bare name: a port, a call, a loop variable, a header alias."""

    name: str


@dataclass
class Attr(Node):
    """``base.name`` -- a call's port, or a key/attribute of a value."""

    base: Node
    name: str


@dataclass
class Index(Node):
    """``base[i]`` -- an element of a list or a key of an object."""

    base: Node
    index: Node


@dataclass
class Builtin(Node):
    """``name(arg, ...)`` -- one of the language's fixed functions."""

    name: str
    args: list[Node]


@dataclass
class TypedValue(Node):
    """``Tag{...}`` or ``expr as Tag`` -- a value made into a type's value.

    Both spellings mean the same thing: take what the expression produces --
    which may be partial, the way a literal written by hand usually is -- and
    make it the type named, filling in whatever that type defaults. It is how a
    flow builds a value for a port that wants a real type rather than a bag of
    keys.
    """

    type: "TypeExpression"
    value: Node


@dataclass
class Unary(Node):
    """``not operand``."""

    op: str
    operand: Node


@dataclass
class Binary(Node):
    """``left op right`` for ``and or == != < <= > >= in``."""

    op: str
    left: Node
    right: Node


# --- Pipelines ---------------------------------------------------------------


@dataclass
class Stage(Node):
    """One ``| name arg`` stage of a pipeline."""

    name: str
    arg: Any = None


@dataclass
class Pipeline(Node):
    """A source expression and the stages its values pass through."""

    source: Node
    stages: list[Stage] = field(default_factory=list)


@dataclass
class Outcome(Node):
    """``status subject`` -- the status of a call, a node, or a barrier.

    Reading one is a synchronisation point: the subject has to be finished
    before there is a status to report.
    """

    subject: Node


@dataclass
class PipelineValue(Node):
    """``(stream | stage ...)`` used where a value is expected.

    The pipeline's first value is read once and used as the value, which is what
    lets a condition ask about a stream: ``if (hits | count) > 0``.
    """

    pipeline: Pipeline


# --- Calls -------------------------------------------------------------------


@dataclass
class CallModifiers(Node):
    """The ``tee``/``via``/``timeout``/``after``/``with``/``forward`` tail."""

    tee: bool = False
    node_map: str | None = None
    timeout: timing.Duration | None = None
    after: list[str] = field(default_factory=list)
    headers: list[tuple[str, Node]] = field(default_factory=list)
    action_id: Node | None = None
    #: Header names, or ``*`` patterns, that ``forward headers`` names.
    forward: list[str] = field(default_factory=list)


@dataclass
class CallExpression(Node):
    """``run``/``call action(port: pipeline, ...)`` and its modifiers.

    ``mode`` is the verb that was written: ``"run"`` for the handler registered
    here, ``"call"`` for the stream this flow is attached to.
    """

    action: str
    mode: str
    args: list[tuple[str, Pipeline]]
    modifiers: CallModifiers
    tolerant: bool = False


# --- Statements --------------------------------------------------------------


@dataclass
class Bind(Node):
    """``name = ...`` -- a step the rest of the flow can refer to.

    A call, so its ports can be named; or a `wait`/`drain` barrier, so another
    statement can be held until it happens.
    """

    name: str
    value: Node


@dataclass
class CallStatement(Node):
    """A call whose outputs nobody names (they are drained for it)."""

    call: CallExpression


@dataclass
class Pipe(Node):
    """``pipeline -> target, target`` -- write a stream into one or more
    nodes."""

    pipeline: Pipeline
    targets: list[Node]
    after: list[str] = field(default_factory=list)


@dataclass
class Skip(Node):
    """``skip pipeline`` -- read a stream to its end and discard the values.

    With a ``count`` it is ``skip n reference`` instead, which discards the
    first ``n`` values of that one node for every reader of it rather than
    reading the whole thing.
    """

    pipeline: Pipeline
    after: list[str] = field(default_factory=list)
    count: int | None = None


@dataclass
class Wait(Node):
    """``wait subject`` -- hold until a call, or a node this flow writes, is
    finished."""

    subject: Node
    timeout: timing.Duration | None = None
    after: list[str] = field(default_factory=list)


@dataclass
class Drain(Node):
    """``drain target`` -- hold until a node's writers are done and its buffer
    has landed."""

    target: Node
    after: list[str] = field(default_factory=list)


@dataclass
class Cancel(Node):
    """``cancel name`` -- ask a called action to stop, cooperatively."""

    name: str
    after: list[str] = field(default_factory=list)


@dataclass
class Fail(Node):
    """``fail [code] [message]`` -- end the flow with a status.

    Either operand may be an expression, so a status grabbed from a recoverable
    failure can be re-raised as it stands, or translated into another one.
    """

    code: Node | None = None
    message: Node | None = None
    after: list[str] = field(default_factory=list)


@dataclass
class ForEach(Node):
    """``for name in pipeline [parallel n] { ... }``."""

    variable: str
    pipeline: Pipeline
    parallel: int
    body: list[Node]


@dataclass
class Repeat(Node):
    """``repeat [name = expr] [max n] { ... }``."""

    variable: str | None
    start: Node | None
    max_iterations: int
    body: list[Node]


@dataclass
class Carry(Node):
    """``name <- pipeline`` -- what the next pass of a ``repeat`` carries."""

    name: str
    pipeline: Pipeline


@dataclass
class Until(Node):
    """``until expr`` / ``while expr`` -- when a ``repeat`` stops."""

    condition: Node
    stop_when: bool


@dataclass
class If(Node):
    """``if expr { ... } else { ... }``."""

    condition: Node
    then_body: list[Node]
    else_body: list[Node]


@dataclass
class Nodes(Node):
    """``nodes name [{ ... }]`` -- declare a temporary node map.

    With a block, every call inside it is placed in that map. Without one, the
    map is simply declared, for `Bind` of a node or a call's ``via`` to name.
    """

    name: str
    body: list[Node] = field(default_factory=list)


@dataclass
class NodeExpression(Node):
    """``node [id] [in map]`` -- a node of this flow's own.

    With an ``id`` expression the flow attaches to the node that names, which is
    how it writes to a node its caller asked it to use. Without one it makes a
    fresh node. Either way the node lands in the contextually active node map
    unless ``in`` names another.
    """

    id: Node | None = None
    node_map: str | None = None


# --- Declarations ------------------------------------------------------------


@dataclass
class TypeExpression(Node):
    """The type of a port: ``string``, ``list[a11.NodeFragment]``, a tag.

    ``name`` is what was written before the brackets -- a built-in type name, a
    dotted serialisation tag, or a mimetype when ``quoted`` -- and
    ``parameters`` are the types a generic one was given.
    """

    name: str
    parameters: list["TypeExpression"] = field(default_factory=list)
    quoted: bool = False

    def __str__(self) -> str:
        if not self.parameters:
            return self.name
        inside = ", ".join(str(parameter) for parameter in self.parameters)
        return f"{self.name}[{inside}]"


@dataclass
class PortDeclaration(Node):
    """``in``/``out name: type [stream] [required]``."""

    name: str
    direction: str
    type: TypeExpression
    unary: bool = False
    required: bool = False
    description: str = ""


@dataclass
class HeaderDeclaration(Node):
    """``header "x-name" [as alias] [= default]``."""

    name: str
    alias: str
    default: Any = None
    description: str = ""


@dataclass
class FlowDeclaration(Node):
    """One ``flow name { ... }`` declaration."""

    name: str
    description: str = ""
    ports: list[PortDeclaration] = field(default_factory=list)
    headers: list[HeaderDeclaration] = field(default_factory=list)
    body: list[Node] = field(default_factory=list)


__all__ = [
    "Attr",
    "Bind",
    "Binary",
    "Builtin",
    "CallExpression",
    "CallModifiers",
    "CallStatement",
    "Cancel",
    "Carry",
    "Drain",
    "Fail",
    "FlowDeclaration",
    "ForEach",
    "HeaderDeclaration",
    "If",
    "Index",
    "It",
    "ListLiteral",
    "Literal",
    "Name",
    "Node",
    "NodeExpression",
    "Nodes",
    "Outcome",
    "ObjectLiteral",
    "Pipe",
    "Pipeline",
    "PortDeclaration",
    "Repeat",
    "Skip",
    "Stage",
    "TypeExpression",
    "TypedValue",
    "Unary",
    "Until",
    "Wait",
]

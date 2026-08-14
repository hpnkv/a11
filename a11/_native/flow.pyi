"""
The Flow language: one lexer, one grammar, one set of checks, shared by the Python API, the `a11 flow` command and every editor.
"""

from __future__ import annotations
import collections.abc
import typing
from a11._native import (
    Action,
    ActionHandler,
    ActionHeaderSchema,
    ActionPortSchema,
    ActionRegistry,
    ActionSchema,
    NativeActionHandler,
    WireStream,
)

__all__: list[str] = [
    "FlowPlan",
    "Program",
    "check",
    "codes",
    "compile",
    "complete",
    "format",
    "highlight",
    "parse",
    "plan",
    "request",
    "stages",
    "strformat",
    "syntax",
    "tokenize",
    "vocabulary",
]

class FlowPlan:
    """
    One compiled flow: an action schema, and the graph implementing it.

    A handle onto the program it came from, which keeps the program -- and the syntax
    tree its graph borrows -- alive for as long as anything holds the flow. So a
    handler taken from one still runs after the program variable has gone.
    """

    def __repr__(self) -> str: ...
    def action(self, **kwargs: typing.Any) -> Action:
        """
        Build a standalone [Action][a11.actions.action.Action] for it.
        """

    def describe(self) -> dict[str, typing.Any]:
        """
        The whole composition as plain data.

        The `flow.plan/v1` entry for this flow: its ports, headers, node maps and steps,
        nested bodies and all.
        """

    async def invoke(
        self,
        inputs: collections.abc.Mapping[str, typing.Any] | None = None,
        **kwargs: typing.Any,
    ) -> dict[str, typing.Any]:
        """
        Run the flow once, here, and collect its outputs.

        See [a11.flow.runtime.invoke][] for what the keywords mean.
        """

    def make_handler(
        self, dispatch_stream: WireStream | None = None
    ) -> ActionHandler | NativeActionHandler | None:
        """
        The action handler that runs this flow.

        ``dispatch_stream`` is only for a flow a *client* runs over a session it already
        holds: the calls that belong to the peer are bound to that stream, and the flow's
        own action is not. An action that is run locally *and* holds a stream ends that
        stream when it finishes, after which the session can dispatch nothing.
        """

    def register(
        self, registry: ActionRegistry, name: str | None = None
    ) -> FlowPlan:
        """
        Register this flow as an action in ``registry``.

        After this the composition is an action like any other: a session
        dispatches it, another flow calls it, and a model can be offered it as a
        tool, without any of them knowing it is a composition.
        """

    @property
    def description(self) -> str: ...
    @property
    def handler(self) -> ActionHandler | NativeActionHandler | None:
        """
        The action handler that runs this flow.
        """

    @property
    def headers(self) -> collections.abc.Mapping[str, ActionHeaderSchema]:
        """
        The declared headers, by name.
        """

    @property
    def inputs(self) -> collections.abc.Mapping[str, ActionPortSchema]:
        """
        The declared input ports, by name, as the action schema has them.
        """

    @property
    def name(self) -> str: ...
    @property
    def node_maps(self) -> list[str]: ...
    @property
    def outputs(self) -> collections.abc.Mapping[str, ActionPortSchema]:
        """
        The declared output ports, by name, as the action schema has them.
        """

    @property
    def schema(self) -> ActionSchema:
        """
        The [ActionSchema][a11.actions.action.ActionSchema] a flow presents.

        A flow is an action: it has ports, headers and a name, so anything that
        can dispatch an action can dispatch a composition without being told it
        is one.

        Built through the Python validator rather than taken from the native
        schema, because a port's ``typeinfo`` -- the Python type its JSON schema
        comes from, and so what a model is shown -- is something only this side
        can supply.
        """

    @property
    def source_name(self) -> str: ...

class Program:
    """
    The flows compiled from one Flow source file.

    A program is self-contained: its flows may call each other by name, and anything
    else they call is looked up in the action registry of whatever runtime dispatches
    them.
    """

    def __contains__(self, name: object) -> bool: ...
    def __getitem__(self, name: str) -> FlowPlan: ...
    def __iter__(self) -> typing.Iterator[FlowPlan]: ...
    def __len__(self) -> int: ...
    def __repr__(self) -> str: ...
    def describe(self) -> dict[str, typing.Any]: ...
    def get(self, name: str) -> FlowPlan | None:
        """
        The flow of this name, or ``None``.
        """

    def register_all(self, registry: ActionRegistry) -> Program:
        """
        Register every flow in ``registry``.
        """

    @property
    def flows(self) -> dict[str, FlowPlan]:
        """
        Every flow, by name, in declaration order.
        """

    @property
    def main(self) -> FlowPlan:
        """
        The first flow declared, which is the one a file is usually about.
        """

    @property
    def names(self) -> list[str]:
        """
        Every flow's name, in the order the file declares them.
        """

    @property
    def source_name(self) -> str: ...

def check(source: str, source_name: str = "-") -> dict[str, typing.Any]:
    """
    Everything wrong with one flow file.

    Returns a ``flow.diagnostics/v1`` payload: the syntax and form problems the parser
    found, and the name, sequence and barrier problems the resolver found, in source
    order. Every problem in the file, not the first -- both passes recover.

    This is the whole of what ``a11 flow check`` and an editor need. ``flow.compile``
    is the same engine with a strict door on it, for actually running one.
    """

def codes() -> list[dict[str, typing.Any]]:
    """
    Every diagnostic code the language publishes, with its meaning.

    The same table ``testdata/flow/codes.json`` is generated from, so a toolchain may
    read either and get the same answer.
    """

def compile(source: str, source_name: str = "") -> Program:
    """
    Compile Flow source into a runnable program.

    The strict door onto the engine every other function here reads through: the
    parser and the resolver both recover and report everything, and this refuses on
    the first error with the line, the column and the message. ``a11.flow.loads``
    turns that into ``FlowSyntaxError``.
    """

def complete(source: str, offset: typing.SupportsInt) -> dict[str, typing.Any]:
    """
    What may be written at ``offset``.

    Returns a ``flow.completions/v1`` payload: the proposals in the order they should
    be offered, the partial word at the caret, and where that word starts. After a
    ``|`` only a stage; past a port's ``:`` only a type; after ``x.`` only what ``x``
    has. Unfiltered on purpose -- every frontend filters by its own rules, and
    filtering twice drops what a fuzzy matcher would have kept.
    """

def format(source: str) -> dict[str, typing.Any]:
    """
    Format Flow source.

    Returns a ``flow.format/v1`` payload: the formatted text, whether it differs, one
    edit that turns the input into it, and any problems found on the way.

    It decides indentation, the spaces between tokens, blank lines and the columns of a
    run of declarations. It does *not* decide where the lines break: that is a judgement
    about what belongs together, and it stays the author's. A file with an error in it is
    returned exactly as it was, with the diagnostics saying why.
    """

def highlight(source: str, source_name: str = "-") -> dict[str, typing.Any]:
    """
    Classify Flow source for colouring.

    Returns a ``flow.tokens/v1`` payload: one entry per token with the *meaning* of
    the word at that position -- a stage after a ``|``, a type past a port's ``:``, a
    member after a ``.``, a function only where it is called. This is the one
    implementation of that judgement; an editor maps its names to a palette.
    """

def parse(source: str, source_name: str = "-") -> dict[str, typing.Any]:
    """
    Parse Flow source into its syntax tree.

    Returns a ``flow.syntax/v1`` payload: the flows the file declares, and every
    problem found in it. Both, always -- parsing never fails. The parser recovers
    where the Python reference raises: a statement it cannot read costs its own line,
    stands in as an ``error`` node, and the rest of the file is parsed and reported on.

    ``flow.loads`` is the strict door onto the same engine: it raises
    ``FlowSyntaxError`` built from the first ``error`` diagnostic here, with the line,
    column and message the Python compiler has always reported.
    """

def plan(source: str, source_name: str = "-") -> dict[str, typing.Any]:
    """
    What each flow of a file resolved to.

    Returns a ``flow.plan/v1`` payload -- the ports, headers, node maps and steps of
    every flow in the file, nested bodies and all -- and the diagnostics, because a
    plan of a file with an error in it is a partial plan and a reader shown it as the
    whole truth would be misled.
    """

def request(request: dict[str, typing.Any]) -> dict[str, typing.Any]:
    """
    One request to the language service, answered.

    The same method dispatch the standalone ``a11-flow serve`` speaks, reachable
    without spawning it: ``{"method": "check", "source": "..."}`` gives
    ``{"ok": true, "result": {...}}``. Every method is available through a named
    function here as well; this is for a frontend that is *relaying* -- a server, a
    plugin, a test of the protocol -- and would otherwise have to keep its own table
    of which method means which call.
    """

def stages() -> dict[str, str]:
    """
    Every pipeline stage, and what each one takes after its name.

    ``"none"``, ``"number"``, ``"expr"``, ``"string"``, ``"string?"`` or ``"stream"``
    -- the same table the parser reads, so an editor offering completions after a
    ``|`` needs no list of its own.
    """

def strformat(
    format: str, arguments: collections.abc.Iterable[typing.Any]
) -> str:
    """
    ``format`` with each ``%`` conversion replaced by one of ``arguments``.

    printf's syntax, and *only* a format string: no attribute access, no indexing,
    nothing a template can reach through. A flow's templates can come from a model,
    so that matters more here than a richer template language would.
    """

def syntax(target: str = "sublime") -> dict[str, str]:
    """
    An editor definition, generated from the language's own tables.

    Returns where the file belongs and what should be in it. A static grammar file is a
    copy of the word lists, and a copy falls behind; generating it means a word added to
    the language reaches the editor by running this, and CI notices when nobody has.

    ``target`` is ``"sublime"`` for the Sublime/TextMate-family grammar or
    ``"pygments"`` for the lexer that colours a fenced flow in A11's documentation.
    """

def tokenize(source: str, keep_comments: bool = True) -> dict[str, typing.Any]:
    """
    Tokenize Flow source.

    Returns a dict of ``tokens`` and ``diagnostics``. Lexing never fails: an
    unterminated string ends at its line, an unknown character is one ``bad`` token,
    and what follows is still read, because an editor is looking at a file somebody is
    in the middle of typing.

    With ``keep_comments`` a comment is a token, which is what a highlighter and a
    formatter need; without it the stream is what the parser reads.
    """

def vocabulary() -> dict[str, typing.Any]:
    """
    Every word set the language gives meaning to.

    The one table, as ``flow.vocabulary/v1``. Anything generating a static grammar file
    reads this rather than restating it, and ``a11 flow syntax`` holds an editor
    definition that still keeps a copy to it.
    """

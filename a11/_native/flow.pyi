# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
The Flow language: one lexer, one grammar, one set of checks, shared by the Python API, the `a11 flow` command and every editor.
"""

from __future__ import annotations
import a11._native
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
    "CallOptions",
    "DocumentChange",
    "DocumentSymbolHandle",
    "Expression",
    "FlowBuilder",
    "FlowPlan",
    "ModuleDocument",
    "ModuleGraphChange",
    "Program",
    "SourceEdit",
    "Statement",
    "StructBuilder",
    "Type",
    "check",
    "check_program",
    "codes",
    "compile",
    "complete",
    "format",
    "highlight",
    "parse",
    "plan",
    "register_standard_actions",
    "request",
    "run_program",
    "scan",
    "stages",
    "standard_action_schemas",
    "strformat",
    "syntax",
    "syntax_targets",
    "tokenize",
    "vocabulary",
]

class CallOptions:
    """
    Typed modifiers for an invocation expression.
    """

    id: Expression | None
    node_map: str
    remote: bool
    tee: bool
    tolerant: bool
    def __init__(self) -> None:
        """
        Create default local invocation options.
        """

    @property
    def after(self) -> list[Expression]: ...
    @after.setter
    def after(self, arg0: collections.abc.Sequence[Expression]) -> None: ...

    @property
    def forward(self) -> list[str]: ...
    @forward.setter
    def forward(self, arg0: collections.abc.Sequence[str]) -> None: ...

    @property
    def headers(self) -> dict[str, Expression]:
        """
        Explicit header expressions in evaluation order.
        """
    @headers.setter
    def headers(self, arg1: dict[str, Expression]) -> None: ...

    @property
    def timeout(self) -> typing.Any:
        """
        An optional A11 duration or duration string.
        """
    @timeout.setter
    def timeout(self, arg1: typing.Any) -> None: ...

class DocumentChange:
    """
    The edits and diagnostics returned by an atomic document transaction.
    """

    @property
    def diagnostics(self) -> typing.Any:
        """
        Diagnostics after the transaction.
        """

    @property
    def edits(self) -> list[SourceEdit]: ...
    @property
    def revision(self) -> int: ...

class DocumentSymbolHandle:
    """
    A symbol in one document revision; edits reject stale handles.
    """

    @property
    def document(self) -> str: ...
    @property
    def kind(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def occurrence(self) -> int: ...
    @property
    def revision(self) -> int: ...

class Expression:
    """
    A native Flow expression with grammar-aware source emission.
    """

    @staticmethod
    def array(items: collections.abc.Sequence[Expression]) -> Expression:
        """
        Construct an array of expressions.
        """

    @staticmethod
    def builtin(
        name: str, arguments: collections.abc.Sequence[Expression]
    ) -> Expression:
        """
        Apply a language builtin.
        """

    @staticmethod
    def completion(operation: str, subject: Expression) -> Expression:
        """
        Read status, wait for, or drain a subject.
        """

    @staticmethod
    def invoke(
        action: str,
        arguments: collections.abc.Mapping[str, Expression] = {},
        remote: bool = False,
        tolerant: bool = False,
        node_map: str = "",
    ) -> Expression:
        """
        Construct an action invocation, grouped for result access.
        """

    @staticmethod
    def invoke_with(
        action: str,
        arguments: collections.abc.Mapping[str, Expression],
        options: CallOptions,
    ) -> Expression:
        """
        Construct an invocation with typed modifiers.
        """

    @staticmethod
    def literal(value: typing.Any) -> Expression:
        """
        Construct a Flow literal from a JSON value.
        """

    @staticmethod
    def node(id: Expression | None = None, node_map: str = "") -> Expression:
        """
        Allocate or attach a node.
        """

    @staticmethod
    def object(fields: dict[str, Expression]) -> Expression:
        """
        Construct an object, retaining field evaluation order.
        """

    @staticmethod
    def reference(name: str) -> Expression:
        """
        Refer to a name or a qualified field.
        """

    def cast(self, type: Type) -> Expression:
        """
        Coerce an expression to a type.
        """

    def field(self, name: str) -> Expression:
        """
        Select a port or field.
        """

    def index(self, index: Expression) -> Expression:
        """
        Select an indexed item.
        """

    def pipeline(
        self, stage: str, argument: Expression | None = None
    ) -> Expression:
        """
        Apply a pipeline stage with required grouping.
        """

    @property
    def source(self) -> str:
        """
        The emitted Flow expression.
        """

class FlowBuilder:
    """
    Construct an implemented flow or an interface declaration.
    """

    def __init__(self, name: str, interface_only: bool = False) -> None:
        """
        Create a flow builder.
        """

    def add(self, statement: Statement) -> None:
        """
        Append a typed executable statement.
        """

    def add_declaration(self, interface: FlowBuilder) -> None:
        """
        Add a flow-local callable declaration.
        """

    def add_import(self, reference: str, alias: str, symbol: str = "") -> None:
        """
        Import a namespace or one named symbol into this flow.
        """

    def add_struct(self, shape: StructBuilder) -> None:
        """
        Add a flow-local struct.
        """

    def bind(self, name: str, value: Expression) -> None:
        """
        Bind an expression value.
        """

    def emit(self) -> str:
        """
        Emit and validate Flow source.
        """

    def header(
        self,
        name: str,
        alias: str,
        default_value: str | None = None,
        description: str = "",
    ) -> None:
        """
        Append a documented header with an optional byte-text default.
        """

    def pipe(self, value: Expression, destination: Expression) -> None:
        """
        Write an expression to a destination.
        """

    @typing.overload
    def port(
        self,
        name: str,
        type: str,
        output: bool = False,
        unary: bool = True,
        required: bool = False,
        description: str = "",
    ) -> None:
        """
        Append an input or output port.
        """
    @typing.overload
    def port(
        self,
        name: str,
        type: Type,
        output: bool = False,
        unary: bool = True,
        required: bool = False,
        description: str = "",
    ) -> None:
        """
        Append a port with a typed grammar descriptor.
        """

    @property
    def description(self) -> str:
        """
        The action description.
        """
    @description.setter
    def description(self, arg1: str) -> None: ...

    @property
    def exported(self) -> bool:
        """
        Whether this flow belongs to the module's public API.
        """
    @exported.setter
    def exported(self, arg1: bool) -> None: ...

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

    def action_schema(self) -> ActionSchema:
        """
        The selected native implementation's action schema, including header defaults.
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
    def exported(self) -> bool:
        """
        Whether the flow is part of its module's public API.
        """

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
    def imported(self) -> bool:
        """
        Whether the implementation belongs to an imported module.
        """

    @property
    def inputs(self) -> collections.abc.Mapping[str, ActionPortSchema]:
        """
        The declared input ports, by name, as the action schema has them.
        """

    @property
    def interface_only(self) -> bool:
        """
        Whether this is an interface declaration.
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

        The native schema supplies ports and header defaults. Python attaches
        the corresponding builtin ``typeinfo`` handles.
        """

    @property
    def source_name(self) -> str: ...

class ModuleDocument:
    """
    An owning editable Flow module, with revisioned native source transactions.
    """

    def __init__(self, identity: str = "<memory>", source: str = "") -> None:
        """
        Create a module or open source for editing.
        """

    def apply(
        self,
        revision: typing.SupportsInt,
        edits: collections.abc.Sequence[SourceEdit],
    ) -> DocumentChange:
        """
        Commit nonoverlapping edits against an expected revision.
        """

    def check(
        self, modules: collections.abc.Mapping[str, str] = {}
    ) -> typing.Any:
        """
        Analyze this module and its supplied import graph.
        """

    def compile(
        self, modules: collections.abc.Mapping[str, str] = {}
    ) -> Program:
        """
        Build an immutable execution snapshot from supplied modules.
        """

    def effective_schema(
        self, name: str, modules: collections.abc.Mapping[str, str] = {}
    ) -> typing.Any:
        """
        Query the effective conjunction of compatible callable contributions.
        """

    def insert(self, flow: FlowBuilder) -> DocumentChange:
        """
        Insert one flow contribution.
        """

    def insert_struct(self, shape: StructBuilder) -> DocumentChange:
        """
        Insert a struct contribution.
        """

    def remove(self, symbol: DocumentSymbolHandle) -> DocumentChange:
        """
        Remove one contributing symbol.
        """

    def remove_import(
        self, alias: str, scope: DocumentSymbolHandle | None = None
    ) -> DocumentChange:
        """
        Remove an import from its lexical scope.
        """

    def rename(self, symbol: DocumentSymbolHandle, name: str) -> DocumentChange:
        """
        Rename a local symbol and its references.
        """

    def rename_import(
        self, alias: str, name: str, scope: DocumentSymbolHandle | None = None
    ) -> DocumentChange:
        """
        Rename a namespace and its lexically bound action/type references.
        """

    def rename_in_graph(
        self,
        symbol: DocumentSymbolHandle,
        name: str,
        modules: collections.abc.Mapping[str, str] = {},
    ) -> ModuleGraphChange:
        """
        Prepare validated reference edits across supplied modules without mutating this document.
        """

    def set_exported(
        self, symbol: DocumentSymbolHandle, value: bool
    ) -> DocumentChange:
        """
        Set visibility with a local source edit.
        """

    def symbols(self) -> list[DocumentSymbolHandle]:
        """
        Find the contributing declarations and definitions.
        """

    def update(
        self, symbol: DocumentSymbolHandle, flow: FlowBuilder
    ) -> DocumentChange:
        """
        Replace one owned contribution, retaining visibility and declaration kind.
        """

    def update_struct(
        self, symbol: DocumentSymbolHandle, shape: StructBuilder
    ) -> DocumentChange:
        """
        Update one struct contribution, retaining visibility and unrelated source text.
        """

    def upsert_declaration(
        self,
        schema: ActionSchema,
        symbol: DocumentSymbolHandle | None = None,
        exported: bool = False,
    ) -> DocumentChange:
        """
        Synchronize one owned interface from a native action schema without weakening unrepresentable metadata.
        """

    def upsert_import(
        self,
        reference: str,
        alias: str,
        scope: DocumentSymbolHandle | None = None,
        symbol: str = "",
    ) -> DocumentChange:
        """
        Insert or update an import in the module or an implemented flow.
        """

    @property
    def identity(self) -> str:
        """
        The module's canonical host identity.
        """

    @property
    def revision(self) -> int:
        """
        The current document revision.
        """

    @property
    def source(self) -> str:
        """
        The current source text.
        """

class ModuleGraphChange:
    """
    A validated source rename across an in-memory module graph.
    """

    @property
    def diagnostics(self) -> typing.Any:
        """
        Public API and graph boundary information.
        """

    @property
    def edits(self) -> dict[str, list[SourceEdit]]:
        """
        Byte edits grouped by source identity.
        """

    @property
    def sources(self) -> dict[str, str]:
        """
        The updated source snapshots.
        """

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
        Register the root module's named implementations in ``registry``.
        """

    @property
    def flows(self) -> dict[str, FlowPlan]:
        """
        Every flow, by name, in declaration order.
        """

    @property
    def has_entry(self) -> bool:
        """
        Whether the file declares a `flow { ... }` -- a program.

        Returns a bool because an entry flow has no name and cannot be accessed through
        `program["..."]`, `run`, or `call`. Use this to choose between `run_program` and
        a named flow from `names`.
        """

    @property
    def main(self) -> FlowPlan:
        """
        The first flow declared, which is the one a file is usually about.
        """

    @property
    def names(self) -> list[str]:
        """
        Every named flow, in the order the file declares them. The entry flow -- `flow { ... }` -- is not here: it has no name, and it is run rather than called.
        """

    @property
    def source_name(self) -> str: ...

class SourceEdit:
    """
    A byte-range replacement in a source document.
    """

    text: str
    def __init__(
        self, start: typing.SupportsInt, end: typing.SupportsInt, text: str
    ) -> None:
        """
        Construct a source replacement.
        """

    @property
    def end(self) -> int: ...
    @end.setter
    def end(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def start(self) -> int: ...
    @start.setter
    def start(self, arg0: typing.SupportsInt) -> None: ...

class Statement:
    """
    A typed statement in an executable Flow body.
    """

    @staticmethod
    def after(
        subjects: collections.abc.Sequence[Expression],
        body: collections.abc.Sequence[Statement],
        tolerant: bool = False,
    ) -> Statement:
        """
        Gate a concurrent block on completion subjects.
        """

    @staticmethod
    def bind(name: str, value: Expression) -> Statement:
        """
        Bind a scalar expression.
        """

    @staticmethod
    def block(
        body: collections.abc.Sequence[Statement], tolerant: bool = False
    ) -> Statement:
        """
        Group concurrent statements with one outcome.
        """

    @staticmethod
    def cancel(subject: Expression) -> Statement:
        """
        Cancel an owned action.
        """

    @staticmethod
    def fail(code: Expression, message: Expression | None = None) -> Statement:
        """
        Fail with a status and optional message.
        """

    @staticmethod
    def for_(
        variable: str,
        source: Expression,
        body: collections.abc.Sequence[Statement],
        parallel: typing.SupportsInt = 1,
    ) -> Statement:
        """
        Construct a loop over a stream.
        """

    @staticmethod
    def if_(
        condition: Expression,
        then_body: collections.abc.Sequence[Statement],
        else_body: collections.abc.Sequence[Statement] = [],
    ) -> Statement:
        """
        Construct conditional statement bodies.
        """

    @staticmethod
    def pipe(value: Expression, destination: Expression) -> Statement:
        """
        Write a stream to a destination.
        """

    @staticmethod
    def wait(subject: Expression, drain: bool = False) -> Statement:
        """
        Wait for or drain a subject.
        """

    @property
    def source(self) -> str:
        """
        The emitted statement.
        """

class StructBuilder:
    """
    Construct a native Flow struct.
    """

    def __init__(self, name: str) -> None:
        """
        Create a struct builder.
        """

    def emit(self) -> str:
        """
        Emit validated struct source.
        """

    def field(
        self,
        name: str,
        type: Type,
        required: bool = False,
        default_value: Expression | None = None,
        description: str = "",
    ) -> None:
        """
        Append a typed field with an optional literal default.
        """

    @property
    def description(self) -> str:
        """
        The struct description.
        """
    @description.setter
    def description(self, arg1: str) -> None: ...

    @property
    def exported(self) -> bool:
        """
        The module visibility.
        """
    @exported.setter
    def exported(self, arg1: bool) -> None: ...

class Type:
    """
    A native Flow type expression.
    """

    def __init__(
        self,
        name: str,
        parameters: collections.abc.Sequence[Type] = [],
        quoted: bool = False,
    ) -> None:
        """
        Construct a type, optionally with generic arguments or a quoted media type.
        """

    @property
    def source(self) -> str:
        """
        The grammar-aware type spelling.
        """

def check(
    source: str,
    source_name: str = "-",
    modules: collections.abc.Mapping[str, str] = {},
    resolve_files: bool = False,
) -> dict[str, typing.Any]:
    """
    Everything wrong with one flow file.

    Returns a ``flow.diagnostics/v1`` payload: the syntax and form problems the parser
    found, and the name, sequence and barrier problems the resolver found, in source
    order. Every problem in the file, not the first -- both passes recover.

    This is the whole of what ``a11 flow check`` and an editor need. ``flow.compile``
    is the same engine with a strict door on it, for actually running one.
    """

def check_program(source: str, source_name: str = "") -> str:
    """
    What a program's entry flow is, compiling it and running nothing.

    Raises if the source will not compile or declares no ``flow { ... }``.
    """

def codes() -> list[dict[str, typing.Any]]:
    """
    Every diagnostic code the language publishes, with its meaning.

    The same table ``testdata/flow/codes.json`` is generated from, so a toolchain may
    read either and get the same answer.
    """

def compile(
    source: str,
    source_name: str = "",
    modules: collections.abc.Mapping[str, str] = {},
    resolve_files: bool = False,
) -> Program:
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

def register_standard_actions(
    registry: ActionRegistry,
    roots: collections.abc.Sequence[str],
    allow_write: bool = False,
    allow_run: bool = False,
    require_sandbox: bool = True,
    inherit_environment: bool = False,
    max_seconds: typing.SupportsInt = 600,
    current_directory: str = ".",
    unrestricted: bool = False,
) -> None:
    """
    Register the native filesystem and process actions on a registry.

    The policy is captured by the handlers at registration. A caller may narrow one
    run through action options but cannot widen the roots, write access, process
    access, environment inheritance, deadline ceiling, or kernel-sandbox requirement.
    Relative filesystem paths resolve against ``current_directory``. Spawned
    processes may make outbound network connections by default.
    ``unrestricted=True`` disables filesystem containment and the kernel sandbox;
    write and process permissions, environment filtering, and limits still apply.
    """

def request(
    request: dict[str, typing.Any], resolve_files: bool = False
) -> dict[str, typing.Any]:
    """
    One request to the language service, answered.

    The same method dispatch the standalone ``a11-flow serve`` speaks, reachable
    without spawning it: ``{"method": "check", "source": "..."}`` gives
    ``{"ok": true, "result": {...}}``. Every method is available through a named
    function here as well; this is for a frontend that is *relaying* -- a server, a
    plugin, a test of the protocol -- and would otherwise have to keep its own table
    of which method means which call.
    """

def run_program(
    source: str,
    source_name: str = "",
    *,
    arguments: collections.abc.Sequence[str] = [],
    roots: collections.abc.Sequence[str] = [],
    allow_write: bool = False,
    allow_run: bool = False,
    allow_net: bool = False,
    allow_local_net: bool = False,
    allow_env: collections.abc.Sequence[str] = [],
    unrestricted: bool = False,
    timeout_seconds: typing.SupportsFloat | None = None,
    standard_streams: bool = True,
    registry: typing.Any | None = None,
    session: typing.Any | None = None,
    dispatch_stream: typing.Any | None = None,
) -> dict[str, typing.Any]:
    """
    Run a Flow program's entry flow, and return what it did.

    The same interpreter ``a11-flow-run`` is -- one implementation, called two ways
    -- so a program behaves identically whichever started it.

    Pass a ``registry`` that already holds your own actions and the program can call
    them: a process that registered ``interact_with_llm`` can run a file that asks a
    model, with no subprocess and nothing about the file changed. Names you
    registered are never replaced by the standard library's.

    What the program may *do* is the arguments here and nothing the file can say --
    ``roots``, ``allow_write``, ``allow_run``, ``allow_net``, ``allow_env`` -- for
    the same reason they are command-line flags in the CLI.

    Pass ``session`` and ``dispatch_stream`` together and the program's ``call``
    steps are dispatched at that peer while its ``run`` steps stay here. The peer's
    actions still have to be in ``registry`` for their schemas -- registered without
    handlers, which is how "this one lives on the peer" is spelled -- because the
    resolver looks a name up before it decides anything.
    """

def scan(paths: collections.abc.Sequence[str]) -> dict[str, typing.Any]:
    """
    The actions a project declares, read out of its source.

    Walks each path -- a file or a directory -- for ``ActionSchema`` declarations in
    Python, C++ and TypeScript, and returns a ``flow.catalogue/v1`` payload in which
    every entry carries its declaration ``origin``. Editors use that location for
    hover information and "go to declaration".

    A tolerant textual read, not a parser for three languages: a schema written as a
    constructor call with literal arguments comes back whole, one assembled statement
    by statement comes back with thinner ports, and one whose *name* is computed is
    not found. ``scanned`` says how many files were read and what was skipped, so a
    caller can tell a half-read tree from a small one.

    The result is what a frontend passes as ``context`` to the other methods, or
    merges over the embedded snapshot itself.
    """

def stages() -> dict[str, str]:
    """
    Every pipeline stage, and what each one takes after its name.

    ``"none"``, ``"number"``, ``"expr"``, ``"string"``, ``"string?"`` or ``"stream"``
    -- the same table the parser reads, so an editor offering completions after a
    ``|`` needs no list of its own.
    """

def standard_action_schemas() -> list[ActionSchema]:
    """
    Return native standard-library schemas without running any actions.
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

def syntax_targets() -> list[dict[str, str]]:
    """
    Every editor definition the language generates, and where each belongs.

    One entry per target: its name, as ``syntax`` is asked for it, and the path the
    generated file is checked in at. What the CLI offers as a choice and what it
    walks when it is asked to check them all -- so a target added in C++ is a target
    the check covers, rather than one a list in Python has to be told about.
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

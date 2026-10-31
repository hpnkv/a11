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

r"""Compose asynchronous, streaming actions as reusable actions.

Use ``run`` for a local handler and ``call`` for a peer. Steps overlap; data
and explicit ``after`` dependencies determine their order. Every output is
read, including outputs that the composition does not expose.

```a11flow
flow answer {
  in question: string required
  out text: string
  work = run search(query: question) timeout 30s
  work.results | collect | map std::join(it, "\\n") -> text
}
```

Initialize values with ``x = expression`` or ``T x = expression``. Initializers
run once when their scope activates and dependencies are ready. Reassignment
uses source order; child scopes publish updates when they complete. Conflicting
updates in parallel loop passes are rejected. Capability and port names retain
their own categories and cannot be reassigned as ordinary values.

``take stream`` consumes the next item. Successive reads consume successive
items in source order, and EOF fails. ``try take stream`` returns an option with
``has_value`` and, when present, ``value``. Null items remain distinct from EOF;
upstream failures propagate. Destructuring reads its RHS once.
``T settings = try take options or T{}`` unwraps a present option and evaluates
the fallback only on absence. Present null, false, zero and empty values stay
present. Stored options support the same lazy ``or`` default.

Put loop postconditions after the closing brace:

```a11flow
flow forward {
  in words: string stream
  out result: string stream
  for word in words {
    word -> result
  } until word == "stop"
}
```

The stopping pass finishes before the condition is tested. ``close node`` ends
an owned node normally; ``wait subject`` observes completion. ``cancel`` stops
an action and ``abort`` ends a node with an error. ``try`` makes an operation's
failure available through its status.

``timeout`` accepts a positive ``std::duration`` expression.
``deadline`` accepts
an absolute ``std::time`` expression, evaluated eagerly once per scope
activation. ``deadline std::now() + 30s after prepared`` evaluates the entire
expression after ``prepared`` finishes. Parentheses around the expression do
not change this. Both words are contextual and may name variables.

Use ``::`` for module and type qualification, and ``.`` for runtime fields and
ports. Modules are declared with ``mod name { ... }``; ``declare mod`` contains
contracts and complete struct definitions. Modules and their members are
private by default; ``exported`` exposes a declaration and
``export symbol [as alias]`` exposes an accessible existing symbol.

``import "path/library.flow"`` binds ``library``; ``as alias`` selects another
name. ``from library import member`` and ``from library import *`` bind public
members in the current module or flow. The implicit reserved ``std`` module
needs no file import. Standard functions and nonprimitive types are lowercase
and qualified, such as ``std::now`` and ``std::duration``. Primitive names
``int``, ``int``, ``bool``, ``number``, ``string``, ``bytes``, ``object`` and
``null`` are available directly, together with their conversions. ``object[V]``
is a string-keyed homogeneous object; bare ``object`` has heterogeneous values.

Filesystem imports require explicit ``source_roots``. The default permits no
filesystem imports; supplied in-memory modules remain available. Compilation
uses host-configurable byte, token, nesting, import, work and time limits.

The lexer, parser, resolver, tooling and runtime live in ``cpp/a11/flow``.
Python, command-line tools and editors use that native implementation.
"""

from __future__ import annotations

import os
from collections.abc import Mapping, Sequence
from typing import Any

from a11._native import flow as _flow
from a11.flow.diagnostics import FlowSyntaxError
from a11.flow.plan import TYPE_NAMES, FlowPlan, Program, compile_source
from a11._native.flow import (
    CompilationLimits,
    Expression,
    Statement,
    CallOptions,
    Type,
    StructBuilder,
    FlowBuilder,
    ModuleDocument,
    DocumentSymbolHandle,
    SourceEdit,
    DocumentChange,
    ModuleGraphChange,
)
from a11.flow.runtime import invoke

#: Every function an expression may call, read from the language's own table.
BUILTINS: frozenset[str] = frozenset(_flow.vocabulary()["builtins"])

#: Every pipeline stage, and what each one takes after its name: ``"none"``,
#: ``"number"``, ``"expr"``, ``"string"``, ``"string?"`` or ``"stream"``.
STAGES: dict[str, str] = _flow.stages()

#: The names `fail` accepts, in the case they are canonically spelled in.
FAIL_CODES: tuple[str, ...] = tuple(
    sorted(code.upper() for code in _flow.vocabulary()["status_codes"])
)

#: A compact language reference for models that write flows. Action schemas
#: provide the remaining information needed to compose available actions.
REFERENCE = """A11 Flow composes streaming actions into actions.
Ordinary and triple-quoted strings decode \\0, \\a, \\b, \\f, \\n, \\r, \\t,
\\v, escaped quotes and backslashes. Other escapes yield the following character.
Keywords are lower or UPPER
case. Statements overlap; data and after dependencies determine execution.

flow NAME [exported] {
  in input: string [stream] [required]
  out output: string [stream]
  job = run local_action(input: input) timeout 30s
  job.output | map std::upper(it) -> output
}
call invokes a peer; run invokes a local handler. All outputs are read.

x = expression or T x = expression initializes once per scope, eagerly.
Reassignment follows source order, including child-scope updates. Parallel
conflicting updates are rejected. Names and destructuring patterns are lvalues.
x = take source consumes one successive item; EOF fails.
x = try take source returns {has_value: bool, value: T when present}.
Errors fail.
A real null item has has_value=true. Prefer if item = try take source { ... }.
if item = option evaluates once and binds its payload in the success branch.
EOF is absence; false-like payloads remain present. The name must be fresh.
Explicit option.has_value and option.value access remains available.
T settings = try take options or T{} unwraps the option or lazily evaluates T{}.
Stored std::option[T] values support the same or default. Presence, including
null, false, zero and empty payloads, skips the fallback. Errors propagate.
With ordinary values, or keeps its truthiness-based short-circuit behavior.
Read the RHS of destructuring only once.

for item in source [parallel N] { body }
  [until EXPR | while EXPR] [after subject]
A bare list literal, for x in [a, b, c] { ... }, iterates its elements once,
one level deep. Empty literals make no passes. Other sources keep their stream
values: use | flatten for named lists or list-valued pipelines.
parallel accepts an integer expression, e.g. parallel workers or
parallel (workers + 1), evaluated once per loop or stage activation. Dynamic
counts must be positive signed 32-bit integers and retain parallel safety
restrictions on loop updates and trailing conditions.
repeat [state = INITIAL] [max N] {
  body
  state <- NEXT
} [until EXPR | while EXPR]
Postconditions run after the entire pass, in that pass's scope. Postconditions
require sequential loops. A repeat needs a condition or an explicit bound.

a -> b -> out associates right and forwards b, then a. A transfer returns a
continuation sink: alias it without repeating work; attach streams on its right.
Independent attachments interleave after the prefix; wait/status include all
attached writers. Virtual sinks and serial chain flattening are enabled
by default.
A terminal comma list broadcasts. Group ambiguous combinations explicitly.

node() creates a node. close NODE ends it normally; abort NODE [CODE [MESSAGE]]
ends it with an error. wait SUBJECT observes completion; status SUBJECT is its
status. cancel ACTION cancels an action. fail [CODE [MESSAGE]] fails the scope.
try run/call or try { body } exposes a failure through the operation's status.
wait first of a, b chooses a completion; wait all of a, b waits for every one.
Bare wait all is a scope-local execution barrier: all preceding work must
complete successfully before any following statement activates. This
includes initializers, arguments, eager deadlines, owned calls, feeds,
draining, and child scopes. Work within a phase remains concurrent. Barriers
inside branches, blocks, and loop passes affect only that scope or pass.
Stream aliases do not read or exhaust streams; cross-phase producer/consumer
cycles are errors.
Bare wait all accepts no modifiers or value binding. wait all of a, b remains
an explicit-subject wait and does not gate following statements.
after a, b { body } gates a scope. Bound blocks and loops expose completion.

Call and wait modifiers: timeout DURATION_EXPRESSION, deadline TIME_EXPRESSION,
after SUBJECTS. Deadlines evaluate once eagerly; an after clause delays the
whole expression. deadline std::now() + 30s after action and the parenthesized
form have identical meaning. timeout timeout and deadline deadline are valid
when the names hold the corresponding values. The earlier bound applies when
both modifiers are present. Expiry is DEADLINE_EXCEEDED.
Other call modifiers: via MAP, tee, id EXPR, with "header": EXPR,
forward headers "name". Local nodes stay off the wire.

source | stage -> destination [, destination]; -> _ discards results.
source | run flow_name(config: value) | call module::flow_name() composes
whole-stream invocations. After explicit arguments, exactly one input must
remain (optional inputs count too), and exactly one output must be declared.
Unary inputs validate zero/one or exactly one item according to required.
Declare/import external interfaces before using them as stages. Calls retain
ordinary dispatch, modifiers, draining, failure, and cleanup semantics.
try run/call tolerates the invocation, not failures reading its output;
use a try block for the whole pipeline, or combine a try invocation with a
try pipe to tolerate transfer errors too. No item is dropped.

Stages include map EXPR, where EXPR, first N, drop N, collect, flatten,
scan INITIAL as state, EXPR, fold INITIAL as state, EXPR, sum, count,
take while EXPR, take until EXPR, as TYPE, json, packb, timeout DURATION.
Stages stay unqualified. Functions are qualified: std::len, std::upper,
std::join, std::match, std::strformat, std::now, std::duration, std::time.
from std import now makes now() available without a prefix.
std::parse_time(text) and std::parse_duration(text) fail on invalid text;
std::time(text) and std::duration(text) retain permissive conversions.

mod name [exported] { declarations }
declare mod name [exported] { contracts, declared modules, full structs }
declare flow company::search::find { interface }
struct Row [exported] {
  name: string required
  count: int
}
import "path/library.flow" [as alias] loads a module; default name is library.
from library import member [as alias], other; from library import *.
export library::member [as alias] publishes an accessible existing symbol.
Modules cannot be created inside flows or structs. std is reserved.
:: qualifies static modules/types; . projects runtime fields/ports.
Host type source a11::sdk::Interaction retains wire tag a11.sdk.Interaction.

Primitive prelude: int/integer, bool, number, string, bytes, object, null;
literals true, false, null. Exposed types expose their conversions too.
Other standard types: std::status, std::time, std::duration, std::list[T],
std::option[T], std::stream[T]. object[V] has string keys and one value type;
bare object is heterogeneous. Typed initialization uses existing coercions and
checks the target invariant. Initializers may not be omitted.

File imports require explicitly approved source_roots; none means filesystem
imports are disallowed. Explicit in-memory modules remain available. Parsing
and compilation have configurable depth, size, work and time limits.
"""


#: The conventional extension for a file of flows.
EXTENSION = ".flow"


def loads(
    source: str,
    source_name: str = "",
    *,
    modules: dict[str, str] | None = None,
    source_roots: list[str] | None = None,
    limits: CompilationLimits | None = None,
    single_item_slots: bool = True,
) -> Program:
    """Compile Flow source into a [Program][a11.flow.plan.Program].

    Args:
        source: The text of one or more ``flow`` declarations.
        source_name: A name for error messages, usually a file path.

    Raises:
        FlowSyntaxError: On any problem, with the line and column it was at.
    """
    return compile_source(
        source,
        source_name,
        modules=modules,
        source_roots=source_roots,
        limits=limits,
        single_item_slots=single_item_slots,
    )


def load(
    path: str | os.PathLike[str],
    *,
    source_roots: list[str] | None = None,
    limits: CompilationLimits | None = None,
    single_item_slots: bool = True,
) -> Program:
    """Compile a ``.flow`` file."""
    with open(path, encoding="utf-8") as handle:
        return compile_source(
            handle.read(),
            os.path.abspath(path),
            source_roots=source_roots,
            limits=limits,
            single_item_slots=single_item_slots,
        )


def register(source: str, registry: Any, source_name: str = "") -> Program:
    """Compile Flow source and register its root implementations as actions.

    The one call a service needs to accept a composition from outside and
    make it runnable: after this the flows are in the registry, and a session
    dispatches them like any other action.
    """
    return loads(source, source_name).register_all(registry)


def run_program(
    source: str,
    source_name: str = "",
    *,
    arguments: Sequence[str] | None = None,
    roots: Sequence[str] | None = None,
    allow_write: bool = False,
    allow_run: bool = False,
    allow_net: bool = False,
    allow_local_net: bool = False,
    allow_env: Sequence[str] | None = None,
    unrestricted: bool = False,
    timeout_seconds: float | None = None,
    standard_streams: bool = True,
    registry: Any = None,
    session: Any = None,
    dispatch_stream: Any = None,
    source_roots: Sequence[str] | None = None,
    limits: CompilationLimits | None = None,
    single_item_slots: bool = True,
    frame_cache_bytes: int = 256 * 1024,
    frame_budget_bytes: int | None = None,
) -> dict[str, Any]:
    """Run a Flow program's entry flow — the `flow { ... }` with no name.

    This calls the `a11-flow-run` interpreter in process. Pass a registry to
    expose actions from the host process:

    ```python
    from a11 import flow
    from a11.actions import ActionRegistry
    from a11.sdk import llm_tools

    registry = ActionRegistry()
    llm_tools.register(registry)
    flow.run_program(
        source,
        "summarise.flow",
        registry=registry,
        arguments=["summarise.flow", "notes.txt"],
    )
    ```

    A name already in `registry` is never replaced by the standard library's: a
    host that registered its own `read_file` meant its own `read_file`.

    !!! important "Run off-loop for async action handlers"

        This runs the program to completion, so it blocks the thread it is
        called on. A Python action handler written `async def` has to be driven
        by an asyncio loop — and if that loop is on *this* thread, it cannot run
        while this call is blocking it, and the program waits forever on its own
        handler. So call it in a thread:

        ```python
        outcome = await asyncio.to_thread(
            flow.run_program, source, "program.flow", registry=registry
        )
        ```

        A program that only uses the standard library needs none of this: those
        actions are native and take no GIL.

    Args:
        source: The program's text.
        source_name: What diagnostics should call it, usually the path.
        arguments: The program's `argv`. By convention `arguments[0]` is the
            file, as a C program's is, so what a user passed starts at index 1.
        roots: Directories the program may reach. Defaults to the working
            directory.
        allow_write: Whether it may write, inside those roots.
        allow_run: Whether it may run programs, confined by the kernel where
            the platform can.
        allow_net: Whether it may reach the network. Loopback, private and
            link-local addresses stay refused unless `allow_local_net`.
        allow_local_net: Also allow loopback, private, and link-local
            addresses (e.g. cloud metadata services).
        allow_env: Environment variables it may read.
        unrestricted: No filesystem sandbox. Use only with trusted source.
        timeout_seconds: A bound on the whole run, applied as the deadline
            header every standard-library action honours.
        standard_streams: Whether to bind this process's stdin/stdout/stderr.
            Clear it in a host with no useful standard input. A read then fails
            instead of waiting indefinitely.
        registry: An [ActionRegistry][a11.actions.ActionRegistry] whose actions
            the program may call. One is made when omitted.

        frame_cache_bytes: Cached frame capacity per runner; zero disables reuse.
        frame_budget_bytes: Reserved frame capacity across local Flow calls.
            Excludes payloads, fiber stacks, and ordinary containers.

    Returns:
        `{"exit_code": int, "diagnostics": [...]}` — the code the program put on
        an `out exit_code: int` port (or 0), and whatever the compiler said
        that was not an error.

    Raises:
        Exception: When the source will not compile, declares no entry flow, or
            the program itself failed. What the flow failed with is the message.
    """
    return _flow.run_program(
        source,
        source_name,
        arguments=list(arguments or ()),
        roots=list(roots or ()),
        allow_write=allow_write,
        allow_run=allow_run,
        allow_net=allow_net,
        allow_local_net=allow_local_net,
        allow_env=list(allow_env or ()),
        unrestricted=unrestricted,
        timeout_seconds=timeout_seconds,
        standard_streams=standard_streams,
        registry=registry,
        session=session,
        dispatch_stream=dispatch_stream,
        source_roots=list(source_roots or []),
        limits=limits or CompilationLimits(),
        single_item_slots=single_item_slots,
        frame_cache_bytes=frame_cache_bytes,
        frame_budget_bytes=frame_budget_bytes,
    )


def register_standard_actions(
    registry: Any,
    roots: Sequence[str],
    *,
    allow_write: bool = False,
    allow_run: bool = False,
    require_sandbox: bool = True,
    inherit_environment: bool = False,
    max_seconds: int = 600,
    current_directory: str = ".",
    unrestricted: bool = False,
) -> None:
    """Register native filesystem and process actions with a fixed policy.

    ``spawn_process`` uses Landlock on Linux and Seatbelt on macOS. With
    ``require_sandbox=True``, registration succeeds but a process call refuses
    to start on a host where the kernel confinement is unavailable.
    Relative filesystem paths resolve against ``current_directory``.
    ``unrestricted=True`` disables filesystem containment and the kernel
    sandbox. Write/process permissions and resource limits still apply.
    """
    _flow.register_standard_actions(
        registry,
        list(roots),
        allow_write=allow_write,
        allow_run=allow_run,
        require_sandbox=require_sandbox,
        inherit_environment=inherit_environment,
        max_seconds=max_seconds,
        current_directory=current_directory,
        unrestricted=unrestricted,
    )


def check_program(source: str, source_name: str = "") -> str:
    """What a program's entry flow is, compiling it and running nothing.

    Raises if the source will not compile or declares no `flow { ... }`.
    """
    return _flow.check_program(source, source_name)


def request(payload: Mapping[str, Any]) -> dict[str, Any]:
    """Ask the language one question about one document.

    The Python adapter over the one service every Flow frontend is a frontend
    of — the same `{"method": .., "source": ..}` request `a11-flow serve` and
    `a11 flow serve` answer, and the same envelope back. A capability added
    there is available here without anything being added here.

    ```python
    from a11 import flow

    problems = flow.request({"method": "check", "source": "flow t { }"})
    schema = flow.request(
        {"method": "schema", "source": src, "struct": "Source"}
    )
    ```
    """
    return _flow.request(dict(payload))


__all__ = [
    "BUILTINS",
    "EXTENSION",
    "FAIL_CODES",
    "Expression",
    "Statement",
    "CallOptions",
    "Type",
    "StructBuilder",
    "FlowBuilder",
    "ModuleDocument",
    "DocumentSymbolHandle",
    "SourceEdit",
    "DocumentChange",
    "ModuleGraphChange",
    "FlowPlan",
    "FlowSyntaxError",
    "Program",
    "REFERENCE",
    "STAGES",
    "TYPE_NAMES",
    "compile_source",
    "invoke",
    "load",
    "loads",
    "register",
    "register_standard_actions",
    "check_program",
    "request",
    "run_program",
]

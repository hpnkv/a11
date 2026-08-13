# Copyright 2026 The A11 Authors.

"""``a11 flow``: read, check and describe Flow programs from the command line.

Subcommands:

* ``check FILE...`` -- compile each file and report what is wrong with it.
  Exits 1 if anything is an error, so it works in a pre-commit hook or a CI job.
* ``fmt FILE...`` -- format a flow: ``-i`` to rewrite it, ``--check`` for CI.
* ``parse FILE`` -- the syntax tree, and everything the parser could not read.
* ``describe FILE`` -- the resolved plan: ports, headers, steps, node maps.
* ``highlight FILE`` -- what each token means, for a syntax highlighter.
* ``complete FILE`` -- what may be written at a position in it.
* ``run FILE`` -- run one of its flows here, and print what its ports produced.
* ``serve`` -- answer language requests on standard input, one per line.
* ``syntax`` -- generate the editor definitions, or check they are current.
* ``codes`` -- every diagnostic code the language publishes, and what it means.

Every subcommand takes ``--format``. ``text`` is for people, ``json`` is the
versioned envelope documented in [a11.flow.diagnostics][a11.flow.diagnostics], and
``sarif`` is what code-scanning services and CI annotators already read. A file of
``-`` is standard input, so a flow can be piped in from anywhere.

The engine behind all of these is `cpp/a11/flow/` -- one lexer, one grammar, one
resolver, one set of checks, one formatter -- reached through `a11._native.flow`.
The standalone `a11-flow` binary runs the same library without Python, and answers
in the same envelopes; the formats are the contract between them, which is why they
are pinned by `testdata/flow/codes.json` and by this module's tests rather than by
whichever frontend happens to be printing them.

The one thing that is still Python is *running* a flow: ``run`` compiles through
`a11.flow.loads` and executes the graph `a11/flow/runtime.py` walks.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Any, Sequence

from a11 import timing
from a11.cli.app import Command
from a11.flow import diagnostics as diag

#: What `--format` accepts, in the order `--help` lists them.
_FORMATS = ("text", "json", "sarif")

#: Standard input, spelled the way every other tool spells it.
_STDIN = "-"


def _read(path: str) -> tuple[str, str]:
    """The source at ``path``, and the name to report problems against."""
    if path == _STDIN:
        return sys.stdin.read(), _STDIN
    return pathlib.Path(path).read_text(encoding="utf-8"), path


def _syntax_error_diagnostic(error: Any, source: str) -> diag.Diagnostic:
    """A `FlowSyntaxError` as the diagnostic every frontend renders.

    The compiler stops at the first problem it cannot read past, so this is one
    diagnostic and not a list. It carries a line and a column; the offset comes
    from the source, because a diagnostic promises all three.

    The code is the general one: every problem the current compiler raises is a
    `FlowSyntaxError` with a sentence, and inventing a precise code by matching on
    that sentence would be a mapping that rots. Precise codes arrive with the
    native parser and resolver, which report them at the point they are found.
    """
    index = diag.LineIndex(source)
    line = getattr(error, "line", 1) or 1
    column = getattr(error, "column", 1) or 1
    start = index.offset_of(line, column)
    # `message` is the sentence; `str(error)` prefixes it with the location, which
    # every output format here writes for itself.
    message = getattr(error, "message", None) or str(error)
    # The compiler points at a token without saying how long it is. A range of
    # one character is honest about that, and every frontend widens to the word
    # under it when it wants to.
    return diag.Diagnostic(
        code="flow.syntax.unexpected",
        message=message,
        range=index.between(start, min(start + 1, index.length)),
        severity=diag.Severity.ERROR,
        family=diag.Family.SYNTAX,
    )


def _check_source(source: str) -> list[diag.Diagnostic]:
    """Everything wrong with one flow file.

    One call into the native engine, which parses and resolves and reports *every*
    problem it finds rather than the first: three misspelled stages are three
    diagnostics, and a syntax problem on line 4 does not hide an unknown name on
    line 5. Both passes recover, which is what makes that possible.
    """
    from a11._native import flow as native_flow

    payload = native_flow.check(source)
    return [
        diag.Diagnostic.from_payload(entry) for entry in payload["diagnostics"]
    ]


def _emit(payload: dict[str, Any]) -> None:
    print(json.dumps(payload, indent=2))


async def _run_check(args: argparse.Namespace) -> int:
    exit_code = 0
    for path in args.files:
        try:
            source, name = _read(path)
        except OSError as error:
            print(f"{path}: cannot read: {error.strerror}", file=sys.stderr)
            exit_code = 2
            continue

        found = diag.sort_diagnostics(_check_source(source))
        errors = sum(1 for one in found if one.severity is diag.Severity.ERROR)
        if errors:
            exit_code = max(exit_code, 1)

        if args.format == "json":
            _emit(diag.diagnostics_envelope(name, found))
        elif args.format == "sarif":
            # With the text, so `charOffset` is the character count SARIF asks
            # for rather than the byte offset a diagnostic carries.
            _emit(diag.sarif_log(name, found, diag.LineIndex(source)))
        else:
            for one in found:
                print(one.as_text("" if name == _STDIN else name))
            if not found and not args.quiet:
                print(f"{name}: no problems found")
    return exit_code


async def _run_describe(args: argparse.Namespace) -> int:
    """The resolved plan of a file: ports, headers, node maps and steps.

    What a reader diffs to see whether a change to a flow changed what it *does*.
    The native resolver produces it, so this and `check` agree about what a file
    means -- and a file with an error in it still describes as far as it got, with
    the errors on standard error and a non-zero exit.
    """
    from a11._native import flow as native_flow

    source, name = _read(args.file)
    payload = native_flow.plan(source, "" if name == _STDIN else name)
    found = diag.sort_diagnostics(
        diag.Diagnostic.from_payload(entry) for entry in payload["diagnostics"]
    )
    errors = [one for one in found if one.severity is diag.Severity.ERROR]

    if args.format in ("json", "sarif"):
        # A plan is not a set of findings, so SARIF has nothing to say about it:
        # both machine formats give the plan envelope.
        _emit({**payload, "source": name})
        return 1 if errors else 0

    if errors:
        for one in errors:
            print(one.as_text("" if name == _STDIN else name), file=sys.stderr)
        return 1

    for flow in payload.get("flows", []):
        print(f"flow {flow.get('flow', '?')}")
        if flow.get("description"):
            print(f"  {flow['description']}")
        for direction in ("inputs", "outputs"):
            word = direction[:-1]
            for port_name, port in flow.get(direction, {}).items():
                shape = "one value" if port.get("unary", True) else "stream"
                required = ", required" if port.get("required") else ""
                print(
                    f"  {word:6} {port_name}: {port.get('type', 'any')}"
                    f" ({shape}{required})"
                )
        for header in flow.get("headers", []):
            print(f"  header {header}")
        for node_map in flow.get("node_maps", []):
            print(f"  nodes  {node_map}")
        for step in flow.get("steps", []):
            print(f"  {step.get('step', '?'):6} {step.get('label', '?')}")
    return 0


def _outline(node: Any, depth: int = 0) -> None:
    """One syntax node and everything under it, indented.

    For reading, not for parsing: `--format json` is what a tool consumes. What
    this is good for is seeing how the parser read something -- whether that `{`
    opened a block or a value, which statement swallowed the line.
    """
    if not isinstance(node, dict):
        return
    at = node.get("at", {})
    label = node.get("kind", "?")
    # The fields worth putting on the line: what the node is *called*, and the
    # scalars that tell two nodes of a kind apart.
    for key in ("name", "action", "mode", "op", "variable", "alias", "direction"):
        if node.get(key):
            label = f"{label} {node[key]}"
    print(f"  {at.get('line', 1):4}:{at.get('column', 1):<4}"
          f" {'  ' * depth}{label}")
    # In reading order rather than the envelope's -- the JSON is sorted by key,
    # which puts a flow's body in front of its ports.
    order = ("ports", "headers", "type", "condition", "source", "pipeline",
             "subject", "target", "value", "start", "stages", "args",
             "modifiers", "targets", "then_body", "else_body", "body")
    ranked = sorted(node.items(), key=lambda pair: (
        order.index(pair[0]) if pair[0] in order else len(order), pair[0]))
    for key, value in ranked:
        if key in ("kind", "at"):
            continue
        if isinstance(value, dict) and "kind" in value:
            _outline(value, depth + 1)
        elif isinstance(value, list):
            for item in value:
                if isinstance(item, dict) and "kind" in item:
                    _outline(item, depth + 1)
                elif isinstance(item, list) and len(item) == 2:
                    # A named child: an object's key, a call's port.
                    _outline(item[1], depth + 1)


async def _run_parse(args: argparse.Namespace) -> int:
    """The syntax tree, and everything wrong with the file.

    Both, always: the parser recovers, so a file with a mistake in it still has a
    tree. That is what an editor needs, and it is why `--format json` prints the
    tree even when the exit code is 1.
    """
    from a11._native import flow as native_flow

    source, name = _read(args.file)
    payload = native_flow.parse(source, "" if name == _STDIN else name)
    found = diag.sort_diagnostics(
        diag.Diagnostic.from_payload(entry) for entry in payload["diagnostics"]
    )
    errors = sum(1 for one in found if one.severity is diag.Severity.ERROR)

    if args.format in ("json", "sarif"):
        _emit(payload)
    else:
        for flow in payload.get("flows", []):
            _outline(flow)
        for one in found:
            print(
                one.as_text("" if name == _STDIN else name), file=sys.stderr
            )
    return 1 if errors else 0


async def _run_fmt(args: argparse.Namespace) -> int:
    """Format flows, in place, to standard output, or as a check.

    Exit codes are the ones every formatter uses, because that is what a hook and
    a CI job already expect: 0 when there was nothing to do, 1 when something
    would change (or, with `--check`, did not), 2 when a file could not be read or
    could not be parsed.
    """
    from a11._native import flow as native_flow

    exit_code = 0
    for path in args.files:
        try:
            source, name = _read(path)
        except OSError as error:
            print(f"{path}: cannot read: {error.strerror}", file=sys.stderr)
            exit_code = 2
            continue

        payload = native_flow.format(source)
        found = diag.sort_diagnostics(
            diag.Diagnostic.from_payload(entry) for entry in payload["diagnostics"]
        )
        errors = [one for one in found if one.severity is diag.Severity.ERROR]
        if errors:
            # A file that will not parse is left exactly as it is, and the
            # problem is what gets reported: half-formatting somebody's file is
            # how a formatter loses their work.
            for one in errors:
                print(
                    one.as_text("" if name == _STDIN else name), file=sys.stderr
                )
            exit_code = 2
            continue

        if args.format == "json":
            _emit({**payload, "source": name})
            if payload["changed"]:
                exit_code = max(exit_code, 1)
            continue

        if args.check:
            if payload["changed"]:
                print(f"{name}: would be reformatted")
                exit_code = max(exit_code, 1)
            elif not args.quiet:
                print(f"{name}: already formatted")
            continue

        if args.in_place:
            if name == _STDIN:
                print("-i cannot rewrite standard input", file=sys.stderr)
                return 2
            if payload["changed"]:
                pathlib.Path(name).write_text(
                    payload["formatted"], encoding="utf-8"
                )
                print(f"{name}: reformatted")
                exit_code = max(exit_code, 1)
            elif not args.quiet:
                print(f"{name}: already formatted")
            continue

        sys.stdout.write(payload["formatted"])
    return exit_code


async def _run_highlight(args: argparse.Namespace) -> int:
    """What each token in a flow *means*, from the one implementation of that.

    The native classifier decides it -- a stage after a `|`, a type past a port's
    `:`, a member after a `.`, a function only where it is called -- so an editor
    that reads this needs no lexer of its own. `text` prints the source with each
    token labelled, which is mostly useful for seeing why something is coloured
    the way it is.
    """
    from a11._native import flow as native_flow

    source, name = _read(args.file)
    payload = native_flow.highlight(source, "" if name == _STDIN else name)
    if args.format in ("json", "sarif"):
        _emit(payload)
        return 0

    for token in payload["tokens"]:
        text = source[token["start"] : token["end"]]
        if text == "\n":
            continue
        print(
            f"{token['line']:4}:{token['column']:<4} {token['kind']:<20} {text}"
        )
    return 0


#: The targets there are. A short list on purpose: an editor that can run a
#: process reads the language directly (`a11-flow serve`), and only one that
#: cannot -- a static grammar file, loaded by a highlighter with no way to call
#: out -- needs a generated copy of the word lists at all.
_TARGET_NAMES = ("sublime",)


async def _run_syntax(args: argparse.Namespace) -> int:
    """Write the generated editor definitions, or check they are up to date.

    A static grammar file is a copy of the language's word lists, and a copy is a
    thing that falls behind: `then` added as a stage is a stage the editor does not
    colour until somebody remembers the file. So it is generated from the one table
    and this holds it to being current -- which is a diff, not a judgement.
    """
    from a11._native import flow as native_flow

    names = _TARGET_NAMES if args.target == "all" else (args.target,)
    changed = 0
    written: list[str] = []
    for name in names:
        generated = native_flow.syntax(name)
        path = pathlib.Path(args.root) / generated["path"]
        if args.generate:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(generated["text"], encoding="utf-8")
            written.append(str(path))
            if args.format == "text":
                print(f"{generated['path']}: generated")
            continue
        current = (
            path.read_text(encoding="utf-8") if path.is_file() else None
        )
        if current == generated["text"]:
            if args.format == "text" and not args.quiet:
                print(f"{generated['path']}: up to date")
            continue
        changed += 1
        print(
            f"{generated['path']}: out of date -- run"
            f" `a11 flow syntax --target {name} --generate`",
            file=sys.stderr,
        )

    if args.format in ("json", "sarif"):
        _emit(
            {
                "format": "flow.syntax-check/v1",
                "targets": sorted(names),
                "generated": written,
                "out_of_date": changed,
            }
        )
    return 1 if changed else 0


async def _run_complete(args: argparse.Namespace) -> int:
    """What may be written at one position in a file.

    The one implementation of that judgement, through the command line: after a
    `|` only a stage, past a port's `:` only a type, after `x.` only what `x`
    actually has. An editor that can run a process needs no completion of its own,
    and this is the same answer `a11-flow complete` and the plugin get.
    """
    from a11._native import flow as native_flow

    source, name = _read(args.file)
    if args.offset is not None:
        offset = args.offset
    elif args.line is not None:
        index = diag.LineIndex(source)
        offset = index.offset_of(args.line, args.column or 1)
    else:
        print(
            "complete takes --offset N, or --line L and --column C",
            file=sys.stderr,
        )
        return 2

    payload = native_flow.complete(source, offset)
    if args.format in ("json", "sarif"):
        _emit({**payload, "source": name})
        return 0
    for proposal in payload["proposals"]:
        detail = f": {proposal['type']}" if proposal.get("type") else ""
        print(
            f"{proposal['kind']:<13} {proposal['name']}"
            f"{proposal.get('tail', '')}{detail}"
        )
    return 0


async def _run_serve(args: argparse.Namespace) -> int:
    """Answer language requests on standard input, one JSON object per line.

    The same methods `a11-flow serve --protocol json` speaks, from the same
    library -- this is here so a host that already has A11 installed need not find
    the binary. `--protocol lsp` is the standalone tool's: an editor wanting a
    language server should run `a11-flow`, which starts in milliseconds and does
    not import Python.
    """
    from a11._native import flow as native_flow

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except ValueError:
            print(
                json.dumps(
                    {"ok": False, "error": {"message": "That is not JSON."}}
                ),
                flush=True,
            )
            continue
        print(json.dumps(native_flow.request(request)), flush=True)
    return 0


def _input_value(text: str) -> Any:
    """One `name=value` input, with the value read as JSON where it is JSON.

    `--input limit=3` is the number three and `--input question=who?` is that
    string: a port that wants a number should not need quotes on a command line,
    and one that wants a string should not need escaping.
    """
    try:
        return json.loads(text)
    except ValueError:
        return text


async def _run_run(args: argparse.Namespace) -> int:
    """Run one flow of a file, here, and print what its ports produced.

    Everything a flow calls has to be registered in *this* process, so what this
    runs is a composition of actions the CLI itself has -- which is what makes it
    useful for trying a flow out and for a smoke test in a hook. A flow that calls
    a gateway's actions belongs on the gateway, dispatched as an action like any
    other.
    """
    from a11 import flow as flow_api
    from a11.flow.diagnostics import FlowSyntaxError
    from a11.status import StatusException

    source, name = _read(args.file)
    try:
        program = flow_api.loads(source, "" if name == _STDIN else name)
    except FlowSyntaxError as error:
        print(
            _syntax_error_diagnostic(error, source).as_text(
                "" if name == _STDIN else name
            ),
            file=sys.stderr,
        )
        return 1

    flows = [plan.name for plan in program]
    wanted = args.flow or (flows[0] if flows else "")
    if wanted not in flows:
        known = ", ".join(flows) or "none"
        print(
            f"{name} has no flow named {wanted!r} (declared: {known})",
            file=sys.stderr,
        )
        return 2

    inputs = {}
    for given in args.input or ():
        if "=" not in given:
            print(f"--input takes name=value, not {given!r}", file=sys.stderr)
            return 2
        key, _, value = given.partition("=")
        inputs[key] = _input_value(value)

    timeout = (
        None if args.timeout is None else timing.Duration.seconds(args.timeout)
    )
    try:
        produced = await program[wanted].invoke(inputs, timeout=timeout)
    except StatusException as error:
        print(f"{wanted}: {error.status.message}", file=sys.stderr)
        return 1

    if args.format in ("json", "sarif"):
        _emit(
            {
                "format": "flow.run/v1",
                "source": name,
                "flow": wanted,
                "outputs": produced,
            }
        )
        return 0
    for port, value in produced.items():
        if isinstance(value, list):
            for one in value:
                print(f"{port}: {one}")
        else:
            print(f"{port}: {value}")
    return 0


async def _run_codes(args: argparse.Namespace) -> int:
    if args.format in ("json", "sarif"):
        _emit(diag.codes_envelope())
        return 0
    width = max(len(entry.code) for entry in diag.known_codes())
    for entry in diag.known_codes():
        print(f"{entry.code:<{width}}  {entry.severity:<12}  {entry.summary}")
    return 0


async def _run(args: argparse.Namespace) -> int:
    """Dispatch to the subcommand, or print help when none was given."""
    handler = getattr(args, "_flow_handler", None)
    if handler is None:
        args._flow_parser.print_help()
        return 2
    return await handler(args)


def _add_format(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--format",
        choices=_FORMATS,
        default="text",
        help=(
            "How to write the result: 'text' for a person, 'json' for the"
            " versioned envelope other tools read, 'sarif' for CI annotators."
        ),
    )


def _configure(parser: argparse.ArgumentParser) -> None:
    parser.set_defaults(_flow_parser=parser, _flow_handler=None)
    subparsers = parser.add_subparsers(
        title="subcommands", metavar="<subcommand>", dest="subcommand"
    )

    check = subparsers.add_parser(
        "check",
        help="Compile flows and report what is wrong with them.",
        description=(
            "Compiles each file and reports every problem found in it. Exits 1"
            " when anything is an error, 2 when a file could not be read."
        ),
    )
    check.add_argument(
        "files",
        nargs="+",
        metavar="FILE",
        help="Flow files to check; '-' reads standard input.",
    )
    check.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Say nothing about the files that are fine.",
    )
    _add_format(check)
    check.set_defaults(_flow_handler=_run_check)

    describe = subparsers.add_parser(
        "describe",
        help="The resolved plan of a flow file: ports, headers, steps.",
        description=(
            "Compiles a flow file and prints the plan it resolved to -- what a"
            " caller sees, and what the runtime will do."
        ),
    )
    describe.add_argument(
        "file",
        metavar="FILE",
        help="Flow file to describe; '-' reads standard input.",
    )
    _add_format(describe)
    describe.set_defaults(_flow_handler=_run_describe)

    fmt = subparsers.add_parser(
        "fmt",
        help="Format flows: indentation, spacing, and the columns of a run of"
        " declarations.",
        description=(
            "Formats each file. Prints the result by default, rewrites the file"
            " with -i, and says what would change with --check. It decides"
            " indentation, the spaces between tokens, blank lines and the columns"
            " of a run of port or header declarations; it does not decide where"
            " the lines break, because that is a judgement about what belongs"
            " together and it stays yours. Exits 1 when something changed (or,"
            " with --check, needs to), 2 when a file could not be read or"
            " parsed."
        ),
    )
    fmt.add_argument(
        "files",
        nargs="+",
        metavar="FILE",
        help="Flow files to format; '-' reads standard input.",
    )
    fmt.add_argument(
        "-i",
        "--in-place",
        action="store_true",
        help="Rewrite each file rather than printing it.",
    )
    fmt.add_argument(
        "--check",
        action="store_true",
        help="Say what would change and exit 1, without writing anything.",
    )
    fmt.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Say nothing about the files that are already formatted.",
    )
    _add_format(fmt)
    fmt.set_defaults(_flow_handler=_run_fmt)

    parse = subparsers.add_parser(
        "parse",
        help="The syntax tree of a flow file, and what is wrong with it.",
        description=(
            "Parses a flow file and prints the tree it read. The parser recovers,"
            " so a file with a mistake in it still gives a tree: 'text' shows the"
            " outline with the problems on standard error, 'json' is the"
            " flow.syntax/v1 envelope with both. Exits 1 if anything is an error."
        ),
    )
    parse.add_argument(
        "file",
        metavar="FILE",
        help="Flow file to parse; '-' reads standard input.",
    )
    _add_format(parse)
    parse.set_defaults(_flow_handler=_run_parse)

    highlight = subparsers.add_parser(
        "highlight",
        help="What each token in a flow means, for a syntax highlighter.",
        description=(
            "Classifies every token the way a reader colours it: a stage after"
            " a '|', a type past a port's ':', a member after a '.', a function"
            " only where it is called. One implementation of that judgement,"
            " which is what an editor reads instead of writing its own lexer."
        ),
    )
    highlight.add_argument(
        "file",
        metavar="FILE",
        help="Flow file to classify; '-' reads standard input.",
    )
    _add_format(highlight)
    highlight.set_defaults(_flow_handler=_run_highlight)

    complete = subparsers.add_parser(
        "complete",
        help="What may be written at a position in a flow.",
        description=(
            "Everything the language allows at one offset, in the order it should"
            " be offered: after a '|' only a stage, past a port's ':' only a type,"
            " after 'x.' only what x has. Unfiltered -- whoever offers these"
            " filters by their own rules."
        ),
    )
    complete.add_argument(
        "file",
        metavar="FILE",
        help="Flow file to complete in; '-' reads standard input.",
    )
    complete.add_argument(
        "--offset", type=int, help="Byte offset of the caret."
    )
    complete.add_argument("--line", type=int, help="Line of the caret, 1-based.")
    complete.add_argument(
        "--column", type=int, help="Column of the caret, 1-based."
    )
    _add_format(complete)
    complete.set_defaults(_flow_handler=_run_complete)

    serve = subparsers.add_parser(
        "serve",
        help="Answer language requests on standard input.",
        description=(
            "One JSON request per line, one answer per line:"
            ' {"method": "check", "source": "flow t { }"}. The same methods the'
            " standalone `a11-flow serve` speaks, for a host that already has A11"
            " installed. An editor wanting a language server should run `a11-flow"
            " serve --protocol lsp`, which starts in milliseconds and imports no"
            " Python."
        ),
    )
    serve.add_argument(
        "--protocol",
        choices=("json",),
        default="json",
        help="What to speak; LSP is the standalone tool's.",
    )
    serve.set_defaults(_flow_handler=_run_serve)

    run = subparsers.add_parser(
        "run",
        help="Run a flow here and print what its ports produced.",
        description=(
            "Compiles the file, runs one of its flows in this process, and prints"
            " each output port. Everything the flow calls has to be registered"
            " here, so this runs a composition of the actions the CLI itself has:"
            " it is for trying a flow out, not for dispatching one at a gateway."
        ),
    )
    run.add_argument(
        "file",
        metavar="FILE",
        help="Flow file to run; '-' reads standard input.",
    )
    run.add_argument(
        "--flow",
        help="Which flow to run. The first one declared, by default.",
    )
    run.add_argument(
        "--input",
        action="append",
        metavar="NAME=VALUE",
        help=(
            "Fill an input port. The value is read as JSON where it is JSON, so"
            " --input limit=3 is a number and --input q=who? is a string."
        ),
    )
    run.add_argument(
        "--timeout",
        type=float,
        help="Seconds to wait for the flow before giving up.",
    )
    _add_format(run)
    run.set_defaults(_flow_handler=_run_run)

    syntax = subparsers.add_parser(
        "syntax",
        help="Generate the editor definitions, or check they are current.",
        description=(
            "The editor definitions the language writes for itself. A static"
            " grammar file is a copy of the language's word lists, so it is"
            " generated from the one table rather than maintained: --generate"
            " writes it, and the default checks it and exits 1 when it is out of"
            " date, which is what a CI job gates on."
        ),
    )
    syntax.add_argument(
        "--target",
        choices=(*_TARGET_NAMES, "all"),
        default="all",
        help="Which editor definition.",
    )
    syntax.add_argument(
        "--generate",
        action="store_true",
        help="Write each definition rather than checking it.",
    )
    syntax.add_argument(
        "--quiet",
        action="store_true",
        help="Say nothing about the definitions that are current.",
    )
    syntax.add_argument(
        "--root",
        default=".",
        help="Repository root the definitions are written under.",
    )
    _add_format(syntax)
    syntax.set_defaults(_flow_handler=_run_syntax)

    codes = subparsers.add_parser(
        "codes",
        help="Every diagnostic code the language publishes.",
        description=(
            "The published table of diagnostic codes, their families, their"
            " default severities and what each one means. A toolchain may match"
            " on a code: they are stable, and the wording of a message is not."
        ),
    )
    _add_format(codes)
    codes.set_defaults(_flow_handler=_run_codes)


FLOW_COMMAND = Command(
    name="flow",
    help="Check and describe Flow programs.",
    description=__doc__,
    run=_run,
    configure=_configure,
)

__all__ = ["FLOW_COMMAND"]

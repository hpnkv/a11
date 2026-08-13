#!/usr/bin/env python3
"""Generate or verify the PEP 561 stubs for A11's pybind11 module.

``a11._native`` is one compiled module with a submodule (``flow``), so the stubs
are a package -- `a11/_native/__init__.pyi` plus one file per submodule -- which
is what pybind11-stubgen writes and what resolves ``a11._native.flow`` as a
module for a type checker and an IDE.
"""

from __future__ import annotations

import argparse
import ast
import builtins
import importlib
import inspect
import re
import tempfile
import types
from pathlib import Path
from typing import NamedTuple

import a11._native as native
import black
from pybind11_stubgen import main as stubgen_main

ROOT = Path(__file__).resolve().parents[1]
#: The checked-in stub *package*: ``__init__.pyi`` for ``a11._native`` and one
#: file per submodule it exports, which is what pybind11-stubgen writes and what
#: a type checker or an IDE needs to resolve ``a11._native.flow`` as a module.
#: A directory of ``.pyi`` files beside ``_native.cpython-*.so`` does not shadow
#: the extension: it is only a namespace-package candidate, and a real module
#: wins.
STUB = ROOT / "a11" / "_native"


def _load_optional_public_protocols() -> None:
    """Attach facades that are not imported by the root ``a11`` package."""
    if hasattr(native, "AudioInput"):
        importlib.import_module("a11.sdk.audio.client")
    if hasattr(native, "flow"):
        # `a11.flow.plan` is what attaches the mapping, registration and
        # `invoke` conveniences onto the native `FlowPlan` and `Program`.
        importlib.import_module("a11.flow")


def _native_namespaces() -> list[types.ModuleType]:
    """``a11._native`` and every submodule it exports, such as ``flow``."""
    prefix = f"{native.__name__}."
    return [native] + [
        value
        for value in vars(native).values()
        if isinstance(value, types.ModuleType)
        and value.__name__.startswith(prefix)
    ]


class _ProtocolMethod(NamedTuple):
    """One attached Python method, as the stub has to render it."""

    #: ``async def`` in the source, which stubgen reads as a plain function.
    is_async: bool
    #: Parameters the source annotated ``object``. stubgen writes those out as
    #: ``typing.Any``, and the difference matters: a membership test annotated
    #: ``object`` answers for any value, which is what typeshed spells it with.
    object_parameters: frozenset[str]


def _protocol_method(member: types.FunctionType) -> _ProtocolMethod:
    annotated: set[str] = set()
    for name, parameter in inspect.signature(member).parameters.items():
        if parameter.annotation in (object, "object"):
            annotated.add(name)
    return _ProtocolMethod(
        is_async=inspect.iscoroutinefunction(member),
        object_parameters=frozenset(annotated),
    )


def _python_protocol_methods() -> dict[str, dict[str, _ProtocolMethod]]:
    """Return Python methods attached directly to bound native classes."""

    result: dict[str, dict[str, _ProtocolMethod]] = {}
    for module in _native_namespaces():
        for value in vars(module).values():
            if not isinstance(value, type):
                continue
            methods = result.setdefault(value.__name__, {})
            for name, member in vars(value).items():
                if isinstance(member, types.FunctionType):
                    methods[name] = _protocol_method(member)
    return result


def _expose_bound_classes_in_native_module() -> None:
    """Keep public aliases from turning generated classes into imports."""

    for module in _native_namespaces():
        for value in vars(module).values():
            if isinstance(value, type) and value.__module__.startswith("a11"):
                # A protocol attached from `a11.flow.plan` renames the class it
                # was attached to; stubgen would then emit an import of that
                # module instead of the class it is generating.
                value.__module__ = module.__name__


def _replace_first_argument(signature: str) -> str:
    start = signature.index("(") + 1
    depth = 0
    for index in range(start, len(signature)):
        character = signature[index]
        if character in "([{":
            depth += 1
        elif character in ")]}":
            if depth == 0:
                return signature[:start] + "self" + signature[index:]
            depth -= 1
        elif character == "," and depth == 0:
            return signature[:start] + "self" + signature[index:]
    raise RuntimeError(f"could not find first argument in {signature!r}")


def _normalise_protocol_methods(
    stub: str, methods: dict[str, dict[str, _ProtocolMethod]]
) -> str:
    """Correct stubgen's treatment of dynamically attached Python methods."""

    output: list[str] = []
    # (indent, name) per enclosing class, so a nested class is attributed to
    # itself rather than to the class it is written in.
    enclosing: list[tuple[int, str]] = []
    for line in stub.splitlines():
        stripped = line.lstrip()
        indent = len(line) - len(stripped)
        if stripped:
            while enclosing and indent <= enclosing[-1][0]:
                enclosing.pop()
        if stripped.startswith("class "):
            name = stripped.removeprefix("class ").split("(", 1)[0]
            enclosing.append((indent, name.removesuffix(":")))
        elif enclosing and stripped.startswith("def "):
            method_name = stripped.removeprefix("def ").split("(", 1)[0]
            class_methods = methods.get(enclosing[-1][1], {})
            method = class_methods.get(method_name)
            if method is not None:
                if output and output[-1].strip() == "@staticmethod":
                    output.pop()
                line = _replace_first_argument(line)
                for parameter in method.object_parameters:
                    line = line.replace(
                        f"{parameter}: typing.Any", f"{parameter}: object"
                    )
                if method.is_async:
                    line = f"{' ' * indent}async {line.lstrip()}"
        output.append(line)
    return "\n".join(output) + "\n"


# Readers whose result is decided by ``obj_type``. Their runtime annotation
# spells out every possibility; the stub narrows it to ``T | None`` and lets
# ``T`` default to ``Any``, so ``next(obj_type=Reading)`` is ``Reading | None``
# and a bare ``next()`` is ``Any | None``.
_OBJ_TYPE_READERS = ("consume", "next", "next_object")

_OBJ_TYPE_SIGNATURE = re.compile(
    r"(    async def (?:%s)\(self, obj_type: type\[T\] \| None = None"
    r"[^\n]*\) -> T) \| typing\.Any \| None:" % "|".join(_OBJ_TYPE_READERS)
)


def _narrow_obj_type_readers(stub: str) -> str:
    """Tie an ``obj_type``-driven reader's result to the type it was given."""
    stub, count = _OBJ_TYPE_SIGNATURE.subn(r"\1 | None:", stub)
    if count != len(_OBJ_TYPE_READERS):
        raise RuntimeError(
            f"expected {len(_OBJ_TYPE_READERS)} obj_type readers to narrow, "
            f"rewrote {count}"
        )
    return stub


# Readers that raise on an empty node unless ``allow_none`` says otherwise, so
# the default call can never return None.
_ALLOW_NONE_READERS = ("consume", "consume_chunk", "consume_fragment")

_ALLOW_NONE_SIGNATURE = re.compile(
    r"^    async def (?P<name>%s)\((?P<params>self.*?), "
    r"allow_none: bool = False\) -> (?P<returns>.+) \| None:$"
    % "|".join(_ALLOW_NONE_READERS)
)


def _take_docstring(lines: list[str], index: int) -> tuple[list[str], int]:
    """The docstring block starting at ``index``, and the line after it."""
    if index >= len(lines) or lines[index].strip() != '"""':
        return [], index
    block = [lines[index]]
    index += 1
    while index < len(lines) and lines[index].strip() != '"""':
        block.append(lines[index])
        index += 1
    if index < len(lines):
        block.append(lines[index])
        index += 1
    return block, index


def _dedent_docstrings(stub: str) -> str:
    """Align a docstring's body with its own first line.

    A docstring that came from an *attached* Python member -- a protocol method,
    or a property, which is what `attach_protocol` copies onto the native class
    -- reaches stubgen as a raw ``__doc__``: the summary dedented, every line
    after it still carrying the indentation of the class body it was written in.
    Markdown then reads the whole body as a code block, so the prose renders
    preformatted in the API reference. This puts the body back on the summary's
    indentation, keeping the *relative* indentation inside it (a nested list, an
    example fence) intact.
    """
    lines = stub.splitlines()
    output: list[str] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.strip() != '"""':
            output.append(line)
            index += 1
            continue
        block, index = _take_docstring(lines, index)
        indent = len(block[0]) - len(block[0].lstrip())
        body = block[1:-1]
        # The summary is the first body line and is already aligned; whatever
        # follows it is what may be over-indented, uniformly.
        rest = [entry for entry in body[1:] if entry.strip()]
        extra = min((len(e) - len(e.lstrip()) for e in rest), default=indent)
        if extra > indent:
            body = body[:1] + [
                entry[extra - indent :] if entry.strip() else entry
                for entry in body[1:]
            ]
        while body and not body[-1].strip():
            body.pop()
        output += [block[0], *body, block[-1]]
    return "\n".join(output) + "\n"


def _split_on_allow_none(stub: str) -> str:
    """Let ``allow_none`` decide whether a reader's result is optional.

    ``await node.consume(obj_type=Reading)`` cannot return None — the reader
    raises on an empty node — so the default overload drops it, and only
    ``allow_none=True`` widens. A third, plain-``bool`` overload keeps a
    computed flag type-checking, at the cost of the union.

    Runs on unformatted stubgen output, where a signature is a single line.
    """
    output: list[str] = []
    lines = stub.splitlines()
    rewritten = 0
    index = 0
    while index < len(lines):
        match = _ALLOW_NONE_SIGNATURE.match(lines[index])
        if match is None:
            output.append(lines[index])
            index += 1
            continue

        name = match.group("name")
        params = match.group("params")
        returns = match.group("returns")
        index += 1
        docstring, index = _take_docstring(lines, index)

        def signature(flag: str, result: str) -> str:
            return (
                f"    async def {name}({params}, allow_none: {flag})"
                f" -> {result}:"
            )

        # Every overload needs a default, since ``allow_none`` follows
        # defaulted parameters. The ``= True`` is never what an omitted
        # argument resolves to: the Literal[False] overload comes first.
        output.append("    @typing.overload")
        output.append(signature("typing.Literal[False] = False", returns))
        output.extend(docstring or ["        ..."])
        output.append("    @typing.overload")
        output.append(
            signature("typing.Literal[True] = True", f"{returns} | None")
            + " ..."
        )
        output.append("    @typing.overload")
        output.append(signature("bool = False", f"{returns} | None") + " ...")
        rewritten += 1

    if rewritten != len(_ALLOW_NONE_READERS):
        raise RuntimeError(
            f"expected {len(_ALLOW_NONE_READERS)} allow_none readers to "
            f"split, rewrote {rewritten}"
        )
    return "\n".join(output) + "\n"


_DECODE_SIGNATURE = re.compile(
    r"^    def (?P<name>get_header)\((?P<params>self.*?), "
    r"decode: bool = False\) -> bytes \| str \| None:$"
)


def _split_on_decode(stub: str) -> str:
    """Let ``decode`` decide whether a header reads back as str or bytes."""
    output: list[str] = []
    lines = stub.splitlines()
    rewritten = 0
    index = 0
    while index < len(lines):
        match = _DECODE_SIGNATURE.match(lines[index])
        if match is None:
            output.append(lines[index])
            index += 1
            continue

        name = match.group("name")
        params = match.group("params")
        index += 1
        docstring, index = _take_docstring(lines, index)

        def signature(flag: str, result: str) -> str:
            return f"    def {name}({params}, decode: {flag}) -> {result}:"

        output.append("    @typing.overload")
        output.append(
            signature("typing.Literal[False] = False", "bytes | None")
        )
        output.extend(docstring or ["        ..."])
        output.append("    @typing.overload")
        output.append(
            signature("typing.Literal[True] = True", "str | None") + " ..."
        )
        output.append("    @typing.overload")
        output.append(
            signature("bool = False", "bytes | str | None") + " ..."
        )
        rewritten += 1

    if rewritten != 1:
        raise RuntimeError(
            f"expected 1 decode-driven reader to split, rewrote {rewritten}"
        )
    return "\n".join(output) + "\n"


def _normalise_annotations(stub: str, submodule: str | None = None) -> str:
    """Resolve facade annotations and raw C++ names in generated output.

    ``submodule`` names the file's own module inside ``a11._native``, whose
    classes it refers to by bare name; the root stub is generated without one.
    """

    replacements = {
        # A submodule's own classes come out fully qualified
        # (``a11._native.flow.FlowPlan``), and in its own file the bare name is
        # what resolves. This has to come before the package prefix goes.
        **({f"a11._native.{submodule}.": ""} if submodule else {}),
        "a11._native.": "",
        "_NativeAsyncNode": "AsyncNode",
        "_NativeNodeMap": "NodeMap",
        "_NativeReader": "ChunkStoreReader",
        "_NativeWriter": "ChunkStoreWriter",
        "_native.WireStream": "WireStream",
        "_ActionDoneEvent": "_DoneEvent",
        "_SessionDoneEvent": "_DoneEvent",
        "StatusExceptionCasters": "a11.status.StatusExceptionCasters",
        # Raw C++ return/parameter types the bindings expose by name. The
        # ``registry: ...`` rule is broad on purpose: it resolves the named
        # ``registry`` parameter wherever it appears (Action.__init__,
        # bind_registry, set_action_registry) and, as a tail match, the
        # ``action_registry: ...`` constructor parameters too.
        "registry: ...": "registry: ActionRegistry | None",
        "get_registry(self) -> ...": "get_registry(self) -> ActionRegistry",
        "expected: ... = None) -> ...": (
            "expected: AsyncNode | None = None) -> AsyncNode | None"
        ),
        "get(self, node_id: str) -> ...": (
            "get(self, node_id: str) -> AsyncNode"
        ),
        "get_if_exists(self, node_id: str) -> ...": (
            "get_if_exists(self, node_id: str) -> AsyncNode | None"
        ),
        "actions(self) -> list[tuple[str, ...]]": (
            "actions(self) -> list[tuple[str, Action]]"
        ),
        "get_action(self, action_id: str) -> ...": (
            "get_action(self, action_id: str) -> Action"
        ),
        "get_action_registry(self) -> ...": (
            "get_action_registry(self) -> ActionRegistry | None"
        ),
        "action_registry(self) -> ...": (
            "action_registry(self) -> ActionRegistry | None"
        ),
        "action_registry(self, arg1: ...)": (
            "action_registry(self, arg1: ActionRegistry)"
        ),
        "http2_options(self) -> ...": "http2_options(self) -> Http2Options",
        "http2_options(self, arg0: ...)": (
            "http2_options(self, arg0: Http2Options)"
        ),
    }
    for old, new in replacements.items():
        stub = stub.replace(old, new)

    # The bare alias resolves to its callable form, but only as a whole word:
    # a plain substring replace would also rewrite the tail of concrete class
    # names such as ``SQLiteChunkStoreFactory``.
    stub = re.sub(
        r"(?<![\w.])ChunkStoreFactory\b",
        "typing.Callable[[str], ChunkStore]",
        stub,
    )

    # Shorten the module-qualified facade types in *annotations* to their bare
    # names, but only when not already part of a longer dotted path (a
    # docstring cross-reference such as ``a11.data.types.Chunk`` must survive).
    for module, name in (
        ("types", "Chunk"),
        ("types", "NodeFragment"),
        ("timing", "Duration"),
        ("timing", "Time"),
    ):
        stub = re.sub(rf"(?<![.\w]){module}\.{name}\b", name, stub)

    stub = re.sub(r"\s+# value = .*?$", "", stub, flags=re.MULTILINE)
    # Runtime version comes from the root VERSION file. Keep generated typing
    # independent of the particular extension used to run stubgen.
    stub = re.sub(
        r'^__version__: str = ["\'][^"\']+["\']$',
        "__version__: str",
        stub,
        flags=re.MULTILINE,
    )
    # A comparison answers for *any* value rather than raising, which is what
    # `object` says and what typeshed spells both dunders with. A membership
    # test is not swept up with them: several of these take a `std::string` and
    # do raise, and only the attached Python ones promise more (see
    # [_ProtocolMethod]).
    stub = re.sub(
        r"def (__eq__|__ne__)\(self, (\w+): [^)]+\) -> bool:",
        r"def \1(self, \2: object) -> bool:",
        stub,
    )
    stub = re.sub(
        r": ([A-Za-z_][\w.]*(?:\[[^\n,=]+\])?) = None",
        r": \1 | None = None",
        stub,
    )
    stub = stub.replace(
        "__hash__: typing.ClassVar[None] | None = None",
        "__hash__: None = None",
    )

    stub = re.sub(r"(?<![.\w])Mapping(?=\[)", "collections.abc.Mapping", stub)
    stub = re.sub(
        r"(?<![.\w])AsyncIterator(?=\[)",
        "collections.abc.AsyncIterator",
        stub,
    )
    stub = re.sub(r"(?<![.\w])Self\b", "typing.Self", stub)

    if submodule is not None:
        return _check_annotations(stub, f"{submodule}.pyi")

    definitions = (
        # PEP 696 default: a reader called without an ``obj_type`` returns
        # whatever the chunk's own tag decides, which is ``Any``.
        'T = typing_extensions.TypeVar("T", default=typing.Any)\n'
        "class _DoneEvent(typing.Protocol):\n"
        "    def is_set(self) -> bool:\n"
        "        ...\n"
        "    async def wait(self) -> bool:\n"
        "        ...\n"
    )
    if "ActionHandler | NativeActionHandler" in stub:
        # The Python half of what a handler can be; the bindings name the
        # native half themselves (see PyActionHandler in actions_bindings.cc).
        definitions += (
            "ActionHandler = collections.abc.Callable["
            '["Action"], collections.abc.Awaitable[None]]\n'
        )
    if "OnTranscription" in stub:
        definitions += (
            "OnTranscription = collections.abc.Callable["
            "[str | None], collections.abc.Awaitable[None]]\n"
            "OnRecognitionDone = collections.abc.Callable["
            "[], collections.abc.Awaitable[None]]\n"
        )
    marker = "import typing\n"
    if marker not in stub:
        raise RuntimeError("stubgen output did not import typing")
    stub = stub.replace(marker, marker + definitions, 1)
    if "fastapi." in stub:
        stub = stub.replace(
            "import asyncio\n", "import asyncio\nimport fastapi\n"
        )
    if "httpx." in stub:
        stub = stub.replace(
            "import fastapi\n", "import fastapi\nimport httpx\n"
        )
    if "os.PathLike" in stub:
        stub = stub.replace("import typing\n", "import typing\nimport os\n", 1)
    if "typing_extensions." in stub or "typing_extensions." in definitions:
        stub = stub.replace(
            "import typing\n", "import typing\nimport typing_extensions\n", 1
        )

    return _check_annotations(stub, "__init__.pyi")


def _module_level_names(stub: str) -> set[str]:
    """Every name a stub file defines or imports at module level."""
    names: set[str] = set()
    for node in ast.parse(stub).body:
        if isinstance(
            node, ast.ClassDef | ast.FunctionDef | ast.AsyncFunctionDef
        ):
            names.add(node.name)
        elif isinstance(node, ast.Assign):
            names.update(
                target.id
                for target in node.targets
                if isinstance(target, ast.Name)
            )
        elif isinstance(node, ast.AnnAssign) and isinstance(
            node.target, ast.Name
        ):
            names.add(node.target.id)
        elif isinstance(node, ast.Import | ast.ImportFrom):
            names.update(
                (alias.asname or alias.name).split(".")[0]
                for alias in node.names
            )
    return names


def _import_parent_names(stub: str, root: str) -> str:
    """Import what a submodule stub names but the root module defines.

    A submodule's signatures refer to the parent module's types by bare name --
    ``make_handler`` answers an ``ActionHandler`` -- because at runtime they are
    one extension. In a stub package each file resolves its own names, so the
    ones that come from the root are imported from it.
    """
    defined = _module_level_names(stub) | set(dir(builtins))
    available = _module_level_names(root)
    wanted = sorted(
        {
            node.id
            for node in ast.walk(ast.parse(stub))
            if isinstance(node, ast.Name)
            and node.id not in defined
            and node.id in available
        }
    )
    if not wanted:
        return stub
    imports = f"from a11._native import {', '.join(wanted)}\n"
    header = re.search(r"^import [^\n]+\n(?!import )", stub, flags=re.MULTILINE)
    if header is None:
        raise RuntimeError("submodule stub has no imports to follow")
    return stub[: header.end()] + imports + stub[header.end() :]


def _check_annotations(stub: str, name: str) -> str:
    """Refuse a stub that still carries a type stubgen could not work out."""
    unresolved_type = re.search(r"(?:: \.\.\.(?:\s*=)?|-> \.\.\.)", stub)
    if unresolved_type is not None:
        line = stub.count("\n", 0, unresolved_type.start()) + 1
        raise RuntimeError(f"unresolved generated type on {name} line {line}")
    ast.parse(stub, filename=str(STUB / name))
    return stub


def _normalise_stub(
    path: Path,
    methods: dict[str, dict[str, bool]],
    submodule: str | None = None,
) -> None:
    """Rewrite one generated file in place, as the checked-in stub has it.

    The overload splitting and the narrowing below are about readers of the root
    module and count what they rewrote, so they run on that file only;
    ``submodule`` says the file is a submodule's and takes the shared half.
    """
    stub = path.read_text()
    stub = _normalise_protocol_methods(stub, methods)
    stub = _dedent_docstrings(stub)
    stub = _normalise_annotations(stub, submodule)
    if submodule is None:
        stub = _narrow_obj_type_readers(stub)
        stub = _split_on_allow_none(stub)
        stub = _split_on_decode(stub)
    else:
        root = (path.parent / "__init__.pyi").read_text()
        stub = _import_parent_names(stub, root)
    mode = black.Mode(
        target_versions={black.TargetVersion.PY311},
        line_length=80,
        is_pyi=True,
        preview=True,
        enabled_features={black.Preview.string_processing},
    )
    try:
        stub = black.format_file_contents(stub, fast=False, mode=mode)
    except black.NothingChanged:
        pass
    stub = stub.replace(
        "    __hash__: None = None",
        "    __hash__: None = None  "
        "# pyright: ignore[reportIncompatibleMethodOverride]",
    )
    for method in ("accept", "start"):
        stub = stub.replace(
            f"    def {method}(self) -> typing.Any: ...",
            f"    def {method}(self) -> typing.Any: ...  "
            "# type: ignore[override]",
            1,
        )
    path.write_text(stub)


def _generate(output_dir: Path) -> Path:
    _load_optional_public_protocols()
    protocol_methods = _python_protocol_methods()
    _expose_bound_classes_in_native_module()
    stubgen_main([
        native.__name__,
        "--output-dir",
        str(output_dir),
        "--ignore-invalid-expressions",
        r"^(?:a11::|<).*$",
        "--ignore-unresolved-names",
        r".*",
        "--exit-code",
    ])
    package = output_dir / "a11" / "_native"
    _normalise_stub(package / "__init__.pyi", protocol_methods)
    written = {"__init__.pyi"}
    for path in sorted(package.glob("*.pyi")):
        if path.name in written:
            continue
        _normalise_stub(path, protocol_methods, path.stem)
        written.add(path.name)
    # A submodule that has gone leaves its file behind, which would then be a
    # stub for a module that no longer exists.
    for path in sorted(package.iterdir()):
        if path.name not in written:
            path.unlink()
    return package


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if the checked-in stubs differ from generated output",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT,
        help="output root passed to pybind11-stubgen",
    )
    args = parser.parse_args()

    if not args.check:
        generated = _generate(args.output_dir.resolve())
        print(generated)
        return

    with tempfile.TemporaryDirectory(prefix="a11-stubs-") as temporary:
        generated = _generate(Path(temporary))
        expected = {
            path.name: path.read_bytes() for path in generated.iterdir()
        }
        checked_in = (
            {path.name: path.read_bytes() for path in STUB.iterdir()}
            if STUB.is_dir()
            else {}
        )
        if expected != checked_in:
            raise SystemExit(
                "a11/_native/ is stale; run scripts/generate_stubs.py"
            )


if __name__ == "__main__":
    main()

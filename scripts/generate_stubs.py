#!/usr/bin/env python3
"""Generate or verify the PEP 561 stub for A11's pybind11 module."""

from __future__ import annotations

import argparse
import ast
import importlib
import inspect
import re
import tempfile
import types
from pathlib import Path

import a11._native as native
import black
from pybind11_stubgen import main as stubgen_main

ROOT = Path(__file__).resolve().parents[1]
STUB = ROOT / "a11" / "_native.pyi"


def _load_optional_public_protocols() -> None:
    """Attach facades that are not imported by the root ``a11`` package."""
    if hasattr(native, "AudioInput"):
        importlib.import_module("a11.sdk.audio.client")


def _python_protocol_methods() -> dict[str, dict[str, bool]]:
    """Return Python methods attached directly to bound native classes."""

    result: dict[str, dict[str, bool]] = {}
    for value in vars(native).values():
        if not isinstance(value, type):
            continue
        methods = result.setdefault(value.__name__, {})
        for name, member in vars(value).items():
            if isinstance(member, types.FunctionType):
                methods[name] = inspect.iscoroutinefunction(member)
    return result


def _expose_bound_classes_in_native_module() -> None:
    """Keep public aliases from turning generated classes into imports."""

    for value in vars(native).values():
        if isinstance(value, type) and value.__module__.startswith("a11"):
            value.__module__ = native.__name__


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
    stub: str, methods: dict[str, dict[str, bool]]
) -> str:
    """Correct stubgen's treatment of dynamically attached Python methods."""

    output: list[str] = []
    current_class: str | None = None
    for line in stub.splitlines():
        if line.startswith("class "):
            current_class = line.removeprefix("class ").split("(", 1)[0]
            current_class = current_class.removesuffix(":")
        elif line and not line[0].isspace():
            current_class = None

        prefix = "    def "
        if current_class is not None and line.startswith(prefix):
            method_name = line[len(prefix) :].split("(", 1)[0]
            class_methods = methods.get(current_class, {})
            if method_name in class_methods:
                if output and output[-1] == "    @staticmethod":
                    output.pop()
                line = _replace_first_argument(line)
                if class_methods[method_name]:
                    line = line.replace(prefix, "    async def ", 1)
        output.append(line)
    return "\n".join(output) + "\n"


def _normalise_annotations(stub: str) -> str:
    """Resolve facade annotations and raw C++ names in generated output."""

    replacements = {
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
    stub = re.sub(
        r"def __eq__\(self, (\w+): [^)]+\) -> bool:",
        r"def __eq__(self, \1: object) -> bool:",
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

    definitions = (
        'T = typing.TypeVar("T")\n'
        "class _DoneEvent(typing.Protocol):\n"
        "    def is_set(self) -> bool:\n"
        "        ...\n"
        "    async def wait(self) -> bool:\n"
        "        ...\n"
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

    unresolved_type = re.search(r"(?:: \.\.\.(?:\s*=)?|-> \.\.\.)", stub)
    if unresolved_type is not None:
        line = stub.count("\n", 0, unresolved_type.start()) + 1
        raise RuntimeError(f"unresolved generated type on line {line}")
    ast.parse(stub, filename=str(STUB))
    return stub


def _normalise_stub(path: Path, methods: dict[str, dict[str, bool]]) -> None:
    stub = path.read_text()
    stub = _normalise_protocol_methods(stub, methods)
    stub = _normalise_annotations(stub)
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
    stubgen_main(
        [
            native.__name__,
            "--output-dir",
            str(output_dir),
            "--ignore-invalid-expressions",
            r"^(?:a11::|<).*$",
            "--ignore-unresolved-names",
            r".*",
            "--exit-code",
        ]
    )
    generated = output_dir / "a11" / "_native.pyi"
    _normalise_stub(generated, protocol_methods)
    return generated


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if the checked-in stub differs from generated output",
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
        if not STUB.exists() or generated.read_bytes() != STUB.read_bytes():
            raise SystemExit(
                "a11/_native.pyi is stale; run scripts/generate_stubs.py"
            )


if __name__ == "__main__":
    main()

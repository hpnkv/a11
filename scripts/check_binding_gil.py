#!/usr/bin/env python3
# Copyright 2026 The A11 Authors.

"""Report bindings that hold the GIL across a call into the fiber runtime.

A11's fiber primitives can block the calling thread: a contended
`thread::Mutex`, an inline `Submit`, or a `Future::OnReady` racing completion.
A Python thread that retains the GIL during that park blocks fibers that need
the GIL.

A binding releases the GIL around a native call that can reach the fiber
runtime. This checker recognizes the release helpers in cpp/python/interop.h,
estimates native-call reachability, and prints unaccounted bindings.

    scripts/check_binding_gil.py [repository root]

Exits 1 when a binding is unaccounted for. `ACCOUNTED` below carries the ones
whose native call cannot park, with the reason.

The estimate resolves by receiver type and one hop within that type. Shared
helpers, destructors, and longer call chains remain outside the model. A clean
run means no binding matches the modeled shape.
"""

from __future__ import annotations

import collections
import pathlib
import re
import sys

#: Wrappers in cpp/python/interop.h that drop the GIL for the native call.
RELEASERS = (
    "WithoutGil",
    "CallWithoutGil",
    "ValueWithoutGil",
    "gil_scoped_release",
    "call_guard",
)

#: Entering the fiber runtime. A thread here can end up parked in the fiber
#: scheduler, whether or not it is a fiber itself.
ENTERS = re.compile(
    r"\b(?:a11::)?(SubmitTask|SubmitWithCancellationHook|Submit"
    r"|ScheduleCancelable|Schedule|NewTree)\s*[(<]"
    r"|\.Await\s*\(|->Await\s*\(|thread::Select\s*\("
)

#: Bindings allowed to retain the GIL because their native calls cannot park.
#: Keys have the form "file: Class.binding".
ACCOUNTED: dict[str, str] = {}

CLASS_DECL = re.compile(
    r"py::(?:class_|classh)<\s*([A-Za-z_][A-Za-z0-9_:<>, ]*?)\s*(?:,[^>]*)?>"
)
DEF_OPEN = re.compile(r'\.def(?:_static)?\(\s*\n?\s*"([^"]+)"')
NATIVE_CALL = re.compile(r"\bself(?:->|\.)([A-Z][A-Za-z0-9_]*)\s*\(")
FACTORY_CALL = re.compile(r"\b([A-Z][A-Za-z0-9_]*)::([A-Z][A-Za-z0-9_]*)\s*\(")
DEFINITION = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)\s*\("
    r"[^;{)]*\)?[^;{]*\{"
)
SAME_CLASS_CALL = re.compile(r"(?<![:\w])([A-Z][A-Za-z0-9_]*)\s*\(")


def balanced(
    text: str, start: int, opens: str = "([{", closes: str = ")]}"
) -> str:
    """The text from `start` to the close matching the bracket before it."""
    depth, index = 1, start
    while index < len(text) and depth:
        depth += (text[index] in opens) - (text[index] in closes)
        index += 1
    return text[start:index]


def index_methods(root: pathlib.Path) -> dict[str, dict[str, str]]:
    """Method bodies by owning class, from A11's own sources."""
    sources = [
        *(root / "cpp/a11").rglob("*.cc"),
        *(root / "cpp/a11").rglob("*.h"),
        *(root / "cpp/thread").rglob("*.cc"),
    ]
    methods: dict[str, dict[str, str]] = collections.defaultdict(dict)
    for path in sources:
        text = path.read_text(errors="ignore")
        for match in DEFINITION.finditer(text):
            owner, method = match.group(1), match.group(2)
            methods[owner].setdefault(
                method, balanced(text, match.end(), "{", "}")
            )
    return methods


def enters_runtime(
    methods: dict[str, dict[str, str]], owner: str, method: str, hops: int = 1
) -> str | None:
    """A "Class::Method -> ..." trail when the call can park, else None."""
    body = methods.get(owner, {}).get(method)
    if body is None:
        return None
    if ENTERS.search(body):
        return f"{owner}::{method}"
    if hops:
        for callee in sorted(set(SAME_CLASS_CALL.findall(body))):
            if callee == method:
                continue
            deeper = enters_runtime(methods, owner, callee, hops - 1)
            if deeper:
                return f"{owner}::{method} -> {deeper}"
    return None


def audit(root: pathlib.Path) -> list[tuple[str, str, str, str]]:
    """(file, class, binding, trail) for each unaccounted binding."""
    methods = index_methods(root)
    findings = []
    for path in sorted((root / "cpp/python").glob("*_bindings.cc")):
        text = path.read_text()
        declarations = [
            (
                match.start(),
                match.group(1).split("<")[0].split("::")[-1].strip(),
            )
            for match in CLASS_DECL.finditer(text)
        ]
        for match in DEF_OPEN.finditer(text):
            body = balanced(text, match.end())
            if any(token in body for token in RELEASERS):
                continue
            owner = ""
            for position, name in declarations:
                if position < match.start():
                    owner = name
            calls = {(owner, name) for name in NATIVE_CALL.findall(body)}
            calls |= set(FACTORY_CALL.findall(body))
            for receiver, name in sorted(calls):
                trail = enters_runtime(methods, receiver, name)
                if trail is None:
                    continue
                if f"{path.name}: {owner}.{match.group(1)}" in ACCOUNTED:
                    break
                findings.append((path.name, owner, match.group(1), trail))
                break
    return findings


def main(argv: list[str]) -> int:
    root = pathlib.Path(argv[1] if len(argv) > 1 else ".")
    findings = audit(root)
    if not findings:
        print("No binding holds the GIL into the fiber runtime.")
        return 0
    print(f"{len(findings)} binding(s) hold the GIL into the fiber runtime:\n")
    for path, owner, name, trail in findings:
        print(f"  {path}: {owner}.{name}")
        print(f"      reaches {trail}")
    print(
        "\nWrap the native call in WithoutGil / CallWithoutGil /"
        " ValueWithoutGil (cpp/python/interop.h), or add the binding to"
        " ACCOUNTED in this script with the reason it cannot park."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Fail a wheel test when native artifacts have non-system dynamic deps."""

from __future__ import annotations

import ast
import importlib
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

_LINUX_SYSTEM_LIBRARIES = re.compile(
    r"^(lib(c|m|pthread|rt|dl|util|resolv|stdc\+\+|gcc_s|z)\.so(?:\..*)?"
    r"|ld-linux[^/]*\.so(?:\..*)?)$"
)


def _output(*command: str) -> str:
    return subprocess.run(
        command, check=True, text=True, stdout=subprocess.PIPE
    ).stdout


def _audit_macos(binary: Path) -> None:
    dependencies = _output("otool", "-L", str(binary)).splitlines()[1:]
    for line in dependencies:
        dependency = line.strip().split(" (", 1)[0]
        if dependency.startswith(("/usr/lib/", "/System/Library/")):
            continue
        raise RuntimeError(f"non-system Mach-O dependency: {dependency}")

    load_commands = _output("otool", "-l", str(binary)).splitlines()
    for index, line in enumerate(load_commands):
        if line.strip() == "cmd LC_RPATH":
            path = load_commands[index + 2].strip().split(" ", 1)[1]
            if not path.startswith(("@loader_path", "@executable_path")):
                raise RuntimeError(f"non-relocatable LC_RPATH: {path}")


def _audit_linux(binary: Path) -> None:
    dynamic = _output("readelf", "-d", str(binary))
    for dependency in re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic):
        if not _LINUX_SYSTEM_LIBRARIES.match(dependency):
            raise RuntimeError(f"non-system ELF dependency: {dependency}")
    for value in re.findall(r"\((?:RPATH|RUNPATH)\).*?\[(.*?)\]", dynamic):
        for path in value.split(":"):
            if path and not path.startswith("$ORIGIN"):
                raise RuntimeError(f"non-relocatable ELF loader path: {path}")


def _audit_typing_files(root: Path) -> None:
    marker = root / "a11" / "py.typed"
    stub = root / "a11" / "_native.pyi"
    if not marker.is_file():
        raise RuntimeError("wheel does not contain a11/py.typed")
    if not stub.is_file():
        raise RuntimeError("wheel does not contain a11/_native.pyi")
    source = stub.read_text()
    ast.parse(source, filename=str(stub))
    for declaration in (
        "class Http2Client:",
        "class Http2Server:",
        "class WebRtcWireStream(WireStream):",
        "class WebSocketSignallingServer:",
        "class WebSocketWireStream(WireStream):",
    ):
        if declaration not in source:
            raise RuntimeError(f"native stub is missing {declaration}")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: audit_wheel.py WHEEL")
    wheel = Path(sys.argv[1]).resolve()
    if "universal2" in wheel.name:
        raise RuntimeError("Boost.Context wheels must be architecture-specific")

    with tempfile.TemporaryDirectory() as directory:
        with zipfile.ZipFile(wheel) as archive:
            archive.extractall(directory)
        _audit_typing_files(Path(directory))
        binaries = [
            path
            for path in Path(directory).rglob("*")
            if path.suffix in {".so", ".dylib", ".pyd"}
        ]
        native_binaries = [
            path
            for path in binaries
            if path.parent.name == "a11" and path.name.startswith("_native")
        ]
        if len(native_binaries) != 1:
            raise RuntimeError(
                "wheel must contain exactly one ABI-specific a11/_native; "
                f"found {len(native_binaries)}"
            )
        for binary in binaries:
            if sys.platform == "darwin":
                _audit_macos(binary)
            elif sys.platform.startswith("linux"):
                _audit_linux(binary)

    importlib.import_module("a11._native")
    importlib.import_module("pybind11_abseil.status")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build A11's per-architecture wheel matrix with cibuildwheel."""

from __future__ import annotations

import argparse
import os
import platform
import shlex
import subprocess
import sys
from pathlib import Path


def _platforms(requested: list[str] | None) -> list[str]:
    if requested:
        return requested
    if sys.platform == "darwin":
        return ["macos", "linux"]
    if sys.platform.startswith("linux"):
        return ["linux"]
    raise SystemExit("Automatic wheel builds currently support macOS and Linux")


def _architectures(target: str) -> tuple[str, ...]:
    if target == "macos":
        return ("x86_64", "arm64")
    return ("x86_64", "aarch64")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--platform",
        action="append",
        choices=("macos", "linux"),
        help="platform to build; defaults to host-required platforms",
    )
    parser.add_argument("--output-dir", default="dist")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    output = (root / args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    host_arch = platform.machine().lower().replace("amd64", "x86_64")

    for target in _platforms(args.platform):
        for arch in _architectures(target):
            prefix = f"/tmp/a11-wheel-deps-{target}-{arch}"
            if target == "linux":
                prefix = f"/opt/a11-wheel-deps-{arch}"
            build_environment = [
                f"A11_DEPS_PREFIX={shlex.quote(prefix)}",
                f"A11_WHEEL_ARCH={shlex.quote(arch)}",
                f"CMAKE_PREFIX_PATH={shlex.quote(prefix)}",
                f"OPENSSL_ROOT_DIR={shlex.quote(prefix)}",
                "PKG_CONFIG_PATH=" + shlex.quote(f"{prefix}/lib/pkgconfig"),
                "CMAKE_ARGS="
                + shlex.quote(
                    " ".join(
                        (
                            "-DA11_REQUIRE_STATIC_DEPS=ON",
                            "-DA11_FETCH_MISSING_DEPS=ON",
                            f"-DCMAKE_PREFIX_PATH={prefix}",
                            f"-DOPENSSL_ROOT_DIR={prefix}",
                        )
                    )
                ),
            ]
            if target == "macos":
                build_environment.append("MACOSX_DEPLOYMENT_TARGET=13.0")
            env = os.environ.copy()
            env.update(
                {
                    "A11_DEPS_PREFIX": prefix,
                    "A11_WHEEL_ARCH": arch,
                    "CIBW_ENVIRONMENT": " ".join(build_environment),
                }
            )
            if target == "macos":
                env["MACOSX_DEPLOYMENT_TARGET"] = "13.0"
            if target == "macos" and arch != host_arch:
                env["CIBW_TEST_SKIP"] = f"*-macosx_{arch}"
            command = (
                sys.executable,
                "-m",
                "cibuildwheel",
                "--platform",
                target,
                "--archs",
                arch,
                "--output-dir",
                str(output),
            )
            subprocess.run(command, cwd=root, env=env, check=True)


if __name__ == "__main__":
    main()

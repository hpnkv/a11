#!/usr/bin/env python3
"""Build A11's per-architecture wheel matrix with cibuildwheel.

The default builds only the host operating system's wheels, native
architecture first, so the common case is fast and every wheel it produces can
be audited on the spot. Building the other operating system (Linux from macOS)
is opt-in via ``--platform linux`` or ``--platform all`` because it needs Docker
and, for a non-native architecture, slow QEMU emulation.

Dependency discovery is single-sourced: this script exports ``A11_DEPS_PREFIX``
and ``A11_WHEEL_ARCH`` (plus ``MACOSX_DEPLOYMENT_TARGET`` on macOS), and
``pyproject.toml``'s ``[tool.cibuildwheel] environment`` derives
``CMAKE_PREFIX_PATH``/``OPENSSL_ROOT_DIR``/``PKG_CONFIG_PATH``/``CMAKE_ARGS``
from that prefix. ``scripts/bootstrap_wheel_deps.sh`` (cibuildwheel's
``before-all``) builds the static dependencies into the prefix and fails loudly
if it is unset.
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

# Framework directory that cibuildwheel expects each targeted macOS CPython in.
MACOS_FRAMEWORKS = Path("/Library/Frameworks/Python.framework/Versions")


def _host_os() -> str:
    if sys.platform == "darwin":
        return "macos"
    if sys.platform.startswith("linux"):
        return "linux"
    raise SystemExit("Automatic wheel builds currently support macOS and Linux")


def _host_arch() -> str:
    return platform.machine().lower().replace("amd64", "x86_64")


def _canon_arch(arch: str) -> str:
    """Canonical name so macOS 'arm64' and Linux 'aarch64' compare equal."""
    arch = arch.lower()
    if arch in ("amd64", "x86_64"):
        return "x86_64"
    if arch in ("arm64", "aarch64"):
        return "aarch64"
    return arch


def _platforms(requested: list[str] | None) -> list[str]:
    """Resolve the requested platforms; default to the host OS only."""
    if not requested:
        return [_host_os()]
    if "all" in requested:
        return ["macos", "linux"]
    # Preserve request order while removing duplicates.
    return list(dict.fromkeys(requested))


def _architectures(target: str, host_arch: str) -> tuple[str, ...]:
    """Architectures for a target, host architecture first (fail fast)."""
    if target == "macos":
        arches = ["arm64", "x86_64"]
    else:
        arches = ["x86_64", "aarch64"]
    arches.sort(key=lambda arch: _canon_arch(arch) != _canon_arch(host_arch))
    return tuple(arches)


def _rosetta_available() -> bool:
    """True when x86_64 binaries can run on this host (native or via Rosetta)."""
    if _host_arch() == "x86_64":
        return True
    if sys.platform != "darwin":
        return False
    try:
        subprocess.run(
            ["arch", "-x86_64", "/usr/bin/true"],
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    return True


def _wheel_is_runnable(target: str, arch: str, host_arch: str) -> bool:
    """Whether a freshly built wheel can execute (be audited) on this host."""
    if target == "linux":
        # cibuildwheel runs the audit inside the build container (emulated when
        # the architecture is not native), so it always runs.
        return True
    if _canon_arch(arch) == _canon_arch(host_arch):
        return True
    # The only cross case we can execute is an x86_64 wheel on Apple silicon
    # with Rosetta 2 present. arm64 wheels cannot run on an x86_64 host.
    return _canon_arch(arch) == "x86_64" and _rosetta_available()


def _check_container_engine() -> None:
    """Fail early and clearly when Linux builds have no working Docker."""
    engine = "docker"
    if shutil.which(engine) is None:
        raise SystemExit(
            "Linux wheels need Docker, but the 'docker' command was not found. "
            "Install Docker Desktop (or Colima) and start it, or build Linux "
            "wheels on a Linux host."
        )
    probe = subprocess.run([engine, "info"], capture_output=True, text=True)
    if probe.returncode != 0:
        raise SystemExit(
            "Linux wheels need a running Docker daemon, but 'docker info' "
            "failed. Start Docker and retry.\n"
            f"docker info said: {probe.stderr.strip() or probe.stdout.strip()}"
        )


def _targeted_macos_versions(root: Path, env: dict[str, str]) -> list[str]:
    """CPython X.Y versions the matrix targets on macOS, from cibuildwheel."""
    result = subprocess.run(
        (
            sys.executable,
            "-m",
            "cibuildwheel",
            "--print-build-identifiers",
            "--platform",
            "macos",
        ),
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
        check=True,
    )
    versions: list[str] = []
    for identifier in result.stdout.split():
        tag = identifier.split("-", 1)[0]  # e.g. "cp312"
        if tag.startswith("cp") and tag[2:].isdigit() and len(tag) >= 4:
            version = f"{tag[2]}.{tag[3:]}"
            if version not in versions:
                versions.append(version)
    return versions


def _missing_macos_frameworks(versions: list[str]) -> list[str]:
    return [v for v in versions if not (MACOS_FRAMEWORKS / v).is_dir()]


def _skip_pattern(versions: list[str]) -> str:
    """A CIBW_SKIP pattern excluding the given CPython versions on macOS."""
    return " ".join(f"cp{v.replace('.', '')}-macosx_*" for v in versions)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--platform",
        action="append",
        choices=("macos", "linux", "all"),
        help=(
            "platform(s) to build; defaults to the host OS only. Use 'all' for "
            "the full macOS+Linux release matrix (Linux needs Docker)."
        ),
    )
    parser.add_argument("--output-dir", default="dist")
    args = parser.parse_args()

    host_os = _host_os()
    host_arch = _host_arch()
    targets = _platforms(args.platform)

    if "macos" in targets and host_os != "macos":
        raise SystemExit("macOS wheels can only be built on macOS.")

    root = Path(__file__).resolve().parents[1]
    output = (root / args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)

    # Preflight every target once, before any lengthy dependency bootstrap, so
    # missing prerequisites surface immediately rather than mid-build.
    macos_skip = ""
    for target in targets:
        if target == "linux":
            _check_container_engine()
            non_native = [
                a
                for a in _architectures(target, host_arch)
                if _canon_arch(a) != _canon_arch(host_arch)
            ]
            if non_native:
                print(
                    "note: building non-native Linux "
                    f"{', '.join(non_native)} needs QEMU/binfmt and is slow.",
                    file=sys.stderr,
                )
        if target == "macos":
            versions = _targeted_macos_versions(root, os.environ.copy())
            missing = _missing_macos_frameworks(versions)
            if missing:
                macos_skip = _skip_pattern(missing)
                print(
                    "note: skipping macOS CPython "
                    f"{', '.join(missing)} (no python.org framework under "
                    f"{MACOS_FRAMEWORKS}). Install from python.org for a "
                    "complete matrix.",
                    file=sys.stderr,
                )

    for target in targets:
        for arch in _architectures(target, host_arch):
            prefix = f"/tmp/a11-wheel-deps-{target}-{arch}"
            if target == "linux":
                prefix = f"/opt/a11-wheel-deps-{arch}"
            env = os.environ.copy()
            env["A11_DEPS_PREFIX"] = prefix
            env["A11_WHEEL_ARCH"] = arch
            if target == "macos":
                env["MACOSX_DEPLOYMENT_TARGET"] = "14.4"
                if macos_skip:
                    env["CIBW_SKIP"] = macos_skip
            # Audit every wheel we can execute; skip only the genuinely
            # unrunnable cross-architecture case (with a notice) so a broken
            # cross wheel is never shipped silently.
            if not _wheel_is_runnable(target, arch, host_arch):
                env["CIBW_TEST_SKIP"] = f"*-macosx_{arch}"
                print(
                    f"note: cannot run {target} {arch} wheels on this host; "
                    "their audit is skipped. Build on that architecture (or "
                    "enable Rosetta) to audit them.",
                    file=sys.stderr,
                )
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

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

"""Resolve a coding session's repository, paths, and instructions."""

from __future__ import annotations

import os
import pathlib
import subprocess
from dataclasses import dataclass, field

from a11.status import Status, StatusCode

INSTRUCTION_FILE = "AGENTS.md"


def _inside(path: pathlib.Path, root: pathlib.Path) -> bool:
    return path == root or root in path.parents


def _git_output(cwd: pathlib.Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(cwd), *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


@dataclass
class Workspace:
    """Canonical roots and mutable facts for one coding session."""

    cwd: pathlib.Path
    root: pathlib.Path
    repo_root: pathlib.Path | None
    read_roots: tuple[pathlib.Path, ...]
    write_roots: tuple[pathlib.Path, ...]
    initial_status: str
    changed_files: set[str] = field(default_factory=set)
    checks: list[dict[str, object]] = field(default_factory=list)

    @classmethod
    def open(
        cls,
        cwd: str | os.PathLike[str],
        add_dirs: tuple[str, ...] = (),
    ) -> Workspace:
        """Open a workspace around ``cwd`` and canonicalize its roots."""
        start = pathlib.Path(cwd).expanduser().resolve(strict=True)
        if not start.is_dir():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"The coding workspace is not a directory: {start}",
            ).to_exception()

        found = _git_output(start, "rev-parse", "--show-toplevel")
        repo_root = pathlib.Path(found).resolve() if found else None
        root = repo_root or start
        extras: list[pathlib.Path] = []
        for value in add_dirs:
            extra = pathlib.Path(value).expanduser().resolve(strict=True)
            if not extra.is_dir():
                message = (
                    f"An additional workspace root is not a directory: {extra}"
                )
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=message,
                ).to_exception()
            extras.append(extra)

        status = _git_output(root, "status", "--short") if repo_root else ""
        return cls(
            cwd=start,
            root=root,
            repo_root=repo_root,
            read_roots=(root, *extras),
            write_roots=(root, *extras),
            initial_status=status or "",
        )

    def resolve(
        self,
        value: str | os.PathLike[str] = ".",
        *,
        write: bool = False,
        directory: bool | None = None,
    ) -> pathlib.Path:
        """Resolve a path and enforce the configured read or write roots."""
        raw = pathlib.Path(value).expanduser()
        candidate = raw if raw.is_absolute() else self.cwd / raw
        candidate = candidate.resolve(strict=False)
        roots = self.write_roots if write else self.read_roots
        if not any(_inside(candidate, root) for root in roots):
            operation = "write" if write else "read"
            message = (
                f"The {operation} path is outside the workspace: {candidate}"
            )
            raise Status(
                code=StatusCode.PERMISSION_DENIED,
                message=message,
                details=[{"roots": [str(root) for root in roots]}],
            ).to_exception()
        if directory is True and candidate.exists() and not candidate.is_dir():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Expected a directory: {candidate}",
            ).to_exception()
        if (
            directory is False
            and candidate.exists()
            and not candidate.is_file()
        ):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Expected a file: {candidate}",
            ).to_exception()
        return candidate

    def relative(self, path: pathlib.Path) -> str:
        """Return a stable display path for a file in a configured root."""
        for root in self.read_roots:
            if _inside(path, root):
                relative = path.relative_to(root)
                if root == self.root:
                    return str(relative) or "."
                return f"{root.name}/{relative}"
        return str(path)

    def instructions_for(
        self, value: str | os.PathLike[str] = "."
    ) -> list[dict[str, str]]:
        """Read instruction files whose directory contains ``value``."""
        target = self.resolve(value)
        parent = target if target.is_dir() else target.parent
        owning_root = next(
            root for root in self.read_roots if _inside(parent, root)
        )
        directories = [owning_root]
        if parent != owning_root:
            relative = parent.relative_to(owning_root)
            current = owning_root
            for part in relative.parts:
                current /= part
                directories.append(current)

        instructions: list[dict[str, str]] = []
        for directory in directories:
            source = directory / INSTRUCTION_FILE
            if source.is_file():
                try:
                    text = source.read_text(encoding="utf-8")
                except (OSError, UnicodeError) as error:
                    message = (
                        f"Cannot read repository instructions: {source}:"
                        f" {error}"
                    )
                    raise Status(
                        code=StatusCode.DATA_LOSS,
                        message=message,
                    ).to_exception() from error
                instructions.append({
                    "path": self.relative(source),
                    "text": text,
                })
        return instructions

    def git_status(self) -> str:
        """Return the current short Git status, or an empty string."""
        if self.repo_root is None:
            return ""
        return _git_output(self.repo_root, "status", "--short") or ""

    def summary(self) -> dict[str, object]:
        """Describe the workspace without returning repository contents."""
        return {
            "cwd": str(self.cwd),
            "root": str(self.root),
            "repository_root": str(self.repo_root or ""),
            "read_roots": [str(path) for path in self.read_roots],
            "write_roots": [str(path) for path in self.write_roots],
            "initial_git_status": self.initial_status,
            "current_git_status": self.git_status(),
            "agent_changed_files": sorted(self.changed_files),
            "checks": list(self.checks),
            "instructions": self.instructions_for(self.cwd),
        }

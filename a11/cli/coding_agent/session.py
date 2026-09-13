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

"""Persist coding-chat interactions and task state in a versioned record."""

from __future__ import annotations

import json
import os
import pathlib
import time
import uuid
from dataclasses import dataclass, field

import a11
from a11.sdk.llm import Interaction
from a11.status import Status, StatusCode

SESSION_VERSION = 1


def default_root() -> pathlib.Path:
    """Return the user-private directory for coding-chat sessions."""
    if configured := os.environ.get("A11_CHAT_SESSION_ROOT", ""):
        return pathlib.Path(configured).expanduser()
    state = os.environ.get("XDG_STATE_HOME", "") or "~/.local/state"
    return pathlib.Path(state).expanduser() / "a11" / "chat"


def _encode(interaction: Interaction) -> str:
    return a11.to_chunk(interaction).data.decode("utf-8")


def _decode(value: str) -> Interaction:
    chunk = a11.to_chunk(json.loads(value), "application/json")
    return a11.from_chunk(chunk, obj_type=Interaction)


@dataclass
class SessionRecord:
    """Serializable state required to continue a coding conversation."""

    id: str = field(default_factory=lambda: uuid.uuid4().hex)
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)
    workspace: str = ""
    provider: str = ""
    model: str = ""
    task: str = ""
    history: list[Interaction] = field(default_factory=list)
    agent_state: dict[str, object] = field(default_factory=dict)

    def document(self) -> dict[str, object]:
        """Return the versioned JSON document written to disk."""
        return {
            "version": SESSION_VERSION,
            "id": self.id,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
            "workspace": self.workspace,
            "provider": self.provider,
            "model": self.model,
            "task": self.task,
            "history": [_encode(item) for item in self.history],
            "agent_state": self.agent_state,
        }

    @classmethod
    def from_document(cls, value: dict[str, object]) -> SessionRecord:
        """Validate and decode one stored document."""
        if value.get("version") != SESSION_VERSION:
            version = value.get("version")
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=f"Unsupported coding session version: {version}",
            ).to_exception()
        try:
            return cls(
                id=str(value["id"]),
                created_at=float(value["created_at"]),
                updated_at=float(value["updated_at"]),
                workspace=str(value["workspace"]),
                provider=str(value["provider"]),
                model=str(value["model"]),
                task=str(value.get("task", "")),
                history=[
                    _decode(str(item)) for item in value.get("history", [])
                ],
                agent_state=dict(value.get("agent_state", {})),
            )
        except (KeyError, TypeError, ValueError) as error:
            raise Status(
                code=StatusCode.DATA_LOSS,
                message=f"Invalid coding session record: {error}",
            ).to_exception() from error


class SessionStore:
    """Atomic private-file storage for coding session records."""

    def __init__(self, root: pathlib.Path | None = None) -> None:
        self.root = pathlib.Path(root or default_root()).expanduser()
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)

    def save(self, record: SessionRecord) -> pathlib.Path:
        """Atomically write ``record`` and return its path."""
        record.updated_at = time.time()
        path = self.root / f"{record.id}.json"
        temporary = self.root / f".{record.id}.{uuid.uuid4().hex}.tmp"
        data = json.dumps(record.document(), ensure_ascii=False, indent=2)
        descriptor = os.open(
            temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
        )
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            if temporary.exists():
                temporary.unlink()
        return path

    def load(self, session_id: str | None = None) -> SessionRecord:
        """Load a named session, or the most recently updated one."""
        if session_id:
            path = self.root / f"{session_id}.json"
        else:
            candidates = sorted(
                self.root.glob("*.json"),
                key=lambda item: item.stat().st_mtime,
                reverse=True,
            )
            if not candidates:
                raise Status(
                    code=StatusCode.NOT_FOUND,
                    message="There is no coding chat session to resume.",
                ).to_exception()
            path = candidates[0]
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError as error:
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=f"No coding chat session called {session_id!r}.",
            ).to_exception() from error
        except (OSError, json.JSONDecodeError) as error:
            raise Status(
                code=StatusCode.DATA_LOSS,
                message=f"Cannot read coding session {path}: {error}",
            ).to_exception() from error
        if not isinstance(value, dict):
            raise Status(
                code=StatusCode.DATA_LOSS,
                message=f"Coding session {path} is not a JSON object.",
            ).to_exception()
        return SessionRecord.from_document(value)

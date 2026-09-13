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

"""Private, atomic storage for terminal chat preferences."""

from __future__ import annotations

import json
import os
import pathlib
import uuid
from dataclasses import asdict, dataclass
from dataclasses import field

from a11.client.credentials import config_home


@dataclass
class ChatSettings:
    """Preferences that should survive between chat sessions."""

    provider: str = ""
    model: str = ""
    provider_configs: dict[str, dict[str, object]] = field(default_factory=dict)


class ChatSettingsStore:
    """Read and atomically replace the user's chat preferences."""

    def __init__(self, root: pathlib.Path | None = None) -> None:
        self.root = pathlib.Path(root or config_home()).expanduser()
        self.path = self.root / "chat.json"
        self.history_path = self.root / "chat-history"

    def load(self) -> ChatSettings:
        """Return saved settings, or defaults for a missing/invalid file."""
        try:
            value = json.loads(self.path.read_text(encoding="utf-8"))
            if not isinstance(value, dict):
                return ChatSettings()
            return ChatSettings(
                provider=str(value.get("provider", "")),
                model=str(value.get("model", "")),
                provider_configs={
                    str(provider): dict(config)
                    for provider, config in dict(
                        value.get("provider_configs", {})
                    ).items()
                    if isinstance(config, dict)
                },
            )
        except (FileNotFoundError, OSError, ValueError, TypeError):
            return ChatSettings()

    def save(self, settings: ChatSettings) -> None:
        """Persist settings in a user-private file."""
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        temporary = self.root / f".chat.{uuid.uuid4().hex}.tmp"
        descriptor = os.open(
            temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
        )
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                json.dump(asdict(settings), stream, indent=2)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)
        finally:
            if temporary.exists():
                temporary.unlink()


__all__ = ["ChatSettings", "ChatSettingsStore"]

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

"""Authorize coding-agent effects before they reach their execution boundary."""

from __future__ import annotations

from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from enum import StrEnum

from a11.status import Status, StatusCode


class ApprovalMode(StrEnum):
    """How a coding session resolves effectful operations."""

    ASK = "ask"
    SUGGEST = "suggest"
    AUTO = "auto"


ApprovalCallback = Callable[[str, str], Awaitable[bool]]


@dataclass
class Policy:
    """Session permission mode and its recorded user decisions."""

    mode: ApprovalMode
    approve: ApprovalCallback | None = None
    decisions: list[dict[str, str]] = field(default_factory=list)

    async def authorize(self, kind: str, description: str) -> None:
        """Authorize one effect or raise ``PERMISSION_DENIED``."""
        if self.mode == ApprovalMode.AUTO:
            self.decisions.append({
                "kind": kind,
                "decision": "auto",
                "description": description,
            })
            return
        if self.mode == ApprovalMode.SUGGEST:
            self.decisions.append({
                "kind": kind,
                "decision": "denied",
                "description": description,
            })
            raise Status(
                code=StatusCode.PERMISSION_DENIED,
                message=(
                    f"{kind} is disabled in suggest mode. Present the proposed"
                    " change or command to the user instead."
                ),
            ).to_exception()
        if self.approve is None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=f"{kind} requires an interactive approval callback.",
            ).to_exception()
        allowed = await self.approve(kind, description)
        self.decisions.append({
            "kind": kind,
            "decision": "approved" if allowed else "denied",
            "description": description,
        })
        if not allowed:
            raise Status(
                code=StatusCode.PERMISSION_DENIED,
                message=f"The user denied {kind}: {description}",
            ).to_exception()

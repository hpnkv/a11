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

"""Observe bounded JSON/SSE usage while relaying provider bytes unchanged."""

from __future__ import annotations

import json
import threading
import time
from collections.abc import Callable

from a11.sdk.usage import UsageScope, UsageUpdate, provider_usage


class ResponseUsageObserver:
    """Consume passive relay events and publish shared usage snapshots.

    Args:
        profile: chat, responses, or anthropic wire format.
        request_id: Stable request identity for replacement usage revisions.
        record: Synchronous metadata sink. It runs on the relay worker.
        metadata: Host-selected run/arm/model labels.
        max_event_bytes: Maximum buffered JSON document or SSE event.
        started_at: Optional host monotonic request start, including projection
            time in the first-byte measurement.
    """

    def __init__(
        self,
        profile: str,
        request_id: str,
        record: Callable[[dict], None],
        *,
        metadata: dict | None = None,
        max_event_bytes: int = 1024 * 1024,
        started_at: float | None = None,
    ):
        self.profile, self.request_id, self.record = profile, request_id, record
        self.metadata = metadata or {}
        self.max_event_bytes = max_event_bytes
        self._lock = threading.Lock()
        self._buffer = bytearray()
        self._sse = False
        self._usage = None
        self._revision = 0
        self._status = 0
        self._started = time.monotonic() if started_at is None else started_at
        self._first_byte = None
        self._closed = False
        self._limited = False
        self._terminal = False
        self._model = ""

    def __call__(self, event, head, data: bytes) -> None:
        """Accept native relay headers/body/complete/limit/error events."""
        with self._lock:
            if self._closed:
                return
            if event == "headers":
                self._status = head.status
                self._sse = any(
                    k.lower() == "content-type"
                    and "text/event-stream" in v.lower()
                    for k, v in head.headers
                )
            elif event == "body":
                if self._first_byte is None:
                    self._first_byte = time.monotonic() - self._started
                if self._limited:
                    return
                self._buffer.extend(data)
                if self._sse:
                    while True:
                        raw = bytes(self._buffer)
                        lf, crlf = raw.find(b"\n\n"), raw.find(b"\r\n\r\n")
                        boundaries = [
                            (i, width)
                            for i, width in ((lf, 2), (crlf, 4))
                            if i >= 0
                        ]
                        if not boundaries:
                            break
                        end, width = min(boundaries)
                        block = raw[:end]
                        del self._buffer[: end + width]
                        if len(block) <= self.max_event_bytes:
                            payload = b"\n".join(
                                line[5:].lstrip(b" ")
                                for line in block.splitlines()
                                if line.startswith(b"data:")
                            )
                            self._consume(payload)
                        else:
                            self._limited = True
                if len(self._buffer) > self.max_event_bytes:
                    self._limited = True
                    self._buffer.clear()
            elif event in ("complete", "limit", "error"):
                if not self._sse and self._buffer and not self._limited:
                    self._consume(bytes(self._buffer))
                if self._sse and self._buffer.strip():
                    self._limited = True
                complete = (
                    event == "complete"
                    and not self._limited
                    and (not self._sse or self._terminal)
                    and 200 <= self._status < 400
                )
                self._publish(complete)
                self._closed = True
                self.record(
                    {
                        **self.metadata,
                        "schema": "a11.llm-context.report/v1",
                        "operation": "response_observation",
                        "request_id": self.request_id,
                        "first_byte_seconds": self._first_byte,
                        "http_status": self._status,
                        "coverage": "complete" if complete else "partial",
                        "usage_available": self._usage is not None,
                    }
                )

    def _consume(self, payload: bytes) -> None:
        if payload == b"[DONE]":
            self._terminal = True
            return
        if not payload:
            return
        try:
            value = json.loads(payload)
        except (ValueError, UnicodeError):
            self._limited = True
            return
        if not isinstance(value, dict):
            return
        event_type = value.get("type")
        if event_type in ("error", "response.failed", "response.incomplete"):
            self._limited = True
        if event_type in ("message_stop", "response.completed"):
            self._terminal = True
        response = value.get("response", value.get("message", value))
        if not isinstance(response, dict):
            return
        if response.get("error") or response.get("status") in (
            "failed",
            "incomplete",
            "cancelled",
        ):
            self._limited = True
        self._model = response.get("model", self._model)
        usage = response.get("usage", value.get("usage"))
        if isinstance(usage, dict):
            self._usage = {**(self._usage or {}), **usage}
            self._publish(False)

    def _publish(self, complete: bool) -> None:
        if self._usage is None:
            return
        normalized = provider_usage(self.profile, self._usage)
        self._revision += 1
        identity = self.request_id + ":usage"
        normalized = normalized.model_copy(
            update={
                "usage_id": identity,
                "usage_revision": self._revision,
                "complete": complete,
            }
        )
        update = UsageUpdate(
            id=identity,
            provider=self.profile,
            model=self._model,
            revision=self._revision,
            usage=normalized,
            coverage="proxy_sse" if self._sse else "proxy_json",
            scope=UsageScope(request_id=self.request_id),
        )
        self.record({**self.metadata, **update.model_dump(mode="json")})

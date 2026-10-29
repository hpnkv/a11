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

"""Serve provider request projections over A11's native streaming HTTP relay."""

from __future__ import annotations

import asyncio
import inspect
import json
import time
import uuid
from collections.abc import Awaitable, Callable
from urllib.parse import urlsplit

from a11 import _native, timing
from a11.net.http2 import Http2Options, Http2Server
from a11.sdk.llm_context.config import (
    LlmContextConfig,
    LlmContextScope,
    LlmContextServices,
)
from a11.sdk.llm_context.observation import ResponseUsageObserver
from a11.sdk.llm_context.runtime import project_view
from a11.status import Status, StatusCode, StatusException


async def project_request(
    body: bytes,
    *,
    path: str,
    profile: str,
    config: LlmContextConfig,
    services: LlmContextServices | None = None,
    reporter: Callable[[dict], Awaitable[None]] | None = None,
    scope: dict | None = None,
) -> bytes:
    """Project supported JSON messages, preserving unknown request fields.

    The profile names a wire format, independently of the harness using it.
    Stateful response chains require a history-replacement capability and are
    rejected when summary compaction is selected.

    Args:
        body: Complete JSON request bytes within the policy byte limit.
        path: Incoming provider path, including any query.
        profile: chat, responses, or anthropic wire shape.
        config: Host-selected projection and compaction policy.
        services: Host tokenizer and named summarizers.
        reporter: Optional async metadata observer for each selected span.
        scope: Host-authenticated and authorized scope for retained views.

    Returns:
        Original bytes when no field changes, or a JSON request containing
        projected messages. Unknown request fields retain their JSON values.

    Raises:
        StatusException: Parsing/limits fail or required history/storage
            capabilities are unavailable.
    """
    fields = {
        "chat": ("/chat/completions", "messages"),
        "responses": ("/responses", "input"),
        "anthropic": ("/messages", "messages"),
    }
    if profile not in fields:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Unknown provider wire profile",
        ).to_exception()
    suffix, field = fields[profile]
    if not path.split("?", 1)[0].endswith(suffix) or (
        not config.enabled and config.measurement.mode == "off"
    ):
        return body
    if config.boundary == "interaction":
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Interaction projection requires A11 Interaction inputs",
        ).to_exception()
    if len(body) > config.max_bytes:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="Proxy request exceeds context byte limit",
        ).to_exception()
    # Native validation bounds nesting before Python reconstructs the envelope.
    _native.llm_context_compact_json(body.decode(), config.max_bytes)
    payload = json.loads(body)
    if not isinstance(payload, dict):
        return body
    messages = payload.get(field)
    if isinstance(messages, str) and field == "input":
        messages = [{"role": "user", "content": messages}]
    if not isinstance(messages, list) or not all(
        isinstance(m, dict) for m in messages
    ):
        return body
    services = services or LlmContextServices()
    callback = services.summarizers.get(config.summarizer)
    prepared = await project_view(
        messages,
        config,
        services=services,
        scope=scope,
        reporter=reporter,
        summarizer=callback,
        stateful=bool(payload.get("previous_response_id")),
    )
    projected = prepared["messages"]
    if projected == messages:
        return body
    payload[field] = projected
    return json.dumps(
        payload, ensure_ascii=False, separators=(",", ":")
    ).encode()


def create_proxy(
    upstream: str,
    *,
    profile: str = "chat",
    config: LlmContextConfig | None = None,
    services: LlmContextServices | None = None,
    bind_address: str = "127.0.0.1",
    port: int = 0,
    max_inflight: int = 32,
    timeout_seconds: float = 120,
    options: Http2Options | None = None,
) -> Http2Server:
    """Start an embeddable provider proxy with host-selected policy.

    Upstream is a fixed origin with an optional path prefix. Credentials pass
    through from each request; inbound context headers never configure the host.
    Responses, including SSE and provider errors, remain native byte streams.
    Stop the returned native server to release its listening socket.

    Args:
        upstream: Fixed HTTP/HTTPS origin with an optional literal path prefix.
        profile: chat, responses, or anthropic request format.
        config: Host-selected policy; omitted policy disables projections.
        services: Host dependencies and optional metadata recorder.
        bind_address: Listener address, initially loopback.
        port: Listener port; zero selects an available port.
        max_inflight: Maximum admitted concurrent exchanges.
        timeout_seconds: Deadline per relayed exchange.
        options: Native HTTP transport/TLS and buffering options.

    Returns:
        The running native Http2Server. Responses remain streamed by the native
        relay, including non-success statuses and trailers.

    Raises:
        StatusException: Configuration, limits, or listener setup fail.
    """
    config = config or LlmContextConfig(enabled=False)
    services = services or LlmContextServices()
    if profile not in ("chat", "responses", "anthropic"):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Unknown provider wire profile",
        ).to_exception()
    parsed = urlsplit(upstream)
    if (
        parsed.scheme not in ("http", "https")
        or not parsed.netloc
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or max_inflight < 1
        or timeout_seconds <= 0
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Proxy needs a fixed HTTP origin and positive limits",
        ).to_exception()
    if (
        config.max_bytes > services.max_bytes
        or config.max_tokens > services.max_tokens
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Proxy policy exceeds host limits",
        ).to_exception()
    transport = options or Http2Options()
    transport.max_request_body_size = config.max_bytes
    gate = asyncio.Semaphore(max_inflight)

    async def handler(request, response):
        # Admission is bounded; queued requests receive an explicit response.
        if gate.locked():
            response.send_response(503, [], b"Proxy admission limit reached")
            return
        async with gate:
            started = time.monotonic()
            operation_id = uuid.uuid4().hex

            async def record(report):
                if services.recorder is not None:
                    await asyncio.to_thread(
                        services.recorder.record,
                        {
                            **report,
                            "operation_id": str(uuid.uuid4()),
                            "request_id": operation_id,
                            "surface": "proxy",
                        },
                    )

            try:
                headers = {k.lower(): v for k, v in request.headers}
                if (
                    request.method == "POST"
                    and headers.get("content-type", "").split(";", 1)[0]
                    == "application/json"
                    and headers.get("content-encoding", "identity")
                    == "identity"
                ):
                    scope = None
                    if (
                        config.enabled
                        and config.retention
                        and config.measurement.mode != "shadow"
                    ):
                        if (
                            services.resolve_scope is None
                            or services.authorize_scope is None
                        ):
                            raise Status(
                                code=StatusCode.FAILED_PRECONDITION,
                                message=(
                                    "Proxy retention requires host scope"
                                    " resolution and authorization"
                                ),
                            ).to_exception()
                        scope = services.resolve_scope(request)
                        if inspect.isawaitable(scope):
                            scope = await scope
                        scope = LlmContextScope.model_validate(
                            scope
                        ).model_dump()
                        allowed = services.authorize_scope(request, scope)
                        if inspect.isawaitable(allowed):
                            allowed = await allowed
                        if not allowed:
                            raise Status(
                                code=StatusCode.PERMISSION_DENIED,
                                message="Proxy context scope access denied",
                            ).to_exception()
                    request.body = await project_request(
                        request.body,
                        path=request.path,
                        profile=profile,
                        config=config,
                        services=services,
                        reporter=record,
                        scope=scope,
                    )
                observer = None
                if services.recorder is not None:
                    observer = ResponseUsageObserver(
                        profile,
                        operation_id,
                        services.recorder.record,
                        metadata={
                            "surface": "proxy",
                            "run_id": config.measurement.run_id,
                            "arm": config.measurement.arm,
                        },
                        max_event_bytes=config.max_bytes,
                        started_at=started,
                    )
                await _native.relay_http(
                    upstream.rstrip("/") + "/" + request.path.lstrip("/"),
                    request,
                    response,
                    transport,
                    timing.now() + timing.Duration.seconds(timeout_seconds),
                    observer=observer,
                    max_observed_bytes=config.max_bytes,
                )
            except (StatusException, UnicodeError, ValueError) as error:
                if not response.finished:
                    response.send_response(502, [], str(error).encode())
            finally:
                await record(
                    {
                        "schema": "a11.llm-context.report/v1",
                        "operation": "relay",
                        "elapsed_seconds": time.monotonic() - started,
                        "run_id": config.measurement.run_id,
                        "arm": config.measurement.arm,
                    }
                )

    return Http2Server.create(bind_address, port, handler, transport)

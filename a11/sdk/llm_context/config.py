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

"""Resolve immutable context policy from ordinary action headers."""

from __future__ import annotations

import enum
import functools
import inspect
import threading
import weakref
from collections import OrderedDict
from collections.abc import Mapping
from contextvars import ContextVar
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Any, Callable, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError

import a11
from a11.status import Status, StatusCode


class LlmContextHeaders(enum.StrEnum):
    """Action headers carrying caller policy and explicit owner scope."""

    CONFIG = "x-a11-llm-context-config"
    SCOPE = "x-a11-llm-context-scope"
    STAGE = "x-a11-llm-context-stage"
    OPERATION = "x-a11-llm-context-operation"


class ShellConfig(BaseModel):
    """Select off, count-only observation, or completed-result compression."""

    model_config = ConfigDict(frozen=True, extra="forbid")
    mode: Literal["off", "observe", "compress"] = "off"


class MeasurementConfig(BaseModel):
    """Label actual observations or unchanged-input shadow projections."""

    model_config = ConfigDict(frozen=True, extra="forbid")
    mode: Literal["off", "observe", "shadow"] = "off"
    run_id: str = Field(default="", max_length=128)
    arm: str = Field(default="", max_length=128)


class LlmContextConfig(BaseModel):
    """Select model-facing transforms independently of generation options.

    Input text, tool results, history summaries, retention, and shell results
    have separate opt-ins. Counts use max_tokens minus reserve_tokens as the
    history budget. Header absence resolves to disabled; an explicitly created
    configuration has enabled=True and each automatic transform initially off.
    Policy fields are immutable, bounded, and reject unknown names.
    """

    model_config = ConfigDict(frozen=True, extra="forbid", strict=True)
    enabled: bool = True
    boundary: Literal["provider", "interaction"] = "provider"
    input_content: Literal["off", "lossless"] = "off"
    tool_results: Literal["off", "lossless"] = "off"
    strategy: Literal["auto", "json", "lines", "json-keys"] = "auto"
    retention: bool = False
    compaction: Literal["off", "summary", "native"] = "off"
    summarizer: str = Field(default="", max_length=128)
    max_tokens: int = Field(default=24000, ge=1, le=2000000)
    reserve_tokens: int = Field(default=2048, ge=0, le=2000000)
    retain_recent: int = Field(default=4, ge=1, le=10000)
    max_bytes: int = Field(default=8 * 1024 * 1024, ge=1, le=32 * 1024 * 1024)
    min_gain: int = Field(default=16, ge=0, le=1000000)
    shell: ShellConfig = Field(default_factory=ShellConfig)
    measurement: MeasurementConfig = Field(default_factory=MeasurementConfig)


class LlmContextScope(BaseModel):
    """Carry owner and revision identity across action/session boundaries."""

    model_config = ConfigDict(frozen=True, extra="forbid", strict=True)
    owner: str = Field(min_length=1, max_length=256)
    conversation: str = Field(min_length=1, max_length=256)
    branch: str = Field(default="main", min_length=1, max_length=256)
    expected_revision: int = Field(default=0, ge=0)
    operation_id: str = Field(default="", max_length=256)


DISABLED = LlmContextConfig(enabled=False)
LLM_CONTEXT_HEADERS = {
    LlmContextHeaders.STAGE: a11.ActionHeaderSchema(
        LlmContextHeaders.STAGE,
        "SDK projection ownership for the current action identity.",
    ),
    LlmContextHeaders.OPERATION: a11.ActionHeaderSchema(
        LlmContextHeaders.OPERATION,
        "Optional LLM usage attribution for a nested summarization action.",
    ),
    LlmContextHeaders.CONFIG: a11.ActionHeaderSchema(
        LlmContextHeaders.CONFIG,
        "Optional JSON LLM context compression/compaction policy.",
    ),
    LlmContextHeaders.SCOPE: a11.ActionHeaderSchema(
        LlmContextHeaders.SCOPE,
        "Optional JSON owner, conversation, branch and expected view revision.",
    ),
}
MAX_HEADER_BYTES = 8192
_CACHE_LIMIT = 256
_cache: OrderedDict[bytes, LlmContextConfig] = OrderedDict()
_lock = threading.RLock()
_active: ContextVar[tuple[bytes | None, LlmContextConfig] | None] = ContextVar(
    "a11_llm_context_policy", default=None
)


def decode_config(raw: bytes | str | None) -> LlmContextConfig:
    """Decode a bounded policy header, reusing equal immutable values.

    Args:
        raw: UTF-8 JSON bytes/text or None for disabled policy.

    Returns:
        An immutable validated policy. A bounded process cache and an optional
        active-action fast path avoid repeated decoding of inherited headers.

    Raises:
        StatusException: The header exceeds 8192 bytes or fails validation.
    """
    if raw is None:
        return DISABLED
    raw = raw.encode() if isinstance(raw, str) else bytes(raw)
    if len(raw) > MAX_HEADER_BYTES:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Context config exceeds 8192 bytes",
        ).to_exception()
    inherited = _active.get()
    if inherited is not None and inherited[0] == raw:
        return inherited[1]
    with _lock:
        if raw in _cache:
            _cache.move_to_end(raw)
            return _cache[raw]
        try:
            config = LlmContextConfig.model_validate_json(raw)
        except (ValidationError, ValueError) as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid context config: {exc}",
            ).to_exception()
        _cache[raw] = config
        while len(_cache) > _CACHE_LIMIT:
            _cache.popitem(last=False)
        return config


@dataclass(frozen=True)
class LlmContextServices:
    """Carry host dependencies bound independently on each receiving registry.

    Attributes:
        max_bytes: Host ceiling for admitted content.
        max_tokens: Host ceiling for requested history budgets.
        summarizers: Names mapped to sync/async prefix-to-text callbacks.
        authorize_scope: Sync/async predicate receiving an Action or incoming
            HTTP request and its explicit scope.
        resolve_scope: Sync/async HTTP request-to-scope resolver. The host
            authenticates the request before returning a scope; authorize_scope
            then authorizes that request and scope independently.
        retention: Optional RetainedContentStore owned by this host.
        recorder: Optional metadata RunRecorder or compatible record sink.
        token_counter: Optional tokenizer returning a nonnegative count.
        revision: Host-selected service configuration revision. Change it when
            changing tokenizer/summarizer behavior to invalidate saved views.
    """

    max_bytes: int = 8 * 1024 * 1024
    max_tokens: int = 2000000
    summarizers: Mapping[str, Callable] = field(default_factory=dict)
    authorize_scope: Callable | None = None
    resolve_scope: Callable | None = None
    retention: Any = None
    recorder: Any = None
    token_counter: Callable[[str], int] | None = None
    revision: str = "1"

    def __post_init__(self) -> None:
        if self.max_bytes < 1 or self.max_tokens < 1:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Context service limits must be positive",
            ).to_exception()
        object.__setattr__(
            self, "summarizers", MappingProxyType(dict(self.summarizers))
        )


_services: weakref.WeakKeyDictionary = weakref.WeakKeyDictionary()
_DEFAULT_SERVICES = LlmContextServices()


def bind_services(
    registry: a11.ActionRegistry, services: LlmContextServices
) -> None:
    """Bind host dependencies to a registry and its declared SDK handlers.

    Registered Python handlers retain their host binding when the native
    registry is copied for a session. A receiving process registers its own
    services; caller headers carry no Python dependencies.
    """
    with _lock:
        _services[registry] = services
    for name in registry.list_registered_actions():
        schema = registry.get_schema(name)
        handler = registry.get_handler(name)
        if LlmContextHeaders.CONFIG not in schema.headers or not callable(
            handler
        ):
            continue
        original = getattr(handler, "_llm_context_original", handler)

        def bind(original):
            @functools.wraps(original)
            async def bound(action, *args, **kwargs):
                receiving = action.get_registry()
                if receiving is not None:
                    with _lock:
                        _services[receiving] = services
                return await original(action, *args, **kwargs)

            bound._llm_context_original = original
            return bound

        registry.register(name, schema, bind(original))


def services_for_registry(
    registry: a11.ActionRegistry | None,
) -> LlmContextServices:
    """Read receiving-host dependencies or the stateless host defaults."""
    with _lock:
        return (
            _services.get(registry, _DEFAULT_SERVICES)
            if registry is not None
            else _DEFAULT_SERVICES
        )


def services_for(action: a11.Action | None) -> LlmContextServices:
    """Resolve services from the receiving action registry."""
    get_registry = getattr(action, "get_registry", None)
    return services_for_registry(
        get_registry() if get_registry is not None else None
    )


def config_for(action: a11.Action) -> LlmContextConfig:
    """Resolve policy and enforce host limits during context processing."""
    config = decode_config(action.get_header(LlmContextHeaders.CONFIG))
    if not config.enabled and config.measurement.mode == "off":
        return config
    _validate_host_limits(config, services_for(action))
    return config


def _validate_host_limits(
    config: LlmContextConfig, services: LlmContextServices
) -> None:
    if (
        config.max_bytes > services.max_bytes
        or config.max_tokens > services.max_tokens
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Context policy exceeds host limits",
        ).to_exception()


async def authorized_scope(action, scope=None) -> dict:
    """Validate explicit scope and ask the receiving host to authorize it."""
    services = services_for(action)
    try:
        if scope is None:
            raw = action.get_header(LlmContextHeaders.SCOPE)
            if raw is None or len(raw) > MAX_HEADER_BYTES:
                raise ValueError("A bounded context scope header is required")
            selected = LlmContextScope.model_validate_json(raw)
        else:
            selected = LlmContextScope.model_validate(scope)
    except (ValidationError, ValueError) as error:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid context scope: {error}",
        ).to_exception()
    scope = selected.model_dump()
    if services.authorize_scope is None:
        raise Status(
            code=StatusCode.PERMISSION_DENIED,
            message="Host scope authorization is required",
        ).to_exception()
    allowed = services.authorize_scope(action, scope)
    if inspect.isawaitable(allowed):
        allowed = await allowed
    if not allowed:
        raise Status(
            code=StatusCode.PERMISSION_DENIED,
            message="Context scope access denied",
        ).to_exception()
    return scope


def configure_registry(
    registry: a11.ActionRegistry,
    config: LlmContextConfig,
    *,
    names: tuple[str, ...] | list[str] = ("interact_with_llm",),
    services: LlmContextServices | None = None,
) -> None:
    """Publish policy defaults using existing header schemas and handlers.

    Explicit per-call headers override these defaults. Each receiving registry
    binds its own host dependencies, including registries copied for sessions.
    """
    if services is not None:
        bind_services(registry, services)
    for name in names:
        schema = registry.get_schema(name)
        headers = dict(schema.headers)
        headers[LlmContextHeaders.CONFIG] = a11.ActionHeaderSchema(
            LlmContextHeaders.CONFIG,
            "Default context policy",
            config.model_dump_json().encode(),
        )
        headers[LlmContextHeaders.SCOPE] = LLM_CONTEXT_HEADERS[
            LlmContextHeaders.SCOPE
        ]
        configured = a11.ActionSchema(
            name=schema.name,
            description=schema.description,
            inputs=dict(schema.inputs),
            outputs=dict(schema.outputs),
            headers=headers,
            output_to_json_field=dict(schema.output_to_json_field),
        )
        registry.register(name, configured, registry.get_handler(name))

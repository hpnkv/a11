# Copyright 2026 The A11 Authors.

"""Explicit OpenTelemetry configuration for A11.

Tracing is emitted natively from the C++ runtime and is off until
`configure_otel` (or a backend helper such as `langfuse`) is
called. Spans are exported directly from C++ over OTLP/HTTP (JSON), so
telemetry never depends on the Python layer being on the hot path.
"""

from __future__ import annotations

import atexit
import base64
import json
import os
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, Any
from urllib.parse import quote, unquote

from a11 import _native

if TYPE_CHECKING:
    from a11.actions import Action

_LANGFUSE_INPUT_ATTR = "langfuse.observation.input"
_LANGFUSE_OUTPUT_ATTR = "langfuse.observation.output"


def _span_json(value: Any) -> str:
    if isinstance(value, str):
        return value
    return json.dumps(value, default=str)


class Span:
    """A standalone tracing span, e.g. to group work that isn't a single action.

    Create one with `start_span`, pass ``span.traceparent()`` to
    `enable_tracing` on child actions to nest them, and ``end()`` it (or
    use it as a context manager). Mutators are no-ops when tracing is off.
    """

    __slots__ = ("_native",)

    def __init__(self, native: Any) -> None:
        self._native = native

    def traceparent(self) -> str:
        """W3C traceparent for parenting child actions/spans to this one."""
        return self._native.traceparent()

    def set_attribute(self, key: str, value: Any) -> "Span":
        self._native.set_attribute(key, value)
        return self

    def set_name(self, name: str) -> "Span":
        self._native.set_name(name)
        return self

    def set_status(self, code: str, description: str = "") -> "Span":
        """Set the span status. ``code`` is ``"ok"``, ``"error"`` or
        ``"unset"``; ``description`` is used for errors."""
        self._native.set_status(code, description)
        return self

    def set_input(self, value: Any) -> "Span":
        self._native.set_attribute(_LANGFUSE_INPUT_ATTR, _span_json(value))
        return self

    def set_output(self, value: Any) -> "Span":
        self._native.set_attribute(_LANGFUSE_OUTPUT_ATTR, _span_json(value))
        return self

    def end(self) -> None:
        self._native.end()

    def __enter__(self) -> "Span":
        return self

    def __exit__(self, *exc: object) -> bool:
        self.end()
        return False


def start_span(
    name: str,
    *,
    kind: str = "internal",
    parent_traceparent: str | None = None,
) -> Span:
    """Start a standalone span. When ``parent_traceparent`` is given the span
    continues that trace; otherwise it begins a new root trace. Returns an
    inactive Span (all no-ops) if tracing is not configured."""
    return Span(_native.obs_start_span(name, kind, parent_traceparent or ""))


_DEFAULT_OTLP_TRACES_ENDPOINT = "http://localhost:4318/v1/traces"

TRACEPARENT_HEADER: str = _native.OTEL_TRACEPARENT_HEADER
TRACESTATE_HEADER: str = _native.OTEL_TRACESTATE_HEADER
BAGGAGE_HEADER: str = _native.OTEL_BAGGAGE_HEADER

# Baggage keys Langfuse reads as span attributes. Promoted by default when you
# configure via langfuse(); set x-otel-baggage: langfuse.session.id=... upstream
# and it lands on every span in the trace.
LANGFUSE_BAGGAGE_KEYS: tuple[str, ...] = (
    "langfuse.session.id",
    "langfuse.user.id",
)


def configure_otel(
    *,
    service_name: str = "a11",
    resource_attributes: Mapping[str, str] | None = None,
    exporter: str = "otlp_http",
    endpoint: str = "",
    headers: Mapping[str, str] | None = None,
    timeout_millis: int = 10000,
    use_simple_processor: bool = False,
    baggage_span_attributes: Sequence[str] | None = None,
) -> None:
    """Install the global tracer provider.

    ``exporter`` is one of ``"otlp_http"``, ``"ostream"``, ``"in_memory"`` or
    ``"none"``. For ``"otlp_http"`` supply ``endpoint`` (a full traces URL) and
    any auth ``headers``. ``baggage_span_attributes`` names W3C baggage keys
    that are copied onto every span as attributes (e.g. ``langfuse.session.id``)
    while still propagating downstream. Raises ``StatusException`` on error
    (e.g. an OTLP endpoint on a build without OTLP support).
    """
    _native.obs_configure(
        service_name=service_name,
        resource_attributes=dict(resource_attributes or {}),
        exporter=exporter,
        use_simple_processor=use_simple_processor,
        otlp_endpoint=endpoint,
        otlp_headers=dict(headers or {}),
        otlp_timeout_millis=timeout_millis,
        baggage_span_attributes=list(baggage_span_attributes or ()),
    )


def shutdown_otel() -> None:
    """Flush and tear down the global tracer provider."""
    _native.obs_shutdown()


def is_configured() -> bool:
    return _native.obs_is_configured()


def langfuse_otlp_endpoint(host: str = "https://cloud.langfuse.com") -> str:
    """Return the OTLP traces endpoint for a Langfuse host."""
    return host.rstrip("/") + "/api/public/otel/v1/traces"


def langfuse_auth_header(public_key: str, secret_key: str) -> str:
    """Return the HTTP Basic auth header value Langfuse expects."""
    token = base64.b64encode(f"{public_key}:{secret_key}".encode()).decode(
        "ascii"
    )
    return f"Basic {token}"


def langfuse(
    *,
    public_key: str,
    secret_key: str,
    host: str = "https://cloud.langfuse.com",
    service_name: str = "a11",
    resource_attributes: Mapping[str, str] | None = None,
    extra_headers: Mapping[str, str] | None = None,
    baggage_span_attributes: Sequence[str] | None = None,
    timeout_millis: int = 10000,
) -> str:
    """Configure native OTLP/HTTP export to Langfuse's ingestion endpoint.

    Langfuse is OTLP-compatible, so this just points the native exporter at the
    Langfuse traces endpoint with Basic auth. Additional ``resource_attributes``
    (and ``extra_headers``) let a Langfuse integration attach its own metadata.
    ``baggage_span_attributes`` defaults to `LANGFUSE_BAGGAGE_KEYS`, so
    request-scoped values set upstream via the ``x-otel-baggage`` header --
    e.g. ``langfuse.session.id`` -- are promoted to span attributes Langfuse
    reads, and still propagate across nested/remote actions.

    Returns the resolved endpoint.
    """
    endpoint = langfuse_otlp_endpoint(host)
    headers = {"Authorization": langfuse_auth_header(public_key, secret_key)}
    if extra_headers:
        headers.update(extra_headers)
    if baggage_span_attributes is None:
        baggage_span_attributes = LANGFUSE_BAGGAGE_KEYS
    configure_otel(
        service_name=service_name,
        resource_attributes=resource_attributes,
        exporter="otlp_http",
        endpoint=endpoint,
        headers=headers,
        timeout_millis=timeout_millis,
        baggage_span_attributes=baggage_span_attributes,
    )
    return endpoint


def new_traceparent(*, sampled: bool = True) -> str:
    """Mint a fresh W3C ``traceparent`` value for a brand-new root trace.

    Use this when the action you run/call has no upstream context and you want
    it to start its own trace: set it as the ``TRACEPARENT_HEADER`` before
    ``run()``/``call()`` (or just use `enable_tracing`).
    """
    trace_id = os.urandom(16).hex()
    span_id = os.urandom(8).hex()
    while trace_id == "0" * 32:  # the parser rejects an all-zero trace id
        trace_id = os.urandom(16).hex()
    while span_id == "0" * 16:
        span_id = os.urandom(8).hex()
    return f"00-{trace_id}-{span_id}-{'01' if sampled else '00'}"


def _format_baggage(baggage: Mapping[str, str] | str) -> str:
    if isinstance(baggage, str):
        return baggage
    return ",".join(
        f"{key}={quote(str(value), safe='')}" for key, value in baggage.items()
    )


def enable_tracing(
    action: "Action",
    *,
    traceparent: str | None = None,
    tracestate: str | None = None,
    baggage: Mapping[str, str] | str | None = None,
) -> "Action":
    """Enable tracing on a root ``action`` before you run or call it.

    Sets the reserved W3C headers so the action's handler run becomes a span
    (tracing must already be configured). With no ``traceparent`` an existing
    one on the action is reused, otherwise a fresh root trace is minted.
    ``baggage`` -- a mapping such as ``{"langfuse.session.id": "..."}`` or a
    preformatted header string -- rides along and propagates downstream.

    Returns the action so it chains: ``enable_tracing(a).run()``. Nested actions
    are traced automatically and do not need this call.
    """
    resolved = traceparent
    if resolved is None:
        existing = action.get_header(TRACEPARENT_HEADER, decode=True)
        resolved = existing or new_traceparent()
    action.set_header(TRACEPARENT_HEADER, resolved.encode())
    if tracestate:
        action.set_header(TRACESTATE_HEADER, tracestate.encode())
    if baggage:
        action.set_header(BAGGAGE_HEADER, _format_baggage(baggage).encode())
    return action


def _parse_kv_list(value: str) -> dict[str, str]:
    """Parse a ``k=v,k2=v2`` list; values are percent-decoded per spec."""
    result: dict[str, str] = {}
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        key, sep, val = item.partition("=")
        if not sep:
            continue
        result[key.strip()] = unquote(val.strip())
    return result


def _env_flag(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {
        "1",
        "true",
        "yes",
        "on",
    }


def _otel_service_name_from_env() -> str | None:
    name = os.environ.get("OTEL_SERVICE_NAME")
    if name:
        return name
    return _parse_kv_list(os.environ.get("OTEL_RESOURCE_ATTRIBUTES", "")).get(
        "service.name"
    )


def _otel_resource_attributes_from_env() -> dict[str, str]:
    return _parse_kv_list(os.environ.get("OTEL_RESOURCE_ATTRIBUTES", ""))


def _otlp_traces_endpoint_from_env() -> str:
    """Resolve the OTLP traces endpoint per the OpenTelemetry spec.

    A per-signal ``OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`` is used verbatim; a base
    ``OTEL_EXPORTER_OTLP_ENDPOINT`` gets ``/v1/traces`` appended; otherwise the
    local-collector default is used.
    """
    endpoint = os.environ.get("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT")
    if endpoint:
        return endpoint
    base = os.environ.get("OTEL_EXPORTER_OTLP_ENDPOINT")
    if base:
        return base.rstrip("/") + "/v1/traces"
    return _DEFAULT_OTLP_TRACES_ENDPOINT


def _otlp_headers_from_env() -> dict[str, str]:
    headers = _parse_kv_list(os.environ.get("OTEL_EXPORTER_OTLP_HEADERS", ""))
    headers.update(
        _parse_kv_list(os.environ.get("OTEL_EXPORTER_OTLP_TRACES_HEADERS", ""))
    )
    return headers


def _otlp_timeout_from_env(default: int = 10000) -> int:
    for var in (
        "OTEL_EXPORTER_OTLP_TRACES_TIMEOUT",
        "OTEL_EXPORTER_OTLP_TIMEOUT",
    ):
        raw = os.environ.get(var)
        if raw:
            try:
                return int(raw)
            except ValueError:
                pass
    return default


def configure_otel_from_env(*, service_name: str = "a11") -> bool:
    """Configure native tracing from standard OpenTelemetry environment vars.

    Honors ``OTEL_SDK_DISABLED``, ``OTEL_TRACES_EXPORTER``
    (``otlp`` | ``console`` | ``none``), ``OTEL_SERVICE_NAME``,
    ``OTEL_RESOURCE_ATTRIBUTES``, and the ``OTEL_EXPORTER_OTLP_*`` endpoint,
    headers and timeout variables (per-signal ``..._TRACES_*`` overrides win).
    Only OTLP/HTTP (JSON) transport is supported, so a gRPC protocol selection
    is rejected. Returns ``True`` if a provider was installed, ``False`` if the
    environment disabled tracing.
    """
    if _env_flag("OTEL_SDK_DISABLED"):
        return False
    exporter = os.environ.get("OTEL_TRACES_EXPORTER", "otlp").strip().lower()
    if exporter in {"", "none"}:
        return False

    resolved_service = _otel_service_name_from_env() or service_name
    resource_attributes = _otel_resource_attributes_from_env() or None

    if exporter == "console":
        configure_otel(
            service_name=resolved_service,
            resource_attributes=resource_attributes,
            exporter="ostream",
        )
        return True
    if exporter != "otlp":
        raise ValueError(
            f"Unsupported OTEL_TRACES_EXPORTER {exporter!r} "
            "(expected 'otlp', 'console' or 'none')"
        )

    protocol = (
        (
            os.environ.get("OTEL_EXPORTER_OTLP_TRACES_PROTOCOL")
            or os.environ.get("OTEL_EXPORTER_OTLP_PROTOCOL")
            or ""
        )
        .strip()
        .lower()
    )
    if "grpc" in protocol:
        raise ValueError(
            "A11 exports OTLP over HTTP only; "
            "OTEL_EXPORTER_OTLP_PROTOCOL=grpc is not supported"
        )

    configure_otel(
        service_name=resolved_service,
        resource_attributes=resource_attributes,
        exporter="otlp_http",
        endpoint=_otlp_traces_endpoint_from_env(),
        headers=_otlp_headers_from_env(),
        timeout_millis=_otlp_timeout_from_env(),
    )
    return True


def configure_langfuse_from_env(
    *, host: str | None = None, service_name: str = "a11"
) -> bool:
    """Configure native OTLP export to Langfuse from ``LANGFUSE_*`` env vars.

    Reads ``LANGFUSE_PUBLIC_KEY``, ``LANGFUSE_SECRET_KEY`` and ``LANGFUSE_HOST``
    (also honoring ``OTEL_SERVICE_NAME`` / ``OTEL_RESOURCE_ATTRIBUTES``).
    Returns ``True`` when configured, ``False`` when both keys are absent, and
    raises ``ValueError`` if only one of the key pair is set.
    """
    public_key = os.environ.get("LANGFUSE_PUBLIC_KEY")
    secret_key = os.environ.get("LANGFUSE_SECRET_KEY")
    if not public_key and not secret_key:
        return False
    if not public_key or not secret_key:
        raise ValueError(
            "Both LANGFUSE_PUBLIC_KEY and LANGFUSE_SECRET_KEY must be set"
        )
    resolved_host = (
        host or os.environ.get("LANGFUSE_HOST") or "https://cloud.langfuse.com"
    )
    langfuse(
        public_key=public_key,
        secret_key=secret_key,
        host=resolved_host,
        service_name=_otel_service_name_from_env() or service_name,
        resource_attributes=_otel_resource_attributes_from_env() or None,
    )
    return True


# Introspection helpers (meaningful only with the in-memory exporter); used by
# tests.
def recorded_spans() -> list[dict]:
    return _native.obs_recorded_spans()


def clear_recorded_spans() -> None:
    _native.obs_clear_recorded_spans()


def _shutdown_at_exit() -> None:
    """Flush and tear down the provider at interpreter exit.

    Registered once at import. This runs while the runtime is still healthy, so
    the batch span processor's worker thread is joined cleanly (avoiding a
    "mutex lock failed" abort during C++ static destruction) and any buffered
    spans are exported before the process ends.
    """
    try:
        if is_configured():
            shutdown_otel()
    except Exception:  # never raise during interpreter shutdown
        pass


atexit.register(_shutdown_at_exit)


__all__ = [
    "BAGGAGE_HEADER",
    "LANGFUSE_BAGGAGE_KEYS",
    "Span",
    "TRACEPARENT_HEADER",
    "TRACESTATE_HEADER",
    "clear_recorded_spans",
    "configure_langfuse_from_env",
    "configure_otel",
    "configure_otel_from_env",
    "enable_tracing",
    "is_configured",
    "langfuse",
    "langfuse_auth_header",
    "langfuse_otlp_endpoint",
    "new_traceparent",
    "recorded_spans",
    "shutdown_otel",
    "start_span",
]

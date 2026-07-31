"""
OpenTelemetry tracing configuration for A11.
"""

from __future__ import annotations
import collections.abc
import typing

__all__: list[str] = [
    "BAGGAGE_HEADER",
    "TRACEPARENT_HEADER",
    "TRACESTATE_HEADER",
    "clear_recorded_spans",
    "configure",
    "is_configured",
    "recorded_spans",
    "shutdown",
]

def clear_recorded_spans() -> None: ...
def configure(
    service_name: str = "a11",
    resource_attributes: collections.abc.Mapping[str, str] = {},
    exporter: str = "otlp_http",
    use_simple_processor: bool = False,
    otlp_endpoint: str = "",
    otlp_headers: collections.abc.Mapping[str, str] = {},
    otlp_timeout_millis: typing.SupportsInt = 10000,
) -> None: ...
def is_configured() -> bool: ...
def recorded_spans() -> list: ...
def shutdown() -> None: ...

BAGGAGE_HEADER: str = "x-otel-baggage"
TRACEPARENT_HEADER: str = "x-otel-traceparent"
TRACESTATE_HEADER: str = "x-otel-tracestate"

# Observability

The C++ runtime emits traces over OTLP/HTTP when configured. Use Langfuse or
another OpenTelemetry backend.

See [Enable telemetry](../guides/telemetry.md) for collector configuration,
lifecycle spans, trace propagation, baggage, and shutdown.

## Configuration model

Tracing is process-wide and disabled until `configure_otel()`,
`configure_otel_from_env()`, or `langfuse()` installs a provider. Configure it
before creating sessions or starting wire streams so their complete lifetimes
are recorded. `shutdown_otel()` flushes buffered spans and tears down the
provider.

The native exporter supports OTLP over HTTP/JSON. `ostream` provides console
output, `in_memory` supports tests, and `none` disables export. Standard
`OTEL_*` variables configure endpoints, headers, service metadata, timeouts,
and exporter selection.

## Emitted spans

| Span | Activation | Recorded data |
| --- | --- | --- |
| Action name | A valid trace context is present | Action name, ID, run/call mode, status, and nested call events |
| `a11.session` | Provider configured before session creation | Session ID, lifetime, and terminal status |
| `a11.wire_stream` | Provider configured before `start` or `accept` | Stream ID, terminal status, send events, and endpoint role when defined |

Action traces follow application requests. `enable_tracing()` creates or
reuses W3C context on a root action. Nested actions become child spans, and
remote calls inject the current context into reserved action headers.

Session and stream spans form independent infrastructure traces. Their stable
`a11.session.id` and `a11.stream.id` attributes support filtering and
correlation. Each `a11.wire.send` event records action-message count,
node-fragment count, and approximate bytes; payload values are omitted.

## Baggage and span attributes

`enable_tracing(..., baggage={...})` carries W3C baggage through nested and
remote actions. `baggage_span_attributes` selects a bounded set of keys to copy
onto every span for backend filtering. Langfuse configuration selects
`langfuse.session.id` and `langfuse.user.id` by default.

Handlers can update their active action span with `set_span_name()`,
`set_span_attribute()`, `set_span_status()`, `set_span_input()`, and
`set_span_output()`. `start_span()` creates a standalone parent for work that
contains several actions, such as a complete chat turn.

::: a11.observability

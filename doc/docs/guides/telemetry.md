# Enable telemetry

A11 emits action, session, and transport spans from its C++ runtime and exports
them over OTLP/HTTP.
Configure one tracer provider when the process starts, then add trace context to
each root action. Nested and remote actions inherit that context through A11's
action headers.

This guide uses environment variables so the same application can send traces
to a local collector, an observability service, or Langfuse without changing
its runtime code.

## Configure an OTLP collector

Set a service name and the collector's HTTP endpoint before starting the
application:

```shell
export OTEL_SERVICE_NAME=research-worker
export OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318
export OTEL_RESOURCE_ATTRIBUTES=deployment.environment=development,service.version=1.4.0
```

`OTEL_EXPORTER_OTLP_ENDPOINT` is a base URL; A11 appends `/v1/traces`. Set
`OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` when the collector provides a complete
traces URL instead:

```shell
export OTEL_EXPORTER_OTLP_TRACES_ENDPOINT=https://otel.example.com/v1/traces
export OTEL_EXPORTER_OTLP_TRACES_HEADERS=authorization=Bearer%20token
```

Header values use the OpenTelemetry comma-separated format and percent
encoding. Per-signal `OTEL_EXPORTER_OTLP_TRACES_*` settings take precedence
over their `OTEL_EXPORTER_OTLP_*` equivalents.

Call the environment helper once during process startup:

```python
import a11

a11.configure_otel_from_env()
```

The helper recognizes `otlp`, `console`, and `none` through
`OTEL_TRACES_EXPORTER`. `OTEL_SDK_DISABLED=true` also disables tracing. A11's
native exporter uses OTLP over HTTP/JSON; gRPC protocol settings raise
`ValueError` during configuration.

## Observe sessions and transport streams

Once the provider is configured, sessions and `WireStream` endpoints emit
lifecycle spans automatically. They require no trace header or
`enable_tracing()` call.

| Span | Lifetime | Attributes and events |
|---|---|---|
| `a11.session` | Session creation through full completion | `a11.session.id` |
| `a11.wire_stream` | `start` or `accept` through full completion | `a11.stream.id`, and `a11.stream.role` for channel transports |

A session span has server kind. Its final status matches the session status,
including aborts and deadlines. A generated session ID is 32 hexadecimal
characters and also becomes the trace ID. A custom session ID remains
available as `a11.session.id`; a 32-character hexadecimal custom ID can pin
the trace ID for correlation with an external system.

Each stream endpoint emits an internal span and records an `a11.wire.send`
event for every admitted `WireMessage`. The event contains:

- `a11.wire.action_messages`: number of action protocol messages.
- `a11.wire.node_fragments`: number of streamed node fragments.
- `a11.wire.bytes`: approximate encoded message size.

The stream span ends after clean bilateral half-close or abort and carries the
terminal stream status. The payload and fragment values are omitted from
telemetry.

Session and stream spans form independent infrastructure traces. Use
`a11.session.id` and `a11.stream.id` to filter or join them in the telemetry
backend. An in-process stream pair shares its generated 32-character stream ID
and trace ID across both endpoint spans. WebSocket and WebRTC spans retain
their transport-prefixed stream IDs as attributes.

## Start a trace for a root action

Configuring an exporter does not trace every action automatically. Add a fresh
W3C `traceparent` to an action at the boundary where a request or background
job begins:

```python
from a11.observability import enable_tracing

enable_tracing(action)
action.run()
await action.wait()
```

`enable_tracing()` reuses trace context already present on the action. Without
one, it creates a sampled root trace. Calls made by the handler become child
spans, including calls routed through a session to another A11 process.

When an HTTP server or another instrumented component supplies W3C context,
pass it through explicitly:

```python
enable_tracing(
    action,
    traceparent=request.headers.get("traceparent"),
    tracestate=request.headers.get("tracestate"),
)
```

A11 carries these values in the reserved `x-otel-traceparent` and
`x-otel-tracestate` action headers. Use `TRACEPARENT_HEADER` and
`TRACESTATE_HEADER` when setting or reading the headers directly.

## Attach request metadata with baggage

W3C baggage propagates request-scoped values across nested and remote actions.
Pass a mapping to `enable_tracing()`; A11 percent-encodes it for the wire:

```python
enable_tracing(
    action,
    baggage={
        "tenant.id": tenant_id,
        "langfuse.session.id": conversation_id,
        "langfuse.user.id": user_id,
    },
)
```

Baggage remains propagation data unless its keys are selected as span
attributes. Configure that promotion when the backend needs the values for
search or grouping:

```python
a11.configure_otel(
    service_name="research-worker",
    exporter="otlp_http",
    endpoint="https://otel.example.com/v1/traces",
    baggage_span_attributes=["tenant.id"],
)
```

Promote a bounded set of stable identifiers. Avoid secrets and high-volume
payloads because promoted values appear on every span in the trace.

## Group work in a standalone span

Use `start_span()` when the parent operation is broader than one action, such
as a complete chat turn or scheduled job. Pass its trace context into the root
action:

```python
from a11.observability import enable_tracing, start_span

with start_span("chat turn", kind="server") as turn:
    turn.set_attribute("chat.session.id", conversation_id)
    turn.set_input(user_message)

    enable_tracing(action, traceparent=turn.traceparent())
    action.run()
    await action.wait()

    turn.set_output(reply)
```

Action handlers can refine their current span with `set_span_name()`,
`set_span_attribute()`, `set_span_status()`, `set_span_input()`, and
`set_span_output()`. These methods have no effect on an untraced action.

## Send traces to Langfuse

Langfuse accepts the same native OTLP/HTTP export. Set its credentials and
configure the provider at startup:

```shell
export LANGFUSE_PUBLIC_KEY=pk-lf-...
export LANGFUSE_SECRET_KEY=sk-lf-...
export LANGFUSE_HOST=https://cloud.langfuse.com
export OTEL_SERVICE_NAME=agent-api
```

```python
import a11

a11.configure_langfuse_from_env()
```

`LANGFUSE_HOST` can point to a self-hosted instance. The helper promotes
`langfuse.session.id` and `langfuse.user.id` baggage to span attributes, so the
baggage example above groups spans by conversation and user in Langfuse.

For explicit configuration, call
`a11.langfuse(public_key=..., secret_key=..., host=...)`.

## Flush before process shutdown

A11 registers an interpreter exit hook that flushes buffered spans. Services
with an explicit shutdown path can flush earlier:

```python
try:
    await serve()
finally:
    a11.shutdown_otel()
```

Call `shutdown_otel()` after accepting work has stopped and traced actions have
finished. The next configuration call can then install a new provider.

See the [observability API](../api/observability.md) for the complete
configuration surface and span methods.

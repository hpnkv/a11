# Export native telemetry {#cpp_telemetry}

A11 emits action, session, and wire-stream spans from the native runtime.
Configure one process-wide provider before creating those objects, then attach
trace context at the boundary where an application request begins.

## Configure OTLP/HTTP

The native exporter accepts a complete traces URL. Authentication headers and
resource attributes remain application configuration, so the same binary can
target a local collector or a hosted backend.

```cpp
a11::obs::ProviderOptions telemetry{
    .service_name = "research-worker",
    .resource_attributes = {
        {"deployment.environment", "production"},
        {"service.version", "1.4.0"},
    },
    .exporter = a11::obs::ExporterKind::kOtlpHttp,
    .otlp_endpoint = "https://otel.example.com/v1/traces",
    .otlp_headers = {{"authorization", "Bearer token"}},
    .otlp_timeout_millis = 10000,
    .baggage_span_attributes = {"tenant.id"},
};
ABSL_RETURN_IF_ERROR(a11::obs::Configure(telemetry));
```

Build A11 with OTLP/HTTP support when selecting `kOtlpHttp`. Use `kOstream` for
local inspection and `kInMemory` for deterministic tests.

## Trace a root action

Configuring the provider records session and stream lifecycles automatically.
Actions require W3C context. Start a span for the surrounding request, inject
its context into the action headers, and run the action normally.

```cpp
a11::obs::Span request = a11::obs::Tracer::StartRootSpan(
    "POST /research", a11::obs::SpanKind::kServer);
request.SetAttribute("tenant.id", tenant_id);

a11::data::ByteMap trace_headers;
ABSL_RETURN_IF_ERROR(request.InjectContext(trace_headers));
for (auto& [name, value] : trace_headers) {
  ABSL_RETURN_IF_ERROR(action->SetHeader(name, std::move(value)));
}

ABSL_RETURN_IF_ERROR(action->Run().status());
const absl::Status completed = action->Wait().Await().status();
request.SetStatus(completed);
ABSL_RETURN_IF_ERROR(completed);
```

The action becomes a child of `request`. Nested actions inherit that context,
and remote calls carry it in A11's reserved trace headers. A service receiving
the call continues the same trace without OpenTelemetry thread-local state.

## Continue incoming context

An HTTP or messaging boundary can provide a decoded `TraceContext`. Pass it to
`Tracer::StartSpan` and inject the resulting context into downstream work:

```cpp
ABSL_ASSIGN_OR_RETURN(
    std::optional<a11::obs::TraceContext> parent,
    a11::obs::ExtractTraceContext(incoming_action_headers));
a11::obs::Span span = a11::obs::Tracer::StartSpan(
    "handle request", a11::obs::SpanKind::kServer,
    parent.has_value() ? &*parent : nullptr);
```

Malformed trace headers return an error rather than detaching work from the
requested trace.

## Session and stream spans

Once configured, the runtime emits these infrastructure spans without action
headers:

| Span | Lifetime | Data |
| --- | --- | --- |
| `a11.session` | Creation through completion | ID and terminal status |
| `a11.wire_stream` | Startup through bilateral completion | ID, role, status, send events |

Each `a11.wire.send` event records action-message count, node-fragment count,
and approximate bytes. Payload values are omitted. Session and stream spans use
independent infrastructure traces; correlate them with `a11.session.id` and
`a11.stream.id`.

## Flush during shutdown

Stop accepting work, drain services, and finish traced actions before shutting
down the provider:

```cpp
ABSL_RETURN_IF_ERROR(service->StopAccepting());
ABSL_RETURN_IF_ERROR(service->Drain(absl::Seconds(30)).Await().status());
a11::obs::Shutdown();
```

`Shutdown()` flushes buffered spans and returns tracing to its unconfigured
state. A later `Configure()` call can install a new provider.

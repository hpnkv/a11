# Copyright 2026 The A11 Authors.

import base64
import os
import subprocess
import sys
import textwrap

import pytest

import a11
from a11 import observability as obs
from a11.actions import Action, ActionSchema
from a11.status import Status, StatusCode, StatusException

_TRACE_ID = "0af7651916cd43dd8448eb211c80319c"
_TRACEPARENT = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"


def test_langfuse_endpoint_and_auth_helpers():
    assert (
        obs.langfuse_otlp_endpoint("https://cloud.langfuse.com/")
        == "https://cloud.langfuse.com/api/public/otel/v1/traces"
    )
    header = obs.langfuse_auth_header("pk-lf-1", "sk-lf-2")
    assert header.startswith("Basic ")
    decoded = base64.b64decode(header.split(" ", 1)[1]).decode()
    assert decoded == "pk-lf-1:sk-lf-2"


def test_reserved_header_names_match_default_action_headers():
    assert obs.TRACEPARENT_HEADER == "x-otel-traceparent"
    assert obs.TRACESTATE_HEADER == "x-otel-tracestate"
    assert obs.BAGGAGE_HEADER == "x-otel-baggage"


def test_configure_and_shutdown_toggle_state():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    try:
        assert obs.is_configured()
    finally:
        obs.shutdown_otel()
    assert not obs.is_configured()


def test_otlp_without_endpoint_raises():
    with pytest.raises(StatusException):
        obs.configure_otel(exporter="otlp_http", endpoint="")


def test_top_level_reexports_present():
    assert a11.configure_otel is obs.configure_otel
    assert a11.langfuse is obs.langfuse
    assert a11.shutdown_otel is obs.shutdown_otel


@pytest.fixture
def clean_otel_env(monkeypatch):
    """Remove all OTEL_*/LANGFUSE_* vars so each test controls the environment,
    and ensure tracing is shut down afterwards."""
    for name in list(os.environ):
        if name.startswith(("OTEL_", "LANGFUSE_")):
            monkeypatch.delenv(name, raising=False)
    yield monkeypatch
    obs.shutdown_otel()


def test_otlp_endpoint_resolution_from_env(clean_otel_env):
    mp = clean_otel_env
    # Per-signal endpoint wins verbatim.
    mp.setenv("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "https://x/v1/traces")
    mp.setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "https://base")
    assert obs._otlp_traces_endpoint_from_env() == "https://x/v1/traces"
    # Base endpoint gets /v1/traces appended.
    mp.delenv("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT")
    assert obs._otlp_traces_endpoint_from_env() == "https://base/v1/traces"
    # Default local collector.
    mp.delenv("OTEL_EXPORTER_OTLP_ENDPOINT")
    assert (
        obs._otlp_traces_endpoint_from_env()
        == "http://localhost:4318/v1/traces"
    )


def test_otlp_header_and_resource_parsing(clean_otel_env):
    mp = clean_otel_env
    mp.setenv("OTEL_EXPORTER_OTLP_HEADERS", "api-key=secret,x-tenant=acme%20co")
    mp.setenv("OTEL_EXPORTER_OTLP_TRACES_HEADERS", "x-tenant=override")
    headers = obs._otlp_headers_from_env()
    assert headers["api-key"] == "secret"
    assert headers["x-tenant"] == "override"  # per-signal wins
    mp.setenv(
        "OTEL_RESOURCE_ATTRIBUTES",
        "service.name=svc,deployment.environment=prod",
    )
    assert obs._otel_service_name_from_env() == "svc"


def test_configure_otel_from_env_disabled_and_none(clean_otel_env):
    mp = clean_otel_env
    mp.setenv("OTEL_SDK_DISABLED", "true")
    assert obs.configure_otel_from_env() is False
    assert not obs.is_configured()
    mp.delenv("OTEL_SDK_DISABLED")
    mp.setenv("OTEL_TRACES_EXPORTER", "none")
    assert obs.configure_otel_from_env() is False
    assert not obs.is_configured()


def test_configure_otel_from_env_otlp(clean_otel_env):
    mp = clean_otel_env
    mp.setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:4318")
    mp.setenv("OTEL_EXPORTER_OTLP_HEADERS", "authorization=Bearer%20tok")
    assert obs.configure_otel_from_env() is True
    assert obs.is_configured()


def test_configure_otel_from_env_rejects_grpc(clean_otel_env):
    mp = clean_otel_env
    mp.setenv("OTEL_EXPORTER_OTLP_PROTOCOL", "grpc")
    with pytest.raises(ValueError):
        obs.configure_otel_from_env()


def test_configure_langfuse_from_env(clean_otel_env):
    mp = clean_otel_env
    # Absent keys -> no-op.
    assert obs.configure_langfuse_from_env() is False
    assert not obs.is_configured()
    # Only one key -> error.
    mp.setenv("LANGFUSE_PUBLIC_KEY", "pk-lf-1")
    with pytest.raises(ValueError):
        obs.configure_langfuse_from_env()
    # Both keys -> configured.
    mp.setenv("LANGFUSE_SECRET_KEY", "sk-lf-2")
    mp.setenv("LANGFUSE_HOST", "https://us.cloud.langfuse.com")
    assert obs.configure_langfuse_from_env() is True
    assert obs.is_configured()


def test_env_helpers_reexported_at_top_level():
    assert a11.configure_otel_from_env is obs.configure_otel_from_env
    assert a11.configure_langfuse_from_env is obs.configure_langfuse_from_env


@pytest.mark.asyncio
async def test_baggage_key_is_promoted_to_span_attribute():
    """A langfuse.session.id set via x-otel-baggage lands on the span as an
    attribute when configured in baggage_span_attributes."""
    obs.configure_otel(
        exporter="in_memory",
        use_simple_processor=True,
        baggage_span_attributes=["langfuse.session.id"],
    )
    obs.clear_recorded_spans()
    try:

        async def handler(action: Action) -> None:
            return None

        action = Action(ActionSchema(name="pybaggage"), handler=handler)
        action.set_header(obs.TRACEPARENT_HEADER, _TRACEPARENT.encode())
        action.set_header(obs.BAGGAGE_HEADER, b"langfuse.session.id=sess-9")
        action.run()
        await action.wait()

        span = next(s for s in obs.recorded_spans() if s["name"] == "pybaggage")
        assert span["attributes"].get("langfuse.session.id") == "sess-9"
    finally:
        obs.shutdown_otel()


@pytest.mark.asyncio
async def test_action_set_span_name_overrides_recorded_name():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    obs.clear_recorded_spans()
    try:

        async def handler(action: Action) -> None:
            action.set_span_name("renamed")

        action = Action(ActionSchema(name="original"), handler=handler)
        action.set_header(obs.TRACEPARENT_HEADER, _TRACEPARENT.encode())
        action.run()
        await action.wait()

        names = [s["name"] for s in obs.recorded_spans()]
        assert "renamed" in names
        assert "original" not in names
    finally:
        obs.shutdown_otel()


def test_start_span_records_name_attributes_and_io():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    obs.clear_recorded_spans()
    try:
        with obs.start_span("root", kind="server") as span:
            span.set_name("A11 Chat").set_attribute("k", "v")
            span.set_input({"a": 1})
            span.set_output("done")
        s = next(x for x in obs.recorded_spans() if x["name"] == "A11 Chat")
        assert s["attributes"]["k"] == "v"
        assert s["attributes"]["langfuse.observation.input"] == '{"a": 1}'
        assert s["attributes"]["langfuse.observation.output"] == "done"
    finally:
        obs.shutdown_otel()


@pytest.mark.asyncio
async def test_start_span_parents_child_action():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    obs.clear_recorded_spans()
    try:
        chat = obs.start_span("A11 Chat", kind="server")
        tp = chat.traceparent()
        _, chat_trace, chat_span, _flags = tp.split("-")

        async def handler(action: Action) -> None:
            return None

        action = Action(ActionSchema(name="turn"), handler=handler)
        obs.enable_tracing(action, traceparent=tp)
        action.run()
        await action.wait()
        chat.end()

        spans = obs.recorded_spans()
        parent = next(s for s in spans if s["name"] == "A11 Chat")
        child = next(s for s in spans if s["name"] == "turn")
        assert parent["trace_id"] == chat_trace
        assert parent["span_id"] == chat_span
        assert child["trace_id"] == chat_trace
        assert child["parent_span_id"] == chat_span
    finally:
        obs.shutdown_otel()


@pytest.mark.asyncio
async def test_action_set_span_status_overrides_auto_status():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    obs.clear_recorded_spans()
    try:

        async def handler(action: Action) -> None:
            # Force an error status even though the action itself succeeds.
            action.set_span_status("error", "boom")

        action = Action(ActionSchema(name="st"), handler=handler)
        action.set_header(obs.TRACEPARENT_HEADER, _TRACEPARENT.encode())
        action.run()
        await action.wait()

        s = next(x for x in obs.recorded_spans() if x["name"] == "st")
        assert s["status_code"] == 2  # error, not the auto-ok
        assert s["status_description"] == "boom"
    finally:
        obs.shutdown_otel()


@pytest.mark.asyncio
async def test_failing_action_records_error_type_and_description():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    obs.clear_recorded_spans()
    try:

        async def handler(action: Action) -> None:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT, message="bad input"
            ).to_exception()

        action = Action(ActionSchema(name="boom"), handler=handler)
        action.set_header(obs.TRACEPARENT_HEADER, _TRACEPARENT.encode())
        action.run()
        with pytest.raises(StatusException):
            await action.wait()

        s = next(x for x in obs.recorded_spans() if x["name"] == "boom")
        assert s["status_code"] == 2
        assert s["status_description"] == "bad input"
        assert s["attributes"]["error.type"] == "INVALID_ARGUMENT"
    finally:
        obs.shutdown_otel()


def test_standalone_span_set_status():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    obs.clear_recorded_spans()
    try:
        span = obs.start_span("root")
        span.set_status("error", "nope")
        span.end()
        s = next(x for x in obs.recorded_spans() if x["name"] == "root")
        assert s["status_code"] == 2
        assert s["status_description"] == "nope"
    finally:
        obs.shutdown_otel()


def test_invalid_span_status_raises():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    try:
        span = obs.start_span("bad")
        with pytest.raises(StatusException):
            span.set_status("bogus")
        span.end()
    finally:
        obs.shutdown_otel()


def test_new_traceparent_format_and_uniqueness():
    tp = obs.new_traceparent()
    version, trace_id, span_id, flags = tp.split("-")
    assert version == "00"
    assert len(trace_id) == 32 and int(trace_id, 16) != 0
    assert len(span_id) == 16 and int(span_id, 16) != 0
    assert flags == "01"
    assert obs.new_traceparent(sampled=False).endswith("-00")
    assert obs.new_traceparent() != obs.new_traceparent()


def test_enable_tracing_mints_reuses_and_returns_action():
    action = Action(ActionSchema(name="et"))
    # Mints when absent, returns the action for chaining.
    assert obs.enable_tracing(action) is action
    minted = action.get_header(obs.TRACEPARENT_HEADER, decode=True)
    assert minted and minted.startswith("00-")
    # Reuses the existing traceparent on a second call.
    obs.enable_tracing(action)
    assert action.get_header(obs.TRACEPARENT_HEADER, decode=True) == minted
    # Explicit traceparent + baggage mapping.
    obs.enable_tracing(
        action, traceparent=_TRACEPARENT, baggage={"langfuse.session.id": "s1"}
    )
    assert (
        action.get_header(obs.TRACEPARENT_HEADER, decode=True) == _TRACEPARENT
    )
    assert (
        action.get_header(obs.BAGGAGE_HEADER, decode=True)
        == "langfuse.session.id=s1"
    )


@pytest.mark.asyncio
async def test_enable_tracing_end_to_end_with_baggage():
    obs.configure_otel(
        exporter="in_memory",
        use_simple_processor=True,
        baggage_span_attributes=["langfuse.session.id"],
    )
    obs.clear_recorded_spans()
    try:

        async def handler(action: Action) -> None:
            return None

        action = Action(ActionSchema(name="et2"), handler=handler)
        tp = obs.new_traceparent()
        obs.enable_tracing(
            action, traceparent=tp, baggage={"langfuse.session.id": "sess-7"}
        )
        action.run()
        await action.wait()

        span = next(s for s in obs.recorded_spans() if s["name"] == "et2")
        assert span["trace_id"] == tp.split("-")[1]
        assert span["attributes"].get("langfuse.session.id") == "sess-7"
    finally:
        obs.shutdown_otel()


def test_tracing_helpers_reexported_at_top_level():
    assert a11.enable_tracing is obs.enable_tracing
    assert a11.new_traceparent is obs.new_traceparent


@pytest.mark.asyncio
async def test_set_span_attribute_and_input_output():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    obs.clear_recorded_spans()
    try:

        async def handler(action: Action) -> None:
            action.set_span_attribute("custom.str", "hi")
            action.set_span_attribute("custom.int", 7)
            action.set_span_attribute("custom.bool", True)
            action.set_span_input({"q": "hello"})
            action.set_span_output("world")

        action = Action(ActionSchema(name="io"), handler=handler)
        action.set_header(obs.TRACEPARENT_HEADER, _TRACEPARENT.encode())
        action.run()
        await action.wait()

        attrs = next(s for s in obs.recorded_spans() if s["name"] == "io")[
            "attributes"
        ]
        assert attrs["custom.str"] == "hi"
        assert attrs["custom.int"] == "7"  # recorded_spans stringifies values
        assert attrs["custom.bool"] == "true"
        assert attrs["langfuse.observation.input"] == '{"q": "hello"}'
        assert attrs["langfuse.observation.output"] == "world"
    finally:
        obs.shutdown_otel()


@pytest.mark.asyncio
async def test_set_span_attribute_is_noop_when_untraced():
    # No tracing configured: setting attributes must not raise.
    async def handler(action: Action) -> None:
        action.set_span_attribute("k", "v")
        action.set_span_input({"a": 1})
        action.set_span_output("b")

    action = Action(ActionSchema(name="io-untraced"), handler=handler)
    action.run()
    await action.wait()
    assert action.get_status().is_ok()


@pytest.mark.asyncio
async def test_action_exposes_trace_and_span_ids():
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    obs.clear_recorded_spans()
    seen = {}
    try:

        async def handler(action: Action) -> None:
            seen["trace_id"] = action.trace_id
            seen["span_id"] = action.span_id

        action = Action(ActionSchema(name="ids"), handler=handler)
        action.set_header(obs.TRACEPARENT_HEADER, _TRACEPARENT.encode())
        action.run()
        await action.wait()

        assert seen["trace_id"] == _TRACE_ID
        assert len(seen["span_id"]) == 16
        span = next(s for s in obs.recorded_spans() if s["name"] == "ids")
        assert span["span_id"] == seen["span_id"]
    finally:
        obs.shutdown_otel()


@pytest.mark.asyncio
async def test_untraced_action_has_empty_ids():
    seen = {}

    async def handler(action: Action) -> None:
        seen["trace_id"] = action.trace_id
        seen["span_id"] = action.span_id

    action = Action(ActionSchema(name="notrace"), handler=handler)
    action.run()
    await action.wait()
    assert seen["trace_id"] == ""
    assert seen["span_id"] == ""


def test_process_exits_cleanly_with_batch_otlp_exporter():
    """Regression: configuring the batch OTLP exporter and exiting without an
    explicit shutdown must not abort ("mutex lock failed") at teardown. The
    atexit hook flushes and joins the batch thread cleanly."""
    script = textwrap.dedent("""
        import asyncio
        import a11
        from a11 import observability as obs
        from a11.actions import Action, ActionSchema

        # Batch processor (default) at an unreachable endpoint: the flush fails
        # fast but teardown must stay clean.
        obs.configure_otel(
            exporter="otlp_http",
            endpoint="http://127.0.0.1:1/v1/traces",
            timeout_millis=200,
        )

        async def main():
            async def handler(action):
                return None
            act = Action(ActionSchema(name="exit-clean"), handler=handler)
            obs.enable_tracing(act)
            act.run()
            await act.wait()

        asyncio.run(main())
        # Deliberately no shutdown_otel(): the atexit hook must handle it.
        """)
    result = subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert (
        result.returncode == 0
    ), f"non-clean exit ({result.returncode}); stderr:\n{result.stderr}"
    assert "libc++abi" not in result.stderr
    assert "mutex lock failed" not in result.stderr


@pytest.mark.asyncio
async def test_python_action_run_emits_native_span_from_traceparent():
    """End-to-end: a Python handler run under a traceparent produces a native
    server span in the same trace, with no Python code on the export path."""
    obs.configure_otel(exporter="in_memory", use_simple_processor=True)
    obs.clear_recorded_spans()
    try:

        async def handler(action: Action) -> None:
            return None

        action = Action(ActionSchema(name="pytrace"), handler=handler)
        action.set_header(obs.TRACEPARENT_HEADER, _TRACEPARENT.encode())
        assert action.run() is action
        await action.wait()

        spans = obs.recorded_spans()
        matching = [s for s in spans if s["name"] == "pytrace"]
        assert matching, f"no pytrace span in {[s['name'] for s in spans]}"
        span = matching[0]
        assert span["trace_id"] == _TRACE_ID
        assert span["status_code"] == 1  # ok
    finally:
        obs.shutdown_otel()

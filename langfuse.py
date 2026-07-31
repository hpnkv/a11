import base64, os, time, requests

host = os.environ.get("LANGFUSE_HOST", "https://cloud.langfuse.com").rstrip("/")
auth = base64.b64encode(
    f'{os.environ["LANGFUSE_PUBLIC_KEY"]}:{os.environ["LANGFUSE_SECRET_KEY"]}'.encode()
).decode()
now = time.time_ns()
payload = {
    "resourceSpans": [{
        "resource": {"attributes": [
            {"key": "service.name", "value": {"stringValue": "otlp-smoke"}}]},
        "scopeSpans": [{
            "scope": {"name": "smoke", "version": "1"},
            "spans": [{
                "traceId": "0af7651916cd43dd8448eb211c80319c",
                "spanId": "b7ad6b7169203331",
                "name": "otlp-smoke-span",
                "kind": 2,
                "startTimeUnixNano": str(now),
                "endTimeUnixNano": str(now + 1_000_000),
                "attributes": [
                    {"key": "langfuse.session.id", "value": {"stringValue": "smoke-session-1"}}],
                "status": {"code": 1},
            }],
        }],
    }]
}
r = requests.post(
    f"{host}/api/public/otel/v1/traces",
    headers={"Content-Type": "application/json", "Authorization": f"Basic {auth}"},
    json=payload, timeout=10,
)
print(r.status_code, r.text[:300])   # expect 2xx

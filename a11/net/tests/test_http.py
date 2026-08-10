# Copyright 2026 The A11 Authors.

"""The by-URL HTTP helpers, driven from asyncio against a local server.

The C++ suites cover redirects, digests and the atomic rename; what matters here
is the binding layer: that the awaitables resolve on the event loop, that a
Python progress callback is invoked from the fetching fiber without deadlocking
the loop, and that failures arrive as A11 statuses.
"""

from __future__ import annotations

import contextlib
import hashlib
from pathlib import Path

import pytest
from a11 import net, timing
from a11.net import http
from a11.status import StatusCode, StatusException

_BODY = b"a11-python-download"
_SHA1 = hashlib.sha1(_BODY).hexdigest()

@contextlib.contextmanager
def _server():
    """A local HTTP server answering /body, /missing and /redirect.

    Built inside the running loop rather than in a fixture: the native server
    captures the ambient asyncio loop when it is created, and one created before
    the loop exists has nowhere to dispatch its Python handler.
    """

    def handle(request, response):
        # The writer's methods are synchronous; the handler may still be a
        # coroutine function, but nothing here needs to await.
        if request.path == "/body":
            response.send_response(
                200, [("content-length", str(len(_BODY)))], _BODY
            )
        elif request.path == "/redirect":
            response.send_response(302, [("location", "/body")], b"")
        else:
            response.send_response(404, [], b"nope")

    async def handler(request, response):
        handle(request, response)

    created = net.Http2Server.create("127.0.0.1", 0, handler)
    try:
        yield created
    finally:
        created.stop()


def _url(server, path: str) -> str:
    return f"http://127.0.0.1:{server.port}{path}"


def _fast(options: http.FetchOptions | None = None) -> http.FetchOptions:
    """Fetch options with a short timeout, so a stall fails fast."""
    resolved = options if options is not None else http.FetchOptions()
    resolved.timeout = timing.Duration.seconds(10)
    return resolved


def test_parse_url_exposes_the_parts_a_caller_dials_with():
    url = http.parse_url("wss://example.com/a11?v=1")
    assert url.scheme == "wss"
    assert url.host == "example.com"
    assert url.port == 443
    assert url.secure
    assert url.target == "/a11?v=1"
    # A default port is not spelled out in an authority.
    assert url.authority == "example.com"
    assert str(url) == "wss://example.com/a11?v=1"


def test_parse_url_raises_a_status_for_a_malformed_url():
    with pytest.raises(StatusException) as caught:
        http.parse_url("nonsense")
    assert caught.value.status.code == StatusCode.INVALID_ARGUMENT


def test_resolve_url_reference_follows_a_location_header():
    base = http.parse_url("https://host.example/a/b/page")
    assert str(http.resolve_url_reference(base, "/c")) == "https://host.example/c"
    assert (
        str(http.resolve_url_reference(base, "sibling"))
        == "https://host.example/a/b/sibling"
    )


@pytest.mark.asyncio
async def test_fetch_resolves_on_the_event_loop():
    with _server() as server:
        response = await http.fetch(_url(server, "/body"), _fast())
    assert response.head.status == 200
    assert response.body == _BODY


@pytest.mark.asyncio
async def test_fetch_follows_a_redirect():
    with _server() as server:
        response = await http.fetch(_url(server, "/redirect"), _fast())
    assert response.body == _BODY


@pytest.mark.asyncio
async def test_fetch_maps_an_error_response_onto_a_status():
    with _server() as server:
        with pytest.raises(StatusException) as caught:
            await http.fetch(_url(server, "/missing"), _fast())
    assert caught.value.status.code == StatusCode.NOT_FOUND


@pytest.mark.asyncio
async def test_download_verifies_and_reports_progress(tmp_path: Path):
    seen: list[tuple[int, int]] = []
    destination = tmp_path / "models" / "artifact.bin"

    with _server() as server:
        path = await http.download(
            _url(server, "/body"),
            destination=destination,
            expected_sha1=_SHA1,
            on_progress=lambda done, total: seen.append((done, total)),
        )

    assert path == destination
    assert path.read_bytes() == _BODY
    # The callback runs on the fetching fiber, which has to take the GIL to do
    # it; that it ran at all is the thing worth pinning.
    assert seen
    assert seen[-1] == (len(_BODY), len(_BODY))
    # Nothing but the finished file: no temporary was left behind.
    assert [entry.name for entry in path.parent.iterdir()] == ["artifact.bin"]


@pytest.mark.asyncio
async def test_download_is_a_cache_hit_the_second_time(tmp_path: Path):
    destination = tmp_path / "artifact.bin"
    with _server() as server:
        for _ in range(2):
            path = await http.download(
                _url(server, "/body"),
                destination=destination,
                expected_sha1=_SHA1,
            )
            assert path.read_bytes() == _BODY


@pytest.mark.asyncio
async def test_a_digest_mismatch_fails_and_leaves_nothing(tmp_path: Path):
    destination = tmp_path / "artifact.bin"
    with _server() as server, pytest.raises(StatusException) as caught:
        await http.download(
            _url(server, "/body"),
            destination=destination,
            expected_sha1="0" * 40,
        )
    assert caught.value.status.code == StatusCode.DATA_LOSS
    assert not destination.exists()
    assert list(tmp_path.iterdir()) == []


@pytest.mark.asyncio
async def test_a_raising_progress_callback_does_not_fail_the_download(
    tmp_path: Path,
):
    """A broken progress bar is not a reason to lose a good download."""

    def explode(done: int, total: int) -> None:
        raise RuntimeError("bar is broken")

    with _server() as server, pytest.warns(RuntimeWarning):
        path = await http.download(
            _url(server, "/body"),
            destination=tmp_path / "artifact.bin",
            on_progress=explode,
        )
    assert path.read_bytes() == _BODY


def test_file_sha1_accepts_a_path(tmp_path: Path):
    target = tmp_path / "bytes"
    target.write_bytes(_BODY)
    assert http.file_sha1(target) == _SHA1


@pytest.mark.asyncio
async def test_download_rejects_unknown_overrides(tmp_path: Path):
    with pytest.raises(TypeError):
        await http.download(
            "http://127.0.0.1:1/x",
            destination=tmp_path / "x",
            nonsense=True,
        )

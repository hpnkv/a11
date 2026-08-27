"""URL parsing, one-shot HTTP requests, and verified downloads.

Where [a11.net.http2][a11.net.http2] exposes a connection you drive, these are
the by-URL conveniences built on top of it: `fetch` for a whole response,
`download` for a file that is either absent or complete and correct, and
`parse_url` for the parser the WebSocket and SSE transports dial with.

Both `fetch` and `download` are awaitable and do their work on A11's fiber pool,
so neither blocks the event loop. TLS is enabled from the URL scheme, redirects
are followed, and a 4xx or 5xx becomes a
[StatusException][a11.status.StatusException] carrying the mapped code.

Examples:
    Fetch a document:

    ```python
    response = await a11.net.http.fetch("https://example.com/index.html")
    print(response.head.status, len(response.body))
    ```

    Cache a model and report download progress:

    ```python
    options = a11.net.http.DownloadOptions()
    options.destination = str(cache_dir / "ggml-tiny.en.bin")
    options.expected_sha1 = "c78c86eb1e8b072bbdd0e9256b8688ee5b8b1e78"
    options.on_progress = lambda done, total: bar.update(done, total)
    path = await a11.net.http.download(url, options)
    ```
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import TYPE_CHECKING

from a11 import _native

from a11._native import DownloadOptions
from a11._native import FetchOptions
from a11._native import ParsedUrl

if TYPE_CHECKING:
    from a11._native import HttpResponse

ParsedUrl.__module__ = __name__
FetchOptions.__module__ = __name__
DownloadOptions.__module__ = __name__

parse_url = _native.parse_url
resolve_url_reference = _native.resolve_url_reference
fetch = _native.fetch


async def download(
    url: str, options: DownloadOptions | None = None, **overrides
) -> Path:
    """Download ``url`` to a verified file, returning its path.

    A thin wrapper over the native call that accepts the destination as any
    path-like and hands back a `Path` rather than a string.

    Args:
        url: Absolute ``http``/``https`` URL.
        options: Prebuilt options, or ``None`` to build them from
            ``overrides``.
        **overrides: ``destination``, ``expected_sha1``, or ``on_progress``,
            applied on top of ``options``.

    Returns:
        The destination path.

    Raises:
        StatusException: On a malformed URL, a failed request, or a digest that
            does not match the download. A failure never leaves a partial file.
    """
    resolved = options if options is not None else DownloadOptions()
    if "destination" in overrides:
        resolved.destination = os.fspath(overrides.pop("destination"))
    if "expected_sha1" in overrides:
        resolved.expected_sha1 = overrides.pop("expected_sha1")
    if "on_progress" in overrides:
        resolved.on_progress = overrides.pop("on_progress")
    if overrides:
        raise TypeError(f"unexpected options: {', '.join(sorted(overrides))}")
    return Path(await _native.download(url, resolved))


def file_sha1(path) -> str:
    """Compute the SHA-1 of a file as lowercase hex.

    Blocks, so call it from a worker thread when it matters; it is here for the
    caller checking a cache it did not populate.
    """
    return _native.file_sha1(os.fspath(path))


__all__ = [
    "DownloadOptions",
    "FetchOptions",
    "ParsedUrl",
    "download",
    "fetch",
    "file_sha1",
    "parse_url",
    "resolve_url_reference",
]

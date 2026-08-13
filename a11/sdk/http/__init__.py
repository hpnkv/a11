"""HTTP as A11 Actions: one protocol-faithful, one shaped like `fetch()`.

Two Actions over one engine in C++:

* ``make_http_request`` -- HTTP with nothing hidden. Every concern the protocol
  keeps separate gets a port of its own: the status, the header fields, the
  body, the trailer section, the redirect chain, the responses the server
  pushed, and how the connection was carried.
* ``web-fetch`` -- the same machinery with the protocol turned down: a status, a
  header map, and the body as text, as JSON, as bytes, or decoded into a stream
  of items.

Why an Action rather than a function. An ordinary HTTP client hands back one
`Response` object because its language gives it nothing better to hand back:
headers, body and trailers are one value that is only complete at the end. An
A11 port is a stream, and there is no reason for these to share one -- so they
do not. A caller can branch on the status while the body is still arriving, give
the body to a parser and the trailers to a checksum verifier, and read pushed
responses off a port that a `fetch()`-shaped API has no way to express.

Register them on a registry:

```python
from a11.actions import ActionRegistry
from a11.sdk import http

registry = ActionRegistry()
http.register(registry)
```

Or drive one directly, which does the port wiring for you:

```python
from a11.sdk import http

async with await http.fetch("https://example.com/data.json") as response:
    print(await response.status())
    for row in await response.json():
        ...
```

Connections are shared while they are in use and closed the moment they are not:
a request to a peer another request is already talking to joins that connection
(HTTP/2 multiplexes), and nothing is kept open with no work on it. See
[a11.sdk.http.actions][a11.sdk.http.actions] for the schemas and
[a11.sdk.http.client][a11.sdk.http.client] for the Python-shaped surface.
"""

from __future__ import annotations

from a11.sdk.http.actions import (
    HTTP_ACTIONS,
    MAKE_HTTP_REQUEST,
    MAKE_HTTP_REQUEST_HANDLER,
    MAKE_HTTP_REQUEST_SCHEMA,
    WEB_FETCH,
    WEB_FETCH_HANDLER,
    WEB_FETCH_SCHEMA,
    register,
)
from a11.sdk.http.client import Response, fetch, request

__all__ = [
    "HTTP_ACTIONS",
    "MAKE_HTTP_REQUEST",
    "MAKE_HTTP_REQUEST_HANDLER",
    "MAKE_HTTP_REQUEST_SCHEMA",
    "WEB_FETCH",
    "WEB_FETCH_HANDLER",
    "WEB_FETCH_SCHEMA",
    "Response",
    "fetch",
    "register",
    "request",
]

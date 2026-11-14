# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Register or call A11's native HTTP actions.

``make_http_request`` exposes status, headers, body, trailers, redirects,
pushed responses and transport details on separate ports. Callers can read
status while the body streams. ``web-fetch`` returns a simpler status, header
map and body representation. ``web-render`` returns WebKit HTML and an optional
bounded PNG.

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

Active requests to the same peer share a connection through HTTP/2 multiplexing.
Idle connections close. See
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
    WEB_RENDER,
    WEB_RENDER_HANDLER,
    WEB_RENDER_SCHEMA,
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
    "WEB_RENDER",
    "WEB_RENDER_HANDLER",
    "WEB_RENDER_SCHEMA",
    "Response",
    "fetch",
    "register",
    "request",
]

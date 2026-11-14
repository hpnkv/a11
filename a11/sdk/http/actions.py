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

"""Register A11's native HTTP actions.

``make_http_request`` exposes protocol details on separate output ports;
``web-fetch`` provides a simpler result; ``web-render`` returns WebKit HTML
and an optional PNG.

Install them on a registry with [`register`][a11.sdk.http.actions.register]:

```python
from a11.actions import ActionRegistry
from a11.sdk.http import actions

registry = ActionRegistry()
actions.register(registry)
```

Each Action's schema and handler are also importable on their own:

```python
from a11.sdk.http.actions import WEB_FETCH, WEB_FETCH_HANDLER, WEB_FETCH_SCHEMA

registry.register(WEB_FETCH, WEB_FETCH_SCHEMA, WEB_FETCH_HANDLER)
```

The exported handlers are native handles accepted by `ActionRegistry.register`.

For managed port access, use [a11.sdk.http.client][a11.sdk.http.client].

## Headers

Action headers outside ``x-a11-*`` become HTTP request headers. Use
``options.headers`` for names A11 headers cannot express or for literal
``x-a11-*`` HTTP headers. Explicit options override inherited values.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from a11 import _native
from a11.actions import ActionHandler, ActionSchema

if TYPE_CHECKING:
    from a11.actions import ActionRegistry

#: Registered name of the low-level request Action.
MAKE_HTTP_REQUEST = "make_http_request"
#: Registered name of the ``fetch()``-shaped adapter.
WEB_FETCH = "web-fetch"
#: Registered name of the platform WebKit renderer.
WEB_RENDER = "web-render"

# The native side owns the table -- the schemas, the handlers, and the order --
# so the exported objects cannot drift from what a C++ host registers.
_ENTRIES = {
    name: (schema, handler) for name, schema, handler in _native.http_actions()
}

#: Schema for ``make_http_request``.
MAKE_HTTP_REQUEST_SCHEMA: ActionSchema = _ENTRIES[MAKE_HTTP_REQUEST][0]
#: Native handler for ``make_http_request``.
MAKE_HTTP_REQUEST_HANDLER: ActionHandler = _ENTRIES[MAKE_HTTP_REQUEST][1]

#: Schema for ``web-fetch``.
WEB_FETCH_SCHEMA: ActionSchema = _ENTRIES[WEB_FETCH][0]
#: Native handler for ``web-fetch``.
WEB_FETCH_HANDLER: ActionHandler = _ENTRIES[WEB_FETCH][1]
#: Schema for ``web-render``.
WEB_RENDER_SCHEMA: ActionSchema = _ENTRIES[WEB_RENDER][0]
#: Native handler for ``web-render``.
WEB_RENDER_HANDLER: ActionHandler = _ENTRIES[WEB_RENDER][1]

#: The three (schema, handler) pairs, low-level first.
HTTP_ACTIONS: tuple[tuple[ActionSchema, ActionHandler], ...] = (
    (MAKE_HTTP_REQUEST_SCHEMA, MAKE_HTTP_REQUEST_HANDLER),
    (WEB_FETCH_SCHEMA, WEB_FETCH_HANDLER),
    (WEB_RENDER_SCHEMA, WEB_RENDER_HANDLER),
)

del _ENTRIES


def register(
    registry: ActionRegistry,
    *,
    low_level: bool = True,
    adapter: bool = True,
    renderer: bool = True,
) -> None:
    """Register the HTTP Actions on ``registry``.

    Args:
        registry: Registry to register on.
        low_level: Register ``make_http_request``.
        adapter: Register ``web-fetch``.
        renderer: Register ``web-render``.

    The Actions are separately selectable because they answer to different
    amounts of trust. A gateway can expose document retrieval without also
    exposing streamed uploads, arbitrary methods, server pushes, or a browser.
    """
    selected = [
        *((HTTP_ACTIONS[0],) if low_level else ()),
        *((HTTP_ACTIONS[1],) if adapter else ()),
        *((HTTP_ACTIONS[2],) if renderer else ()),
    ]
    for schema, handler in selected:
        registry.register(schema.name, schema, handler)


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
    "register",
]

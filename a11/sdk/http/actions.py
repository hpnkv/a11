"""The two native HTTP Actions, and how to register them.

* ``make_http_request`` is HTTP with nothing hidden. Every concern the protocol
  keeps separate gets a port of its own -- the status, the header fields, the
  body, the trailer section, the redirect chain, the responses the server
  pushed, and how the connection was carried -- so a caller can act on the
  status while the body is still arriving, or hand the body to one consumer and
  the trailers to another.
* ``web-fetch`` is the same machinery with the protocol turned down: a status, a
  header map, and the body as text, as JSON, as bytes, or decoded into a stream
  of items.

Install both on a registry with [`register`][a11.sdk.http.actions.register]:

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

The handlers are `NativeActionHandler` handles rather than Python callables:
pass one wherever a handler is accepted and the C++ implementation runs
directly.

For driving them from Python without wiring ports by hand, see
[a11.sdk.http.client][a11.sdk.http.client], whose `request` and `fetch` do the
draining and hand back an object.

## Headers

An action header that does not begin with ``x-a11-`` is sent as an HTTP request
header, verbatim. That makes Flow's ``with "accept": "application/json"`` and
``forward headers "authorization"`` HTTP header syntax already, and keeps A11's
own framework headers -- a deadline, a trace -- out of the request. Anything
that cannot be spelled as an A11 header name (or a literal ``x-a11-`` header a
peer genuinely wants) goes in ``options.headers`` instead, which also wins over
an inherited value of the same name.
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

#: The two (schema, handler) pairs, low-level first.
HTTP_ACTIONS: tuple[tuple[ActionSchema, ActionHandler], ...] = (
    (MAKE_HTTP_REQUEST_SCHEMA, MAKE_HTTP_REQUEST_HANDLER),
    (WEB_FETCH_SCHEMA, WEB_FETCH_HANDLER),
)

del _ENTRIES


def register(
    registry: ActionRegistry,
    *,
    low_level: bool = True,
    adapter: bool = True,
) -> None:
    """Register the HTTP Actions on ``registry``.

    Args:
        registry: Registry to register on.
        low_level: Register ``make_http_request``.
        adapter: Register ``web-fetch``.

    The two are separately selectable because they answer to different amounts
    of trust. A gateway happy to let a caller fetch a document may not want to
    hand out streamed uploads, arbitrary methods, and server pushes; serving
    only ``web-fetch`` is how it says so.
    """
    selected = [
        *((HTTP_ACTIONS[0],) if low_level else ()),
        *((HTTP_ACTIONS[1],) if adapter else ()),
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
    "register",
]

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

"""Native services: an action registry, its sessions, and their lifecycle.

A `Service` is what a peer can call, decoupled from where it listens. Hand
`Service.accept` to any transport server and that server's connections become
sessions on this service; hand it an in-process stream and there is no server at
all.

A `Service` coordinates active sessions and dispatches actions without
managing socket lifecycles directly. The application owns its listeners and decides when
to stop -- see [a11.service.serving][a11.service.serving] for the helper
that binds the two together.

Examples:
    One service, two transports, one lifecycle:

    ```python
    service = a11.Service(action_registry=registry)
    async with serving(service, websocket(ws_options), http_sse(sse_options)):
        await stop.wait()
    ```
"""

from __future__ import annotations

from collections.abc import Awaitable, Callable

from a11.net.wire_stream import WireStream
from a11.service._native_service import Service, ServiceOptions

#: Async callback run once per connection, after its session exists and before
#: it starts pumping. The window in which a connection can be specialised --
#: swapping in a registry copy, binding a reverse-dispatch bridge -- without
#: racing its first message. Raising rejects the connection.
OnServiceConnection = Callable[["Session", WireStream], Awaitable[None]]

Service.__module__ = __name__

__all__ = ["OnServiceConnection", "Service", "ServiceOptions"]

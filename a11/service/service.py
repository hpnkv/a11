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

"""Serve actions through transport connections or in-process streams.

Pass `Service.accept` to a transport server to create a session for each
connection. A `Service` dispatches actions and manages sessions. The
application manages listeners and shutdown; see
[a11.service.serving][a11.service.serving].

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

#: Runs once per connection after session creation and before message pumping.
#: It may customize the session; raising rejects the connection.
OnServiceConnection = Callable[["Session", WireStream], Awaitable[None]]

Service.__module__ = __name__

__all__ = ["OnServiceConnection", "Service", "ServiceOptions"]

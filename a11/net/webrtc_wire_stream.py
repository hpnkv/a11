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

"""Native WebRTC data-channel wire streams.

A `WebRtcWireStream` carries A11 [WireMessage][a11.data.types.WireMessage]
traffic over a WebRTC data channel, enabling peer-to-peer (including
NAT-traversed) connections between agents. It relies on a signalling channel
(see [a11.net.signalling][a11.net.signalling] ) to establish the peer
connection. See
[a11.net.wire_stream][a11.net.wire_stream] for the transport-agnostic interface
it implements.
"""

from a11 import _native
from a11._native_protocol import attach_protocol

from .signalling import SignallingService, SignallingTransport
from .wire_stream import WireStream, WireStreamOptions

from a11._native import TurnRelayType
from a11._native import TurnServer
from a11._native import WebRtcConfiguration
from a11._native import WebRtcWireStream
from a11._native import WebRtcWireServer


class _WebRtcWireServerProtocol:
    """Context-manager protocol for the server (``stop`` on exit)."""

    def __enter__(self) -> "WebRtcWireServer":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.stop()


attach_protocol(WebRtcWireServer, _WebRtcWireServerProtocol)

for _class in (
    TurnRelayType,
    TurnServer,
    WebRtcConfiguration,
    WebRtcWireStream,
    WebRtcWireServer,
):
    _class.__module__ = __name__


__all__ = [
    "SignallingService",
    "SignallingTransport",
    "TurnRelayType",
    "TurnServer",
    "WebRtcConfiguration",
    "WebRtcWireServer",
    "WebRtcWireStream",
    "WireStream",
    "WireStreamOptions",
]

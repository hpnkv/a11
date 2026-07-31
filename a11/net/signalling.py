"""Native in-process and nghttp2 WebSocket signalling services.

Signalling is the out-of-band handshake that lets two peers find each other and
agree on how to connect before a data transport exists -- most importantly to
exchange the SDP offers/answers and ICE candidates a
[WebRtcWireStream][a11.net.webrtc_wire_stream.WebRtcWireStream] needs. A
`SignallingService` routes `SignallingMessage` values between
endpoints; `WebSocketSignallingServer` and
`WebSocketSignallingClient` are the WebSocket-transported implementations
you deploy when the peers are on different machines.

The classes exported here are the native ``a11._native`` signalling types; this
module attaches their (synchronous) context-manager protocols via
[attach_protocol][a11._native_protocol.attach_protocol], so a service,
transport, or
server stops/closes itself when its ``with`` block ends.
"""

from a11 import _native
from a11._native_protocol import attach_protocol

from a11._native import SignallingMessageType
from a11._native import SignallingMessage
from a11._native import SignallingTransport
from a11._native import SignallingEndpoint
from a11._native import SignallingService
from a11._native import WebSocketSignallingClientOptions
from a11._native import WebSocketSignallingClient
from a11._native import WebSocketSignallingServerOptions
from a11._native import WebSocketSignallingServer


class _SignallingServiceProtocol:
    """Context-manager protocol for a signalling service (``stop`` on exit)."""

    def __enter__(self) -> "SignallingService":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.stop()


class _SignallingTransportProtocol:
    """Context-manager protocol for a signalling transport (``close`` on
    exit)."""

    def __enter__(self) -> "SignallingTransport":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.close()


class _WebSocketSignallingServerProtocol:
    """Context-manager protocol for the WebSocket signalling server
    (``stop``)."""

    def __enter__(self) -> "WebSocketSignallingServer":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.stop()


attach_protocol(SignallingService, _SignallingServiceProtocol)
attach_protocol(SignallingTransport, _SignallingTransportProtocol)
attach_protocol(WebSocketSignallingServer, _WebSocketSignallingServerProtocol)

for _class in (
    SignallingMessageType,
    SignallingMessage,
    SignallingTransport,
    SignallingEndpoint,
    SignallingService,
    WebSocketSignallingClientOptions,
    WebSocketSignallingClient,
    WebSocketSignallingServerOptions,
    WebSocketSignallingServer,
):
    _class.__module__ = __name__


__all__ = [
    "SignallingEndpoint",
    "SignallingMessage",
    "SignallingMessageType",
    "SignallingService",
    "SignallingTransport",
    "WebSocketSignallingClient",
    "WebSocketSignallingClientOptions",
    "WebSocketSignallingServer",
    "WebSocketSignallingServerOptions",
]

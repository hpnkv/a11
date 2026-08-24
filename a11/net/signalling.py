"""Native in-process and nghttp2 WebSocket signalling services.

Signalling is the out-of-band handshake that lets two peers find each other and
agree on how to connect before a data transport exists -- most importantly to
exchange the SDP offers/answers and ICE candidates a
[WebRtcWireStream][a11.net.webrtc_wire_stream.WebRtcWireStream] needs. A
`SignallingService` routes `SignallingMessage` values between
endpoints; `WebSocketSignallingServer` and
`WebSocketSignallingClient` are the WebSocket-transported implementations
you deploy when the peers are on different machines.

A server exposed beyond one process usually needs to say who may register and
what may be sent. `WebSocketSignallingServerOptions` carries four optional
hooks for that -- ``on_admit`` (given a `SignallingAdmission`), ``on_message``,
``on_unroutable`` and ``on_departed`` -- and `SignallingService.deliver` is the
ingress that pairs with ``on_unroutable`` when several servers act as one
fabric. A11 supplies the slots; the policy is the deployment's.

The classes exported here are the native ``a11._native`` signalling types; this
module attaches their (synchronous) context-manager protocols via
[attach_protocol][a11._native_protocol.attach_protocol], so a service,
transport, or
server stops/closes itself when its ``with`` block ends.
"""

from a11 import _native
from a11._native_protocol import attach_protocol

from a11._native import SignallingAdmission
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


def client_options() -> "WebSocketSignallingClientOptions":
    """Options for registering with a signalling server over HTTP/1.1.

    A signalling WebSocket is one long-lived request, and reverse proxies do
    not carry it over HTTP/2: RFC 8441's extended CONNECT is unimplemented by
    nginx and most of its peers, which answer the handshake with a bare `400`.
    A client left on its default preference therefore reaches a native server
    directly and fails the moment one is put behind an ingress -- so ask for
    HTTP/1.1, which both a proxy and A11's own server accept.

    `client_preference` is what decides it. Clearing `enable_h2`/`enable_h2c`
    reads like it should be enough and is not: the WebSocket client still
    offered h2 and still got the `400`, so both are set here and the
    preference is the one doing the work.
    """
    options = WebSocketSignallingClientOptions()
    options.http2_options.client_preference = (
        _native.HttpProtocolPreference.HTTP11
    )
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    return options


attach_protocol(SignallingService, _SignallingServiceProtocol)
attach_protocol(SignallingTransport, _SignallingTransportProtocol)
attach_protocol(WebSocketSignallingServer, _WebSocketSignallingServerProtocol)

for _class in (
    SignallingAdmission,
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
    "SignallingAdmission",
    "SignallingEndpoint",
    "SignallingMessage",
    "SignallingMessageType",
    "SignallingService",
    "SignallingTransport",
    "WebSocketSignallingClient",
    "WebSocketSignallingClientOptions",
    "WebSocketSignallingServer",
    "WebSocketSignallingServerOptions",
    "client_options",
]

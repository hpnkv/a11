"""Native WebRTC data-channel wire streams."""

from a11 import _native

from .signalling import SignallingService, SignallingTransport
from .wire_stream import WireStream, WireStreamOptions

TurnRelayType = _native.TurnRelayType
TurnServer = _native.TurnServer
WebRtcConfiguration = _native.WebRtcConfiguration
WebRtcWireStream = _native.WebRtcWireStream
WebRtcWireServer = _native.WebRtcWireServer


def _server_enter(server: WebRtcWireServer) -> WebRtcWireServer:
    return server


def _server_exit(server: WebRtcWireServer, exc_type, exc, traceback) -> None:
    del exc_type, exc, traceback
    server.stop()


WebRtcWireServer.__enter__ = _server_enter
WebRtcWireServer.__exit__ = _server_exit

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

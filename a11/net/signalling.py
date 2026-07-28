"""Native in-process and nghttp2 WebSocket signalling services."""

from a11 import _native

SignallingMessageType = _native.SignallingMessageType
SignallingMessage = _native.SignallingMessage
SignallingTransport = _native.SignallingTransport
SignallingEndpoint = _native.SignallingEndpoint
SignallingService = _native.SignallingService
WebSocketSignallingClientOptions = _native.WebSocketSignallingClientOptions
WebSocketSignallingClient = _native.WebSocketSignallingClient
WebSocketSignallingServerOptions = _native.WebSocketSignallingServerOptions
WebSocketSignallingServer = _native.WebSocketSignallingServer


def _service_enter(service: SignallingService) -> SignallingService:
    return service


def _service_exit(service: SignallingService, exc_type, exc, traceback) -> None:
    del exc_type, exc, traceback
    service.stop()


def _transport_enter(transport: SignallingTransport) -> SignallingTransport:
    return transport


def _transport_exit(
    transport: SignallingTransport, exc_type, exc, traceback
) -> None:
    del exc_type, exc, traceback
    transport.close()


def _server_enter(
    server: WebSocketSignallingServer,
) -> WebSocketSignallingServer:
    return server


def _server_exit(
    server: WebSocketSignallingServer, exc_type, exc, traceback
) -> None:
    del exc_type, exc, traceback
    server.stop()


SignallingService.__enter__ = _service_enter
SignallingService.__exit__ = _service_exit
SignallingTransport.__enter__ = _transport_enter
SignallingTransport.__exit__ = _transport_exit
WebSocketSignallingServer.__enter__ = _server_enter
WebSocketSignallingServer.__exit__ = _server_exit

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

# Copyright 2026 The A11 Authors.

"""Being hosted: taking a claim, keeping it, and staying registered.

`a11 serve --hosted <identity>` is three things that all have to keep working
for as long as the process runs, not just at start-up:

* a **claim**, which expires and must be renewed before it does;
* a **signalling connection**, which can drop and must be re-made;
* the **WebRTC listener** bound to it, which has to follow the connection.

A host that does the first of each and none of the rest works for an hour and
then quietly stops being reachable, which is the failure this class exists to
prevent. `HostedEndpoint` owns all three and reports the one thing a caller
cares about: whether it is currently reachable.
"""

from __future__ import annotations

import asyncio
import contextlib
import datetime
import socket
from typing import Any

from absl import logging

from a11 import net, timing
from a11.client.exchange import Claim, ExchangeClient
from a11.status import Status, StatusCode, StatusException

#: How long the signalling handshake is given.
CONNECT_TIMEOUT = timing.Duration.seconds(20)
#: How soon after a lost connection to try again, and how far to back off.
MIN_RECONNECT_BACKOFF = 1.0
MAX_RECONNECT_BACKOFF = 30.0
#: Renewal is scheduled from the claim's own `renew_after`; this bounds how
#: long the loop sleeps between checks so a clock change cannot strand it.
RENEW_POLL_SECONDS = 30.0


def _turn_server(url: str, username: str, password: str) -> "net.TurnServer":
    """One `turn:`/`turns:` URL as A11 wants it -- parts, not a URL."""
    scheme, _, rest = url.partition(":")
    rest, _, query = rest.partition("?")
    host, separator, port = rest.rpartition(":")
    if not separator:  # no port given
        host, port = rest, ""
    transport = ""
    if query.startswith("transport="):
        transport = query[len("transport="):].lower()
    if not transport:
        transport = "tls" if scheme == "turns" else "udp"

    server = net.TurnServer()
    server.hostname = host
    server.port = int(port) if port else (5349 if scheme == "turns" else 3478)
    server.username = username
    server.password = password
    server.relay_type = {
        "udp": net.TurnRelayType.UDP,
        "tcp": net.TurnRelayType.TCP,
        "tls": net.TurnRelayType.TLS,
    }.get(transport, net.TurnRelayType.UDP)
    return server


def hosted_configuration(
    ice_servers: "list[dict] | None" = None,
    *,
    multiplex_ice: bool = False,
) -> "net.WebRtcConfiguration":
    """How a hosted agent should negotiate with peers that dial it.

    `ice_servers` is the list the exchange handed over with the claim, in
    `RTCIceServer` shape. Passing it matters more than it looks: without it a
    host behind NAT offers only its own private addresses and the relay only
    its cloud NAT's, neither can use the other's, and the handshake just never
    completes -- with nothing in either log saying why. Prefer
    `HostedEndpoint.webrtc_configuration()`, which reads the live claim.

    ## Why ICE is *not* multiplexed by default

    Multiplexing every peer connection onto one UDP port is appealing for a
    hosted agent -- it is the side several peers converge on, and one
    predictable port is friendlier to a NAT than an ephemeral port each. It is
    also, for a relayed agent, broken.

    A muxed socket routes an incoming packet by its **source address**. When a
    connection is relayed, every packet from every peer arrives from the *TURN
    server's* address, so two concurrent connections are indistinguishable at
    the socket: the second completes its handshake and then receives nothing.
    Measured against the deployed exchange, a host with muxing on answered a
    discovery question 2 times in 4, each failure costing the full timeout,
    while the same host with muxing off answered 6 for 6 in about a second. A
    relayed agent has concurrent connections as its normal condition -- a
    caller, a second caller, a discovery question -- so this is not an edge.

    `multiplex_ice=True` is available for a host that knows it will have one
    peer at a time and wants the single port. It is not the default because the
    failure it causes is silent: a handshake that completes and then delivers
    nothing looks exactly like an agent that has stopped answering.
    """
    configuration = net.WebRtcConfiguration()
    configuration.enable_ice_udp_mux = multiplex_ice

    stun: list[str] = []
    turn: list[net.TurnServer] = []
    for entry in ice_servers or ():
        username = entry.get("username", "")
        password = entry.get("credential", "")
        for url in entry.get("urls", ()):
            if url.startswith("stun:"):
                stun.append(url)
            elif url.startswith(("turn:", "turns:")):
                turn.append(_turn_server(url, username, password))
    configuration.stun_servers = stun
    configuration.turn_servers = turn
    return configuration


def default_holder() -> str:
    """A description of this process, for whoever is looking at two of them."""
    import os

    return f"{socket.gethostname()}/{os.getpid()}"


def _parse_time(value: str) -> datetime.datetime | None:
    if not value:
        return None
    try:
        return datetime.datetime.fromisoformat(value)
    except ValueError:
        return None


class HostedEndpoint:
    """One identity, hosted through an exchange, kept reachable.

    Use it as an async context manager; the signalling transport it exposes is
    what `a11.service.serving.webrtc` binds a service to.
    """

    def __init__(
        self,
        client: ExchangeClient,
        identity: str,
        *,
        holder: str = "",
        ttl_seconds: int | None = None,
        signalling_url: str = "",
    ) -> None:
        self._client = client
        self.identity = identity
        self._holder = holder or default_holder()
        self._ttl = ttl_seconds
        self._signalling_url = signalling_url

        self.claim: Claim | None = None
        self._transport: Any | None = None
        self._maintainer: asyncio.Task | None = None
        self._stopped = asyncio.Event()
        #: Raised by `wait_connected` when the endpoint gives up entirely.
        self._fatal: Status | None = None
        self._connected = asyncio.Event()
        #: Called with the new transport whenever one is established, so a
        #: service can rebind its listener. Set by `serve_hosted`.
        self.on_transport: Any = None

    @property
    def transport(self):
        """The live signalling transport, or None while reconnecting."""
        return self._transport

    @property
    def connected(self) -> bool:
        """Whether this endpoint is currently registered for signalling."""
        return self._transport is not None and self._transport.connected()

    def webrtc_configuration(self) -> "net.WebRtcConfiguration":
        """How to negotiate, using the ICE servers this claim came with.

        Read it after `start`, and read it again after a renewal: TURN
        credentials are derived against an expiry, so a configuration built
        from a lapsed claim is refused by the TURN server rather than ignored.
        """
        ice_servers = self.claim.ice_servers if self.claim is not None else []
        return hosted_configuration(ice_servers)

    async def __aenter__(self) -> "HostedEndpoint":
        await self.start()
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        await self.aclose()

    async def start(self) -> None:
        """Take the claim, connect, and keep both alive.

        Raises:
            StatusException: whatever the exchange said, when the first claim
                or the first connection fails. A host that cannot start should
                fail loudly rather than retry forever behind a healthy-looking
                process.
        """
        self.claim = await self._client.claim(
            self.identity, holder=self._holder, ttl_seconds=self._ttl
        )
        await self._connect()
        self._maintainer = asyncio.create_task(self._maintain())

    async def aclose(self) -> None:
        """Stop hosting: release the claim and close the connection.

        Releasing is what makes a clean shutdown different from a crash --
        the identity becomes free immediately rather than at expiry, so a
        replacement does not have to supersede anything.
        """
        self._stopped.set()
        maintainer, self._maintainer = self._maintainer, None
        if maintainer is not None:
            maintainer.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await maintainer

        transport, self._transport = self._transport, None
        if transport is not None:
            with contextlib.suppress(Exception):
                transport.close()

        if self.claim is not None:
            with contextlib.suppress(Exception):
                await self._client.release(self.identity, self.claim.token)
            self.claim = None

    async def wait_connected(self, timeout: float = 30.0) -> None:
        """Wait until registered, or raise what stopped it."""
        try:
            await asyncio.wait_for(self._connected.wait(), timeout=timeout)
        except asyncio.TimeoutError as exc:
            if self._fatal is not None:
                raise self._fatal.to_exception() from exc
            raise Status(
                code=StatusCode.DEADLINE_EXCEEDED,
                message=f"{self.identity} did not become reachable in time.",
            ).to_exception() from exc

    # --- the three things it keeps alive --------------------------------

    async def _connect(self) -> None:
        """Open the signalling connection this claim entitles us to."""
        if self.claim is None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="A hosting claim must be taken before connecting.",
            ).to_exception()

        url = self._signalling_url or self.claim.signalling_url
        if not url:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=(
                    "The exchange did not say where to signal, and no"
                    " --signalling-url was given."
                ),
            ).to_exception()

        options = net.signalling.client_options()
        options.deadline = timing.now() + CONNECT_TIMEOUT
        # The credential travels as a header rather than in the query string,
        # which is the reason A11's signalling client grew header support.
        options.headers = {
            "authorization": f"Bearer {self._client.api_key}",
            "x-a11-claim": self.claim.token,
        }

        transport = await net.WebSocketSignallingClient.connect(
            url, self.identity, None, options
        )
        self._transport = transport
        self._connected.set()
        logging.info("hosting %s through %s", self.identity, url)
        if self.on_transport is not None:
            await self.on_transport(transport)

    async def _maintain(self) -> None:
        """Renew the claim before it lapses; reconnect when the socket goes."""
        backoff = MIN_RECONNECT_BACKOFF
        while not self._stopped.is_set():
            await asyncio.sleep(
                min(RENEW_POLL_SECONDS, MAX_RECONNECT_BACKOFF)
            )
            if self._stopped.is_set():
                return

            try:
                await self._renew_if_due()
            except StatusException as exc:
                if exc.status.code == StatusCode.FAILED_PRECONDITION:
                    # Superseded, released or expired: somebody else is this
                    # identity now, and pretending otherwise would have two
                    # processes claiming to be one agent.
                    await self._give_up(exc.status)
                    return
                logging.warning(
                    "could not renew the claim on %s: %s",
                    self.identity,
                    exc.status.message,
                )

            if self.connected:
                backoff = MIN_RECONNECT_BACKOFF
                continue

            self._connected.clear()
            logging.info("signalling for %s dropped; reconnecting", self.identity)
            try:
                await self._connect()
                backoff = MIN_RECONNECT_BACKOFF
            except StatusException as exc:
                logging.warning(
                    "could not re-register %s: %s",
                    self.identity,
                    exc.status.message,
                )
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, MAX_RECONNECT_BACKOFF)

    async def _renew_if_due(self) -> None:
        """Renew when the claim says it is time, and not before."""
        if self.claim is None:
            return
        due = _parse_time(self.claim.renew_after)
        if due is not None:
            now = datetime.datetime.now(due.tzinfo)
            if now < due:
                return
        self.claim = await self._client.renew(
            self.identity, self.claim.token, ttl_seconds=self._ttl
        )
        logging.info(
            "renewed the claim on %s until %s",
            self.identity,
            self.claim.expires_at,
        )

    async def _give_up(self, status: Status) -> None:
        """Stop hosting because the exchange says we are no longer the host."""
        logging.error(
            "no longer hosting %s: %s", self.identity, status.message
        )
        self._fatal = status
        self._stopped.set()
        self._connected.clear()
        transport, self._transport = self._transport, None
        if transport is not None:
            with contextlib.suppress(Exception):
                transport.close()


__all__ = [
    "CONNECT_TIMEOUT",
    "MAX_RECONNECT_BACKOFF",
    "MIN_RECONNECT_BACKOFF",
    "HostedEndpoint",
    "default_holder",
]

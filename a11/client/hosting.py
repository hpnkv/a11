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
import time
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
#: How long before the bound relay credentials lapse to rebuild the listener.
#: Comfortably more than one poll interval, so the rebind never races the
#: expiry it exists to stay ahead of.
REBIND_MARGIN_SECONDS = 120.0

#: Failures a retry cannot mend, so hosting stops instead of looping.
#:
#: `FAILED_PRECONDITION` is the claim: superseded, released or expired.
#: `UNAUTHENTICATED` and `PERMISSION_DENIED` are the credential: a key that has
#: been revoked or narrowed answers the same way for ever, and a host that
#: retries it stays online-looking and unreachable until a person reads the log.
_HOPELESS = frozenset({
    StatusCode.FAILED_PRECONDITION,
    StatusCode.UNAUTHENTICATED,
    StatusCode.PERMISSION_DENIED,
})


def _turn_server(url: str, username: str, password: str) -> "net.TurnServer":
    """One `turn:`/`turns:` URL as A11 wants it -- parts, not a URL."""
    scheme, _, rest = url.partition(":")
    rest, _, query = rest.partition("?")
    host, separator, port = rest.rpartition(":")
    if not separator:  # no port given
        host, port = rest, ""
    transport = ""
    if query.startswith("transport="):
        transport = query[len("transport=") :].lower()
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
    discover_path_mtu: bool = False,
) -> "net.WebRtcConfiguration":
    """How a hosted agent should negotiate with peers that dial it.

    `ice_servers` is the `RTCIceServer` list included with the exchange claim.
    It supplies relay addresses needed when hosts are behind NAT. Prefer
    `HostedEndpoint.webrtc_configuration()`, which reads the live claim.

    `discover_path_mtu` is disabled here, overriding the transport default.
    Discovery raises the association MTU after acknowledging a burst of padded
    heartbeats. Intermittent probe success can select a size that later packets
    cannot carry, delaying fallback until black-hole detection completes.
    Hosted agents commonly use internet paths through TURN relays, whose MTU
    cannot be characterised reliably, so they keep the configured value. Pass
    True only when both endpoints use a controlled network.
    """
    configuration = net.WebRtcConfiguration()
    configuration.enable_ice_udp_mux = multiplex_ice
    configuration.path_mtu_discovery = discover_path_mtu
    if not discover_path_mtu:
        # Belt and braces: with the search off the ceiling is unused, and
        # bringing it down to the floor means even a build that ignored the
        # flag could not raise above what was configured.
        configuration.max_discovered_mtu = configuration.mtu or 1280

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
        identity: str | None,
        *,
        holder: str = "",
        ttl_seconds: int | None = None,
        signalling_url: str = "",
        organization: str = "",
    ) -> None:
        self._client = client
        #: Empty until `start` when no identity was asked for: the exchange
        #: picks a scoped one and this becomes whatever it granted. Everything
        #: after the claim -- renewing, rebinding, releasing -- is keyed on this
        #: name, so it has to be settled before anything connects.
        self.identity = identity or ""
        self._organization = organization
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
        #: service can rebind its listener.
        self.on_transport: Any = None
        #: Awaited, with no arguments, before the current transport is dropped:
        #: stop the listener bound to it. Pairs with `on_transport`, which
        #: builds the replacement. A host that sets neither stops being
        #: reachable an hour in -- see `_rebind_if_lapsing`.
        self.on_drop_listener: Any = None
        #: The credential expiry the live listener was built with, so staleness
        #: is judged against what is *bound* rather than what is merely known.
        self._bound_expiry: float | None = None

    @property
    def transport(self):
        """The live signalling transport, or None while reconnecting."""
        return self._transport

    @property
    def connected(self) -> bool:
        """Whether this endpoint is currently registered for signalling."""
        return self._transport is not None and self._transport.connected()

    def credentials_expire_at(self) -> float | None:
        """When the claim's relay credentials lapse, or None without any.

        The earliest expiry across the ICE servers, because one lapsed entry is
        enough to lose the candidate that was carrying the traffic.
        """
        if self.claim is None:
            return None
        expiries = [
            float(entry["expires_at"])
            for entry in self.claim.ice_servers
            if entry.get("expires_at")
        ]
        return min(expiries) if expiries else None

    def webrtc_configuration(self) -> "net.WebRtcConfiguration":
        """How to negotiate, using the ICE servers this claim came with.

        Records the expiry it was built with, so `on_rebind` can fire before
        the listener holding it goes quietly unreachable.
        """
        ice_servers = self.claim.ice_servers if self.claim is not None else []
        self._bound_expiry = self.credentials_expire_at()
        return hosted_configuration(ice_servers)

    async def __aenter__(self) -> "HostedEndpoint":
        await self.start()
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        await self.aclose()

    async def start(self) -> None:
        """Take the claim, connect, and keep both alive.

        With no identity asked for, the claim is taken on a scoped one the
        exchange assigns -- and the name it granted is adopted here before
        anything else uses it.

        Raises:
            StatusException: whatever the exchange said, when the first claim
                or the first connection fails. A host that cannot start should
                fail loudly rather than retry forever behind a healthy-looking
                process.
        """
        if self.identity:
            self.claim = await self._client.claim(
                self.identity, holder=self._holder, ttl_seconds=self._ttl
            )
        else:
            self.claim = await self._client.claim_scoped(
                organization=self._organization,
                holder=self._holder,
                ttl_seconds=self._ttl,
            )
            self.identity = self.claim.identity
            if not self.identity:
                raise Status(
                    code=StatusCode.INTERNAL,
                    message=(
                        "The exchange granted a claim without naming the"
                        " identity it is on, so there is nothing to host."
                    ),
                ).to_exception()
            logging.info("the exchange assigned the identity %s", self.identity)
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
            await asyncio.sleep(min(RENEW_POLL_SECONDS, MAX_RECONNECT_BACKOFF))
            if self._stopped.is_set():
                return

            try:
                await self._renew_if_due()
            except StatusException as exc:
                if exc.status.code in _HOPELESS:
                    # Two different endings, both final. FAILED_PRECONDITION is
                    # superseded, released or expired: somebody else is this
                    # identity now, and pretending otherwise would have two
                    # processes claiming to be one agent. UNAUTHENTICATED and
                    # PERMISSION_DENIED are the credential itself: retrying a
                    # revoked key produces the same 401 every thirty seconds
                    # for as long as the process lives, while presence, the
                    # listener and the log all go on saying "hosting".
                    await self._give_up(exc.status)
                    return
                logging.warning(
                    "could not renew the claim on %s: %s",
                    self.identity,
                    exc.status.message,
                )

            try:
                await self._rebind_if_lapsing()
            except Exception:  # noqa: BLE001 - a hook may raise anything
                logging.exception("could not rebind %s", self.identity)

            if self.connected:
                backoff = MIN_RECONNECT_BACKOFF
                continue

            self._connected.clear()
            logging.info(
                "signalling for %s dropped; reconnecting", self.identity
            )
            await self._relinquish()
            try:
                await self._connect()
                backoff = MIN_RECONNECT_BACKOFF
            except Exception as exc:  # noqa: BLE001 - a transport may raise anything
                logging.warning(
                    "could not re-register %s: %s", self.identity, exc
                )
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, MAX_RECONNECT_BACKOFF)

    async def _rebind_if_lapsing(self) -> None:
        """Rebuild the listener before the credentials it holds expire.

        A renewal mints new relay credentials, but a listener already bound
        keeps the old ones -- and a TURN credential is checked against the
        expiry embedded in its username, so once that passes the host can no
        longer gather a relayed candidate. Nothing fails loudly: signalling
        stays up, presence stays online, and new connections simply stop
        completing. It presents as "it worked for a while and then stopped",
        an hour in, which is the default claim lifetime.

        The whole signalling connection is replaced, not just the listener,
        because a WebRTC server closes its transport when it stops (see
        `a11.service.serving.webrtc`) -- so there is no way to hand a fresh
        configuration to a new server on the *same* transport. The order is
        therefore: drop the listener, reconnect, let `on_transport` build the
        replacement against the current claim.

        Deferred until the bound credentials are nearly out rather than done on
        every renewal, because it costs the peer connections the old listener
        was carrying. The relay's grace period turns that into latency for a
        caller, but it is not free, so once an hour beats twice.
        """
        if self.on_drop_listener is None or self._bound_expiry is None:
            return
        if self._bound_expiry - time.time() > REBIND_MARGIN_SECONDS:
            return
        fresh = self.credentials_expire_at()
        if fresh is None or fresh <= self._bound_expiry:
            # A renewal has not yet produced newer credentials; rebinding to
            # the same expiry would drop connections and fix nothing.
            return

        logging.info(
            "rebinding %s: relay credentials lapse in %.0fs",
            self.identity,
            self._bound_expiry - time.time(),
        )
        await self._relinquish()
        await self._connect()
        self._bound_expiry = fresh

    async def _relinquish(self) -> None:
        """Let go of the listener and the transport it is bound to.

        Always both, and in that order: a WebRTC server closes its transport
        when it stops, so a listener kept across a reconnect holds a dead
        socket, and a transport kept across a failed reconnect makes
        `connected` lie.
        """
        if self.on_drop_listener is not None:
            try:
                await self.on_drop_listener()
            except Exception:  # noqa: BLE001 - a hook may raise anything
                logging.exception(
                    "could not drop the listener for %s", self.identity
                )
        transport, self._transport = self._transport, None
        if transport is not None:
            with contextlib.suppress(Exception):
                transport.close()
        self._connected.clear()

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
        """Stop hosting for good, and say why in terms of what to do next."""
        if status.code in (
            StatusCode.UNAUTHENTICATED,
            StatusCode.PERMISSION_DENIED,
        ):
            # Worth spelling out, because the cause is somewhere else entirely:
            # `POST /v1/auth/login` revokes every live key of the same
            # `client_name`, so a *personal* key given to a long-running host
            # dies at its owner's next login. A host wants a key of its own.
            logging.error(
                "stopped hosting %s: %s. The credential this host holds is no"
                " longer accepted -- a key is revoked when another login"
                " supersedes it, so a long-running host wants a key of its own"
                " rather than a person's. Issue one and restart.",
                self.identity,
                status.message,
            )
        else:
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

    # --- what a host that has stopped tells its caller -------------------

    @property
    def fatal(self) -> Status | None:
        """Why hosting stopped for good, or `None` while it has not."""
        return self._fatal

    async def wait_stopped(self) -> Status | None:
        """Wait until hosting has stopped for good, and say why.

        For a caller whose whole job is to serve this endpoint: without it a
        process goes on running -- its other listeners still up, its exit code
        still zero -- while the hosted half of it is dead. See
        [`a11 serve`][a11.cli.commands.serve.serve], which races this against
        its signal handler so that a revoked credential ends the command
        instead of being one warning in a log nobody is reading.
        """
        await self._stopped.wait()
        return self._fatal


__all__ = [
    "CONNECT_TIMEOUT",
    "REBIND_MARGIN_SECONDS",
    "MAX_RECONNECT_BACKOFF",
    "MIN_RECONNECT_BACKOFF",
    "HostedEndpoint",
    "default_holder",
]

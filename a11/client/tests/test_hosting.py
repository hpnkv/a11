# Copyright 2026 The A11 Authors.

"""Turning an exchange's ICE server list into a WebRTC configuration.

Worth its own tests because getting this wrong is silent. A host with no TURN
server, or one with the wrong port or transport, negotiates exactly as far as a
correct one and then never completes a handshake -- there is no error to read,
only a peer that never answers. So the translation is asserted field by field
rather than trusted to look right.
"""

from __future__ import annotations

import asyncio

import pytest

from a11 import net
from a11.client.hosting import hosted_configuration
from a11.status import Status, StatusCode


def test_ice_is_not_multiplexed_by_default():
    """A muxed socket cannot tell two relayed peers apart.

    Relayed packets all arrive from the TURN server's address, so the second
    connection completes its handshake and then receives nothing. Measured on
    the deployed exchange at 45s spacing: 3/6 and 2/5 multiplexed, 6/6 not.
    """
    configuration = hosted_configuration()

    assert not configuration.enable_ice_udp_mux
    assert list(configuration.stun_servers) == []
    assert list(configuration.turn_servers) == []


def test_a_hosted_agent_holds_its_mtu_rather_than_probing_upward():
    """Disable MTU discovery for hosted agents.

    A raised MTU whose packets are dropped in flight produces no send error, so
    the association waits for the black-hole detector before falling back.
    Hosted agents use internet paths through TURN relays, where probing is not
    reliable enough to raise the configured MTU.
    """
    configuration = hosted_configuration()

    assert configuration.path_mtu_discovery is False
    # The ceiling comes down with it, so even a build that ignored the flag
    # could not raise above the configured floor.
    assert configuration.max_discovered_mtu == 1280


def test_path_mtu_discovery_is_available_where_the_path_is_known():
    configuration = hosted_configuration(discover_path_mtu=True)

    assert configuration.path_mtu_discovery is True
    assert configuration.max_discovered_mtu == 9216


def test_multiplexing_is_available_for_a_single_peer_host():
    configuration = hosted_configuration(multiplex_ice=True)

    assert configuration.enable_ice_udp_mux


def test_stun_and_turn_are_separated():
    configuration = hosted_configuration([
        {"urls": ["stun:stun.example:3478"]},
        {
            "urls": ["turn:turn.example:3478"],
            "username": "1787568765:a-claim",
            "credential": "the-derived-password",
        },
    ])

    assert list(configuration.stun_servers) == ["stun:stun.example:3478"]
    assert len(configuration.turn_servers) == 1
    server = configuration.turn_servers[0]
    assert server.hostname == "turn.example"
    assert server.port == 3478
    assert server.username == "1787568765:a-claim"
    assert server.password == "the-derived-password"
    assert server.relay_type == net.TurnRelayType.UDP


def test_one_entry_may_carry_several_urls():
    """As `RTCIceServer` allows, and as the exchange emits."""
    configuration = hosted_configuration([
        {
            "urls": [
                "turn:turn.example:3478",
                "turn:turn.example:3478?transport=tcp",
                "turns:turn.example:5349",
            ],
            "username": "u",
            "credential": "p",
        }
    ])

    kinds = [server.relay_type for server in configuration.turn_servers]
    assert kinds == [
        net.TurnRelayType.UDP,
        net.TurnRelayType.TCP,
        net.TurnRelayType.TLS,
    ]
    assert [server.port for server in configuration.turn_servers] == [
        3478,
        3478,
        5349,
    ]
    # The credential is per entry, so every URL under it carries the same one.
    assert {server.username for server in configuration.turn_servers} == {"u"}


def test_a_url_without_a_port_gets_the_scheme_default():
    configuration = hosted_configuration([
        {"urls": ["turn:turn.example", "turns:turn.example"]}
    ])

    assert [s.hostname for s in configuration.turn_servers] == [
        "turn.example",
        "turn.example",
    ]
    assert [s.port for s in configuration.turn_servers] == [3478, 5349]


def test_unknown_schemes_are_dropped_rather_than_guessed():
    """An exchange may advertise something this client does not speak."""
    configuration = hosted_configuration([
        {"urls": ["https://turn.example/rest", "stun:stun.example:3478"]}
    ])

    assert list(configuration.stun_servers) == ["stun:stun.example:3478"]
    assert list(configuration.turn_servers) == []


def test_the_gateway_client_asks_for_http_1_1():
    """The other half of the same trap, on the calling side.

    A gateway published through an ingress is reached over `wss://`, and nginx
    will not carry a WebSocket over HTTP/2. Asserted because the symptom -- a
    `400` before any credential is read -- looks like authentication failing.
    """
    from a11 import timing
    from a11.client.connection import websocket_client_options

    deadline = timing.now() + timing.Duration.seconds(5)
    options = websocket_client_options(deadline)

    assert (
        options.http2_options.client_preference
        == net.HttpProtocolPreference.HTTP11
    )
    assert not options.http2_options.enable_h2
    assert not options.http2_options.enable_h2c
    # And the deadline bounds the handshake, never the stream.
    assert options.handshake_deadline == deadline
    assert options.http2_options.deadline != deadline


class _FakeClaim:
    """Just the parts of a claim the rebind decision reads."""

    def __init__(self, expires_at: float | None) -> None:
        self.ice_servers = (
            [
                {"urls": ["stun:stun.example:3478"]},
                {
                    "urls": ["turn:turn.example:3478"],
                    "username": f"{int(expires_at)}:a-claim",
                    "credential": "p",
                    "expires_at": expires_at,
                },
            ]
            if expires_at is not None
            else [{"urls": ["stun:stun.example:3478"]}]
        )


def _endpoint() -> "hosting.HostedEndpoint":
    from a11.client import hosting

    return hosting.HostedEndpoint.__new__(hosting.HostedEndpoint)


@pytest.mark.asyncio
async def test_a_lapsing_credential_rebinds_in_the_right_order():
    """The bug this exists for: a renewed claim never reaching the listener.

    A TURN credential is checked against the expiry in its own username, so
    once it passes the host cannot gather a relayed candidate -- while
    signalling stays up and presence stays online. It presents as an agent that
    worked for an hour and then stopped.

    The order is asserted, not just the fact: a WebRTC server closes its
    transport when it stops, so the listener must be dropped *before* the
    reconnect that builds its replacement. Getting this backwards produced a
    host that had neither.
    """
    import time as time_module

    endpoint = _endpoint()
    endpoint.identity = "agent"
    endpoint._connected = asyncio.Event()
    endpoint._transport = None
    steps: list[str] = []

    async def drop_listener() -> None:
        steps.append("dropped")

    async def connect() -> None:
        steps.append("connected")

    endpoint.on_drop_listener = drop_listener
    endpoint._connect = connect
    now = time_module.time()

    # Bound credentials nearly out, and a renewal has produced newer ones.
    endpoint._bound_expiry = now + 30
    endpoint.claim = _FakeClaim(now + 3600)
    await endpoint._rebind_if_lapsing()
    assert steps == ["dropped", "connected"]

    # Now that the listener holds the fresh expiry, it must settle.
    await endpoint._rebind_if_lapsing()
    assert steps == ["dropped", "connected"]


@pytest.mark.asyncio
async def test_a_failing_drop_hook_does_not_stop_the_rebind():
    """A failing drop hook does not prevent transport rebinding.

    Hook failures are isolated so the host can register the replacement
    transport.
    """
    import time as time_module

    endpoint = _endpoint()
    endpoint.identity = "agent"
    endpoint._connected = asyncio.Event()
    endpoint._transport = None
    endpoint.claim = _FakeClaim(time_module.time() + 3600)
    endpoint._bound_expiry = time_module.time() + 30
    connected: list[str] = []

    async def drop_listener() -> None:
        raise RuntimeError("the listener would not stop")

    async def connect() -> None:
        connected.append("connected")

    endpoint.on_drop_listener = drop_listener
    endpoint._connect = connect

    await endpoint._rebind_if_lapsing()

    assert connected == ["connected"]


@pytest.mark.asyncio
async def test_no_rebind_while_the_bound_credential_has_time():
    import time as time_module

    endpoint = _endpoint()
    endpoint.identity = "agent"
    endpoint._connected = asyncio.Event()
    endpoint._transport = None
    rebinds: list[int] = []

    async def on_rebind() -> None:
        rebinds.append(1)

    endpoint.on_drop_listener = on_rebind
    now = time_module.time()
    endpoint._bound_expiry = now + 3600
    endpoint.claim = _FakeClaim(now + 7200)

    await endpoint._rebind_if_lapsing()

    assert rebinds == []


@pytest.mark.asyncio
async def test_no_rebind_when_renewal_has_not_produced_newer_credentials():
    """Rebinding to the same expiry would drop connections and fix nothing."""
    import time as time_module

    endpoint = _endpoint()
    endpoint.identity = "agent"
    endpoint._connected = asyncio.Event()
    endpoint._transport = None
    rebinds: list[int] = []

    async def on_rebind() -> None:
        rebinds.append(1)

    endpoint.on_drop_listener = on_rebind
    now = time_module.time()
    endpoint._bound_expiry = now + 10
    endpoint.claim = _FakeClaim(now + 10)

    await endpoint._rebind_if_lapsing()

    assert rebinds == []


def test_credentials_expire_at_takes_the_earliest():
    endpoint = _endpoint()
    endpoint.claim = _FakeClaim(1000.0)
    endpoint.claim.ice_servers.append({
        "urls": ["turn:other.example:3478"],
        "expires_at": 500.0,
    })

    assert endpoint.credentials_expire_at() == 500.0


@pytest.mark.asyncio
async def test_a_refused_reconnect_is_retried_not_permanent():
    """One transient 504 must not unregister a host for the rest of its life.

    The transport is dropped before retrying. Keeping it meant `connected`
    consulted an object that still claimed to be connected, so the loop
    short-circuited on every later tick: one warning in the log, silence after
    it, and an agent that never came back.
    """
    from a11.client import hosting

    endpoint = _endpoint()
    endpoint.identity = "agent"
    endpoint._connected = asyncio.Event()
    endpoint.on_drop_listener = None

    class DeadTransport:
        """A socket that has gone but has not noticed."""

        def connected(self) -> bool:
            return True

        def close(self) -> None:
            pass

    endpoint._transport = DeadTransport()
    assert endpoint.connected, "precondition: the stale transport looks alive"

    await endpoint._relinquish()

    assert not endpoint.connected
    assert endpoint._transport is None


# --- Being assigned an identity ----------------------------------------------


class _FakeExchange:
    """An exchange that records which claim call was made."""

    def __init__(self, granted: str = "acme--3f19c2b4") -> None:
        self.granted = granted
        self.by_name: list[str] = []
        self.scoped: list[dict] = []

    async def claim(self, identity, *, holder="", ttl_seconds=None):
        from a11.client.exchange import Claim

        self.by_name.append(identity)
        return Claim(
            identity=identity,
            token="t",
            expires_at="",
            renew_after="",
            signalling_url="wss://signal.example/ws",
            ice_servers=[],
        )

    async def claim_scoped(
        self, *, name="", organization="", holder="", ttl_seconds=None
    ):
        from a11.client.exchange import Claim

        self.scoped.append({"name": name, "organization": organization})
        return Claim(
            identity=self.granted,
            token="t",
            expires_at="",
            renew_after="",
            signalling_url="wss://signal.example/ws",
            ice_servers=[],
        )


def _startable(client, identity):
    """An endpoint whose connecting and maintaining are stubbed out."""
    from a11.client import hosting

    endpoint = hosting.HostedEndpoint(client, identity)
    endpoint._connect = _noop_connect
    return endpoint


async def _noop_connect() -> None:
    return None


@pytest.mark.asyncio
async def test_asking_for_no_identity_adopts_the_one_granted():
    """Bare `--hosted`: the exchange picks, and the endpoint takes that name.

    Everything after the claim -- renewing, rebinding, releasing -- is keyed on
    `identity`, so an endpoint that kept it empty would renew nothing and
    release nothing while looking healthy.
    """
    client = _FakeExchange()
    endpoint = _startable(client, None)

    await endpoint.start()
    try:
        assert endpoint.identity == "acme--3f19c2b4"
        assert client.scoped == [{"name": "", "organization": ""}]
        assert client.by_name == []
    finally:
        endpoint._stopped.set()
        if endpoint._maintainer is not None:
            endpoint._maintainer.cancel()


@pytest.mark.asyncio
async def test_a_named_identity_is_claimed_by_name():
    client = _FakeExchange()
    endpoint = _startable(client, "acme--staging")

    await endpoint.start()
    try:
        assert endpoint.identity == "acme--staging"
        assert client.by_name == ["acme--staging"]
        assert client.scoped == []
    finally:
        endpoint._stopped.set()
        if endpoint._maintainer is not None:
            endpoint._maintainer.cancel()


@pytest.mark.asyncio
async def test_an_organization_is_passed_on_when_one_was_named():
    client = _FakeExchange()
    endpoint = _startable(client, None)
    endpoint._organization = "acme"

    await endpoint.start()
    try:
        assert client.scoped == [{"name": "", "organization": "acme"}]
    finally:
        endpoint._stopped.set()
        if endpoint._maintainer is not None:
            endpoint._maintainer.cancel()


@pytest.mark.asyncio
async def test_a_claim_that_names_no_identity_is_refused():
    """Nothing to host is a failure, not a host that quietly serves nobody."""
    from a11.status import StatusException

    client = _FakeExchange(granted="")
    endpoint = _startable(client, None)

    with pytest.raises(StatusException):
        await endpoint.start()


# --- A credential that has stopped working -----------------------------------


def _maintaining(status_code) -> "hosting.HostedEndpoint":
    """An endpoint whose renewal always fails with ``status_code``."""
    from a11.client import hosting

    endpoint = _endpoint()
    endpoint.identity = "agent"
    endpoint._connected = asyncio.Event()
    endpoint._stopped = asyncio.Event()
    endpoint._fatal = None
    endpoint._transport = None
    endpoint.on_drop_listener = None
    endpoint._bound_expiry = None

    async def refuse() -> None:
        raise Status(
            code=status_code, message="The credential is not valid."
        ).to_exception()

    endpoint._renew_if_due = refuse
    return endpoint


@pytest.mark.parametrize(
    "code",
    [StatusCode.UNAUTHENTICATED, StatusCode.PERMISSION_DENIED],
    ids=["unauthenticated", "permission-denied"],
)
@pytest.mark.asyncio
async def test_a_credential_that_stopped_working_stops_hosting(
    code, monkeypatch
):
    """Rather than retrying it every thirty seconds for the life of the process.

    The bug this exists for: a personal key was given to a long-running host,
    the owner logged in again -- which revokes every live key of the same
    `client_name` -- and the host then logged the same 401 twice a minute for
    hours while presence, the listener and the log all went on saying
    "hosting". A revoked key never becomes valid again, so retrying it can only
    hide the one thing worth knowing.
    """
    from a11.client import hosting

    monkeypatch.setattr(hosting, "RENEW_POLL_SECONDS", 0.01)
    endpoint = _maintaining(code)

    await asyncio.wait_for(endpoint._maintain(), timeout=5)

    assert endpoint._stopped.is_set(), "hosting must stop, not loop"
    assert endpoint.fatal is not None
    assert endpoint.fatal.code == code
    # And a caller waiting on it is told, without having to poll.
    assert (
        await asyncio.wait_for(endpoint.wait_stopped(), timeout=5) is not None
    )


@pytest.mark.asyncio
async def test_a_transient_renewal_failure_keeps_hosting(monkeypatch):
    """The other half: UNAVAILABLE is the exchange having a moment.

    Giving up on one of those would turn a blip into an outage needing a human.
    """
    from a11.client import hosting

    monkeypatch.setattr(hosting, "RENEW_POLL_SECONDS", 0.01)
    monkeypatch.setattr(hosting, "MIN_RECONNECT_BACKOFF", 0.01)
    monkeypatch.setattr(hosting, "MAX_RECONNECT_BACKOFF", 0.01)
    endpoint = _maintaining(StatusCode.UNAVAILABLE)

    # Reconnection is not what this is about: the loop reaches it because the
    # fake has no transport, and a real re-register would need a live exchange.
    async def reconnected() -> None:
        return None

    endpoint._connect = reconnected
    calls = 0

    async def refuse_then_stop() -> None:
        nonlocal calls
        calls += 1
        if calls >= 3:
            endpoint._stopped.set()
        raise Status(
            code=StatusCode.UNAVAILABLE, message="try later"
        ).to_exception()

    endpoint._renew_if_due = refuse_then_stop

    await asyncio.wait_for(endpoint._maintain(), timeout=5)

    assert calls >= 3, "it must keep trying"
    assert endpoint.fatal is None, "a blip is not a reason to stop hosting"

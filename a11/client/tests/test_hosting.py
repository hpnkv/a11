# Copyright 2026 The A11 Authors.

"""Turning an exchange's ICE server list into a WebRTC configuration.

Worth its own tests because getting this wrong is silent. A host with no TURN
server, or one with the wrong port or transport, negotiates exactly as far as a
correct one and then never completes a handshake -- there is no error to read,
only a peer that never answers. So the translation is asserted field by field
rather than trusted to look right.
"""

from __future__ import annotations

import pytest

from a11 import net
from a11.client.hosting import hosted_configuration


def test_ice_is_multiplexed_by_default():
    """One UDP port for every peer that converges on a hosted agent.

    The dialling side must then not multiplex, which is what the exchange
    relay does.
    """
    configuration = hosted_configuration()

    assert configuration.enable_ice_udp_mux
    assert list(configuration.stun_servers) == []
    assert list(configuration.turn_servers) == []


def test_multiplexing_can_be_turned_off():
    configuration = hosted_configuration(multiplex_ice=False)

    assert not configuration.enable_ice_udp_mux


def test_stun_and_turn_are_separated():
    configuration = hosted_configuration(
        [
            {"urls": ["stun:stun.example:3478"]},
            {
                "urls": ["turn:turn.example:3478"],
                "username": "1787568765:a-claim",
                "credential": "the-derived-password",
            },
        ]
    )

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
    configuration = hosted_configuration(
        [
            {
                "urls": [
                    "turn:turn.example:3478",
                    "turn:turn.example:3478?transport=tcp",
                    "turns:turn.example:5349",
                ],
                "username": "u",
                "credential": "p",
            }
        ]
    )

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
    configuration = hosted_configuration(
        [{"urls": ["turn:turn.example", "turns:turn.example"]}]
    )

    assert [s.hostname for s in configuration.turn_servers] == [
        "turn.example",
        "turn.example",
    ]
    assert [s.port for s in configuration.turn_servers] == [3478, 5349]


def test_unknown_schemes_are_dropped_rather_than_guessed():
    """An exchange may advertise something this client does not speak."""
    configuration = hosted_configuration(
        [{"urls": ["https://turn.example/rest", "stun:stun.example:3478"]}]
    )

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
async def test_a_lapsing_credential_triggers_one_rebind():
    """The bug this exists for: a renewed claim never reaching the listener.

    A TURN credential is checked against the expiry in its own username, so
    once it passes the host cannot gather a relayed candidate -- while
    signalling stays up and presence stays online. It presents as an agent that
    worked for an hour and then stopped.
    """
    import time as time_module

    from a11.client import hosting

    endpoint = _endpoint()
    endpoint.identity = "agent"
    rebinds: list[int] = []

    async def on_rebind() -> None:
        rebinds.append(1)

    endpoint.on_rebind = on_rebind
    now = time_module.time()

    # Bound credentials nearly out, and a renewal has produced newer ones.
    endpoint._bound_expiry = now + 30
    endpoint.claim = _FakeClaim(now + 3600)
    await endpoint._rebind_if_lapsing()
    assert rebinds == [1]

    # Now that the listener holds the fresh expiry, it must settle.
    await endpoint._rebind_if_lapsing()
    assert rebinds == [1]


@pytest.mark.asyncio
async def test_no_rebind_while_the_bound_credential_has_time():
    import time as time_module

    endpoint = _endpoint()
    endpoint.identity = "agent"
    rebinds: list[int] = []

    async def on_rebind() -> None:
        rebinds.append(1)

    endpoint.on_rebind = on_rebind
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
    rebinds: list[int] = []

    async def on_rebind() -> None:
        rebinds.append(1)

    endpoint.on_rebind = on_rebind
    now = time_module.time()
    endpoint._bound_expiry = now + 10
    endpoint.claim = _FakeClaim(now + 10)

    await endpoint._rebind_if_lapsing()

    assert rebinds == []


def test_credentials_expire_at_takes_the_earliest():
    endpoint = _endpoint()
    endpoint.claim = _FakeClaim(1000.0)
    endpoint.claim.ice_servers.append(
        {"urls": ["turn:other.example:3478"], "expires_at": 500.0}
    )

    assert endpoint.credentials_expire_at() == 500.0

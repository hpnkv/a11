# Copyright 2026 The A11 Authors.

"""Turning an exchange's ICE server list into a WebRTC configuration.

Worth its own tests because getting this wrong is silent. A host with no TURN
server, or one with the wrong port or transport, negotiates exactly as far as a
correct one and then never completes a handshake -- there is no error to read,
only a peer that never answers. So the translation is asserted field by field
rather than trusted to look right.
"""

from __future__ import annotations

from a11 import net
from a11.client.hosting import hosted_configuration


def test_ice_is_not_multiplexed_by_default():
    """Because a relayed host has concurrent connections as its normal case.

    A muxed socket routes by source address, and every relayed packet arrives
    from the TURN server's address -- so a second concurrent connection
    completes its handshake and then receives nothing. Measured against the
    deployed exchange: 2 failures in 4 with muxing on, 0 in 6 with it off.
    """
    configuration = hosted_configuration()

    assert not configuration.enable_ice_udp_mux
    assert list(configuration.stun_servers) == []
    assert list(configuration.turn_servers) == []


def test_multiplexing_is_available_for_a_single_peer_host():
    configuration = hosted_configuration(multiplex_ice=True)

    assert configuration.enable_ice_udp_mux


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

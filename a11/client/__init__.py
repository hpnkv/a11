# Copyright 2026 The A11 Authors.

"""Talking to A11 peers: gateways, turns, and hosted identities.

The client half of what `a11 chat`, the IntelliJ plugin and `a11 serve
--hosted` all do. Keeping it here rather than inside the CLI is the point: a
turn loop is not a terminal concern, and neither is keeping a hosting claim
alive.

`ExchangeClient`, `CredentialStore` and `HostedEndpoint` speak to an A11
exchange -- the hosting service at `a11.services` -- over its documented HTTP
and WebSocket contract. The dependency runs one way, over the network, so the
exchange stays a separate thing A11 knows how to use rather than a part of it.
"""

from a11.client.connection import (
    DEFAULT_GATEWAY_URL,
    GatewayConnection,
    open_gateway,
)
from a11.client.credentials import (
    DEFAULT_EXCHANGE,
    Credential,
    CredentialStore,
)
from a11.client.exchange import Claim, ExchangeClient
from a11.client.hosting import HostedEndpoint
from a11.client.turn import TurnConfig, run_turn

__all__ = [
    "DEFAULT_EXCHANGE",
    "DEFAULT_GATEWAY_URL",
    "Claim",
    "Credential",
    "CredentialStore",
    "ExchangeClient",
    "GatewayConnection",
    "HostedEndpoint",
    "TurnConfig",
    "open_gateway",
    "run_turn",
]

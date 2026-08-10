# Copyright 2026 The A11 Authors.

"""Talking to an A11 gateway: connecting, probing, and driving one turn.

The client half of what `a11 chat` and the IntelliJ plugin both do. Keeping it
here rather than inside the CLI is the point: the turn loop is not a terminal
concern, and the terminal was the only place it existed.
"""

from a11.client.connection import (
    DEFAULT_GATEWAY_URL,
    GatewayConnection,
    open_gateway,
)
from a11.client.turn import TurnConfig, run_turn

__all__ = [
    "DEFAULT_GATEWAY_URL",
    "GatewayConnection",
    "TurnConfig",
    "open_gateway",
    "run_turn",
]

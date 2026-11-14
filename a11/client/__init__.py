# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Connect to A11 gateways and manage hosted identities.

`GatewayConnection` and `run_turn` support chat clients. `ExchangeClient`,
`CredentialStore` and `HostedEndpoint` use the exchange's HTTP and WebSocket
contract to manage hosting claims.
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

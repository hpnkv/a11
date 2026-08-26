import asyncio
from typing import Sequence

import a11
from absl import app
from absl import logging
from absl import flags

from a11 import logging as a11_logging
from a11 import timing
from a11.client.connection import GatewayConnection
from a11.client.discovery import install_peer_actions

RELAY = "wss://a11.to/ws"

_SERVER = flags.DEFINE_string(
    "server", "demoserver", "The hosted identity to call."
)


async def main(_: Sequence[str]) -> None:
    connection = await GatewayConnection.connect(
        f"{RELAY}/{_SERVER.value}", timeout=timing.Duration.seconds(15)
    )
    try:
        registry = a11.ActionRegistry()
        await install_peer_actions(connection, registry, names=["greet"])

        greet = await connection.action(
            "greet", registry.get_schema("greet")
        ).call()

        await greet["name"].finalize("Helena")

        async for piece in greet["reply"]:
            print(piece, end="")
        print()

        await greet.wait(timing.Duration.seconds(15))
    finally:
        await connection.aclose()


def sync_main(argv: Sequence[str]) -> None:
    a11_logging.set_level(logging.DEBUG)
    asyncio.run(main(argv))


if __name__ == "__main__":
    app.run(sync_main)

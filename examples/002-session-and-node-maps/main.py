import asyncio
from typing import Sequence

import a11
from absl import app as absl_app
from absl import logging

ECHO_SCHEMA = a11.ActionSchema(
    name="echo",
    description="Echoes `input` into `output` chunk by chunk.",
    inputs={
        "input": a11.ActionPortSchema(
            name="input", type="text/plain", required=True
        )
    },
    outputs={
        "output": a11.ActionPortSchema(
            name="output", type="text/plain", required=True
        )
    },
)


async def run_echo(action: a11.Action):
    async with action["output"] as output:
        async for fragment in action["input"].iter_fragments():
            await output.put_fragment(fragment)


def make_server_action_registry() -> a11.ActionRegistry:
    registry = a11.ActionRegistry()
    registry.register("echo", ECHO_SCHEMA, handler=run_echo)
    return registry


async def handler(request, response):
    print(request.protocol, request.body_stream)
    assert request.protocol == "echo"
    assert request.body_stream is not None
    response.send_headers(200, {"x-transport": "nghttp2"})
    async for data in request.body_stream:
        print(data)
        response.write(b"echo:" + data)
    response.finish()


async def main(_argv: Sequence[str]):
    server = a11.Http2Server.create(handler=handler)
    print(f"http://127.0.0.1/{server.port}")
    # try:
    #     while True:
    #         await asyncio.sleep(1)
    # except KeyboardInterrupt:
    #     server.stop()
    client = None
    try:
        client = await asyncio.wait_for(
            a11.Http2Client.connect("127.0.0.1", server.port), timeout=5
        )
        stream = client.extended_connect("echo", "/duplex")
        head = await asyncio.wait_for(stream.headers(), timeout=5)
        assert head.status == 200
        assert ("x-transport", "nghttp2") in head.headers

        stream.write(b"one")
        stream.write(b"two")
        stream.finish()
        assert [chunk async for chunk in stream] == [
            b"echo:one",
            b"echo:two",
        ]
        await asyncio.wait_for(stream.wait_done(), timeout=5)
    finally:
        if client is not None:
            client.close()
        server.stop()

    # server_session = a11.Session()
    # server_session.set_action_registry(make_server_action_registry())
    #
    # client_session = a11.Session()
    #
    # server_stream, client_stream = a11.InProcessWireStream.create_pair()
    #
    # client_session.add_stream(client_stream, mode="start")
    # server_session.add_stream(server_stream, mode="accept")
    #
    # echo = (
    #     a11.Action(ECHO_SCHEMA)
    #     .bind_session(client_session)
    #     .bind_node_map(client_session.node_map)
    #     .bind_stream(client_stream)
    # )
    # await echo.call()
    #
    # async with echo["input"] as input_node:
    #     await input_node.put("hello")
    #     await input_node.put(1, final=True)
    #
    #     async for text in echo["output"]:
    #         print(text)
    #
    # client_session.half_close()
    # for _, stream in client_session.streams():
    #     await stream.drain_outgoing_messages()
    #
    # server_session.half_close()
    #
    # await client_session.done.wait()
    # client_session.get_status().raise_if_not_ok()


def sync_main(argv: Sequence[str]):
    logging.use_absl_handler()
    logging.set_verbosity(logging.INFO)

    return asyncio.run(main(argv))


if __name__ == "__main__":
    absl_app.run(sync_main)

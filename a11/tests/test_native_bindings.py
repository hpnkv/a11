import asyncio
from pathlib import Path

import pytest

from a11 import timing
from a11.actions import (
    Action,
    ActionHeaderSchema,
    ActionPortSchema,
    ActionRegistry,
    ActionSchema,
)
from a11.data.types import ActionMessage, Chunk, NodeFragment, WireMessage
from a11.net.in_process_wire_stream import InProcessWireStream
from a11.net.wire_stream import WireStreamWithRecv
from a11.nodes.async_node import AsyncNode, NodeMap
from a11.service.session import Session, SessionWithRecv
from a11.status import Status, StatusCode, StatusException
from a11.stores.chunk_store import ChunkStore
from a11.stores.chunk_store_reader import ChunkStoreReader
from a11.stores.chunk_store_writer import ChunkStoreWriter

native = pytest.importorskip("a11._native")

_TEST_DATA = Path(__file__).parents[2] / "cpp" / "tests" / "testdata"


def test_public_stateful_types_are_the_bound_native_classes():
    assert Status is native.Status
    assert timing.Duration is native.Duration
    assert timing.Time is native.Time
    assert Chunk is native.Chunk
    assert NodeFragment is native.NodeFragment
    assert ActionMessage is native.ActionMessage
    assert WireMessage is native.WireMessage
    assert ChunkStore is native.ChunkStore
    assert ChunkStoreReader is native.ChunkStoreReader
    assert ChunkStoreWriter is native.ChunkStoreWriter
    assert InProcessWireStream is native.InProcessWireStream
    assert WireStreamWithRecv is native.WireStreamWithRecv
    assert AsyncNode is native.AsyncNode
    assert NodeMap is native.NodeMap
    assert ActionSchema is native.ActionSchema
    assert Action is native.Action
    assert ActionRegistry is native.ActionRegistry
    assert Session is native.Session
    assert SessionWithRecv is native.SessionWithRecv


def test_native_action_schema_maps_are_live_validated_views():
    schema = ActionSchema(name="mutable")
    schema.outputs["result"] = {
        "name": "result",
        "type": "application/json",
    }
    schema.outputs["result"].description = "live native value"
    schema.output_to_json_field["result"] = "payload"

    assert schema.outputs["result"].description == "live native value"
    assert schema.model_dump()["output_to_json_field"] == {"result": "payload"}

    with pytest.raises(StatusException):
        schema.outputs["wrong-key"] = ActionPortSchema(
            name="different-name", type="text/plain"
        )
    assert "wrong-key" not in schema.outputs

    with pytest.raises(StatusException):
        del schema.outputs["result"]
    assert "result" in schema.outputs

    schema.output_to_json_field.clear()
    del schema.outputs["result"]
    assert not schema.outputs


def test_native_action_schema_json_round_trip_preserves_binary_fields():
    schema = ActionSchema(
        name="json-round-trip",
        inputs={
            "request": ActionPortSchema(
                name="request",
                type="application/octet-stream",
                autofills=[Chunk(data=b"\x00\xff")],
            )
        },
        outputs={
            "response": ActionPortSchema(
                name="response", type="application/json"
            )
        },
        headers={
            "token": ActionHeaderSchema(name="token", default=b"\x01\xfe")
        },
        output_to_json_field={"response": "payload"},
    )

    restored = ActionSchema.model_validate_json(schema.model_dump_json())
    assert restored == schema
    assert isinstance(restored, native.ActionSchema)


def test_sync_native_failures_raise_public_status_exception():
    framing = native.ChannelFramingOptions()
    framing.split_size = 0
    signalling = native.SignallingMessage()
    rtc = native.WebRtcConfiguration()
    rtc.channel_split_size = 0
    sse = native.HttpSseOptions()
    sse.connect_endpoint = ""

    for validate in (
        framing.validate,
        signalling.validate,
        rtc.validate,
        sse.validate,
    ):
        with pytest.raises(StatusException) as raised:
            validate()
        assert raised.value.status.code == StatusCode.INVALID_ARGUMENT

    with pytest.raises(StatusException):
        native.validate_http_headers([("bad header", "value")])
    with pytest.raises(StatusException):
        native.TurnServer.from_string("")


def test_public_status_code_conversions_use_native_contract():
    http_codes = {
        StatusCode.OK: 200,
        StatusCode.CANCELLED: 499,
        StatusCode.UNKNOWN: 500,
        StatusCode.INVALID_ARGUMENT: 400,
        StatusCode.DEADLINE_EXCEEDED: 504,
        StatusCode.NOT_FOUND: 404,
        StatusCode.ALREADY_EXISTS: 409,
        StatusCode.PERMISSION_DENIED: 403,
        StatusCode.RESOURCE_EXHAUSTED: 429,
        StatusCode.FAILED_PRECONDITION: 400,
        StatusCode.ABORTED: 409,
        StatusCode.OUT_OF_RANGE: 400,
        StatusCode.UNIMPLEMENTED: 501,
        StatusCode.INTERNAL: 500,
        StatusCode.UNAVAILABLE: 503,
        StatusCode.DATA_LOSS: 500,
        StatusCode.UNAUTHENTICATED: 401,
    }
    websocket_codes = {
        StatusCode.OK: 1000,
        **{StatusCode(value): 3999 + value for value in range(1, 16)},
        StatusCode.UNAUTHENTICATED: 4007,
    }

    assert {code: code.to_http_code() for code in StatusCode} == http_codes
    assert {code: code.to_ws_code() for code in StatusCode} == websocket_codes
    assert StatusCode.from_http_code(204) is StatusCode.OK
    assert StatusCode.from_http_code(418) is StatusCode.FAILED_PRECONDITION
    assert StatusCode.from_ws_code(4004) is StatusCode.NOT_FOUND
    assert StatusCode.from_ws_code(-1) is StatusCode.UNKNOWN


def test_native_wire_sequence_views_mutate_the_owned_cpp_value():
    message = WireMessage()
    message.actions.append(ActionMessage(id="first", name="run"))
    message.actions.extend([ActionMessage(id="second", name="run")])
    message.node_fragments.append(
        NodeFragment(id="node", data=Chunk(data=b"payload"))
    )

    message.actions[0].name = "updated"
    message.actions[:] = list(reversed(message.actions.copy()))

    restored = WireMessage.from_msgpack(message.to_msgpack())
    assert [action.id for action in restored.actions] == ["second", "first"]
    assert restored.actions[1].name == "updated"
    assert restored.node_fragments[0].get_chunk().data == b"payload"


class _PythonChunkStore(native.ChunkStore):
    def __init__(self) -> None:
        super().__init__()
        self.fragments: list[NodeFragment] = []
        self.close_status: Status | None = None

    async def get(self, seq, deadline=None):
        return self.fragments[seq]

    async def get_by_arrival_order(self, arrival_order, deadline=None):
        return self.fragments[arrival_order]

    async def next(self, deadline=None, limit=1):
        return [*self.fragments[:limit]]

    async def put(self, fragment):
        return (await self.put_many([fragment]))[0]

    async def put_many(self, fragments):
        sequences = []
        for fragment in fragments:
            sequence = (
                len(self.fragments) if fragment.seq is None else fragment.seq
            )
            self.fragments.append(fragment.model_copy(update={"seq": sequence}))
            sequences.append(sequence)
        return sequences

    async def clear_data(self, seq):
        return self.fragments[seq]

    async def get_seq_for_arrival_order(self, arrival_order):
        return arrival_order

    async def get_final_seq(self):
        if not self.fragments or self.fragments[-1].continued:
            return None
        return self.fragments[-1].seq

    async def close_writes_with_status(
        self, status, return_status_if_already_closed=False
    ):
        if self.close_status is not None and return_status_if_already_closed:
            return self.close_status
        self.close_status = status
        return status

    async def size(self):
        return len(self.fragments)

    def get_id(self):
        return "python-store"


class _PythonWireStream(native.WireStream):
    def __init__(self) -> None:
        super().__init__()
        self.sent: list[WireMessage] = []
        self.half_close_trailers = None
        self._deadline = timing.infinite_future()
        self._trailers = {"remote": b"done"}

    def send(self, message):
        self.sent.append(message)

    async def start(self, on_message, on_done):
        message = WireMessage(
            node_fragments=[
                NodeFragment(id="python", data=Chunk(data=b"incoming"))
            ]
        )
        await on_message(message)
        await on_message(None)
        await on_done()

    async def accept(self, on_message, on_done):
        await self.start(on_message, on_done)

    def half_close(self, trailers=None):
        self.half_close_trailers = trailers or {}

    async def drain_outgoing_messages(self):
        return None

    def abort(self, status):
        return None

    def set_deadline(self, deadline=None):
        self._deadline = (
            timing.infinite_future() if deadline is None else deadline
        )

    @property
    def deadline(self):
        return self._deadline

    def get_status(self):
        return Status.ok()

    def get_trailers(self):
        return self._trailers

    def get_id(self):
        return "python-wire"

    def get_impl(self):
        return self


@pytest.mark.asyncio
async def test_python_chunk_store_is_consumed_through_cpp_virtual_interface():
    store = _PythonChunkStore()
    wire = _PythonWireStream()
    writer = native.ChunkStoreWriter(store)
    writer.attach_stream(wire)

    confirmation = await writer.put_chunk(Chunk(data=b"payload"), final=True)
    sequence = await confirmation
    assert sequence == 0
    assert store.fragments == [
        NodeFragment(
            id="python-store",
            data=Chunk(data=b"payload"),
            seq=0,
            continued=False,
        )
    ]
    assert wire.sent == [WireMessage(node_fragments=[store.fragments[0]])]

    await writer.drain_and_close()
    assert store.close_status is not None
    assert store.close_status.is_ok()


@pytest.mark.asyncio
async def test_python_wire_stream_is_consumed_through_cpp_pull_adapter():
    stream = _PythonWireStream()
    adapter = native.WireStreamWithRecv(stream)

    started = asyncio.ensure_future(adapter.start())
    message = await asyncio.wait_for(adapter.receive(), timeout=5)
    eof = await asyncio.wait_for(adapter.receive(), timeout=5)
    await asyncio.wait_for(started, timeout=5)

    assert message.node_fragments[0].data.data == b"incoming"
    assert eof is None
    assert adapter.get_id() == "python-wire"
    assert adapter.get_trailers() == {"remote": b"done"}
    assert adapter.get_status().is_ok()

    outbound = WireMessage(
        node_fragments=[NodeFragment(id="cpp", data=Chunk(data=b"outgoing"))]
    )
    adapter.send(outbound)
    adapter.half_close({"local": b"done"})
    await adapter.drain_outgoing_messages()
    assert stream.sent == [outbound]
    assert stream.half_close_trailers == {"local": b"done"}


@pytest.mark.asyncio
async def test_native_sse_pull_adapters_half_close_without_cancelling_peer():
    server = native.HttpSseServer.create()
    try:
        raw_client = native.HttpSseClientWireStream(
            f"http://127.0.0.1:{server.port}"
        )
        client = native.WireStreamWithRecv(raw_client)
        client_started = asyncio.ensure_future(client.start())
        raw_accepted = await asyncio.wait_for(
            server.wait_for_stream(), timeout=5
        )
        accepted = native.WireStreamWithRecv(raw_accepted)
        await asyncio.wait_for(accepted.accept(), timeout=5)
        await asyncio.wait_for(client_started, timeout=5)

        to_server = WireMessage(
            node_fragments=[
                NodeFragment(id="client", data=Chunk(data=b"payload"))
            ]
        )
        to_client = WireMessage(
            node_fragments=[
                NodeFragment(id="server", data=Chunk(data=b"payload"))
            ]
        )
        client.send(to_server)
        accepted.send(to_client)
        assert (
            await asyncio.wait_for(accepted.receive(), timeout=5) == to_server
        )
        assert await asyncio.wait_for(client.receive(), timeout=5) == to_client

        client.half_close({"client": b"done"})
        accepted.half_close({"server": b"done"})
        await asyncio.wait_for(
            asyncio.gather(
                client.drain_outgoing_messages(),
                accepted.drain_outgoing_messages(),
            ),
            timeout=5,
        )

        assert await asyncio.wait_for(accepted.receive(), timeout=5) is None
        assert await asyncio.wait_for(client.receive(), timeout=5) is None
        assert accepted.get_status().is_ok()
        assert client.get_status().is_ok()
        assert accepted.get_trailers()["client"] == b"done"
        assert client.get_trailers() == {"server": b"done"}
    finally:
        server.stop()


@pytest.mark.asyncio
async def test_native_http2_tls_invokes_python_handler():
    server_options = native.Http2Options()
    server_options.tls.enabled = True
    server_options.tls.certificate_pem_file = str(
        _TEST_DATA / "localhost-cert.pem"
    )
    server_options.tls.key_pem_file = str(_TEST_DATA / "localhost-key.pem")

    async def handler(request, response):
        assert request.scheme == "https"
        response.send_response(200, body=b"python-handler")

    server = native.Http2Server.create(handler=handler, options=server_options)
    client = None
    try:
        client_options = native.Http2Options()
        client_options.tls.enabled = True
        client_options.tls.ca_certificate_pem_file = str(
            _TEST_DATA / "localhost-cert.pem"
        )
        client = await asyncio.wait_for(
            native.Http2Client.connect(
                "127.0.0.1", server.port, client_options
            ),
            timeout=5,
        )
        response = await asyncio.wait_for(
            client.request("GET", "/secure"), timeout=5
        )
        assert server.secure
        assert client.secure
        assert response.head.status == 200
        assert response.body == b"python-handler"
    finally:
        if client is not None:
            client.close()
        server.stop()


@pytest.mark.asyncio
async def test_native_action_invokes_async_python_handler():
    port_type = "application/octet-stream"
    schema = native.ActionSchema(
        "echo",
        inputs={"input": native.ActionPortSchema("input", port_type)},
        outputs={"output": native.ActionPortSchema("output", port_type)},
    )

    async def handler(action):
        source = action.get_input("input", False)
        destination = action.get_output("output", False)
        chunk = await source.next_chunk()
        assert chunk is not None
        await destination.put_chunk(chunk, final=True)

    action = native.Action(schema, "python-action", handler)
    source = action.get_input("input", False)
    await source.put_chunk(Chunk(data=b"echo"), final=True)
    action.run()
    await asyncio.wait_for(action.wait(timing.Duration.seconds(5)), timeout=5)

    result = await action.get_output("output", False).next_chunk()
    assert result == Chunk(data=b"echo")
    assert action.get_status().is_ok()


@pytest.mark.asyncio
async def test_native_session_marshals_stream_callbacks_to_python_loop():
    received = asyncio.Event()
    completed = asyncio.Event()
    messages = []

    async def on_message(message, stream, session):
        assert stream.get_id()
        assert session.get_id() == "receiver"
        if message is not None:
            messages.append(message)
            received.set()

    async def on_done(stream, session):
        assert stream.get_id()
        assert session.get_status().is_ok()
        completed.set()

    options = native.SessionOptions(
        no_stream_timeout=timing.infinite_duration()
    )
    sender = native.Session("sender", options=options)
    receiver = native.Session("receiver", on_message, on_done, options=options)
    client_stream, server_stream = native.create_in_process_wire_stream_pair()
    await asyncio.gather(
        sender.add_stream(client_stream, "start"),
        receiver.add_stream(server_stream, "accept"),
    )

    message = WireMessage(
        node_fragments=[NodeFragment(id="node", data=Chunk(data=b"value"))]
    )
    sender.send(message)
    await asyncio.wait_for(received.wait(), timeout=5)
    assert messages == [message]

    sender.half_close()
    receiver.half_close()
    await asyncio.wait_for(completed.wait(), timeout=5)
    await asyncio.wait_for(
        asyncio.gather(client_stream.wait(), server_stream.wait()), timeout=5
    )


@pytest.mark.asyncio
async def test_native_webrtc_streams_cross_python_callbacks():
    signalling = native.SignallingService.create()
    accepted_future = asyncio.get_running_loop().create_future()

    async def on_stream(stream):
        accepted = native.WireStreamWithRecv(stream)
        await accepted.accept()
        accepted_future.set_result(accepted)

    server = native.WebRtcWireServer.create("server", signalling, on_stream)
    try:
        raw_client = native.WebRtcWireStream.create_client(
            "client", "server", signalling
        )
        client = native.WireStreamWithRecv(raw_client)
        await asyncio.wait_for(client.start(), timeout=10)
        accepted = await asyncio.wait_for(accepted_future, timeout=10)

        payload = b"x" * (220 * 1024)
        message = WireMessage(
            node_fragments=[NodeFragment(id="large", data=Chunk(data=payload))]
        )
        client.send(message)
        assert await asyncio.wait_for(accepted.receive(), timeout=10) == message

        reply = WireMessage(
            node_fragments=[NodeFragment(id="reply", data=Chunk(data=b"hello"))]
        )
        accepted.send(reply)
        assert await asyncio.wait_for(client.receive(), timeout=10) == reply

        client.half_close()
        accepted.half_close()
        await asyncio.wait_for(
            asyncio.gather(
                client.drain_outgoing_messages(),
                accepted.drain_outgoing_messages(),
            ),
            timeout=10,
        )
        assert await asyncio.wait_for(client.receive(), timeout=10) is None
        assert await asyncio.wait_for(accepted.receive(), timeout=10) is None
        assert client.get_status().is_ok()
        assert accepted.get_status().is_ok()
    finally:
        server.stop()
        signalling.stop()

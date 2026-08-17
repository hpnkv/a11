"""
Native C++ backend for A11
"""

from __future__ import annotations
import a11.status
import a11.stores.local_chunk_store
import asyncio
import fastapi
import httpx
import collections.abc
import msgpack._cmsgpack
import typing
import typing_extensions
import os

T = typing_extensions.TypeVar("T", default=typing.Any)

class _DoneEvent(typing.Protocol):
    def is_set(self) -> bool: ...
    async def wait(self) -> bool: ...

ActionHandler = collections.abc.Callable[
    ["Action"], collections.abc.Awaitable[None]
]
OnTranscription = collections.abc.Callable[
    [str | None], collections.abc.Awaitable[None]
]
OnRecognitionDone = collections.abc.Callable[
    [], collections.abc.Awaitable[None]
]
from . import flow

__all__: list[str] = [
    "ACCEPT",
    "ACTION_DISPATCH_STATUS_OUTPUT",
    "ACTION_HEADER_PREFIX",
    "ACTION_STATUS_MIMETYPE",
    "ACTION_STATUS_OUTPUT",
    "Action",
    "ActionHeaderSchema",
    "ActionMessage",
    "ActionPortSchema",
    "ActionRegistry",
    "ActionSchema",
    "ActionSettings",
    "AsyncNode",
    "AudioBuffer",
    "AudioCaptureEvent",
    "AudioControlEvent",
    "AudioDeviceInfo",
    "AudioInput",
    "AudioInputOptions",
    "AudioModelSpec",
    "AudioSubscription",
    "CANCEL_ACTION_HEADER",
    "CANCEL_ACTION_NAME",
    "CANDIDATE",
    "CLOSE_STATUS_ATTRIBUTE",
    "ChannelFramingOptions",
    "Chunk",
    "ChunkMetadata",
    "ChunkStore",
    "ChunkStoreReader",
    "ChunkStoreReaderOptions",
    "ChunkStoreWriter",
    "ChunkStoreWriterOptions",
    "DEFAULT_ASR_MODEL",
    "DEFAULT_MAX_CONCURRENT_NESTED_ACTIONS",
    "DEFAULT_SSE_CONNECT_ENDPOINT",
    "DEFAULT_SSE_MESSAGE_ENDPOINT",
    "DEFAULT_VAD_MODEL",
    "DESCRIPTION",
    "DownloadOptions",
    "Duration",
    "EMPTY_WIRE_MESSAGE_SIZE",
    "ERROR",
    "FetchOptions",
    "Http2Client",
    "Http2DuplexStream",
    "Http2Options",
    "Http2RequestBodyStream",
    "Http2ResponseStream",
    "Http2ResponseWriter",
    "Http2Server",
    "Http2TlsOptions",
    "HttpProtocolPreference",
    "HttpPushedResponse",
    "HttpRequest",
    "HttpResponse",
    "HttpResponseHead",
    "HttpSseClientWireStream",
    "HttpSseOptions",
    "HttpSseServer",
    "HttpSseServerWireStream",
    "HttpSseWireStream",
    "HttpSseWireStreamServer",
    "InProcessWireStream",
    "JSON_MIMETYPE",
    "LocalChunkStore",
    "MAKE_HTTP_REQUEST_ACTION",
    "MAX_SINGLE_MESSAGE_SIZE",
    "MSGPACK_MIMETYPE",
    "NativeActionHandler",
    "NodeFragment",
    "NodeMap",
    "NodeRef",
    "OTEL_BAGGAGE_HEADER",
    "OTEL_TRACEPARENT_HEADER",
    "OTEL_TRACESTATE_HEADER",
    "ParsedUrl",
    "Port",
    "RedisChunkStore",
    "RedisChunkStoreKeys",
    "RedisChunkStoreMetadata",
    "RedisChunkStoreOptions",
    "RedisClient",
    "RedisClientOptions",
    "RedisReply",
    "RedisReplyType",
    "RedisSubscription",
    "SESSION_MAX_SINGLE_MESSAGE_SIZE",
    "SESSION_STATUS_HEADER",
    "SQLiteChunkStore",
    "SQLiteChunkStoreFactory",
    "SQLiteChunkStoreMetadata",
    "SQLiteChunkStoreOptions",
    "SQLiteSynchronous",
    "SSE_HTTP_HEADER_PREFIX",
    "SSE_STREAM_ID_HEADER",
    "START",
    "SerializationRegistry",
    "Service",
    "ServiceOptions",
    "Session",
    "SessionOptions",
    "SessionWithRecv",
    "SignallingEndpoint",
    "SseOutboundDelivery",
    "SignallingMessage",
    "SignallingMessageType",
    "SignallingService",
    "SignallingTransport",
    "SpeechRecognizer",
    "SpeechRecognizerOptions",
    "Status",
    "StreamMode",
    "TCP",
    "TLS",
    "Time",
    "TranscriptionEvent",
    "TurnRelayType",
    "TurnServer",
    "UDP",
    "WEB_FETCH_ACTION",
    "WHOLE_JSON",
    "WIRE_MESSAGE_VERSION",
    "WIRE_STREAM_ABORT_STATUS_HEADER",
    "WIRE_STREAM_MAX_SINGLE_MESSAGE_SIZE",
    "WebRtcConfiguration",
    "WebRtcWireServer",
    "WebRtcWireStream",
    "WebSocketClientOptions",
    "WebSocketServerOptions",
    "WebSocketSignallingClient",
    "WebSocketSignallingClientOptions",
    "WebSocketSignallingServer",
    "WebSocketSignallingServerOptions",
    "WebSocketWireServer",
    "WebSocketWireStream",
    "WireMessage",
    "WireStream",
    "WireStreamOptions",
    "WireStreamWithRecv",
    "asr_model_shorthands",
    "audio_actions",
    "audio_buffer_from_msgpack",
    "audio_buffer_to_msgpack",
    "audio_device_info",
    "audio_model_cache_dir",
    "create_in_process_wire_stream_pair",
    "default_audio_input_device",
    "default_redis_client",
    "download",
    "emit_log",
    "fetch",
    "file_sha1",
    "flow",
    "get_http_header",
    "http_actions",
    "is_close_status_chunk",
    "is_half_close_message",
    "is_status_chunk",
    "list_audio_devices",
    "lookup_asr_model",
    "lookup_vad_model",
    "make_half_close_message",
    "normalize_session_headers",
    "obs_clear_recorded_spans",
    "obs_configure",
    "obs_is_configured",
    "obs_recorded_spans",
    "obs_shutdown",
    "obs_start_span",
    "parse_url",
    "register_audio_actions",
    "register_http_actions",
    "reset_default_redis_client",
    "resolve_asr_model",
    "resolve_url_reference",
    "resolve_vad_model",
    "set_default_redis_client",
    "set_log_sink",
    "set_min_log_level",
    "set_stderr_threshold",
    "set_vlog_level",
    "status_code_from_http",
    "status_code_from_websocket",
    "status_code_to_http",
    "status_code_to_websocket",
    "status_from_chunk",
    "status_to_chunk",
    "vad_model_shorthands",
    "validate_http_headers",
    "validate_name_string",
]

class Action:
    """
    A runnable unit of work with typed input/output ports and headers.
    """

    @staticmethod
    def make_node_id(action_id: str, node_name: str) -> str:
        """
        Build the node id for a named port of the given action.
        """

    @staticmethod
    def run_in_background(*args, **kwargs):
        """
        run(self: Action) -> Action

        Run the action's handler and return the running action. The Python layer also exposes the native entry point as `run_in_background`.

        Examples:
            Start local work after binding its handler:

            ```python
            job = Action(SUMMARISE).bind_handler(summarise).run()
            ```
        """

    def __contains__(self, name: str) -> bool:
        """
        Return True when the action has a port with the given name.
        """

    def __getitem__(self, name: str) -> AsyncNode:
        """
        Return the port node with the given name.
        """

    def __init__(
        self,
        schema: ActionSchema,
        action_id: str = "",
        handler: ActionHandler | NativeActionHandler | None = None,
        *,
        node_map: NodeMap | None = None,
        stream: WireStream | None = None,
        session: Session | None = None,
        registry: ActionRegistry | None = None,
        max_concurrent_nested_actions: typing.SupportsInt = 64,
    ) -> None:
        """
        Create an action from a schema and optional bindings.
        """

    def add_done_callback(
        self,
        callback: typing.Callable[
            ["Action"], typing.Any | Awaitable[typing.Any]
        ],
    ) -> asyncio.Task:
        """
        Invoke ``callback(action)`` once this action completes.

        The callback fires exactly once when the action finishes for any reason
        -- normal completion, a handler error, or cancellation -- because it is
        driven off the same completion view as ``done`` (whose wait swallows the
        operation status). If the action is already done, the callback still
        runs on the next event-loop iteration.

        A synchronous callback runs to completion; one returning an awaitable is
        awaited. Cleanup routines registered here should therefore never assume
        success -- they run on the failure and cancellation paths too, which is
        exactly what makes this the right hook for releasing resources (e.g.
        tearing down a transient shell) tied to the action's lifetime.

        Returns the scheduled ``asyncio.Task`` so the caller can cancel the
        pending callback or await it. Must be called from within a running event
        loop.
        """

    def bind_handler(
        self, handler: ActionHandler | NativeActionHandler | None
    ) -> Action:
        """
        Bind the action's handler and return the action.
        """

    def bind_node_map(self, node_map: NodeMap) -> Action:
        """
        Bind the action's node map and return the action.
        """

    def bind_registry(self, registry: ActionRegistry | None) -> Action:
        """
        Bind the action's registry and return the action.
        """

    def bind_session(self, session: Session) -> Action:
        """
        Bind the action's session and return the action.
        """

    def bind_stream(self, stream: WireStream) -> Action:
        """
        Bind the action's wire stream and return the action.
        """

    def bind_streams_on_inputs_by_default(self, bind: bool) -> Action:
        """
        Set default stream binding for inputs and return the action.
        """

    def bind_streams_on_outputs_by_default(self, bind: bool) -> Action:
        """
        Set default stream binding for outputs and return the action.
        """

    def call(
        self, wire_headers: collections.abc.Mapping[str, bytes] | None = None
    ) -> asyncio.Future[Action]:
        """
        Dispatch the action remotely and return a future of the action.

        Examples:
            Call a child action and consume its result:

            ```python
            lookup = action.make_nested("find_customer")
            await lookup["email"].put_final(request.email)
            await lookup["email"].drain_and_close()
            await lookup.call()
            customer = await lookup["customer"].consume(obj_type=Customer)
            ```
        """

    def cancel(self) -> None:
        """
        Cancel the action.
        """

    def cancelled(self) -> bool:
        """
        Return True when the action has been cancelled.
        """

    def clear_inputs_after_run(self, clear: bool = True) -> Action:
        """
        Set whether inputs are cleared after run and return the action.
        """

    def clear_outputs_after_run(self, clear: bool = True) -> Action:
        """
        Set whether outputs are cleared after run and return the action.
        """

    def contains_port(self, name: str) -> bool:
        """
        Return True when the action has a port with the given name.
        """

    def forward_header(self, target: Action, name: str) -> None:
        """
        Copy a single header from this action to a target action.
        """

    def forward_headers_with_prefix(
        self, target: Action, prefix: str = "x-a11-"
    ) -> None:
        """
        Copy all headers with the given prefix to a target action.
        """

    def get_action_message(self) -> ActionMessage:
        """
        Return the action's wire message representation.
        """

    def get_dispatch_status(self) -> Status | None:
        """
        Return the action's dispatch status, or None when not dispatched.
        """

    def get_handler(self) -> ActionHandler | NativeActionHandler | None:
        """
        Return the action's Python handler, or None.
        """

    @typing.overload
    def get_header(
        self, name: str, decode: typing.Literal[False] = False
    ) -> bytes | None:
        """
        Return header ``name`` (``None`` if absent); ``decode`` UTF-8 to
        ``str``.
        """
    @typing.overload
    def get_header(
        self, name: str, decode: typing.Literal[True] = True
    ) -> str | None: ...
    @typing.overload
    def get_header(
        self, name: str, decode: bool = False
    ) -> bytes | str | None: ...

    def get_id(self) -> str:
        """
        Return the action's id.
        """

    def get_input(
        self, name: str, bind_stream: bool | None = None
    ) -> AsyncNode:
        """
        Return the input port node with the given name.
        """

    def get_node(self, node_id: str) -> AsyncNode:
        """
        Return the async node with the given id.
        """

    def get_node_map(self) -> NodeMap:
        """
        Return the action's bound node map.
        """

    def get_output(
        self, name: str, bind_stream: bool | None = None
    ) -> AsyncNode:
        """
        Return the output port node with the given name.
        """

    def get_port(self, name: str) -> AsyncNode:
        """
        Return the port node with the given name.
        """

    def get_registry(self) -> ActionRegistry:
        """
        Return the action's bound registry.
        """

    def get_schema(self) -> ActionSchema:
        """
        Return the action's schema.
        """

    def get_session(self) -> Session:
        """
        Return the action's bound session.
        """

    def get_status(self) -> Status:
        """
        Return the action's completion status.
        """

    def get_stream(self) -> WireStream:
        """
        Return the action's bound wire stream.
        """

    def has_been_called(self) -> bool:
        """
        Return True when the action has been dispatched remotely.
        """

    def has_been_run(self) -> bool:
        """
        Return True when the action has been run locally.
        """

    def has_handler(self) -> bool:
        """
        Return True when the action has a handler bound.
        """

    def has_header(self, name: str) -> bool:
        """
        Return True when the action has a header with the given name.
        """

    def is_done(self) -> bool:
        """
        Return True when the action has finished.
        """

    @typing.overload
    def make_nested(
        self,
        schema: ActionSchema,
        propagate_io: bool = True,
        forward_headers: bool = True,
    ) -> Action:
        """
        Create a nested child action from a schema.
        """
    @typing.overload
    def make_nested(
        self,
        action_name: str,
        propagate_io: bool = True,
        forward_headers: bool = True,
    ) -> Action:
        """
        Create a nested child action from a registered action name.

        Examples:
            Prepare a registered lookup while preserving the parent context:

            ```python
            lookup = action.make_nested("find_customer")
            ```
        """

    def map_ports_from_message(self, message: ActionMessage) -> Action:
        """
        Map the action's ports from a wire message and return the action.
        """

    def remove_header(self, name: str) -> None:
        """
        Remove the header with the given name.
        """

    def run(self) -> Action:
        """
        Run the action's handler and return the running action. The Python layer also exposes the native entry point as `run_in_background`.

        Examples:
            Start local work after binding its handler:

            ```python
            job = Action(SUMMARISE).bind_handler(summarise).run()
            ```
        """

    def set_header(self, name: str, value: typing.Any) -> Action:
        """
        Set a header from a str or bytes value and return the action.
        """

    def set_id(self, action_id: str) -> Action:
        """
        Set the action's id and return the action.
        """

    def set_on_cancelled(self, callback: typing.Any) -> None:
        """
        Register a synchronous callback invoked when the action is cancelled.
        """

    def set_schema(self, schema: ActionSchema) -> Action:
        """
        Set the action's schema and return the action.
        """

    def set_span_attribute(self, key: str, value: typing.Any) -> None:
        """
        Set an attribute on the action's span; no-op when untraced.
        """

    def set_span_input(self, value: typing.Any) -> None:
        """
        Record this action span's input (Langfuse observation input).
        """

    def set_span_name(self, name: str) -> None:
        """
        Set the name of the action's span.
        """

    def set_span_output(self, value: typing.Any) -> None:
        """
        Record this action span's output (Langfuse observation output).
        """

    def set_span_status(self, code: str, description: str = "") -> None:
        """
        Set the span status explicitly ('ok', 'error' or 'unset').
        """

    def wait(self, timeout: Duration | None = None) -> asyncio.Future[Action]:
        """
        Return a future that resolves when the action completes.
        """

    def wait_for_dispatch(
        self, timeout: Duration | None = None
    ) -> asyncio.Future[Status]:
        """
        Return a future that resolves when the action has been dispatched.
        """

    @property
    def done(self) -> _DoneEvent:
        """
        An `asyncio.Event`-shaped view of completion (``await
        action.done.wait()``).
        """

    @property
    def headers(self) -> dict[str, bytes]:
        """
        The action's headers as a mapping of name to bytes.
        """

    @property
    def id(self) -> str:
        """
        The action's id.
        """
    @id.setter
    def id(self, arg1: str) -> None: ...

    @property
    def schema(self) -> ActionSchema:
        """
        The action's schema.
        """
    @schema.setter
    def schema(self, arg1: ActionSchema) -> None: ...

    @property
    def settings(self) -> ActionSettings:
        """
        The action's live `ActionSettings` (field writes propagate back).
        """
    @settings.setter
    def settings(self, settings: ActionSettings) -> None: ...

    @property
    def span_id(self) -> str:
        """
        The action's span id as lowercase hex, or empty when untraced.
        """

    @property
    def trace_id(self) -> str:
        """
        The action's trace id as lowercase hex, or empty when untraced.
        """

class ActionHeaderSchema:
    """
    Schema describing a single header of an action.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(cls, value: str | bytes, **_: typing.Any): ...
    def __eq__(self, arg0: object) -> bool:
        """
        Return True when two header schemas are equal.
        """

    def __init__(
        self,
        name: str,
        description: str = "",
        default: typing.Any | None = None,
    ) -> None:
        """
        Create a validated header schema.
        """

    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, *, mode: str = "python", **_: typing.Any): ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def validate(self) -> None:
        """
        Validate the header schema, raising on error.
        """

    @property
    def default(self) -> bytes | None:
        """
        Default header value as bytes, or None when unset.
        """
    @default.setter
    def default(self, arg1: typing.Any) -> None: ...

    @property
    def description(self) -> str:
        """
        Human-readable description of the header.
        """
    @description.setter
    def description(self, arg0: str) -> None: ...

    @property
    def name(self) -> str:
        """
        The header's name.
        """
    @name.setter
    def name(self, arg0: str) -> None: ...

class ActionMessage:
    """
    A message invoking a named action with input and output ports.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @staticmethod
    def from_msgpack(data: typing.Any) -> ActionMessage:
        """
        Deserialize a value from MessagePack bytes.
        """

    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_construct(cls, **values: typing.Any):
        """
        Construct a native value from trusted field values.

        Native records retain C++ invariants. This validates input rather than
        creating an invalid object.
        """

    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(
        cls, value: str | bytes | bytearray, **_: typing.Any
    ): ...
    def __copy__(self) -> ActionMessage:
        """
        Return a shallow copy of the value.
        """

    def __deepcopy__(self, memo: dict) -> ActionMessage:
        """
        Return a deep copy of the value.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether two values are equal.
        """

    def __init__(
        self,
        id: str = "",
        name: str = "",
        inputs: typing.Any = [],
        outputs: typing.Any = [],
        headers: collections.abc.Mapping[str, bytes] | None = {},
    ) -> None:
        """
        Create an action message from id, name, ports, and headers.
        """

    def __repr__(self) -> str:
        """
        Return a human-readable debug string.
        """

    def debug_string(self) -> str:
        """
        Return a human-readable debug string.
        """

    def model_copy(
        self, *, update: dict[str, typing.Any] | None = None, deep: bool = False
    ): ...
    def model_dump(
        self, *, mode: str = "python", **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def to_msgpack(self) -> bytes:
        """
        Serialize the value to MessagePack bytes.
        """

    def validate(self) -> None:
        """
        Raise if the value fails structural validation.
        """

    @property
    def approx_bytes(self) -> int:
        """
        Approximate in-memory size of the message in bytes.
        """

    @property
    def headers(self) -> _ByteMapView:
        """
        Byte-string header map attached to the action.
        """
    @headers.setter
    def headers(
        self, arg1: collections.abc.Mapping[str, bytes] | None
    ) -> None: ...

    @property
    def id(self) -> str:
        """
        Identifier of the action invocation.
        """
    @id.setter
    def id(self, arg0: str) -> None: ...

    @property
    def inputs(self) -> _PortVectorView:
        """
        Input ports of the action.
        """
    @inputs.setter
    def inputs(self, arg1: typing.Any) -> None: ...

    @property
    def name(self) -> str:
        """
        Name of the action being invoked.
        """
    @name.setter
    def name(self, arg0: str) -> None: ...

    @property
    def outputs(self) -> _PortVectorView:
        """
        Output ports of the action.
        """
    @outputs.setter
    def outputs(self, arg1: typing.Any) -> None: ...

class ActionPortSchema:
    """
    Schema describing a single input or output port of an action.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(cls, value: str | bytes, **_: typing.Any): ...
    def __eq__(self, arg0: object) -> bool:
        """
        Return True when two port schemas are equal.
        """

    def __init__(
        self,
        name: str,
        type: str,
        description: str = "",
        required: bool = False,
        unary: bool = False,
        autofills: typing.Any | None = None,
        typeinfo: type | None = None,
    ) -> None:
        """
        Create a validated port schema.
        """

    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, *, mode: str = "python", **_: typing.Any): ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def validate(self) -> None:
        """
        Validate the port schema, raising on error.
        """

    @property
    def autofills(self) -> list[NodeFragment | None] | None:
        """
        Node fragments used to autofill the port, or None entries.
        """
    @autofills.setter
    def autofills(self, arg1: typing.Any) -> None: ...

    @property
    def description(self) -> str:
        """
        Human-readable description of the port.
        """
    @description.setter
    def description(self, arg0: str) -> None: ...

    @property
    def name(self) -> str:
        """
        The port's name.
        """
    @name.setter
    def name(self, arg0: str) -> None: ...

    @property
    def required(self) -> bool:
        """
        Whether the port must be provided.
        """
    @required.setter
    def required(self, arg0: bool) -> None: ...

    @property
    def type(self) -> str:
        """
        The port's data type name.
        """
    @type.setter
    def type(self, arg0: str) -> None: ...

    @property
    def typeinfo(self) -> type | None:
        """
        Optional Python type associated with the port, or None.
        """
    @typeinfo.setter
    def typeinfo(self, arg1: typing.Any) -> None: ...

    @property
    def unary(self) -> bool:
        """
        Whether the port carries a single value.
        """
    @unary.setter
    def unary(self, arg0: bool) -> None: ...

class ActionRegistry:
    """
    Registry mapping action names to their schemas and handlers.
    """

    def __init__(self) -> None:
        """
        Create an empty action registry.
        """

    def copy(self, clear_autofills: bool = True) -> ActionRegistry:
        """
        Return a copy of the registry, optionally clearing autofills.
        """

    def get_handler(
        self, action_name: str
    ) -> ActionHandler | NativeActionHandler | None:
        """
        Return the handler registered under the given action name: the Python callable it was registered with, a NativeActionHandler when the action is implemented in C++, or None when it has no handler.
        """

    def get_schema(self, action_name: str) -> ActionSchema:
        """
        Return the schema registered under the given action name.
        """

    def is_registered(self, action_name: str) -> bool:
        """
        Return True when an action with the given name is registered.
        """

    def list_registered_actions(self) -> list[str]:
        """
        Return the names of all registered actions.
        """

    def make_action(
        self,
        action_name: str,
        action_id: str = "",
        node_map: NodeMap | None = None,
        stream: WireStream | None = None,
        session: Session | None = None,
    ) -> Action:
        """
        Create an action instance from a registered action name.

        Examples:
            Construct and start work without repeating the schema:

            ```python
            job = registry.make_action("summarise")
            job.run()
            ```
        """

    def make_action_message(
        self, action_name: str, action_id: str = ""
    ) -> ActionMessage:
        """
        Create a wire action message for a registered action name.
        """

    def register(
        self,
        action_name: str,
        schema: ActionSchema,
        handler: ActionHandler | NativeActionHandler | None = None,
    ) -> None:
        """
        Register an action with a schema and optional async handler.

        Examples:
            Publish an application handler under its schema name:

            ```python
            registry.register("summarise", SUMMARISE, summarise)
            ```
        """

    def register_sync(
        self,
        action_name: str,
        schema: ActionSchema,
        handler: ActionHandler | NativeActionHandler | None,
    ) -> None:
        """
        Register an action with a schema and a synchronous handler.
        """

    def unregister(self, action_name: str) -> None:
        """
        Remove the action with the given name from the registry.
        """

class ActionSchema:
    """
    Schema describing an action's ports, headers and output mappings.
    """

    WHOLE_JSON: typing.ClassVar[str] = "$"
    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(cls, value: str | bytes, **_: typing.Any): ...
    def __eq__(self, arg0: object) -> bool:
        """
        Return True when two action schemas are equal.
        """

    def __init__(
        self,
        name: str,
        description: str = "",
        inputs: typing.Any = {},
        outputs: typing.Any = {},
        headers: collections.abc.Mapping[str, bytes] | None = {},
        output_to_json_field: collections.abc.Mapping[str, str] = {},
    ) -> None:
        """
        Create a validated action schema.
        """

    def map_output_to_json(
        self, output_name: str, field_name: str = ""
    ) -> None:
        """
        Map an output port to a JSON field in the action's response.
        """

    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, *, mode: str = "python", **_: typing.Any): ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def validate(self) -> None:
        """
        Validate the action schema, raising on error.
        """

    @property
    def description(self) -> str:
        """
        Human-readable description of the action.
        """
    @description.setter
    def description(self, arg0: str) -> None: ...

    @property
    def headers(self) -> _ActionHeaderSchemaMapView:
        """
        Mapping of header names to their header schemas.
        """
    @headers.setter
    def headers(self, arg1: typing.Any) -> None: ...

    @property
    def inputs(self) -> _ActionPortSchemaMapView:
        """
        Mapping of input port names to their port schemas.
        """
    @inputs.setter
    def inputs(self, arg1: typing.Any) -> None: ...

    @property
    def name(self) -> str:
        """
        The action's name.
        """
    @name.setter
    def name(self, arg0: str) -> None: ...

    @property
    def output_to_json_field(self) -> _StringSchemaMapView:
        """
        Mapping of output port names to JSON field names.
        """
    @output_to_json_field.setter
    def output_to_json_field(
        self, arg1: collections.abc.Mapping[str, str]
    ) -> None: ...

    @property
    def outputs(self) -> _ActionPortSchemaMapView:
        """
        Mapping of output port names to their port schemas.
        """
    @outputs.setter
    def outputs(self, arg1: typing.Any) -> None: ...

class ActionSettings:
    """
    Runtime settings controlling an action's stream binding and cleanup.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    bind_streams_on_inputs_by_default: typing.Any
    bind_streams_on_outputs_by_default: typing.Any
    clear_inputs_after_run: typing.Any
    clear_outputs_after_run: typing.Any
    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(cls, value: str | bytes, **_: typing.Any): ...
    def __eq__(self, arg0: object) -> bool:
        """
        Return True when two settings objects are equal.
        """

    def __init__(
        self,
        bind_streams_on_inputs_by_default: bool | None = None,
        bind_streams_on_outputs_by_default: bool | None = None,
        clear_inputs_after_run: bool = False,
        clear_outputs_after_run: bool = False,
    ) -> None:
        """
        Create action settings.
        """

    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, *, mode: str = "python", **_: typing.Any): ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...

class AsyncNode:
    ITER_BATCH: typing.ClassVar[int] = 64
    @staticmethod
    def _validate_expected_types(
        mimetype_patterns: str | typing.Sequence[str], obj_type: type | None
    ) -> str | tuple[str, ...]: ...
    def expect_types(self, **kwds) -> typing.Iterator[AsyncNode]:
        """
        Temporarily set the expected read types for the ``with`` block.
        """

    @classmethod
    def create(
        cls: type[AsyncNode],
        node_id: str,
        node_map: NodeMap | None = None,
        *,
        serialization_registry: SerializationRegistry | None = None,
        reader_options: (
            ChunkStoreReaderOptions | dict[str, typing.Any] | None
        ) = None,
        writer_options: (
            ChunkStoreWriterOptions | dict[str, typing.Any] | None
        ) = None,
        chunk_store_factory: typing.Callable[
            [str], ChunkStore
        ] = a11.stores.local_chunk_store.LocalChunkStore,
    ) -> AsyncNode:
        """
        Create a standalone node identified by ``node_id``.

        ``chunk_store_factory`` builds the backing store from the id; it
        defaults to an in-memory
        [LocalChunkStore][a11.stores.local_chunk_store.LocalChunkStore],
        so overriding it is how you place a node's data in a different backend.

        Examples:
            Create a stream used to deliver answer fragments:

            ```python
            answer = AsyncNode.create("answer-tokens")
            ```
        """

    async def __aenter__(self) -> AsyncNode: ...
    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: typing.Any,
    ) -> None: ...
    def __aiter__(self) -> AsyncNode: ...
    async def __anext__(
        self, timeout: Duration | None = None
    ) -> typing.Any: ...
    def __init__(
        self,
        chunk_store: ChunkStore,
        node_map: NodeMap | None = None,
        *,
        serialization_registry: SerializationRegistry | None = None,
        reader_options: (
            ChunkStoreReaderOptions | dict[str, typing.Any] | None
        ) = None,
        writer_options: (
            ChunkStoreWriterOptions | dict[str, typing.Any] | None
        ) = None,
    ) -> None:
        """
        Build a node over ``chunk_store``.

        Prefer `create`, which constructs the store for you from a node
        id. Pass ``reader_options`` / ``writer_options`` (as
        `ChunkStoreReaderOptions` / `ChunkStoreWriterOptions` or plain dicts) to
        tune buffering and ordering, and a custom ``serialization_registry`` to
        control how Python objects map to chunks.
        """

    def abort_with_status(self, status: Status) -> asyncio.Future[None]:
        """
        Aborts the stream with the given error status and returns a future that resolves once the abort has propagated. Consumers then observe the error rather than a normal end-of-stream.
        """

    def attach_stream(self, stream: WireStream) -> None:
        """
        Attaches a wire stream so this node's chunks are mirrored over the network transport. The stream is kept alive for the node's lifetime.
        """

    def cancel(self) -> None:
        """
        Cancels both the reader and the writer, tearing down all pending streaming operations on the node at once.
        """

    def cancel_reader(self) -> None:
        """
        Cancels the node's reader, unblocking any pending next-chunk or next-fragment awaits on the consuming side of the stream.
        """

    def cancel_writer(self) -> None:
        """
        Cancels the node's writer, unblocking any pending put or drain awaits on the producing side of the stream.
        """

    @typing.overload
    async def consume(
        self,
        obj_type: type[T] | None = None,
        timeout: Duration | None = None,
        mimetype_patterns: str | typing.Sequence[str] = "",
        allow_none: typing.Literal[False] = False,
    ) -> T:
        """
        Consume exactly one whole value and return it deserialized.

        For a node that carries a single result, such as a unary action
        output. Pass ``obj_type`` to deserialize to a specific type, or request
        ``NodeFragment``/``Chunk`` to get the raw form.

        Examples:
            Read the unary customer input of an action handler:

            ```python
            customer = await action["customer"].consume(obj_type=Customer)
            ```
        """
    @typing.overload
    async def consume(
        self,
        obj_type: type[T] | None = None,
        timeout: Duration | None = None,
        mimetype_patterns: str | typing.Sequence[str] = "",
        allow_none: typing.Literal[True] = True,
    ) -> T | None: ...
    @typing.overload
    async def consume(
        self,
        obj_type: type[T] | None = None,
        timeout: Duration | None = None,
        mimetype_patterns: str | typing.Sequence[str] = "",
        allow_none: bool = False,
    ) -> T | None: ...

    @typing.overload
    async def consume_chunk(
        self,
        timeout: Duration | None = None,
        allow_none: typing.Literal[False] = False,
    ) -> Chunk:
        """
        Consume exactly one whole value and return its raw chunk.
        """
    @typing.overload
    async def consume_chunk(
        self,
        timeout: Duration | None = None,
        allow_none: typing.Literal[True] = True,
    ) -> Chunk | None: ...
    @typing.overload
    async def consume_chunk(
        self, timeout: Duration | None = None, allow_none: bool = False
    ) -> Chunk | None: ...

    @typing.overload
    async def consume_fragment(
        self,
        timeout: Duration | None = None,
        allow_none: typing.Literal[False] = False,
    ) -> NodeFragment:
        """
        Read exactly one whole value's fragment, enforcing the terminator.

        Unlike `next_fragment`, this expects the node to hold exactly one
        value, and raises if that shape is violated. Two spellings are
        accepted: the value written as final, or the value followed by a null
        final chunk. With ``allow_none`` a node that holds no value — closed
        empty, or holding nothing but a null final — yields ``None`` instead of
        raising. Requires an ordered reader.
        """
    @typing.overload
    async def consume_fragment(
        self,
        timeout: Duration | None = None,
        allow_none: typing.Literal[True] = True,
    ) -> NodeFragment | None: ...
    @typing.overload
    async def consume_fragment(
        self, timeout: Duration | None = None, allow_none: bool = False
    ) -> NodeFragment | None: ...

    def detach_stream(self, stream: WireStream) -> None:
        """
        Detaches a previously attached wire stream so the node stops mirroring its chunks over that transport.
        """

    def drain_and_close(self) -> asyncio.Future[None]:
        """
        Returns a future that resolves once all buffered chunks have been flushed and the writer is closed. This does not mark a chunk as final: call put_final() or put_null_final() first when readers must synchronise on the logical end of the stream.
        """

    def get_chunk_store(self) -> ChunkStore:
        """
        Returns the underlying chunk store backing this node. The chunk store is the ordered storage boundary that the node's reader and writer stream through; reach for it when you need lower-level access than the async put/next API provides.
        """

    def get_id(self) -> str:
        """
        Returns the node's stable identifier. Raises if the id cannot be resolved.
        """

    def get_reader_options(self) -> ChunkStoreReaderOptions:
        """
        Return a copy of the reader's current options.
        """

    def get_reader_status(self) -> Status:
        """
        Returns the current status of the node's reader. Check it to tell whether the consuming end of the stream is healthy, has completed, or has failed while streaming.
        """

    def get_writer_abort_status(self) -> Status | None:
        """
        Returns the status the writer was aborted with, or None if the writer has not been aborted.
        """

    def get_writer_options(self) -> ChunkStoreWriterOptions:
        """
        Return a copy of the writer's current options.
        """

    def get_writer_status(self) -> Status:
        """
        Returns the current status of the node's writer. Check it to tell whether the producing end of the stream is healthy, has completed, or has failed while streaming.
        """

    def is_writable(self) -> asyncio.Future[bool]:
        """
        Returns a future that resolves once it is known whether the node can currently accept writes. Await it before producing chunks to respect backpressure rather than blocking a busy stream.
        """

    def iter_chunks(
        self, timeout: Duration | None = None
    ) -> collections.abc.AsyncIterator[Chunk]:
        """
        Async-iterate raw chunks until the stream ends.
        """

    def iter_fragments(
        self, timeout: Duration | None = None
    ) -> collections.abc.AsyncIterator[NodeFragment]:
        """
        Async-iterate raw fragments until the stream ends.
        """

    def iter_with_deadline(self, deadline: Time):
        """
        Async-iterate deserialized values until ``deadline`` or end of
        stream.
        """

    async def next(
        self,
        obj_type: type[T] | None = None,
        timeout: Duration | None = None,
        mimetype_patterns: str | typing.Sequence[str] = "",
    ) -> T | None:
        """
        Alias for `next_object`: the next deserialized value or ``None``.

        Examples:
            Process a live audit stream one event at a time:

            ```python
            events.set_expected_types("application/json", AuditEvent)
            while (event := await events.next()) is not None:
                await audit_index.store(event)
            ```
        """

    async def next_chunk(self, timeout: Duration | None = None) -> Chunk | None:
        """
        Read the next raw chunk, or ``None`` at end of stream.
        """

    async def next_fragment(
        self, timeout: Duration | None = None
    ) -> NodeFragment | None:
        """
        Read the next raw fragment, or ``None`` at end of stream.
        """

    def next_fragments(
        self, limit: typing.SupportsInt, timeout: Duration | None = None
    ) -> asyncio.Future[list[NodeFragment | None]]:
        """
        Returns a future resolving to a list of up to `limit` fragments, with a trailing None at end-of-stream. The batched counterpart to next_fragment, and the one to prefer when draining: every await costs an event-loop turn, so reading a hundred values one await at a time is a hundred turns. It returns whatever is already buffered and waits only when nothing is, so a live stream still yields each value as soon as it arrives.
        """

    async def next_object(
        self,
        obj_type: type[T] | None = None,
        timeout: Duration | None = None,
        mimetype_patterns: str | typing.Sequence[str] = "",
    ) -> T | None:
        """
        Read and deserialize the next value, or ``None`` at end of stream.
        """

    async def put(
        self,
        value: typing.Any,
        seq: int | None = None,
        final: bool = False,
        mimetype: str = "",
    ) -> asyncio.Future[int]:
        """
        Write ``value`` and return its store-confirmation future.

        ``value`` may be a [NodeFragment][a11.data.types.NodeFragment], a
        [Chunk][a11.data.types.Chunk], or any Python object the node's
        serialization registry can encode (``mimetype`` selects the encoding).
        Set ``final=True`` on the last data fragment so readers know where the
        logical value ends. Finality does not close the writer: call
        `drain_and_close` after the confirmation future resolves. The returned
        `asyncio.Future` resolves to the stored sequence number after the
        backing store accepts the fragment. Attached WireStream sends are
        attempted or queued by the writer but are not separately acknowledged.

        Examples:
            Add an intermediate token while a model response is produced:

            ```python
            await answer.put("The shipment ")
            ```
        """

    async def put_chunk(
        self, chunk: Chunk, seq: int | None = None, final: bool = False
    ) -> asyncio.Future[int]:
        """
        Admit a native chunk and return its store-confirmation future.

        Await this coroutine to respect the writer's bounded admission buffer,
        then await the returned future when the backing store must have
        accepted the fragment. Attached stream sends are attempted or queued
        as the writer processes the batch, but do not add a second delivery
        confirmation.

        Awaiting the confirmation flushes the writer on this thread, so a
        store that accepts the write without waiting resolves it with no
        event-loop turn at all. A producer that awaits only admission does not
        flush: its write goes out on the writer's own pump, which is the point
        of the two being separate.
        """

    async def put_final(
        self, value: typing.Any, seq: int | None = None, mimetype: str = ""
    ) -> asyncio.Future[int]:
        """
        Write ``value`` as the logical final element.

        This marks the final sequence but leaves the writer open. The returned
        confirmation can be awaited when immediate store acceptance matters;
        otherwise a later `drain_and_close` flushes queued work.

        Examples:
            Mark the last visible fragment and close the producer:

            ```python
            await answer.put_final("arrives Friday.")
            ```
        """

    async def put_fragment(self, fragment: NodeFragment) -> asyncio.Future[int]:
        """
        Enqueue a [NodeFragment][a11.data.types.NodeFragment] (carrying its
        seq/final).
        """

    async def put_null_final(
        self, seq: int | None = None
    ) -> asyncio.Future[int]:
        """
        Write an explicit null fragment as the logical terminator.

        Use this after a non-final value when `consume` should treat that value
        as one complete unary result. It does not close the writer; finish with
        `drain_and_close` after the confirmation resolves.
        """

    def reset_reader(
        self,
        options: ChunkStoreReaderOptions | dict[str, typing.Any] | None = None,
    ) -> AsyncNode:
        """
        Rewind/reconfigure the reader (e.g. to re-read from an offset).
        """

    def set_expected_types(
        self,
        mimetype_patterns: str | typing.Sequence[str],
        obj_type: type | None,
    ) -> AsyncNode:
        """
        Set the default MIME patterns and object type for reads.

        Once set, ``next()``/``consume()`` and ``async for`` deserialize to
        ``obj_type`` (matching ``mimetype_patterns``) without repeating those
        arguments on every call. Returns ``self`` for chaining.
        """

    def set_reader_options(
        self, options: ChunkStoreReaderOptions | dict[str, typing.Any]
    ) -> AsyncNode:
        """
        Replace the reader options and return ``self`` for chaining.
        """

    def set_serialization_registry(
        self, registry: SerializationRegistry
    ) -> AsyncNode:
        """
        Set the serialization registry and return ``self`` for chaining.
        """

    def set_writer_options(
        self, options: ChunkStoreWriterOptions | dict[str, typing.Any]
    ) -> AsyncNode:
        """
        Replace the writer options and return ``self`` for chaining.
        """

    def wait_for_buffer_to_drain(self) -> asyncio.Future[None]:
        """
        Returns a future that resolves once the write buffer has drained. Await it to apply backpressure from a fast producer, letting consumers catch up before you push more chunks.
        """

    @property
    def chunk_store(self) -> ChunkStore:
        """
        The underlying chunk store backing this node (see get_chunk_store).
        """

    @property
    def id(self) -> str:
        """
        The node's stable identifier (see get_id).
        """

    @property
    def reader(self) -> ChunkStoreReader:
        """
        The node's
        [ChunkStoreReader][a11.stores.chunk_store_reader.ChunkStoreReader].
        """

    @property
    def reader_options(self) -> ChunkStoreReaderOptions:
        """
        Options controlling how this node reads from its chunk store, such as buffering and flow control.
        """
    @reader_options.setter
    def reader_options(self, arg1: ChunkStoreReaderOptions) -> None: ...

    @property
    def serialization_registry(self) -> SerializationRegistry:
        """
        The registry used to (de)serialize Python objects for this node.
        """
    @serialization_registry.setter
    def serialization_registry(
        self, registry: SerializationRegistry
    ) -> None: ...

    @property
    def writer(self) -> ChunkStoreWriter:
        """
        The node's
        [ChunkStoreWriter][a11.stores.chunk_store_writer.ChunkStoreWriter].
        """

    @property
    def writer_options(self) -> ChunkStoreWriterOptions:
        """
        Options controlling how this node writes to its chunk store, such as buffering and flow control.
        """
    @writer_options.setter
    def writer_options(self, arg1: ChunkStoreWriterOptions) -> None: ...

class AudioBuffer:
    """
    A captured block of samples stored channel-major (planar). Use `memoryview(buffer)` for a zero-copy (channels x frames) float view, or `channel(i)` for one channel.
    """

    def __buffer__(self, flags):
        """
        Return a buffer object that exposes the underlying memory of the object.
        """

    def __init__(
        self,
        data: collections.abc.Buffer,
        sample_rate: typing.SupportsFloat,
        num_channels: typing.SupportsInt = 1,
    ) -> None:
        """
        Build an AudioBuffer from a buffer-protocol object (bytes, a NumPy array, a CPU PyTorch tensor, ...). Samples are read as channel-major float32 (raw bytes are reinterpreted as float32); a 2-D buffer's first dimension is the channel count.
        """

    def __release_buffer__(self, buffer):
        """
        Release the buffer object that exposes the underlying memory of the object.
        """

    def channel(self, index: int) -> memoryview:
        """
        Return a zero-copy read-only view of one channel's samples.
        """

    @property
    def end_time(self) -> Time:
        """
        Best-effort instant the final sample in this buffer was taken.
        """

    @property
    def num_channels(self) -> int:
        """
        Number of channels in this buffer.
        """

    @property
    def num_frames(self) -> int:
        """
        Number of samples per channel in this buffer.
        """

    @property
    def sample_rate(self) -> float:
        """
        Sample rate, in hertz, the samples were captured at.
        """

    @property
    def samples(self) -> memoryview:
        """
        A zero-copy read-only ``(channels, frames)`` float view.
        """

class AudioCaptureEvent:
    """
    A capture lifecycle or dropped-buffer notification from capture_audio.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def buffers_dropped(count: typing.SupportsInt) -> AudioCaptureEvent: ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    @staticmethod
    def started() -> AudioCaptureEvent: ...
    @staticmethod
    def stopped() -> AudioCaptureEvent: ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self, kind: str = "started", dropped: typing.SupportsInt = 0
    ) -> None:
        """
        Construct a capture event.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...

    @property
    def dropped(self) -> int:
        """
        Buffers dropped since the previous event.
        """
    @dropped.setter
    def dropped(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def kind(self) -> str:
        """
        The event kind: 'started', 'buffers_dropped' or 'stopped'.
        """
    @kind.setter
    def kind(self, arg1: str) -> None: ...

class AudioControlEvent:
    """
    A command on an Action's control_events input; 'stop' finishes capture gracefully.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    @staticmethod
    def stop() -> AudioControlEvent:
        """
        A stop command that finishes capture gracefully.
        """

    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(self, command: str = "stop") -> None:
        """
        Construct a control event.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...

    @property
    def command(self) -> str:
        """
        The command name, e.g. 'stop'.
        """
    @command.setter
    def command(self, arg1: str) -> None: ...

class AudioDeviceInfo:
    """
    Static metadata describing one host audio device.
    """

    def __repr__(self) -> str: ...
    @property
    def default_high_input_latency(self) -> Duration:
        """
        Suggested latency for robust, buffered input use.
        """

    @property
    def default_low_input_latency(self) -> Duration:
        """
        Suggested latency for interactive input use.
        """

    @property
    def default_sample_rate(self) -> float:
        """
        Default sample rate in hertz.
        """

    @property
    def host_api(self) -> str:
        """
        Host API backing the device (e.g. Core Audio, ALSA).
        """

    @property
    def index(self) -> int:
        """
        PortAudio device index, stable within a process run.
        """

    @property
    def is_default_input(self) -> bool:
        """
        Whether this is the host default input device.
        """

    @property
    def is_default_output(self) -> bool:
        """
        Whether this is the host default output device.
        """

    @property
    def max_input_channels(self) -> int:
        """
        Maximum capture channels the device offers.
        """

    @property
    def max_output_channels(self) -> int:
        """
        Maximum playback channels the device offers.
        """

    @property
    def name(self) -> str:
        """
        Human-readable name.
        """

class AudioInput:
    """
    A capturable input device that samples continuously while at least one subscription is alive.
    """

    @staticmethod
    def open(
        options: (
            AudioInputOptions | collections.abc.Mapping[str, typing.Any] | None
        ) = None,
    ) -> AudioInput:
        """
        Resolve the device and validate options without starting capture.
        """

    def __init__(
        self,
        options: (
            AudioInputOptions | collections.abc.Mapping[str, typing.Any] | None
        ) = None,
    ) -> None:
        """
        Resolve the device and validate options without starting capture.

        A plain mapping is validated into the same bound options object used by
        C++, rather than creating a second Python configuration model.
        """

    def subscribe(self, buffer_size: int) -> AudioSubscription:
        """
        Begin receiving buffers of ``buffer_size`` frames per channel.

        Capture starts when the first subscription is created and stops when
        the last one is closed.
        """

    @property
    def capturing(self) -> bool:
        """
        Whether a capture stream is currently open.
        """

    @property
    def channels(self) -> int:
        """
        Number of channels every subscription receives.
        """

    @property
    def device(self) -> AudioDeviceInfo:
        """
        The captured device's metadata.
        """

    @property
    def device_index(self) -> int:
        """
        The captured device's index.
        """

    @property
    def name(self) -> str:
        """
        The captured device's name.
        """

    @property
    def sample_rate(self) -> float:
        """
        Sample rate, in hertz, capture runs at.
        """

class AudioInputOptions:
    """
    How an AudioInput opens its capture stream.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        device_index: typing.SupportsInt = -1,
        device_name: str = "",
        sample_rate: typing.SupportsFloat = 0.0,
        channels: typing.SupportsInt = 0,
        block_frames: typing.SupportsInt = 256,
        ring_blocks: typing.SupportsInt = 32,
        buffer_frames: typing.SupportsInt = 0,
    ) -> None:
        """
        Construct validated audio input options.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...

    @property
    def block_frames(self) -> int:
        """
        Frames per PortAudio callback block.
        """
    @block_frames.setter
    def block_frames(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def buffer_frames(self) -> int:
        """
        Frames per delivered subscription buffer, or 0 for the block size.
        """
    @buffer_frames.setter
    def buffer_frames(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def channels(self) -> int:
        """
        Requested channel count, or 0 for the device's count.
        """
    @channels.setter
    def channels(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def device_index(self) -> int:
        """
        Device index to capture from, or negative for default.
        """
    @device_index.setter
    def device_index(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def device_name(self) -> str:
        """
        Input device name to capture from; empty selects by index or the default input.
        """
    @device_name.setter
    def device_name(self, arg0: str) -> None: ...

    @property
    def ring_blocks(self) -> int:
        """
        Depth of the internal callback-to-fiber ring, in blocks.
        """
    @ring_blocks.setter
    def ring_blocks(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def sample_rate(self) -> float:
        """
        Requested sample rate in hertz, or 0 for the default.
        """
    @sample_rate.setter
    def sample_rate(self, arg0: typing.SupportsFloat) -> None: ...

class AudioModelSpec:
    def __repr__(self) -> str: ...
    @property
    def filename(self) -> str:
        """
        Cache filename, ggml-<name>.bin.
        """

    @property
    def name(self) -> str:
        """
        The shorthand it is known by.
        """

    @property
    def sha1(self) -> str:
        """
        Published SHA-1, as lowercase hex.
        """

    @property
    def size_mib(self) -> int:
        """
        Approximate size in MiB.
        """

    @property
    def url(self) -> str:
        """
        Where the artifact is fetched from.
        """

class AudioSubscription:
    """
    A live subscription delivering fixed-size buffers from an AudioInput.
    """

    async def __aenter__(self) -> typing.Self: ...
    async def __aexit__(self, exc_type, exc, traceback) -> None: ...
    def __aiter__(self) -> typing.Self: ...
    async def __anext__(self) -> AudioBuffer: ...
    def close(self) -> None:
        """
        Stop delivering; stops capture if this was the last subscription.
        """

    async def read(self) -> AudioBuffer:
        """
        Await the next captured buffer for this subscription.
        """

    @property
    def buffer_size(self) -> int:
        """
        Frames per channel in every delivered buffer.
        """

    @property
    def channels(self) -> int:
        """
        Number of channels in every delivered buffer.
        """

    @property
    def dropped(self) -> int:
        """
        Buffers dropped because this subscription fell behind.
        """

    @property
    def sample_rate(self) -> float:
        """
        Sample rate, in hertz, of every delivered buffer.
        """

class ChannelFramingOptions:
    def __init__(self) -> None:
        """
        Construct default channel framing options.
        """

    def validate(self) -> None:
        """
        Validate the framing options, raising on invalid configuration.
        """

    @property
    def max_pending_bytes(self) -> int:
        """
        Maximum total bytes of in-flight frames.
        """
    @max_pending_bytes.setter
    def max_pending_bytes(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def max_pending_messages(self) -> int:
        """
        Maximum number of in-flight (unacknowledged) frames.
        """
    @max_pending_messages.setter
    def max_pending_messages(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def split_size(self) -> int:
        """
        Maximum payload size before a message is split into multiple frames.
        """
    @split_size.setter
    def split_size(self, arg0: typing.SupportsInt) -> None: ...

class Chunk:
    """
    A unit of node data with optional metadata and ref.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @staticmethod
    def from_msgpack(data: typing.Any) -> Chunk:
        """
        Deserialize a value from MessagePack bytes.
        """

    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_construct(cls, **values: typing.Any):
        """
        Construct a native value from trusted field values.

        Native records retain C++ invariants. This validates input rather than
        creating an invalid object.
        """

    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(
        cls, value: str | bytes | bytearray, **_: typing.Any
    ): ...
    def __copy__(self) -> Chunk:
        """
        Return a shallow copy of the value.
        """

    def __deepcopy__(self, memo: dict) -> Chunk:
        """
        Return a deep copy of the value.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether two values are equal.
        """

    def __init__(
        self,
        metadata: typing.Any | None = None,
        ref: str = "",
        data: str | bytes | bytearray | memoryview = b"",
    ) -> None:
        """
        Create a chunk from optional metadata, a ref, and payload data.
        """

    def __repr__(self) -> str:
        """
        Return a human-readable debug string.
        """

    def debug_string(self) -> str:
        """
        Return a human-readable debug string.
        """

    def get_mimetype(self) -> str:
        """
        Return the chunk's MIME type, or empty if it has no metadata.
        """

    def is_empty(self) -> bool:
        """
        Return whether the chunk has no payload data.
        """

    def is_null(self) -> bool:
        """
        Return whether the chunk is null (no metadata and no data).
        """

    def model_copy(
        self, *, update: dict[str, typing.Any] | None = None, deep: bool = False
    ): ...
    def model_dump(
        self, *, mode: str = "python", **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def to_msgpack(self) -> bytes:
        """
        Serialize the value to MessagePack bytes.
        """

    def validate(self) -> None:
        """
        Raise if the value fails structural validation.
        """

    @property
    def approx_bytes(self) -> int:
        """
        Approximate in-memory size of the chunk in bytes.
        """

    @property
    def data(self) -> bytes:
        """
        Raw payload bytes of the chunk.
        """
    @data.setter
    def data(self, arg1: str | bytes | bytearray | memoryview) -> None: ...

    @property
    def metadata(self) -> ChunkMetadata | None:
        """
        Optional metadata describing the chunk.
        """
    @metadata.setter
    def metadata(self, arg1: typing.Any) -> None: ...

    @property
    def ref(self) -> str:
        """
        Reference identifying the chunk's stored payload.
        """
    @ref.setter
    def ref(self, arg0: str) -> None: ...

class ChunkMetadata:
    """
    Metadata describing a chunk of node data.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @staticmethod
    def from_msgpack(data: typing.Any) -> ChunkMetadata:
        """
        Deserialize a value from MessagePack bytes.
        """

    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_construct(cls, **values: typing.Any):
        """
        Construct a native value from trusted field values.

        Native records retain C++ invariants. This validates input rather than
        creating an invalid object.
        """

    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(
        cls, value: str | bytes | bytearray, **_: typing.Any
    ): ...
    def __copy__(self) -> ChunkMetadata:
        """
        Return a shallow copy of the value.
        """

    def __deepcopy__(self, memo: dict) -> ChunkMetadata:
        """
        Return a deep copy of the value.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether two values are equal.
        """

    def __init__(
        self,
        mimetype: str,
        timestamp: typing.Any | None = None,
        attributes: typing.Any = {},
    ) -> None:
        """
        Create chunk metadata from a MIME type, timestamp, and attributes.
        """

    def __repr__(self) -> str:
        """
        Return a human-readable debug string.
        """

    def debug_string(self) -> str:
        """
        Return a human-readable debug string.
        """

    def get_attribute(self, key: str) -> bytes:
        """
        Return the attribute bytes for a key, raising if it is absent.
        """

    def model_copy(
        self, *, update: dict[str, typing.Any] | None = None, deep: bool = False
    ): ...
    def model_dump(
        self, *, mode: str = "python", **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def set_attribute(self, key: str, bytes: typing.Any) -> None:
        """
        Set the attribute bytes for a key.
        """

    def to_msgpack(self) -> bytes:
        """
        Serialize the value to MessagePack bytes.
        """

    def validate(self) -> None:
        """
        Raise if the value fails structural validation.
        """

    @property
    def approx_bytes(self) -> int:
        """
        Approximate in-memory size of the metadata in bytes.
        """

    @property
    def attributes(self) -> _ByteMapView:
        """
        Byte-string attribute map attached to the chunk.
        """
    @attributes.setter
    def attributes(self, arg1: typing.Any) -> None: ...

    @property
    def mimetype(self) -> str:
        """
        MIME type describing the chunk payload.
        """
    @mimetype.setter
    def mimetype(self, arg0: str) -> None: ...

    @property
    def timestamp(self) -> Time | None:
        """
        Optional timestamp associated with the chunk.
        """
    @timestamp.setter
    def timestamp(self, arg1: typing.Any) -> None: ...

class ChunkStore:
    def __init__(self) -> None:
        """
        Construct the abstract base. Subclass this in Python to back an agent with a custom asynchronous chunk store; every data method returns an awaitable so callers never block the event loop.
        """

    def clear_data(
        self, seq: typing.SupportsInt
    ) -> asyncio.Future[NodeFragment]:
        """
        Erase the payload of the fragment at a sequence number while keeping its slot, and await the resulting fragment.
        """

    def close_writes_with_status(
        self, status: Status, return_status_if_already_closed: bool = False
    ) -> asyncio.Future[Status]:
        """
        Seal the store against further writes with a terminal status and await completion. Waiting readers are released. With `return_status_if_already_closed`, a repeated close returns the status recorded by the first.

        Examples:
            Publish clean producer completion to all readers:

            ```python
            await store.close_writes_with_status(Status.ok())
            ```
        """

    def get(
        self, seq: typing.SupportsInt, deadline: Time | None = None
    ) -> asyncio.Future[NodeFragment]:
        """
        Await the fragment stored at a sequence number. The future resolves when the fragment is available or the optional deadline elapses.

        Examples:
            Read back a fragment after retaining its assigned position:

            ```python
            fragment = await store.get(seq)
            ```
        """

    def get_by_arrival_order(
        self, arrival_order: typing.SupportsInt, deadline: Time | None = None
    ) -> asyncio.Future[NodeFragment]:
        """
        Await the fragment identified by the order in which it arrived rather than its sequence number. The future resolves when the fragment is present or the optional deadline passes.
        """

    def get_final_seq(self) -> asyncio.Future[int | None]:
        """
        Await the explicitly marked final sequence, or None if no fragment has declared finality. Finality is independent of write closure: closing the store does not create a final sequence.
        """

    def get_id(self) -> str:
        """
        Return the store's node identifier. Raises if a Python subclass does not override `get_id`.
        """

    def get_seq_for_arrival_order(
        self, arrival_order: typing.SupportsInt
    ) -> asyncio.Future[int]:
        """
        Await the sequence number that corresponds to a given arrival order.
        """

    def next(
        self, deadline: Time | None = None, limit: typing.SupportsInt = 1
    ) -> asyncio.Future[list[NodeFragment | None]]:
        """
        Await up to `limit` of the next available fragments as a stream. This is the primary way an agent consumes chunks as they are produced: the future resolves with whatever is ready before the optional deadline, and slots may be None when a fragment is missing. Loop over successive calls to follow a growing store.
        """

    def put(self, fragment: NodeFragment) -> asyncio.Future[int]:
        """
        Append a single fragment and await its assigned sequence number. The future resolves once the backing store accepts the write.

        Examples:
            Store a fragment and retain its assigned position:

            ```python
            seq = await store.put(fragment)
            ```
        """

    def put_many(
        self, fragments: collections.abc.Sequence[NodeFragment]
    ) -> asyncio.Future[list[int]]:
        """
        Append several fragments in one batch and await their assigned sequence numbers. Prefer this over repeated `put` calls when an agent emits many chunks at once, to reduce round-trips.
        """

    def size(self) -> asyncio.Future[int]:
        """
        Await the number of fragments currently in the store.
        """

class ChunkStoreReader:
    def __aiter__(self) -> ChunkStoreReader: ...
    async def __anext__(self) -> NodeFragment: ...
    def __init__(
        self,
        store: ChunkStore,
        options: ChunkStoreReaderOptions | dict[str, typing.Any] | None = None,
    ) -> None:
        """
        Open a reader over ``store``.

        ``options`` (a `ChunkStoreReaderOptions` or plain dict) tunes
        ordering, buffering, starting offset, sticky mimetypes, and whether
        chunks are popped as they are read.
        """

    def cancel(self) -> None:
        """
        Stop the background read pump. Pending `next` awaitables are resolved and no further chunks are fetched.
        """

    def ensure_started(self) -> None:
        """
        Start the background read pump if it is not already running. Reading normally starts it lazily; call this to begin buffering before the first `next`.
        """

    def get_status(self) -> Status:
        """
        Return the reader's current status. An agent can inspect this to distinguish a healthy stream from one that has failed or ended.
        """

    def next(self, timeout: Duration = ...):
        """
        Return an awaitable for the next fragment in this reader's view.

        It resolves to ``None`` after the configured range or final sequence is
        exhausted. ``timeout`` bounds this wait only; a timed-out read does not
        close the store or prevent a later call from continuing the stream.

        Raises:
            StatusException: If the timeout expires, the store closes with an
                error, or the reader encounters invalid stream state.

        Examples:
            Resume a replay after an application checkpoint:

            ```python
            reader = ChunkStoreReader(store, {"offset": checkpoint + 1})
            while fragment := await reader.next():
                await replay(fragment)
            ```
        """

    def wait(self) -> asyncio.Future[None]:
        """
        Await completion of the background read pump. The returned future resolves once the reader has drained the store or been cancelled.
        """

    @property
    def buffer_size(self) -> int:
        """
        Number of prefetched fragments currently held in the reader's buffer.
        """

    @property
    def options(self) -> ChunkStoreReaderOptions:
        """
        The ChunkStoreReaderOptions this reader was created with.
        """

    @property
    def store(self) -> ChunkStore:
        """
        The ChunkStore this reader draws fragments from.
        """

class ChunkStoreReaderOptions:
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        ordered: bool = True,
        pop_chunks: bool = False,
        num_chunks_to_buffer: typing.SupportsInt | None = 32,
        offset: typing.SupportsInt | None = 0,
        max_chunks_to_read: typing.SupportsInt | None = None,
        sticky_mimetype: bool = False,
    ) -> None:
        """
        Construct validated options for a ChunkStoreReader.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...
    def validate(self) -> None:
        """
        Raise if the options are not internally consistent.
        """

    @property
    def max_chunks_to_read(self) -> int | None:
        """
        Optional cap on the total number of chunks to read.
        """
    @max_chunks_to_read.setter
    def max_chunks_to_read(self, arg0: typing.SupportsInt | None) -> None: ...

    @property
    def num_chunks_to_buffer(self) -> int:
        """
        Maximum number of chunks to prefetch into the buffer.
        """
    @num_chunks_to_buffer.setter
    def num_chunks_to_buffer(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def offset(self) -> int:
        """
        Sequence number at which reading begins.
        """
    @offset.setter
    def offset(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def ordered(self) -> bool:
        """
        Whether chunks are delivered strictly in sequence order.
        """
    @ordered.setter
    def ordered(self, arg0: bool) -> None: ...

    @property
    def pop_chunks(self) -> bool:
        """
        Whether chunks are removed from the store as they are read.
        """
    @pop_chunks.setter
    def pop_chunks(self, arg0: bool) -> None: ...

    @property
    def sticky_mimetype(self) -> bool:
        """
        Whether ordered chunks inherit the last explicitly set mimetype.
        """
    @sticky_mimetype.setter
    def sticky_mimetype(self, arg0: bool) -> None: ...

class ChunkStoreWriter:
    def __init__(
        self,
        chunk_store: ChunkStore,
        options: ChunkStoreWriterOptions | dict[str, typing.Any] | None = None,
    ) -> None:
        """
        Open a writer over ``chunk_store``.

        ``options`` (a `ChunkStoreWriterOptions` or plain dict) tunes the
        starting offset, sticky mimetypes, and how much is buffered/flushed at
        once.
        """

    def abort_with_status(self, status: Status) -> asyncio.Future[None]:
        """
        Abort the writer with an error status and await teardown. Readers then observe the error rather than a clean end-of-stream.
        """

    def attach_stream(self, stream: WireStream) -> None:
        """
        Tee stored fragments to an additional wire stream. After the store accepts a batch, the writer calls send on attached streams; a successful send confirms local transport admission, not peer delivery. A transport failure stops later writes but cannot revoke the current batch's store confirmations. The writer keeps the stream alive while attached.
        """

    def cancel(self) -> asyncio.Future[None]:
        """
        Stop the writer immediately and await teardown, discarding any chunks still queued.
        """

    def detach_stream(self, stream: WireStream) -> None:
        """
        Stop mirroring fragments to a previously attached wire stream. Raises if the stream was not attached.
        """

    def drain_and_close(self) -> asyncio.Future[None]:
        """
        Flush every queued chunk, close the writer, and await completion. This does not append a final fragment: mark the last chunk final before draining when readers need a final sequence number.
        """

    def enqueue_chunk(
        self,
        chunk: Chunk,
        seq: typing.SupportsInt | None = None,
        final: bool = False,
    ) -> tuple:
        """
        Enqueue a chunk and get back a (confirmation, admission) pair of awaitables. Unlike `put_chunk`, this exposes backpressure explicitly: `admission` resolves when the chunk is accepted into the bounded queue (None if it fit immediately) and `confirmation` resolves with the sequence assigned by the backing store. An agent awaits admission to pace production and confirmation to know the store accepted the write.
        """

    def ensure_started(self) -> None:
        """
        Start the background flush loop if it is not already running. Writing normally starts it lazily; call this to begin flushing before the first chunk is enqueued.
        """

    def flush(self) -> None:
        """
        Run the flush loop now, on this thread, if it is idle. A store that accepts the batch without waiting -- an in-memory one does -- has confirmed those writes by the time this returns, so their confirmations resolve without an event-loop turn. A store that cannot leaves the batch in flight and this does nothing, so chunks enqueued behind it still go out together. Awaiting a confirmation calls this for you; call it directly only to push a queue out without awaiting anything.
        """

    def get_abort_status(self) -> Status | None:
        """
        Return the status the writer was aborted with, or None if it was not aborted. Use this to distinguish a clean close from an error-driven abort.
        """

    def get_status(self) -> Status | None:
        """
        Return the writer's terminal status, or None while it is still open. An agent can poll this to detect that the stream has closed or failed.
        """

    def is_writable(self) -> bool:
        """
        Return whether the writer still accepts chunks. False once the stream has been drained, closed, or aborted.
        """

    async def put(
        self, obj: typing.Any, seq: int | None = None, final: bool = False
    ) -> asyncio.Future[int]:
        """
        Write a chunk and return its store-confirmation future.

        The writer operates at the chunk level; pass an already-serialized
        [Chunk][a11.data.types.Chunk] (use
        [AsyncNode][a11.nodes.async_node.AsyncNode]
        to write arbitrary Python objects). Returns a `asyncio.Future`
        resolving to the stored sequence number.
        """

    async def put_chunk(
        self, chunk: Chunk, seq: int | None = None, final: bool = False
    ) -> asyncio.Future[int]:
        """
        Enqueue a native chunk and return its store-confirmation future.

        Set ``final=True`` on the last chunk when readers must know the logical
        end of the sequence. Calling `drain_and_close` later only flushes and
        closes the writer; it does not add that final marker for you.

        Examples:
            Checkpoint only after the store accepts the final event:

            ```python
            confirmation = await writer.put_chunk(
                a11.to_chunk(event), final=True
            )
            stored_seq = await confirmation
            await checkpoints.save(stored_seq)
            ```

        Awaiting the confirmation flushes the writer on this thread, so a
        store that accepts the write without waiting resolves it without an
        event-loop turn. Awaiting only this coroutine does not flush: the
        write goes out on the writer's own pump, which is what lets a producer
        run ahead of a slow store.
        """

    def wait_for_buffer_to_drain(self) -> asyncio.Future[None]:
        """
        Await until the in-flight write buffer empties. An agent can use this as a backpressure checkpoint before enqueuing more chunks.
        """

    @property
    def options(self) -> ChunkStoreWriterOptions:
        """
        The ChunkStoreWriterOptions this writer was created with.
        """

    @property
    def queue_size(self) -> int:
        """
        Number of chunks currently waiting in the writer's flush queue.
        """

    @property
    def store(self) -> ChunkStore:
        """
        The ChunkStore this writer persists chunks to.
        """

class ChunkStoreWriterOptions:
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        offset: typing.SupportsInt | None = 0,
        max_chunks_to_write_at_once: typing.SupportsInt | None = 8,
        num_chunks_to_buffer: typing.SupportsInt | None = None,
        sticky_mimetype: bool = False,
    ) -> None:
        """
        Construct validated options for a ChunkStoreWriter.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...
    def validate(self) -> None:
        """
        Raise if the options are not internally consistent.
        """

    @property
    def max_chunks_to_write_at_once(self) -> int:
        """
        Maximum number of chunks flushed to the store per batch.
        """
    @max_chunks_to_write_at_once.setter
    def max_chunks_to_write_at_once(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def num_chunks_to_buffer(self) -> int | None:
        """
        Optional bound on the in-flight write buffer size.
        """
    @num_chunks_to_buffer.setter
    def num_chunks_to_buffer(self, arg0: typing.SupportsInt | None) -> None: ...

    @property
    def offset(self) -> int:
        """
        Sequence number at which writing begins.
        """
    @offset.setter
    def offset(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def sticky_mimetype(self) -> bool:
        """
        Whether repeated contiguous chunk mimetypes are omitted.
        """
    @sticky_mimetype.setter
    def sticky_mimetype(self, arg0: bool) -> None: ...

class DownloadOptions:
    def __init__(self) -> None:
        """
        Construct default download options.
        """

    @property
    def destination(self) -> str:
        """
        Final path; parent directories are created.
        """
    @destination.setter
    def destination(self, arg1: str) -> None: ...

    @property
    def expected_sha1(self) -> str:
        """
        Expected SHA-1 as hex, or empty to skip verification.
        """
    @expected_sha1.setter
    def expected_sha1(self, arg0: str) -> None: ...

    @property
    def fetch(self) -> FetchOptions:
        """
        Request settings.
        """
    @fetch.setter
    def fetch(self, arg0: FetchOptions) -> None: ...

    @property
    def on_progress(
        self,
    ) -> (
        collections.abc.Callable[[typing.SupportsInt, typing.SupportsInt], None]
        | None
    ):
        """
        Callable taking (bytes_done, bytes_total); write-only.
        """
    @on_progress.setter
    def on_progress(
        self,
        arg1: (
            collections.abc.Callable[
                [typing.SupportsInt, typing.SupportsInt], None
            ]
            | None
        ),
    ) -> None: ...

class Duration:
    @staticmethod
    def _negative_infinity() -> Duration:
        """
        Returns the negative-infinite duration.
        """

    @staticmethod
    def _positive_infinity() -> Duration:
        """
        Returns the positive-infinite duration.
        """

    @staticmethod
    def microseconds(value: typing.SupportsFloat | None) -> Duration:
        """
        Creates a duration from microseconds; None or negative means infinite.
        """

    @staticmethod
    def milliseconds(value: typing.SupportsFloat | None) -> Duration:
        """
        Creates a duration from milliseconds; None or negative means infinite.
        """

    @staticmethod
    def nanoseconds(value: typing.SupportsInt | None) -> Duration:
        """
        Creates a duration from nanoseconds; None or negative means infinite.
        """

    @staticmethod
    def seconds(value: typing.SupportsFloat | None) -> Duration:
        """
        Creates a duration from seconds; None or negative means infinite.
        """

    def __add__(self, right: Duration) -> Duration:
        """
        Returns the sum of two durations.
        """

    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, right: object) -> bool:
        """
        Returns whether two durations are equal.
        """

    def __hash__(self) -> int:
        """
        Returns a hash of the duration.
        """

    def __init__(self, nanoseconds: typing.SupportsInt) -> None:
        """
        Creates a duration from a whole number of nanoseconds.
        """

    def __le__(self, right: Duration) -> bool:
        """
        Returns whether this duration is less than or equal to another.
        """

    def __lt__(self, right: Duration) -> bool:
        """
        Returns whether this duration is less than another.
        """

    def __neg__(self) -> Duration:
        """
        Returns the negated duration.
        """

    def __repr__(self) -> str:
        """
        Returns a debug representation of the duration.
        """

    def __sub__(self, right: Duration) -> Duration:
        """
        Returns the difference of two durations.
        """

    def float_seconds(
        self, infinity_value: typing.SupportsFloat | None = None
    ) -> float | None:
        """
        Returns the duration in seconds as a float; infinity_value is returned for a positive-infinite duration.
        """

    def is_infinite(self) -> bool:
        """
        Returns whether the duration is positive or negative infinite.
        """

    @property
    def nanoseconds_value(self) -> int:
        """
        The duration as a whole number of nanoseconds.
        """

class FetchOptions:
    def __init__(self) -> None:
        """
        Construct default fetch options.
        """

    def validate(self) -> None:
        """
        Validate the options, raising on error.
        """

    @property
    def body(self) -> bytes:
        """
        Request body, for methods that take one.
        """
    @body.setter
    def body(self, arg1: typing.Any) -> None: ...

    @property
    def default_user_agent(self) -> bool:
        """
        Send a default user-agent when headers omit one.
        """
    @default_user_agent.setter
    def default_user_agent(self, arg0: bool) -> None: ...

    @property
    def headers(self) -> list[tuple[str, str]]:
        """
        Extra request headers as a list of (name, value) pairs.
        """
    @headers.setter
    def headers(
        self, arg1: collections.abc.Iterable[tuple[str, str]] | None
    ) -> None: ...

    @property
    def max_redirects(self) -> int:
        """
        Redirects to follow; 0 returns the 3xx response itself.
        """
    @max_redirects.setter
    def max_redirects(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def method(self) -> str:
        """
        Request method.
        """
    @method.setter
    def method(self, arg0: str) -> None: ...

    @property
    def timeout(self) -> Duration:
        """
        Wall-clock bound on the whole operation, redirects included.
        """
    @timeout.setter
    def timeout(self, arg1: Duration | None) -> None: ...

    @property
    def transport(self) -> Http2Options:
        """
        Transport settings; tls.enabled follows the URL scheme.
        """
    @transport.setter
    def transport(self, arg0: Http2Options) -> None: ...

class Http2Client:
    @staticmethod
    def connect(
        host: str, port: typing.SupportsInt, options: Http2Options = ...
    ) -> asyncio.Future[Http2Client]:
        """
        Asynchronously connect to an HTTP/2 server, returning a future that resolves to the connected client.
        """

    async def __aenter__(self) -> "Http2Client": ...
    async def __aexit__(self, exc_type, exc, traceback) -> None: ...
    def close(self) -> None:
        """
        Close the client connection.
        """

    def extended_connect(
        self,
        protocol: str,
        path: str,
        headers: collections.abc.Iterable[tuple[str, str]] | None = None,
        scheme: str = "",
    ) -> Http2DuplexStream:
        """
        Open an extended CONNECT duplex stream for bidirectional data.
        """

    def get_impl(self) -> typing_extensions.CapsuleType | None:
        """
        Return an opaque capsule wrapping the native client handle.
        """

    def request(
        self,
        method: str,
        path: str,
        headers: collections.abc.Iterable[tuple[str, str]] | None = None,
        body: typing.Any = b"",
        scheme: str = "",
    ) -> asyncio.Future[HttpResponse]:
        """
        Send a request and await the full buffered response.
        """

    def request_stream(
        self,
        method: str,
        path: str,
        headers: collections.abc.Iterable[tuple[str, str]] | None = None,
        body: typing.Any = b"",
        scheme: str = "",
    ) -> Http2ResponseStream:
        """
        Open a request and return a pull-oriented response stream for reading the response body incrementally.
        """

    def request_streaming_body(
        self,
        method: str,
        path: str,
        headers: collections.abc.Iterable[tuple[str, str]] | None = None,
        scheme: str = "",
    ) -> Http2DuplexStream:
        """
        Open a request whose body is written incrementally afterwards, for an upload of unknown or unbounded length. Returns a duplex stream: write() sends more of the body, finish() ends it, and the response is read from the same handle. Do not set content-length.
        """

    @property
    def connected(self) -> bool:
        """
        Whether the client is currently connected.
        """

    @property
    def host(self) -> str:
        """
        The host the client is connected to.
        """

    @property
    def multiplexed(self) -> bool:
        """
        Whether this connection can carry several exchanges at once: true for HTTP/2, false for HTTP/1.1, which A11 limits to one request per connection.
        """

    @property
    def port(self) -> int:
        """
        The port the client is connected to.
        """

    @property
    def secure(self) -> bool:
        """
        Whether the connection is using TLS.
        """

class Http2DuplexStream:
    def __aiter__(self): ...
    async def __anext__(self) -> bytes: ...
    def abort(self, status: Status | None = None) -> None:
        """
        Abort the duplex stream with an optional status.
        """

    def finish(self) -> None:
        """
        Signal the end of the request side of the duplex stream.
        """

    def headers(self) -> asyncio.Future[HttpResponseHead]:
        """
        Await the response head (status and headers).
        """

    def read(self) -> asyncio.Future[bytes | None]:
        """
        Await the next chunk of response data, or None at end of stream.
        """

    def wait_done(self) -> asyncio.Future[None]:
        """
        Await completion of the duplex stream.
        """

    def write(self, data: typing.Any) -> None:
        """
        Write a chunk of request data to the duplex stream.
        """

    @property
    def done(self) -> asyncio.Future[None]:
        """
        Future that completes when the duplex stream is done.
        """

    @property
    def response(self) -> Http2ResponseStream:
        """
        The read half, for the parts of a response this facade does not forward: its trailers, and any pushed responses.
        """

    @property
    def stream_id(self) -> int:
        """
        The HTTP/2 stream identifier.
        """

class Http2Options:
    def __init__(self) -> None:
        """
        Construct default HTTP/2 options.
        """

    def validate(self) -> None:
        """
        Validate the options, raising on error.
        """

    @property
    def client_allow_downgrade(self) -> bool:
        """
        Whether a cleartext client may retry with the other protocol when its first attempt fails.
        """
    @client_allow_downgrade.setter
    def client_allow_downgrade(self, arg0: bool) -> None: ...

    @property
    def client_preference(self) -> HttpProtocolPreference:
        """
        Client protocol preference and cleartext attempt order.
        """
    @client_preference.setter
    def client_preference(self, arg0: HttpProtocolPreference) -> None: ...

    @property
    def deadline(self) -> Time:
        """
        The operation deadline.
        """
    @deadline.setter
    def deadline(self, arg1: Time | None) -> None: ...

    @property
    def enable_h2(self) -> bool:
        """
        Serve/accept HTTP/2 over TLS (ALPN "h2").
        """
    @enable_h2.setter
    def enable_h2(self, arg0: bool) -> None: ...

    @property
    def enable_h2c(self) -> bool:
        """
        Serve/accept cleartext prior-knowledge HTTP/2.
        """
    @enable_h2c.setter
    def enable_h2c(self, arg0: bool) -> None: ...

    @property
    def enable_http1(self) -> bool:
        """
        Serve/accept HTTP/1.1 (ALPN and/or cleartext).
        """
    @enable_http1.setter
    def enable_http1(self, arg0: bool) -> None: ...

    @property
    def enable_push(self) -> bool:
        """
        Client: accept HTTP/2 server pushes. Off by default, and advertised as off, so a peer cannot spend this side's streams on responses nobody asked for. A client that enables it must read Http2ResponseStream.next_push and either consume or cancel each pushed response.
        """
    @enable_push.setter
    def enable_push(self, arg0: bool) -> None: ...

    @property
    def max_buffered_request_bytes(self) -> int:
        """
        Maximum buffered request bytes before backpressure.
        """
    @max_buffered_request_bytes.setter
    def max_buffered_request_bytes(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def max_buffered_response_bytes(self) -> int:
        """
        Maximum buffered response bytes before backpressure.
        """
    @max_buffered_response_bytes.setter
    def max_buffered_response_bytes(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def max_request_body_size(self) -> int:
        """
        Maximum accepted request body size in bytes.
        """
    @max_request_body_size.setter
    def max_request_body_size(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def max_response_body_size(self) -> int:
        """
        Maximum accepted response body size in bytes.
        """
    @max_response_body_size.setter
    def max_response_body_size(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def tls(self) -> Http2TlsOptions:
        """
        The TLS options.
        """
    @tls.setter
    def tls(self, arg0: Http2TlsOptions) -> None: ...

class Http2RequestBodyStream:
    def __aiter__(self): ...
    async def __anext__(self) -> bytes: ...
    def cancel(self, status: Status | None = None) -> None:
        """
        Cancel the request body stream with an optional status.
        """

    def read(self) -> asyncio.Future[bytes | None]:
        """
        Await the next chunk of request body data, or None at end of stream.
        """

    def wait_done(self) -> asyncio.Future[None]:
        """
        Await completion of the request body stream.
        """

    @property
    def done(self) -> asyncio.Future[None]:
        """
        Future that completes when the request body stream is done.
        """

    @property
    def stream_id(self) -> int:
        """
        The HTTP/2 stream identifier.
        """

class Http2ResponseStream:
    def __aiter__(self): ...
    async def __anext__(self) -> bytes: ...
    def cancel(self, status: Status | None = None) -> None:
        """
        Cancel the response stream with an optional status.
        """

    def headers(self) -> asyncio.Future[HttpResponseHead]:
        """
        Await the response head (status and headers).
        """

    def next_push(self) -> asyncio.Future[HttpPushedResponse | None]:
        """
        Await the next response the server pushed alongside this one, or None once this response has ended (after which no push can arrive). Requires Http2Options.enable_push.
        """

    def read(self) -> asyncio.Future[bytes | None]:
        """
        Await the next chunk of response body data, or None at end of stream.
        """

    def trailers(self) -> asyncio.Future[list[tuple[str, str]]]:
        """
        Await the trailer fields that followed the body. Resolves to an empty list when the peer sent no trailer section, so this can be awaited without knowing whether one was coming.
        """

    def wait_done(self) -> asyncio.Future[None]:
        """
        Await completion of the response stream.
        """

    @property
    def done(self) -> asyncio.Future[None]:
        """
        Future that completes when the response stream is done.
        """

    @property
    def stream_id(self) -> int:
        """
        The HTTP/2 stream identifier.
        """

class Http2ResponseWriter:
    def abort(self, status: Status) -> None:
        """
        Abort the response with the given status.
        """

    def finish(self) -> None:
        """
        Signal the end of the response body.
        """

    def finish_with_trailers(
        self, trailers: collections.abc.Iterable[tuple[str, str]] | None = None
    ) -> None:
        """
        End the response body with a trailer section -- the only place a value computed while streaming (a checksum, a row count) can be reported from. Equivalent to finish() when empty.
        """

    def push_promise(
        self,
        method: str,
        path: str,
        headers: collections.abc.Iterable[tuple[str, str]] | None = None,
    ) -> Http2ResponseWriter:
        """
        Promise a response the client did not ask for, returning the writer for it. Must be called before this response is finished, and fails when the client did not enable push.
        """

    def send_headers(
        self,
        status: typing.SupportsInt,
        headers: collections.abc.Iterable[tuple[str, str]] | None = None,
    ) -> None:
        """
        Send the response status and headers.
        """

    def send_response(
        self,
        status: typing.SupportsInt,
        headers: collections.abc.Iterable[tuple[str, str]] | None = None,
        body: typing.Any = b"",
    ) -> None:
        """
        Send a complete response (status, headers, and body) at once.
        """

    def wait_done(self) -> asyncio.Future[None]:
        """
        Await completion of the response.
        """

    def write(self, data: typing.Any) -> None:
        """
        Write a chunk of response body data.
        """

    @property
    def done(self) -> asyncio.Future[None]:
        """
        Future that completes when the response is done.
        """

    @property
    def finished(self) -> bool:
        """
        Whether the response has been finished.
        """

    @property
    def headers_sent(self) -> bool:
        """
        Whether the response headers have been sent.
        """

    @property
    def stream_id(self) -> int:
        """
        The HTTP/2 stream identifier.
        """

class Http2Server:
    @staticmethod
    def create(
        bind_address: str = "127.0.0.1",
        port: typing.SupportsInt = 0,
        handler: typing.Any | None = None,
        options: Http2Options = ...,
    ) -> Http2Server:
        """
        Create and start an HTTP/2 server bound to the given address and port, dispatching each request to the async handler.
        """

    def __enter__(self) -> "Http2Server": ...
    def __exit__(self, exc_type, exc, traceback) -> None: ...
    def get_impl(self) -> typing_extensions.CapsuleType | None:
        """
        Return an opaque capsule wrapping the native server handle.
        """

    def stop(self) -> None:
        """
        Stop the server and release its resources.
        """

    @property
    def bind_address(self) -> str:
        """
        The address the server is bound to.
        """

    @property
    def port(self) -> int:
        """
        The port the server is listening on.
        """

    @property
    def running(self) -> bool:
        """
        Whether the server is currently running.
        """

    @property
    def secure(self) -> bool:
        """
        Whether the server is using TLS.
        """

class Http2TlsOptions:
    def __init__(self) -> None:
        """
        Construct default HTTP/2 TLS options.
        """

    def validate(self) -> None:
        """
        Validate the TLS options, raising on error.
        """

    @property
    def ca_certificate_pem_file(self) -> str:
        """
        Path to the PEM CA certificate file.
        """
    @ca_certificate_pem_file.setter
    def ca_certificate_pem_file(self, arg0: str) -> None: ...

    @property
    def certificate_pem_file(self) -> str:
        """
        Path to the PEM certificate file.
        """
    @certificate_pem_file.setter
    def certificate_pem_file(self, arg0: str) -> None: ...

    @property
    def enabled(self) -> bool:
        """
        Whether TLS is enabled.
        """
    @enabled.setter
    def enabled(self, arg0: bool) -> None: ...

    @property
    def key_pem_file(self) -> str:
        """
        Path to the PEM private key file.
        """
    @key_pem_file.setter
    def key_pem_file(self, arg0: str) -> None: ...

    @property
    def verify_peer(self) -> bool:
        """
        Whether to verify the peer certificate.
        """
    @verify_peer.setter
    def verify_peer(self, arg0: bool) -> None: ...

class HttpProtocolPreference:
    """
    Members:

    AUTO : Prefer HTTP/2, fall back to HTTP/1.1 (ALPN order / downgrade).

    HTTP2 : Require HTTP/2 (h2 over TLS, prior-knowledge h2c cleartext).

    HTTP11 : Require HTTP/1.1.
    """

    AUTO: typing.ClassVar[HttpProtocolPreference]
    HTTP11: typing.ClassVar[HttpProtocolPreference]
    HTTP2: typing.ClassVar[HttpProtocolPreference]
    __members__: typing.ClassVar[dict[str, HttpProtocolPreference]]
    def __eq__(self, other: object) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: typing.SupportsInt) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: typing.SupportsInt) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class HttpPushedResponse:
    def __repr__(self) -> str: ...
    @property
    def authority(self) -> str:
        """
        Authority of the promised request.
        """

    @property
    def headers(self) -> list[tuple[str, str]]:
        """
        Header fields of the promised request, as (name, value) pairs.
        """

    @property
    def method(self) -> str:
        """
        Method of the request the server promised.
        """

    @property
    def path(self) -> str:
        """
        Path and query of the promised request.
        """

    @property
    def response(self) -> Http2ResponseStream:
        """
        The pushed response: its own head, body and trailers. Cancel it to refuse the push.
        """

    @property
    def scheme(self) -> str:
        """
        Scheme of the promised request.
        """

class HttpRequest:
    def __init__(
        self,
        method: str = "GET",
        scheme: str = "http",
        authority: str = "",
        path: str = "/",
        headers: collections.abc.Iterable[tuple[str, str]] | None = None,
        body: typing.Any = b"",
    ) -> None:
        """
        Construct an HTTP request.
        """

    @property
    def authority(self) -> str:
        """
        The request authority (host and optional port).
        """
    @authority.setter
    def authority(self, arg0: str) -> None: ...

    @property
    def body(self) -> bytes:
        """
        The request body as bytes.
        """
    @body.setter
    def body(self, arg1: typing.Any) -> None: ...

    @property
    def body_stream(self) -> Http2RequestBodyStream:
        """
        Pull-oriented request body stream, present only for requests that remain open after their headers.
        """

    @property
    def headers(self) -> list[tuple[str, str]]:
        """
        The request headers as a list of (name, value) pairs.
        """
    @headers.setter
    def headers(
        self, arg1: collections.abc.Iterable[tuple[str, str]] | None
    ) -> None: ...

    @property
    def method(self) -> str:
        """
        The HTTP request method (e.g. GET, POST).
        """
    @method.setter
    def method(self, arg0: str) -> None: ...

    @property
    def path(self) -> str:
        """
        The request target path.
        """
    @path.setter
    def path(self, arg0: str) -> None: ...

    @property
    def protocol(self) -> str:
        """
        The extended CONNECT protocol, if any.
        """
    @protocol.setter
    def protocol(self, arg0: str) -> None: ...

    @property
    def scheme(self) -> str:
        """
        The URI scheme (e.g. http, https).
        """
    @scheme.setter
    def scheme(self, arg0: str) -> None: ...

class HttpResponse:
    def __init__(
        self, head: HttpResponseHead = ..., body: typing.Any = b""
    ) -> None:
        """
        Construct an HTTP response from a head and body.
        """

    @property
    def body(self) -> bytes:
        """
        The response body as bytes.
        """
    @body.setter
    def body(self, arg1: typing.Any) -> None: ...

    @property
    def head(self) -> HttpResponseHead:
        """
        The response head (status and headers).
        """
    @head.setter
    def head(self, arg0: HttpResponseHead) -> None: ...

class HttpResponseHead:
    def __init__(
        self,
        status: typing.SupportsInt = 0,
        headers: collections.abc.Iterable[tuple[str, str]] | None = None,
    ) -> None:
        """
        Construct an HTTP response head (status and headers).
        """

    @property
    def headers(self) -> list[tuple[str, str]]:
        """
        The response headers as a list of (name, value) pairs.
        """
    @headers.setter
    def headers(
        self, arg1: collections.abc.Iterable[tuple[str, str]] | None
    ) -> None: ...

    @property
    def status(self) -> int:
        """
        The HTTP status code.
        """
    @status.setter
    def status(self, arg0: typing.SupportsInt) -> None: ...

class HttpSseClientWireStream(HttpSseWireStream):
    @staticmethod
    def create(
        url: str,
        options: HttpSseOptions = ...,
        client: Http2Client | None = None,
        request_headers: (
            collections.abc.Iterable[tuple[str, str]] | None
        ) = None,
    ) -> HttpSseClientWireStream:
        """
        Create a client-side SSE wire stream connecting to the given URL, optionally reusing an existing HTTP/2 client. Prefer this factory when wiring an agent's outbound transport, and drive the returned stream asynchronously as SSE events arrive.
        """

    def __init__(
        self,
        url: str,
        options: HttpSseOptions = ...,
        client: Http2Client | None = None,
        request_headers: (
            collections.abc.Iterable[tuple[str, str]] | None
        ) = None,
    ) -> None:
        """
        Construct a client-side SSE wire stream that connects to the given URL. This is the transport an A11 agent uses to exchange messages with a remote service over HTTP/2 Server-Sent Events; the connection is opened lazily and runs asynchronously, so await the stream's lifecycle futures rather than blocking.
        """

    @property
    def client(self) -> Http2Client:
        """
        The underlying HTTP/2 client backing this SSE wire stream, which you can reuse to multiplex additional streams from the same agent connection.
        """

    @property
    def outbound_delivery(self) -> SseOutboundDelivery:
        """
        The outbound delivery method actually in use. Equals the requested one once connected, except where a STREAM request fell back to POST because the server did not advertise streamed delivery.
        """

class HttpSseOptions:
    def __init__(self) -> None:
        """
        Construct default HTTP SSE wire stream options.
        """

    def validate(self) -> None:
        """
        Validate the options, raising on error.
        """

    @property
    def connect_endpoint(self) -> str:
        """
        The endpoint path used to open the SSE connection.
        """
    @connect_endpoint.setter
    def connect_endpoint(self, arg0: str) -> None: ...

    @property
    def cors_allow_headers(self) -> str:
        """
        Value for Access-Control-Allow-Headers.
        """
    @cors_allow_headers.setter
    def cors_allow_headers(self, arg0: str) -> None: ...

    @property
    def cors_allow_methods(self) -> str:
        """
        Value for Access-Control-Allow-Methods.
        """
    @cors_allow_methods.setter
    def cors_allow_methods(self, arg0: str) -> None: ...

    @property
    def cors_allow_origin(self) -> str:
        """
        Value for Access-Control-Allow-Origin; empty disables CORS.
        """
    @cors_allow_origin.setter
    def cors_allow_origin(self, arg0: str) -> None: ...

    @property
    def cors_expose_headers(self) -> str:
        """
        Value for Access-Control-Expose-Headers.
        """
    @cors_expose_headers.setter
    def cors_expose_headers(self, arg0: str) -> None: ...

    @property
    def http2_options(self) -> Http2Options:
        """
        The underlying HTTP/2 transport options.
        """
    @http2_options.setter
    def http2_options(self, arg0: Http2Options) -> None: ...

    @property
    def max_concurrent_posts(self) -> int:
        """
        Outbound POSTs kept in flight at once; 1 restores strictly serialised delivery.
        """
    @max_concurrent_posts.setter
    def max_concurrent_posts(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def message_endpoint(self) -> str:
        """
        The endpoint path template used to post messages.
        """
    @message_endpoint.setter
    def message_endpoint(self, arg0: str) -> None: ...

    @property
    def outbound(self) -> SseOutboundDelivery:
        """
        Client-side outbound delivery method.
        """
    @outbound.setter
    def outbound(self, arg0: SseOutboundDelivery) -> None: ...

    @property
    def stream_options(self) -> WireStreamOptions:
        """
        The underlying wire stream options.
        """
    @stream_options.setter
    def stream_options(self, arg0: WireStreamOptions) -> None: ...

class HttpSseServer:
    @staticmethod
    def create(
        bind_address: str = "127.0.0.1",
        port: typing.SupportsInt = 0,
        on_connect: typing.Any | None = None,
        options: HttpSseOptions = ...,
    ) -> HttpSseServer:
        """
        Create and start an SSE server that accepts A11 wire streams, invoking the optional async on_connect callback for each client.
        """

    def __enter__(self) -> HttpSseServer: ...
    def __exit__(self, exc_type, exc, traceback) -> None: ...
    def stop(self) -> None:
        """
        Stop the server and release its resources.
        """

    def wait_for_stream(self) -> asyncio.Future[HttpSseServerWireStream]:
        """
        Await the next incoming SSE wire stream from a connecting client.
        """

    @property
    def http2_server(self) -> Http2Server:
        """
        The underlying HTTP/2 server.
        """

    @property
    def port(self) -> int:
        """
        The port the server is listening on.
        """

    @property
    def running(self) -> bool:
        """
        Whether the server is currently running.
        """

class HttpSseServerWireStream(HttpSseWireStream):
    def accepted(self) -> asyncio.Future[None]:
        """
        Await acceptance of this server-side SSE wire stream. This is the server counterpart delivered to your on_connect handler when a client opens an SSE connection; await this future to know the stream has been fully established before your agent starts sending messages on it.
        """

class HttpSseWireStream(WireStream):
    def get_http_request_headers(self) -> list[tuple[str, str]]:
        """
        Return the HTTP headers carried on the underlying SSE request.
        """

    def get_http_response_headers(self) -> list[tuple[str, str]] | None:
        """
        Return the HTTP response headers negotiated for the SSE connection, or None if they have not arrived yet. Because the connection is established asynchronously, prefer awaiting wait_for_http_headers() before relying on this value.
        """

    def set_http_request_headers(
        self, headers: collections.abc.Iterable[tuple[str, str]] | None
    ) -> None:
        """
        Set the HTTP headers to send on the underlying SSE request. Call this before the stream connects to attach auth or routing metadata that your agent's transport needs.
        """

    def set_http_response_headers(
        self, headers: collections.abc.Iterable[tuple[str, str]] | None
    ) -> None:
        """
        Set the HTTP headers to send on the SSE response. Used on the server side to attach transport metadata before the streaming response is flushed to the client.
        """

    def wait_for_http_headers(self) -> asyncio.Future[None]:
        """
        Await the exchange of HTTP headers for the SSE connection. Because SSE wire streams connect asynchronously, await this future before reading response headers or assuming the stream is live.
        """

class InProcessWireStream(WireStream):
    @staticmethod
    def create_pair(
        options: WireStreamOptions | None = None,
        first_options: WireStreamOptions | None = None,
        second_options: WireStreamOptions | None = None,
    ) -> tuple[InProcessWireStream, InProcessWireStream]:
        """
        Create a connected pair of in-process wire streams that talk to each other directly in memory, with no network involved. One endpoint drives start() while the other drives accept(). Pass shared options, or per-endpoint first_options/second_options, to tune buffering and timeouts.
        """

    def wait(self) -> asyncio.Future[None]:
        """
        Await until this in-process stream has fully finished. Block on this to know a local agent exchange has completed before tearing the pair down.
        """

class LocalChunkStore(ChunkStore):
    @staticmethod
    def create(node_id: str) -> LocalChunkStore:
        """
        Create a LocalChunkStore for the given node id. Equivalent to the constructor; use whichever reads more clearly at the call site.
        """

    def __init__(self, id: str) -> None:
        """
        Create an in-memory ChunkStore identified by `id`. This is the default backing store for an agent running in a single process: all reads and writes stay in local memory yet still return awaitables, so it composes with the same async reader and writer as remote stores.
        """

class NativeActionHandler:
    """
    An Action handler implemented in C++, such as one of the audio SDK's. It is an opaque handle rather than something Python calls: pass it wherever a handler is accepted -- ActionRegistry.register(), Action.bind_handler() -- and the native implementation runs directly, without a round trip through the interpreter.
    """

    def __bool__(self) -> bool: ...
    def __repr__(self) -> str: ...

class NodeFragment:
    """
    A fragment of a logical node carrying a Chunk or NodeRef.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @staticmethod
    def from_msgpack(data: typing.Any) -> NodeFragment:
        """
        Deserialize a value from MessagePack bytes.
        """

    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_construct(cls, **values: typing.Any):
        """
        Construct a native value from trusted field values.

        Native records retain C++ invariants. This validates input rather than
        creating an invalid object.
        """

    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(
        cls, value: str | bytes | bytearray, **_: typing.Any
    ): ...
    def __copy__(self) -> NodeFragment:
        """
        Return a shallow copy of the value.
        """

    def __deepcopy__(self, memo: dict) -> NodeFragment:
        """
        Return a deep copy of the value.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether two values are equal.
        """

    def __init__(
        self,
        data: typing.Any,
        id: str = "",
        seq: typing.SupportsInt | None = None,
        continued: bool = False,
    ) -> None:
        """
        Create a node fragment from Chunk/NodeRef data and framing fields.
        """

    def __repr__(self) -> str:
        """
        Return a human-readable debug string.
        """

    def debug_string(self) -> str:
        """
        Return a human-readable debug string.
        """

    def get_chunk(self) -> Chunk:
        """
        Return the fragment's Chunk, raising if it holds a NodeRef.
        """

    def get_node_ref(self) -> NodeRef:
        """
        Return the fragment's NodeRef, raising if it holds a Chunk.
        """

    def model_copy(
        self, *, update: dict[str, typing.Any] | None = None, deep: bool = False
    ): ...
    def model_dump(
        self, *, mode: str = "python", **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def to_msgpack(self) -> bytes:
        """
        Serialize the value to MessagePack bytes.
        """

    def validate(self) -> None:
        """
        Raise if the value fails structural validation.
        """

    @property
    def approx_bytes(self) -> int:
        """
        Approximate in-memory size of the fragment in bytes.
        """

    @property
    def continued(self) -> bool:
        """
        Whether more fragments follow for this node.
        """
    @continued.setter
    def continued(self, arg0: bool) -> None: ...

    @property
    def data(self) -> Chunk | NodeRef:
        """
        Payload of the fragment as either a Chunk or a NodeRef.
        """
    @data.setter
    def data(self, arg1: typing.Any) -> None: ...

    @property
    def id(self) -> str:
        """
        Identifier of the logical node this fragment belongs to.
        """
    @id.setter
    def id(self, arg0: str) -> None: ...

    @property
    def seq(self) -> int | None:
        """
        Optional sequence number of the fragment.
        """
    @seq.setter
    def seq(self, arg0: typing.SupportsInt | None) -> None: ...

class NodeMap:
    @staticmethod
    def __getitem__(*args, **kwargs):
        """
        get(self: NodeMap, node_id: str) -> a11::nodes::AsyncNode

        Returns the node for the given id, creating it if it does not already exist.
        """

    def __contains__(self, node_id: str) -> bool:
        """
        Returns whether a node with the given id exists.
        """

    def __init__(self, chunk_store_factory: typing.Any | None = None) -> None:
        """
        Creates a node map, optionally backed by a chunk-store factory callable invoked to construct the backing store for each new node.
        """

    def __len__(self) -> int:
        """
        Number of nodes in the map.
        """

    def contains(self, node_id: str) -> bool:
        """
        Returns whether a node with the given id exists.
        """

    def discard(
        self, node_id: str, expected: AsyncNode | None = None
    ) -> AsyncNode | None:
        """
        Removes the node for the given id, optionally only if it matches the expected node.
        """

    def get(self, node_id: str) -> AsyncNode:
        """
        Returns the node for the given id, creating it if it does not already exist.
        """

    def get_if_exists(self, node_id: str) -> AsyncNode | None:
        """
        Returns the node for the given id, or None if it does not exist.
        """

    def ids(self) -> list[str]:
        """
        The id of every node the map holds, sorted.

        A snapshot: nodes are created on demand, so this is what was there when it was asked for.
        """

    def size(self) -> int:
        """
        Number of nodes in the map.
        """

class NodeRef:
    """
    Reference to a byte range of another logical node.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @staticmethod
    def from_msgpack(data: typing.Any) -> NodeRef:
        """
        Deserialize a value from MessagePack bytes.
        """

    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_construct(cls, **values: typing.Any):
        """
        Construct a native value from trusted field values.

        Native records retain C++ invariants. This validates input rather than
        creating an invalid object.
        """

    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(
        cls, value: str | bytes | bytearray, **_: typing.Any
    ): ...
    def __copy__(self) -> NodeRef:
        """
        Return a shallow copy of the value.
        """

    def __deepcopy__(self, memo: dict) -> NodeRef:
        """
        Return a deep copy of the value.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether two values are equal.
        """

    def __init__(
        self,
        id: str,
        offset: typing.SupportsInt | None = 0,
        length: typing.Any | None = None,
    ) -> None:
        """
        Create a node reference from an id, byte offset, and length.
        """

    def __repr__(self) -> str:
        """
        Return a human-readable debug string.
        """

    def debug_string(self) -> str:
        """
        Return a human-readable debug string.
        """

    def model_copy(
        self, *, update: dict[str, typing.Any] | None = None, deep: bool = False
    ): ...
    def model_dump(
        self, *, mode: str = "python", **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def to_msgpack(self) -> bytes:
        """
        Serialize the value to MessagePack bytes.
        """

    def validate(self) -> None:
        """
        Raise if the value fails structural validation.
        """

    @property
    def approx_bytes(self) -> int:
        """
        Approximate in-memory size of the ref in bytes.
        """

    @property
    def id(self) -> str:
        """
        Identifier of the referenced node.
        """
    @id.setter
    def id(self, arg0: str) -> None: ...

    @property
    def length(self) -> int | None:
        """
        Optional byte length of the referenced range.
        """
    @length.setter
    def length(self, arg0: typing.SupportsInt | None) -> None: ...

    @property
    def offset(self) -> int:
        """
        Byte offset into the referenced node.
        """
    @offset.setter
    def offset(self, arg0: typing.SupportsInt) -> None: ...

class ParsedUrl:
    def __init__(self) -> None:
        """
        Construct an empty parsed URL.
        """

    def __repr__(self) -> str: ...
    def __str__(self) -> str: ...
    @property
    def authority(self) -> str:
        """
        The authority as a header value.
        """

    @property
    def host(self) -> str:
        """
        Hostname or IP literal, without IPv6 brackets.
        """
    @host.setter
    def host(self, arg0: str) -> None: ...

    @property
    def origin(self) -> str:
        """
        scheme://authority, with no trailing slash.
        """

    @property
    def path(self) -> str:
        """
        Path beginning with '/', or empty when none was given.
        """
    @path.setter
    def path(self, arg0: str) -> None: ...

    @property
    def port(self) -> int:
        """
        Explicit port, or the scheme's default.
        """
    @port.setter
    def port(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def query(self) -> str:
        """
        Query without the leading '?'.
        """
    @query.setter
    def query(self, arg0: str) -> None: ...

    @property
    def scheme(self) -> str:
        """
        Lowercase scheme, without "://".
        """
    @scheme.setter
    def scheme(self, arg0: str) -> None: ...

    @property
    def secure(self) -> bool:
        """
        Whether the scheme implies TLS.
        """

    @property
    def target(self) -> str:
        """
        The request target: path and query, at least "/".
        """

class Port:
    """
    A named input or output port of an action.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @staticmethod
    def from_msgpack(data: typing.Any) -> Port:
        """
        Deserialize a value from MessagePack bytes.
        """

    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_construct(cls, **values: typing.Any):
        """
        Construct a native value from trusted field values.

        Native records retain C++ invariants. This validates input rather than
        creating an invalid object.
        """

    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(
        cls, value: str | bytes | bytearray, **_: typing.Any
    ): ...
    def __copy__(self) -> Port:
        """
        Return a shallow copy of the value.
        """

    def __deepcopy__(self, memo: dict) -> Port:
        """
        Return a deep copy of the value.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether two values are equal.
        """

    def __init__(self, name: str = "", id: str = "") -> None:
        """
        Create a port from a name and node id.
        """

    def __repr__(self) -> str:
        """
        Return a human-readable debug string.
        """

    def debug_string(self) -> str:
        """
        Return a human-readable debug string.
        """

    def model_copy(
        self, *, update: dict[str, typing.Any] | None = None, deep: bool = False
    ): ...
    def model_dump(
        self, *, mode: str = "python", **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def to_msgpack(self) -> bytes:
        """
        Serialize the value to MessagePack bytes.
        """

    def validate(self) -> None:
        """
        Raise if the value fails structural validation.
        """

    @property
    def approx_bytes(self) -> int:
        """
        Approximate in-memory size of the port in bytes.
        """

    @property
    def id(self) -> str:
        """
        Identifier of the node bound to the port.
        """
    @id.setter
    def id(self, arg0: str) -> None: ...

    @property
    def name(self) -> str:
        """
        Name of the port.
        """
    @name.setter
    def name(self, arg0: str) -> None: ...

class RedisChunkStore(ChunkStore):
    """
    A persistent, multi-process ChunkStore backed by Redis Streams.
    """

    @staticmethod
    def create(
        id: str,
        client: RedisClient | None = None,
        options: RedisChunkStoreOptions | dict[str, typing.Any] | None = None,
    ) -> RedisChunkStore:
        """
        Create a Redis-backed fragment log for one node id.
        """

    def __init__(
        self,
        id: str,
        client: RedisClient | None = None,
        options: RedisChunkStoreOptions | dict[str, typing.Any] | None = None,
    ) -> None:
        """
        Open one node's persistent stream with an injected Redis client.

        Stores for the same id address the same Redis state. Pass a shared
        `RedisClient` in production so many nodes reuse one connection pool;
        call `initialize` when metadata must exist before the first write.
        """

    async def clear_data(self, seq: int) -> NodeFragment:
        """
        Tombstone one payload while retaining ordering metadata.
        """

    async def close_writes_with_status(
        self, status: Status, return_status_if_already_closed: bool = False
    ) -> Status:
        """
        Atomically seal writes with a terminal status and wake readers.

        This closes the producer side but does not mark data final. Write a
        final fragment first when consumers use whole-value semantics such as
        `AsyncNode.consume`.
        """

    async def get(self, seq: int, deadline: Time | None = None) -> NodeFragment:
        """
        Wait for and return a fragment by sequence number.

        The deadline bounds both Redis work and the wait for a future fragment.
        """

    async def get_by_arrival_order(
        self, arrival_order: int, deadline: Time | None = None
    ) -> NodeFragment:
        """
        Wait for a fragment by its zero-based Redis ingestion order.
        """

    async def get_final_seq(self) -> int | None:
        """
        Return the logical final sequence, if one has been written.

        The final marker is independent of Redis write closure. Closing a store
        does not synthesize it, and a final fragment does not close the store.
        """

    async def get_metadata(self) -> RedisChunkStoreMetadata:
        """
        Read size, finality, and closure state without scanning fragments.
        """

    async def get_seq_for_arrival_order(self, arrival_order: int) -> int:
        """
        Translate a zero-based ingestion position to its sequence number.
        """

    async def initialize(self) -> None:
        """
        Ensure node metadata exists without writing a fragment.

        This is useful during provisioning or health checks; ordinary writes
        initialize the store lazily.
        """

    async def next(
        self, deadline: Time | None = None, limit: int = 1
    ) -> list[NodeFragment | None]:
        """
        Read from the persistent shared logical-sequence cursor.

        The cursor advances through sequence numbers and waits at gaps;
        ``None`` marks clean end-of-stream. Use `get_by_arrival_order` for
        ingestion order. Prefer `ChunkStoreReader` for normal node consumption;
        it adds buffering, offsets, and final-sequence handling above this
        primitive.
        """

    async def put(self, fragment: NodeFragment) -> int:
        """
        Atomically append one fragment and return its sequence number.
        """

    async def put_many(
        self, fragments: typing.Sequence[NodeFragment]
    ) -> list[int]:
        """
        Atomically append a batch and return its assigned sequences.
        """

    async def size(self) -> int:
        """
        Return the number of fragment entries recorded for this node.
        """

    @property
    def client(self) -> RedisClient:
        """
        The explicitly composed RedisClient.
        """

    @property
    def keys(self) -> RedisChunkStoreKeys:
        """
        A copy of the sharding-safe Redis key layout.
        """

    @property
    def options(self) -> RedisChunkStoreOptions:
        """
        A copy of this store's key and payload policy.
        """

class RedisChunkStoreKeys:
    """
    The sharding-safe Redis keys owned by one node stream.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    def __eq__(self, arg0: object) -> bool: ...
    def script_keys(self) -> list[str]:
        """
        Return keys in the stable order used by the Lua state machine.
        """

    @property
    def arrival_index(self) -> str:
        """
        Arrival-order-to-sequence hash.
        """

    @property
    def blobs(self) -> str:
        """
        Hash containing large encoded chunk payloads.
        """

    @property
    def events(self) -> str:
        """
        Pub/Sub channel used for invalidation notifications.
        """

    @property
    def metadata(self) -> str:
        """
        Hash containing node-level metadata.
        """

    @property
    def sequence_index(self) -> str:
        """
        Sequence-to-stream-entry hash.
        """

    @property
    def stream(self) -> str:
        """
        Redis Stream containing chunk and control entries.
        """

class RedisChunkStoreMetadata:
    """
    Node-level Redis state read without iterating over chunk entries.
    """

    @property
    def closed(self) -> bool:
        """
        Whether the store rejects new writes.
        """

    @property
    def final_seq(self) -> int | None:
        """
        The declared final sequence, if one has arrived.
        """

    @property
    def id(self) -> str:
        """
        The owning AsyncNode identifier.
        """

    @property
    def max_seq(self) -> int | None:
        """
        Largest sequence currently present.
        """

    @property
    def next_cursor(self) -> int:
        """
        Global SPMC cursor used by next().
        """

    @property
    def revision(self) -> int:
        """
        Monotonic mutation generation published to waiters.
        """

    @property
    def size(self) -> int:
        """
        Number of chunk slots in the store.
        """

    @property
    def status(self) -> Status | None:
        """
        Return the terminal status when closed, otherwise ``None``.
        """

    @property
    def total_chunks_put(self) -> int:
        """
        Number of chunks appended over the store lifetime.
        """

class RedisChunkStoreOptions:
    """
    Key layout and inline-payload policy for RedisChunkStore.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def from_environment() -> RedisChunkStoreOptions:
        """
        Read the A11_REDIS_CHUNK_STORE_* environment variables.
        """

    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        key_prefix: str = "a11:",
        inline_data_threshold: typing.SupportsInt | None = 262144,
    ) -> None:
        """
        Construct validated Redis chunk-store options.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...
    def validate(self) -> None:
        """
        Raise if the key layout policy is invalid.
        """

    @property
    def inline_data_threshold(self) -> int:
        """
        Chunk data larger than this many bytes uses the blob hash.
        """
    @inline_data_threshold.setter
    def inline_data_threshold(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def key_prefix(self) -> str:
        """
        Prefix before the per-node Redis Cluster hash tag.
        """
    @key_prefix.setter
    def key_prefix(self, arg0: str) -> None: ...

class RedisClient:
    """
    An asynchronous, binary-safe hiredis/libuv client using A11 futures.
    """

    @staticmethod
    def create(
        options: RedisClientOptions | dict[str, typing.Any] | None = None,
    ) -> RedisClient:
        """
        Create a client and begin connecting without blocking.
        """

    async def __aenter__(self) -> typing.Self: ...
    async def __aexit__(self, exc_type, exc, traceback) -> None: ...
    def __init__(
        self, options: RedisClientOptions | dict[str, typing.Any] | None = None
    ) -> None:
        """
        Begin connecting with validated native options.

        A plain mapping is validated into the same bound options object used
        by C++, rather than creating a second Python configuration model.
        """

    def close(self) -> None:
        """
        Begin an idempotent asynchronous disconnect.
        """

    async def command(
        self,
        parts: typing.Sequence[str | bytes | bytearray | memoryview],
        deadline: Time | None = None,
    ) -> RedisReply:
        """
        Execute one binary-safe Redis command.
        """

    async def eval(
        self,
        script: str | bytes,
        keys: typing.Sequence[str | bytes],
        arguments: typing.Sequence[
            str | bytes | bytearray | memoryview
        ] = tuple(),
        deadline: Time | None = None,
    ) -> RedisReply:
        """
        Execute Lua with every cluster-sensitive key declared.
        """

    async def ready(self) -> None:
        """
        Wait until command and Pub/Sub connections are initialized.
        """

    async def subscribe(
        self, channel: str, deadline: Time | None = None
    ) -> RedisSubscription:
        """
        Subscribe and wait for Redis's acknowledgement.
        """

    @property
    def options(self) -> RedisClientOptions:
        """
        A copy of the client's connection options.
        """

class RedisClientOptions:
    """
    Connection and timeout policy for Redis.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def from_environment() -> RedisClientOptions:
        """
        Read A11_REDIS_URL or the individual A11_REDIS_* variables.
        """

    @staticmethod
    def from_url(url: str) -> RedisClientOptions:
        """
        Parse a `redis://[user:password@]host[:port][/database]` URL.
        """

    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        host: str = "127.0.0.1",
        port: typing.SupportsInt = 6379,
        username: str = "",
        password: str = "",
        database: typing.SupportsInt = 0,
        client_name: str = "a11",
        connect_timeout: typing.Any | None = None,
        command_timeout: typing.Any | None = None,
    ) -> None:
        """
        Construct validated Redis connection options.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...
    def validate(self) -> None:
        """
        Raise if these connection options are invalid.
        """

    @property
    def client_name(self) -> str:
        """
        Name reported through CLIENT SETNAME.
        """
    @client_name.setter
    def client_name(self, arg0: str) -> None: ...

    @property
    def command_timeout(self) -> Duration:
        """
        Default maximum time allowed for one command.
        """
    @command_timeout.setter
    def command_timeout(self, arg1: typing.Any) -> None: ...

    @property
    def connect_timeout(self) -> Duration:
        """
        Maximum time allowed for establishing a connection.
        """
    @connect_timeout.setter
    def connect_timeout(self, arg1: typing.Any) -> None: ...

    @property
    def database(self) -> int:
        """
        Logical Redis database selected after connection.
        """
    @database.setter
    def database(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def host(self) -> str:
        """
        Redis host name or IP address.
        """
    @host.setter
    def host(self, arg0: str) -> None: ...

    @property
    def password(self) -> str:
        """
        ACL password, if authentication is enabled.
        """
    @password.setter
    def password(self, arg0: str) -> None: ...

    @property
    def port(self) -> int:
        """
        Redis TCP port.
        """
    @port.setter
    def port(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def username(self) -> str:
        """
        ACL username, if authentication is enabled.
        """
    @username.setter
    def username(self, arg0: str) -> None: ...

class RedisReply:
    """
    An owned, binary-safe RESP value returned by RedisClient.
    """

    def __bytes__(self) -> bytes:
        """
        Return the bytes held by a string reply.
        """

    def __repr__(self) -> str:
        """
        Return a diagnostic representation of the RESP value.
        """

    def as_boolean(self) -> bool:
        """
        Return the boolean payload, raising for another RESP kind.
        """

    def as_bytes(self) -> bytes:
        """
        Return string payload bytes, raising for another RESP kind.
        """

    def as_elements(self) -> list[RedisReply]:
        """
        Return aggregate children; maps alternate key and value entries.
        """

    def as_float(self) -> float:
        """
        Return the double payload, raising for another RESP kind.
        """

    def as_integer(self) -> int:
        """
        Return the integer payload, raising for another RESP kind.
        """

    def debug_string(self) -> str:
        """
        Return a diagnostic representation of the RESP value.
        """

    @property
    def is_null(self) -> bool:
        """
        Whether this is a null RESP value.
        """

    @property
    def type(self) -> RedisReplyType:
        """
        The RESP kind of this value.
        """

class RedisReplyType:
    """
    The RESP value kind held by RedisReply.

    Members:

      NULL

      STRING

      INTEGER

      DOUBLE

      BOOLEAN

      ARRAY

      MAP

      SET
    """

    ARRAY: typing.ClassVar[RedisReplyType]
    BOOLEAN: typing.ClassVar[RedisReplyType]
    DOUBLE: typing.ClassVar[RedisReplyType]
    INTEGER: typing.ClassVar[RedisReplyType]
    MAP: typing.ClassVar[RedisReplyType]
    NULL: typing.ClassVar[RedisReplyType]
    SET: typing.ClassVar[RedisReplyType]
    STRING: typing.ClassVar[RedisReplyType]
    __members__: typing.ClassVar[dict[str, RedisReplyType]]
    def __eq__(self, other: object) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: typing.SupportsInt) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: typing.SupportsInt) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class RedisSubscription:
    """
    A non-buffering broadcast subscription for invalidation events.
    """

    async def wait(self, after: int, deadline: Time | None = None) -> int:
        """
        Wait until a message advances the generation beyond ``after``.
        """

    @property
    def channel(self) -> str:
        """
        The subscribed Redis channel.
        """

    @property
    def generation(self) -> int:
        """
        The current broadcast generation.
        """

class SQLiteChunkStore(ChunkStore):
    """
    A durable, embedded ChunkStore backed by SQLite and blob files.
    """

    @staticmethod
    def create(
        id: str,
        root: str | typing.Any | None = None,
        options: SQLiteChunkStoreOptions | dict[str, typing.Any] | None = None,
    ) -> SQLiteChunkStore:
        """
        Create a SQLite-backed fragment log for one node id.
        """

    def __init__(
        self,
        id: str,
        root: str | typing.Any | None = None,
        options: SQLiteChunkStoreOptions | dict[str, typing.Any] | None = None,
    ) -> None:
        """
        Open one node's durable stream under a storage root.

        Stores for the same id and root address the same rows, so reopening
        after a restart resumes the same fragment log. Omit `root` to use
        `SQLiteChunkStoreFactory.default_root()`. Opening many nodes under one
        root is cheap; they share a single database.
        """

    async def clear_data(self, seq: int) -> NodeFragment:
        """
        Tombstone one payload while retaining ordering metadata.

        Returns the fragment as it was. Any blob file backing it is unlinked
        once the transaction commits. Node-reference fragments cannot be
        cleared, because a tombstone is chunk-shaped.
        """

    async def close_writes_with_status(
        self, status: Status, return_status_if_already_closed: bool = False
    ) -> Status:
        """
        Atomically seal writes with a terminal status and wake readers.

        This closes the producer side but does not mark data final. Write a
        final fragment first when consumers use whole-value semantics such as
        `AsyncNode.consume`.
        """

    async def find_referrers(self, limit: int = 100) -> list[NodeFragment]:
        """
        Find fragments elsewhere whose `NodeRef` points at this node.

        This is the traversal the relational layout exists for: the answer
        comes from an index on the reference target, so the cost tracks the
        number of referrers rather than the size of the database.
        """

    async def get(self, seq: int, deadline: Time | None = None) -> NodeFragment:
        """
        Wait for and return a fragment by sequence number.

        The wait parks on a per-node event rather than polling the database,
        and resolves early with an error once the fragment can no longer
        arrive.
        """

    async def get_by_arrival_order(
        self, arrival_order: int, deadline: Time | None = None
    ) -> NodeFragment:
        """
        Wait for a fragment by its zero-based ingestion order.
        """

    async def get_final_seq(self) -> int | None:
        """
        Return the logical final sequence, if one has been written.

        The final marker is independent of write closure. Closing a store does
        not synthesize it, and a final fragment does not close the store.
        """

    async def get_metadata(self) -> SQLiteChunkStoreMetadata:
        """
        Read cursors, finality, and closure state in one row read.
        """

    async def get_seq_for_arrival_order(self, arrival_order: int) -> int:
        """
        Translate a zero-based ingestion position to its sequence number.
        """

    async def next(
        self, deadline: Time | None = None, limit: int = 1
    ) -> list[NodeFragment | None]:
        """
        Read from the persistent shared logical-sequence cursor.

        The cursor lives in the database, so it survives a restart and is
        shared by every store open on this node. It advances through sequence
        numbers and waits at gaps; ``None`` marks clean end-of-stream. Prefer
        `ChunkStoreReader` for ordinary consumption.
        """

    async def put(self, fragment: NodeFragment) -> int:
        """
        Atomically append one fragment and return its sequence number.
        """

    async def put_many(
        self, fragments: typing.Sequence[NodeFragment]
    ) -> list[int]:
        """
        Atomically append a batch and return its assigned sequences.

        The batch commits in one transaction: either every fragment is stored,
        or none is and no blob file is left behind.
        """

    async def size(self) -> int:
        """
        Return the number of fragment slots, tombstones included.
        """

    async def sweep_orphan_blobs(self) -> int:
        """
        Delete unreferenced blob files older than the grace period.

        A crash between writing a blob and committing its row leaves a file
        nothing points at. This reclaims those; the grace period keeps it from
        deleting a blob whose transaction is still in flight elsewhere.
        """

    @property
    def options(self) -> SQLiteChunkStoreOptions:
        """
        A copy of this store's storage policy.
        """

    @property
    def root(self) -> str:
        """
        The storage root this store reads and writes.
        """

class SQLiteChunkStoreFactory:
    """
    Creates SQLiteChunkStores that share one database per storage root.
    """

    @staticmethod
    def default_root() -> str:
        """
        The process-wide default storage root: $A11_SQLITE_CHUNK_STORE_ROOT, else $XDG_CACHE_HOME/a11/chunks, else ~/.cache/a11/chunks.
        """

    def __call__(self, node_id: str) -> SQLiteChunkStore:
        """
        Open a store, so the factory can be passed directly wherever a chunk_store_factory callable is expected.
        """

    def __init__(
        self,
        root: str | typing.Any | None = None,
        options: SQLiteChunkStoreOptions | dict[str, typing.Any] | None = None,
    ) -> None:
        """
        Create a factory rooted at a directory, created when absent.

        Pass the factory itself wherever a ``chunk_store_factory`` callable is
        expected to make SQLite the backing store for a `NodeMap`, `AsyncNode`,
        or `Session`.
        """

    def open(self, node_id: str) -> SQLiteChunkStore:
        """
        Open a store for `node_id` under this factory's root.
        """

    async def sweep_orphan_blobs(self) -> int:
        """
        Delete unreferenced blob files older than the grace period.
        """

    @property
    def options(self) -> SQLiteChunkStoreOptions:
        """
        A copy of the storage policy applied to every store created here.
        """

    @property
    def root(self) -> str:
        """
        The root this factory creates stores under.
        """

class SQLiteChunkStoreMetadata:
    """
    Node-level SQLite state read without listing fragments.
    """

    @property
    def closed(self) -> bool:
        """
        Whether the store rejects new writes.
        """

    @property
    def created_at(self) -> Time:
        """
        When the node row was created by its first accepted write.
        """

    @property
    def data_bytes(self) -> int:
        """
        Cached total of stored payload bytes.
        """

    @property
    def final_seq(self) -> int | None:
        """
        The declared final sequence, if one has arrived.
        """

    @property
    def id(self) -> str:
        """
        The owning AsyncNode identifier.
        """

    @property
    def max_seq(self) -> int | None:
        """
        Largest sequence currently present.
        """

    @property
    def next_cursor(self) -> int:
        """
        The next sequence the shared next() cursor will want.
        """

    @property
    def owner_id(self) -> str:
        """
        Owner recorded on the node row, possibly empty.
        """

    @property
    def revision(self) -> int:
        """
        Monotonic mutation generation.
        """

    @property
    def size(self) -> int:
        """
        Number of fragment slots, tombstones included.
        """

    @property
    def status(self) -> Status | None:
        """
        Return the terminal status when closed, otherwise ``None``.
        """

    @property
    def total_chunks_put(self) -> int:
        """
        Fragments accepted over the store lifetime.
        """

    @property
    def updated_at(self) -> Time:
        """
        When the node row was last mutated.
        """

class SQLiteChunkStoreOptions:
    """
    Payload, ownership and durability policy for SQLiteChunkStore.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def from_environment() -> SQLiteChunkStoreOptions:
        """
        Read the A11_SQLITE_CHUNK_STORE_* environment variables.
        """

    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        inline_data_threshold: typing.SupportsInt | None = 131072,
        owner_id: str = "",
        synchronous: SQLiteSynchronous = ...,
        cross_process_poll_interval: typing.Any | None = None,
        blob_grace_period: typing.Any | None = None,
    ) -> None:
        """
        Construct validated SQLite chunk-store options.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...
    def validate(self) -> None:
        """
        Raise if the storage policy is invalid.
        """

    @property
    def blob_grace_period(self) -> Duration:
        """
        How long an unreferenced blob survives before a sweep removes it.
        """
    @blob_grace_period.setter
    def blob_grace_period(self, arg1: typing.Any) -> None: ...

    @property
    def cross_process_poll_interval(self) -> Duration:
        """
        How often to notice other processes' commits; zero disables it.
        """
    @cross_process_poll_interval.setter
    def cross_process_poll_interval(self, arg1: typing.Any) -> None: ...

    @property
    def inline_data_threshold(self) -> int:
        """
        Payloads larger than this many bytes move into a blob file.
        """
    @inline_data_threshold.setter
    def inline_data_threshold(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def owner_id(self) -> str:
        """
        Owner recorded on the node row; carries no enforcement.
        """
    @owner_id.setter
    def owner_id(self, arg0: str) -> None: ...

    @property
    def synchronous(self) -> SQLiteSynchronous:
        """
        Durability level applied with PRAGMA synchronous.
        """
    @synchronous.setter
    def synchronous(self, arg0: SQLiteSynchronous) -> None: ...

class SQLiteSynchronous:
    """
    How much durability a SQLite chunk store trades for write throughput.

    Members:

      OFF : Fastest; a machine crash can corrupt recent commits.

      NORMAL : Default; survives an application crash, may lose the newest commits on power loss.

      FULL : Every commit is fsynced.
    """

    FULL: typing.ClassVar[SQLiteSynchronous]
    NORMAL: typing.ClassVar[SQLiteSynchronous]
    OFF: typing.ClassVar[SQLiteSynchronous]
    __members__: typing.ClassVar[dict[str, SQLiteSynchronous]]
    def __eq__(self, other: object) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: typing.SupportsInt) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: typing.SupportsInt) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class SerializationRegistry:
    def __init__(self, register_defaults: bool = False) -> None:
        """
        Creates a serialization registry, optionally pre-populated with the built-in serializers and deserializers.
        """

    def register_defaults(self) -> None:
        """
        Registers the built-in serializers and deserializers on this registry.
        """

    @property
    def deserializer_count(self) -> int:
        """
        Number of registered deserializers.
        """

    @property
    def serializer_count(self) -> int:
        """
        Number of registered serializers.
        """

class Service:
    async def __aenter__(self) -> "Service": ...
    async def __aexit__(self, *exc_info) -> None: ...
    def __init__(
        self,
        *,
        action_registry: ActionRegistry | None = None,
        on_connection: typing.Any | None = None,
        options: ServiceOptions | None = None,
    ) -> None:
        """
        A service: an action registry plus the sessions serving it.

        `accept` is shaped to be a transport's on-stream callback, so one service can be
        bound to several listeners, or to none at all (hand it an in-process stream). The
        optional `on_connection(session, stream)` coroutine runs once per connection,
        after the session exists and before it starts pumping -- the only window in which
        a connection can be specialised without racing its first message.

        Examples:
            Serve a gateway over WebSocket:

            ```python
            service = a11.Service(action_registry=registry, on_connection=prepare)
            server = a11.net.WebSocketWireServer.create(service.accept, options)
            ```
        """

    def abort(self, status: Status) -> None:
        """
        Stop accepting and abort every live session.
        """

    def accept(self, stream: WireStream) -> asyncio.Future[None]:
        """
        Serve an accepted stream, awaiting its session's whole lifetime.
        """

    async def aclose(self, *, timeout: Duration | None = None) -> None:
        """
        Stop accepting, then wait for what is in flight.

        The graceful shutdown, in the order that makes it graceful: refusing new
        connections first means the set being waited on cannot grow.
        """

    def add_stream_to_session(
        self, session_id: str, stream: WireStream, mode: typing.Any = "accept"
    ) -> None:
        """
        Attach another transport to an existing session.
        """

    async def drain(self, timeout: Duration | None = None) -> None:
        """
        Await the completion of every session currently being served.

        Args:
            timeout: How long to wait. ``None`` waits indefinitely.

        Raises:
            StatusException: ``DEADLINE_EXCEEDED`` when sessions remain after
                ``timeout``; they are left running, so follow with `abort` if
                they must go.
        """

    def get_session(self, session_id: str) -> Session:
        """
        The session with this id, raising NOT_FOUND if there is none.
        """

    def get_session_for_stream(self, stream_id: str) -> Session:
        """
        The session serving this stream.
        """

    def serve(
        self, stream: WireStream, mode: typing.Any = "accept"
    ) -> asyncio.Future[None]:
        """
        Serve a stream in the given mode ("start" or "accept").
        """

    def session_ids(self) -> list[str]:
        """
        The ids of the sessions currently being served.
        """

    def set_action_registry(
        self, action_registry: ActionRegistry | None
    ) -> None:
        """
        Replace the registry new connections are built from, without interrupting any stream.
        """

    def start(self, stream: WireStream) -> asyncio.Future[None]:
        """
        Serve a stream this side initiated, awaiting its whole lifetime.
        """

    def start_stream_handler(
        self, stream: WireStream, mode: typing.Any = "accept"
    ) -> Session:
        """
        Begin serving a stream and return its session immediately.
        """

    def stop_accepting(self) -> None:
        """
        Refuse new connections, leaving live ones alone.
        """

    def wait_done(self) -> asyncio.Future[None]:
        """
        Await the service being closed and empty.
        """

    @property
    def accepting(self) -> bool:
        """
        Whether new connections are still admitted.
        """

    @property
    def action_registry(self) -> ActionRegistry | None:
        """
        The template registry new connections are built from.
        """

    @property
    def done(self) -> _ServiceDoneEvent:
        """
        An `asyncio.Event`-shaped view of the service being closed and empty.

        Set once the service has stopped accepting *and* every session it was
        serving has finished.
        """

    @property
    def session_count(self) -> int:
        """
        How many sessions are being served.
        """

class ServiceOptions:
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        *,
        session_options: SessionOptions | None = None,
        copy_registry_per_connection: bool = False,
        session_headers: collections.abc.Mapping[str, bytes] | None = None,
        drain_timeout: Duration | None = None,
    ) -> None:
        """
        Construct service options; all parameters are keyword-only.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...
    def validate(self) -> None:
        """
        Validate the options, raising on error.
        """

    @property
    def copy_registry_per_connection(self) -> bool:
        """
        Give each connection its own copy of the registry. Leave false when the connection hook makes the copy itself.
        """
    @copy_registry_per_connection.setter
    def copy_registry_per_connection(self, arg0: bool) -> None: ...

    @property
    def drain_timeout(self) -> Duration:
        """
        How long draining waits for live sessions.
        """
    @drain_timeout.setter
    def drain_timeout(self, arg1: Duration | None) -> None: ...

    @property
    def session_headers(self) -> dict[str, bytes]:
        """
        Headers stamped on every session the service creates.
        """
    @session_headers.setter
    def session_headers(
        self, arg1: collections.abc.Mapping[str, bytes] | None
    ) -> None: ...

    @property
    def session_options(self) -> SessionOptions:
        """
        Limits and timeouts for every session created.
        """
    @session_options.setter
    def session_options(self, arg0: SessionOptions) -> None: ...

class Session:
    def __init__(
        self,
        session_id: str = "",
        on_stream_message: typing.Any | None = None,
        on_stream_done: typing.Any | None = None,
        headers: collections.abc.Mapping[str, bytes] | None = None,
        options: SessionOptions | None = None,
        node_map: NodeMap | None = None,
        action_registry: ActionRegistry | None = None,
    ) -> None:
        """
        Create an A11 session that multiplexes wire streams and actions. Streams deliver messages asynchronously to the optional on_stream_message and on_stream_done callbacks, which may be coroutines. This is the top-level object an agent drives to exchange wire messages and run actions.
        """

    def abort(self, status: Status) -> None:
        """
        Abort the session immediately with the given error status, cancelling streams and actions.

        Examples:
            Propagate an authentication failure to the peer:

            ```python
            session.abort(Status(
                code=StatusCode.PERMISSION_DENIED,
                message=str(error),
            ))
            ```
        """

    def actions(self) -> list[tuple[str, Action]]:
        """
        Return the (action_id, action) pairs currently running in the session. Actions execute asynchronously, so this is a point-in-time snapshot of in-flight work.
        """

    def add_done_callback(
        self,
        callback: typing.Callable[
            ["Session"], typing.Any | Awaitable[typing.Any]
        ],
    ) -> asyncio.Task:
        """
        Invoke ``callback(session)`` once this session fully completes.

        The callback fires exactly once when the session finishes -- whether it
        drained and closed cleanly, its deadline elapsed, or it was aborted
        ("dies") -- because every one of those paths resolves the completion
        view exposed by ``done``. If the session is already done, the callback
        still runs on the next event-loop iteration.

        A synchronous callback runs to completion; one returning an awaitable is
        awaited. This is the hook connection-scoped resources should register on
        so they are released when the session ends regardless of outcome (e.g.
        reaping the shells started within a session).

        Returns the scheduled ``asyncio.Task``. Must be called from within a
        running event loop.
        """

    def add_stream(
        self, stream: WireStream, mode: typing.Any = "start"
    ) -> asyncio.Future[None]:
        """
        Attach a wire stream and begin pumping its messages, returning an awaitable for the stream's lifetime. `mode` selects whether this side starts (`"start"`) or accepts (`"accept"`) the stream.

        Examples:
            Attach the client transport before exchanging messages:

            ```python
            stream_lifetime = session.add_stream(websocket_stream)
            ```
        """

    def await_all_actions(
        self, timeout: Duration | None = None
    ) -> asyncio.Future[None]:
        """
        Return an awaitable that resolves once all in-flight actions have finished, or the optional timeout elapses. Await this to synchronize on the session's outstanding asynchronous work before proceeding.
        """

    def cancel_action(self, action_id: str) -> None:
        """
        Request cancellation of the running action with the given id, raising if it is unknown. Cancellation is cooperative and completes asynchronously as the action unwinds.
        """

    def cancel_all_actions(self) -> None:
        """
        Request cancellation of every action currently running in the session. Each action unwinds asynchronously; await await_all_actions to observe completion.
        """

    def dispatch_action(self, action: typing.Any) -> asyncio.Future[None]:
        """
        Dispatch an already-constructed Action to run within the session, returning an awaitable for its handling.
        """

    def dispatch_action_message(
        self,
        action_message: ActionMessage,
        origin_stream: WireStream | None = None,
    ) -> asyncio.Future[None]:
        """
        Dispatch an action message, resolving it against the action registry and running the resulting action. Returns an awaitable that completes when the action has been handled; origin_stream attributes the message to a source stream.
        """

    def dispatch_node_fragment(
        self, fragment: NodeFragment
    ) -> asyncio.Future[int]:
        """
        Dispatch a node fragment into the session's NodeMap and return an awaitable resolving to the applied revision. Fragments are applied asynchronously in order, letting an agent stream incremental document updates.
        """

    def dispatch_wire_message(
        self, message: WireMessage, origin_stream: WireStream | None = None
    ) -> asyncio.Future[None]:
        """
        Route a wire message through the session as though it arrived on a stream, returning an awaitable for its processing. origin_stream optionally records which stream the message is attributed to.
        """

    def get_action(self, action_id: str) -> Action:
        """
        Look up a running action by its id, raising if none matches.
        """

    def get_action_registry(self) -> ActionRegistry | None:
        """
        Return the ActionRegistry used to resolve incoming action messages into runnable actions.
        """

    def get_id(self) -> str:
        """
        Return the session's unique identifier string.
        """

    def get_node_map(self) -> NodeMap:
        """
        Return the NodeMap backing this session's node state. Node fragments dispatched to the session are applied to this map as messages stream in.
        """

    def get_status(self) -> Status:
        """
        Return the session's terminal status, indicating whether it completed successfully or was aborted.
        """

    def get_stream(self, stream_id: str) -> WireStream:
        """
        Look up an attached stream by its id, raising if no such stream exists. Because streams come and go over the session's lifetime, guard against a stream having been removed since you last observed it.
        """

    def half_close(self) -> None:
        """
        Signal that this side will send no more messages, allowing the session to drain and finish once peers do the same. Remaining inbound messages continue to be processed asynchronously.

        Examples:
            Finish an exchange after sending the last message:

            ```python
            session.half_close()
            await session.done.wait()
            ```
        """

    def is_closed(self) -> bool:
        """
        Return whether the session has been closed and no longer accepts new streams or messages.
        """

    def is_done(self) -> bool:
        """
        Return whether the session has fully finished, including all streams and actions. Prefer awaiting done for asynchronous completion rather than polling this flag.
        """

    def send(self, message: WireMessage, stream_id: str = "") -> None:
        """
        Enqueue a wire message for delivery on the named stream (or the default stream), raising on failure. Delivery happens asynchronously as the stream drains.

        Examples:
            Route a response through a particular attached transport:

            ```python
            session.send(response, stream_id=websocket_stream.get_id())
            ```
        """

    def set_action_registry(self, registry: ActionRegistry | None) -> None:
        """
        Replace the ActionRegistry used to resolve incoming action messages, raising on failure. Active actions are rebound for later nested-name resolution; configure it before dispatch to avoid mixing registry versions.
        """

    def set_deadline(self, deadline: Time | None = None) -> None:
        """
        Set the absolute deadline after which the session is aborted; passing None clears it to no deadline. The session enforces this asynchronously as time passes.
        """

    def set_node_map(self, node_map: NodeMap) -> None:
        """
        Replace the NodeMap backing this session's node state, raising on failure. Active actions are rebound, but existing fragments are not migrated; set it before traffic to avoid splitting state between maps.
        """

    def streams(self) -> list[tuple[str, WireStream]]:
        """
        Return the (stream_id, stream) pairs currently attached to the session. Streams are added and removed asynchronously as peers connect and disconnect, so treat the result as a snapshot taken at call time.
        """

    def wait_done(self) -> asyncio.Future[None]:
        """
        Return an awaitable that resolves when the session has fully finished. Await this to block until every stream and action has completed asynchronously.
        """

    @property
    def action_registry(self) -> ActionRegistry | None:
        """
        The ActionRegistry used to resolve action messages; assigning replaces it.
        """
    @action_registry.setter
    def action_registry(self, arg1: ActionRegistry) -> None: ...

    @property
    def deadline(self) -> Time:
        """
        The absolute time after which the session will be aborted.
        """

    @property
    def done(self) -> _DoneEvent:
        """
        An `asyncio.Event`-shaped view of full session completion.

        `Session.is_closed` can become true as soon as shutdown starts. Await
        this event (or ``wait_done``) when streams and actions must all have
        released their runtime state.
        """

    @property
    def id(self) -> str:
        """
        The session's unique identifier string.
        """

    @property
    def node_map(self) -> NodeMap:
        """
        The NodeMap backing this session's node state; assigning replaces it.
        """
    @node_map.setter
    def node_map(self, arg1: NodeMap) -> None: ...

class SessionOptions:
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        *,
        max_buffered_messages_total: typing.SupportsInt | None = 256,
        max_buffered_messages_per_stream: typing.SupportsInt | None = 32,
        max_concurrent_root_actions: typing.SupportsInt | None = 32,
        max_concurrent_nested_actions: typing.SupportsInt | None = 128,
        max_single_message_size: typing.SupportsInt | None = 33554432,
        max_buffered_bytes_total: typing.SupportsInt | None = 33554432,
        max_buffered_bytes_per_stream: typing.SupportsInt | None = 4194304,
        no_stream_timeout: Duration | None = None,
        deadline: Time | None = None,
    ) -> None:
        """
        Construct session limits and timeouts; all parameters are keyword-only.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...
    def validate(self) -> None:
        """
        Validate the option values, raising on invalid configuration.
        """

    @property
    def deadline(self) -> Time:
        """
        Absolute time after which the session is aborted.
        """
    @deadline.setter
    def deadline(self, arg1: typing.Any) -> None: ...

    @property
    def max_buffered_bytes_per_stream(self) -> int:
        """
        Maximum bytes buffered per stream.
        """
    @max_buffered_bytes_per_stream.setter
    def max_buffered_bytes_per_stream(
        self, arg0: typing.SupportsInt
    ) -> None: ...

    @property
    def max_buffered_bytes_total(self) -> int:
        """
        Maximum total bytes buffered across all streams.
        """
    @max_buffered_bytes_total.setter
    def max_buffered_bytes_total(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def max_buffered_messages_per_stream(self) -> int:
        """
        Maximum number of messages buffered per stream.
        """
    @max_buffered_messages_per_stream.setter
    def max_buffered_messages_per_stream(
        self, arg0: typing.SupportsInt
    ) -> None: ...

    @property
    def max_buffered_messages_total(self) -> int:
        """
        Maximum number of messages buffered across all streams.
        """
    @max_buffered_messages_total.setter
    def max_buffered_messages_total(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def max_concurrent_nested_actions(self) -> int:
        """
        Maximum number of concurrently running nested actions.
        """
    @max_concurrent_nested_actions.setter
    def max_concurrent_nested_actions(
        self, arg0: typing.SupportsInt
    ) -> None: ...

    @property
    def max_concurrent_root_actions(self) -> int:
        """
        Maximum number of concurrently running root actions.
        """
    @max_concurrent_root_actions.setter
    def max_concurrent_root_actions(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def max_single_message_size(self) -> int:
        """
        Maximum size in bytes of a single wire message.
        """
    @max_single_message_size.setter
    def max_single_message_size(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def no_stream_timeout(self) -> Duration:
        """
        How long the session waits with no active stream before finishing.
        """
    @no_stream_timeout.setter
    def no_stream_timeout(self, arg1: typing.Any) -> None: ...

class SessionWithRecv(Session):
    def __init__(
        self,
        session_id: str = "",
        headers: collections.abc.Mapping[str, bytes] | None = None,
        options: SessionOptions | None = None,
        node_map: NodeMap | None = None,
        action_registry: ActionRegistry | None = None,
    ) -> None:
        """
        Create a session that buffers inbound messages for explicit pull-based reception instead of callbacks. receive and receive_with_stream_id await messages as they stream in.
        """

    async def receive(self, deadline=None):
        """
        Await the next inbound message, or ``None`` when the session ends.

        Use this when one receive loop handles every attached stream. Choose
        `receive_with_stream_id` when replies or diagnostics must retain their
        transport identity. The optional absolute deadline limits only this
        wait; it does not change the session deadline.

        Examples:
            Route messages from a session with one attached transport:

            ```python
            while message := await session.receive():
                await route_message(message)
            ```
        """

    async def receive_with_stream_id(self, deadline=None):
        """
        Await ``(message, stream_id)``, or ``None`` after completion.

        This is the pull-style counterpart to ``OnSessionStreamMessage`` and
        is useful when an agent multiplexes several transports in one loop.

        Examples:
            Preserve the source while routing gateway traffic:

            ```python
            while item := await session.receive_with_stream_id():
                message, stream_id = item
                await route_message(message, source=stream_id)
            ```
        """

class SignallingEndpoint(SignallingTransport):
    pass

class SignallingMessage:
    @staticmethod
    def from_json(json: str) -> SignallingMessage:
        """
        Parse a signalling message from its JSON wire representation.
        """

    def __init__(
        self,
        type: SignallingMessageType = ...,
        sender: str = "",
        recipient: str = "",
        description: str = "",
        description_type: str = "",
        candidate: str = "",
        mid: str = "",
        error: typing.Any | None = None,
    ) -> None:
        """
        Construct a signalling message describing an SDP offer/answer, an ICE candidate, or an error.
        """

    def to_json(self) -> str:
        """
        Serialize this message to its JSON wire representation.
        """

    def validate(self) -> None:
        """
        Raise if the message fields are inconsistent for its type.
        """

    @property
    def candidate(self) -> str:
        """
        ICE candidate string (for CANDIDATE messages).
        """
    @candidate.setter
    def candidate(self, arg0: str) -> None: ...

    @property
    def description(self) -> str:
        """
        SDP description body (for DESCRIPTION messages).
        """
    @description.setter
    def description(self, arg0: str) -> None: ...

    @property
    def description_type(self) -> str:
        """
        SDP description type, e.g. "offer" or "answer".
        """
    @description_type.setter
    def description_type(self, arg0: str) -> None: ...

    @property
    def error(self) -> Status:
        """
        Error status carried by ERROR messages, or OK otherwise.
        """
    @error.setter
    def error(self, arg1: Status) -> None: ...

    @property
    def mid(self) -> str:
        """
        Media stream identifier associated with the candidate.
        """
    @mid.setter
    def mid(self, arg0: str) -> None: ...

    @property
    def recipient(self) -> str:
        """
        Identity of the peer this message is addressed to.
        """
    @recipient.setter
    def recipient(self, arg0: str) -> None: ...

    @property
    def sender(self) -> str:
        """
        Identity of the peer that sent this message.
        """
    @sender.setter
    def sender(self, arg0: str) -> None: ...

    @property
    def type(self) -> SignallingMessageType:
        """
        Kind of signalling payload this message carries.
        """
    @type.setter
    def type(self, arg0: SignallingMessageType) -> None: ...

class SignallingMessageType:
    """
    Members:

    DESCRIPTION

    CANDIDATE

    ERROR
    """

    CANDIDATE: typing.ClassVar[SignallingMessageType]
    DESCRIPTION: typing.ClassVar[SignallingMessageType]
    ERROR: typing.ClassVar[SignallingMessageType]
    __members__: typing.ClassVar[dict[str, SignallingMessageType]]
    def __eq__(self, other: object) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: typing.SupportsInt) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: typing.SupportsInt) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class SignallingService:
    @staticmethod
    def create() -> SignallingService:
        """
        Create a new in-process signalling service.
        """

    def __contains__(self, identity: str) -> bool:
        """
        Return whether the given identity is currently connected.
        """

    def __enter__(self) -> SignallingService: ...
    def __exit__(self, exc_type, exc, traceback) -> None: ...
    def __init__(self) -> None:
        """
        Create a new in-process signalling service.
        """

    def connect(
        self, identity: str, on_message: typing.Any
    ) -> SignallingEndpoint:
        """
        Register an identity and its async inbound-message callback, returning a signalling endpoint.
        """

    def contains(self, identity: str) -> bool:
        """
        Return whether the given identity is currently connected.
        """

    def identities(self) -> list[str]:
        """
        Return the list of currently connected identities.
        """

    def stop(self) -> None:
        """
        Stop the service and disconnect all endpoints.
        """

class SignallingTransport:
    def __enter__(self) -> SignallingTransport: ...
    def __exit__(self, exc_type, exc, traceback) -> None: ...
    def __init__(self) -> None:
        """
        Construct a base signalling transport.
        """

    def close(self) -> None:
        """
        Close the transport and release its resources.
        """

    def connected(self) -> bool:
        """
        Return whether the transport is currently connected.
        """

    def get_status(self) -> Status:
        """
        Return the current transport status.
        """

    def identity(self) -> str:
        """
        Return the local identity bound to this transport.
        """

    def send(self, message: SignallingMessage) -> None:
        """
        Send a signalling message to the peer (non-blocking).
        """

    def set_on_message(self, callback: typing.Any) -> None:
        """
        Register an async callback invoked for each inbound message.
        """

class SpeechRecognizer:
    """
    Restartable local automatic speech recognizer backed by whisper.cpp. A cheap energy gate endpoints utterances so silence never reaches the decoder; a Silero VAD model, when configured, then filters each utterance to genuine speech before inference.
    """

    @staticmethod
    def create(
        model: str | os.PathLike[str],
        source: AudioInput | AudioSubscription | None = None,
        options: (
            SpeechRecognizerOptions
            | collections.abc.Mapping[str, typing.Any]
            | None
        ) = None,
    ) -> SpeechRecognizer:
        """
        Load a model and construct a recognizer.
        """

    def __init__(
        self,
        model: str | os.PathLike[str],
        source: AudioInput | AudioSubscription | None = None,
        options: (
            SpeechRecognizerOptions
            | collections.abc.Mapping[str, typing.Any]
            | None
        ) = None,
    ) -> None:
        """
        Load a whisper.cpp GGML/GGUF model for local recognition.

        Pass an :class:`AudioInput` to create a fresh subscription per run, an
        :class:`AudioSubscription` to consume that subscription once, or
        ``None`` to use the system default input. Loading happens once, so the
        same recognizer can be paused while an agent responds and restarted for
        the next user turn without reloading model weights.
        """

    def get_status(self) -> Status:
        """
        Return the current or final recognition status.
        """

    async def start(
        self, on_transcription: OnTranscription, on_done: OnRecognitionDone
    ) -> None:
        """
        Start capture and deliver awaited transcription callbacks.

        ``on_transcription`` receives non-empty pieces as speech endpoints are
        decoded, then exactly one ``None``. ``on_done`` follows that terminal
        marker. Silence does not invoke whisper or produce a piece.
        """

    async def stop(self) -> None:
        """
        Stop capture and await the terminal piece and done callbacks.
        """

    @property
    def model(self) -> str:
        """
        Path of the loaded whisper model.
        """

    @property
    def options(self) -> SpeechRecognizerOptions:
        """
        The validated native options used by this recognizer.
        """

    @property
    def running(self) -> bool:
        """
        Whether a recognition run is active.
        """

class SpeechRecognizerOptions:
    """
    Configuration for whisper.cpp transcription, a cheap energy VAD gate, and optional whisper.cpp Silero neural VAD.
    """

    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        model: str = "",
        language: str = "auto",
        translate: bool = False,
        inference_threads: typing.SupportsInt = 0,
        use_gpu: bool = True,
        flash_attention: bool = True,
        use_context: bool = False,
        initial_prompt: str = "",
        subscription_buffer_millis: typing.SupportsInt = 100,
        vad_threshold: typing.SupportsFloat = 0.009999999776482582,
        vad_noise_ratio: typing.SupportsFloat = 2.5,
        vad_window_millis: typing.SupportsInt = 20,
        min_speech_millis: typing.SupportsInt = 250,
        min_silence_millis: typing.SupportsInt = 600,
        speech_pad_millis: typing.SupportsInt = 160,
        max_speech_seconds: typing.SupportsInt = 30,
        vad_model: str = "",
        silero_threshold: typing.SupportsFloat = 0.5,
    ) -> None:
        """
        Construct validated speech recognition options.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...

    @property
    def flash_attention(self) -> bool:
        """
        Use flash attention when supported.
        """
    @flash_attention.setter
    def flash_attention(self, arg0: bool) -> None: ...

    @property
    def inference_threads(self) -> int:
        """
        Decoder threads, or zero for the bounded default.
        """
    @inference_threads.setter
    def inference_threads(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def initial_prompt(self) -> str:
        """
        Optional initial decoder prompt.
        """
    @initial_prompt.setter
    def initial_prompt(self, arg0: str) -> None: ...

    @property
    def language(self) -> str:
        """
        Whisper language code, or 'auto'.
        """
    @language.setter
    def language(self, arg0: str) -> None: ...

    @property
    def max_speech_seconds(self) -> int:
        """
        Maximum utterance duration before splitting.
        """
    @max_speech_seconds.setter
    def max_speech_seconds(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def min_silence_millis(self) -> int:
        """
        Silence needed to endpoint speech.
        """
    @min_silence_millis.setter
    def min_silence_millis(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def min_speech_millis(self) -> int:
        """
        Minimum voiced duration accepted.
        """
    @min_speech_millis.setter
    def min_speech_millis(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def model(self) -> str:
        """
        Path to the whisper.cpp model; used by the transcription action (empty is rejected there).
        """
    @model.setter
    def model(self, arg0: str) -> None: ...

    @property
    def silero_threshold(self) -> float:
        """
        Silero speech-probability threshold in (0, 1].
        """
    @silero_threshold.setter
    def silero_threshold(self, arg0: typing.SupportsFloat) -> None: ...

    @property
    def speech_pad_millis(self) -> int:
        """
        Audio retained around an utterance.
        """
    @speech_pad_millis.setter
    def speech_pad_millis(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def subscription_buffer_millis(self) -> int:
        """
        Duration of internally-created capture buffers.
        """
    @subscription_buffer_millis.setter
    def subscription_buffer_millis(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def translate(self) -> bool:
        """
        Translate speech to English.
        """
    @translate.setter
    def translate(self, arg0: bool) -> None: ...

    @property
    def use_context(self) -> bool:
        """
        Carry decoder context between utterances.
        """
    @use_context.setter
    def use_context(self, arg0: bool) -> None: ...

    @property
    def use_gpu(self) -> bool:
        """
        Use a compiled GPU backend when available.
        """
    @use_gpu.setter
    def use_gpu(self, arg0: bool) -> None: ...

    @property
    def vad_model(self) -> str:
        """
        Path to a Silero VAD model; empty disables Silero VAD.
        """
    @vad_model.setter
    def vad_model(self, arg0: str) -> None: ...

    @property
    def vad_noise_ratio(self) -> float:
        """
        Speech threshold relative to learned noise.
        """
    @vad_noise_ratio.setter
    def vad_noise_ratio(self, arg0: typing.SupportsFloat) -> None: ...

    @property
    def vad_threshold(self) -> float:
        """
        Absolute RMS speech threshold.
        """
    @vad_threshold.setter
    def vad_threshold(self, arg0: typing.SupportsFloat) -> None: ...

    @property
    def vad_window_millis(self) -> int:
        """
        RMS analysis window duration.
        """
    @vad_window_millis.setter
    def vad_window_millis(self, arg0: typing.SupportsInt) -> None: ...

class SseOutboundDelivery:
    """
    How an SSE client delivers its outbound WireMessages. Servers accept either; only the client chooses.

    Members:

      POST : One HTTP POST per message, issued concurrently up to max_concurrent_posts. Reachable from anything that can fetch().

      STREAM : One long-lived request body carrying every message: HTTP/2 DATA frames or an HTTP/1.1 chunked body. Removes the one-request-per-message ceiling; falls back to POST against a server that does not advertise it.
    """

    POST: typing.ClassVar[SseOutboundDelivery]
    STREAM: typing.ClassVar[SseOutboundDelivery]
    __members__: typing.ClassVar[dict[str, SseOutboundDelivery]]
    def __eq__(self, other: object) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: typing.SupportsInt) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: typing.SupportsInt) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class Status:
    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @staticmethod
    def from_exception(
        exc: BaseException,
        casters: a11.status.StatusExceptionCasters | None = None,
    ) -> Status:
        """
        Convert an application exception to a transportable status.

        Existing `StatusException` values retain their structured status;
        registered casters handle framework-specific types, and unknown
        exceptions become ``UNKNOWN``.
        """

    @staticmethod
    def from_http_exception(
        http_exception: (
            fastapi.exceptions.HTTPException | httpx.HTTPStatusError
        ),
    ) -> Status:
        """
        Convert a FastAPI/httpx HTTP exception to an A11 status.
        """

    @staticmethod
    def get_fastapi_response_dict_for_codes(
        *codes: a11.status.StatusCode,
    ) -> dict[int, dict]:
        """
        Build FastAPI response documentation for portable status codes.
        """

    @staticmethod
    def get_fastapi_response_dict_for_http_codes(
        *codes: int,
    ) -> dict[int, dict]:
        """
        Build FastAPI response documentation for explicit HTTP codes.
        """

    @staticmethod
    def ok(message: str | None = None) -> Status:
        """
        Create a successful status with an optional descriptive message.
        """

    @staticmethod
    def parse_from_json(data: str | bytes) -> a11.status.StatusParseResult:
        """
        Parse status JSON and return validation state without throwing.
        """

    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_json_schema(
        cls, **kwargs: typing.Any
    ) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any) -> Status: ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, right: object) -> bool:
        """
        Returns whether two statuses have equal code, message, and details.
        """

    def __init__(
        self,
        code: typing.SupportsInt = 0,
        message: str = "OK",
        details: typing.Any = [],
    ) -> None:
        """
        Creates a status from a canonical code, message, and details list.
        """

    def __repr__(self) -> str:
        """
        Returns a debug representation of the status.
        """

    def __str__(self) -> str:
        """
        Returns a 'CODE: message' string form of the status.
        """

    def _as_dict(self) -> dict[str, typing.Any]:
        """
        Returns the status as a JSON-compatible dict.
        """

    def _copy(self) -> Status:
        """
        Returns a copy of this status.
        """

    def is_ok(self) -> bool:
        """
        Returns whether the status is OK (no error).
        """

    def model_copy(
        self, *, update: dict[str, typing.Any] | None = None, deep: bool = False
    ) -> Status: ...
    def model_dump(
        self, *, mode: str = "python", **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def raise_if_not_ok(self) -> None:
        """
        Raise `StatusException` when this status is non-OK.
        """

    def to_exception(self) -> a11.status.StatusException:
        """
        Convert a non-OK status to its Python boundary exception.
        """

    def to_msgpack(self, packer: msgpack._cmsgpack.Packer) -> None:
        """
        Append this status to an A11 MessagePack encoder.
        """

    @property
    def code(self) -> a11.status.StatusCode:
        """
        The canonical status code.
        """
    @code.setter
    def code(self, arg1: typing.SupportsInt) -> None: ...

    @property
    def details(self) -> list[typing.Any]:
        """
        The structured status details, as a list.
        """
    @details.setter
    def details(self, arg1: list[typing.Any]) -> None: ...

    @property
    def message(self) -> str:
        """
        The human-readable status message.
        """
    @message.setter
    def message(self, arg1: str) -> None: ...

class StreamMode:
    """
    Members:

    START

    ACCEPT
    """

    ACCEPT: typing.ClassVar[StreamMode]
    START: typing.ClassVar[StreamMode]
    __members__: typing.ClassVar[dict[str, StreamMode]]
    def __eq__(self, other: object) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: typing.SupportsInt) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: typing.SupportsInt) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class Time:
    @staticmethod
    def _infinite_future() -> Time:
        """
        Returns the infinite-future time.
        """

    @staticmethod
    def _infinite_past() -> Time:
        """
        Returns the infinite-past time.
        """

    @staticmethod
    def _now() -> Time:
        """
        Returns the current time.
        """

    @staticmethod
    def from_nanoseconds_since_epoch(nanoseconds: typing.SupportsInt) -> Time:
        """
        Creates a time from nanoseconds since the Unix epoch.
        """

    def __add__(self, duration: Duration) -> Time:
        """
        Returns the time shifted forward by a duration.
        """

    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, right: object) -> bool:
        """
        Returns whether two times are equal.
        """

    def __hash__(self) -> int:
        """
        Returns a hash of the time.
        """

    def __init__(self, nanoseconds_since_epoch: typing.SupportsInt) -> None:
        """
        Creates a time from nanoseconds since the Unix epoch.
        """

    def __le__(self, right: Time) -> bool:
        """
        Returns whether this time is at or before another.
        """

    def __lt__(self, right: Time) -> bool:
        """
        Returns whether this time is before another.
        """

    def __repr__(self) -> str:
        """
        Returns a debug representation of the time.
        """

    def __sub__(self, other: typing.Any) -> Time | Duration:
        """
        Returns the duration between two times, or a time shifted back by a duration.
        """

    @property
    def nanoseconds_since_epoch(self) -> int:
        """
        The time as nanoseconds since the Unix epoch.
        """

class TranscriptionEvent:
    """
    A capture/inference lifecycle notification from capture_transcription.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def capture_started() -> TranscriptionEvent: ...
    @staticmethod
    def capture_stopped() -> TranscriptionEvent: ...
    @staticmethod
    def inference_started() -> TranscriptionEvent: ...
    @staticmethod
    def inference_stopped() -> TranscriptionEvent: ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(self, kind: str = "capture_started") -> None:
        """
        Construct a transcription event.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...

    @property
    def kind(self) -> str:
        """
        The event kind, e.g. 'inference_started'.
        """
    @kind.setter
    def kind(self, arg1: str) -> None: ...

class TurnRelayType:
    """
    Members:

    UDP

    TCP

    TLS
    """

    TCP: typing.ClassVar[TurnRelayType]
    TLS: typing.ClassVar[TurnRelayType]
    UDP: typing.ClassVar[TurnRelayType]
    __members__: typing.ClassVar[dict[str, TurnRelayType]]
    def __eq__(self, other: object) -> bool: ...
    def __getstate__(self) -> int: ...
    def __hash__(self) -> int: ...
    def __index__(self) -> int: ...
    def __init__(self, value: typing.SupportsInt) -> None: ...
    def __int__(self) -> int: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...
    def __setstate__(self, state: typing.SupportsInt) -> None: ...
    def __str__(self) -> str: ...
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class TurnServer:
    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @staticmethod
    def from_string(value: str) -> TurnServer:
        """
        Parse a TURN server from a URL-like string.
        """

    def __eq__(self, arg0: object) -> bool:
        """
        Return whether two TURN server configurations are equal.
        """

    def __init__(self) -> None:
        """
        Construct an empty TURN server configuration.
        """

    @property
    def hostname(self) -> str:
        """
        TURN server hostname.
        """
    @hostname.setter
    def hostname(self, arg0: str) -> None: ...

    @property
    def password(self) -> str:
        """
        Password used to authenticate with the TURN server.
        """
    @password.setter
    def password(self, arg0: str) -> None: ...

    @property
    def port(self) -> int:
        """
        TURN server port (default 3478).
        """
    @port.setter
    def port(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def relay_type(self) -> TurnRelayType:
        """
        Transport used to reach the TURN server (UDP/TCP/TLS).
        """
    @relay_type.setter
    def relay_type(self, arg0: TurnRelayType) -> None: ...

    @property
    def username(self) -> str:
        """
        Username used to authenticate with the TURN server.
        """
    @username.setter
    def username(self, arg0: str) -> None: ...

class WebRtcConfiguration:
    def __init__(self) -> None:
        """
        Construct a default WebRTC configuration.
        """

    def validate(self) -> None:
        """
        Raise if the configuration is invalid.
        """

    @property
    def bind_address(self) -> str | None:
        """
        Optional local address to bind ICE sockets to.
        """
    @bind_address.setter
    def bind_address(self, arg0: str | None) -> None: ...

    @property
    def channel_split_size(self) -> int:
        """
        Size at which A11 fragments large logical messages.
        """
    @channel_split_size.setter
    def channel_split_size(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def desired_channels(self) -> int:
        """
        Number of WebRTC data channels a dialing client opens per connection and keeps replenished. Streaming with several channels lets slow per-channel acknowledgement round-trips overlap; the stream still behaves as one ordered, reliable channel. Defaults to 8. Has no effect on the accepting side.
        """
    @desired_channels.setter
    def desired_channels(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def enable_ice_udp_mux(self) -> bool:
        """
        Whether to multiplex ICE traffic over a single UDP port.
        """
    @enable_ice_udp_mux.setter
    def enable_ice_udp_mux(self, arg0: bool) -> None: ...

    @property
    def max_channels(self) -> int:
        """
        Maximum number of WebRTC data channels an accepting server admits per peer connection. Surplus channels a client opens beyond this are refused. Defaults to 8. Has no effect on the dialing side.
        """
    @max_channels.setter
    def max_channels(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def max_message_size(self) -> int | None:
        """
        Advertised local libdatachannel message size ceiling.
        """
    @max_message_size.setter
    def max_message_size(self, arg0: typing.SupportsInt | None) -> None: ...

    @property
    def preferred_port_range(self) -> tuple[int, int] | None:
        """
        Optional (min, max) local port range for ICE.
        """
    @preferred_port_range.setter
    def preferred_port_range(
        self, arg0: tuple[typing.SupportsInt, typing.SupportsInt] | None
    ) -> None: ...

    @property
    def stun_servers(self) -> list[str]:
        """
        List of STUN server URLs used for ICE.
        """
    @stun_servers.setter
    def stun_servers(self, arg0: collections.abc.Sequence[str]) -> None: ...

    @property
    def turn_servers(self) -> list[TurnServer]:
        """
        List of TURN servers used to relay ICE traffic.
        """
    @turn_servers.setter
    def turn_servers(
        self, arg0: collections.abc.Sequence[TurnServer]
    ) -> None: ...

class WebRtcWireServer:
    @staticmethod
    def create(
        identity: str,
        signalling: SignallingService,
        on_stream: typing.Any,
        configuration: WebRtcConfiguration = ...,
        stream_options: WireStreamOptions = ...,
    ) -> WebRtcWireServer:
        """
        Create a WebRTC server that accepts peer connections and invokes the async on_stream callback with each new WebRtcWireStream.
        """

    def __enter__(self) -> WebRtcWireServer: ...
    def __exit__(self, exc_type, exc, traceback) -> None: ...
    def stop(self) -> None:
        """
        Stop the server and stop accepting new peer connections.
        """

    @property
    def identity(self) -> str:
        """
        Local identity this server listens as.
        """

    @property
    def pending_peer_count(self) -> int:
        """
        Number of peers still completing negotiation.
        """

    @property
    def running(self) -> bool:
        """
        Whether the server is currently running.
        """

    @property
    def signalling_endpoint(self) -> SignallingEndpoint:
        """
        Signalling endpoint the server negotiates over.
        """

class WebRtcWireStream(WireStream):
    @staticmethod
    @typing.overload
    def create_client(
        identity: str,
        peer_identity: str,
        signalling: SignallingService,
        configuration: WebRtcConfiguration = ...,
        options: WireStreamOptions = ...,
    ) -> WebRtcWireStream:
        """
        Open a WebRTC data-channel wire stream to a named peer over a shared in-process signalling service. It performs the ICE/SDP handshake and resolves to a WireStream carrying A11-framed messages, fragmenting large payloads transparently.
        """
    @staticmethod
    @typing.overload
    def create_client(
        peer_identity: str,
        signalling: SignallingTransport,
        configuration: WebRtcConfiguration = ...,
        options: WireStreamOptions = ...,
    ) -> WebRtcWireStream:
        """
        Open a WebRTC data-channel wire stream to a peer over an explicit signalling transport (e.g. a WebSocket signalling client). Prefer this overload when your agent reaches a peer across a network via a remote signalling server rather than an in-process service. The call drives the asynchronous ICE/SDP negotiation and yields a WireStream for streaming logical messages once connected.
        """

    @property
    def data_channel(self) -> typing_extensions.CapsuleType | None:
        """
        Opaque capsule around the underlying libdatachannel DataChannel. Exposed for advanced interop and diagnostics; agent code normally reads and writes through the WireStream API rather than touching this directly.
        """

    @property
    def peer_connection(self) -> typing_extensions.CapsuleType | None:
        """
        Opaque capsule around the underlying libdatachannel PeerConnection, for inspecting ICE/connection state during debugging.
        """

    @property
    def signalling_endpoint(self) -> SignallingTransport:
        """
        Signalling transport this stream negotiated over, the channel that carried the asynchronous SDP/ICE handshake.
        """

class WebSocketClientOptions:
    def __init__(self) -> None:
        """
        Construct default WebSocket client options.
        """

    def validate(self) -> None:
        """
        Validate the client options, raising on invalid configuration.
        """

    @property
    def framing(self) -> ChannelFramingOptions:
        """
        Channel framing options controlling message splitting and buffering.
        """
    @framing.setter
    def framing(self, arg0: ChannelFramingOptions) -> None: ...

    @property
    def headers(self) -> list[tuple[str, str]]:
        """
        Extra HTTP headers sent on the WebSocket handshake, as a list of (name, value) string pairs.
        """
    @headers.setter
    def headers(
        self, arg1: collections.abc.Mapping[str, str] | list[tuple[str, str]]
    ) -> None: ...

    @property
    def http2_options(self) -> Http2Options:
        """
        HTTP/2 transport options for the client connection.
        """
    @http2_options.setter
    def http2_options(self, arg0: Http2Options) -> None: ...

class WebSocketServerOptions:
    def __init__(self) -> None:
        """
        Construct default WebSocket server options.
        """

    def validate(self) -> None:
        """
        Validate the server options, raising on invalid configuration.
        """

    @property
    def bind_address(self) -> str:
        """
        Local address the server binds to.
        """
    @bind_address.setter
    def bind_address(self, arg0: str) -> None: ...

    @property
    def enable_tls(self) -> bool:
        """
        Whether TLS is enabled (mirrors http2_options.tls.enabled).
        """
    @enable_tls.setter
    def enable_tls(self, arg1: bool) -> None: ...

    @property
    def framing(self) -> ChannelFramingOptions:
        """
        Channel framing options for accepted streams.
        """
    @framing.setter
    def framing(self, arg0: ChannelFramingOptions) -> None: ...

    @property
    def http2_options(self) -> Http2Options:
        """
        HTTP/2 transport options, including TLS settings.
        """
    @http2_options.setter
    def http2_options(self, arg0: Http2Options) -> None: ...

    @property
    def path(self) -> str:
        """
        URL path on which the server accepts WebSocket connections.
        """
    @path.setter
    def path(self, arg0: str) -> None: ...

    @property
    def port(self) -> int:
        """
        TCP port to listen on; 0 selects an ephemeral port.
        """
    @port.setter
    def port(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def stream_options(self) -> WireStreamOptions:
        """
        Default WireStreamOptions applied to each accepted stream.
        """
    @stream_options.setter
    def stream_options(self, arg0: WireStreamOptions) -> None: ...

class WebSocketSignallingClient(SignallingTransport):
    @staticmethod
    def connect(
        url: str,
        identity: str,
        on_message: typing.Any | None = None,
        options: WebSocketSignallingClientOptions = ...,
    ) -> asyncio.Future[WebSocketSignallingClient]:
        """
        Asynchronously connect to a WebSocket signalling server, resolving to a client once registered under the given identity.
        """

    def get_impl(self) -> typing_extensions.CapsuleType | None:
        """
        Opaque capsule around the native implementation, for interop.
        """

class WebSocketSignallingClientOptions:
    def __init__(self) -> None:
        """
        Construct default WebSocket signalling client options.
        """

    def validate(self) -> None:
        """
        Raise if the options are invalid.
        """

    @property
    def deadline(self) -> Time:
        """
        Deadline by which the connection must be established.
        """
    @deadline.setter
    def deadline(self, arg1: Time | None) -> None: ...

    @property
    def http2_options(self) -> Http2Options:
        """
        HTTP/2 transport options used for the connection.
        """
    @http2_options.setter
    def http2_options(self, arg0: Http2Options) -> None: ...

    @property
    def max_message_size(self) -> int:
        """
        Maximum inbound signalling message size in bytes.
        """
    @max_message_size.setter
    def max_message_size(self, arg0: typing.SupportsInt) -> None: ...

class WebSocketSignallingServer:
    @staticmethod
    def create(
        service: SignallingService,
        options: WebSocketSignallingServerOptions = ...,
    ) -> WebSocketSignallingServer:
        """
        Create a WebSocket signalling server that fronts the given in-process signalling service.
        """

    def __enter__(self) -> WebSocketSignallingServer: ...
    def __exit__(self, exc_type, exc, traceback) -> None: ...
    def get_impl(self) -> typing_extensions.CapsuleType | None:
        """
        Opaque capsule around the native implementation, for interop.
        """

    def stop(self) -> None:
        """
        Stop the server and close all client connections.
        """

    @property
    def port(self) -> int:
        """
        Port the server is listening on.
        """

    @property
    def running(self) -> bool:
        """
        Whether the server is currently running.
        """

    @property
    def service(self) -> SignallingService:
        """
        Signalling service this server fronts.
        """

class WebSocketSignallingServerOptions:
    def __init__(self) -> None:
        """
        Construct default WebSocket signalling server options.
        """

    def validate(self) -> None:
        """
        Raise if the options are invalid.
        """

    @property
    def bind_address(self) -> str:
        """
        Local address to bind the listening socket to.
        """
    @bind_address.setter
    def bind_address(self, arg0: str) -> None: ...

    @property
    def enable_tls(self) -> bool:
        """
        Whether TLS is enabled for the server transport.
        """
    @enable_tls.setter
    def enable_tls(self, arg1: bool) -> None: ...

    @property
    def http2_options(self) -> Http2Options:
        """
        HTTP/2 transport options used for the server.
        """
    @http2_options.setter
    def http2_options(self, arg0: Http2Options) -> None: ...

    @property
    def max_message_size(self) -> int:
        """
        Maximum inbound signalling message size in bytes.
        """
    @max_message_size.setter
    def max_message_size(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def path_prefix(self) -> str:
        """
        URL path prefix the server listens on.
        """
    @path_prefix.setter
    def path_prefix(self, arg0: str) -> None: ...

    @property
    def port(self) -> int:
        """
        TCP port to listen on (0 selects an ephemeral port).
        """
    @port.setter
    def port(self, arg0: typing.SupportsInt) -> None: ...

class WebSocketWireServer:
    @staticmethod
    def create(
        on_stream: typing.Any, options: WebSocketServerOptions = ...
    ) -> WebSocketWireServer:
        """
        Start a WebSocket server that accepts incoming A11 connections, invoking the asynchronous on_stream callback with a fresh WireStream for each accepted client. This is the server-side entry point for hosting an agent: each callback runs concurrently and typically drives accept() on its stream. Configure the listen address, port, path and TLS via options.
        """

    def __enter__(self) -> WebSocketWireServer: ...
    def __exit__(self, exc_type, exc, traceback) -> None: ...
    def get_impl(self) -> typing_extensions.CapsuleType | None:
        """
        Return an opaque native handle to the underlying implementation, or None. Intended for advanced interop.
        """

    def stop(self) -> None:
        """
        Stop the server and close the listening socket, releasing the bound port. Call this to shut the agent host down cleanly; it blocks until shutdown completes.
        """

    @property
    def port(self) -> int:
        """
        The actual TCP port the server is listening on, resolved even when an ephemeral port (0) was requested.
        """

    @property
    def running(self) -> bool:
        """
        Whether the server is currently accepting connections.
        """

class WebSocketWireStream(WireStream):
    @staticmethod
    def connect(
        url: str,
        options: WireStreamOptions = ...,
        websocket_options: WebSocketClientOptions = ...,
    ) -> WebSocketWireStream:
        """
        Open a client WebSocket connection to url and return a WireStream over it. This is the standard way for an agent to dial out to a remote A11 endpoint; the returned stream is then driven asynchronously via start()/send(). Tune transport buffering with options and the handshake (headers, framing, HTTP/2, TLS) with websocket_options.
        """

class WireMessage:
    """
    A wire-format message bundling node fragments and actions.
    """

    VERSION: typing.ClassVar[int] = 1
    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    @staticmethod
    def from_json(value: str) -> WireMessage:
        """
        Deserialize a message from its JSON wire encoding.
        """

    @staticmethod
    def from_msgpack(data: typing.Any) -> WireMessage:
        """
        Deserialize a value from MessagePack bytes.
        """

    @classmethod
    def __get_pydantic_core_schema__(cls, _source_type, _handler): ...
    @classmethod
    def __get_pydantic_json_schema__(cls, _schema, _handler): ...
    @classmethod
    def model_construct(cls, **values: typing.Any):
        """
        Construct a native value from trusted field values.

        Native records retain C++ invariants. This validates input rather than
        creating an invalid object.
        """

    @classmethod
    def model_json_schema(cls, **_: typing.Any) -> dict[str, typing.Any]: ...
    @classmethod
    def model_validate(cls, value: typing.Any, **_: typing.Any): ...
    @classmethod
    def model_validate_json(
        cls, value: str | bytes | bytearray, **_: typing.Any
    ): ...
    def __copy__(self) -> WireMessage:
        """
        Return a shallow copy of the value.
        """

    def __deepcopy__(self, memo: dict) -> WireMessage:
        """
        Return a deep copy of the value.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether two values are equal.
        """

    def __init__(
        self,
        node_fragments: typing.Any = [],
        actions: typing.Any = [],
        headers: collections.abc.Mapping[str, bytes] | None = {},
    ) -> None:
        """
        Create a wire message from node fragments, actions, and headers.
        """

    def __repr__(self) -> str:
        """
        Return a human-readable debug string.
        """

    def debug_string(self) -> str:
        """
        Return a human-readable debug string.
        """

    def model_copy(
        self, *, update: dict[str, typing.Any] | None = None, deep: bool = False
    ): ...
    def model_dump(
        self, *, mode: str = "python", **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    def model_dump_json(self, **kwargs: typing.Any) -> str: ...
    def to_json(self) -> str:
        """
        Serialize the message to its JSON wire encoding.
        """

    def to_msgpack(self) -> bytes:
        """
        Serialize the value to MessagePack bytes.
        """

    def validate(self) -> None:
        """
        Raise if the value fails structural validation.
        """

    @property
    def actions(self) -> _ActionMessageVectorView:
        """
        Action messages carried by the message.
        """
    @actions.setter
    def actions(self, arg1: typing.Any) -> None: ...

    @property
    def approx_bytes(self) -> int:
        """
        Approximate in-memory size of the message in bytes.
        """

    @property
    def headers(self) -> _ByteMapView:
        """
        Byte-string header map attached to the message.
        """
    @headers.setter
    def headers(
        self, arg1: collections.abc.Mapping[str, bytes] | None
    ) -> None: ...

    @property
    def node_fragments(self) -> _NodeFragmentVectorView:
        """
        Node fragments carried by the message.
        """
    @node_fragments.setter
    def node_fragments(self, arg1: typing.Any) -> None: ...

class WireStream:
    async def __aenter__(self) -> typing.Self: ...
    async def __aexit__(self, exc_type, exc, traceback) -> None: ...
    def __init__(self) -> None:
        """
        Construct the abstract WireStream base. Subclass this in Python to implement a custom asynchronous, bidirectional transport for an agent; the abstract operations (send, start/accept, get_status, get_trailers, ...) are dispatched to your overrides.
        """

    def abort(self, status: Status) -> None:
        """
        Terminate the stream immediately with an error status, discarding buffered messages and propagating failure to the peer and pending receivers.

        Examples:
            End an exchange when its upstream disappears:

            ```python
            stream.abort(Status(
                code=StatusCode.UNAVAILABLE,
                message="upstream connection was lost",
            ))
            ```
        """

    def accept(
        self,
        on_message: collections.abc.Callable[[WireMessage | None], typing.Any],
        on_done: collections.abc.Callable[[], typing.Any],
    ) -> asyncio.Future[None]:
        """
        Begin driving the stream as the responding (server) side, delivering each inbound message to the asynchronous on_message callback and end-of-stream to on_done. Use this instead of start() when this endpoint is answering an incoming agent connection. Returns an awaitable that resolves when acceptance completes; use on_done as the terminal barrier.
        """

    def drain_outgoing_messages(self) -> asyncio.Future[None]:
        """
        Await until every queued outbound message has been handed to the transport. Call `half_close` first so buffered output is not dropped.

        Examples:
            Use the transport delivery barrier during orderly shutdown:

            ```python
            stream.half_close()
            await stream.drain_outgoing_messages()
            ```
        """

    def get_id(self) -> str:
        """
        Return the stream's stable identifier, which also seeds its tracing trace id.
        """

    def get_impl(self) -> typing_extensions.CapsuleType | None:
        """
        Return an opaque native handle to the underlying implementation, or None. Intended for advanced interop, not normal agent code.
        """

    def get_status(self) -> Status:
        """
        Return the stream's terminal status once it has finished, or OK while it is still active. Inspect this after the stream completes to learn whether the agent exchange succeeded or failed.
        """

    def get_trailers(self) -> dict[str, bytes] | None:
        """
        Return the trailers (final metadata) the peer sent at half-close, or None if none were received. Read this after the stream ends to recover end-of-turn metadata from the agent exchange.
        """

    def half_close(
        self, trailers: collections.abc.Mapping[str, bytes] | None = None
    ) -> None:
        """
        Signal that this endpoint has finished sending, optionally attaching trailers. The stream stays open for inbound messages.

        Examples:
            End the local half and wait until queued messages reach the transport:

            ```python
            stream.half_close()
            await stream.drain_outgoing_messages()
            ```
        """

    def send(self, message: WireMessage) -> None:
        """
        Queue a message for asynchronous delivery to the peer. This call is non-blocking: the message enters the ordered outbound queue and the transport applies backpressure.

        Examples:
            Admit a request before closing the local sending side:

            ```python
            stream.send(request_message)
            ```
        """

    def set_deadline(self, deadline: Time | None = None) -> None:
        """
        Set an absolute wall-clock deadline after which the stream is automatically aborted; pass None to clear it.
        """

    def start(
        self,
        on_message: collections.abc.Callable[[WireMessage | None], typing.Any],
        on_done: collections.abc.Callable[[], typing.Any],
    ) -> asyncio.Future[None]:
        """
        Begin driving the stream as the initiating side, delivering inbound messages to `on_message` and completion to `on_done`. Callbacks are awaited as data arrives.

        Examples:
            Start a client transport with application callbacks:

            ```python
            await stream.start(on_message, on_transport_done)
            ```
        """

    @property
    def deadline(self) -> Time:
        """
        The stream's current absolute deadline, after which it is automatically aborted.
        """

class WireStreamOptions:
    _a11_options_installed: typing.ClassVar[bool] = True
    @staticmethod
    def __get_pydantic_core_schema__(option_cls, _source_type, _handler): ...
    @staticmethod
    def __get_pydantic_json_schema__(option_cls, _schema, _handler): ...
    @staticmethod
    def model_json_schema(
        option_cls, **_: typing.Any
    ) -> dict[str, typing.Any]: ...
    @staticmethod
    def model_validate(option_cls, value: typing.Any, **_: typing.Any): ...
    def __copy__(self): ...
    def __deepcopy__(self, _memo): ...
    def __eq__(self, other: object) -> bool: ...
    def __init__(
        self,
        max_buffered_incoming_messages: typing.SupportsInt | None = 100,
        max_single_message_size: typing.SupportsInt | None = 33554432,
        max_buffered_incoming_bytes: typing.SupportsInt | None = 33554432,
        message_timeout_millis: typing.Any | None = None,
        deadline: Time | None = None,
    ) -> None:
        """
        Construct wire-stream options controlling buffering and timeouts for an agent stream. All arguments are keyword-friendly and validated on construction.
        """

    def __repr__(self) -> str: ...
    def model_copy(
        self,
        *,
        update: collections.abc.Mapping[str, typing.Any] | None = None,
        deep: bool = False,
    ): ...
    def model_dump(self, **_: typing.Any) -> dict[str, typing.Any]: ...
    def validate(self) -> None:
        """
        Validate the options, raising on invalid configuration.
        """

    @property
    def deadline(self) -> Time:
        """
        Absolute wall-clock deadline after which the stream is aborted.
        """
    @deadline.setter
    def deadline(self, arg1: typing.Any) -> None: ...

    @property
    def max_buffered_incoming_bytes(self) -> int:
        """
        Maximum total bytes of buffered inbound messages before backpressure is applied.
        """
    @max_buffered_incoming_bytes.setter
    def max_buffered_incoming_bytes(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def max_buffered_incoming_messages(self) -> int:
        """
        Maximum number of inbound messages buffered before backpressure is applied.
        """
    @max_buffered_incoming_messages.setter
    def max_buffered_incoming_messages(
        self, arg0: typing.SupportsInt
    ) -> None: ...

    @property
    def max_single_message_size(self) -> int:
        """
        Maximum size, in bytes, of a single wire message.
        """
    @max_single_message_size.setter
    def max_single_message_size(self, arg0: typing.SupportsInt) -> None: ...

    @property
    def message_timeout(self) -> Duration:
        """
        Per-message inactivity timeout as a duration.
        """
    @message_timeout.setter
    def message_timeout(self, arg1: typing.Any) -> None: ...

    @property
    def message_timeout_millis(self) -> Duration:
        """
        Per-message inactivity timeout expressed in milliseconds.
        """
    @message_timeout_millis.setter
    def message_timeout_millis(self, arg1: typing.Any) -> None: ...

class WireStreamWithRecv(WireStream):
    def __init__(self, stream: typing.Any) -> None:
        """
        Wrap a callback-based WireStream in a pull-oriented adapter that exposes receive().
        """

    def accept(self) -> asyncio.Future[None]:
        """
        Accept on the wrapped stream as the responding side; returns an awaitable.
        """

    def receive(
        self, timeout: Duration | None = None
    ) -> asyncio.Future[WireMessage | None]:
        """
        Await the next inbound message, or None at end of stream, honoring the optional timeout.
        """

    def start(self) -> asyncio.Future[None]:
        """
        Start the wrapped stream as the initiating side; returns an awaitable.
        """

    @property
    def wrapped_stream(self) -> WireStream:
        """
        The underlying WireStream being adapted.
        """

class _ActionHeaderSchemaMapView:
    """
    Mutable dict-like view over an action schema's header map.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    def __bool__(self) -> bool:
        """
        Return True when the map has at least one entry.
        """

    def __contains__(self, key: str) -> bool:
        """
        Return True when the map contains the given key.
        """

    def __delitem__(self, key: str) -> None:
        """
        Remove the entry stored under the given key.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return True when the map equals another mapping.
        """

    def __getitem__(self, key: str) -> ActionHeaderSchema:
        """
        Return the value stored under the given key.
        """

    def __iter__(self) -> collections.abc.Iterator[str]:
        """
        Return an iterator over the map's keys.
        """

    def __len__(self) -> int:
        """
        Return the number of entries in the map.
        """

    def __repr__(self) -> str:
        """
        Return the repr of the map's entries.
        """

    def __setitem__(self, key: str, value: typing.Any) -> None:
        """
        Store a value under the given key, re-validating the schema.
        """

    def clear(self) -> None:
        """
        Remove all entries from the map.
        """

    def copy(self) -> dict[str, ActionHeaderSchema]:
        """
        Return a plain dict copy of the map's entries.
        """

    def get(
        self, key: str, default: ActionHeaderSchema | None = None
    ) -> ActionHeaderSchema | None:
        """
        Return the value for the key, or the default if it is absent.
        """

    def items(self) -> collections.abc.Iterable[tuple[str, ActionHeaderSchema]]:
        """
        Return a view of the map's (key, value) pairs.
        """

    def keys(self) -> collections.abc.Iterable[str]:
        """
        Return a view of the map's keys.
        """

    def update(self, updates: typing.Any) -> None:
        """
        Merge the entries of another mapping into this map.
        """

    def values(self) -> collections.abc.Iterable[ActionHeaderSchema]:
        """
        Return a view of the map's values.
        """

class _ActionMessageVectorView:
    """
    Mutable list view over an ActionMessage vector field.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    def __bool__(self) -> bool:
        """
        Return whether the vector contains any elements.
        """

    def __contains__(self, value: ActionMessage) -> bool:
        """
        Return whether the vector contains the given value.
        """

    @typing.overload
    def __delitem__(self, index: typing.SupportsInt) -> None:
        """
        Delete the element at the given index.
        """
    @typing.overload
    def __delitem__(self, slice: slice) -> None:
        """
        Delete the elements selected by the slice.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether the vector equals the given object.
        """

    @typing.overload
    def __getitem__(self, index: typing.SupportsInt) -> ActionMessage:
        """
        Return the element at the given index.
        """
    @typing.overload
    def __getitem__(self, slice: slice) -> list:
        """
        Return a list of the elements selected by the slice.
        """

    def __iter__(self) -> collections.abc.Iterator[ActionMessage]:
        """
        Return an iterator over the elements.
        """

    def __len__(self) -> int:
        """
        Return the number of elements in the vector.
        """

    def __repr__(self) -> str:
        """
        Return the repr of the vector as a list.
        """

    @typing.overload
    def __setitem__(
        self, index: typing.SupportsInt, value: ActionMessage
    ) -> None:
        """
        Assign a value to the element at the given index.
        """
    @typing.overload
    def __setitem__(self, slice: slice, replacements: typing.Any) -> None:
        """
        Assign a sequence of values to the elements selected by the slice.
        """

    def append(self, value: ActionMessage) -> None:
        """
        Append a value to the end of the vector.
        """

    def clear(self) -> None:
        """
        Remove all elements from the vector.
        """

    def copy(self) -> list:
        """
        Return a shallow copy of the elements as a list.
        """

    def count(self, value: ActionMessage) -> int:
        """
        Return the number of elements equal to the given value.
        """

    def extend(self, values: typing.Any) -> None:
        """
        Append every value from the iterable to the vector.
        """

    def index(self, value: ActionMessage) -> int:
        """
        Return the index of the first element equal to the given value.
        """

    def insert(self, index: typing.SupportsInt, value: ActionMessage) -> None:
        """
        Insert a value before the given index.
        """

    def pop(self, index: typing.SupportsInt = -1) -> ActionMessage:
        """
        Remove and return the element at the given index (default last).
        """

    def remove(self, value: ActionMessage) -> None:
        """
        Remove the first element equal to the given value.
        """

    def reverse(self) -> None:
        """
        Reverse the elements of the vector in place.
        """

class _ActionPortSchemaMapView:
    """
    Mutable dict-like view over an action schema's port map.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    def __bool__(self) -> bool:
        """
        Return True when the map has at least one entry.
        """

    def __contains__(self, key: str) -> bool:
        """
        Return True when the map contains the given key.
        """

    def __delitem__(self, key: str) -> None:
        """
        Remove the entry stored under the given key.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return True when the map equals another mapping.
        """

    def __getitem__(self, key: str) -> ActionPortSchema:
        """
        Return the value stored under the given key.
        """

    def __iter__(self) -> collections.abc.Iterator[str]:
        """
        Return an iterator over the map's keys.
        """

    def __len__(self) -> int:
        """
        Return the number of entries in the map.
        """

    def __repr__(self) -> str:
        """
        Return the repr of the map's entries.
        """

    def __setitem__(self, key: str, value: typing.Any) -> None:
        """
        Store a value under the given key, re-validating the schema.
        """

    def clear(self) -> None:
        """
        Remove all entries from the map.
        """

    def copy(self) -> dict[str, ActionPortSchema]:
        """
        Return a plain dict copy of the map's entries.
        """

    def get(
        self, key: str, default: ActionPortSchema | None = None
    ) -> ActionPortSchema | None:
        """
        Return the value for the key, or the default if it is absent.
        """

    def items(self) -> collections.abc.Iterable[tuple[str, ActionPortSchema]]:
        """
        Return a view of the map's (key, value) pairs.
        """

    def keys(self) -> collections.abc.Iterable[str]:
        """
        Return a view of the map's keys.
        """

    def update(self, updates: typing.Any) -> None:
        """
        Merge the entries of another mapping into this map.
        """

    def values(self) -> collections.abc.Iterable[ActionPortSchema]:
        """
        Return a view of the map's values.
        """

class _ByteMapView:
    """
    Mutable mapping view over a byte-string map field.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    def __contains__(self, key: str) -> bool:
        """
        Return whether the mapping contains the given key.
        """

    def __delitem__(self, key: str) -> None:
        """
        Delete the entry with the given key.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether the mapping equals the given object.
        """

    def __getitem__(self, key: str) -> bytes:
        """
        Return the bytes stored under the given key.
        """

    def __iter__(self) -> collections.abc.Iterator[str]:
        """
        Return an iterator over the keys.
        """

    def __len__(self) -> int:
        """
        Return the number of entries in the mapping.
        """

    def __repr__(self) -> str:
        """
        Return the repr of the mapping as a dict.
        """

    def __setitem__(self, key: str, item: typing.Any) -> None:
        """
        Store bytes under the given key.
        """

    def clear(self) -> None:
        """
        Remove all entries from the mapping.
        """

    def copy(self) -> dict[str, bytes]:
        """
        Return a plain dict copy of the mapping.
        """

    def get(self, key: str, default: bytes | None = None) -> bytes | None:
        """
        Return the bytes for a key, or the default if it is absent.
        """

    def items(self) -> collections.abc.Iterable[tuple[str, bytes]]:
        """
        Return a view of the mapping's key/value pairs.
        """

    def keys(self) -> collections.abc.Iterable[str]:
        """
        Return a view of the mapping's keys.
        """

    def update(self, updates: typing.Any) -> None:
        """
        Merge entries from another mapping into this one.
        """

    def values(self) -> collections.abc.Iterable[bytes]:
        """
        Return a view of the mapping's values.
        """

class _NodeFragmentVectorView:
    """
    Mutable list view over a NodeFragment vector field.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    def __bool__(self) -> bool:
        """
        Return whether the vector contains any elements.
        """

    def __contains__(self, value: NodeFragment) -> bool:
        """
        Return whether the vector contains the given value.
        """

    @typing.overload
    def __delitem__(self, index: typing.SupportsInt) -> None:
        """
        Delete the element at the given index.
        """
    @typing.overload
    def __delitem__(self, slice: slice) -> None:
        """
        Delete the elements selected by the slice.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether the vector equals the given object.
        """

    @typing.overload
    def __getitem__(self, index: typing.SupportsInt) -> NodeFragment:
        """
        Return the element at the given index.
        """
    @typing.overload
    def __getitem__(self, slice: slice) -> list:
        """
        Return a list of the elements selected by the slice.
        """

    def __iter__(self) -> collections.abc.Iterator[NodeFragment]:
        """
        Return an iterator over the elements.
        """

    def __len__(self) -> int:
        """
        Return the number of elements in the vector.
        """

    def __repr__(self) -> str:
        """
        Return the repr of the vector as a list.
        """

    @typing.overload
    def __setitem__(
        self, index: typing.SupportsInt, value: NodeFragment
    ) -> None:
        """
        Assign a value to the element at the given index.
        """
    @typing.overload
    def __setitem__(self, slice: slice, replacements: typing.Any) -> None:
        """
        Assign a sequence of values to the elements selected by the slice.
        """

    def append(self, value: NodeFragment) -> None:
        """
        Append a value to the end of the vector.
        """

    def clear(self) -> None:
        """
        Remove all elements from the vector.
        """

    def copy(self) -> list:
        """
        Return a shallow copy of the elements as a list.
        """

    def count(self, value: NodeFragment) -> int:
        """
        Return the number of elements equal to the given value.
        """

    def extend(self, values: typing.Any) -> None:
        """
        Append every value from the iterable to the vector.
        """

    def index(self, value: NodeFragment) -> int:
        """
        Return the index of the first element equal to the given value.
        """

    def insert(self, index: typing.SupportsInt, value: NodeFragment) -> None:
        """
        Insert a value before the given index.
        """

    def pop(self, index: typing.SupportsInt = -1) -> NodeFragment:
        """
        Remove and return the element at the given index (default last).
        """

    def remove(self, value: NodeFragment) -> None:
        """
        Remove the first element equal to the given value.
        """

    def reverse(self) -> None:
        """
        Reverse the elements of the vector in place.
        """

class _PortVectorView:
    """
    Mutable list view over a Port vector field.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    def __bool__(self) -> bool:
        """
        Return whether the vector contains any elements.
        """

    def __contains__(self, value: Port) -> bool:
        """
        Return whether the vector contains the given value.
        """

    @typing.overload
    def __delitem__(self, index: typing.SupportsInt) -> None:
        """
        Delete the element at the given index.
        """
    @typing.overload
    def __delitem__(self, slice: slice) -> None:
        """
        Delete the elements selected by the slice.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return whether the vector equals the given object.
        """

    @typing.overload
    def __getitem__(self, index: typing.SupportsInt) -> Port:
        """
        Return the element at the given index.
        """
    @typing.overload
    def __getitem__(self, slice: slice) -> list:
        """
        Return a list of the elements selected by the slice.
        """

    def __iter__(self) -> collections.abc.Iterator[Port]:
        """
        Return an iterator over the elements.
        """

    def __len__(self) -> int:
        """
        Return the number of elements in the vector.
        """

    def __repr__(self) -> str:
        """
        Return the repr of the vector as a list.
        """

    @typing.overload
    def __setitem__(self, index: typing.SupportsInt, value: Port) -> None:
        """
        Assign a value to the element at the given index.
        """
    @typing.overload
    def __setitem__(self, slice: slice, replacements: typing.Any) -> None:
        """
        Assign a sequence of values to the elements selected by the slice.
        """

    def append(self, value: Port) -> None:
        """
        Append a value to the end of the vector.
        """

    def clear(self) -> None:
        """
        Remove all elements from the vector.
        """

    def copy(self) -> list:
        """
        Return a shallow copy of the elements as a list.
        """

    def count(self, value: Port) -> int:
        """
        Return the number of elements equal to the given value.
        """

    def extend(self, values: typing.Any) -> None:
        """
        Append every value from the iterable to the vector.
        """

    def index(self, value: Port) -> int:
        """
        Return the index of the first element equal to the given value.
        """

    def insert(self, index: typing.SupportsInt, value: Port) -> None:
        """
        Insert a value before the given index.
        """

    def pop(self, index: typing.SupportsInt = -1) -> Port:
        """
        Remove and return the element at the given index (default last).
        """

    def remove(self, value: Port) -> None:
        """
        Remove the first element equal to the given value.
        """

    def reverse(self) -> None:
        """
        Reverse the elements of the vector in place.
        """

class _Span:
    def end(self) -> None:
        """
        Ends the span.
        """

    def set_attribute(self, key: str, value: typing.Any) -> None:
        """
        Sets an attribute on the span.
        """

    def set_name(self, name: str) -> None:
        """
        Updates the span's name.
        """

    def set_status(self, code: str, description: str = "") -> None:
        """
        Sets the span's status code ('ok', 'error', or 'unset') and an optional description.
        """

    def traceparent(self) -> str:
        """
        Returns the W3C traceparent for this span, so it can parent child actions or spans.
        """

class _StringSchemaMapView:
    """
    Mutable dict-like view over an action schema's string map.
    """

    __hash__: None = None  # pyright: ignore[reportIncompatibleMethodOverride]
    def __bool__(self) -> bool:
        """
        Return True when the map has at least one entry.
        """

    def __contains__(self, key: str) -> bool:
        """
        Return True when the map contains the given key.
        """

    def __delitem__(self, key: str) -> None:
        """
        Remove the entry stored under the given key.
        """

    def __eq__(self, other: object) -> bool:
        """
        Return True when the map equals another mapping.
        """

    def __getitem__(self, key: str) -> str:
        """
        Return the value stored under the given key.
        """

    def __iter__(self) -> collections.abc.Iterator[str]:
        """
        Return an iterator over the map's keys.
        """

    def __len__(self) -> int:
        """
        Return the number of entries in the map.
        """

    def __repr__(self) -> str:
        """
        Return the repr of the map's entries.
        """

    def __setitem__(self, key: str, value: typing.Any) -> None:
        """
        Store a value under the given key, re-validating the schema.
        """

    def clear(self) -> None:
        """
        Remove all entries from the map.
        """

    def copy(self) -> dict[str, str]:
        """
        Return a plain dict copy of the map's entries.
        """

    def get(self, key: str, default: str | None = None) -> str | None:
        """
        Return the value for the key, or the default if it is absent.
        """

    def items(self) -> collections.abc.Iterable[tuple[str, str]]:
        """
        Return a view of the map's (key, value) pairs.
        """

    def keys(self) -> collections.abc.Iterable[str]:
        """
        Return a view of the map's keys.
        """

    def update(self, updates: typing.Any) -> None:
        """
        Merge the entries of another mapping into this map.
        """

    def values(self) -> collections.abc.Iterable[str]:
        """
        Return a view of the map's values.
        """

def asr_model_shorthands() -> list[str]:
    """
    The accepted transcription-model shorthands, in a stable order.
    """

def audio_actions() -> (
    list[tuple[str, ActionSchema, ActionHandler | NativeActionHandler | None]]
):
    """
    Return the audio Actions as (name, schema, handler) triples in protocol order, each schema's ports already wired to the matching audio type and their serializers installed.
    """

def audio_buffer_from_msgpack(data: bytes) -> AudioBuffer:
    """
    Decode an AudioBuffer from A11's MessagePack representation.
    """

def audio_buffer_to_msgpack(buffer: AudioBuffer) -> bytes:
    """
    Encode an AudioBuffer to A11's MessagePack representation.
    """

def audio_device_info(index: typing.SupportsInt) -> AudioDeviceInfo:
    """
    Return metadata for the audio device at `index`.
    """

def audio_model_cache_dir() -> str:
    """
    The directory shorthand models are cached in.
    """

def create_in_process_wire_stream_pair(
    options: WireStreamOptions | None = None,
    first_options: WireStreamOptions | None = None,
    second_options: WireStreamOptions | None = None,
) -> tuple[InProcessWireStream, InProcessWireStream]:
    """
    Create a connected pair of in-process wire streams (free-function form of InProcessWireStream.create_pair).
    """

def default_audio_input_device() -> AudioDeviceInfo:
    """
    Return metadata for the host's default input device.
    """

def default_redis_client() -> RedisClient:
    """
    Return the process-global client configured from A11_REDIS_*.
    """

def download(url: str, options: DownloadOptions) -> asyncio.Future[str]:
    """
    Download a URL to a verified file, atomically.

    Returns the destination path. A destination that already exists and matches
    ``expected_sha1`` is returned without touching the network. Awaitable.
    """

def emit_log(
    severity: typing.SupportsInt,
    message: str,
    verbosity: typing.SupportsInt = -1,
) -> None:
    """
    Write one entry to the native log, so an application can check that its logging configuration reaches the C++ runtime. FATAL is not available here.
    """

def fetch(
    url: str, options: FetchOptions | None = None
) -> asyncio.Future[HttpResponse]:
    """
    Fetch a URL and buffer the whole response.

    Follows redirects, maps a 4xx/5xx onto a status error, and enables TLS from the
    scheme. Awaitable.

    Examples:
        ```python
        response = await a11.net.http.fetch("https://example.com/index.html")
        print(response.head.status, len(response.body))
        ```
    """

def file_sha1(path: str) -> str:
    """
    Compute the SHA-1 of a file as lowercase hex. Blocks.
    """

def get_http_header(headers: list[tuple[str, str]], name: str) -> str | None:
    """
    Look up a header value by name, returning None if it is absent.
    """

def http_actions() -> (
    list[tuple[str, ActionSchema, ActionHandler | NativeActionHandler | None]]
):
    """
    Return the HTTP Actions as (name, schema, handler) triples: make_http_request and web-fetch, in that order.
    """

def is_close_status_chunk(chunk: Chunk) -> bool:
    """
    Return True when the chunk is a status chunk marking that a node's write half was closed, rather than a status value.
    """

def is_half_close_message(message: WireMessage) -> bool:
    """
    Return whether the message is a half-close signal.
    """

def is_status_chunk(chunk: Chunk) -> bool:
    """
    Return True when the chunk carries an action status.
    """

def list_audio_devices() -> list[AudioDeviceInfo]:
    """
    Return metadata for every audio device, in index order.
    """

def lookup_asr_model(shorthand: str) -> AudioModelSpec:
    """
    Look up a transcription model by shorthand.
    """

def lookup_vad_model(shorthand: str) -> AudioModelSpec:
    """
    Look up a VAD model by shorthand.
    """

def make_half_close_message(
    trailers: collections.abc.Mapping[str, bytes] | None = {},
) -> WireMessage:
    """
    Build a half-close wire message carrying the given trailers.
    """

def normalize_session_headers(
    headers: collections.abc.Mapping[str, bytes] | None = None,
) -> dict[str, bytes]:
    """
    Normalize a session headers mapping, returning the canonicalized header dict.
    """

def obs_clear_recorded_spans() -> None:
    """
    Clears the in-memory span buffer.
    """

def obs_configure(
    service_name: str = "a11",
    resource_attributes: collections.abc.Mapping[str, str] = {},
    exporter: str = "otlp_http",
    use_simple_processor: bool = False,
    otlp_endpoint: str = "",
    otlp_headers: collections.abc.Mapping[str, str] = {},
    otlp_timeout_millis: typing.SupportsInt = 10000,
    baggage_span_attributes: collections.abc.Sequence[str] = [],
) -> None:
    """
    Installs the global tracer provider with the given service name, exporter, and OTLP options; replaces any existing provider.
    """

def obs_is_configured() -> bool:
    """
    Returns whether a tracer provider is currently installed.
    """

def obs_recorded_spans() -> list[dict[str, typing.Any]]:
    """
    Returns finished spans captured by the in-memory exporter, oldest first.
    """

def obs_shutdown() -> None:
    """
    Flushes and tears down the global tracer provider.
    """

def obs_start_span(
    name: str, kind: str = "internal", parent_traceparent: str = ""
) -> _Span:
    """
    Starts a new span with the given name and kind, optionally parented by a W3C traceparent.
    """

def parse_url(url: str) -> ParsedUrl:
    """
    Parse an absolute http/https/ws/wss URL, raising on a malformed one.
    """

def register_audio_actions(registry: ActionRegistry | None) -> None:
    """
    Register every audio Action on `registry`, wiring each port's typeinfo to the matching audio type and ensuring their serializers are installed.
    """

def register_http_actions(registry: ActionRegistry | None) -> None:
    """
    Register make_http_request and web-fetch on `registry`.
    """

def reset_default_redis_client() -> None:
    """
    Clear the global Redis client so its environment is reread.
    """

def resolve_asr_model(
    spec: str,
    on_progress: (
        collections.abc.Callable[[typing.SupportsInt, typing.SupportsInt], None]
        | None
    ) = None,
) -> asyncio.Future[str]:
    """
    Resolve a transcription model shorthand or path to a local file, downloading it if needed. Awaitable.
    """

def resolve_url_reference(base: ParsedUrl, reference: str) -> ParsedUrl:
    """
    Resolve a reference (such as a Location header) against a base URL.
    """

def resolve_vad_model(
    spec: str,
    on_progress: (
        collections.abc.Callable[[typing.SupportsInt, typing.SupportsInt], None]
        | None
    ) = None,
) -> asyncio.Future[str]:
    """
    Resolve a VAD model shorthand or path to a local file, downloading it if needed. An empty spec resolves to an empty path. Awaitable.
    """

def set_default_redis_client(client: RedisClient) -> None:
    """
    Replace the process-global Redis client.
    """

def set_log_sink(callback: typing.Any) -> None:
    """
    Route native log entries to `callback(severity, verbosity, filename, line, message, unix_seconds)`, replacing any previously installed sink. None removes the sink. FATAL entries are never routed; Abseil writes those to stderr with a backtrace.
    """

def set_min_log_level(severity: typing.SupportsInt) -> None:
    """
    Drop native log entries below this absl severity (0=INFO, 1=WARNING, 2=ERROR, 3=FATAL) before they are formatted.
    """

def set_stderr_threshold(severity: typing.SupportsInt) -> None:
    """
    Write native log entries at or above this absl severity straight to stderr, bypassing any installed sink.
    """

def set_vlog_level(level: typing.SupportsInt) -> None:
    """
    Emit VLOG(n) entries for n at or below this level. 0 disables them.
    """

def status_code_from_http(arg0: typing.SupportsInt) -> int: ...
def status_code_from_websocket(arg0: typing.SupportsInt) -> int: ...
def status_code_to_http(arg0: typing.SupportsInt) -> int: ...
def status_code_to_websocket(arg0: typing.SupportsInt) -> int: ...
def status_from_chunk(chunk: Chunk) -> Status:
    """
    Decode an absl Status from a data chunk.
    """

def status_to_chunk(status: Status, closing: bool = False) -> Chunk:
    """
    Encode an absl Status as a data chunk. With closing=True the chunk is a node closure marker rather than a value: it reports that the producer drained the node and closed its write half with that status.
    """

def vad_model_shorthands() -> list[str]:
    """
    The accepted VAD-model shorthands.
    """

def validate_http_headers(
    headers: collections.abc.Iterable[tuple[str, str]] | None,
) -> None:
    """
    Validate a collection of HTTP headers, raising on error.
    """

def validate_name_string(name: str) -> str:
    """
    Validate a name string and return it, raising if it is invalid.
    """

ACCEPT: StreamMode
ACTION_DISPATCH_STATUS_OUTPUT: str = "__dispatch_status__"
ACTION_HEADER_PREFIX: str = "x-a11-"
ACTION_STATUS_MIMETYPE: str = "application/x-a11-status"
ACTION_STATUS_OUTPUT: str = "__status__"
CANCEL_ACTION_HEADER: str = "__action"
CANCEL_ACTION_NAME: str = "__cancel__"
CANDIDATE: SignallingMessageType
CLOSE_STATUS_ATTRIBUTE: str = "a11-close"
DEFAULT_ASR_MODEL: str = "tiny.en"
DEFAULT_MAX_CONCURRENT_NESTED_ACTIONS: int = 64
DEFAULT_SSE_CONNECT_ENDPOINT: str = "/connect"
DEFAULT_SSE_MESSAGE_ENDPOINT: str = "/streams/{id}/message"
DEFAULT_VAD_MODEL: str = "silero-v5.1.2"
DESCRIPTION: SignallingMessageType
EMPTY_WIRE_MESSAGE_SIZE: int = 4
ERROR: SignallingMessageType
JSON_MIMETYPE: str = "application/json"
MAKE_HTTP_REQUEST_ACTION: str = "make_http_request"
MAX_SINGLE_MESSAGE_SIZE: int = 33554432
MSGPACK_MIMETYPE: str = "application/x-msgpack"
OTEL_BAGGAGE_HEADER: str = "x-otel-baggage"
OTEL_TRACEPARENT_HEADER: str = "x-otel-traceparent"
OTEL_TRACESTATE_HEADER: str = "x-otel-tracestate"
SESSION_MAX_SINGLE_MESSAGE_SIZE: int = 33554432
SESSION_STATUS_HEADER: str = "x-a11-session-status"
SSE_HTTP_HEADER_PREFIX: str = "x-a11-http-"
SSE_STREAM_ID_HEADER: str = "x-a11-stream-id"
START: StreamMode
TCP: TurnRelayType
TLS: TurnRelayType
UDP: TurnRelayType
WEB_FETCH_ACTION: str = "web-fetch"
WHOLE_JSON: str = "$"
WIRE_MESSAGE_VERSION: int = 1
WIRE_STREAM_ABORT_STATUS_HEADER: str = "x-a11-abort-status"
WIRE_STREAM_MAX_SINGLE_MESSAGE_SIZE: int = 33554432
__version__: str
HttpSseWireStreamServer = HttpSseServer

from typing import Any, Sequence

from . import logging as logging

# Before the submodules load, so anything they log on the way in is already
# governed by the importing process's configuration rather than by A11.
logging._configure_from_context()

from .actions import (
    Action,
    ActionHeaderSchema,
    ActionPortSchema,
    ActionRegistry,
    ActionSchema,
    ActionSettings,
)
from .actions.action import DEFAULT_HEADERS as DEFAULT_ACTION_HEADERS
from .data.types import (
    ChunkMetadata,
    Chunk,
    NodeRef,
    NodeFragment,
    Port,
    ActionMessage,
    WireMessage,
)
from .data.serialization import get_global_serialization_registry
from .logging import (
    disable as disable_logging,
    enable as enable_logging,
    get_level as get_log_level,
    get_logger,
    set_level as set_log_level,
)
from .net.http2 import (
    Http2Client,
    Http2DuplexStream,
    Http2Options,
    Http2RequestBodyStream,
    Http2ResponseStream,
    Http2ResponseWriter,
    Http2Server,
    Http2TlsOptions,
    HttpProtocolPreference,
    HttpRequest,
    HttpResponse,
    HttpResponseHead,
)
from .net.http_sse_wire_stream import (
    HttpSseClientWireStream,
    HttpSseOptions,
    HttpSseServer,
    HttpSseServerWireStream,
    HttpSseWireStream,
)
from .net.in_process_wire_stream import (
    InProcessWireStream,
    create_in_process_wire_stream_pair,
)
from .net.signalling import (
    SignallingEndpoint,
    SignallingMessage,
    SignallingMessageType,
    SignallingService,
    SignallingTransport,
    WebSocketSignallingClient,
    WebSocketSignallingClientOptions,
    WebSocketSignallingServer,
    WebSocketSignallingServerOptions,
)
from .net.webrtc_wire_stream import (
    TurnRelayType,
    TurnServer,
    WebRtcConfiguration,
    WebRtcWireServer,
    WebRtcWireStream,
)
from .net.websocket_wire_stream import (
    ChannelFramingOptions,
    WebSocketClientOptions,
    WebSocketServerOptions,
    WebSocketWireServer,
    WebSocketWireStream,
)
from .net.wire_stream import WireStream, WireStreamOptions, WireStreamWithRecv
from .nodes.async_node import AsyncNode, NodeMap
from .observability import (
    configure_langfuse_from_env,
    configure_otel,
    configure_otel_from_env,
    enable_tracing,
    langfuse,
    new_traceparent,
    shutdown_otel,
)
from .redis.client import (
    RedisClient,
    RedisClientOptions,
    RedisReply,
    RedisReplyType,
    RedisSubscription,
    default_client as default_redis_client,
    reset_default_client as reset_default_redis_client,
    set_default_client as set_default_redis_client,
)
from .service.session import (
    normalise_headers,
    Session,
    SessionOptions,
    SessionWithRecv,
)
from .status import Status, StatusCode
from .stores.chunk_store import ChunkStore, ChunkStoreFactory
from .stores.chunk_store_reader import ChunkStoreReader, ChunkStoreReaderOptions
from .stores.chunk_store_writer import ChunkStoreWriter, ChunkStoreWriterOptions
from .stores.local_chunk_store import LocalChunkStore
from .stores.redis_chunk_store import (
    RedisChunkStore,
    RedisChunkStoreKeys,
    RedisChunkStoreMetadata,
    RedisChunkStoreOptions,
)
from .stores.sqlite_chunk_store import (
    SQLiteChunkStore,
    SQLiteChunkStoreFactory,
    SQLiteChunkStoreMetadata,
    SQLiteChunkStoreOptions,
    SQLiteSynchronous,
)
from .timing import (
    Duration,
    Time,
    infinite_duration,
    infinite_future,
    infinite_past,
    now,
    zero_duration,
)


def get_deadline(action: Action) -> Time:
    """Read the ``x-a11-deadline`` header as an absolute :class:`Time`.

    The value is a base-10 count of **milliseconds** since the Unix epoch, or a
    base-10 count of **nanoseconds** when suffixed with ``ns``. An absent header
    means no deadline (:func:`infinite_future`).
    """
    deadline_str = action.get_header("x-a11-deadline", decode=True)
    if deadline_str is None:
        return infinite_future()

    nanos = False
    if deadline_str.endswith("ns"):
        deadline_str = deadline_str[:-2]
        nanos = True

    try:
        deadline = int(deadline_str)
    except ValueError:
        deadline = -1
    if deadline < 0:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "The x-a11-deadline header must be a non-negative base-10"
                " integer of milliseconds since the epoch, or nanoseconds with"
                " an 'ns' suffix."
            ),
        ).to_exception()

    # Bare values are milliseconds; only the 'ns' form is already nanoseconds.
    if not nanos:
        deadline *= 1000000

    return Time.from_nanoseconds_since_epoch(deadline)


def set_deadline_header(action: Action, deadline: Time = infinite_future()):
    """Write ``deadline`` to the ``x-a11-deadline`` header as milliseconds.

    An infinite deadline clears the header. The value is emitted as a bare
    base-10 millisecond count, the representation :func:`get_deadline` reads by
    default.
    """
    if deadline == infinite_future():
        try:
            action.remove_header("x-a11-deadline")
            return
        except:
            pass

    action.set_header(
        "x-a11-deadline",
        str(deadline.nanoseconds_since_epoch // 1000000).encode(),
    )


def to_chunk(obj: Any, mimetype: str = "") -> Chunk:
    """Serialize ``obj`` into a chunk.

    If ``mimetype`` is empty, the closest registered Python type wins and
    registration order chooses its preferred representation.  An explicit
    MIME value can be exact or contain ``*`` wildcards.  Returned chunks
    always have an exact MIME type and a stable Python type identifier.
    """
    return get_global_serialization_registry().to_chunk(obj, mimetype)


def from_chunk(
    chunk: Chunk,
    mimetype_patterns: str | Sequence[str] = "",
    obj_type: type | None = None,
) -> Any:
    """Deserialize ``chunk`` using the first matching MIME selector.

    Selectors are matched in order against the chunk's MIME type and may
    contain wildcards.  If the chunk has no MIME metadata, a supplied exact
    selector acts as the representation.  A requested ``obj_type`` uses an
    exact registration first and then registrations for its superclasses.
    """
    return get_global_serialization_registry().from_chunk(
        chunk, mimetype_patterns, obj_type
    )

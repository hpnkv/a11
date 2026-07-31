"""Native paired in-process wire streams.

An `InProcessWireStream` connects two endpoints inside a single process
without any network -- the ideal transport for tests and for composing agents
that run in the same interpreter. Create a connected pair with
`create_in_process_wire_stream_pair`. See
[a11.net.wire_stream][a11.net.wire_stream] for
the transport-agnostic interface it implements.
"""

from a11 import _native
from a11.net.wire_stream import WireStreamOptions

from a11._native import InProcessWireStream

InProcessWireStream.__module__ = __name__


def create_in_process_wire_stream_pair(
    options: WireStreamOptions | None = None,
    *,
    first_options: WireStreamOptions | None = None,
    second_options: WireStreamOptions | None = None,
) -> tuple[InProcessWireStream, InProcessWireStream]:
    return _native.create_in_process_wire_stream_pair(
        options,
        first_options=first_options,
        second_options=second_options,
    )


__all__ = [
    "InProcessWireStream",
    "create_in_process_wire_stream_pair",
]

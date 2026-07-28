"""Native paired in-process wire streams."""

from a11 import _native
from a11.net.wire_stream import WireStreamOptions

InProcessWireStream = _native.InProcessWireStream
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

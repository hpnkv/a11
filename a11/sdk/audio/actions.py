"""Action-based interface to the audio SDK.

Four native (C++) Actions wrap the capture and recognition primitives:

* ``list_audio_inputs`` streams one :class:`~a11.sdk.audio.AudioDeviceInfo` per
  available input device;
* ``capture_audio`` streams :class:`~a11.sdk.audio.AudioBuffer` blocks with a
  control input that accepts a stop command;
* ``capture_transcription`` streams recognized text pieces, reusing the capture
  path and driving whisper.cpp recognition;
* ``transcribe_audio`` streams recognized text pieces from a caller-supplied
  :class:`~a11.sdk.audio.AudioBuffer` stream instead of a device, stopping on
  input closure or cancellation.

Install all four on a registry with :func:`register`:

```python
from a11.actions import ActionRegistry
from a11.sdk.audio import actions

registry = ActionRegistry()
actions.register(registry)
```

Each Action's schema and handler are also importable on their own, so a host
can publish a subset, inspect a schema before committing to it, or bind a
handler to an Action it built itself:

```python
from a11.sdk.audio.actions import (
    TRANSCRIBE_AUDIO,
    TRANSCRIBE_AUDIO_HANDLER,
    TRANSCRIBE_AUDIO_SCHEMA,
)

registry.register(TRANSCRIBE_AUDIO, TRANSCRIBE_AUDIO_SCHEMA,
                  TRANSCRIBE_AUDIO_HANDLER)
```

The handlers are :class:`~a11.actions.action.NativeActionHandler` handles, not
Python callables: pass one wherever a handler is accepted and the C++
implementation runs directly. The value-type serialization also lives in C++,
and each port's ``typeinfo`` is wired to the matching native audio class so
schema-driven tooling sees the right types.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from a11 import _native
from a11._native_options import install_native_options
from a11.actions import ActionHandler, ActionSchema
from a11.data import serial_tags
from a11.sdk.audio.client import register_json_codec

from a11._native import AudioCaptureEvent
from a11._native import AudioControlEvent
from a11._native import TranscriptionEvent

if TYPE_CHECKING:
    from a11.actions import ActionRegistry

#: Registered name of the list-inputs Action.
LIST_AUDIO_INPUTS = "list_audio_inputs"
#: Registered name of the capture Action.
CAPTURE_AUDIO = "capture_audio"
#: Registered name of the transcription Action.
CAPTURE_TRANSCRIPTION = "capture_transcription"
#: Registered name of the buffer-stream transcription Action.
TRANSCRIBE_AUDIO = "transcribe_audio"

install_native_options(AudioControlEvent, {"command": (str, "stop")})
install_native_options(
    AudioCaptureEvent, {"kind": (str, "started"), "dropped": (int, 0)}
)
install_native_options(TranscriptionEvent, {"kind": (str, "capture_started")})

# JSON codecs so the event types round-trip through the Python serialization
# registry the AsyncNode uses, matching the native C++ tags.
register_json_codec(AudioControlEvent, serial_tags.AUDIO_CONTROL_EVENT)
register_json_codec(AudioCaptureEvent, serial_tags.AUDIO_CAPTURE_EVENT)
register_json_codec(TranscriptionEvent, serial_tags.TRANSCRIPTION_EVENT)

# The native side owns the table -- schemas with typeinfo attached, handlers,
# and the protocol order -- so the exported objects cannot drift from what a
# C++ host registers.
_ENTRIES = {
    name: (schema, handler) for name, schema, handler in _native.audio_actions()
}

#: Schema for ``list_audio_inputs``.
LIST_AUDIO_INPUTS_SCHEMA: ActionSchema = _ENTRIES[LIST_AUDIO_INPUTS][0]
#: Native handler for ``list_audio_inputs``.
LIST_AUDIO_INPUTS_HANDLER: ActionHandler = _ENTRIES[LIST_AUDIO_INPUTS][1]

#: Schema for ``capture_audio``.
CAPTURE_AUDIO_SCHEMA: ActionSchema = _ENTRIES[CAPTURE_AUDIO][0]
#: Native handler for ``capture_audio``.
CAPTURE_AUDIO_HANDLER: ActionHandler = _ENTRIES[CAPTURE_AUDIO][1]

#: Schema for ``capture_transcription``.
CAPTURE_TRANSCRIPTION_SCHEMA: ActionSchema = _ENTRIES[CAPTURE_TRANSCRIPTION][0]
#: Native handler for ``capture_transcription``.
CAPTURE_TRANSCRIPTION_HANDLER: ActionHandler = _ENTRIES[CAPTURE_TRANSCRIPTION][
    1
]

#: Schema for ``transcribe_audio``.
TRANSCRIBE_AUDIO_SCHEMA: ActionSchema = _ENTRIES[TRANSCRIBE_AUDIO][0]
#: Native handler for ``transcribe_audio``.
TRANSCRIBE_AUDIO_HANDLER: ActionHandler = _ENTRIES[TRANSCRIBE_AUDIO][1]

#: The four (schema, handler) pairs, in protocol order.
AUDIO_ACTIONS: tuple[tuple[ActionSchema, ActionHandler], ...] = (
    (LIST_AUDIO_INPUTS_SCHEMA, LIST_AUDIO_INPUTS_HANDLER),
    (CAPTURE_AUDIO_SCHEMA, CAPTURE_AUDIO_HANDLER),
    (CAPTURE_TRANSCRIPTION_SCHEMA, CAPTURE_TRANSCRIPTION_HANDLER),
    (TRANSCRIBE_AUDIO_SCHEMA, TRANSCRIBE_AUDIO_HANDLER),
)

del _ENTRIES


def register(registry: ActionRegistry) -> None:
    """Register all four audio Actions on ``registry``.

    Installs ``list_audio_inputs``, ``capture_audio``,
    ``capture_transcription`` and ``transcribe_audio`` (schema + native
    handler) and ensures the audio value-type serializers are available.
    Raises if a name is already registered.
    """
    for schema, handler in AUDIO_ACTIONS:
        registry.register(schema.name, schema, handler)


for _class in (AudioControlEvent, AudioCaptureEvent, TranscriptionEvent):
    _class.__module__ = __name__


__all__ = [
    "AUDIO_ACTIONS",
    "CAPTURE_AUDIO",
    "CAPTURE_AUDIO_HANDLER",
    "CAPTURE_AUDIO_SCHEMA",
    "CAPTURE_TRANSCRIPTION",
    "CAPTURE_TRANSCRIPTION_HANDLER",
    "CAPTURE_TRANSCRIPTION_SCHEMA",
    "LIST_AUDIO_INPUTS",
    "LIST_AUDIO_INPUTS_HANDLER",
    "LIST_AUDIO_INPUTS_SCHEMA",
    "TRANSCRIBE_AUDIO",
    "TRANSCRIBE_AUDIO_HANDLER",
    "TRANSCRIBE_AUDIO_SCHEMA",
    "AudioCaptureEvent",
    "AudioControlEvent",
    "TranscriptionEvent",
    "register",
]

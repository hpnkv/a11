"""Action-based interface to the audio SDK.

Three native (C++) Actions wrap the capture and recognition primitives and can
be registered on any :class:`a11.ActionRegistry`:

* ``list_audio_inputs`` streams one :class:`~a11.sdk.audio.AudioDeviceInfo` per
  available input device;
* ``capture_audio`` streams :class:`~a11.sdk.audio.AudioBuffer` blocks with a
  control input that accepts a stop command;
* ``capture_transcription`` streams recognized text pieces, reusing the capture
  path and driving whisper.cpp recognition;
* ``transcribe_audio`` streams recognized text pieces from a caller-supplied
  :class:`~a11.sdk.audio.AudioBuffer` stream instead of a device, stopping on
  input closure or cancellation.

Call :func:`register` to install all three on a registry; their handlers and
value-type serialization live in C++, and each port's ``typeinfo`` is wired to
the matching native audio class so schema-driven tooling sees the right types.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from a11 import _native
from a11._native_options import install_native_options
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
register_json_codec(AudioControlEvent, "a11.sdk.audio.AudioControlEvent")
register_json_codec(AudioCaptureEvent, "a11.sdk.audio.AudioCaptureEvent")
register_json_codec(TranscriptionEvent, "a11.sdk.audio.TranscriptionEvent")

_native_register_audio_actions = _native.register_audio_actions


def register(registry: ActionRegistry) -> None:
    """Register the three audio Actions on ``registry``.

    Installs ``list_audio_inputs``, ``capture_audio`` and
    ``capture_transcription`` (schema + native handler) and ensures the audio
    value-type serializers are available. Raises if a name is already
    registered.
    """
    _native_register_audio_actions(registry)


for _class in (AudioControlEvent, AudioCaptureEvent, TranscriptionEvent):
    _class.__module__ = __name__


__all__ = [
    "CAPTURE_AUDIO",
    "CAPTURE_TRANSCRIPTION",
    "LIST_AUDIO_INPUTS",
    "TRANSCRIBE_AUDIO",
    "AudioCaptureEvent",
    "AudioControlEvent",
    "TranscriptionEvent",
    "register",
]

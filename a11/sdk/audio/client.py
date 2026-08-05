"""Asyncio-shaped access to A11's native PortAudio input capture.

The public types here *are* the classes exported by the native extension (see
``AGENTS.md``, "Python boundary"); this module only attaches the idiomatic
asynchronous surface — awaitable reads, async iteration, and context-manager
lifetimes — onto them. Buffers expose their samples through the Python buffer
protocol, so ``memoryview(buffer)`` is a zero-copy ``(channels, frames)`` view
and ``buffer.channel(i)`` is one channel's row.
"""

from __future__ import annotations

import json
import os
from collections.abc import Awaitable, Callable, Mapping
from typing import Any, Self

from a11 import _native
from a11._native_options import install_native_options
from a11._native_protocol import attach_protocol
from a11.data.serialization import (
    get_global_serialization_registry,
    set_global_type_tag,
)
from a11.status import Status, StatusCode, StatusException

from a11._native import AudioBuffer
from a11._native import AudioDeviceInfo
from a11._native import AudioInput
from a11._native import AudioInputOptions
from a11._native import AudioSubscription
from a11._native import SpeechRecognizer
from a11._native import SpeechRecognizerOptions

OnTranscription = Callable[[str | None], Awaitable[None]]
OnRecognitionDone = Callable[[], Awaitable[None]]

install_native_options(
    AudioInputOptions,
    {
        "device_index": (int, -1),
        "device_name": (str, ""),
        "sample_rate": (float, 0.0),
        "channels": (int, 0),
        "block_frames": (int, 256),
        "ring_blocks": (int, 32),
        "buffer_frames": (int, 0),
    },
)

install_native_options(
    SpeechRecognizerOptions,
    {
        "model_path": (str, ""),
        "language": (str, "auto"),
        "translate": (bool, False),
        "inference_threads": (int, 0),
        "use_gpu": (bool, True),
        "flash_attention": (bool, True),
        "use_context": (bool, False),
        "initial_prompt": (str, ""),
        "subscription_buffer_millis": (int, 100),
        "vad_threshold": (float, 0.01),
        "vad_noise_ratio": (float, 2.5),
        "vad_window_millis": (int, 20),
        "min_speech_millis": (int, 250),
        "min_silence_millis": (int, 600),
        "speech_pad_millis": (int, 160),
        "max_speech_seconds": (int, 30),
        "vad_model_path": (str, ""),
        "silero_threshold": (float, 0.5),
    },
)

def register_json_codec(cls: type, tag: str) -> None:
    """Register a JSON codec for ``cls`` on the global Python registry.

    Mirrors the native C++ codec so an audio value serialized by one language
    (with the tag ``application/json;type=<tag>``) round-trips through the
    other. ``cls`` must expose ``model_dump``/``model_validate`` (installed by
    :func:`install_native_options`). Idempotent across repeated imports.
    """
    registry = get_global_serialization_registry()
    set_global_type_tag(cls, tag)

    def _serialize(obj: Any, _cls: type = cls) -> str:
        return json.dumps(obj.model_dump())

    def _deserialize(chunk: Any, _cls: type = cls) -> Any:
        data = chunk.data
        if isinstance(data, (bytes, bytearray)):
            data = bytes(data).decode("utf-8")
        return _cls.model_validate(json.loads(data))

    try:
        registry.register(
            cls, "application/json", _serialize, _deserialize,
            receives_chunk=True,
        )
    except StatusException:
        pass  # already registered in this process


register_json_codec(AudioInputOptions, "a11.sdk.audio.AudioInputOptions")
register_json_codec(
    SpeechRecognizerOptions, "a11.sdk.audio.SpeechRecognizerOptions"
)


def _register_audio_buffer_codec() -> None:
    """Register the MessagePack codec for AudioBuffer on the global registry.

    Both directions delegate to the native encoder/decoder so the bytes are
    byte-for-byte identical to what C++ produces and consumes (samples as a
    little-endian float32 blob).
    """
    registry = get_global_serialization_registry()
    set_global_type_tag(AudioBuffer, "a11.sdk.audio.AudioBuffer")

    def _serialize(buffer: AudioBuffer) -> bytes:
        return _native.audio_buffer_to_msgpack(buffer)

    def _deserialize(chunk: Any) -> AudioBuffer:
        data = chunk.data
        if isinstance(data, (bytes, bytearray)):
            data = bytes(data)
        return _native.audio_buffer_from_msgpack(data)

    try:
        registry.register(
            AudioBuffer, "application/x-msgpack", _serialize, _deserialize,
            receives_chunk=True,
        )
    except StatusException:
        pass  # already registered in this process


_register_audio_buffer_codec()

_native_list_devices = _native.list_audio_devices
_native_default_input_device = _native.default_audio_input_device
_native_device_info = _native.audio_device_info

_native_input_init = AudioInput.__init__
_native_subscribe = AudioInput.subscribe
_native_read = AudioSubscription.read
_native_close = AudioSubscription.close

_native_recognizer_init = SpeechRecognizer.__init__
_native_recognizer_start = SpeechRecognizer.start
_native_recognizer_stop = SpeechRecognizer.stop


def list_devices() -> list[AudioDeviceInfo]:
    """Return metadata for every audio device, in index order."""
    return _native_list_devices()


def default_input_device() -> AudioDeviceInfo:
    """Return metadata for the host's default input device."""
    return _native_default_input_device()


def device_info(index: int) -> AudioDeviceInfo:
    """Return metadata for the audio device at ``index``."""
    if not isinstance(index, int) or isinstance(index, bool):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="index must be an integer.",
        ).to_exception()
    return _native_device_info(index)


class _AudioInputProtocol:
    """Typed surface over the native AudioInput capture device."""

    def __init__(
        self,
        options: AudioInputOptions | Mapping[str, Any] | None = None,
    ) -> None:
        """Resolve the device and validate options without starting capture.

        A plain mapping is validated into the same bound options object used by
        C++, rather than creating a second Python configuration model.
        """
        if options is None:
            options = AudioInputOptions()
        elif not isinstance(options, AudioInputOptions):
            options = AudioInputOptions.model_validate(options)
        _native_input_init(self, options)

    @staticmethod
    def open(
        options: AudioInputOptions | Mapping[str, Any] | None = None,
    ) -> AudioInput:
        """Resolve the device and validate options without starting capture."""
        return AudioInput(options)

    def subscribe(self, buffer_size: int) -> AudioSubscription:
        """Begin receiving buffers of ``buffer_size`` frames per channel.

        Capture starts when the first subscription is created and stops when
        the last one is closed.
        """
        if not isinstance(buffer_size, int) or isinstance(buffer_size, bool):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="buffer_size must be an integer.",
            ).to_exception()
        return _native_subscribe(self, buffer_size)


class _AudioSubscriptionProtocol:
    """Typed asyncio surface over one native subscription's buffer stream."""

    async def read(self) -> AudioBuffer:
        """Await the next captured buffer for this subscription."""
        return await _native_read(self)

    def close(self) -> None:
        """Stop delivering; stops capture if this was the last subscription."""
        _native_close(self)

    async def __aenter__(self) -> Self:
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.close()

    def __aiter__(self) -> Self:
        return self

    async def __anext__(self) -> AudioBuffer:
        try:
            return await self.read()
        except StatusException as error:
            if error.status.code == StatusCode.OUT_OF_RANGE:
                raise StopAsyncIteration from None
            raise


class _AudioBufferProtocol:
    """Sample-access helpers over a native captured buffer."""

    @property
    def samples(self) -> memoryview:
        """A zero-copy read-only ``(channels, frames)`` float view."""
        return memoryview(self)

    def channel(self, index: int) -> memoryview:
        """Return a zero-copy read-only view of one channel's samples."""
        if not 0 <= index < self.num_channels:
            raise IndexError(
                f"channel {index} out of range [0, {self.num_channels})"
            )
        # The buffer is channel-major, so each channel is a contiguous slice of
        # the flattened float view; slicing keeps the buffer alive, zero-copy.
        frames = self.num_frames
        flat = memoryview(self).cast("B").cast("f")  # 2-D 'f' -> 1-D 'f'
        return flat[index * frames : (index + 1) * frames]


class _SpeechRecognizerProtocol:
    """Typed asyncio surface over native whisper.cpp recognition."""

    def __init__(
        self,
        model_path: str | os.PathLike[str],
        source: AudioInput | AudioSubscription | None = None,
        options: SpeechRecognizerOptions | Mapping[str, Any] | None = None,
    ) -> None:
        """Load a whisper.cpp GGML/GGUF model for local recognition.

        Pass an :class:`AudioInput` to create a fresh subscription per run, an
        :class:`AudioSubscription` to consume that subscription once, or
        ``None`` to use the system default input. Loading happens once, so the
        same recognizer can be paused while an agent responds and restarted for
        the next user turn without reloading model weights.
        """
        if options is None:
            options = SpeechRecognizerOptions()
        elif not isinstance(options, SpeechRecognizerOptions):
            options = SpeechRecognizerOptions.model_validate(options)
        _native_recognizer_init(self, os.fspath(model_path), source, options)

    @staticmethod
    def create(
        model_path: str | os.PathLike[str],
        source: AudioInput | AudioSubscription | None = None,
        options: SpeechRecognizerOptions | Mapping[str, Any] | None = None,
    ) -> SpeechRecognizer:
        """Load a model and construct a recognizer."""
        return SpeechRecognizer(model_path, source, options)

    async def start(
        self,
        on_transcription: OnTranscription,
        on_done: OnRecognitionDone,
    ) -> None:
        """Start capture and deliver awaited transcription callbacks.

        ``on_transcription`` receives non-empty pieces as speech endpoints are
        decoded, then exactly one ``None``. ``on_done`` follows that terminal
        marker. Silence does not invoke whisper or produce a piece.
        """
        await _native_recognizer_start(self, on_transcription, on_done)

    async def stop(self) -> None:
        """Stop capture and await the terminal piece and done callbacks."""
        await _native_recognizer_stop(self)


attach_protocol(AudioInput, _AudioInputProtocol)
attach_protocol(AudioSubscription, _AudioSubscriptionProtocol)
attach_protocol(AudioBuffer, _AudioBufferProtocol)
attach_protocol(SpeechRecognizer, _SpeechRecognizerProtocol)

for _class in (
    AudioBuffer,
    AudioDeviceInfo,
    AudioInput,
    AudioInputOptions,
    AudioSubscription,
    SpeechRecognizer,
    SpeechRecognizerOptions,
):
    _class.__module__ = __name__


__all__ = [
    "AudioBuffer",
    "AudioDeviceInfo",
    "AudioInput",
    "AudioInputOptions",
    "AudioSubscription",
    "OnRecognitionDone",
    "OnTranscription",
    "SpeechRecognizer",
    "SpeechRecognizerOptions",
    "default_input_device",
    "device_info",
    "list_devices",
]

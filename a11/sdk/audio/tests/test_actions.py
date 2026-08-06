"""Tests for the Action-based interface to the audio SDK.

These exercise the schema/typeinfo wiring and the serializable value types; they
do not open a capture stream, so they run everywhere.
"""

from __future__ import annotations

import array
import asyncio

import pytest

import a11
from a11.data.serialization import get_global_serialization_registry
from a11.status import StatusCode, StatusException
from a11.sdk.audio import actions
from a11.sdk.audio.client import (
    AudioBuffer,
    AudioDeviceInfo,
    AudioInputOptions,
    SpeechRecognizerOptions,
)
from a11.sdk.audio.actions import (
    AudioCaptureEvent,
    AudioControlEvent,
    TranscriptionEvent,
)


def _registry() -> a11.ActionRegistry:
    registry = a11.ActionRegistry()
    actions.register(registry)
    return registry


def test_register_installs_three_actions() -> None:
    registry = _registry()
    for name in (
        actions.LIST_AUDIO_INPUTS,
        actions.CAPTURE_AUDIO,
        actions.CAPTURE_TRANSCRIPTION,
    ):
        assert registry.is_registered(name)
        registry.get_schema(name).validate()  # raises if invalid


def test_list_audio_inputs_typeinfo() -> None:
    schema = _registry().get_schema(actions.LIST_AUDIO_INPUTS)
    assert schema.outputs["inputs"].typeinfo is AudioDeviceInfo
    assert not schema.outputs["inputs"].unary


def test_capture_audio_typeinfo() -> None:
    schema = _registry().get_schema(actions.CAPTURE_AUDIO)
    assert schema.inputs["options"].typeinfo is AudioInputOptions
    assert schema.inputs["options"].required
    assert schema.inputs["options"].unary
    assert schema.inputs["control_events"].typeinfo is AudioControlEvent
    assert schema.outputs["audio"].typeinfo is AudioBuffer
    assert schema.outputs["events"].typeinfo is AudioCaptureEvent


def test_capture_transcription_typeinfo() -> None:
    schema = _registry().get_schema(actions.CAPTURE_TRANSCRIPTION)
    assert schema.inputs["capture_options"].typeinfo is AudioInputOptions
    assert not schema.inputs["capture_options"].required
    assert schema.inputs["asr_options"].typeinfo is SpeechRecognizerOptions
    assert schema.inputs["control_events"].typeinfo is AudioControlEvent
    # Plain text pieces are typed as str.
    assert schema.outputs["transcription_pieces"].typeinfo is str
    assert schema.outputs["events"].typeinfo is TranscriptionEvent


def test_transcribe_audio_typeinfo() -> None:
    schema = _registry().get_schema(actions.TRANSCRIBE_AUDIO)
    assert schema.inputs["audio"].typeinfo is AudioBuffer
    assert schema.inputs["audio"].required
    assert not schema.inputs["audio"].unary
    assert schema.inputs["asr_options"].typeinfo is SpeechRecognizerOptions
    assert not schema.inputs["asr_options"].required
    assert schema.outputs["transcription_pieces"].typeinfo is str
    assert schema.outputs["events"].typeinfo is TranscriptionEvent


def test_audio_buffer_from_bytes() -> None:
    raw = array.array("f", [0.0, 0.25, -0.5, 1.0]).tobytes()
    buffer = AudioBuffer(raw, sample_rate=16000.0)
    assert buffer.num_channels == 1
    assert buffer.num_frames == 4
    assert buffer.sample_rate == 16000.0
    assert list(buffer.channel(0)) == [0.0, 0.25, -0.5, 1.0]


def test_audio_buffer_from_float_array_stereo() -> None:
    samples = array.array("f", [0.0, 0.1, 0.2, 0.3, 0.4, 0.5])
    buffer = AudioBuffer(samples, sample_rate=8000.0, num_channels=2)
    assert buffer.num_channels == 2
    assert buffer.num_frames == 3


def test_audio_buffer_msgpack_round_trip() -> None:
    registry = get_global_serialization_registry()
    buffer = AudioBuffer(
        array.array("f", [0.1, 0.2, 0.3, 0.4]), sample_rate=16000.0
    )
    chunk = registry.to_chunk(buffer)
    assert "type=a11.sdk.AudioBuffer" in chunk.metadata.mimetype
    decoded = registry.from_chunk(chunk, obj_type=AudioBuffer)
    assert decoded.num_frames == 4
    assert decoded.sample_rate == 16000.0
    got = list(decoded.channel(0))
    assert len(got) == 4
    for value, expected in zip(got, (0.1, 0.2, 0.3, 0.4)):
        assert abs(value - expected) < 1e-6


def test_transcribe_audio_requires_model() -> None:
    async def run():
        registry = _registry()
        action = registry.make_action(actions.TRANSCRIBE_AUDIO)
        await action.get_input("audio").put(
            AudioBuffer(array.array("f", [0.0] * 320), sample_rate=16000.0),
            final=True,
        )
        await action.get_input("asr_options").put_null_final()
        action.run_in_background()
        try:
            await action.wait()
        except Exception:  # noqa: BLE001 - status surfaced below
            pass
        return action.get_status()

    status = asyncio.run(run())
    assert status is not None
    assert status.code == StatusCode.INVALID_ARGUMENT


def test_audio_schemas_declare_deadline_header() -> None:
    registry = _registry()
    for name in (
        actions.LIST_AUDIO_INPUTS,
        actions.CAPTURE_AUDIO,
        actions.CAPTURE_TRANSCRIPTION,
        actions.TRANSCRIBE_AUDIO,
    ):
        assert "x-a11-deadline" in registry.get_schema(name).headers


def test_deadline_header_ms_and_ns_semantics() -> None:
    action = _registry().make_action(actions.TRANSCRIBE_AUDIO)
    # Bare value is milliseconds since the epoch.
    action.set_header("x-a11-deadline", b"1500")
    assert a11.get_deadline(action).nanoseconds_since_epoch == 1500 * 1_000_000
    # An 'ns' suffix is nanoseconds since the epoch.
    action.set_header("x-a11-deadline", b"1500ns")
    assert a11.get_deadline(action).nanoseconds_since_epoch == 1500
    # Absent header means no deadline.
    action.remove_header("x-a11-deadline")
    assert a11.get_deadline(action) == a11.infinite_future()


def test_set_deadline_header_round_trips_at_ms() -> None:
    action = _registry().make_action(actions.TRANSCRIBE_AUDIO)
    deadline = a11.Time.from_nanoseconds_since_epoch(1_700_000_000_000 * 1_000_000)
    a11.set_deadline_header(action, deadline)
    assert action.get_header("x-a11-deadline", decode=True) == "1700000000000"
    assert a11.get_deadline(action) == deadline
    # Infinite clears the header.
    a11.set_deadline_header(action, a11.infinite_future())
    assert action.get_header("x-a11-deadline") is None


def test_deadline_header_rejects_malformed() -> None:
    action = _registry().make_action(actions.TRANSCRIBE_AUDIO)
    action.set_header("x-a11-deadline", b"not-a-number")
    with pytest.raises(StatusException) as excinfo:
        a11.get_deadline(action)
    assert excinfo.value.status.code == StatusCode.INVALID_ARGUMENT


def test_transcribe_audio_deadline_already_passed() -> None:
    async def run():
        registry = _registry()
        action = registry.make_action(actions.TRANSCRIBE_AUDIO)
        action.set_header("x-a11-deadline", b"1")  # 1 ms since epoch: long past
        await action.get_input("asr_options").put_null_final()
        await action.get_input("audio").put_null_final()
        action.run_in_background()
        try:
            await action.wait()
        except Exception:  # noqa: BLE001
            pass
        return action.get_status()

    status = asyncio.run(run())
    assert status.code == StatusCode.DEADLINE_EXCEEDED


def test_make_action_from_registry() -> None:
    registry = _registry()
    action = registry.make_action(actions.CAPTURE_AUDIO)
    assert action.get_schema().name == actions.CAPTURE_AUDIO


def test_control_event_surface() -> None:
    assert AudioControlEvent().command == "stop"
    assert AudioControlEvent.stop().command == "stop"
    assert AudioControlEvent(command="stop") == AudioControlEvent.stop()
    assert AudioControlEvent.model_validate({"command": "stop"}).command == "stop"
    assert AudioControlEvent().model_dump() == {"command": "stop"}


def test_capture_event_surface() -> None:
    dropped = AudioCaptureEvent.buffers_dropped(3)
    assert dropped.kind == "buffers_dropped"
    assert dropped.dropped == 3
    assert AudioCaptureEvent.started().kind == "started"
    assert AudioCaptureEvent.stopped().kind == "stopped"
    assert AudioCaptureEvent.model_validate(
        {"kind": "buffers_dropped", "dropped": 7}
    ).dropped == 7


def test_transcription_event_surface() -> None:
    assert TranscriptionEvent().kind == "capture_started"
    assert TranscriptionEvent.inference_started().kind == "inference_started"
    assert (
        TranscriptionEvent.model_validate({"kind": "inference_stopped"}).kind
        == "inference_stopped"
    )


def test_audio_input_options_new_fields() -> None:
    options = AudioInputOptions.model_validate(
        {"device_name": "Mic", "buffer_frames": 512}
    )
    assert options.device_name == "Mic"
    assert options.buffer_frames == 512
    # Defaults omitted from a compact dump.
    dumped = AudioInputOptions().model_dump()
    assert dumped["device_name"] == ""
    assert dumped["buffer_frames"] == 0


def test_speech_recognizer_options_model_path() -> None:
    options = SpeechRecognizerOptions.model_validate({"model_path": "/m.bin"})
    assert options.model_path == "/m.bin"
    assert options.model_dump()["model_path"] == "/m.bin"

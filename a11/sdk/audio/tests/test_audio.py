"""Tests for the PortAudio-backed audio input SDK.

The device-independent behaviour (enumeration, option validation, error
propagation) runs everywhere. Anything that opens a real capture stream is
skipped unless a default input device is present, and live sample reads only
run when ``A11_AUDIO_LIVE_TEST`` is set, since CI hosts have no microphone.
"""

from __future__ import annotations

import asyncio
import os

import pytest

from a11.sdk.audio.client import (
    AudioBuffer,
    AudioDeviceInfo,
    AudioInput,
    AudioInputOptions,
    SpeechRecognizerOptions,
    default_input_device,
    device_info,
    list_devices,
)
from a11.status import StatusCode, StatusException


def _has_input_device() -> bool:
    return any(d.max_input_channels > 0 for d in list_devices())


def test_list_devices_returns_metadata():
    devices = list_devices()
    assert isinstance(devices, list)
    for device in devices:
        assert isinstance(device, AudioDeviceInfo)
        assert isinstance(device.name, str)


def test_options_validate_from_mapping():
    options = AudioInputOptions.model_validate(
        {
            "sample_rate": 44100.0,
            "channels": 1,
        }
    )
    assert options.sample_rate == 44100.0
    assert options.channels == 1
    assert options.block_frames == 256  # default preserved


def test_options_reject_tiny_block():
    with pytest.raises(StatusException) as excinfo:
        AudioInputOptions(block_frames=8)
    assert excinfo.value.status.code == StatusCode.INVALID_ARGUMENT


def test_speech_recognizer_options_validate_from_mapping():
    options = SpeechRecognizerOptions.model_validate(
        {
            "language": "en",
            "min_silence_millis": 400,
        }
    )
    assert options.language == "en"
    assert options.min_silence_millis == 400
    assert options.use_gpu is True


def test_speech_recognizer_options_reject_invalid_vad():
    with pytest.raises(StatusException) as excinfo:
        SpeechRecognizerOptions(vad_threshold=0.0)
    assert excinfo.value.status.code == StatusCode.INVALID_ARGUMENT


def test_device_info_negative_index_raises():
    with pytest.raises(StatusException) as excinfo:
        device_info(-1)
    assert excinfo.value.status.code == StatusCode.OUT_OF_RANGE


def test_default_input_device_when_present():
    if not _has_input_device():
        pytest.skip("No input device on this host")
    device = default_input_device()
    assert isinstance(device, AudioDeviceInfo)
    assert device.index >= 0


def test_subscribe_rejects_small_buffer():
    if not _has_input_device():
        pytest.skip("No input device on this host")
    audio_input = AudioInput()
    with pytest.raises(StatusException) as excinfo:
        audio_input.subscribe(16)
    assert excinfo.value.status.code == StatusCode.INVALID_ARGUMENT
    assert not audio_input.capturing


def test_input_exposes_metadata():
    if not _has_input_device():
        pytest.skip("No input device on this host")
    audio_input = AudioInput()
    assert audio_input.channels > 0
    assert audio_input.sample_rate > 0
    assert not audio_input.capturing


@pytest.mark.asyncio
async def test_live_capture_delivers_buffers():
    if not os.environ.get("A11_AUDIO_LIVE_TEST"):
        pytest.skip("Set A11_AUDIO_LIVE_TEST to exercise real capture")
    if not _has_input_device():
        pytest.skip("No input device on this host")

    audio_input = AudioInput()
    subscription = audio_input.subscribe(256)
    try:
        assert audio_input.capturing
        buffer = await asyncio.wait_for(subscription.read(), timeout=5.0)
        assert isinstance(buffer, AudioBuffer)
        assert buffer.num_frames == 256
        assert buffer.num_channels == audio_input.channels

        view = memoryview(buffer)
        assert view.shape == (buffer.num_channels, buffer.num_frames)
        channel0 = buffer.channel(0)
        assert len(channel0) == buffer.num_frames
    finally:
        subscription.close()
    assert not audio_input.capturing


@pytest.mark.asyncio
async def test_read_after_close_stops_iteration():
    if not os.environ.get("A11_AUDIO_LIVE_TEST"):
        pytest.skip("Set A11_AUDIO_LIVE_TEST to exercise real capture")
    if not _has_input_device():
        pytest.skip("No input device on this host")

    audio_input = AudioInput()
    subscription = audio_input.subscribe(256)
    subscription.close()
    with pytest.raises(StatusException) as excinfo:
        await asyncio.wait_for(subscription.read(), timeout=2.0)
    assert excinfo.value.status.code == StatusCode.OUT_OF_RANGE

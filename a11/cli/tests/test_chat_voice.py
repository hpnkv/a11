# Copyright 2026 The A11 Authors.

"""Offline model-cache and turn-scoping tests for ``a11 chat`` voice input."""

from __future__ import annotations

import hashlib
import io

import pytest
from rich.console import Console

from a11.cli.backends import PROVIDERS
from a11.cli.chat_ui import ChatUI
from a11.cli.voice import VOICE_MODELS, VoiceModel, ensure_voice_model


def _console() -> tuple[Console, io.StringIO]:
    output = io.StringIO()
    return Console(file=output, force_terminal=False), output


def test_verified_cached_model_skips_download(tmp_path, monkeypatch):
    payload = b"small fake whisper model"
    digest = hashlib.sha1(payload, usedforsecurity=False).hexdigest()
    model = VoiceModel("test", digest, 1)
    monkeypatch.setitem(VOICE_MODELS, "test", model)
    destination = tmp_path / model.filename
    destination.write_bytes(payload)

    def fail_download(*args, **kwargs):
        del args, kwargs
        raise AssertionError("cached model should not be downloaded")

    monkeypatch.setattr("urllib.request.urlopen", fail_download)
    console, output = _console()
    assert (
        ensure_voice_model("test", console, cache_dir=tmp_path) == destination
    )
    assert "cached" in output.getvalue()


def test_download_is_verified_and_moved_into_cache(tmp_path, monkeypatch):
    payload = b"downloaded fake whisper model"
    digest = hashlib.sha1(payload, usedforsecurity=False).hexdigest()
    model = VoiceModel("test", digest, 1)
    monkeypatch.setitem(VOICE_MODELS, "test", model)

    class Response(io.BytesIO):
        headers = {"Content-Length": str(len(payload))}

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, traceback):
            del exc_type, exc, traceback
            self.close()

    monkeypatch.setattr(
        "urllib.request.urlopen",
        lambda request, timeout: Response(payload),
    )
    console, output = _console()
    destination = ensure_voice_model("test", console, cache_dir=tmp_path)

    assert destination.read_bytes() == payload
    assert "downloading voice model" in output.getvalue()


@pytest.mark.asyncio
async def test_transcriptions_only_edit_an_active_user_prompt():
    ui = ChatUI(
        PROVIDERS["ollama"],
        "test-model",
        shell_tools=False,
        voice=True,
    )

    class FakeRecognizer:
        running = False
        callback = None

        async def start(self, on_transcription, on_done):
            del on_done
            self.running = True
            self.callback = on_transcription

        async def stop(self):
            self.running = False

    recognizer = FakeRecognizer()
    ui._recognizer = recognizer
    await ui._start_voice_input()
    assert recognizer.callback is not None

    await recognizer.callback("hello world")
    assert ui._session.default_buffer.text == "hello world"

    await ui._stop_voice_input()
    await recognizer.callback("ignored while the LLM responds")
    assert ui._session.default_buffer.text == "hello world"

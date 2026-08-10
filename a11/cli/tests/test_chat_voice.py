# Copyright 2026 The A11 Authors.

"""Offline model-cache and turn-scoping tests for ``a11 chat`` voice input."""

from __future__ import annotations

import io

import pytest
from rich.console import Console

import a11
from a11.cli.backends import PROVIDERS
from a11.client.connection import GatewayConnection
from a11.cli.chat_ui import ChatUI
from a11.cli.voice import (
    DEFAULT_VOICE_MODEL,
    VAD_MODEL,
    VOICE_MODELS,
    ensure_voice_model,
    voice_cache_dir,
)
from a11.status import StatusCode, StatusException


def _console() -> tuple[Console, io.StringIO]:
    output = io.StringIO()
    return Console(file=output, force_terminal=False), output


def test_the_model_table_comes_from_the_native_registry():
    """One table, so the CLI's choices and the gateway's cannot drift.

    Downloading, verifying and the atomic rename are the native registry's job
    and are covered by the C++ suites; what has to hold here is that this module
    reports the same catalogue the actions accept.
    """
    assert set(VOICE_MODELS) == {"tiny", "tiny.en", "base", "base.en"}
    assert DEFAULT_VOICE_MODEL in VOICE_MODELS
    base = VOICE_MODELS["base.en"]
    assert base.filename == "ggml-base.en.bin"
    assert base.url.endswith("/ggml-base.en.bin")
    assert len(base.sha1) == 40
    assert VAD_MODEL.name == "silero-v5.1.2"


def test_the_cache_directory_is_the_one_the_cli_has_always_used():
    # Not XDG-derived: a second spelling would re-download every model a user
    # already has.
    assert voice_cache_dir().parts[-3:] == (".cache", "a11", "audio")


@pytest.mark.asyncio
async def test_an_existing_file_is_accepted_as_a_model(tmp_path):
    """A path is as good as a shorthand, and reaches no network."""
    model = tmp_path / "hand-built.bin"
    model.write_bytes(b"not really a whisper model")
    console, _ = _console()

    assert await ensure_voice_model(str(model), console) == model


@pytest.mark.asyncio
async def test_an_unknown_shorthand_names_the_valid_ones(tmp_path):
    console, _ = _console()
    with pytest.raises(StatusException) as caught:
        await ensure_voice_model("enormous.en", console)
    assert caught.value.status.code == StatusCode.INVALID_ARGUMENT
    # The message has to be actionable, since the flag accepts free-form paths
    # too and so cannot be validated by argparse alone.
    assert "tiny.en" in caught.value.status.message


@pytest.mark.asyncio
async def test_transcriptions_only_edit_an_active_user_prompt():
    class _Session:
        action_registry = a11.ActionRegistry()
        node_map = None

    ui = ChatUI(
        PROVIDERS["ollama"],
        "test-model",
        GatewayConnection(_Session(), None, embedded=True),
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

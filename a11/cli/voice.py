# Copyright 2026 The A11 Authors.

"""Visible, integrity-checked whisper.cpp model downloads for ``a11 chat``.

The model table, the cache directory and the verified-atomic download all live
in C++ (``cpp/sdk/audio/model_registry.h``), because the actions that need them
run wherever the gateway runs. What is left here is the part that is genuinely a
CLI concern: rendering progress with `rich`.
"""

from __future__ import annotations

from pathlib import Path

from a11 import _native
from a11._native import AudioModelSpec
from rich.console import Console
from rich.progress import (
    BarColumn,
    DownloadColumn,
    Progress,
    TextColumn,
    TimeRemainingColumn,
    TransferSpeedColumn,
)

AudioModelSpec.__module__ = __name__

#: The transcription models `a11 chat` accepts, keyed by shorthand. Sourced from
#: the native registry so the CLI's ``--voice-model`` choices and the gateway's
#: accepted shorthands cannot drift apart.
VOICE_MODELS: dict[str, AudioModelSpec] = {
    name: _native.lookup_asr_model(name)
    for name in _native.asr_model_shorthands()
}

#: The default transcription model: small enough that a first run is not a wait.
DEFAULT_VOICE_MODEL: str = _native.DEFAULT_ASR_MODEL

#: whisper.cpp's Silero VAD model. It gates each endpointed utterance so the
#: decoder never runs on energy-gate false positives.
VAD_MODEL: AudioModelSpec = _native.lookup_vad_model(_native.DEFAULT_VAD_MODEL)


def voice_cache_dir() -> Path:
    """Return the directory shorthand models are cached in."""
    return Path(_native.audio_model_cache_dir())


async def ensure_voice_model(name: str, console: Console) -> Path:
    """Return a verified transcription-model path, downloading if needed.

    Args:
        name: A shorthand from `VOICE_MODELS`, or a path to a model file.
        console: Console the progress bar is drawn on.

    Returns:
        The local model path.

    Raises:
        StatusException: When ``name`` is neither a known shorthand nor an
            existing file, or the download fails its digest check.
    """
    return await _resolve(_native.resolve_asr_model, name, console)


async def ensure_vad_model(console: Console) -> Path:
    """Return a verified Silero VAD model path, downloading if needed."""
    return await _resolve(_native.resolve_vad_model, VAD_MODEL.name, console)


async def _resolve(resolve, spec: str, console: Console) -> Path:
    """Resolve ``spec`` through ``resolve``, drawing a progress bar if it fetches.

    The bar is created lazily, on the first progress callback: a cache hit never
    reaches the network and should not flash an empty bar on its way past.
    """
    progress: Progress | None = None
    task = None

    def on_progress(done: int, total: int) -> None:
        nonlocal progress, task
        if progress is None:
            progress = Progress(
                TextColumn("[progress.description]{task.description}"),
                BarColumn(),
                DownloadColumn(),
                TransferSpeedColumn(),
                TimeRemainingColumn(),
                console=console,
            )
            progress.start()
            task = progress.add_task(spec, total=total or None)
        progress.update(task, completed=done, total=total or None)

    try:
        path = Path(await resolve(spec, on_progress))
    finally:
        if progress is not None:
            progress.stop()
    if progress is None:
        console.print(f"voice model: [bold]{spec}[/] · {path} (cached)",
                      style="dim")
    else:
        console.print(f"voice model ready: {path}", style="green")
    return path


__all__ = [
    "DEFAULT_VOICE_MODEL",
    "VAD_MODEL",
    "VOICE_MODELS",
    "AudioModelSpec",
    "ensure_vad_model",
    "ensure_voice_model",
    "voice_cache_dir",
]

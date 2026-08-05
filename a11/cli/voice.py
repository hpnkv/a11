# Copyright 2026 The A11 Authors.

"""Visible, integrity-checked whisper.cpp model downloads for ``a11 chat``."""

from __future__ import annotations

import dataclasses
import hashlib
import os
import urllib.request
from pathlib import Path

from rich.console import Console
from rich.progress import (
    BarColumn,
    DownloadColumn,
    Progress,
    TextColumn,
    TimeRemainingColumn,
    TransferSpeedColumn,
)

_MODEL_ROOT = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main"
_VAD_MODEL_ROOT = "https://huggingface.co/ggml-org/whisper-vad/resolve/main"


@dataclasses.dataclass(frozen=True)
class VoiceModel:
    """One supported whisper.cpp model artifact (transcription or VAD)."""

    name: str
    sha1: str
    size_mib: int
    # Repository the ``ggml-<name>.bin`` artifact is fetched from. Transcription
    # models live in the whisper.cpp repo; the Silero VAD model in whisper-vad.
    root: str = _MODEL_ROOT

    @property
    def filename(self) -> str:
        return f"ggml-{self.name}.bin"

    @property
    def url(self) -> str:
        return f"{self.root}/{self.filename}"


# Hashes are published by the upstream whisper.cpp model repository. Restrict
# chat to tiny/base so startup, local inference, and wheel testing remain sane.
VOICE_MODELS: dict[str, VoiceModel] = {
    "tiny": VoiceModel("tiny", "bd577a113a864445d4c299885e0cb97d4ba92b5f", 75),
    "tiny.en": VoiceModel(
        "tiny.en", "c78c86eb1a8faa21b369bcd33207cc90d64ae9df", 75
    ),
    "base": VoiceModel("base", "465707469ff3a37a2b9b8d8f89f2f99de7299dac", 142),
    "base.en": VoiceModel(
        "base.en", "137c40403d78fd54d454da0f9bd998f78703390c", 142
    ),
}

DEFAULT_VOICE_MODEL = "tiny.en"

# whisper.cpp's Silero VAD model, fetched from the whisper-vad repository. It
# gates each endpointed utterance so the decoder never runs on energy-gate
# false-positives. The hash is the upstream published SHA-1.
VAD_MODEL = VoiceModel(
    "silero-v5.1.2",
    "a372f48dcf0bd9e4330eef2802bc46e061c19634",
    1,
    root=_VAD_MODEL_ROOT,
)


def voice_cache_dir() -> Path:
    """Return the model cache required by the CLI contract."""
    return Path.home() / ".cache" / "a11" / "audio"


def _file_sha1(path: Path) -> str:
    digest = hashlib.sha1(usedforsecurity=False)
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_voice_model(
    name: str,
    console: Console,
    *,
    cache_dir: Path | None = None,
) -> Path:
    """Return a verified transcription-model path, downloading if needed."""
    try:
        model = VOICE_MODELS[name]
    except KeyError as error:
        choices = ", ".join(VOICE_MODELS)
        raise ValueError(
            f"unknown voice model {name!r}; choose from {choices}"
        ) from error
    return _ensure_model(model, console, cache_dir=cache_dir)


def ensure_vad_model(console: Console, *, cache_dir: Path | None = None) -> Path:
    """Return a verified Silero VAD model path, downloading if needed."""
    return _ensure_model(VAD_MODEL, console, cache_dir=cache_dir)


def _ensure_model(
    model: VoiceModel,
    console: Console,
    *,
    cache_dir: Path | None = None,
) -> Path:
    """Return a verified model path, downloading it with visible progress.

    A complete download is written beside the destination and atomically moved
    into place only after its upstream SHA-1 matches. Existing verified files
    are reused. A corrupt cache entry is left recoverable until its replacement
    is ready.
    """
    name = model.name
    directory = cache_dir or voice_cache_dir()
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / model.filename
    if destination.is_file() and _file_sha1(destination) == model.sha1:
        console.print(
            f"voice model: [bold]{name}[/] · {destination} (cached)",
            style="dim",
        )
        return destination

    if destination.exists():
        console.print(
            f"voice model cache is invalid; replacing {destination}",
            style="yellow",
            markup=False,
        )

    temporary = directory / f".{model.filename}.{os.getpid()}.download"
    console.print(
        f"downloading voice model [bold]{name}[/] "
        f"(~{model.size_mib} MiB) to {destination}"
    )
    request = urllib.request.Request(
        model.url,
        headers={"User-Agent": "a11-chat/voice-model"},
    )
    digest = hashlib.sha1(usedforsecurity=False)
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            content_length = response.headers.get("Content-Length")
            total = int(content_length) if content_length else None
            with (
                temporary.open("wb") as output,
                Progress(
                    TextColumn("[progress.description]{task.description}"),
                    BarColumn(),
                    DownloadColumn(),
                    TransferSpeedColumn(),
                    TimeRemainingColumn(),
                    console=console,
                ) as progress,
            ):
                task = progress.add_task(model.filename, total=total)
                while chunk := response.read(1024 * 1024):
                    output.write(chunk)
                    digest.update(chunk)
                    progress.update(task, advance=len(chunk))

        actual = digest.hexdigest()
        if actual != model.sha1:
            raise RuntimeError(
                f"downloaded {model.filename} has SHA-1 {actual}; "
                f"expected {model.sha1}"
            )
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    console.print(f"voice model ready: {destination}", style="green")
    return destination


__all__ = [
    "DEFAULT_VOICE_MODEL",
    "VAD_MODEL",
    "VOICE_MODELS",
    "VoiceModel",
    "ensure_vad_model",
    "ensure_voice_model",
    "voice_cache_dir",
]

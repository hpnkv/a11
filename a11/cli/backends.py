# Copyright 2026 The A11 Authors.

"""Provider metadata for the chat CLI.

Backend selection happens in the ``interact_with_llm`` action: the CLI hands it
a provider name via the ``x-a11-llm-provider`` header and the action routes to
the concrete ``interact_with_*`` handler, importing the SDK lazily. This module
holds the provider knowledge the CLI itself needs — the default model to
advertise and which environment variables hold the API key — plus a helper to
build a backend-neutral user message.
"""

from __future__ import annotations

import dataclasses
import os

import a11
from a11.sdk.llm import Interaction, Role


@dataclasses.dataclass(frozen=True)
class Provider:
    """CLI-facing knobs for one LLM provider."""

    name: str
    default_model: str
    api_key_env: tuple[str, ...]
    # Base URL for providers reached over a local/self-hosted server rather
    # than a hosted API (e.g. Ollama). Empty for the hosted backends.
    base_url: str = ""

    def api_key(self) -> str:
        """First non-empty value among `api_key_env`, or ``""``."""
        for env in self.api_key_env:
            if value := os.environ.get(env, ""):
                return value
        return ""


PROVIDERS: dict[str, Provider] = {
    "claude": Provider(
        name="claude",
        default_model="claude-sonnet-4-6",
        api_key_env=("ANTHROPIC_API_KEY",),
    ),
    "claude_code": Provider(
        name="claude_code",
        default_model="claude-sonnet-4-6",
        # The `claude` CLI holds the subscription this provider runs on.
        api_key_env=(),
    ),
    "gemini": Provider(
        name="gemini",
        default_model="gemini-3.5-flash",
        api_key_env=("GEMINI_API_KEY", "GOOGLE_API_KEY"),
    ),
    "gpt": Provider(
        name="gpt",
        default_model="gpt-6-astra",
        api_key_env=("OPENAI_API_KEY",),
    ),
    "codex": Provider(
        name="codex",
        default_model="",
        api_key_env=(),
    ),
    "ollama": Provider(
        name="ollama",
        default_model="glm-4.7-flash",
        api_key_env=(),
        base_url="http://localhost:11434",
    ),
    "vllm": Provider(
        name="vllm",
        # A deployment serves the models it was started with, and the handler
        # asks it for the first of them when no model is named.
        default_model="",
        # Only a server started with --api-key needs a credential, and the
        # handler reads VLLM_API_KEY itself where one is set.
        api_key_env=(),
        base_url="http://localhost:8000/v1",
    ),
}

DEFAULT_PROVIDER = "claude"


def normalize_provider_name(name: str) -> str:
    """The canonical key for a provider name written with either separator."""
    return name.strip().casefold().replace("-", "_")


#: Provider names accepted on the command line. `interact_with_llm` reads `-`
#: and `_` as the same separator, so both spellings are offered here too.
PROVIDER_CHOICES: tuple[str, ...] = tuple(
    sorted({*PROVIDERS, *(name.replace("_", "-") for name in PROVIDERS)})
)


def make_user_interaction(text: str) -> Interaction:
    """Build a backend-neutral user text interaction.

    The ``{"role", "content": [text part]}`` envelope is understood by every
    backend without a normalisation round-trip, so it survives a mid-chat
    provider switch. Left untagged, it is treated as native by whichever
    backend consumes it.
    """
    return Interaction(
        role=Role.USER,
        content=[
            a11.to_chunk(
                {
                    "role": "user",
                    "content": [{"type": "text", "text": text}],
                }
            )
        ],
    )


__all__ = [
    "DEFAULT_PROVIDER",
    "PROVIDERS",
    "PROVIDER_CHOICES",
    "Provider",
    "make_user_interaction",
    "normalize_provider_name",
]

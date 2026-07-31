# Copyright 2026 The A11 Authors.

"""Provider metadata for the chat CLI.

Backend selection no longer happens here: the CLI hands a provider name to the
``interact_with_llm`` action (via the ``x-a11-llm-provider`` header) and that
action routes to the concrete ``interact_with_*`` handler, importing the SDK
lazily. All this module keeps is the small amount of provider knowledge the CLI
itself needs — the default model to advertise and which environment variables
hold the API key — plus a helper to build a backend-neutral user message.
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

    def api_key(self) -> str:
        """First non-empty value among :attr:`api_key_env`, or ``""``."""
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
    "gemini": Provider(
        name="gemini",
        default_model="gemini-3.5-flash",
        api_key_env=("GEMINI_API_KEY", "GOOGLE_API_KEY"),
    ),
}

DEFAULT_PROVIDER = "claude"


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


__all__ = ["Provider", "PROVIDERS", "DEFAULT_PROVIDER", "make_user_interaction"]

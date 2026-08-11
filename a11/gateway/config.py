# Copyright 2026 The A11 Authors.

"""What a gateway is configured with, independent of how it was invoked.

The gateway used to be built straight from an `argparse.Namespace`, which meant
only the CLI could build one. Anything else that wants a gateway -- an embedded
one inside `a11 chat`, a test, a future embedding -- needs a plain value, and it
should not have to guess which ``no_*`` attributes the namespace happened to
carry.

Flags are positive here. The negation belongs to argparse, where ``--no-x`` reads
naturally on a command line; a config object that has to be read as
``not no_shell_tools`` does not.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib

from a11.gateway import conversations

#: Where a gateway listens unless told otherwise, and where a client looks for
#: one. Shared with the IntelliJ plugin's default setting.
DEFAULT_HOST = "127.0.0.1"
DEFAULT_A11_PORT = 8011
DEFAULT_PATH = "/a11"
DEFAULT_GATEWAY_URL = f"ws://{DEFAULT_HOST}:{DEFAULT_A11_PORT}{DEFAULT_PATH}"


@dataclasses.dataclass(frozen=True)
class GatewayConfig:
    """Everything a gateway needs to be built and served."""

    host: str = DEFAULT_HOST
    a11_port: int = DEFAULT_A11_PORT
    path: str = DEFAULT_PATH
    conversation_store_root: pathlib.Path = dataclasses.field(
        default_factory=conversations.default_root
    )
    #: Serve the ``shell_*`` action family.
    shell_tools: bool = True
    #: Serve ``flow_actions``, ``flow_check`` and ``flow_run``, which let a
    #: caller compose the gateway's other actions into one step.
    flow_tools: bool = True
    #: Serve ``list_audio_inputs`` and ``capture_audio``.
    audio_capture: bool = True
    #: Serve ``capture_transcription`` and ``transcribe_audio``.
    speech_recognition: bool = True

    @property
    def url(self) -> str:
        """The ``ws://`` URL this configuration serves on."""
        return f"ws://{self.host}:{self.a11_port}{self.path}"

    @classmethod
    def from_args(cls, args: argparse.Namespace) -> GatewayConfig:
        """Build a configuration from parsed CLI arguments.

        Reads the ``no_*`` flags argparse produces and flips them, so the
        double negative stops at this boundary.
        """
        return cls(
            host=getattr(args, "host", DEFAULT_HOST),
            a11_port=getattr(args, "a11_port", DEFAULT_A11_PORT),
            conversation_store_root=getattr(
                args, "conversation_store_root", conversations.default_root()
            ),
            shell_tools=not getattr(args, "no_shell_tools", False),
            flow_tools=not getattr(args, "no_flow_tools", False),
            audio_capture=not getattr(args, "no_audio_capture", False),
            speech_recognition=not getattr(
                args, "no_speech_recognition", False
            ),
        )


__all__ = [
    "DEFAULT_A11_PORT",
    "DEFAULT_GATEWAY_URL",
    "DEFAULT_HOST",
    "DEFAULT_PATH",
    "GatewayConfig",
]

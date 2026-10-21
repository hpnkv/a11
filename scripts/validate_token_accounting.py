#!/usr/bin/env python3
"""Validate live usage through the gateway, persistence and presentation APIs.

Use the provider's ordinary API-key environment variable. The probe runs two
short text turns and one tool turn. Pass --image for an inline PNG request.
Evidence contains provider events and usage records, with credentials redacted.
"""

import argparse
import asyncio
import json
import os
import shutil
import struct
import tempfile
import zlib
from pathlib import Path

import a11
from a11.cli.backends import PROVIDERS, make_user_interaction
from a11.cli.chat_tokens import ChatTokens
from a11.client.turn import TurnConfig, run_turn
from a11.gateway import conversations
from a11.gateway.config import GatewayConfig
from a11.gateway.embedded import embedded_gateway
from a11.sdk.presentation import BlockKind, PresentationReducer


def _config(provider: str, workspace: str) -> dict:
    if provider == "gpt":
        return {"max_completion_tokens": 128}
    if provider == "claude":
        return {"max_tokens": 128}
    if provider == "gemini":
        return {"max_output_tokens": 64, "thinking_level": "minimal"}
    if provider == "ollama":
        return {"num_predict": 128, "think": False}
    if provider == "vllm":
        return {"max_tokens": 128}
    if provider == "claude_code":
        return {
            "cwd": workspace,
            "builtin_tools": False,
            "max_turns": 4,
            "max_budget_usd": 0.25,
            "cli_path": shutil.which("claude"),
        }
    overrides = {}
    if os.environ.get("OPENAI_API_KEY"):
        overrides = {
            "model_provider": "a11_usage_probe",
            "model_providers.a11_usage_probe.name": "Token accounting probe",
            "model_providers.a11_usage_probe.base_url": "https://api.openai.com/v1",
            "model_providers.a11_usage_probe.wire_api": "responses",
            "model_providers.a11_usage_probe.env_key": "CODEX_API_KEY",
        }
    return {
        "cwd": workspace,
        "reasoning_effort": "low",
        "config_overrides": overrides,
    }


def _png() -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack("!I", len(data))
            + kind
            + data
            + struct.pack("!I", zlib.crc32(kind + data))
        )

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack("!2I5B", 64, 64, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress((b"\0" + b"\xff\0\0" * 64) * 64))
        + chunk(b"IEND", b"")
    )


def _event(value):
    return (
        value.model_dump(mode="json") if hasattr(value, "model_dump") else value
    )


async def _probe(args) -> dict:
    provider = PROVIDERS[args.provider]
    key = provider.api_key()
    if args.provider == "codex":
        key = os.environ.get("OPENAI_API_KEY", key)
    registry = a11.ActionRegistry()
    schema = a11.ActionSchema(
        name="token_audit_lookup",
        inputs={},
        outputs={
            "result": a11.ActionPortSchema("result", "text/plain", unary=True)
        },
    )

    async def lookup(action):
        await action["result"].finalize("The audit number is 42.")

    registry.register(schema.name, schema, lookup)
    evidence = {"provider": args.provider, "model": args.model, "turns": []}
    with tempfile.TemporaryDirectory(prefix="a11-token-probe-") as workspace:
        conversations._STORE = None
        settings = GatewayConfig(
            conversation_store_root=Path(workspace) / "conversations",
            shell_tools=False,
            audio_capture=False,
            speech_recognition=False,
        )
        history = []
        counter = ChatTokens()
        prompts = [
            "Reply with just ALPHA.",
            "Reply with just BETA.",
            "Call token_audit_lookup with no arguments once "
            "and report its number.",
        ]
        if args.image:
            prompts.append("Name the color of this image in one word.")
        async with embedded_gateway(settings, registry=registry) as connection:
            for index, prompt in enumerate(prompts):
                question = make_user_interaction(prompt)
                if args.image and index == len(prompts) - 1:
                    question.content.append(a11.to_chunk(_png(), "image/png"))
                events = []
                reducer = PresentationReducer()
                config = TurnConfig(
                    provider=args.provider,
                    model=args.model,
                    api_key=key,
                    base_url=args.base_url or provider.base_url,
                    provider_config=_config(args.provider, workspace),
                    allowed_actions=schema.name,
                    on_event=lambda value: events.append(_event(value)),
                )
                result = await run_turn(
                    connection, history, question, (), config, reducer
                )
                root = history[0].id if history else question.id
                stored = await conversations.get_conversation_store().read(root)
                stored_by_id = {
                    interaction.id: interaction for interaction in stored
                }
                replay = PresentationReducer()
                for interaction in result:
                    persisted = stored_by_id[interaction.id]
                    assert (
                        persisted.usage_metadata == interaction.usage_metadata
                    )
                    assert (
                        persisted.backend_specific_metadata
                        == interaction.backend_specific_metadata
                    )
                    replay.on_interaction(persisted)
                live = [
                    b.usage for b in reducer.blocks if b.kind == BlockKind.USAGE
                ]
                assert live == [
                    b.usage for b in replay.blocks if b.kind == BlockKind.USAGE
                ]
                counter.begin_turn()
                for block in reducer.blocks:
                    counter.observe(block)
                counter.finish()
                metadata = [
                    {
                        name: value.decode(errors="replace")
                        for name, value in i.backend_specific_metadata.items()
                    }
                    for i in result
                    if i.usage_metadata is not None
                ]
                evidence["turns"].append({
                    "index": index + 1,
                    "usage": [usage.model_dump(mode="json") for usage in live],
                    "backend_metadata": metadata,
                    "events": events,
                    "context_tokens": counter.context,
                    "output_label": counter.output_label,
                    "persistence_matches": True,
                    "presentation_replay_matches": True,
                })
                history.extend([question, *result])
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", choices=PROVIDERS, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--base-url", default="")
    parser.add_argument("--image", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    evidence = asyncio.run(_probe(args))
    serialized = json.dumps(evidence, indent=2)
    for provider in PROVIDERS.values():
        if secret := provider.api_key():
            serialized = serialized.replace(secret, "[REDACTED]")
    args.output.write_text(serialized + "\n")
    print(f"Recorded {len(evidence['turns'])} turns in {args.output}")


if __name__ == "__main__":
    main()

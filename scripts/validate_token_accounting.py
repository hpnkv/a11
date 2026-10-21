#!/usr/bin/env python3
"""Validate live usage through the gateway, persistence and presentation APIs.

Use the provider's ordinary API-key environment variable. The probe runs two
short text turns and one tool turn. Pass --image for an inline PNG request.
Evidence contains provider events and usage records, with credentials redacted.
"""

import argparse
import asyncio
import importlib.metadata
from pydantic_core import from_json, to_json
import os
import signal
import shutil
import struct
import tempfile
import zlib
from pathlib import Path
from typing import Any
from pydantic import BaseModel, Field

import a11
from a11.cli.backends import PROVIDERS, make_user_interaction
from a11.cli.chat_tokens import ChatTokens
from a11.client.turn import TurnConfig, run_turn
from a11.gateway import conversations
from a11.gateway.config import GatewayConfig
from a11.gateway.embedded import embedded_gateway
from a11.sdk.presentation import BlockKind, PresentationReducer
from a11.sdk.usage import parse_update, interaction_updates, UsageUpdate
from a11.sdk.llm import UsageMetadata
from usage_evidence import (
    reconcile,
    scenario_triggers,
    UsageCaptureProxy,
    UsageComparison,
    ScenarioTriggers,
    CapturedRequest,
)


class TurnEvidence(BaseModel):
    index: int
    usage: list[UsageMetadata]
    backend_metadata: list[dict[str, str]]
    events: list[dict[str, Any]]
    usage_records: list[UsageUpdate]
    failure: str | None
    reconciliation: list[UsageComparison]
    triggers: ScenarioTriggers
    validation_errors: list[str]
    context_tokens: int | None
    output_label: str
    persistence_matches: bool
    presentation_replay_matches: bool


class ProbeEvidence(BaseModel):
    provider: str
    model: str
    turns: list[TurnEvidence] = Field(default_factory=list)
    sdk_versions: dict[str, str]
    captured_requests: list[CapturedRequest] = Field(default_factory=list)
    original_tool_output_bytes: list[int] = Field(default_factory=list)


class ProbeOptions(BaseModel):
    provider: str
    model: str
    base_url: str = ""
    image: bool = False
    offer_tools: bool = True
    prompt: list[str] | None = None
    provider_config: str = "{}"
    tool_output_chars: int = Field(default=0, ge=0, le=256_000)
    context_chars: int = Field(default=0, ge=0, le=128_000)
    expect_failure: bool = False
    capture_requests: bool = False
    cancel_after_usage: bool = False
    crash_after_usage: bool = False
    fork_on_turn: int | None = Field(default=None, ge=2)
    fallback_fault: bool = False
    require: list[str] = Field(default_factory=list)
    output: Path


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
            "model_providers.a11_usage_probe.base_url": (
                "https://api.openai.com/v1"
            ),
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


async def _probe(args: ProbeOptions) -> ProbeEvidence:
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
        text = "The audit number is 42."
        if args.tool_output_chars:
            text += "\n" + "0123456789abcdef " * (args.tool_output_chars // 17)
            text += "\nEND_A11_AUDIT_PAYLOAD"
        evidence.original_tool_output_bytes.append(len(text.encode()))
        await action["result"].finalize(text)

    registry.register(schema.name, schema, lookup)
    evidence = ProbeEvidence.model_validate(
        {
            "provider": args.provider,
            "model": args.model,
            "turns": [],
            "sdk_versions": {
                name: importlib.metadata.version(name)
                for name in (
                    "openai",
                    "anthropic",
                    "claude-agent-sdk",
                    "google-genai",
                    "ollama",
                )
            },
        }
    )
    baseline = None
    proxy = (
        UsageCaptureProxy(
            anthropic=args.provider == "claude_code",
            fault_model=args.model if args.fallback_fault else None,
        )
        if args.capture_requests
        else None
    )
    capture_url = await proxy.start() if proxy else None
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
            (
                "Call token_audit_lookup with no arguments once "
                "and report its number."
            ),
        ]
        if args.prompt:
            prompts = args.prompt
        if args.context_chars:
            filler = "0123456789abcdef " * (args.context_chars // 17)
            prompts[0] += (
                "\nReference filler; discard when summarizing:\n" + filler
            )
        if args.image:
            prompts.append("Name the color of this image in one word.")
        async with embedded_gateway(settings, registry=registry) as connection:
            for index, prompt in enumerate(prompts):
                question = make_user_interaction(prompt)
                if args.image and index == len(prompts) - 1:
                    question.content.append(a11.to_chunk(_png(), "image/png"))
                events = []
                interrupted = False
                probe_task = asyncio.current_task()
                reducer = PresentationReducer()
                provider_config = _config(args.provider, workspace)
                overrides = from_json(args.provider_config)
                if "config_overrides" in overrides:
                    provider_config.setdefault("config_overrides", {}).update(
                        overrides.pop("config_overrides")
                    )
                provider_config.update(overrides)
                if args.fork_on_turn == index + 1:
                    assert args.provider == "claude_code"
                    provider_config["fork_session"] = True
                pid_file = Path(workspace) / "harness.pid"
                if args.crash_after_usage:
                    assert args.provider in {"claude_code", "codex"}
                    executable = provider_config.get(
                        "cli_path"
                    ) or shutil.which(
                        "claude" if args.provider == "claude_code" else "codex"
                    )
                    wrapper = Path(workspace) / "probe-harness"
                    wrapper.write_text(
                        "#!/bin/sh\n"
                        f"echo $$ > '{pid_file}'\n"
                        f"exec '{executable}' \"$@\"\n"
                    )
                    wrapper.chmod(0o700)
                    provider_config["cli_path"] = str(wrapper)

                def observe(value):
                    nonlocal interrupted
                    event = _event(value)
                    events.append(event)
                    update = parse_update(event)
                    if (
                        interrupted
                        or update is None
                        or update.usage.context_only
                        or update.usage.input_tokens is None
                        or update.usage.complete
                    ):
                        return
                    if args.crash_after_usage:
                        os.kill(int(pid_file.read_text()), signal.SIGKILL)
                        interrupted = True
                    elif args.cancel_after_usage:
                        probe_task.cancel()
                        interrupted = True

                if capture_url:
                    if args.provider == "claude_code":
                        provider_config.setdefault("env", {})[
                            "ANTHROPIC_BASE_URL"
                        ] = capture_url
                    else:
                        provider_config["config_overrides"][
                            "model_providers.a11_usage_probe.base_url"
                        ] = capture_url
                config = TurnConfig(
                    provider=args.provider,
                    model=args.model,
                    api_key=key,
                    base_url=args.base_url or provider.base_url,
                    provider_config=provider_config,
                    allowed_actions=schema.name if args.offer_tools else "",
                    on_event=observe,
                )
                failure = None
                try:
                    result = await run_turn(
                        connection, history, question, (), config, reducer
                    )
                except (Exception, asyncio.CancelledError) as error:
                    if not args.expect_failure:
                        raise
                    result = []
                    failure = type(error).__name__
                if args.expect_failure:
                    assert (
                        failure is not None
                    ), "Expected a consumed-token failure"
                root = history[0].id if history else question.id
                stored = await conversations.get_conversation_store().read(root)
                stored_by_id = {
                    interaction.id: interaction for interaction in stored
                }
                replay = PresentationReducer()
                replay.defer_usage()
                usage_records = [
                    event
                    for item in stored
                    for event in interaction_updates(item)
                ]
                for event in usage_records:
                    if event.id in {
                        u.id
                        for e in events
                        if (u := parse_update(e)) is not None
                    }:
                        replay.on_usage_event(event)
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
                replay.end_turn()
                live = [
                    b.usage for b in reducer.blocks if b.kind == BlockKind.USAGE
                ]
                replayed = [
                    b.usage for b in replay.blocks if b.kind == BlockKind.USAGE
                ]
                assert live == replayed, (live, replayed)
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
                validation_errors = []
                reconciliation = []
                try:
                    if interrupted and args.provider == "claude_code":
                        from usage_evidence import reconcile_partial_claude_code

                        reconciliation = reconcile_partial_claude_code(events)
                    else:
                        reconciliation, baseline = reconcile(
                            args.provider, events, baseline
                        )
                except AssertionError as error:
                    validation_errors.append(str(error))
                triggers = scenario_triggers(events)
                if interrupted:
                    triggers.failure.append(
                        "harness_killed_after_usage"
                        if args.crash_after_usage
                        else "cancelled_after_usage"
                    )
                for required in args.require:
                    if not getattr(triggers, required):
                        validation_errors.append(
                            f"Scenario not triggered: {required}"
                        )
                evidence.turns.append(
                    TurnEvidence.model_validate(
                        {
                            "index": index + 1,
                            "usage": [
                                usage.model_dump(mode="json") for usage in live
                            ],
                            "backend_metadata": metadata,
                            "events": events,
                            "usage_records": usage_records,
                            "failure": failure,
                            "reconciliation": reconciliation,
                            "triggers": triggers,
                            "validation_errors": validation_errors,
                            "context_tokens": counter.context,
                            "output_label": counter.output_label,
                            "persistence_matches": True,
                            "presentation_replay_matches": True,
                        }
                    )
                )
                history.extend([question, *result])
    if proxy:
        await proxy.close()
        evidence.captured_requests = proxy.exchanges
        if args.fallback_fault:
            assert any(r.injected for r in proxy.exchanges), "No fault injected"
            assert any(
                r.model == provider_config.get("fallback_model")
                and r.status_code == 200
                and r.usage
                for r in proxy.exchanges
            ), "No fallback model executed"
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", choices=PROVIDERS, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--base-url", default="")
    parser.add_argument("--image", action="store_true")
    parser.add_argument("--no-tools", dest="offer_tools", action="store_false")
    parser.add_argument("--prompt", action="append")
    parser.add_argument("--provider-config", default="{}")
    parser.add_argument("--tool-output-chars", type=int, default=0)
    parser.add_argument("--context-chars", type=int, default=0)
    parser.add_argument("--expect-failure", action="store_true")
    parser.add_argument("--capture-requests", action="store_true")
    parser.add_argument("--cancel-after-usage", action="store_true")
    parser.add_argument("--crash-after-usage", action="store_true")
    parser.add_argument("--fork-on-turn", type=int)
    parser.add_argument("--fallback-fault", action="store_true")
    parser.add_argument(
        "--require",
        action="append",
        default=[],
        choices=[
            "compaction",
            "subagents",
            "truncation",
            "failure",
            "clearing",
        ],
    )
    parser.add_argument("--output", type=Path, required=True)
    args = ProbeOptions.model_validate(vars(parser.parse_args()))
    evidence = asyncio.run(_probe(args))
    serialized = to_json(evidence, indent=2).decode()
    for provider in PROVIDERS.values():
        if secret := provider.api_key():
            serialized = serialized.replace(secret, "[REDACTED]")
    args.output.write_text(serialized + "\n")
    print(f"Recorded {len(evidence.turns)} turns in {args.output}")
    if any(turn.validation_errors for turn in evidence.turns):
        raise SystemExit(
            "Usage source reconciliation or scenario trigger failed; see"
            " evidence."
        )


if __name__ == "__main__":
    main()

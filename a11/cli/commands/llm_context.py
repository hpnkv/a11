# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Standalone LLM context utilities, result hooks, and provider interception."""

from __future__ import annotations

import argparse
import asyncio
import importlib
import json
import shutil
import sys

from a11.cli.app import Command
from a11.cli.commands import _command_help
from a11.sdk.llm_context.config import LlmContextServices, decode_config
from a11.sdk.llm_context.content import (
    compress_content,
    count_context,
    expand_content,
)
from a11.sdk.llm_context.history import compact_history
from a11.sdk.llm_context.measurement import RunRecorder, compare_runs, load_runs
from a11.sdk.llm_context.proxy import create_proxy
from a11.sdk.llm_context.retention import RetainedContentStore
from a11.sdk.llm_context.shell import intercept_shell_result, shell_hook
from a11.status import StatusException


def _configure(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "operation",
        choices=(
            "compress",
            "count",
            "expand",
            "compact",
            "compare",
            "measure",
            "retrieve",
            "shell-filter",
            "shell-hook",
            "proxy",
        ),
    )
    parser.add_argument(
        "--config",
        default="{}",
        help="JSON LLM context policy; shell mode defaults off.",
    )
    parser.add_argument(
        "--summarizer", help="Host-selected module:callable for compact."
    )
    parser.add_argument(
        "--record", help="Append metadata-only reports to this JSONL file."
    )
    parser.add_argument(
        "--baseline", help="Recorded baseline JSON/JSONL for compare."
    )
    parser.add_argument(
        "--treatment", help="Recorded treatment JSON/JSONL for compare."
    )
    parser.add_argument(
        "--upstream", help="Host-selected HTTP origin for proxy."
    )
    parser.add_argument(
        "--profile", choices=("chat", "responses", "anthropic"), default="chat"
    )
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8013)
    parser.add_argument(
        "--root", help="Retention owner's local data directory."
    )
    parser.add_argument("--owner", help="Retention owner identity.")


async def _run(args: argparse.Namespace) -> int:
    try:
        config = decode_config(args.config)
        recorder = RunRecorder(args.record) if args.record else None
        if args.operation == "proxy":
            if not args.upstream:
                raise ValueError("proxy requires --upstream")
            server = create_proxy(
                args.upstream,
                profile=args.profile,
                config=config,
                services=LlmContextServices(recorder=recorder),
                bind_address=args.bind,
                port=args.port,
            )
            print(
                "Context proxy listening on"
                f" {server.bind_address}:{server.port}",
                file=sys.stderr,
            )
            try:
                await asyncio.Event().wait()
            finally:
                server.stop()
            return 0
        if args.operation == "compare":
            if not args.baseline or not args.treatment:
                raise ValueError("compare requires --baseline and --treatment")
            result = compare_runs(
                load_runs(args.baseline), load_runs(args.treatment)
            )
        elif args.operation == "shell-filter":
            raw = sys.stdin.buffer.read(config.max_bytes + 1)
            if len(raw) > config.max_bytes:
                sys.stdout.buffer.write(raw)
                shutil.copyfileobj(sys.stdin.buffer, sys.stdout.buffer)
                return 0
            projected = intercept_shell_result(
                {"stdout": raw, "stderr": b"", "exit_code": 0}, config
            )
            sys.stdout.buffer.write(projected["view"]["stdout"])
            if recorder:
                for report in projected["reports"]:
                    recorder.record(report)
            return 0
        else:
            raw = sys.stdin.buffer.read(config.max_bytes + 1)
            if len(raw) > config.max_bytes:
                raise ValueError("stdin exceeds context byte limit")
            text = raw.decode()
            if args.operation == "compress":
                result = compress_content(text, config)
            elif args.operation == "count":
                result = count_context(text)
            elif args.operation == "expand":
                result = {
                    "text": expand_content(text, max_bytes=config.max_bytes)
                }
            elif args.operation == "compact":
                request = json.loads(text)
                summarizer = None
                if args.summarizer:
                    module, name = args.summarizer.rsplit(":", 1)
                    summarizer = getattr(importlib.import_module(module), name)
                result = await compact_history(
                    request["history"],
                    config,
                    checkpoint=request.get("checkpoint"),
                    summarizer=summarizer,
                )
            elif args.operation == "shell-hook":
                result = shell_hook(json.loads(text), config)
            elif args.operation == "retrieve":
                if not args.root or not args.owner:
                    raise ValueError("retrieve requires --root and --owner")
                request = json.loads(text)
                result = await RetainedContentStore(
                    args.root, owner=args.owner
                ).retrieve(
                    request["reference"],
                    request["scope"],
                    cursor=request.get("cursor", 0),
                    max_bytes=request.get("max_bytes", 65536),
                )
            else:
                before = compress_content(
                    text, config.model_copy(update={"enabled": False})
                )
                after = compress_content(text, config)
                result = compare_runs([before["report"]], [after["report"]])
            if recorder and "report" in result:
                recorder.record(result["report"])
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except (StatusException, ValueError, KeyError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1


LLM_CONTEXT_COMMAND = Command(
    name="llm-context",
    help=_command_help("llm-context"),
    configure=_configure,
    run=_run,
)

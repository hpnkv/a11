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

"""Project completed command results without executing or rewriting commands."""

from __future__ import annotations

import copy
import json
from collections.abc import Callable

from a11.sdk.llm_context.config import LlmContextConfig
from a11.sdk.llm_context.content import compress_content
from a11.status import StatusException


def intercept_shell_result(
    result: dict,
    config: LlmContextConfig,
    *,
    counter: Callable[[str], int] | None = None,
    bypass: bool = False,
    source: dict | None = None,
) -> dict:
    """Return separate original and model views of a completed command.

    The result contract uses stdout/stderr text or bytes and exit_code. Optional
    fields include interactive, complete, signal, and truncated. Unknown fields
    survive.
    Command hints in source are metadata. This function never runs a command.

    Args:
        result: Completed result with exit_code, captured streams, and optional
            line arrays. Truncation/interactive/incomplete states pass through.
        config: Policy selecting shell off, observe, or compress.
        counter: Optional tokenizer used by the shared content utility.
        bypass: Per-command passthrough selected by the host.
        source: Optional source metadata retained separately from output.

    Returns:
        Original result, independent model-facing view, field reports, and
        source metadata. Compressed line arrays carry a JSON representation
        envelope; original typed arrays remain in original.
    """
    view = copy.deepcopy(result)
    reports = []
    mode = config.shell.mode if config.enabled else "off"
    reason = None
    if bypass or mode == "off":
        reason = "bypass" if bypass else "disabled"
    elif (
        type(result.get("exit_code")) is not int
        or result.get("exit_code") != 0
        or result.get("signal")
        or result.get("interactive")
        or not result.get("complete", True)
        or result.get("truncated")
    ):
        reason = "ineligible_result"
    if reason is None:
        selected = (
            config
            if mode == "compress"
            else config.model_copy(update={"enabled": False})
        )
        pending = {}
        try:
            for field in (
                "stdout",
                "stderr",
                "output_lines",
                "stdout_lines",
                "stderr_lines",
            ):
                if field not in result:
                    continue
                raw = result[field]
                array = isinstance(raw, list) and all(
                    isinstance(line, str) for line in raw
                )
                if array:
                    raw = json.dumps(raw, ensure_ascii=False, indent=2)
                text = raw.decode("utf-8") if isinstance(raw, bytes) else raw
                if not isinstance(text, str) or "\x00" in text:
                    reason = "binary_or_unknown_encoding"
                    break
                candidate = compress_content(text, selected, counter=counter)
                if candidate["report"]["changed"]:
                    pending[field] = (
                        candidate["text"].encode()
                        if isinstance(raw, bytes)
                        else (
                            {
                                "representation": "application/json",
                                "view": candidate["text"],
                            }
                            if array
                            else candidate["text"]
                        )
                    )
                reports.append(
                    {
                        **candidate["report"],
                        "surface": "shell",
                        "stream": field,
                        "shell_mode": mode,
                    }
                )
            if reason is None:
                view.update(pending)
        except UnicodeDecodeError:
            reason = "binary_or_unknown_encoding"
        except StatusException:
            reason = "transform_failed"
    if reason:
        view = copy.deepcopy(result)
        reports = [
            {
                "schema": "a11.llm-context.report/v1",
                "operation": "shell",
                "changed": False,
                "skip_reason": reason,
                "shell_mode": mode,
            }
        ]
    return {
        "original": result,
        "view": view,
        "reports": reports,
        "source": source or {},
    }


def shell_hook(
    event: dict,
    config: LlmContextConfig,
    *,
    counter: Callable[[str], int] | None = None,
) -> dict:
    """Adapt the versioned completed-result hook envelope.

    External harness recipes map their event keys into this explicit contract.
    Pre-execution events pass through with an unsupported capability report.
    """
    if event.get("schema") != "a11.llm-context.shell-result/v1":
        return {
            "event": event,
            "supported": False,
            "reason": "completed_result_contract_required",
        }
    projected = intercept_shell_result(
        event["result"],
        config,
        counter=counter,
        bypass=event.get("bypass", False),
        source=event.get("source"),
    )
    return {
        "event": {**event, "result": projected["view"]},
        "supported": True,
        "reports": projected["reports"],
    }

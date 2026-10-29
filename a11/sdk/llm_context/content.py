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

"""Compress complete content through the native deterministic transforms."""

from __future__ import annotations

import hashlib
import json
import time
from typing import Callable

from a11 import _native
from a11.sdk.llm_context.config import LlmContextConfig, decode_config
from a11.status import Status, StatusCode, StatusException

_FORMATS = {"lines": "a11.lines/v1", "json-keys": "a11.json-keys/v1"}


def count_context(
    text: str, counter: Callable[[str], int] | None = None
) -> dict:
    """Count text with a tokenizer or a labeled byte estimate.

    Args:
        text: UTF-8 content whose model-facing size is measured.
        counter: Host tokenizer returning a nonnegative integer. Omit to use
            the native byte estimate.

    Returns:
        Token count, UTF-8 byte count, and counting source.

    Raises:
        StatusException: The supplied counter returns an invalid count.
    """
    count = (
        counter(text) if counter else _native.llm_context_estimate_tokens(text)
    )
    if isinstance(count, bool) or not isinstance(count, int) or count < 0:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Token counter must return a nonnegative integer",
        ).to_exception()
    return {
        "tokens": count,
        "source": "counter" if counter else "byte_estimate",
        "bytes": len(text.encode()),
    }


def expand_content(text: str, *, max_bytes: int = 8 * 1024 * 1024) -> str:
    """Recover the exact original from a reversible context envelope.

    Args:
        text: An a11.lines/v1 or a11.json-keys/v1 JSON envelope.
        max_bytes: Limit for the encoded input and expanded output.

    Returns:
        The original UTF-8 text, including its whitespace and numeric tokens.

    Raises:
        StatusException: The envelope is malformed or exceeds the byte limit.
    """
    return _native.llm_context_expand(text, max_bytes)


def compress_content(
    text: str,
    config: LlmContextConfig | dict | None = None,
    *,
    counter: Callable[[str], int] | None = None,
) -> dict:
    """Select a smaller model-facing view of complete text.

    Args:
        text: Original content, retained by the caller.
        config: Immutable policy or policy dictionary. Omit to request the
            default deterministic transforms. An explicit disabled policy
            returns original content with counts.
        counter: Optional host tokenizer used for candidate selection.

    Returns:
        A dictionary with text and a versioned report describing the format,
        guarantee, source digest, counts, and transform time. Shadow mode
        returns original text and reports candidate counts separately.

    Raises:
        StatusException: Policy, content, tokenizer output, or limits fail
            validation.
    """
    if not isinstance(text, str):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Context content must be text",
        ).to_exception()
    config = (
        config
        if isinstance(config, LlmContextConfig)
        else decode_config(json.dumps(config or {}))
    )
    started = time.monotonic()
    original = count_context(text, counter)
    if original["bytes"] > config.max_bytes:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="Context exceeds byte limit",
        ).to_exception()
    best, format_name, guarantee = text, "identity", "byte_exact"
    if config.enabled:
        strategies = (
            ("json", "lines", "json-keys")
            if config.strategy == "auto"
            else (config.strategy,)
        )
        for strategy in strategies:
            try:
                if strategy == "json":
                    candidate = _native.llm_context_compact_json(
                        text, config.max_bytes
                    )
                    candidate_format, candidate_guarantee = (
                        "json",
                        "json_tokens_preserved",
                    )
                else:
                    candidate_format = _FORMATS[strategy]
                    candidate = _native.llm_context_encode(
                        text, candidate_format, config.max_bytes
                    )
                    if len(candidate.encode()) > config.max_bytes:
                        continue
                    if (
                        _native.llm_context_expand(candidate, config.max_bytes)
                        != text
                    ):
                        continue
                    candidate_guarantee = "byte_exact"
                if (
                    count_context(candidate, counter)["tokens"]
                    < count_context(best, counter)["tokens"]
                ):
                    best, format_name, guarantee = (
                        candidate,
                        candidate_format,
                        candidate_guarantee,
                    )
            except StatusException as exc:
                if exc.status.code != StatusCode.INVALID_ARGUMENT:
                    raise
    after = count_context(best, counter)
    if original["tokens"] - after["tokens"] < config.min_gain:
        best, format_name, guarantee, after = (
            text,
            "identity",
            "byte_exact",
            original,
        )
    projected = best
    if config.measurement.mode == "shadow":
        best = text
    return {
        "text": best,
        "report": {
            "schema": "a11.llm-context.report/v1",
            "operation": "compress",
            "format": format_name,
            "guarantee": guarantee,
            "source_digest": hashlib.sha256(text.encode()).hexdigest(),
            "bytes_before": original["bytes"],
            "bytes_after": len(best.encode()),
            "tokens_before": original["tokens"],
            "tokens_after": count_context(best, counter)["tokens"],
            "projected_tokens_after": after["tokens"],
            "count_source": original["source"],
            "elapsed_seconds": time.monotonic() - started,
            "mode": config.measurement.mode,
            "changed": best != text,
            "projected_changed": projected != text,
            "run_id": config.measurement.run_id,
            "arm": config.measurement.arm,
        },
    }

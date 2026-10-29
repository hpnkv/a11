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

"""Project, accept, and replay model-facing views of canonical interactions."""

from __future__ import annotations

import functools
import hashlib
import inspect
import json
from collections import Counter
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import Any

from pydantic import ValidationError

import a11
from a11.data.serialization import get_global_serialization_registry
from a11.data.types import validate_wire
from a11.sdk import llm
from a11.sdk.llm_context.config import (
    LlmContextConfig,
    LlmContextServices,
    decode_config,
)
from a11.sdk.llm_context.history import (
    _checkpoint_text,
    _complete_prefix,
    compact_history,
)
from a11.status import Status, StatusCode

VIEW_SCHEMA = "a11.llm-context.interaction-view/v1"
APPLIED_SCHEMA = "a11.llm-context.applied-interaction/v1"
VIEW_PRODUCER = "a11.llm_context"
VIEW_METADATA = "a11.llm-context.view"


def _json(value: Any) -> str:
    return json.dumps(
        value, sort_keys=True, ensure_ascii=False, separators=(",", ":")
    )


def _digest(value: Any) -> str:
    return hashlib.sha256(_json(value).encode()).hexdigest()


def encode_interactions(interactions: list[llm.Interaction]) -> list[dict]:
    """Serialize existing Interaction values as ordinary typed A11 chunks.

    Args:
        interactions: Canonical records, including original typed fragments.

    Returns:
        JSON-compatible Chunk documents using the existing Interaction wire tag.
        Serialization leaves source records and native chunk metadata unchanged.
    """
    return [
        a11.to_chunk(item, mimetype="application/json").model_dump(mode="json")
        for item in interactions
    ]


def decode_interactions(chunks: list[dict]) -> list[llm.Interaction]:
    """Read Interaction values from their existing typed Chunk representation.

    Args:
        chunks: JSON-compatible Chunk documents from encode_interactions.

    Returns:
        Independent source records with their native typed data intact.

    Raises:
        StatusException: A chunk does not decode as an Interaction.
    """
    try:
        return [
            a11.from_chunk(
                validate_wire(a11.Chunk, item), obj_type=llm.Interaction
            )
            for item in chunks
        ]
    except (ValueError, TypeError, ValidationError) as error:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Invalid serialized Interaction source",
        ).to_exception() from error


def _opaque(value: Any, *, untagged: bool = False) -> bool:
    if isinstance(value, list):
        return any(_opaque(v, untagged=untagged) for v in value)
    if not isinstance(value, dict):
        return not isinstance(value, str)
    kind = value.get("type")
    if untagged and (
        (kind is not None and kind not in ("text", "image", "image_url"))
        or any(key in value for key in ("tool_calls", "parts", "messages"))
        or value.get("role") == "tool"
    ):
        return True
    allowed = {
        "role",
        "content",
        "parts",
        "steps",
        "messages",
        "type",
        "text",
        "image_url",
        "images",
        "source",
        "data",
        "mime_type",
        "media_type",
        "tool_calls",
        "tool_call_id",
        "tool_name",
        "function",
        "name",
        "arguments",
        "id",
        "call_id",
        "tool_use_id",
        "input",
        "result",
        "output",
    }
    if set(value) - allowed:
        return True
    if kind and kind not in {
        "text",
        "input_text",
        "output_text",
        "image",
        "image_url",
        "input_image",
        "tool_use",
        "tool_call",
        "tool_result",
        "function",
        "function_call",
        "function_result",
        "function_call_output",
        "user_input",
        "model_output",
    }:
        return True
    if kind in ("image_url", "input_image"):
        image = value.get("image_url")
        url = image.get("url") if isinstance(image, dict) else image
        if not isinstance(url, str) or not url.startswith("data:"):
            return True
        if isinstance(image, dict) and set(image) != {"url"}:
            return True
    if kind == "image" and "source" in value:
        source = value["source"]
        if not isinstance(source, dict) or source.get("type") != "base64":
            return True
    for key in (
        "content",
        "steps",
        "messages",
        "parts",
        "tool_calls",
        "function",
    ):
        if (
            key in value
            and isinstance(value[key], (list, dict))
            and _opaque(value[key], untagged=untagged)
        ):
            return True
    return False


def _normalized_source(item: llm.Interaction) -> llm.NormalizedMessage | None:
    if item.status is not None and not item.status.is_ok():
        return None
    statuses = item.backend_specific_metadata.get(
        llm.TOOL_STATUSES_METADATA_KEY
    )
    if statuses:
        try:
            if any(
                value.get("code", 0) for value in json.loads(statuses).values()
            ):
                return None
        except (ValueError, AttributeError, TypeError):
            return None
    backend = llm.interaction_backend(item)
    if backend == VIEW_PRODUCER:
        return _read_projection(item)
    if (
        backend is not None
        and item.role != llm.Role.SYSTEM
        and len(item.content) != 1
    ):
        return None
    current = ""
    expected = Counter()
    for chunk in item.content:
        current = chunk.get_mimetype() or current
        if chunk.metadata is not None and chunk.metadata.attributes:
            return None
        if current.split(";", 1)[0].startswith("image/"):
            expected["image"] += 1
            continue
        try:
            value = get_global_serialization_registry().from_chunk(
                chunk, current
            )
        except Exception:
            return None
        if _opaque(value, untagged=backend is None):
            return None
        expected.update(_part_counts(value))
    if not llm.interaction_metadata_is_portable(item):
        return None
    if backend is None and (item.action_calls or item.action_outputs):
        return None
    try:
        normalized = (
            llm.normalize_by_shape(item)
            if item.role == llm.Role.SYSTEM
            else llm.normalize_interaction(item, strict=backend is not None)
        )
    except a11.status.StatusException:
        return None
    if not normalized.parts or normalized.role != item.role:
        return None
    actual = Counter(p.type for p in normalized.parts)
    if any(actual[kind] < count for kind, count in expected.items()):
        return None
    for part in normalized.parts:
        if (
            (part.type == "text" and part.text is None)
            or (part.type == "image" and not part.data)
            or (
                part.type == "tool_call"
                and (not part.id or not part.name or part.arguments is None)
            )
            or (
                part.type == "tool_result"
                and (not part.call_id or part.content is None)
            )
        ):
            return None
    calls = {p.id for p in normalized.parts if p.type == "tool_call"}
    results = {p.call_id for p in normalized.parts if p.type == "tool_result"}
    if any(call.id not in calls for call in item.action_calls):
        return None
    if set(item.action_outputs) - results or (item.action_inputs and not calls):
        return None
    return normalized


def _part_counts(value: Any) -> Counter:
    if isinstance(value, str):
        return Counter(text=1)
    if isinstance(value, list):
        result = Counter()
        for part in value:
            result.update(_part_counts(part))
        return result
    if not isinstance(value, dict):
        return Counter()
    kind = value.get("type")
    if kind in ("image", "image_url", "input_image"):
        return Counter(image=1)
    if kind in ("tool_use", "tool_call", "function_call"):
        return Counter(tool_call=1)
    if (
        kind in ("tool_result", "function_result", "function_call_output")
        or value.get("role") == "tool"
    ):
        images = _part_counts(value.get("content", []))["image"]
        return Counter(tool_result=1, image=images)
    if kind in ("text", "input_text", "output_text"):
        return Counter(text=1)
    result = Counter(tool_call=len(value.get("tool_calls") or []))
    for key in ("content", "messages", "steps", "parts"):
        if key in value:
            result.update(_part_counts(value[key]))
    if "text" in value and "content" not in value:
        result["text"] += 1
    result["image"] += len(value.get("images") or [])
    return result


def _history_row(
    identity: str, normalized: llm.NormalizedMessage | None
) -> dict:
    if normalized is None:
        return {"id": identity, "role": "user", "content": [{"type": "opaque"}]}
    parts = []
    for part in normalized.parts:
        block = part.model_dump(mode="json", exclude_none=True)
        if block["type"] == "tool_call":
            block["type"] = "tool_use"
        elif block["type"] == "tool_result":
            block["tool_use_id"] = block.pop("call_id", None)
        parts.append(block)
    return {
        "id": identity,
        "role": (
            "assistant"
            if normalized.role == llm.Role.ASSISTANT
            else normalized.role.value
        ),
        "content": parts,
    }


def _normalized_row(row: dict) -> dict:
    parts = []
    content = row.get("content", [])
    if isinstance(content, str):
        content = [{"type": "text", "text": content}]
    for value in content:
        block = dict(value)
        if block.get("type") == "tool_use":
            block["type"] = "tool_call"
        elif block.get("type") == "tool_result":
            block["call_id"] = block.pop("tool_use_id", None)
        parts.append(block)
    return llm.NormalizedMessage(
        role=(
            llm.Role.ASSISTANT
            if row["role"] == "assistant"
            else llm.Role(row["role"])
        ),
        parts=parts,
    ).model_dump(mode="json", exclude_none=True)


def _instruction_row(instructions: list[dict]) -> dict | None:
    texts = []
    for value in instructions:
        text = a11.from_chunk(validate_wire(a11.Chunk, value))
        if not isinstance(text, str):
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="Only text instruction chunks can be replayed",
            ).to_exception()
        texts.append(text)
    return (
        {"id": "instructions", "role": "system", "content": "\n\n".join(texts)}
        if texts
        else None
    )


def _context_rows(interactions: list[llm.Interaction]) -> list[dict]:
    rows = [
        _history_row(item.id, llm.normalize_interaction(item))
        for item in interactions
    ]
    instruction = _instruction_row(
        [
            chunk.model_dump(mode="json")
            for item in interactions
            for chunk in item.system_instructions
        ]
    )
    if instruction is not None:
        rows.insert(0, instruction)
    return rows


async def project_interactions(
    interactions: list[llm.Interaction],
    config: LlmContextConfig | dict,
    *,
    source_revision: int | str = 0,
    summarizer: Callable[[list[dict]], str | Awaitable[str]] | None = None,
    checkpoint: str | None = None,
    counter: Callable[[str], int] | None = None,
    reporter: Callable[[dict], None | Awaitable[None]] | None = None,
) -> dict:
    """Build an inspectable, unaccepted view before choosing a provider.

    Args:
        interactions: Ordered canonical Interaction records. Typed fragments
            remain in serialized sources independently of generated summaries.
        config: Opt-in compression and summary policy. Native compaction belongs
            to a harness owner and is unavailable in a local view.
        source_revision: Explicit revision of the host's canonical snapshot.
        summarizer: Host-selected sync/async prefix-to-text callback.
        checkpoint: Existing checkpoint text, replacing a summarizer call.
        counter: Optional tokenizer; otherwise counts are labeled estimates.
        reporter: Optional sync/async sink for transform and compaction reports.

    Returns:
        A versioned JSON proposal with canonical sources, normalized projection,
        instructions, covered IDs, boundary, policy, and integrity digests.
        No history owner is modified. Shadow proposals cannot be accepted.

    Raises:
        StatusException: Identity, limits, ordering, or budget validation fails.
    """
    from a11.sdk.llm_context.runtime import project_messages

    config = (
        config
        if isinstance(config, LlmContextConfig)
        else decode_config(_json(config))
    )
    if type(source_revision) not in (str, int) or isinstance(
        source_revision, bool
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="An explicit source revision is required",
        ).to_exception()
    if config.enabled and config.compaction == "native":
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Native history requires its harness coordinator",
        ).to_exception()
    if not isinstance(interactions, list) or not all(
        isinstance(item, llm.Interaction) for item in interactions
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Interaction sources must be a list of Interaction records",
        ).to_exception()
    ids = [item.id for item in interactions]
    if (
        not ids
        or any(not identity for identity in ids)
        or len(set(ids)) != len(ids)
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Interaction source IDs must be nonempty and unique",
        ).to_exception()
    for previous, item in zip(interactions, interactions[1:]):
        if item.previous_interaction_id not in ("", previous.id):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Interaction source ordering does not match its chain",
            ).to_exception()
    sources = encode_interactions(interactions)
    if len(_json(sources).encode()) > config.max_bytes:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="Interaction sources exceed the byte limit",
        ).to_exception()
    instructions = []
    rows = []
    opaque = []
    for source_index, item in enumerate(interactions):
        if (item.system_instructions or item.role == llm.Role.SYSTEM) and any(
            p.role != llm.Role.SYSTEM for p in interactions[:source_index]
        ):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Instruction chunks must precede model history",
            ).to_exception()
        instructions.extend(
            chunk.model_dump(mode="json") for chunk in item.system_instructions
        )
        normalized = _normalized_source(item)
        if normalized is None:
            opaque.append(item.id)
        if item.role == llm.Role.SYSTEM:
            if normalized is None or any(
                p.type != "text" for p in normalized.parts
            ):
                raise Status(
                    code=StatusCode.FAILED_PRECONDITION,
                    message=(
                        "System Interaction cannot be projected as text"
                        " instructions"
                    ),
                ).to_exception()
            instructions.extend(
                a11.to_chunk(p.text or "").model_dump(mode="json")
                for p in normalized.parts
            )
            continue
        rows.append(_history_row(item.id, normalized))
    instruction = _instruction_row(instructions)
    if instruction is not None:
        rows.insert(0, instruction)
    reports = []

    async def report(value):
        reports.append(value)
        if reporter is not None:
            observed = reporter(value)
            if inspect.isawaitable(observed):
                await observed

    projected = await project_messages(
        rows,
        config.model_copy(update={"compaction": "off"}),
        services=LlmContextServices(token_counter=counter),
        reporter=report,
    )
    compacted = None
    if config.compaction == "summary":
        compacted = await compact_history(
            projected,
            config,
            summarizer=summarizer,
            counter=counter,
            checkpoint=checkpoint,
        )
        await report(compacted["report"])
        projected = compacted.get("proposed_history", compacted["history"])
    entries = []
    for row in projected:
        if row["role"] == "system":
            continue
        identity = row.get("id")
        entries.append(
            {
                "source_id": identity if identity in ids else None,
                "normalized": (
                    None if identity in opaque else _normalized_row(row)
                ),
            }
        )
    if not entries:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "An Interaction view requires model history after instructions"
            ),
        ).to_exception()
    view = {
        "schema": VIEW_SCHEMA,
        "state": "proposal",
        "source_revision": source_revision,
        "sources": sources,
        "source_ids": ids,
        "source_digest": _digest(sources),
        "instructions": instructions,
        "entries": entries,
        "covered_ids": compacted.get("covered_ids", []) if compacted else [],
        "retained_boundary": (
            compacted.get("retained_boundary", 0) if compacted else 0
        ),
        "checkpoint": compacted.get("checkpoint") if compacted else None,
        "policy": config.model_dump(mode="json", exclude={"measurement"}),
        "shadow": config.measurement.mode == "shadow",
        "reports": reports,
    }
    view["projection_digest"] = _digest(
        {"entries": entries, "instructions": instructions}
    )
    view["view_id"] = _view_id(view)
    if len(_json(view).encode()) > config.max_bytes:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="Interaction view exceeds the byte limit",
        ).to_exception()
    validate_interaction_view(view)
    return view


def _manifest(view: dict) -> dict:
    return {
        **{
            key: view[key]
            for key in (
                "schema",
                "source_revision",
                "source_digest",
                "projection_digest",
                "retained_boundary",
                "policy",
                "shadow",
            )
        },
        **{
            key + "_digest": _digest(view[key])
            for key in (
                "source_ids",
                "covered_ids",
                "checkpoint",
            )
        },
    }


def _view_id(view: dict) -> str:
    return _digest(_manifest(view))


def validate_interaction_view(view: dict) -> None:
    """Check a serialized proposal or accepted view without installing it.

    Args:
        view: Versioned JSON Interaction view, including canonical sources.

    Raises:
        StatusException: Digests, source coverage, or the projection disagree.
    """
    try:
        if view["schema"] != VIEW_SCHEMA or view["state"] not in (
            "proposal",
            "accepted",
        ):
            raise ValueError("Unknown view contract")
        config = decode_config(_json(view["policy"]))
        if len(_json(view).encode()) > config.max_bytes:
            raise ValueError("View exceeds its byte limit")
        sources = decode_interactions(view["sources"])
        ids = [item.id for item in sources]
        if (
            not ids
            or any(not identity for identity in ids)
            or ids != view["source_ids"]
            or len(set(ids)) != len(ids)
            or type(view["source_revision"]) not in (str, int)
            or type(view["retained_boundary"]) is not int
            or view["retained_boundary"] < 0
            or type(view["shadow"]) is not bool
        ):
            raise ValueError("Source identities disagree")
        for previous, item in zip(sources, sources[1:]):
            if item.previous_interaction_id not in ("", previous.id):
                raise ValueError("Source ordering disagrees")
        if _digest(view["sources"]) != view["source_digest"]:
            raise ValueError("Source digest disagrees")
        if (
            _digest(
                {
                    "entries": view["entries"],
                    "instructions": view["instructions"],
                }
            )
            != view["projection_digest"]
        ):
            raise ValueError("Projection digest disagrees")
        if _view_id(view) != view["view_id"]:
            raise ValueError("View identity disagrees")
        retained = [
            entry["source_id"]
            for entry in view["entries"]
            if entry["source_id"] is not None
        ]
        covered = view["covered_ids"]
        body = [item.id for item in sources if item.role != llm.Role.SYSTEM]
        if (
            covered != body[: view["retained_boundary"]]
            or retained != body[len(covered) :]
            or len(covered) != view["retained_boundary"]
        ):
            raise ValueError("Covered prefix and retained sources disagree")
        portable = view["state"] == "accepted"
        normalized = (
            {
                entry["source_id"]: (
                    llm.NormalizedMessage.model_validate(entry["normalized"])
                    if entry["normalized"] is not None
                    else None
                )
                for entry in view["entries"]
                if entry["source_id"] is not None
            }
            if portable
            else {item.id: _normalized_source(item) for item in sources}
        )
        rows = [
            _history_row(identity, normalized.get(identity))
            for identity in body
        ]
        if (
            covered
            and not portable
            and _complete_prefix(rows, len(covered)) != len(covered)
        ):
            raise ValueError(
                "Covered prefix crosses a protected or tool boundary"
            )
        generated = [
            entry for entry in view["entries"] if entry["source_id"] is None
        ]
        if bool(generated) != bool(covered) or len(generated) > 1:
            raise ValueError("Checkpoint and covered prefix disagree")
        if generated:
            expected = _normalized_row(
                {
                    "role": "user",
                    "content": _checkpoint_text(view["checkpoint"]),
                }
            )
            if view["entries"][0] != {
                "source_id": None,
                "normalized": expected,
            }:
                raise ValueError(
                    "Generated checkpoint must precede retained sources"
                )
        elif view["checkpoint"] is not None:
            raise ValueError("Checkpoint has no covered sources")
        for entry in view["entries"]:
            if entry["source_id"] is None:
                continue
            original = normalized[entry["source_id"]]
            projected = entry["normalized"]
            if original is None:
                if projected is not None:
                    raise ValueError("Opaque source requires native replay")
                continue
            if projected is None:
                raise ValueError("Portable source has no projected content")
            parsed = llm.NormalizedMessage.model_validate(projected)
            if parsed.role != original.role or len(parsed.parts) != len(
                original.parts
            ):
                raise ValueError("Projected role or parts disagree")
            for before, after in zip(original.parts, parsed.parts):
                before = before.model_dump(mode="json", exclude_none=True)
                after = after.model_dump(mode="json", exclude_none=True)
                field = "text" if before["type"] == "text" else "content"
                mutable = (
                    before["type"] == "text"
                    and original.role == llm.Role.USER
                    and config.enabled
                    and config.input_content != "off"
                ) or (
                    before["type"] == "tool_result"
                    and config.enabled
                    and config.tool_results != "off"
                )
                if mutable:
                    before.pop(field, None)
                    after.pop(field, None)
                if before != after:
                    raise ValueError("Projection changes protected content")
        instructions = []
        for index, item in enumerate(sources):
            if (
                item.system_instructions or item.role == llm.Role.SYSTEM
            ) and any(p.role != llm.Role.SYSTEM for p in sources[:index]):
                raise ValueError("Instructions occur after model history")
            instructions.extend(
                c.model_dump(mode="json") for c in item.system_instructions
            )
            if item.role == llm.Role.SYSTEM:
                system = (
                    llm.normalize_by_shape(item)
                    if portable
                    else normalized[item.id]
                )
                if system is None or any(
                    p.type != "text" for p in system.parts
                ):
                    raise ValueError("Opaque system source")
                instructions.extend(
                    a11.to_chunk(p.text or "").model_dump(mode="json")
                    for p in system.parts
                )
        if instructions != view["instructions"]:
            raise ValueError("Source instructions disagree")
        if view["state"] == "accepted":
            if view["shadow"]:
                raise ValueError("Shadow views cannot be accepted")
            _validate_ack(view, view["installation"])
    except (KeyError, TypeError, ValueError, ValidationError) as error:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid Interaction view: {error}",
        ).to_exception() from error


def _validate_ack(view: dict, acknowledgement: dict) -> None:
    if (
        not isinstance(acknowledgement, dict)
        or acknowledgement.get("installed") is not True
        or acknowledgement.get("view_id") != view["view_id"]
        or acknowledgement.get("source_digest") != view["source_digest"]
        or acknowledgement.get("source_revision") != view["source_revision"]
        or type(acknowledgement.get("revision")) not in (str, int)
        or acknowledgement["revision"] == view["source_revision"]
        or not isinstance(acknowledgement.get("owner"), str)
        or not acknowledgement["owner"]
    ):
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "History owner did not acknowledge this Interaction view;"
                " reconcile before retrying"
            ),
        ).to_exception()


def interaction_view_sources(view: dict) -> list[llm.Interaction]:
    """Decode the canonical sources retained independently of a projection.

    Args:
        view: Proposal or accepted view with its complete source archive.

    Returns:
        Independent Interaction records with their original typed fragments.
    """
    validate_interaction_view(view)
    return decode_interactions(view["sources"])


def _require_portable_entries(view: dict) -> None:
    if any(entry["normalized"] is None for entry in view["entries"]):
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "Interaction view retains opaque content requiring a native"
                " replay capability"
            ),
        ).to_exception()


def use_interaction_view(view: dict) -> list[llm.Interaction]:
    """Materialize an accepted portable view as ordinary A11 Interactions.

    Args:
        view: Owner-accepted JSON view with validated provenance.

    Returns:
        A new Interaction chain with the covered prefix excluded. Existing
        NormalizedMessage content uses an SDK producer normalizer; participating
        backends retain responsibility for their wire formatting. Instructions
        attach once at the start. Original typed fragments remain in sources.

    Raises:
        StatusException: Installation is absent or opaque content cannot be
            replayed through portable normalization without losing data.
    """
    validate_interaction_view(view)
    if view["state"] != "accepted":
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Interaction view has not been accepted by its owner",
        ).to_exception()
    _require_portable_entries(view)
    manifest = _manifest(view)
    result = []
    for index, entry in enumerate(view["entries"]):
        identity = entry["source_id"] or _digest([view["view_id"], index])
        content = a11.to_chunk(entry["normalized"])
        proof = {
            "schema": APPLIED_SCHEMA,
            "view_id": view["view_id"],
            "manifest": manifest,
            "installation": view["installation"],
            "index": index,
            "count": len(view["entries"]),
            "source_id": entry["source_id"],
            "content_digest": _digest(entry["normalized"]),
        }
        result.append(
            llm.Interaction(
                id=identity,
                role=llm.Role(entry["normalized"]["role"]),
                previous_interaction_id=result[-1].id if result else "",
                content=[content],
                system_instructions=(
                    [
                        validate_wire(a11.Chunk, chunk)
                        for chunk in view["instructions"]
                    ]
                    if index == 0
                    else []
                ),
                backend_specific_metadata={
                    llm.BACKEND_METADATA_KEY: VIEW_PRODUCER.encode(),
                    VIEW_METADATA: _json(proof).encode(),
                },
            )
        )
    validate_applied_interactions(result)
    return result


@dataclass(frozen=True)
class _Proof:
    view_id: str
    projection_digest: str
    source_id: str | None
    content_digest: str
    index: int
    count: int
    policy: LlmContextConfig


@functools.lru_cache(maxsize=128)
def _proof(raw: bytes) -> _Proof:
    if len(raw) > 8192:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="Applied Interaction proof exceeds 8192 bytes",
        ).to_exception()
    try:
        value = json.loads(raw)
        manifest = value["manifest"]
        if (
            value["schema"] != APPLIED_SCHEMA
            or manifest["schema"] != VIEW_SCHEMA
            or manifest["shadow"] is not False
            or value["view_id"] != _digest(manifest)
        ):
            raise ValueError("Applied view identity disagrees")
        _validate_ack(
            {**manifest, "view_id": value["view_id"]}, value["installation"]
        )
        if (
            type(value["index"]) is not int
            or type(value["count"]) is not int
            or not 0 <= value["index"] < value["count"]
        ):
            raise ValueError("Applied view ordering disagrees")
        return _Proof(
            value["view_id"],
            manifest["projection_digest"],
            value["source_id"],
            value["content_digest"],
            value["index"],
            value["count"],
            decode_config(_json(manifest["policy"])),
        )
    except (KeyError, TypeError, ValueError) as error:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Invalid applied Interaction proof",
        ).to_exception() from error


def _read_projection(item: llm.Interaction) -> llm.NormalizedMessage:
    try:
        proof = _proof(item.backend_specific_metadata[VIEW_METADATA])
        if len(item.content) != 1:
            raise ValueError(
                "Applied view requires one normalized content chunk"
            )
        value = a11.from_chunk(item.content[0])
        if proof.content_digest != _digest(value):
            raise ValueError("Applied view content disagrees")
        return llm.NormalizedMessage.model_validate(value)
    except (KeyError, TypeError, ValueError, ValidationError) as error:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Invalid applied Interaction view",
        ).to_exception() from error


def validate_applied_interactions(interactions: list[llm.Interaction]) -> int:
    """Validate the complete accepted prefix before a backend reads its parts.

    Args:
        interactions: An applied view prefix followed by optional new records.

    Returns:
        Number of verified projected records, or zero for canonical history.
        The serialized proof cache stores immutable metadata only.

    Raises:
        StatusException: An applied prefix is partial, reordered, or changed.
    """
    if (
        not interactions
        or llm.interaction_backend(interactions[0]) != VIEW_PRODUCER
    ):
        if any(
            llm.interaction_backend(item) == VIEW_PRODUCER
            for item in interactions
        ):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "An accepted Interaction view must form the history prefix"
                ),
            ).to_exception()
        return 0
    try:
        first = _proof(interactions[0].backend_specific_metadata[VIEW_METADATA])
        if first.count > len(interactions):
            raise ValueError("Applied view is incomplete")
        entries = []
        for index, item in enumerate(interactions[: first.count]):
            proof = _proof(item.backend_specific_metadata[VIEW_METADATA])
            if (
                proof.view_id != first.view_id
                or proof.index != index
                or proof.count != first.count
                or (index and item.system_instructions)
                or item.id
                != (proof.source_id or _digest([proof.view_id, index]))
                or item.previous_interaction_id
                != (interactions[index - 1].id if index else "")
            ):
                raise ValueError("Applied view order or identity disagrees")
            normalized = _read_projection(item)
            if item.role != normalized.role:
                raise ValueError("Applied view role disagrees")
            entries.append(
                {
                    "source_id": proof.source_id,
                    "normalized": normalized.model_dump(
                        mode="json", exclude_none=True
                    ),
                }
            )
        instructions = [
            chunk.model_dump(mode="json")
            for chunk in interactions[0].system_instructions
        ]
        if (
            _digest({"entries": entries, "instructions": instructions})
            != first.projection_digest
        ):
            raise ValueError("Applied projection digest disagrees")
        if any(
            llm.interaction_backend(item) == VIEW_PRODUCER
            for item in interactions[first.count :]
        ):
            raise ValueError("Applied views cannot be spliced together")
        ids = [item.id for item in interactions]
        if len(set(ids)) != len(ids):
            raise ValueError("Continuation repeats an Interaction identity")
        for previous, item in zip(
            interactions[first.count - 1 :], interactions[first.count :]
        ):
            if (
                item.previous_interaction_id not in ("", previous.id)
                or item.system_instructions
                or item.role == llm.Role.SYSTEM
            ):
                raise ValueError(
                    "Continuation disagrees with the accepted prefix"
                )
        return first.count
    except (KeyError, TypeError, ValueError) as error:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid applied Interaction chain: {error}",
        ).to_exception() from error


llm.register_interaction_normalizer(VIEW_PRODUCER, _read_projection)

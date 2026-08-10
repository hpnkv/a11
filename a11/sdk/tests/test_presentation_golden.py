# Copyright 2026 The A11 Authors.

"""The cross-language presentation contract.

``testdata/presentation_events.json`` holds conversations paired with the blocks
they must be drawn as. Python and TypeScript each derive the blocks and compare,
which is what keeps a terminal, a webview and any future client showing the same
conversation the same way -- and what pins the field names should a gateway ever
stream blocks directly rather than have each client derive them.

Regenerate with ``pytest --regenerate-presentation-golden`` (see `_write`) when
the derivation changes on purpose, and expect the TypeScript test to fail until
`js/src/sdk/presentation.ts` agrees.
"""

from __future__ import annotations

import json
import pathlib

import pytest

import a11
from a11.sdk.llm import (
    TOOL_LOGS_METADATA_KEY,
    Interaction,
    Role,
    UsageMetadata,
)
from a11.sdk.presentation import present_conversation
from a11.status import Status, StatusCode

_GOLDEN = (
    pathlib.Path(__file__).resolve().parents[3]
    / "testdata"
    / "presentation_events.json"
)

_COMMENT = [
    "Conversations, and the presentation blocks they must be drawn as.",
    "Every client derives these from the stored Interactions rather than",
    "being told them, so this is the contract that keeps a terminal and a",
    "webview showing the same conversation the same way.",
    "Kotlin is absent on purpose: its Interaction is an untyped",
    "LinkedHashMap with no introspection, so a mirror there would be",
    "string-keyed guesswork. Revisit when it becomes a real type.",
    "Rebuild with CASES in a11/sdk/tests/test_presentation_golden.py if the",
    "derivation changes on purpose -- and expect js/test/presentation.test.mjs",
    "to fail until it agrees.",
]


def _text(text: str, role: Role = Role.USER) -> Interaction:
    """An interaction as a client mints one: no backend tag at all."""
    return Interaction(
        role=role,
        content=[
            a11.to_chunk(
                {
                    "role": role.value,
                    "content": [{"type": "text", "text": text}],
                }
            )
        ],
    )


def _cases() -> list[dict]:
    """The conversations under contract, each with a name and its interactions."""
    plain = _text("hello there")

    thinking = _text("Let me think.", role=Role.ASSISTANT)

    call = Interaction(
        role=Role.ASSISTANT,
        content=[
            a11.to_chunk(
                {
                    "role": "model",
                    "content": [{"type": "text", "text": "Checking."}],
                }
            )
        ],
        action_calls=[a11.ActionMessage(id="call-1", name="shell_execute")],
    )
    carrier = Interaction(
        role=Role.USER,
        action_outputs={"call-1": []},
        backend_specific_metadata={
            TOOL_LOGS_METADATA_KEY: json.dumps(
                {"call-1": "$ pwd\n/home/helena"}
            ).encode()
        },
    )
    answer = _text("You are in your home directory.", role=Role.ASSISTANT)

    failed = _text("That did not work.", role=Role.ASSISTANT)
    failed.status = Status(
        code=StatusCode.DEADLINE_EXCEEDED, message="took too long"
    )
    failed.usage_metadata = UsageMetadata(input_tokens=11, output_tokens=3)

    system = _text("You are a helpful assistant.", role=Role.SYSTEM)

    return [
        {"name": "a single untagged user message", "interactions": [plain]},
        {
            "name": "a tool round trip with its log",
            "interactions": [plain, call, carrier, answer],
        },
        {
            "name": "a failed turn with usage",
            "interactions": [thinking, failed],
        },
        {
            "name": "system interactions are not drawn",
            "interactions": [system, plain],
        },
    ]


def _encode(interaction: Interaction) -> str:
    """One interaction as the tagged JSON every language can parse."""
    chunk = a11.to_chunk(interaction)
    return chunk.data.decode()


def _blocks_of(interactions) -> list[dict]:
    """The blocks a conversation is drawn as, as portable JSON."""
    blocks: list[dict] = []
    for turn in present_conversation(interactions):
        for block in turn.blocks:
            entry = {
                # `.value`, not `str()`: BlockKind is a StrEnum but Role is a
                # plain Enum, whose str() is "Role.USER" rather than the wire
                # value every language shares.
                "kind": block.kind.value,
                "role": block.role.value,
                "text": block.text,
            }
            if block.id:
                entry["id"] = block.id
            if block.tool_name:
                entry["tool_name"] = block.tool_name
            if block.status is not None:
                entry["status_code"] = block.status.code.name
            if block.usage is not None:
                entry["usage"] = {
                    "input_tokens": block.usage.input_tokens,
                    "output_tokens": block.usage.output_tokens,
                }
            blocks.append(entry)
    return blocks


def _build() -> dict:
    return {
        "_comment": _COMMENT,
        "cases": [
            {
                "name": case["name"],
                "interactions": [
                    _encode(interaction) for interaction in case["interactions"]
                ],
                "blocks": _blocks_of(case["interactions"]),
            }
            for case in _cases()
        ],
    }


def _write() -> None:
    """Regenerate the fixture from the current derivation."""
    _GOLDEN.write_text(json.dumps(_build(), indent=2) + "\n")


def test_the_golden_file_matches_what_python_derives():
    """The fixture is the contract, so Python must still satisfy it."""
    assert _GOLDEN.exists(), (
        f"{_GOLDEN} is missing; regenerate it with"
        " `python -c \"from a11.sdk.tests.test_presentation_golden import"
        ' _write; _write()"`'
    )
    golden = json.loads(_GOLDEN.read_text())
    current = _build()

    assert [case["name"] for case in golden["cases"]] == [
        case["name"] for case in current["cases"]
    ]
    for stored, derived in zip(golden["cases"], current["cases"]):
        assert stored["blocks"] == derived["blocks"], stored["name"]


@pytest.mark.parametrize("index", range(4))
def test_each_case_round_trips_from_its_encoded_interactions(index: int):
    """The blocks come from *decoding* the fixture, as another language would.

    Deriving from the in-memory objects would not prove that the encoded form
    carries everything the derivation needs -- the tool logs in particular ride
    in metadata that is easy to lose.
    """
    golden = json.loads(_GOLDEN.read_text())
    case = golden["cases"][index]
    decoded = [
        a11.from_chunk(
            a11.Chunk(
                data=payload.encode(),
                metadata=a11.ChunkMetadata(
                    mimetype="application/json;type=a11.sdk.Interaction"
                ),
            ),
            obj_type=Interaction,
        )
        for payload in case["interactions"]
    ]
    assert _blocks_of(decoded) == case["blocks"], case["name"]

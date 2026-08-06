# Copyright 2026 The A11 Authors.

"""The Python half of the cross-language tag contract.

`testdata/serial_tags.json` is the one table every language answers to. These
tests assert Python's constants against it, that the types actually carrying
those tags agree, and that a payload written before the tags were canonicalised
still reads — the three ways this contract can quietly rot.
"""

import base64
import json
import pathlib

import a11
import pytest
from a11.data import serial_tags
from a11.cli.backends import make_user_interaction
from a11.data.serialization import CORE_TYPE_TAGS, _declared_serial_tag
from a11.sdk.llm import (
    A11ActionConfig,
    A11Peer,
    Interaction,
    Role,
    UsageMetadata,
)
from a11.status import Status

_TESTDATA = pathlib.Path(__file__).resolve().parents[3] / "testdata"
_FIXTURE = _TESTDATA / "serial_tags.json"
_GOLDEN = _TESTDATA / "interaction_golden.json"
_TEXT_MESSAGE_GOLDEN = _TESTDATA / "text_message_interaction_golden.json"


def golden_interaction() -> Interaction:
    """The interaction behind `testdata/interaction_golden.json`.

    Every language decodes that fixture, rebuilds what it nests, re-encodes and
    must reproduce the bytes. Change this and rewrite the fixture from it — the
    other languages' suites will fail until they agree, which is the point.
    """
    return Interaction(
        id="00000000-0000-4000-8000-000000000000",
        role=Role.ASSISTANT,
        model="golden-model",
        content=[a11.to_chunk({"t": 1}), a11.to_chunk("plain")],
        system_instructions=[a11.to_chunk("sys")],
        action_configs={"x": A11ActionConfig()},
        action_calls=[a11.ActionMessage(id="call-1", name="rename_symbol")],
        action_inputs={
            "p": [a11.NodeFragment(data=a11.to_chunk("frag"), id="n1", seq=0)]
        },
        backend_specific_metadata={"stop": b"end_turn"},
        usage_metadata=UsageMetadata(input_tokens=3, output_tokens=7),
    )


def _fixture_tags() -> dict[str, str]:
    data = json.loads(_FIXTURE.read_text())
    tags: dict[str, str] = {}
    for section, entries in data.items():
        if section.startswith("_"):
            continue
        tags.update(entries)
    return tags


def test_every_fixture_tag_has_a_python_constant():
    constants = {
        getattr(serial_tags, name)
        for name in serial_tags.__all__
        if name != "SERIAL_TAG_ATTRIBUTE"
    }
    missing = set(_fixture_tags().values()) - constants
    assert not missing, (
        f"testdata/serial_tags.json tags absent from Python: {missing}"
    )


def test_every_python_constant_is_in_the_fixture():
    declared = set(_fixture_tags().values())
    extra = {
        getattr(serial_tags, name)
        for name in serial_tags.__all__
        if name != "SERIAL_TAG_ATTRIBUTE"
    } - declared
    assert not extra, (
        f"Python tags absent from testdata/serial_tags.json: {extra}"
    )


@pytest.mark.parametrize(
    "model, tag",
    [
        (Interaction, serial_tags.INTERACTION),
        (A11Peer, serial_tags.PEER),
        (A11ActionConfig, serial_tags.ACTION_CONFIG),
        (UsageMetadata, serial_tags.USAGE_METADATA),
    ],
)
def test_sdk_models_declare_their_tag(model, tag):
    assert _declared_serial_tag(model) == tag


def test_core_types_are_pinned_to_their_canonical_tag():
    assert CORE_TYPE_TAGS[a11.Chunk] == serial_tags.CHUNK
    assert CORE_TYPE_TAGS[Status] == serial_tags.STATUS


def test_a_subclass_does_not_inherit_its_base_tag():
    """Inheriting a tag would make a subclass serialize as its base."""

    class Narrower(Interaction):
        pass

    assert _declared_serial_tag(Narrower) is None


def test_the_canonical_tags_reach_the_wire():
    interaction = Interaction(
        content=[a11.to_chunk("hi")],
        action_configs={"x": A11ActionConfig()},
        usage_metadata=UsageMetadata(input_tokens=1),
    )
    chunk = a11.to_chunk(interaction)

    expected = f"application/json;type={serial_tags.INTERACTION}"
    assert chunk.get_mimetype() == expected
    payload = json.loads(bytes(chunk.data))
    assert payload["content"][0]["class_name"] == serial_tags.CHUNK
    assert payload["status"]["class_name"] == serial_tags.STATUS
    config = payload["action_configs"]["x"]
    assert config["class_name"] == serial_tags.ACTION_CONFIG
    assert payload["usage_metadata"]["class_name"] == serial_tags.USAGE_METADATA


def test_python_still_writes_the_golden_interaction():
    """The fixture the other languages are held to must stay Python's output."""
    golden = json.loads(_GOLDEN.read_text())
    chunk = a11.to_chunk(golden_interaction())

    assert chunk.get_mimetype() == golden["mimetype"]
    assert base64.b64encode(bytes(chunk.data)).decode() == golden["base64"]


def test_the_golden_interaction_round_trips():
    golden = json.loads(_GOLDEN.read_text())
    chunk = a11.Chunk(
        data=base64.b64decode(golden["base64"]),
        metadata=a11.ChunkMetadata(mimetype=golden["mimetype"]),
    )

    decoded = a11.from_chunk(chunk, "", Interaction)
    reencoded = a11.to_chunk(decoded)

    assert bytes(reencoded.data) == base64.b64decode(golden["base64"])


def test_python_still_writes_the_golden_text_message_interaction():
    """The shape every language's `makeTextMessageInteraction` is held to."""
    golden = json.loads(_TEXT_MESSAGE_GOLDEN.read_text())
    interaction = make_user_interaction(golden["text"])
    interaction.system_instructions = [a11.to_chunk(golden["system_prompt"])]
    interaction.id = json.loads(base64.b64decode(golden["base64"]))["id"]

    chunk = a11.to_chunk(interaction)

    assert chunk.get_mimetype() == golden["mimetype"]
    assert base64.b64encode(bytes(chunk.data)).decode() == golden["base64"]


def test_an_interaction_from_another_language_validates():
    """What TypeScript and Kotlin send on the `interactions` port.

    They leave `status`, `created_at_millis` and `usage_metadata` to this
    model's defaults rather than spelling them out, so the payload is
    equivalent to Python's rather than identical — it still has to validate.
    Above all `content` and `system_instructions` arrive as chunks: a bare
    string there is what produced "Chunk must be validated from a mapping."
    """
    golden = json.loads(_TEXT_MESSAGE_GOLDEN.read_text())
    payload = json.loads(base64.b64decode(golden["base64"]))
    for omitted in ("status", "created_at_millis", "usage_metadata"):
        payload.pop(omitted)
    chunk = a11.Chunk(
        data=json.dumps(payload).encode(),
        metadata=a11.ChunkMetadata(mimetype=golden["mimetype"]),
    )

    decoded = a11.from_chunk(chunk, "", Interaction)

    assert isinstance(decoded, Interaction)
    assert a11.from_chunk(decoded.content[0]) == {
        "role": "user",
        "content": [{"type": "text", "text": golden["text"]}],
    }
    assert a11.from_chunk(decoded.system_instructions[0]) == (
        golden["system_prompt"]
    )


def test_a_payload_written_before_the_rename_still_reads():
    """Peers on the previous release wrote bare and module-qualified names."""
    legacy = {
        "id": "x",
        "role": "user",
        "status": {
            "__a11_serialized_type__": "a11.value",
            "value": {"code": 0, "message": ""},
            "class_name": "Status",
        },
        "content": [
            {
                "__a11_serialized_type__": "a11.value",
                "value": {
                    "data": {
                        "__a11_serialized_type__": "bytes",
                        "value": "ImhpIg==",
                    },
                    "metadata": {"mimetype": "application/json;type=string"},
                },
                "class_name": "Chunk",
            }
        ],
        "usage_metadata": {
            "__a11_serialized_type__": "pydantic",
            "value": {"input_tokens": 2},
            "class_name": "a11.sdk.llm.UsageMetadata",
        },
    }
    chunk = a11.Chunk(
        data=json.dumps(legacy).encode(),
        metadata=a11.ChunkMetadata(
            mimetype="application/json;type=a11.sdk.llm.Interaction"
        ),
    )

    decoded = a11.from_chunk(chunk, "", Interaction)

    assert isinstance(decoded, Interaction)
    assert a11.from_chunk(decoded.content[0]) == "hi"
    assert decoded.usage_metadata.input_tokens == 2

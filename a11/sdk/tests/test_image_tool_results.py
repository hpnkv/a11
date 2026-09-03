# Copyright 2026 The A11 Authors.

"""What an action's `image/*` output becomes on its way back to a model.

An output port carrying an encoded frame has no JSON form, so it leaves the
result document and travels as the picture each provider has for one. The
Anthropic family puts it in the tool result itself; the three routes whose tool
message is text-only send a user message beside it.
"""

import base64

import pytest

import a11
from a11.actions import ActionPortSchema, ActionRegistry, ActionSchema
from a11.sdk import llm
from a11.sdk.anthropic import interact_with_claude
from a11.sdk.anthropic import interact_with_claude_code
from a11.sdk.gemini import interact_with_gemini
from a11.sdk.llm_tools import runner
from a11.sdk.ollama import interact_with_ollama
from a11.sdk.vllm import interact_with_vllm

#: A short PNG signature stands in for a frame; nothing here decodes it.
_FRAME = b"\x89PNG\r\n\x1a\n\x00\x01\x02\xff"

_SCHEMA = ActionSchema(
    name="draw_tool",
    description="Draw one picture and say how wide it is.",
    outputs={
        "picture": ActionPortSchema(
            "picture", "image/png", unary=True, required=True
        ),
        "width": ActionPortSchema(
            "width", "application/json", typeinfo=int, unary=True
        ),
    },
)


async def _draw(action: a11.Action) -> None:
    await action["picture"].finalize(_FRAME, mimetype="image/png")
    await action["width"].finalize(4)


class _Executed:
    """One finished call, in the shape `build_tool_results` reads."""

    def __init__(self, fragments: list[a11.NodeFragment]) -> None:
        self.outputs = {"call-1": fragments}

    def error_message(self, call_id: str) -> str | None:
        return None


async def _fragments() -> list[a11.NodeFragment]:
    """One `draw_tool` run's outputs, as `build_tool_results` reads them."""
    registry = ActionRegistry()
    registry.register(_SCHEMA.name, _SCHEMA, _draw)
    action = registry.make_action(_SCHEMA.name)
    action.run()
    collected, failure = await runner.collect_action_outputs(
        action, a11.now() + a11.infinite_duration()
    )
    assert failure is None
    return collected


@pytest.mark.asyncio
async def test_an_image_output_leaves_the_result_document():
    text, images = await llm.decoded_output_content(await _fragments())

    assert text == '{"width":4}'
    assert len(images) == 1
    assert images[0].type == llm.NormalizedContentType.IMAGE
    assert images[0].mime_type == "image/png"
    assert base64.b64decode(images[0].data) == _FRAME


@pytest.mark.asyncio
async def test_an_action_with_no_image_output_answers_as_text():
    registry = ActionRegistry()
    schema = ActionSchema(
        name="count_tool",
        outputs={
            "total": ActionPortSchema(
                "total", "application/json", typeinfo=int, unary=True
            )
        },
    )

    async def handler(action: a11.Action) -> None:
        await action["total"].finalize(7)

    registry.register(schema.name, schema, handler)
    action = registry.make_action(schema.name)
    action.run()
    collected, _ = await runner.collect_action_outputs(
        action, a11.now() + a11.infinite_duration()
    )

    assert await llm.decoded_output_content(collected) == ('{"total":7}', [])


@pytest.mark.asyncio
async def test_binary_output_is_base64_rather_than_a_failed_call():
    """A msgpack or packed-bytes port is not UTF-8, and still answers."""
    registry = ActionRegistry()
    schema = ActionSchema(
        name="pack_tool",
        outputs={
            "packed": ActionPortSchema(
                "packed", "application/octet-stream", unary=True
            )
        },
    )

    async def handler(action: a11.Action) -> None:
        await action["packed"].finalize(b"\xff\xd8\xff")

    registry.register(schema.name, schema, handler)
    action = registry.make_action(schema.name)
    action.run()
    collected, _ = await runner.collect_action_outputs(
        action, a11.now() + a11.infinite_duration()
    )

    text, images = await llm.decoded_output_content(collected)
    assert images == []
    assert text == '{"packed":"_9j_"}'


@pytest.mark.asyncio
async def test_claude_carries_the_frame_in_the_tool_result():
    results = await interact_with_claude._build_tool_results_from_outputs(
        _Executed(await _fragments())
    )

    assert len(results) == 1
    blocks = results[0]["content"]
    assert blocks[0] == {"type": "text", "text": '{"width":4}'}
    assert blocks[1]["type"] == "image"
    assert blocks[1]["source"]["media_type"] == "image/png"
    assert base64.b64decode(blocks[1]["source"]["data"]) == _FRAME


@pytest.mark.asyncio
async def test_claude_code_carries_the_frame_as_mcp_image_content():
    results = await llm.build_tool_results(
        _Executed(await _fragments()),
        interact_with_claude_code._as_mcp_result,
    )

    assert len(results) == 1
    blocks = results[0]["content"]
    assert blocks[0] == {"type": "text", "text": '{"width":4}'}
    assert blocks[1]["type"] == "image"
    assert blocks[1]["mimeType"] == "image/png"
    assert base64.b64decode(blocks[1]["data"]) == _FRAME


@pytest.mark.asyncio
async def test_gemini_sends_the_frame_as_a_step_beside_the_result():
    steps = await interact_with_gemini._build_tool_results_from_outputs(
        _Executed(await _fragments()), {"call-1": "draw_tool"}
    )

    assert [step["type"] for step in steps] == [
        "function_result",
        "user_input",
    ]
    image = steps[1]["content"][1]
    assert image["mime_type"] == "image/png"
    assert base64.b64decode(image["data"]) == _FRAME


@pytest.mark.asyncio
async def test_ollama_sends_the_frame_on_a_user_message():
    messages = await interact_with_ollama._build_tool_results_from_outputs(
        _Executed(await _fragments()), {"call-1": "draw_tool"}
    )

    assert [message["role"] for message in messages] == ["tool", "user"]
    assert messages[0]["content"] == '{"width":4}'
    assert base64.b64decode(messages[1]["images"][0]) == _FRAME


@pytest.mark.asyncio
async def test_vllm_sends_the_frame_as_an_image_url_part():
    messages = await interact_with_vllm._build_tool_results_from_outputs(
        _Executed(await _fragments())
    )

    assert [message["role"] for message in messages] == ["tool", "user"]
    assert messages[0]["content"] == '{"width":4}'
    url = messages[1]["content"][1]["image_url"]["url"]
    header, _, data = url.partition(",")
    assert header == "data:image/png;base64"
    assert base64.b64decode(data) == _FRAME


@pytest.mark.parametrize(
    ("backend", "content", "mime_type"),
    [
        (
            llm.Backend.CLAUDE,
            {
                "role": "user",
                "content": [
                    {
                        "type": "tool_result",
                        "tool_use_id": "call-1",
                        "content": [
                            {"type": "text", "text": '{"width":4}'},
                            {
                                "type": "image",
                                "source": {
                                    "type": "base64",
                                    "media_type": "image/png",
                                    "data": "QUJD",
                                },
                            },
                        ],
                    }
                ],
            },
            "image/png",
        ),
        (
            llm.Backend.CLAUDE_CODE,
            {
                "role": "user",
                "content": [
                    {
                        "type": "tool_result",
                        "tool_use_id": "call-1",
                        "content": [
                            {"type": "text", "text": '{"width":4}'},
                            {
                                "type": "image",
                                "mimeType": "image/png",
                                "data": "QUJD",
                            },
                        ],
                    }
                ],
            },
            "image/png",
        ),
        (
            llm.Backend.GEMINI,
            {
                "role": "user",
                "content": [
                    {
                        "type": "function_result",
                        "call_id": "call-1",
                        "result": [{"type": "text", "text": '{"width":4}'}],
                    },
                    {
                        "type": "user_input",
                        "content": [
                            {"type": "text", "text": "Images follow."},
                            {
                                "type": "image",
                                "mime_type": "image/png",
                                "data": "QUJD",
                            },
                        ],
                    },
                ],
            },
            "image/png",
        ),
        (
            llm.Backend.OLLAMA,
            {
                "messages": [
                    {
                        "role": "tool",
                        "tool_name": "draw_tool",
                        "content": '{"width":4}',
                    },
                    {
                        "role": "user",
                        "content": "Images follow.",
                        "images": ["QUJD"],
                    },
                ]
            },
            None,
        ),
        (
            llm.Backend.VLLM,
            {
                "messages": [
                    {
                        "role": "tool",
                        "tool_call_id": "call-1",
                        "content": '{"width":4}',
                    },
                    {
                        "role": "user",
                        "content": [
                            {"type": "text", "text": "Images follow."},
                            {
                                "type": "image_url",
                                "image_url": {
                                    "url": "data:image/png;base64,QUJD"
                                },
                            },
                        ],
                    },
                ]
            },
            "image/png",
        ),
    ],
)
def test_tool_output_images_survive_provider_normalization(
    backend: llm.Backend, content: dict, mime_type: str | None
):
    interaction = llm.Interaction(
        role=llm.Role.USER,
        content=[a11.to_chunk(content)],
        backend_specific_metadata={
            llm.BACKEND_METADATA_KEY: str(backend).encode()
        },
    )

    normalized = llm.normalize_interaction(interaction)

    results = [
        part
        for part in normalized.parts
        if part.type == llm.NormalizedContentType.TOOL_RESULT
    ]
    images = [
        part
        for part in normalized.parts
        if part.type == llm.NormalizedContentType.IMAGE
    ]
    assert len(results) == 1
    assert results[0].content == '{"width":4}'
    assert len(images) == 1
    assert images[0].data == "QUJD"
    assert images[0].mime_type == mime_type


@pytest.mark.asyncio
async def test_a_failed_call_reports_its_status_and_no_frame():
    class Failed:
        outputs = {"call-1": []}

        def error_message(self, call_id: str) -> str | None:
            return "NOT_FOUND: no such floor"

    results = await interact_with_claude._build_tool_results_from_outputs(
        Failed()
    )

    assert results[0]["is_error"] is True
    assert results[0]["content"] == "NOT_FOUND: no such floor"

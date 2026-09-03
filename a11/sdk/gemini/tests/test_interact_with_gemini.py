# Copyright 2026 The A11 Authors.

import a11
from types import SimpleNamespace

from a11.sdk.gemini import interact_with_gemini as mod
from a11.sdk.llm import Interaction, Role


def test_raw_image_chunks_become_gemini_input_parts():
    conversation = mod.Conversation()
    conversation.feed_next_interaction(
        Interaction(
            role=Role.USER,
            content=[
                a11.to_chunk("what is this?"),
                a11.to_chunk(b"ABC", "image/png"),
            ],
        )
    )

    assert conversation.incremental_input == [
        {
            "type": "user_input",
            "content": [
                {"type": "text", "text": "what is this?"},
                {"type": "image", "data": "QUJD", "mime_type": "image/png"},
            ],
        }
    ]


def test_streamed_output_images_are_preserved_in_model_steps():
    accumulator = mod._StepAccumulator()
    accumulator.start(0, SimpleNamespace(type="model_output"))
    accumulator.delta(
        0,
        SimpleNamespace(type="image", data="QUJD", mime_type="image/png"),
    )

    assert accumulator.finalize() == [
        {
            "type": "model_output",
            "content": [
                {"type": "image", "data": "QUJD", "mime_type": "image/png"}
            ],
        }
    ]

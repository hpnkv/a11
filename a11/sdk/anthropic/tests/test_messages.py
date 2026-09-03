# Copyright 2026 The A11 Authors.

import json

import a11

from a11.sdk.anthropic.messages import Conversation
from a11.sdk.llm import Interaction, Role


def test_raw_image_chunks_become_anthropic_image_blocks():
    conversation = Conversation()
    conversation.feed_next_interaction(
        Interaction(
            role=Role.USER,
            content=[
                a11.to_chunk("what is this?"),
                a11.to_chunk(b"ABC", "image/jpeg"),
            ],
        )
    )

    assert conversation.messages == [
        {
            "role": "user",
            "content": [
                {"type": "text", "text": "what is this?"},
                {
                    "type": "image",
                    "source": {
                        "type": "base64",
                        "media_type": "image/jpeg",
                        "data": "QUJD",
                    },
                },
            ],
        }
    ]


def test_a_bare_text_chunk_becomes_a_json_encodable_message():
    """A message a request body carries names its role as a string."""
    conversation = Conversation()
    conversation.feed_next_interaction(
        Interaction(role=Role.USER, content=[a11.to_chunk("what is this?")])
    )

    assert conversation.messages == [
        {"role": "user", "content": "what is this?"}
    ]
    assert json.dumps(conversation.messages)

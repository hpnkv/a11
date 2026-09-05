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

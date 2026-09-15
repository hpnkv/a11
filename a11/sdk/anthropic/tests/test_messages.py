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
from a11.sdk.anthropic import interact_with_claude as claude
from a11.sdk.anthropic.interact_with_claude_schema import CreateMessageConfig
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


def test_cache_boundary_follows_the_stable_system_and_tools_prefix():
    system, tools = claude._cached_prefix(
        "Permanent instructions.",
        [{"name": "lookup", "input_schema": {"type": "object"}}],
        "5m",
    )

    assert system == [
        {
            "type": "text",
            "text": "Permanent instructions.",
            "cache_control": {"type": "ephemeral", "ttl": "5m"},
        }
    ]
    assert "cache_control" not in tools[0]


def test_cache_ttls_keep_the_stable_prefix_longer_than_the_conversation():
    config = CreateMessageConfig()

    assert config.stable_cache_ttl == "1h"
    assert config.conversation_cache_ttl == "5m"


def test_tools_form_the_cache_boundary_without_a_system_prompt():
    system, tools = claude._cached_prefix(
        "", [{"name": "lookup", "input_schema": {}}], "1h"
    )

    assert system is not None
    assert tools[0]["cache_control"] == {
        "type": "ephemeral",
        "ttl": "1h",
    }

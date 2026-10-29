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

"""Validate portable Interaction views and explicit history installation."""

import copy
import json
from unittest.mock import AsyncMock, Mock

import pytest

import a11
from a11 import flow
from a11.sdk import llm
from a11.sdk.anthropic.messages import Conversation as ClaudeConversation
from a11.sdk.llm_context import (
    LlmContextConfig,
    LlmContextHeaders,
    LlmContextServices,
    RetainedContentStore,
    accept_interaction_view,
    compact_host_interactions,
    compact_interactions,
    decode_interactions,
    encode_interactions,
    interaction_view_sources,
    load_interaction_view,
    persist_interaction_view,
    project_interactions,
    project_messages,
    register,
    use_interaction_view,
    validate_interaction_view,
)
from a11.sdk.llm_context.config import config_for
from a11.sdk.llm_context.interactions import (
    _digest,
    _proof,
    _view_id,
    validate_applied_interactions,
)
from a11.sdk.llm_context.proxy import project_request
from a11.sdk.llm_context.runtime import (
    interaction_stage,
    prepare_interaction_result,
    prepare_interactions,
    prepare_messages,
    prepare_tool_result,
)
from a11.sdk.vllm.interact_with_vllm import Conversation as ChatConversation
from a11.status import StatusCode, StatusException

POLICY = LlmContextConfig(
    boundary="interaction",
    compaction="summary",
    max_tokens=400,
    reserve_tokens=0,
    retain_recent=1,
)


def sources():
    old = llm.Interaction(
        content=[a11.to_chunk("record " * 2000)],
        system_instructions=[a11.to_chunk("Keep exact identifiers")],
    )
    recent = llm.Interaction(
        content=[a11.to_chunk("What is the identifier?")],
        previous_interaction_id=old.id,
    )
    return [old, recent]


class Owner:
    def __init__(self, interactions, revision=7):
        self.interactions = interactions
        self.revision = revision
        self.view = None
        self.calls = 0

    def read(self):
        return {"interactions": self.interactions, "revision": self.revision}

    def install(self, proposal, *, expected_revision, source_digest):
        assert expected_revision == self.revision
        assert source_digest == _digest(encode_interactions(self.interactions))
        self.calls += 1
        self.view = copy.deepcopy(proposal)
        self.revision += 1
        return {
            "installed": True,
            "owner": "test-host",
            "view_id": proposal["view_id"],
            "source_digest": source_digest,
            "source_revision": expected_revision,
            "revision": self.revision,
        }


async def accepted(interactions=None, config=POLICY, **options):
    owner = Owner(sources() if interactions is None else interactions)
    proposal = await project_interactions(
        owner.interactions, config, source_revision=owner.revision, **options
    )
    return owner, await accept_interaction_view(
        proposal, owner.read, owner.install
    )


async def invoke(registry, name, request, headers=None):
    action = registry.make_action(name)
    for key, value in (headers or {}).items():
        action.set_header(key, value)
    action.run()
    await action["request"].finalize(request)
    result = await action["result"].consume(dict)
    await action.wait()
    return {"result": result}


@pytest.mark.asyncio
async def test_explicit_acceptance_removes_prefix_and_keeps_canonical_sources():
    original = sources()
    before = encode_interactions(original)
    owner = Owner(original)
    proposal = await compact_interactions(
        original, POLICY, checkpoint="identifier ALPHA-17", source_revision=7
    )
    assert proposal["state"] == "proposal"
    assert owner.calls == 0
    with pytest.raises(StatusException, match="not been accepted"):
        use_interaction_view(proposal)
    view = await accept_interaction_view(proposal, owner.read, owner.install)
    replay = use_interaction_view(json.loads(json.dumps(view)))
    assert [entry["source_id"] for entry in view["entries"]] == [
        None,
        original[1].id,
    ]
    assert replay[0].id != original[0].id
    assert replay[1].id == original[1].id
    assert replay[1].previous_interaction_id == replay[0].id
    assert (
        a11.from_chunk(replay[0].system_instructions[0])
        == "Keep exact identifiers"
    )
    assert validate_applied_interactions(replay) == 2
    assert encode_interactions(original) == before
    assert encode_interactions(interaction_view_sources(view)) == before
    assert owner.interactions is original and owner.calls == 1
    assert not replay[1].system_instructions


@pytest.mark.asyncio
async def test_checkpoint_replays_through_chat_and_claude_once():
    summarizer = Mock(return_value="identifier ALPHA-17")
    owner, view = await accepted(summarizer=summarizer)
    replay = use_interaction_view(view)
    for conversation in (ChatConversation(), ClaudeConversation()):
        for item in decode_interactions(encode_interactions(replay)):
            conversation.feed_next_interaction(item)
        assert "ALPHA-17" in json.dumps(conversation.messages)
        assert "What is the identifier?" in json.dumps(conversation.messages)
        assert conversation.system_prompt == "Keep exact identifiers"
        assert "record record" not in json.dumps(conversation.messages)
    assert summarizer.call_count == 1
    assert owner.calls == 1


@pytest.mark.asyncio
async def test_interactions_and_provider_messages_share_engine():
    history = [llm.Interaction(content=[a11.to_chunk('{  "value": 1   }')])]
    config = LlmContextConfig(input_content="lossless", min_gain=0)
    view = await project_interactions(history, config)
    provider = await project_messages(
        [
            {
                "role": "user",
                "content": [{"type": "text", "text": '{  "value": 1   }'}],
            }
        ],
        config,
    )
    assert (
        view["entries"][0]["normalized"]["parts"][0]["text"]
        == provider[0]["content"][0]["text"]
    )
    assert view["entries"][0]["normalized"]["parts"][0]["text"] == '{"value":1}'
    calls = []

    def summarize(prefix):
        calls.append(prefix)
        return "identifier ALPHA-17"

    projected = await project_interactions(
        sources(), POLICY, summarizer=summarize
    )
    await project_messages(
        calls[0] + [{"role": "user", "content": "now"}],
        POLICY,
        summarizer=summarize,
    )
    assert len(calls) == 2
    assert projected["covered_ids"] == [calls[0][0]["id"]]
    assert calls[0] == calls[1]


@pytest.mark.asyncio
async def test_tool_groups_preserve_arguments_results_and_original_fragments():
    from a11.sdk.anthropic import interact_with_claude  # noqa: F401

    history = sources()
    call = llm.Interaction(
        role=llm.Role.ASSISTANT,
        content=[
            a11.to_chunk(
                {
                    "role": "assistant",
                    "content": [
                        {
                            "type": "tool_use",
                            "id": "call",
                            "name": "lookup",
                            "input": {"key": "ALPHA-17"},
                        }
                    ],
                }
            )
        ],
        backend_specific_metadata={"backend": b"claude"},
    )
    result = llm.Interaction(
        content=[
            a11.to_chunk(
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "tool_result",
                            "tool_use_id": "call",
                            "content": '{  "value": 1   }',
                        }
                    ],
                }
            )
        ],
        backend_specific_metadata={"backend": b"claude"},
    )
    history = [
        history[0],
        call,
        result,
        llm.Interaction(content=[a11.to_chunk("next")]),
    ]
    for before, after in zip(history, history[1:]):
        after.previous_interaction_id = before.id
    config = POLICY.model_copy(update={"retain_recent": 2})
    original = encode_interactions(history)
    _, view = await accepted(history, config, checkpoint="old context")
    assert view["covered_ids"] == [history[0].id]
    assert [
        p["type"]
        for e in view["entries"][1:3]
        for p in e["normalized"]["parts"]
    ] == ["tool_call", "tool_result"]
    replay = use_interaction_view(view)
    chat = ChatConversation()
    for item in replay:
        chat.feed_next_interaction(item)
    assert chat.messages[1]["tool_calls"][0]["id"] == "call"
    assert chat.messages[2]["tool_call_id"] == "call"
    assert json.loads(
        chat.messages[1]["tool_calls"][0]["function"]["arguments"]
    ) == {"key": "ALPHA-17"}
    assert encode_interactions(history) == original


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "block",
    [
        {"type": "thinking", "thinking": "secret", "signature": "signed"},
        {"type": "vendor-block", "payload": "unknown"},
        {
            "type": "image_url",
            "image_url": {"url": "https://host/image.png", "detail": "high"},
        },
        {
            "type": "image_url",
            "image_url": {
                "url": "data:image/png;base64,YQ==",
                "detail": "high",
            },
        },
    ],
)
async def test_opaque_content_is_archived_and_replay_fails(
    block,
):
    item = llm.Interaction(
        content=[
            a11.to_chunk(
                {
                    "role": "user",
                    "content": [{"type": "text", "text": "text"}, block],
                }
            )
        ]
    )
    before = encode_interactions([item])
    _, view = await accepted([item], LlmContextConfig())
    assert view["entries"][0]["normalized"] is None
    assert view["sources"] == before
    with pytest.raises(StatusException, match="opaque"):
        use_interaction_view(view)
    with pytest.raises(StatusException, match="Protected history"):
        await compact_interactions(
            [
                item,
                sources()[1].model_copy(
                    update={"previous_interaction_id": item.id}
                ),
            ],
            POLICY.model_copy(update={"max_tokens": 1}),
            checkpoint="summary",
        )


@pytest.mark.asyncio
async def test_raw_image_and_typed_sources_survive_with_protected_boundary():
    history = sources()
    history[1].content.append(
        a11.Chunk(
            metadata=a11.ChunkMetadata(mimetype="image/png"), data=b"\x89PNG"
        )
    )
    before = encode_interactions(history)
    _, view = await accepted(history, checkpoint="old records")
    image = view["entries"][-1]["normalized"]["parts"][-1]
    assert image == {
        "type": "image",
        "data": "iVBORw==",
        "mime_type": "image/png",
    }
    assert encode_interactions(interaction_view_sources(view)) == before
    for conversation in (ChatConversation(), ClaudeConversation()):
        for item in use_interaction_view(view):
            conversation.feed_next_interaction(item)
        assert "iVBORw==" in json.dumps(conversation.messages)


@pytest.mark.asyncio
@pytest.mark.parametrize("change", ["revision", "sources"])
async def test_owner_rechecks_sources_before_install(change):
    owner = Owner(sources())
    proposal = await project_interactions(
        owner.interactions, POLICY, checkpoint="old", source_revision=7
    )
    if change == "revision":
        owner.revision += 1
    else:
        owner.interactions[1].content = [a11.to_chunk("changed")]
    with pytest.raises(StatusException) as error:
        await accept_interaction_view(proposal, owner.read, owner.install)
    assert error.value.status.code == StatusCode.ABORTED
    assert owner.calls == 0


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "ack", [None, {}, {"installed": True}, {"installed": False}]
)
async def test_uncertain_installation_fails_without_retry(ack):
    owner = Owner(sources())
    proposal = await project_interactions(
        owner.interactions, POLICY, checkpoint="old", source_revision=7
    )
    install = Mock(return_value=ack)
    with pytest.raises(StatusException) as error:
        await accept_interaction_view(proposal, owner.read, install)
    assert error.value.status.code == StatusCode.FAILED_PRECONDITION
    install.assert_called_once()


@pytest.mark.asyncio
async def test_projection_tamper_and_partial_replay_fail():
    _, view = await accepted(checkpoint="identifier ALPHA-17")
    for field in ("source_digest", "projection_digest", "view_id"):
        invalid = copy.deepcopy(view)
        invalid[field] = "modified"
        with pytest.raises(StatusException):
            validate_interaction_view(invalid)
    for mutate in (
        lambda records: records.reverse(),
        lambda records: records.pop(),
        lambda records: setattr(
            records[1], "previous_interaction_id", "foreign"
        ),
        lambda records: setattr(records[0], "system_instructions", []),
        lambda records: setattr(
            records[0], "content", [a11.to_chunk("replacement")]
        ),
    ):
        records = use_interaction_view(view)
        mutate(records)
        with pytest.raises(StatusException):
            validate_applied_interactions(records)


@pytest.mark.asyncio
async def test_recomputed_digest_cannot_hide_protected_source_changes():
    _, view = await accepted(checkpoint="identifier ALPHA-17")
    invalid = copy.deepcopy(view)
    invalid["instructions"] = []
    invalid["projection_digest"] = _digest(
        {"entries": invalid["entries"], "instructions": []}
    )
    invalid["view_id"] = _view_id(invalid)
    invalid["installation"]["view_id"] = invalid["view_id"]
    with pytest.raises(StatusException, match="instructions disagree"):
        validate_interaction_view(invalid)


@pytest.mark.asyncio
async def test_host_callbacks_leave_inference_and_persistence_with_host():
    owner = Owner(sources())

    async def read():
        return owner.read()

    async def summarize(prefix):
        return "identifier ALPHA-17"

    view = await compact_host_interactions(
        read, owner.install, POLICY, summarizer=summarize
    )
    assert view["state"] == "accepted"
    assert owner.calls == 1
    assert "record " in a11.from_chunk(owner.interactions[0].content[0])


@pytest.mark.asyncio
async def test_retention_reopen_revision_scope_and_duplicate_operation(
    tmp_path,
):
    owner = Owner(sources(), revision=0)
    proposal = await project_interactions(
        owner.interactions, POLICY, checkpoint="old", source_revision=0
    )
    scope = {
        "owner": "host",
        "conversation": "one",
        "expected_revision": 0,
        "operation_id": "install",
    }
    store = RetainedContentStore(tmp_path, owner="host")
    view = await persist_interaction_view(proposal, store, scope)
    assert view["installation"]["revision"] == 1
    assert (await persist_interaction_view(proposal, store, scope))[
        "view_id"
    ] == view["view_id"]
    reopened = RetainedContentStore(tmp_path, owner="host")
    loaded = await load_interaction_view(reopened, scope)
    assert loaded["view_id"] == view["view_id"]
    assert validate_applied_interactions(use_interaction_view(loaded)) == 2
    assert (
        await load_interaction_view(reopened, {**scope, "conversation": "two"})
        is None
    )
    with pytest.raises(StatusException) as error:
        await persist_interaction_view(
            proposal, reopened, {**scope, "operation_id": "stale"}
        )
    assert error.value.status.code == StatusCode.ABORTED


@pytest.mark.asyncio
async def test_action_and_flow_build_identical_views_and_use_explicit_owner(
    tmp_path,
):
    registry = a11.ActionRegistry()
    store = RetainedContentStore(tmp_path, owner="host")
    register(
        registry,
        services=LlmContextServices(
            retention=store, authorize_scope=lambda action, scope: scope
        ),
    )
    request = {
        "interactions": encode_interactions(sources()),
        "checkpoint": "identifier ALPHA-17",
    }
    headers = {LlmContextHeaders.CONFIG: POLICY.model_dump_json()}
    program = flow.loads("""flow project {
      in request: object
      out result: object
      nodes scratch
      x = run llm_context_compact_interactions(request: request) via scratch
      x.result -> result
    }""")
    program.register_all(registry)
    direct = await invoke(
        registry, "llm_context_compact_interactions", request, headers
    )
    composed = await program.main.invoke(
        {"request": request}, registry=registry, headers=headers
    )
    proposal = direct["result"]
    assert composed["result"]["view_id"] == proposal["view_id"]
    assert proposal["state"] == "proposal"
    scope = {
        "owner": "host",
        "conversation": "one",
        "expected_revision": 0,
        "operation_id": "install",
    }
    installed = await invoke(
        registry,
        "llm_context_accept_interaction_view",
        {"view": proposal, "scope": scope},
        headers,
    )
    result = await invoke(
        registry,
        "llm_context_use_interaction_view",
        {"view": installed["result"]},
    )
    assert (
        validate_applied_interactions(
            decode_interactions(result["result"]["interactions"])
        )
        == 2
    )


@pytest.mark.asyncio
async def test_stage_prevents_repetition_within_one_action(monkeypatch):
    registry = a11.ActionRegistry()
    register(
        registry,
        services=LlmContextServices(
            summarizers={"summary": lambda prefix: "identifier ALPHA-17"}
        ),
    )
    action = registry.make_action("llm_context_project_interactions")
    action.set_header(
        LlmContextHeaders.CONFIG,
        POLICY.model_copy(update={"summarizer": "summary"}).model_dump_json(),
    )
    monkeypatch.setattr("a11.sdk.llm_context.runtime.emit_report", AsyncMock())
    prepared = await prepare_interactions(action, sources())
    assert interaction_stage(action)
    messages = [{"role": "user", "content": '{   "x":1}'}]
    assert await prepare_messages(action, messages) is messages
    raw = "unchanged tool output"
    assert await prepare_tool_result(action, raw, source={}) == raw
    child = action.make_nested(action.schema)
    assert not interaction_stage(child)
    assert config_for(child) is config_for(action)
    _proof.cache_clear()
    validate_applied_interactions(prepared)
    misses = _proof.cache_info().misses
    validate_applied_interactions(prepared)
    assert _proof.cache_info().misses == misses


@pytest.mark.asyncio
async def test_view_continues_without_summary_and_requires_fresh_replay(
    monkeypatch,
):
    summarize = Mock(return_value="identifier ALPHA-17")
    _, view = await accepted(summarizer=summarize)
    registry = a11.ActionRegistry()
    register(registry)
    action = registry.make_action("llm_context_project_interactions")
    action.set_header(LlmContextHeaders.CONFIG, POLICY.model_dump_json())
    records = use_interaction_view(view)
    records.append(
        llm.Interaction(
            content=[a11.to_chunk("Continue")],
            previous_interaction_id=records[-1].id,
        )
    )
    monkeypatch.setattr("a11.sdk.llm_context.runtime.emit_report", AsyncMock())
    prepared = await prepare_interactions(action, records)
    assert len(prepared) == 3
    assert summarize.call_count == 1
    for item in prepared:
        llm.normalize_interaction(item, strict=True)
    with pytest.raises(StatusException, match="fresh replay"):
        await prepare_interactions(action, records, stateful=True)


@pytest.mark.asyncio
async def test_proxy_rejects_interaction_policy_without_interaction_input():
    with pytest.raises(StatusException, match="Interaction inputs"):
        await project_request(
            b'{"messages":[]}',
            path="/v1/chat/completions",
            profile="chat",
            config=POLICY,
        )


@pytest.mark.asyncio
async def test_accepted_view_replay_requires_no_source_producer_normalizer(
    monkeypatch,
):
    owner, view = await accepted(checkpoint="identifier ALPHA-17")
    monkeypatch.setattr(
        "a11.sdk.llm_context.interactions._normalized_source",
        Mock(side_effect=AssertionError("Source producer is unavailable")),
    )
    replay = decode_interactions(
        encode_interactions(use_interaction_view(view))
    )
    for conversation in (ChatConversation(), ClaudeConversation()):
        for item in replay:
            conversation.feed_next_interaction(item.model_copy())
        assert "ALPHA-17" in json.dumps(conversation.messages)
    assert owner.calls == 1


@pytest.mark.asyncio
async def test_new_tool_result_keeps_typed_fragments_and_projects_model_text(
    monkeypatch,
):
    from a11.sdk.openai import interact_with_gpt  # noqa: F401

    registry = a11.ActionRegistry()
    register(registry)
    action = registry.make_action("llm_context_project_interactions")
    action.set_header(
        LlmContextHeaders.CONFIG,
        LlmContextConfig(
            boundary="interaction", tool_results="lossless", min_gain=0
        ).model_dump_json(),
    )
    action.set_header(
        LlmContextHeaders.STAGE,
        json.dumps(
            {
                "schema": "a11.llm-context.stage/v1",
                "owner": "interaction",
                "action_id": action.id,
            }
        ),
    )
    monkeypatch.setattr("a11.sdk.llm_context.runtime.emit_report", AsyncMock())
    result = llm.Interaction(
        content=[
            a11.to_chunk(
                {
                    "messages": [
                        {
                            "role": "tool",
                            "tool_call_id": "call",
                            "content": '{  "value": 1   }',
                        }
                    ]
                }
            )
        ],
        action_outputs={
            "call": [
                a11.NodeFragment(
                    id="result",
                    data=a11.to_chunk({"value": 1}),
                    continued=False,
                )
            ]
        },
        backend_specific_metadata={"backend": b"gpt"},
    )
    before = encode_interactions([result])
    projected = await prepare_interaction_result(action, result)
    assert projected is not result
    assert projected.id == result.id
    normalized = llm.normalize_interaction(projected, strict=True)
    assert normalized.parts[0].content == '{"value":1}'
    assert normalized.parts[0].call_id == "call"
    assert encode_interactions([result]) == before
    assert (
        interaction_view_sources(
            await project_interactions([result], LlmContextConfig())
        )[0]
        .action_outputs["call"][0]
        .data.data
        == result.action_outputs["call"][0].data.data
    )
    result.backend_specific_metadata[llm.TOOL_STATUSES_METADATA_KEY] = (
        b'{"call":{"code":3}}'
    )
    assert await prepare_interaction_result(action, result) is result


@pytest.mark.asyncio
async def test_instruction_only_and_late_instructions_fail_explicitly():
    with pytest.raises(StatusException, match="model history"):
        await project_interactions(
            [
                llm.Interaction(
                    role=llm.Role.SYSTEM, content=[a11.to_chunk("rules")]
                )
            ],
            LlmContextConfig(),
        )
    with pytest.raises(StatusException, match="precede"):
        await project_interactions(
            [
                llm.Interaction(content=[a11.to_chunk("request")]),
                llm.Interaction(
                    role=llm.Role.SYSTEM, content=[a11.to_chunk("rules")]
                ),
            ],
            LlmContextConfig(),
        )


@pytest.mark.asyncio
async def test_disabled_and_shadow_views_remain_uninstalled():
    owner = Owner(sources())
    disabled = await compact_host_interactions(
        owner.read,
        owner.install,
        POLICY.model_copy(update={"enabled": False}),
    )
    assert disabled["covered_ids"] == []
    shadow = await project_interactions(
        owner.interactions,
        POLICY.model_copy(
            update={
                "measurement": (
                    LlmContextConfig(measurement={"mode": "shadow"}).measurement
                )
            }
        ),
        source_revision=7,
        checkpoint="old",
    )
    with pytest.raises(StatusException, match="non-shadow"):
        await accept_interaction_view(shadow, owner.read, owner.install)
    assert owner.calls == 0
    assert len(shadow["sources"]) == 2


@pytest.mark.asyncio
async def test_utility_installation_requires_receiving_owner_authorization(
    tmp_path,
):
    registry = a11.ActionRegistry()
    register(
        registry,
        services=LlmContextServices(
            retention=RetainedContentStore(tmp_path, owner="host"),
        ),
    )
    proposal = await project_interactions(
        sources(),
        POLICY,
        source_revision=0,
        checkpoint="old",
    )
    with pytest.raises(StatusException) as error:
        await invoke(
            registry,
            "llm_context_accept_interaction_view",
            {
                "view": proposal,
                "scope": {"owner": "host", "conversation": "one"},
            },
        )
    assert error.value.status.code == StatusCode.PERMISSION_DENIED


def test_equal_nested_headers_decode_once_and_resolve_one_immutable_policy(
    monkeypatch,
):
    decode = Mock(wraps=LlmContextConfig.model_validate_json)
    monkeypatch.setattr(LlmContextConfig, "model_validate_json", decode)
    registry = a11.ActionRegistry()
    register(registry)
    parent = registry.make_action("llm_context_project_interactions")
    parent.set_header(
        LlmContextHeaders.CONFIG,
        '{"input_content":"lossless","min_gain":71}',
    )
    resolved = config_for(parent)
    child = parent.make_nested(parent.schema)
    grandchild = child.make_nested(child.schema)
    assert config_for(child) is resolved
    assert config_for(grandchild) is resolved
    assert config_for(parent) is resolved
    assert decode.call_count == 1
    with pytest.raises(ValueError):
        resolved.min_gain = 72


@pytest.mark.asyncio
@pytest.mark.parametrize("mutation", ["chain", "identity", "instructions"])
async def test_continuation_must_match_accepted_prefix(mutation):
    _, view = await accepted(checkpoint="old")
    records = use_interaction_view(view)
    tail = llm.Interaction(
        content=[a11.to_chunk("continue")],
        previous_interaction_id=records[-1].id,
    )
    if mutation == "chain":
        tail.previous_interaction_id = "other-history"
    elif mutation == "identity":
        tail.id = records[-1].id
    else:
        tail.system_instructions = [a11.to_chunk("other instructions")]
    with pytest.raises(StatusException):
        validate_applied_interactions([*records, tail])


@pytest.mark.asyncio
async def test_unknown_metadata_and_unrepresentable_known_blocks_stay_opaque():
    from a11.sdk.openai import interact_with_gpt  # noqa: F401

    cases = [
        llm.Interaction(
            content=[a11.to_chunk("text")],
            backend_specific_metadata={
                "backend": b"gpt",
                "vendor_state": b"opaque",
            },
        ),
        llm.Interaction(
            content=[
                a11.to_chunk(
                    [
                        {"type": "text", "text": "visible"},
                        {
                            "type": "tool_use",
                            "id": "call",
                            "name": "lookup",
                            "input": {},
                        },
                    ]
                )
            ]
        ),
        llm.Interaction(
            content=[
                a11.to_chunk(
                    {
                        "role": "user",
                        "content": [
                            {"type": "text", "text": "visible"},
                            {
                                "type": "image",
                                "data": "YQ==",
                                "mime_type": "image/png",
                            },
                        ],
                    }
                )
            ],
            backend_specific_metadata={"backend": b"gpt"},
        ),
    ]
    for item in cases:
        proposal = await project_interactions([item], LlmContextConfig())
        assert proposal["entries"][0]["normalized"] is None
        assert proposal["sources"] == encode_interactions([item])


@pytest.mark.asyncio
async def test_request_view_retry_reuses_durable_commit_after_host_reopen(
    tmp_path,
    monkeypatch,
):
    summarize = Mock(return_value="identifier ALPHA-17")
    scope = {
        "owner": "host",
        "conversation": "one",
        "expected_revision": 0,
        "operation_id": "request",
    }
    inputs = sources()
    monkeypatch.setattr("a11.sdk.llm_context.runtime.emit_report", AsyncMock())
    prepared = []
    for _ in range(2):
        registry = a11.ActionRegistry()
        register(
            registry,
            services=LlmContextServices(
                retention=RetainedContentStore(tmp_path, owner="host"),
                authorize_scope=lambda action, value: True,
                summarizers={"summary": summarize},
            ),
        )
        action = registry.make_action("llm_context_project_interactions")
        action.set_header(
            LlmContextHeaders.CONFIG,
            POLICY.model_copy(
                update={
                    "retention": True,
                    "summarizer": "summary",
                }
            ).model_dump_json(),
        )
        action.set_header(LlmContextHeaders.SCOPE, json.dumps(scope))
        prepared.append(await prepare_interactions(action, inputs))
        updated = json.loads(action.get_header(LlmContextHeaders.SCOPE))
        assert updated["expected_revision"] == 1
        assert updated["operation_id"] == "request"
    assert summarize.call_count == 1
    assert encode_interactions(prepared[0]) == encode_interactions(prepared[1])


@pytest.mark.asyncio
async def test_summary_action_reports_usage_and_disables_recursive_projection(
    tmp_path,
):
    from a11.sdk.llm_context import RunRecorder, load_runs
    from a11.sdk.llm_context.config import LLM_CONTEXT_HEADERS
    from a11.sdk.usage import UsageReporter

    recorder = RunRecorder(tmp_path / "run.jsonl")
    registry = a11.ActionRegistry()
    summary_schema = a11.ActionSchema(
        name="summary",
        inputs={
            "history": a11.ActionPortSchema(
                "history", "application/json", typeinfo=list, unary=True
            )
        },
        outputs={
            "checkpoint": a11.ActionPortSchema(
                "checkpoint", "text/plain", unary=True
            ),
            "event_stream": a11.ActionPortSchema(
                "event_stream", "application/json", typeinfo=dict
            ),
        },
        headers=a11.DEFAULT_ACTION_HEADERS | LLM_CONTEXT_HEADERS,
    )

    async def summarize(action):
        assert not config_for(action).enabled
        assert not interaction_stage(action)
        assert await action["history"].consume(list)
        reporter = UsageReporter(action, "mock-summarizer", "test")
        await reporter.report(
            llm.UsageMetadata(input_tokens=8, output_tokens=2)
        )
        await action["checkpoint"].finalize("identifier ALPHA-17")

    registry.register("summary", summary_schema, summarize)
    register(registry, services=LlmContextServices(recorder=recorder))
    result = await invoke(
        registry,
        "llm_context_compact_interactions",
        {"interactions": encode_interactions(sources())},
        {
            LlmContextHeaders.CONFIG: (
                POLICY.model_copy(
                    update={"summarizer": "summary"}
                ).model_dump_json()
            )
        },
    )
    assert result["result"]["checkpoint"] == "identifier ALPHA-17"
    rows = [
        row
        for row in load_runs(recorder.path)
        if row.get("type") == "a11.usage"
    ]
    assert len(rows) == 1
    assert rows[0]["operation"] == "summarization"
    assert rows[0]["usage"]["input_tokens"] == 8


@pytest.mark.asyncio
async def test_producer_declares_diagnostic_metadata_beside_its_normalizer():
    def normalize(item):
        return llm.NormalizedMessage(
            role=item.role,
            parts=[
                llm.NormalizedPart(
                    type="text", text=a11.from_chunk(item.content[0])
                )
            ],
        )

    llm.register_interaction_normalizer(
        "test-portable-producer",
        normalize,
        replay_metadata_keys=("duration",),
    )
    item = llm.Interaction(
        content=[a11.to_chunk("known content")],
        backend_specific_metadata={
            "backend": b"test-portable-producer",
            "duration": b"10",
        },
    )
    view = await project_interactions([item], LlmContextConfig())
    assert (
        view["entries"][0]["normalized"]["parts"][0]["text"] == "known content"
    )
    assert view["sources"] == encode_interactions([item])
    item.backend_specific_metadata["signature"] = b"opaque"
    view = await project_interactions([item], LlmContextConfig())
    assert view["entries"][0]["normalized"] is None


@pytest.mark.asyncio
async def test_system_interactions_replay_as_ordered_instructions():
    history = [
        llm.Interaction(
            role=llm.Role.SYSTEM,
            content=[a11.to_chunk({"role": "system", "content": "Keep rules"})],
            backend_specific_metadata={"backend": b"gpt"},
        ),
        llm.Interaction(content=[a11.to_chunk("request")]),
    ]
    _, view = await accepted(history, LlmContextConfig())
    assert view["source_ids"] == [item.id for item in history]
    assert view["covered_ids"] == []
    for conversation in (ChatConversation(), ClaudeConversation()):
        for item in use_interaction_view(view):
            conversation.feed_next_interaction(item)
        assert conversation.system_prompt == "Keep rules"
        assert "request" in json.dumps(conversation.messages)

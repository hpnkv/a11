"""Exercise C++ contract review orchestration without model requests."""

import asyncio
import json
from pathlib import Path

import pytest

import a11
from a11 import flow, timing
from a11.sdk.interact_with_llm_schema import INTERACT_WITH_LLM_SCHEMA
from a11.sdk.llm import Interaction
from a11.status import StatusException


SOURCE = Path(__file__).parents[3] / ".a11/flows/cpp_contract_review.flow"
READ_SCHEMA = a11.ActionSchema.model_validate({
    "name": "read_file",
    "inputs": {
        "path": {"type": str, "unary": True},
        "options": {"type": dict, "unary": True},
    },
    "outputs": {"text": {"type": str, "unary": True}},
})
CODE = "absl::Status Save();\nvoid f(int* p) { (void)Save(); }\n"


async def invoke(inputs, read, model, provider="openai"):
    registry = a11.ActionRegistry()
    registry.register("read_file", READ_SCHEMA, read)
    registry.register(INTERACT_WITH_LLM_SCHEMA.name, INTERACT_WITH_LLM_SCHEMA, model)
    program = flow.load(SOURCE)
    program.register_all(registry)
    result = await program["cpp_contract_review"].invoke(
        inputs,
        registry=registry,
        headers={"x-a11-llm-provider": provider, "x-a11-llm-model": "test"},
        timeout=timing.Duration.seconds(10),
    )
    for review in result["reviews"]:
        assert type(review).__name__ == "CppContractReview"
    result["reviews"] = [review.model_dump() for review in result["reviews"]]
    return result


async def finish(action, text):
    await (await action["text_output"].put(text))
    for name in ("text_output", "new_interactions", "thoughts", "event_stream"):
        await action[name].finalize()


@pytest.mark.asyncio
@pytest.mark.parametrize("limit", [1, 2])
async def test_reviews_and_parallel_limit(limit):
    active = 0
    peak = 0
    paths = []
    finding = {
        "rule": "status-discard", "line": 2,
        "message": "(void)Save() discards absl::Status.",
        "suggestion": "Use Save().IgnoreError().",
    }

    async def read(action):
        paths.append(await action["path"].consume(str))
        assert await action["options"].consume(dict) == {"max_bytes": 120000}
        await action["text"].finalize(CODE)

    async def model(action):
        nonlocal active, peak
        interaction = await action["interactions"].consume(Interaction)
        system = a11.from_chunk(interaction.system_instructions[0])
        prompt = a11.from_chunk(interaction.content[0])["content"][0]["text"]
        record = json.loads(prompt.split(": ", 1)[1])
        assert record["source"] == CODE
        for required in ("absl_nonnull", "absl_nullable", "absl_nullability_unknown",
                         "(void)", ".IgnoreError()", "untrusted repository data",
                         "uncertainties", "every pointer level"):
            assert required in system
        assert action.get_header("x-a11-llm-model", decode=True) == "test"
        assert await action["config"].consume(dict) == {}
        assert await action["tools"].consume(dict, allow_none=True) is None
        active += 1
        peak = max(peak, active)
        await asyncio.sleep(0.03)
        active -= 1
        await finish(action, json.dumps({"findings": [finding], "uncertainties": []}))

    result = await invoke(
        {"sources": ["a.cc", "b.h", "c.cc"], "options": {"max_concurrency": limit}},
        read, model,
    )
    assert peak == limit
    assert sorted(paths) == ["a.cc", "b.h", "c.cc"]
    assert sorted(review["path"] for review in result["reviews"]) == ["a.cc", "b.h", "c.cc"]
    for review in result["reviews"]:
        assert review["findings"][0]["rule"] == "status-discard"
        assert review["findings"][0]["line"] == 2
        assert review["uncertainties"] == []
        assert review["error"] == {}


@pytest.mark.asyncio
@pytest.mark.parametrize("inputs", [
    {"sources": []},
    {"sources": ["a.cc"], "options": {"max_concurrency": 0}},
    {"sources": ["a.cc"], "options": {"max_source_bytes": 0}},
])
async def test_invalid_input_never_calls_actions(inputs):
    async def unexpected(action):
        pytest.fail("Invalid input reached an action")

    with pytest.raises(StatusException) as error:
        await invoke(inputs, unexpected, unexpected)
    assert error.value.status.code == a11.StatusCode.INVALID_ARGUMENT


@pytest.mark.asyncio
@pytest.mark.parametrize("response", [
    "", " ", "x" * 24001, "not json", "{}",
    '{"findings": [{"rule": "other"}], "uncertainties": []}',
])
async def test_invalid_model_response_returns_error(response):
    async def read(action):
        await action["text"].finalize(CODE)

    async def model(action):
        await finish(action, response)

    result = await invoke({"sources": ["a.cc"]}, read, model)
    review, = result["reviews"]
    assert review["path"] == "a.cc"
    assert review["error"]["message"]
    assert review["findings"] == []
    assert review["uncertainties"] == []


@pytest.mark.asyncio
@pytest.mark.parametrize("failure", ["read", "model", "response", "", "bad\0.cc"])
async def test_file_failure_does_not_stop_other_files(failure):
    bad_path = failure if failure in ("", "bad\0.cc") else "bad.cc"
    reads = []

    async def read(action):
        path = await action["path"].consume(str)
        reads.append(path)
        if path == bad_path and failure == "read":
            raise a11.Status(
                code=a11.StatusCode.NOT_FOUND, message="file missing",
                details=[{"path": path}],
            ).to_exception()
        await action["text"].finalize(path)

    async def model(action):
        interaction = await action["interactions"].consume(Interaction)
        prompt = a11.from_chunk(interaction.content[0])["content"][0]["text"]
        path = json.loads(prompt.split(": ", 1)[1])["path"]
        if path == bad_path and failure == "model":
            raise a11.Status(
                code=a11.StatusCode.UNAVAILABLE, message="model unavailable",
            ).to_exception()
        response = "not json" if path == bad_path else json.dumps({
            "findings": [], "uncertainties": [], "path": "forged.cc",
        })
        await finish(action, response)

    result = await invoke(
        {"sources": ["first.cc", bad_path, "last.cc"],
         "options": {"max_concurrency": 2}}, read, model,
    )
    reviews = {review["path"]: review for review in result["reviews"]}
    assert len(reviews) == 3
    assert reviews["first.cc"]["error"] == {}
    assert reviews["last.cc"]["error"] == {}
    assert reviews[bad_path]["error"]["message"]
    assert reviews[bad_path]["findings"] == []
    assert reviews[bad_path]["uncertainties"] == []
    if failure == "read":
        assert reviews[bad_path]["error"]["code"] == "NOT_FOUND"
        assert reviews[bad_path]["error"]["details"] == [{"path": bad_path}]
    if failure in ("", "bad\0.cc"):
        assert bad_path not in reads


@pytest.mark.asyncio
async def test_reviews_are_emitted_out_of_order():
    fast_started = asyncio.Event()

    async def read(action):
        await action["text"].finalize(await action["path"].consume(str))

    async def model(action):
        interaction = await action["interactions"].consume(Interaction)
        prompt = a11.from_chunk(interaction.content[0])["content"][0]["text"]
        path = json.loads(prompt.split(": ", 1)[1])["path"]
        if path == "slow.cc":
            await asyncio.wait_for(fast_started.wait(), timeout=3)
            await asyncio.sleep(0.05)
        else:
            fast_started.set()
        await finish(action, '{"findings": [], "uncertainties": []}')

    result = await invoke(
        {"sources": ["slow.cc", "fast.cc"], "options": {"max_concurrency": 2}},
        read, model,
    )
    assert [review["path"] for review in result["reviews"]] == ["fast.cc", "slow.cc"]


@pytest.mark.asyncio
@pytest.mark.parametrize("provider", ["claude_code", "codex"])
async def test_agent_backend_rejected(provider):
    async def unexpected(action):
        pytest.fail("Coding-agent backend reached an action")

    with pytest.raises(StatusException):
        await invoke({"sources": ["a.cc"]}, unexpected, unexpected, provider)

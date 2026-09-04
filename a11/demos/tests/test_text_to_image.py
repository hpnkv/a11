# Copyright 2026 The A11 Authors.

"""Verify the text-to-image action's port contract without model weights.

The action's subject is the port contract the generative-media guide describes:
a step counter on `progress` while the model runs, one PNG on `image` at the
end, and both ports closed however the call goes. A stub pipeline invokes the
step callback with the `diffusers` signature.
"""

import asyncio
import sys
import types

import pytest

import a11
from a11.demos import text_to_image as t2i
from a11.status import StatusCode, StatusException


class _FakeImage:
    """An image stub exposing the `save` method used by `_png_bytes`."""

    def save(self, buffer, format: str) -> None:
        assert format == "PNG"
        buffer.write(b"\x89PNG\r\n\x1a\n" + b"pixels")


class _FakeResult:
    def __init__(self):
        self.images = [_FakeImage()]


class _FakePipeline:
    """A pipeline shaped like `StableDiffusionPipeline.__call__`.

    Calls `callback_on_step_end` once per step with the `diffusers` arguments.
    The callback exercises the handler's thread-to-loop handoff.
    """

    device = "cpu"

    def __init__(self):
        self.calls: list[dict] = []

    def __call__(
        self,
        prompt,
        *,
        num_inference_steps,
        height,
        width,
        generator,
        callback_on_step_end,
    ):
        self.calls.append(
            {
                "prompt": prompt,
                "num_inference_steps": num_inference_steps,
                "height": height,
                "width": width,
            }
        )
        for step in range(num_inference_steps):
            assert callback_on_step_end(self, step, 0, {}) == {}
        return _FakeResult()


@pytest.fixture
def stub_torch(monkeypatch):
    """A `torch` module with the one attribute the handler reaches for.

    The handler builds a `torch.Generator` to carry the seed. This stub keeps
    the port tests independent of `torch`.
    """

    class _Generator:
        def __init__(self, device):
            self.device = device
            self.seed = None

        def manual_seed(self, seed):
            self.seed = seed
            return self

    module = types.ModuleType("torch")
    module.Generator = _Generator
    monkeypatch.setitem(sys.modules, "torch", module)
    return module


@pytest.fixture
def pipeline(monkeypatch):
    """The action's loaded pipeline, replaced by the stub."""
    fake = _FakePipeline()
    monkeypatch.setattr(t2i, "_PIPELINE", fake)
    return fake


async def _run(request: dict, *, read_image: bool = True):
    """Drive the action, draining both output ports the way a client does."""
    action = (
        a11.Action(t2i.TEXT_TO_IMAGE_SCHEMA)
        .bind_handler(t2i.text_to_image)
        .run()
    )

    progress: list = []

    async def pump():
        async for value in action["progress"]:
            progress.append(value)

    reader = asyncio.create_task(pump())
    await action["request"].finalize(request)

    image = None
    if read_image:
        async for chunk in action["image"].iter_chunks():
            if not chunk.is_null():
                image = chunk
    await reader
    await action.wait()
    return progress, image


def test_request_port_declares_the_diffusion_model():
    assert (
        t2i.TEXT_TO_IMAGE_SCHEMA.inputs["request"].typeinfo
        is t2i.DiffusionRequest
    )


@pytest.mark.asyncio
async def test_progress_arrives_per_step_and_the_image_at_the_end(
    stub_torch, pipeline
):
    """Progress precedes the final image on its separate port."""
    progress, image = await _run(
        {
            "prompt": "a lighthouse",
            "num_inference_steps": 4,
        }
    )

    assert [value["step"] for value in progress] == [1, 2, 3, 4]
    assert {value["steps"] for value in progress} == {4}
    assert image is not None
    assert image.get_mimetype() == "image/png"
    assert bytes(image.data).startswith(b"\x89PNG")
    assert pipeline.calls == [
        {
            "prompt": "a lighthouse",
            "num_inference_steps": 4,
            "height": 512,
            "width": 512,
        }
    ]


@pytest.mark.asyncio
async def test_the_request_defaults_leave_only_the_prompt_required(
    stub_torch, pipeline
):
    progress, image = await _run({"prompt": "a storm"})

    assert len(progress) == 20
    assert image is not None
    assert pipeline.calls[0]["num_inference_steps"] == 20


@pytest.mark.asyncio
async def test_a_seed_reaches_the_generator(stub_torch, pipeline, monkeypatch):
    """The request seed reaches `torch.Generator`."""
    seen: list = []

    class _Recording(_FakePipeline):
        def __call__(self, *args, **kwargs):
            seen.append(kwargs["generator"].seed)
            return super().__call__(*args, **kwargs)

    monkeypatch.setattr(t2i, "_PIPELINE", _Recording())
    await _run({"prompt": "a comet", "num_inference_steps": 2, "seed": 7})

    assert seen == [7]


@pytest.mark.asyncio
async def test_a_request_the_model_would_refuse_is_refused_here(
    stub_torch, pipeline
):
    """Invalid input is rejected before pipeline invocation."""
    with pytest.raises(StatusException) as refused:
        await _run({"prompt": "too many steps", "num_inference_steps": 1000})

    assert refused.value.status.code != StatusCode.OK
    assert pipeline.calls == []


@pytest.mark.asyncio
async def test_both_ports_close_when_the_pipeline_fails(
    stub_torch, monkeypatch
):
    """Both output readers finish after a pipeline failure."""

    class _Failing:
        device = "cpu"

        def __call__(self, *args, **kwargs):
            raise RuntimeError("the device fell over")

    monkeypatch.setattr(t2i, "_PIPELINE", _Failing())

    action = (
        a11.Action(t2i.TEXT_TO_IMAGE_SCHEMA)
        .bind_handler(t2i.text_to_image)
        .run()
    )
    await action["request"].finalize({"prompt": "anything"})

    # The ports close before the exception reaches the runtime. The action
    # carries the failure status.
    for port in ("progress", "image"):
        values = [
            chunk
            async for chunk in action[port].iter_chunks()
            if not chunk.is_null()
        ]
        assert values == []

    with pytest.raises(StatusException) as failed:
        await action.wait()
    assert "device fell over" in failed.value.status.message


def test_the_dependency_failure_names_the_extra():
    """The message a page shows when the server has no diffusion stack."""
    with pytest.raises(StatusException) as missing:
        raise t2i._unavailable("text_to_image needs `diffusers`")

    assert missing.value.status.code == StatusCode.FAILED_PRECONDITION

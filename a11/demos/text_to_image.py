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

"""Draw an image while streaming progress on a separate action port.

The image is one final payload, while progress is a stream on a separate port.
A client can render step updates before the image is ready and receive the
image without polling.

Needs `diffusers`, `transformers`, `torch` and a Stable Diffusion checkpoint on
the machine running it -- `pip install 'a11-kit[diffusion]'` covers the three
packages, and the checkpoint is downloaded on first use. Without them the action
fails with `FAILED_PRECONDITION` and says what is missing, which is what the
guide's page displays.
"""

from __future__ import annotations

import asyncio
import io
import secrets
from typing import Any

from absl import logging
from pydantic import BaseModel, Field

import a11
from a11.status import Status, StatusCode

#: Checkpoints to try, in order. The first is what `diffusers` downloads for
#: anyone with no local copy; the others are where a machine that has one tends
#: to keep it.
MODEL_CANDIDATES = (
    "stable-diffusion-v1-5/stable-diffusion-v1-5",
    "runwayml/stable-diffusion-v1-5",
)


class DiffusionRequest(BaseModel):
    """What to draw, and how hard to try."""

    prompt: str = Field(description="What the image should show.")
    num_inference_steps: int = Field(
        default=20, description="How many denoising steps, from 10 to 50."
    )
    height: int = Field(
        default=512,
        description="Image height from 512 to 1024, divisible by 8.",
    )
    width: int = Field(
        default=512,
        description="Image width from 512 to 1024, divisible by 8.",
    )
    seed: int | None = Field(
        default=None, description="Fix the seed to get the same image twice."
    )


TEXT_TO_IMAGE_SCHEMA = a11.ActionSchema(
    name="text_to_image",
    description="Draw an image from a prompt, reporting progress as it goes.",
    inputs={
        "request": a11.ActionPortSchema(
            name="request",
            type="application/json",
            typeinfo=DiffusionRequest,
            unary=True,
            required=True,
            description=(
                "`{prompt, num_inference_steps, height, width, seed}`;"
                " everything but `prompt` has a default."
            ),
        )
    },
    outputs={
        "image": a11.ActionPortSchema(
            name="image",
            type="image/png",
            unary=True,
            required=True,
            description="The finished image, as PNG bytes.",
        ),
        "progress": a11.ActionPortSchema(
            name="progress",
            type="application/json",
            typeinfo=dict,
            required=False,
            description="`{step, steps}` once per denoising step.",
        ),
    },
)

#: The pipeline is loaded once and reused: it is seconds of work and gigabytes
#: of weights, and a demo server answers more than one request.
_PIPELINE: Any = None
#: One image at a time. A second concurrent call would contend for the same
#: device and finish later than if it had waited.
_PIPELINE_LOCK = asyncio.Lock()


def _unavailable(message: str) -> Exception:
    return Status(
        code=StatusCode.FAILED_PRECONDITION, message=message
    ).to_exception()


def _device() -> str:
    import torch

    if torch.cuda.is_available():
        return "cuda"
    if torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def _load_pipeline() -> Any:
    """The Stable Diffusion pipeline, loaded on first use.

    Runs on a worker thread: importing `torch` and reading a checkpoint both
    block for a long time, and the event loop has a session to pump.
    """
    try:
        import torch
        from diffusers import StableDiffusionPipeline
    except ImportError as exc:
        raise _unavailable(
            "text_to_image needs `diffusers`, `transformers` and `torch`"
            " installed on the server (pip install 'a11-kit[diffusion]'):"
            f" {exc}"
        ) from exc

    device = _device()
    dtype = torch.float32 if device == "cpu" else torch.float16
    failures: list[str] = []
    for candidate in MODEL_CANDIDATES:
        try:
            pipeline = StableDiffusionPipeline.from_pretrained(
                candidate,
                dtype=dtype,
                # This action omits the safety checker and its feature
                # extractor.
                # The default extractor also depends on `torchvision`.
                safety_checker=None,
                feature_extractor=None,
                requires_safety_checker=False,
            )
        except Exception as exc:  # noqa: BLE001 - try the next candidate
            failures.append(f"{candidate}: {exc}")
            continue
        pipeline.to(device)
        logging.info("text_to_image: loaded %s on %s", candidate, device)
        return pipeline

    raise _unavailable(
        "text_to_image found no usable Stable Diffusion checkpoint. Tried:"
        f" {'; '.join(failures)}"
    )


async def _pipeline() -> Any:
    global _PIPELINE
    if _PIPELINE is None:
        _PIPELINE = await asyncio.to_thread(_load_pipeline)
    return _PIPELINE


def _png_bytes(image: Any) -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, format="PNG")
    return buffer.getvalue()


def _png_chunk(png: bytes) -> a11.Chunk:
    """The image as a chunk labelled `image/png`.

    `put` encodes values through the serialization registry, which has no codec
    for bytes with the `image/png` media type. `put_chunk` carries the encoded
    PNG bytes directly.
    """
    return a11.Chunk(data=png, metadata=a11.ChunkMetadata(mimetype="image/png"))


def _validate_request(request: DiffusionRequest) -> None:
    if not 10 <= request.num_inference_steps <= 50:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "`num_inference_steps` must be between 10 and 50 inclusive;"
                f" received {request.num_inference_steps}."
            ),
        ).to_exception()

    for name in ("height", "width"):
        value = getattr(request, name)
        if not 512 <= value <= 1024:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    f"`{name}` must be between 512 and 1024 inclusive;"
                    f" received {value}."
                ),
            ).to_exception()
        if value % 8 != 0:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"`{name}` must be divisible by 8; received {value}.",
            ).to_exception()


async def text_to_image(action: a11.Action) -> None:
    """Draw `request.prompt`, streaming a step counter while it works."""

    progress = action["progress"]
    image_out = action["image"]
    try:
        request = await action["request"].consume(DiffusionRequest)
        _validate_request(request)
        await action.logf(
            "Drawing %r in %d steps at %dx%d.",
            request.prompt,
            request.num_inference_steps,
            request.width,
            request.height,
            channel="request",
        )

        await action.log(
            "Loading the diffusion pipeline."
            if _PIPELINE is None
            else "Using the loaded diffusion pipeline.",
            channel="model",
        )
        pipeline = await _pipeline()

        # The diffusers callback runs on the worker thread, so it hands the
        # fragment to the loop rather than awaiting a put itself.
        loop = asyncio.get_running_loop()

        def on_step(_pipeline, step: int, _timestep: int, kwargs: dict) -> dict:
            # Some schedulers emit one callback beyond the requested step count.
            # The progress value remains within the declared total.
            done = min(step + 1, request.num_inference_steps)
            asyncio.run_coroutine_threadsafe(
                progress.put(
                    {"step": done, "steps": request.num_inference_steps},
                    mimetype="application/json",
                ),
                loop,
            )
            return kwargs

        import torch

        random_seed = request.seed is None
        seed = (
            request.seed
            if request.seed is not None
            else secrets.randbits(63)
        )
        await action.logf(
            "Using %s seed %d on %s.",
            "random" if random_seed else "requested",
            seed,
            pipeline.device,
            channel="model",
        )
        generator = torch.Generator(pipeline.device).manual_seed(seed)

        async with _PIPELINE_LOCK:
            result = await asyncio.to_thread(
                pipeline,
                request.prompt,
                num_inference_steps=request.num_inference_steps,
                height=request.height,
                width=request.width,
                generator=generator,
                callback_on_step_end=on_step,
            )

        png = await asyncio.to_thread(_png_bytes, result.images[0])
        await image_out.put_chunk(_png_chunk(png), final=True)
        await action.logf(
            "Finished a %d-byte PNG.", len(png), channel="result"
        )
    finally:
        # A caller reading either port must see it end, however this went.
        # Closed rather than finalized: `progress` has no last event worth
        # marking, and `image_out` marked its own above -- or failed before it
        # had anything to mark.
        await progress.close()
        await image_out.close()

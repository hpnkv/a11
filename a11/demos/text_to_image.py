"""Text to image, as an action with a port per thing the caller cares about.

The image is one final payload, while progress is a stream on a separate port.
A client can render step updates before the image is ready and receive the
image without polling.

Needs `diffusers`, `torch` and a Stable Diffusion checkpoint on the machine
running it. Without them the action fails with `FAILED_PRECONDITION` and says
what is missing, which is what the guide's page displays.
"""

from __future__ import annotations

import asyncio
import io
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
        default=20, ge=1, le=100, description="How many denoising steps."
    )
    height: int = Field(default=512, ge=64, le=1024)
    width: int = Field(default=512, ge=64, le=1024)
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
            typeinfo=dict,
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
            "text_to_image needs `diffusers` and `torch` installed on the"
            f" server: {exc}"
        ) from exc

    device = _device()
    dtype = torch.float32 if device == "cpu" else torch.float16
    failures: list[str] = []
    for candidate in MODEL_CANDIDATES:
        try:
            pipeline = StableDiffusionPipeline.from_pretrained(
                candidate,
                torch_dtype=dtype,
                safety_checker=None,
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


async def text_to_image(action: a11.Action) -> None:
    """Draw `request.prompt`, streaming a step counter while it works."""

    progress = action["progress"]
    image_out = action["image"]
    try:
        request = DiffusionRequest.model_validate(
            await action["request"].consume(dict)
        )
        logging.info(
            "text_to_image %s: %r (%d steps, %dx%d)",
            action.get_id(),
            request.prompt,
            request.num_inference_steps,
            request.width,
            request.height,
        )

        pipeline = await _pipeline()

        # The diffusers callback runs on the worker thread, so it hands the
        # fragment to the loop rather than awaiting a put itself.
        loop = asyncio.get_running_loop()

        def on_step(_pipeline, step: int, _timestep: int, kwargs: dict) -> dict:
            asyncio.run_coroutine_threadsafe(
                progress.put(
                    {"step": step + 1, "steps": request.num_inference_steps},
                    mimetype="application/json",
                ),
                loop,
            )
            return kwargs

        import torch

        generator = torch.Generator(pipeline.device)
        if request.seed is not None:
            generator = generator.manual_seed(request.seed)

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
        await image_out.put(png, mimetype="image/png", final=True)
        logging.info(
            "text_to_image %s: %d bytes of PNG", action.get_id(), len(png)
        )
    finally:
        # A caller reading either port must see it end, however this went.
        # Closed rather than finalized: `progress` has no last event worth
        # marking, and `image_out` marked its own above -- or failed before it
        # had anything to mark.
        await progress.close()
        await image_out.close()

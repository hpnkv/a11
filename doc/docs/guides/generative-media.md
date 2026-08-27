# Stream progress and a finished image separately

Image generation produces two results with different lifecycles: progress while
the model runs and one image when it finishes. Giving each result its own output
port lets a caller drain them concurrently, apply different size limits, and
display either one without decoding a mixed event stream.

This is a general generative-model API pattern, not an agent-specific one. A
video service can separate preview frames, logs, and the completed asset; a
speech synthesizer can separate audio frames from alignment records; an image
editor can return masks and rendered output under distinct media types. Each
consumer subscribes to the result it understands.

!!! note "Before you start"

    This guide's action needs `diffusers`, `torch` and a Stable Diffusion
    checkpoint on the machine running the backend — the first run downloads
    several gigabytes, and a CPU-only machine takes minutes per image:

    ```sh
    pip install diffusers torch
    python -m a11.demos.web_demos_server   # ws://127.0.0.1:9010/a11-demos
    ```

    A page loaded over HTTPS may refuse a plaintext `ws://` socket even to
    localhost (Chrome allows it, Firefox does not), so give a local backend
    the `--certificate` / `--private-key` flags and a trusted certificate —
    [mkcert](https://github.com/FiloSottile/mkcert) makes one — if the
    browser blocks it.

    Without them the action fails with `FAILED_PRECONDITION` and says what is
    missing, which is what the demo below shows in its error region. The other
    three guides' demos work against the hosted backend this page's field
    defaults to; this one wants a backend with a checkpoint.

## Try it

Point it at a backend that has a checkpoint. The bar moves once per denoising
step, from the `progress` port; the image appears when `image` closes.

<link rel="stylesheet" href="../assets/web-demos.css">
<div id="media-demo" class="a11-demo">
  <div class="a11-toolbar">
    <input id="media-server" class="wide" aria-label="Demo server URL" value="wss://a11.services:9443/a11-demos">
    <input id="media-steps" type="number" min="1" max="100" aria-label="Steps" value="20">
    <input id="media-seed" aria-label="Seed" placeholder="seed (optional)">
  </div>
  <div id="media-errors" class="a11-errors" role="alert" aria-live="polite"></div>
  <form id="media-form" class="a11-compose">
    <input id="media-prompt" aria-label="Prompt" autocomplete="off" placeholder="a lighthouse in a storm, oil painting...">
    <button type="submit">Draw</button>
  </form>
  <div class="a11-media">
    <progress id="media-progress" max="20" value="0"></progress>
    <div id="media-status"></div>
    <img id="media-image" alt="The generated image" hidden>
  </div>
</div>
<script type="module" src="../assets/generative-media.js"></script>

The action is
[`a11/demos/text_to_image.py`](https://github.com/hpnkv/a11/blob/main/a11/demos/text_to_image.py)
and the page is
[`js/demo/generative_media.ts`](https://github.com/hpnkv/a11/blob/main/js/demo/generative_media.ts).
[HTTP as separate streams](../api/http-actions.md) applies the same design to
HTTP protocol fields.

## 1. The contract

```python
TEXT_TO_IMAGE_SCHEMA = a11.ActionSchema(
    name="text_to_image",
    description="Draw an image from a prompt, reporting progress as it goes.",
    inputs={
        "request": a11.ActionPortSchema(
            name="request", type="application/json", typeinfo=dict,
            unary=True, required=True,
            description="`{prompt, num_inference_steps, height, width, seed}`.",
        )
    },
    outputs={
        "image": a11.ActionPortSchema(
            name="image", type="image/png", unary=True, required=True),
        "progress": a11.ActionPortSchema(
            name="progress", type="application/json", typeinfo=dict,
            description="`{step, steps}` once per denoising step."),
    },
)
```

`unary=True` declares a port that carries one complete value. Without it,
`progress` is a stream. The browser sends the request as plain JSON, which needs
no shared type registry. The handler validates it into a Pydantic model on
arrival:

```python
request = DiffusionRequest.model_validate(await action["request"].consume(dict))
```

## 2. Reporting from a worker thread

The pipeline is blocking, so it runs on a thread — and the callback that fires per
step is on that thread, not the loop:

```python
loop = asyncio.get_running_loop()

def on_step(_pipeline, step, _timestep, kwargs):
    asyncio.run_coroutine_threadsafe(
        progress.put({"step": step + 1, "steps": request.num_inference_steps},
                     mimetype="application/json"),
        loop,
    )
    return kwargs

result = await asyncio.to_thread(pipeline, request.prompt, callback_on_step_end=on_step, ...)
```

The handler does not await each progress tick's confirmation future, so a
denoising step does not wait for storage. Await both stages for payloads that
must be confirmed — `await (await node.put(value))` — as
`a11.gateway.conversations.ConversationStore.record` does.

Both ports are closed however the handler ends:

```python
finally:
    # `progress` has no final event. `image_out` finalizes the PNG itself or
    # fails before producing one.
    await progress.close()
    await image_out.close()
```

Close every output port so readers can observe the end of the stream.

## 3. The image is bytes

The handler encodes the image in its chosen format and labels the chunk with
the corresponding media type.

```python
png = await asyncio.to_thread(_png_bytes, result.images[0])
await image_out.put(png, mimetype="image/png", final=True)
```

The browser reads the PNG as a *chunk* because it has no registered application
type:

```ts
const node = need(await call.getOutput('image', false));
const chunk = need(await node.nextChunk(900_000));
image.src = URL.createObjectURL(new Blob([chunk.data], {type: chunk.mimetype}));
```

## 4. Read both ports at once

```ts
const progress = readPort(call, 'progress', (value) => {
    bar.value = (value as Progress).step;
});
const chunk = need(await node.nextChunk(900_000));
await progress;
```

An undrained output port stalls its producer. Read `progress` while waiting for
the image.

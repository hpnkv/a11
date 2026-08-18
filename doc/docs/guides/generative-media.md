# A port per thing the caller cares about

Image generation is the shortest example of why an action has ports rather than a
return value. One request produces two entirely different things: a step counter
that matters *while* the work runs, and a payload that arrives at the end. Behind
one request/response you have to choose which one to serve; with a port each, you
serve both.

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
    <input id="media-prompt" aria-label="Prompt" autocomplete="off" placeholder="a lighthouse in a storm, oil painting…">
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
[HTTP as separate streams](../api/http-actions.md) applies the same
port-per-concern idea to a protocol rather than to a model.

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

`unary=True` says a port carries one whole value; `progress` says nothing, so it
is a stream. The request is plain JSON rather than a tagged type on purpose: the
caller is a browser, and `application/json` is a complete description that needs
no shared type table. The handler validates it into a pydantic model on arrival:

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

The confirmation future each `put` returns is dropped here, which is the right
trade for a progress tick: the value is worth sending, not worth blocking a
denoising step for. A payload you must not lose is awaited twice —
`await (await node.put(value))` — the way
`a11.gateway.conversations.ConversationStore.record` does.

Both ports are closed however the handler ends:

```python
finally:
    await progress.drain_and_close()
    await image_out.drain_and_close()
```

An output port nobody closes leaves its reader waiting for a stream that has
already stopped.

## 3. The image is bytes

A11 does not encode images for you, and it should not: the handler knows what
format it wants.

```python
png = await asyncio.to_thread(_png_bytes, result.images[0])
await image_out.put(png, mimetype="image/png", final=True)
```

In the browser it is read as a *chunk* rather than as a value — there is nothing
to deserialize a PNG into:

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

Not out of tidiness: an output port nobody drains stalls the action producing it,
so a page that waited for the image before reading `progress` would eventually
wedge the very work it is waiting for.


# Stream progress and a finished image separately

Image generation produces two results with different lifecycles: progress while
the model runs and one image when it finishes. Giving each result its own output
port lets a caller drain them concurrently, apply different size limits, and
display either one without decoding a mixed event stream.

The pattern applies across generative-model APIs. A video service can separate
preview frames, logs, and the completed asset; a
speech synthesizer can separate audio frames from alignment records; an image
editor can return masks and rendered output under distinct media types. Each
consumer subscribes to the result it understands.

!!! note "Before you start"

    The demo below draws against a hosted Stable Diffusion 1.5 backend. The
    hosted demo needs no local packages.

    To run the action yourself, the backend machine needs `diffusers`,
    `transformers`, `torch` and a checkpoint. The first run downloads the
    checkpoint:

    ```sh
    pip install 'a11-kit[diffusion]'
    python -m a11.demos.web_demos_server
    ```

    A page loaded over HTTPS may refuse a plaintext `ws://` socket even to
    localhost (Chrome allows it, Firefox does not), so give a local backend
    the `--certificate` / `--private-key` flags and a trusted certificate —
    [mkcert](https://github.com/FiloSottile/mkcert) makes one — if the
    browser blocks it.

    A backend without the diffusion stack fails the action with
    `FAILED_PRECONDITION`; the demo shows its message in the error region.

## Try it

The bar moves once per denoising step, from the `progress` port; the image
appears when `image` closes.

<link rel="stylesheet" href="../assets/web-demos.css">
<div id="media-demo" class="a11-demo">
  <div class="a11-toolbar">
    <input id="media-server" class="wide" aria-label="Demo server URL"
           value="wss://a11.to/ws/demoserver">
    <span class="a11-field">
      <label for="media-steps">Steps</label>
      <input id="media-steps" type="number" min="1" max="100" value="20">
    </span>
    <span class="a11-field">
      <label for="media-seed">Seed</label>
      <input id="media-seed" placeholder="optional">
    </span>
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

## 1. Define the contract

```python
TEXT_TO_IMAGE_SCHEMA = a11.ActionSchema(
    name="text_to_image",
    description="Draw an image from a prompt, reporting progress as it goes.",
    inputs={
        "request": a11.ActionPortSchema(
            name="request", type="application/json", typeinfo=DiffusionRequest,
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

`unary=True` declares a port that carries one complete value. The `progress`
port omits it and carries a stream. The declared `DiffusionRequest` type adds
its JSON Schema to the action contract and decodes the browser's plain JSON
into the model:

```python
request = await action["request"].consume(DiffusionRequest)
```

## 2. Report from a worker thread

The pipeline is blocking, so it runs on a thread. Some schedulers invoke the
callback once beyond the requested step count, so the progress value is capped
at the declared total:

```python
loop = asyncio.get_running_loop()

def on_step(_pipeline, step, _timestep, kwargs):
    done = min(step + 1, request.num_inference_steps)
    asyncio.run_coroutine_threadsafe(
        progress.put({"step": done, "steps": request.num_inference_steps},
                     mimetype="application/json"),
        loop,
    )
    return kwargs

result = await asyncio.to_thread(
    pipeline,
    request.prompt,
    num_inference_steps=request.num_inference_steps,
    height=request.height,
    width=request.width,
    callback_on_step_end=on_step,
)
```

The handler does not await each progress tick's confirmation future, so a
denoising step does not wait for storage. Await both stages for payloads that
must be confirmed — `await (await node.put(value))` — as
`a11.gateway.conversations.ConversationStore.record` does.

Both ports are closed however the handler ends:

```python
try:
    ...
finally:
    await progress.close()
    await image_out.close()
```

Close every output port so readers can observe the end of the stream.

## 3. Write the encoded image

The handler encodes the image in its chosen format and labels the chunk with
the corresponding media type. The chunk is built and written directly:

```python
def _png_chunk(png: bytes) -> a11.Chunk:
    return a11.Chunk(
        data=png, metadata=a11.ChunkMetadata(mimetype="image/png")
    )

png = await asyncio.to_thread(_png_bytes, result.images[0])
await image_out.put_chunk(_png_chunk(png), final=True)
```

`put` encodes a value through the serialization registry, which holds a codec
per (type, media type) pair and has none for bytes as `image/png` — so `put`
answers `NOT_FOUND`. A payload that is already bytes in its final format goes on
the port as a chunk, the same way `a11.sdk.http.client` writes a request body.

The browser reads the PNG as a *chunk* because it has no registered application
type:

```ts
const node = need(await call.getOutput('image', false));
const chunk = need(await node.nextChunk(MAX_IMAGE_BYTES));
image.src = URL.createObjectURL(new Blob([chunk.data], {type: chunk.mimetype}));
```

## 4. Read both ports at once

```ts
const progress = readPort(call, 'progress', (value) => {
    bar.value = (value as Progress).step;
});
const chunk = need(await node.nextChunk(MAX_IMAGE_BYTES));
await progress;
```

`nextChunk` takes the largest payload the reader will accept. The page sets a
4 MB ceiling for this action's PNG output.

An undrained output port stalls its producer. Read `progress` while waiting for
the image.

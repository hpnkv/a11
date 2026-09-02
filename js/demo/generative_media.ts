/**
 * "Streaming generative media" guide demo.
 *
 * One action, three ports: the request goes in, the step counter comes back on
 * `progress` while the work runs, and the
 * PNG arrives on `image` at the end. The
 * page renders a progress bar from one port and the final image from another.
 *
 * The hosted demo backend draws these on a GPU. A backend without the
 * diffusion stack fails the action with `FAILED_PRECONDITION`, and this page
 * shows the failure message.
 */

import {ActionPortSchema, ActionSchema} from '../src/index.js';

import {
  DEFAULT_SERVER_URL,
  addLine,
  connect,
  makeCall,
  need,
  probeConnection,
  readPort,
  reportExampleSuccess,
  showError,
  whileBusy,
} from './demo_support.js';

/** Mirrors `a11/demos/text_to_image.py`. */
const TEXT_TO_IMAGE_SCHEMA = new ActionSchema({
  name: 'text_to_image',
  description: 'Draw an image from a prompt, reporting progress as it goes.',
  inputs: {
    request: new ActionPortSchema({name: 'request', type: 'application/json', unary: true, required: true}),
  },
  outputs: {
    image: new ActionPortSchema({name: 'image', type: 'image/png', unary: true, required: true}),
    progress: new ActionPortSchema({name: 'progress', type: 'application/json'}),
  },
});

interface Progress {
  step: number;
  steps: number;
}

/** The largest PNG this page will accept from the `image` port. */
const MAX_IMAGE_BYTES = 4_000_000;

class GenerativeMediaDemo {
  private readonly server = document.querySelector<HTMLInputElement>('#media-server')!;
  private readonly steps = document.querySelector<HTMLInputElement>('#media-steps')!;
  private readonly seed = document.querySelector<HTMLInputElement>('#media-seed')!;
  private readonly errors = document.querySelector<HTMLDivElement>('#media-errors')!;
  private readonly bar = document.querySelector<HTMLProgressElement>('#media-progress')!;
  private readonly status = document.querySelector<HTMLDivElement>('#media-status')!;
  private readonly image = document.querySelector<HTMLImageElement>('#media-image')!;
  private objectUrl: string | null = null;

  async draw(prompt: string): Promise<void> {
    this.errors.textContent = '';
    this.status.replaceChildren();
    const steps = Number(this.steps.value) || 20;
    this.bar.max = steps;
    this.bar.value = 0;

    try {
      const connection = await connect(this.server.value.trim() || DEFAULT_SERVER_URL);
      const call = makeCall(connection, TEXT_TO_IMAGE_SCHEMA);
      need(await call.call());

      const request = need(await call.getInput('request'));
      const seed = this.seed.value.trim();
      need(
        await request.finalize({
          prompt,
          num_inference_steps: steps,
          ...(seed ? {seed: Number(seed)} : {}),
        }),
      );

      // The counter is read alongside the image, not before it: an undrained
      // port stalls the action producing it, and this one runs the whole time
      // the other is silent.
      const progress = readPort(call, 'progress', (value) => {
        const step = value as Progress;
        this.bar.value = step.step;
        this.status.textContent = `step ${step.step} of ${step.steps}`;
      });

      // The PNG is already encoded, so the page reads a size-bounded chunk.
      const node = need(await call.getOutput('image', false));
      const chunk = need(await node.nextChunk(MAX_IMAGE_BYTES));
      await progress;
      need(await call.wait(60_000));

      if (chunk === null) {
        addLine(this.status, 'The backend produced no image.');
        return;
      }
      if (this.objectUrl !== null) URL.revokeObjectURL(this.objectUrl);
      this.objectUrl = URL.createObjectURL(
        new Blob([chunk.data as BlobPart], {type: chunk.mimetype || 'image/png'}),
      );
      this.image.src = this.objectUrl;
      this.image.hidden = false;
      this.status.textContent = `${chunk.data.byteLength} bytes of ${chunk.mimetype}`;
      reportExampleSuccess('generative-media');
      connection.session.halfClose();
    } catch (error) {
      showError(this.errors, error);
    }
  }
}

const root = document.querySelector('#media-demo');
if (root) {
  const demo = new GenerativeMediaDemo();
  const form = document.querySelector<HTMLFormElement>('#media-form')!;
  const input = document.querySelector<HTMLInputElement>('#media-prompt')!;
  form.onsubmit = (event) => {
    event.preventDefault();
    const prompt = input.value.trim();
    if (!prompt) return;
    void whileBusy(form, () => demo.draw(prompt));
  };
  // Early connection check so the page tells the user right away.
  const serverInput = document.querySelector<HTMLInputElement>('#media-server')!;
  const errors = document.querySelector<HTMLDivElement>('#media-errors')!;
  void probeConnection(serverInput.value.trim() || DEFAULT_SERVER_URL, errors);
}

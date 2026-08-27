/**
 * "Streaming generative media" guide demo.
 *
 * One action, three ports: the request goes in, the step counter comes back on
 * `progress` while the work runs, and the
 * PNG arrives on `image` at the end. The
 * page renders a progress bar from one port and the final image from another.
 *
 * Needs a backend with a Stable Diffusion checkpoint (see the guide). Without
 * one the action fails with `FAILED_PRECONDITION` and this page shows what it
 * said.
 */

import {ActionPortSchema, ActionSchema} from '../src/index.js';

import {
  DEFAULT_SERVER_URL,
  addLine,
  connect,
  makeCall,
  need,
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

      // The image is bytes, so it is read as a chunk rather than as a value:
      // what arrives is a PNG, and there is nothing to deserialize it into.
      const node = need(await call.getOutput('image', false));
      const chunk = need(await node.nextChunk(900_000));
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
}

/**
 * "Local models on the web" guide demo: a chat window backed entirely by
 * `interact_with_gemma` running a Gemma model in the page over WebGPU.
 *
 * The demo runs the action locally (no session or transport), feeds the whole
 * conversation into its `interactions` port, and streams the reply off the
 * `text_output` port straight into the reply bubble. It installs a cached engine
 * factory via {@link setGemmaEngineFactory} so the (large) model is downloaded
 * and compiled only once and reused across turns, and so it can surface load
 * progress.
 */

import {
  Action,
  ActionRegistry,
  INTERACT_WITH_GEMMA_SCHEMA,
  LlmHeaders,
  StatusCode,
  fetchModelAssetBuffer,
  interactWithGemma,
  isOk,
  makeTextMessageInteraction,
  parseInteraction,
  setGemmaEngineFactory,
  type GemmaConfig,
  type GemmaEngine,
  type Interaction,
  type Status,
} from '../src/index.js';

const need = <T>(value: T | Status): T => {
  if (!isOk(value)) throw new Error(`${StatusCode[value.code]}: ${value.message}`);
  return value as T;
};

// --- Cached, progress-reporting MediaPipe engine -----------------------------

let cached: { key: string; engine: GemmaEngine } | null = null;
let onEngineStatus: (message: string) => void = () => {};

setGemmaEngineFactory(async (config: GemmaConfig) => {
  const key = `${config.runtime_url}|${config.model_asset_path}`;
  if (cached !== null && cached.key === key) return cached.engine;
  if (!config.model_asset_path) {
    return { code: StatusCode.INVALID_ARGUMENT, message: 'Set a model URL first.' };
  }
  if (typeof navigator === 'undefined' || !('gpu' in navigator)) {
    return {
      code: StatusCode.UNAVAILABLE,
      message: 'This browser has no WebGPU; a Gemma model cannot run here.',
    };
  }
  try {
    onEngineStatus('Loading the MediaPipe runtime…');
    const runtimeUrl = config.runtime_url;
    const mediapipe = (await import(/* @vite-ignore */ runtimeUrl)) as {
      FilesetResolver: { forGenAiTasks(base: string): Promise<unknown> };
      LlmInference: {
        createFromOptions(fileset: unknown, options: unknown): Promise<{
          generateResponse: (
            input: string,
            progress: (partial: string, done: boolean) => void,
          ) => void;
          close?: () => void;
        }>;
      };
    };
    const fileset = await mediapipe.FilesetResolver.forGenAiTasks(config.wasm_base);
    onEngineStatus('Fetching the model (cached after the first load)…');
    // Fetch the model ourselves so HuggingFace `resolve` redirects are followed;
    // the bytes are served from Cache Storage on later loads.
    const modelBuffer = await fetchModelAssetBuffer(config.model_asset_path);
    if (!isOk(modelBuffer)) return modelBuffer;
    onEngineStatus('Compiling the model for WebGPU…');
    const llm = await mediapipe.LlmInference.createFromOptions(fileset, {
      baseOptions: { modelAssetBuffer: modelBuffer },
      maxTokens: config.max_tokens,
      ...(config.top_k != null ? { topK: config.top_k } : {}),
      ...(config.temperature != null ? { temperature: config.temperature } : {}),
      ...(config.random_seed != null ? { randomSeed: config.random_seed } : {}),
    });
    const engine: GemmaEngine = {
      generate(prompt, onToken) {
        return new Promise((resolve) => {
          try {
            let full = '';
            llm.generateResponse(prompt, (partial, done) => {
              if (partial) {
                full += partial;
                try {
                  onToken(partial);
                } catch {
                  /* UI sink failure must not abort generation. */
                }
              }
              if (done) resolve(full);
            });
          } catch (error) {
            resolve({
              code: StatusCode.INTERNAL,
              message: error instanceof Error ? error.message : String(error),
            });
          }
        });
      },
    };
    cached = { key, engine };
    onEngineStatus('Model ready.');
    return engine;
  } catch (error) {
    cached = null;
    return {
      code: StatusCode.UNAVAILABLE,
      message: `Could not load the Gemma model: ${
        error instanceof Error ? error.message : String(error)
      }`,
    };
  }
});

// --- Chat UI -----------------------------------------------------------------

class LocalChat {
  private readonly registry = new ActionRegistry();
  private history: Interaction[] = [];
  private busy = false;
  private readonly messages = document.querySelector<HTMLDivElement>('#gemma-messages')!;
  private readonly status = document.querySelector<HTMLDivElement>('#gemma-status')!;
  private readonly errors = document.querySelector<HTMLDivElement>('#gemma-errors')!;
  private readonly model = document.querySelector<HTMLInputElement>('#gemma-model')!;

  constructor() {
    need(this.registry.register('interact_with_gemma', INTERACT_WITH_GEMMA_SCHEMA, interactWithGemma));
    onEngineStatus = (message) => this.setStatus(message);
  }

  async send(text: string): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    this.errors.textContent = '';
    const bubble = this.addBubble('', 'reply');
    bubble.classList.add('gemma-pending');
    try {
      const user = need(makeTextMessageInteraction(text));
      const turn = [...this.history, user];

      const action = need(
        Action.create(INTERACT_WITH_GEMMA_SCHEMA, {
          handler: interactWithGemma,
          registry: this.registry,
        }),
      );
      need(action.setHeader(LlmHeaders.MODEL, 'gemma-in-browser'));
      need(action.run());

      // Feed the whole conversation plus the model URL config.
      const interactions = need(await action.getInput('interactions'));
      for (let index = 0; index < turn.length; index += 1) {
        need(await interactions.put(turn[index]!, { final: index === turn.length - 1 }));
      }
      const config = need(await action.getInput('config'));
      const url = this.model.value.trim();
      // An empty field falls back to the SDK's default model URL.
      need(await config.putFinal(url ? { model_asset_path: url } : {}));

      // Stream tokens into the reply bubble as they arrive.
      const output = need(await action.getOutput('text_output', false));
      // No read timeout: a cold model download can take minutes before the
      // first token, and the handler closes or aborts the node when done.
      let reply = '';
      while (true) {
        const value = need(await output.next());
        if (value === null) break;
        reply += String(value);
        bubble.textContent = reply;
        bubble.classList.remove('gemma-pending');
        this.messages.scrollTop = this.messages.scrollHeight;
      }

      // Commit the turn so the next message continues the conversation.
      const newInteractions = need(await action.getOutput('new_interactions', false));
      const assistant = need(parseInteraction(need(await newInteractions.next())));
      need(await action.wait(5_000));
      this.history = [...turn, assistant];
      if (!reply) bubble.textContent = '(no output)';
      this.setStatus('Model ready.');
    } catch (error) {
      bubble.remove();
      this.showError(error);
    } finally {
      this.busy = false;
    }
  }

  private addBubble(text: string, kind: 'request' | 'reply'): HTMLDivElement {
    const bubble = document.createElement('div');
    bubble.className = `gemma-bubble ${kind}`;
    bubble.textContent = text;
    this.messages.append(bubble);
    this.messages.scrollTop = this.messages.scrollHeight;
    return bubble;
  }

  addRequest(text: string): void {
    this.addBubble(text, 'request');
    void this.send(text);
  }

  private setStatus(message: string): void {
    this.status.textContent = message;
  }

  private showError(error: unknown): void {
    this.errors.textContent = error instanceof Error ? error.message : String(error);
    this.setStatus('');
  }
}

const root = document.querySelector('#gemma-demo');
if (root) {
  const chat = new LocalChat();
  const form = document.querySelector<HTMLFormElement>('#gemma-form')!;
  form.onsubmit = (event) => {
    event.preventDefault();
    const input = document.querySelector<HTMLInputElement>('#gemma-input')!;
    if (input.value.trim()) {
      chat.addRequest(input.value);
      input.value = '';
    }
  };
}

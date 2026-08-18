/**
 * "The model calls back into the page" guide demo.
 *
 * The page draws a few blobs on a canvas and serves three actions over them. It
 * announces those actions with `__register_tools__`, and from then on the model's
 * tool calls are dispatched back down the same WebSocket and run *here* — the
 * backend never touches the canvas, and the model sees three ordinary A11
 * actions.
 *
 * The port names are the model's argument names: a tool definition is derived
 * from the action's ports, so `set_color(ids, colors)` is what the model is
 * offered. A streaming port becomes an array.
 */

import {z} from 'zod';

import {
  ActionPortSchema,
  ActionRegistry,
  ActionSchema,
  isOk,
  okStatus,
  statusFromUnknown,
  type Action,
  type Status,
} from '../src/index.js';

import {
  BackendControls,
  DEFAULT_SERVER_URL,
  USER_FACING_LOG_PORT,
  addBubble,
  addLine,
  announceTools,
  connect,
  need,
  runTurn,
  showError,
  streamInto,
  whileBusy,
  type Connection,
} from './demo_support.js';

// --- The scene ---------------------------------------------------------------

interface Blob {
  id: number;
  x: number;
  y: number;
  radius: number;
  color: string;
}

const PALETTE = ['#4f6df5', '#f5a34f', '#4fb0f5', '#a34ff5', '#4ff5a3'];

/** The blobs, and the canvas they are drawn on. Plain 2D, no dependencies. */
class Scene {
  private readonly canvas = document.querySelector<HTMLCanvasElement>('#tools-canvas')!;
  readonly blobs: Blob[] = [];

  constructor() {
    for (let index = 0; index < 5; index += 1) {
      this.blobs.push({
        id: index,
        x: 90 + index * 110,
        y: 150,
        radius: 34,
        color: PALETTE[index]!,
      });
    }
    this.draw();
  }

  find(id: number): Blob | undefined {
    return this.blobs.find((blob) => blob.id === id);
  }

  draw(): void {
    const context = this.canvas.getContext('2d');
    if (!context) return;
    context.clearRect(0, 0, this.canvas.width, this.canvas.height);
    for (const blob of this.blobs) {
      context.beginPath();
      context.arc(blob.x, blob.y, blob.radius, 0, Math.PI * 2);
      context.fillStyle = blob.color;
      context.fill();
      context.fillStyle = '#00000099';
      context.font = '13px system-ui, sans-serif';
      context.textAlign = 'center';
      context.fillText(String(blob.id), blob.x, blob.y + 4);
    }
  }

  /** Move a blob over half a second, so the model's work is visible. */
  async glide(blob: Blob, x: number, y: number): Promise<void> {
    const from = {x: blob.x, y: blob.y};
    const frames = 30;
    for (let frame = 1; frame <= frames; frame += 1) {
      blob.x = from.x + ((x - from.x) * frame) / frames;
      blob.y = from.y + ((y - from.y) * frame) / frames;
      this.draw();
      await new Promise((resolve) => setTimeout(resolve, 500 / frames));
    }
  }
}

// --- The actions the page serves ---------------------------------------------

/**
 * A port per argument, and a `user_facing_log` on the ones that change something.
 * That port is the tool's narration for the person watching: the backend keeps it
 * out of the model's result and records it with the turn.
 */
const DESCRIBE_SCENE_SCHEMA = new ActionSchema({
  name: 'describe_scene',
  description:
    'List the blobs on the page: their ids, colours and positions. Call this' +
    ' before changing anything, to find out what is there.',
  outputs: {
    blobs: new ActionPortSchema({
      name: 'blobs',
      type: 'application/json',
      required: true,
      description: 'One `{id, x, y, radius, color}` per blob.',
    }),
  },
});

const SET_COLOR_SCHEMA = new ActionSchema({
  name: 'set_color',
  description: 'Recolour blobs: the i-th id is given the i-th colour.',
  inputs: {
    ids: new ActionPortSchema({
      name: 'ids',
      type: 'application/json',
      required: true,
      description: 'Which blobs to recolour.',
    }),
    colors: new ActionPortSchema({
      name: 'colors',
      type: 'text/plain',
      required: true,
      description: 'One `#rrggbb` per id, in the same order.',
    }),
  },
  outputs: {
    recoloured: new ActionPortSchema({
      name: 'recoloured',
      type: 'application/json',
      unary: true,
      required: true,
      description: 'How many blobs changed colour.',
    }),
    [USER_FACING_LOG_PORT]: new ActionPortSchema({
      name: USER_FACING_LOG_PORT,
      type: 'text/plain',
      description: 'What the page did, for the person watching.',
    }),
  },
});

const SHIFT_POSITION_SCHEMA = new ActionSchema({
  name: 'shift_position',
  description: 'Move blobs by an offset in pixels. +x is right and +y is down.',
  inputs: {
    ids: new ActionPortSchema({
      name: 'ids',
      type: 'application/json',
      required: true,
      description: 'Which blobs to move.',
    }),
    dx: new ActionPortSchema({
      name: 'dx',
      type: 'application/json',
      unary: true,
      required: true,
      description: 'How far to move them horizontally, in pixels.',
    }),
    dy: new ActionPortSchema({
      name: 'dy',
      type: 'application/json',
      unary: true,
      required: true,
      description: 'How far to move them vertically, in pixels.',
    }),
  },
  outputs: {
    moved: new ActionPortSchema({
      name: 'moved',
      type: 'application/json',
      unary: true,
      required: true,
      description: 'How many blobs moved.',
    }),
    [USER_FACING_LOG_PORT]: new ActionPortSchema({
      name: USER_FACING_LOG_PORT,
      type: 'text/plain',
      description: 'What the page did, for the person watching.',
    }),
  },
});

const PAGE_TOOLS = [DESCRIBE_SCENE_SCHEMA, SET_COLOR_SCHEMA, SHIFT_POSITION_SCHEMA];

/**
 * What each port carries, where its MIME type does not say.
 *
 * A JS `ActionPortSchema` has a MIME type and no value type, so an
 * `application/json` port is described to the model as a bare object. These are
 * the shapes the model actually needs to see: numbers, and arrays of them.
 */
const PORT_SCHEMAS = {
  set_color: {ids: z.number().int(), colors: z.string()},
  shift_position: {ids: z.number().int(), dx: z.number(), dy: z.number()},
} as const;

const READ_TIMEOUT_MS = 10_000;

/** Read a streaming input port to its end. */
async function readAll(action: Action, port: string): Promise<unknown[]> {
  const node = need(await action.getInput(port));
  const values: unknown[] = [];
  for (;;) {
    const next = await node.next({timeoutMs: READ_TIMEOUT_MS});
    if (!isOk(next) || next === null) break;
    values.push(next);
  }
  return values;
}

/** Write the tool's narration and close the port. */
async function narrate(action: Action, text: string, onLog: (text: string) => void): Promise<void> {
  onLog(text);
  const node = need(await action.getOutput(USER_FACING_LOG_PORT));
  need(await node.putFinal(text));
  need(await node.drainAndClose());
}

/** Register the page's actions, each backed by the canvas. */
function pageRegistry(scene: Scene, onLog: (text: string) => void): ActionRegistry {
  const registry = new ActionRegistry();

  need(
    registry.register(DESCRIBE_SCENE_SCHEMA.name, DESCRIBE_SCENE_SCHEMA, async (action): Promise<Status> => {
      try {
        onLog(`describe_scene → ${scene.blobs.length} blobs`);
        const node = need(await action.getOutput('blobs'));
        for (const [index, blob] of scene.blobs.entries()) {
          need(await node.put({...blob}, {final: index === scene.blobs.length - 1}));
        }
        return await node.drainAndClose();
      } catch (error) {
        return statusFromUnknown(error, 'describe_scene failed.');
      }
    }),
  );

  need(
    registry.register(SET_COLOR_SCHEMA.name, SET_COLOR_SCHEMA, async (action): Promise<Status> => {
      try {
        const [ids, colors] = await Promise.all([readAll(action, 'ids'), readAll(action, 'colors')]);
        let recoloured = 0;
        for (const [index, id] of ids.entries()) {
          const blob = scene.find(Number(id));
          const color = colors[Math.min(index, colors.length - 1)];
          if (!blob || typeof color !== 'string') continue;
          blob.color = color;
          recoloured += 1;
        }
        scene.draw();
        const result = need(await action.getOutput('recoloured'));
        need(await result.putFinal(recoloured));
        need(await result.drainAndClose());
        await narrate(action, `Recoloured ${recoloured} blob(s): ${ids.join(', ')}.`, onLog);
        return okStatus();
      } catch (error) {
        return statusFromUnknown(error, 'set_color failed.');
      }
    }),
  );

  need(
    registry.register(SHIFT_POSITION_SCHEMA.name, SHIFT_POSITION_SCHEMA, async (action): Promise<Status> => {
      try {
        const ids = await readAll(action, 'ids');
        const dxNode = need(await action.getInput('dx'));
        const dyNode = need(await action.getInput('dy'));
        const dx = Number(need(await dxNode.consume({timeoutMs: READ_TIMEOUT_MS, allowNone: true})) ?? 0);
        const dy = Number(need(await dyNode.consume({timeoutMs: READ_TIMEOUT_MS, allowNone: true})) ?? 0);
        const moving = ids
          .map((id) => scene.find(Number(id)))
          .filter((blob): blob is Blob => blob !== undefined);
        // Answer the model once the move is under way rather than after it: the
        // animation is for the person watching.
        void Promise.all(moving.map((blob) => scene.glide(blob, blob.x + dx, blob.y + dy)));
        const result = need(await action.getOutput('moved'));
        need(await result.putFinal(moving.length));
        need(await result.drainAndClose());
        await narrate(action, `Moved ${moving.length} blob(s) by (${dx}, ${dy}).`, onLog);
        return okStatus();
      } catch (error) {
        return statusFromUnknown(error, 'shift_position failed.');
      }
    }),
  );

  return registry;
}

// --- The demo ----------------------------------------------------------------

const SYSTEM_PROMPT =
  'You are looking after a small canvas of coloured blobs in a web page. Use the' +
  ' tools to inspect and change it: call describe_scene when you need to know' +
  ' what is there, then set_color or shift_position to act. Do not describe what' +
  ' you would do — do it, then say in one sentence what you did. Pick colours' +
  ' yourself when none are given; the canvas is 620 by 300 pixels.';

class BrowserToolsDemo {
  private readonly backend = new BackendControls('tools');
  private readonly errors = document.querySelector<HTMLDivElement>('#tools-errors')!;
  private readonly messages = document.querySelector<HTMLDivElement>('#tools-messages')!;
  private readonly log = document.querySelector<HTMLDivElement>('#tools-log')!;
  private readonly scene = new Scene();
  private connection: Connection | null = null;

  /**
   * The session, with the page's own registry bound to it *before* the stream is
   * attached, and the tools announced once it is up.
   */
  private async connected(): Promise<Connection> {
    if (this.connection !== null) return this.connection;
    const registry = pageRegistry(this.scene, (text) => addLine(this.log, text));
    const connection = await connect(this.backend.server.value.trim() || DEFAULT_SERVER_URL, registry);
    const registered = await announceTools(connection, PAGE_TOOLS);
    addLine(this.log, `announced: ${registered.join(', ')}`, 'done');
    this.connection = connection;
    return connection;
  }

  async send(prompt: string): Promise<void> {
    this.errors.textContent = '';
    addBubble(this.messages, prompt, 'question');
    const answer = addBubble(this.messages, '', 'answer');
    try {
      const connection = await this.connected();
      // No history: each instruction stands on its own here, and the scene --
      // which the model reads with `describe_scene` -- is the state that matters.
      await runTurn({
        connection,
        backend: this.backend.value,
        prompt,
        history: [],
        systemPrompt: SYSTEM_PROMPT,
        tools: PAGE_TOOLS,
        portSchemas: PORT_SCHEMAS,
        onToken: streamInto(answer, this.messages),
      });
    } catch (error) {
      answer.remove();
      this.connection = null;
      showError(this.errors, error);
    }
  }
}

const root = document.querySelector('#tools-demo');
if (root) {
  const demo = new BrowserToolsDemo();
  const form = document.querySelector<HTMLFormElement>('#tools-form')!;
  const input = document.querySelector<HTMLInputElement>('#tools-input')!;
  form.onsubmit = (event) => {
    event.preventDefault();
    const prompt = input.value.trim();
    if (!prompt) return;
    input.value = '';
    void whileBusy(form, () => demo.send(prompt));
  };
}

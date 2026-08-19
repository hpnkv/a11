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
  invalidArgumentError,
  isOk,
  isStatus,
  okStatus,
  statusFromUnknown,
  type Action,
  type Status,
} from '../src/index.js';

import {
  BackendControls,
  DEFAULT_SERVER_URL,
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

  /** The drawing surface, in the coordinates the tools speak. */
  get width(): number {
    return this.canvas.width;
  }

  get height(): number {
    return this.canvas.height;
  }

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

  /** [contain], against this canvas. */
  contain(blob: Blob, x: number, y: number): {x: number; y: number} {
    return contain(blob, x, y, this.width, this.height);
  }

  /** Move a blob over half a second, so the model's work is visible. */
  async glide(blob: Blob, x: number, y: number): Promise<void> {
    // Defence in depth: a caller that got past validation with a NaN would
    // otherwise write NaN into the blob and erase it from the canvas for good.
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
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

/**
 * The nearest point to (x, y) that keeps a blob wholly on a `width` x `height`
 * canvas.
 *
 * A tool call is an outside instruction, and "off the left edge" is a place the
 * scene has no way to show. Clamping is the honest reading of "move it left" when
 * it is already at the edge; the alternative is a blob nobody can see or name
 * again.
 */
export function contain(
  blob: Blob,
  x: number,
  y: number,
  width: number,
  height: number,
): {x: number; y: number} {
  const clamp = (value: number, limit: number) =>
    Math.min(Math.max(value, blob.radius), limit - blob.radius);
  return {x: clamp(x, width), y: clamp(y, height)};
}

// --- Reading a tool call's arguments -----------------------------------------
//
// A tool call is the least trustworthy input a page gets: the values are a
// model's idea of what the schema said. So each argument is *validated before
// anything is touched*, and a call that cannot be honoured comes back as an
// INVALID_ARGUMENT status naming what was wrong. The tool runner hands that to
// the model as the call's result, which is what lets it correct itself and try
// again — whereas coercing the value silently (`Number("a bit left")` is `NaN`)
// applies nonsense to the scene and reports success.

/**
 * `value` as a finite number, or a status saying why it is not one.
 *
 * Emptiness is refused rather than read as zero: `Number('')` is `0`, so a model
 * that sends `dx: null` would otherwise be told it moved five blobs by nothing.
 * A port that carried *no* value at all is the caller's to default — see the
 * handler.
 */
export function finiteNumber(value: unknown, name: string, limit: number): number | Status {
  const empty = value === null || value === undefined || String(value).trim() === '';
  const numeric = empty ? NaN : typeof value === 'number' ? value : Number(String(value).trim());
  if (!Number.isFinite(numeric)) {
    return invalidArgumentError(
      `${name} must be a number of pixels; got ${JSON.stringify(value)}.`,
    );
  }
  if (Math.abs(numeric) > limit) {
    return invalidArgumentError(
      `${name} must be between -${limit} and ${limit} pixels; got ${numeric}.`,
    );
  }
  return numeric;
}

/** The blobs `ids` names, or a status naming the ones that do not exist. */
export function blobsFor(scene: Scene, ids: readonly unknown[]): Blob[] | Status {
  if (ids.length === 0) {
    return invalidArgumentError('ids must name at least one blob.');
  }
  const found: Blob[] = [];
  const unknown: unknown[] = [];
  for (const id of ids) {
    const numeric = typeof id === 'number' ? id : Number(String(id ?? '').trim());
    const blob = Number.isInteger(numeric) ? scene.find(numeric) : undefined;
    if (blob === undefined) unknown.push(id);
    else found.push(blob);
  }
  if (unknown.length > 0) {
    const known = scene.blobs.map((blob) => blob.id).join(', ');
    return invalidArgumentError(
      `no blob has id ${unknown.map((id) => JSON.stringify(id)).join(', ')};` +
        ` the ids are ${known}.`,
    );
  }
  return found;
}

/** A CSS colour this scene will accept, or a status saying why not. */
export function colorFor(value: unknown, id: number): string | Status {
  const color = typeof value === 'string' ? value.trim() : '';
  const known = /^#[0-9a-f]{3}$|^#[0-9a-f]{6}$|^[a-z]{3,20}$/i;
  if (!known.test(color)) {
    return invalidArgumentError(
      `the colour for blob ${id} must be #rgb, #rrggbb or a CSS colour name;` +
        ` got ${JSON.stringify(value)}.`,
    );
  }
  return color;
}

// --- The actions the page serves ---------------------------------------------

/**
 * A port per argument. Narration needs none: `log()` has its own.
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
  },
});

const SHIFT_POSITION_SCHEMA = new ActionSchema({
  name: 'shift_position',
  description:
    'Move blobs by an offset in whole pixels: +x is right, +y is down. The canvas' +
    ' is 620 by 300, and a blob that would leave it stops at the edge.',
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
      description: 'How far to move them horizontally, in pixels (a number).',
    }),
    dy: new ActionPortSchema({
      name: 'dy',
      type: 'application/json',
      unary: true,
      required: true,
      description: 'How far to move them vertically, in pixels (a number).',
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

/**
 * Narrate the run, to the page and to whoever called the tool.
 *
 * No port and nothing to close: `log()` writes to the action's own log, which the
 * backend reads separately from the outputs, so a tool's account of itself can
 * never turn up in the model's result.
 */
async function narrate(action: Action, text: string, onLog: (text: string) => void): Promise<void> {
  onLog(text);
  need(await action.log(text));
}

/**
 * Decline a call, telling both audiences: the log says what the page refused,
 * and the returned status is what the model is handed as this call's result.
 */
async function refuse(
  action: Action,
  status: Status,
  onLog: (text: string) => void,
): Promise<Status> {
  try {
    await narrate(action, `refused: ${status.message}`, onLog);
  } catch {
    // The refusal itself is the result; a log that will not write must not
    // replace it with a different failure.
  }
  return status;
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
        const blobs = blobsFor(scene, ids);
        if (isStatus(blobs)) return await refuse(action, blobs, onLog);
        // One colour is broadcast to every id; otherwise they pair up in order.
        const wanted: string[] = [];
        for (const [index, blob] of blobs.entries()) {
          const color = colorFor(colors[Math.min(index, colors.length - 1)], blob.id);
          if (isStatus(color)) return await refuse(action, color, onLog);
          wanted.push(color);
        }

        blobs.forEach((blob, index) => {
          blob.color = wanted[index]!;
        });
        scene.draw();
        const result = need(await action.getOutput('recoloured'));
        need(await result.putFinal(blobs.length));
        need(await result.drainAndClose());
        await narrate(
          action,
          `Recoloured ${blobs.length} blob(s): ${blobs.map((blob) => blob.id).join(', ')}.`,
          onLog,
        );
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
        const rawDx = need(await dxNode.consume({timeoutMs: READ_TIMEOUT_MS, allowNone: true}));
        const rawDy = need(await dyNode.consume({timeoutMs: READ_TIMEOUT_MS, allowNone: true}));

        const blobs = blobsFor(scene, ids);
        if (isStatus(blobs)) return await refuse(action, blobs, onLog);
        // An axis the caller left out is zero; both left out is a call that asks
        // for nothing, which is worth saying rather than reporting as done.
        if (rawDx === null && rawDy === null) {
          return await refuse(
            action,
            invalidArgumentError('shift_position needs dx, dy, or both.'),
            onLog,
          );
        }
        const dx = rawDx === null ? 0 : finiteNumber(rawDx, 'dx', scene.width);
        if (isStatus(dx)) return await refuse(action, dx, onLog);
        const dy = rawDy === null ? 0 : finiteNumber(rawDy, 'dy', scene.height);
        if (isStatus(dy)) return await refuse(action, dy, onLog);

        // Nothing is written to a blob until every argument has been read: a
        // half-applied move is harder to undo than a refused one.
        const moves = blobs.map((blob) => ({
          blob,
          to: scene.contain(blob, blob.x + dx, blob.y + dy),
        }));
        const held = moves.filter(
          ({blob, to}) => to.x !== blob.x + dx || to.y !== blob.y + dy,
        ).length;
        // Answer the model once the move is under way rather than after it: the
        // animation is for the person watching.
        void Promise.all(moves.map(({blob, to}) => scene.glide(blob, to.x, to.y)));
        const result = need(await action.getOutput('moved'));
        need(await result.putFinal(moves.length));
        need(await result.drainAndClose());
        const edge = held > 0 ? ` ${held} stopped at the edge of the canvas.` : '';
        await narrate(action, `Moved ${moves.length} blob(s) by (${dx}, ${dy}).${edge}`, onLog);
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

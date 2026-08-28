/**
 * "The model calls back into the page" guide demo.
 *
 * The page holds a three.js scene of distorted spheres and serves three actions
 * over it. It serves those actions on its own session's registry -- the backend
 * asks what is there rather than being told -- and from then on the model's tool
 * calls are dispatched back down the same WebSocket and run *here*: the backend
 * never touches the scene, and the model sees three ordinary A11 actions.
 *
 * The port names are the model's argument names: a tool definition is derived
 * from the action's ports, so `set_color(ids, colors)` is what the model is
 * offered. A streaming port becomes an array.
 */

import * as THREE from 'three';
import {OrbitControls} from 'three/examples/jsm/controls/OrbitControls.js';
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
    connect,
    need,
    probeConnection,
    reportExampleSuccess,
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
    z: number;
    radius: number;
    color: string;
    /** The sphere this blob is, so a handler can write straight to it. */
    mesh: THREE.Mesh;
}

const PALETTE = ['#4f6df5', '#f5a34f', '#4fb0f5', '#a34ff5', '#4ff5a3'];

// --- What one world unit is ---------------------------------------------------
//
// The units are pinned to the view rather than chosen freely, so a coordinate
// the model reads or writes means something it could have worked out from the
// picture: **the visible height runs from -1 at the bottom edge to +1 at the
// top**, and one unit is the same number of pixels across as it is down.
//
// Everything else follows from that. Two units cover the canvas vertically, so
// at 620x300 one unit is 150 px; horizontally the same 150 px per unit covers
// `2 * VIEW_ASPECT` units, which is where the x extent comes from. Depth is
// projected onto nothing, so it is given the height's span: the box the blobs
// live in is as deep as it is tall.
//
// Exact on the z = 0 plane the blobs start on. A perspective camera is what
// makes the scene read as three-dimensional and the trade for that is that
// something moved towards the camera is drawn larger; an orthographic one would
// hold 1:1 everywhere and lose the depth cue.

/** The design aspect. The canvas's `aspect-ratio` in CSS must match it. */
const VIEW_ASPECT = 620 / 300;
/** The visible height in world units: -1 to +1. */
const VIEW_HEIGHT = 2;
const FIELD_OF_VIEW = 45;

/**
 * How far back the camera sits for `VIEW_HEIGHT` to fill the frame exactly.
 *
 * Derived rather than written down, because then the two cannot disagree: a
 * perspective camera shows `2 * distance * tan(fov / 2)` at its focal plane.
 */
const CAMERA_DISTANCE =
    VIEW_HEIGHT / 2 / Math.tan(((FIELD_OF_VIEW * Math.PI) / 180) / 2);

/**
 * How far from the origin a blob's centre may be moved, per axis.
 *
 * The visible box, and it bounds the *centre*: a blob clamped to y = 1 sits
 * with its middle on the top edge and its upper half out of frame. Bounding the
 * silhouette instead would mean y = 1 was no longer the top of the picture,
 * which is the one thing these coordinates are for.
 */
const REACH = {
    x: (VIEW_HEIGHT / 2) * VIEW_ASPECT,
    y: VIEW_HEIGHT / 2,
    z: VIEW_HEIGHT / 2,
};

/** Blob size and spacing, in the same units. */
const RADIUS = 0.25;
const SPACING = 0.7;

const NOISE_GLSL = `
uniform float uTime;
uniform float uDistort;
uniform float uFrequency;
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289(((x * 34.0) + 1.0) * x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }
float snoise(vec3 v) {
  const vec2 C = vec2(1.0 / 6.0, 1.0 / 3.0);
  const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);
  vec3 i = floor(v + dot(v, C.yyy));
  vec3 x0 = v - i + dot(i, C.xxx);
  vec3 g = step(x0.yzx, x0.xyz);
  vec3 l = 1.0 - g;
  vec3 i1 = min(g.xyz, l.zxy);
  vec3 i2 = max(g.xyz, l.zxy);
  vec3 x1 = x0 - i1 + C.xxx;
  vec3 x2 = x0 - i2 + C.yyy;
  vec3 x3 = x0 - D.yyy;
  i = mod289(i);
  vec4 p = permute(permute(permute(
      i.z + vec4(0.0, i1.z, i2.z, 1.0)) +
      i.y + vec4(0.0, i1.y, i2.y, 1.0)) +
      i.x + vec4(0.0, i1.x, i2.x, 1.0));
  float n_ = 0.142857142857;
  vec3 ns = n_ * D.wyz - D.xzx;
  vec4 j = p - 49.0 * floor(p * ns.z * ns.z);
  vec4 x_ = floor(j * ns.z);
  vec4 y_ = floor(j - 7.0 * x_);
  vec4 x = x_ * ns.x + ns.yyyy;
  vec4 y = y_ * ns.x + ns.yyyy;
  vec4 h = 1.0 - abs(x) - abs(y);
  vec4 b0 = vec4(x.xy, y.xy);
  vec4 b1 = vec4(x.zw, y.zw);
  vec4 s0 = floor(b0) * 2.0 + 1.0;
  vec4 s1 = floor(b1) * 2.0 + 1.0;
  vec4 sh = -step(h, vec4(0.0));
  vec4 a0 = b0.xzyw + s0.xzyw * sh.xxyy;
  vec4 a1 = b1.xzyw + s1.xzyw * sh.zzww;
  vec3 p0 = vec3(a0.xy, h.x);
  vec3 p1 = vec3(a0.zw, h.y);
  vec3 p2 = vec3(a1.xy, h.z);
  vec3 p3 = vec3(a1.zw, h.w);
  vec4 norm = taylorInvSqrt(
      vec4(dot(p0, p0), dot(p1, p1), dot(p2, p2), dot(p3, p3)));
  p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;
  vec4 m = max(0.6 - vec4(
      dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
  m = m * m;
  return 42.0 * dot(m * m, vec4(
      dot(p0, x0), dot(p1, x1), dot(p2, x2), dot(p3, x3)));
}
`;

function distortMaterial(color: string): THREE.MeshStandardMaterial {
    const material = new THREE.MeshStandardMaterial({
        color: new THREE.Color(color),
        roughness: 0.45,
        metalness: 0.05,
    });
    // Both relative to the radius, so the scene's scale can change without
    // changing how a blob looks: the displacement is a fraction of the radius,
    // and the noise is sampled per radius rather than per unit. A higher
    // frequency separates its lobes and five spheres read as five caltrops.
    const uniforms = {
        uTime: {value: 0},
        uDistort: {value: 0.15 * RADIUS},
        uFrequency: {value: 1.1 / RADIUS},
    };
    material.onBeforeCompile = (shader) => {
        Object.assign(shader.uniforms, uniforms);
        shader.vertexShader = NOISE_GLSL + shader.vertexShader;
        shader.vertexShader = shader.vertexShader.replace(
            '#include <begin_vertex>',
            `vec3 transformed = position + normal * uDistort *
           snoise(position * uFrequency + vec3(uTime * 0.4));`,
        );
    };
    (material as THREE.MeshStandardMaterial & { uDistortUniforms?: typeof uniforms })
        .uDistortUniforms = uniforms;
    return material;
}

/** A small sprite carrying a blob's id, so the person watching can tell which. */
function label(id: number): THREE.Sprite {
    const size = 128;
    const canvas = document.createElement('canvas');
    canvas.width = size;
    canvas.height = size;
    const context = canvas.getContext('2d')!;
    context.font = 'bold 84px system-ui, sans-serif';
    context.textAlign = 'center';
    context.textBaseline = 'middle';
    // A dark digit inside a light halo, because the pane's background follows the
    // site's light and dark themes and a single colour disappears into one of
    // them. Drawn in this order so the halo never covers the digit.
    context.lineWidth = 14;
    context.strokeStyle = '#ffffff';
    context.lineJoin = 'round';
    context.strokeText(String(id), size / 2, size / 2);
    context.fillStyle = '#111418';
    context.fillText(String(id), size / 2, size / 2);
    const texture = new THREE.CanvasTexture(canvas);
    texture.colorSpace = THREE.SRGBColorSpace;
    const sprite = new THREE.Sprite(
        new THREE.SpriteMaterial({map: texture, transparent: true, depthTest: false}),
    );
    sprite.scale.setScalar(RADIUS * 0.85);
    return sprite;
}

class Scene {
    private readonly canvas = document.querySelector<HTMLCanvasElement>('#tools-canvas')!;
    private readonly renderer: THREE.WebGLRenderer;
    private readonly scene = new THREE.Scene();
    private readonly camera: THREE.PerspectiveCamera;
    private readonly controls: OrbitControls;
    private readonly clock = new THREE.Clock();
    readonly blobs: Blob[] = [];

    /** How far a blob's centre may be moved from the origin, per axis. */
    get reach(): { x: number; y: number; z: number } {
        return REACH;
    }

    constructor() {
        this.renderer = new THREE.WebGLRenderer({
            canvas: this.canvas,
            antialias: true,
            alpha: true,
        });
        this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
        this.renderer.setClearColor(0x000000, 0);

        this.camera = new THREE.PerspectiveCamera(
            FIELD_OF_VIEW, VIEW_ASPECT, 0.1, 100,
        );
        // Square on and centred. The orbit target is the origin either way, so
        // an off-axis camera would still frame it -- but only from here does
        // y = +1 land on the top edge and y = -1 on the bottom.
        this.camera.position.set(0, 0, CAMERA_DISTANCE);
        this.scene.add(this.camera);

        this.controls = new OrbitControls(this.camera, this.renderer.domElement);
        this.controls.enableDamping = true;
        this.controls.enablePan = false;
        // Below the starting distance, or the first update would push the
        // camera back out and the framing all of this rests on would not
        // survive a frame.
        this.controls.minDistance = CAMERA_DISTANCE / 2;
        this.controls.maxDistance = CAMERA_DISTANCE * 4;

        this.scene.add(new THREE.HemisphereLight(0xffffff, 0x223344, 1.1));
        const key = new THREE.DirectionalLight(0xffffff, 1.6);
        key.position.set(3, 5, 4);
        this.scene.add(key);

        const geometry = new THREE.SphereGeometry(RADIUS, 64, 64);
        for (let index = 0; index < 5; index += 1) {
            const color = PALETTE[index]!;
            const mesh = new THREE.Mesh(geometry, distortMaterial(color));
            const badge = label(index);
            badge.position.set(0, RADIUS * 1.85, 0);
            mesh.add(badge);
            this.scene.add(mesh);
            const blob: Blob = {
                id: index,
                x: (index - 2) * SPACING,
                y: 0,
                z: 0,
                radius: RADIUS,
                color,
                mesh,
            };
            mesh.position.set(blob.x, blob.y, blob.z);
            this.blobs.push(blob);
        }

        new ResizeObserver(() => this.resize()).observe(this.canvas);
        this.resize();
        this.renderer.setAnimationLoop(() => this.frame());
    }

    find(id: number): Blob | undefined {
        return this.blobs.find((blob) => blob.id === id);
    }

    /** Recolour a blob's material, which is what "colour" means here. */
    recolour(blob: Blob, color: string): void {
        blob.color = color;
        (blob.mesh.material as THREE.MeshStandardMaterial).color.set(color);
    }

    /** [contain], against this scene's visible box. */
    contain(x: number, y: number, z: number): { x: number; y: number; z: number } {
        return contain(x, y, z, REACH);
    }

    /** Move a blob over half a second, so the model's work is visible. */
    async glide(blob: Blob, x: number, y: number, z: number): Promise<void> {
        // Defence in depth: a caller that got past validation with a NaN would
        // otherwise write NaN into the transform and lose the blob for good.
        if (![x, y, z].every(Number.isFinite)) return;
        const from = new THREE.Vector3(blob.x, blob.y, blob.z);
        const to = new THREE.Vector3(x, y, z);
        const started = performance.now();
        const duration = 500;
        for (; ;) {
            const t = Math.min((performance.now() - started) / duration, 1);
            // Ease out, so the move reads as one gesture rather than a jump.
            const eased = 1 - (1 - t) * (1 - t);
            const at = from.clone().lerp(to, eased);
            blob.x = at.x;
            blob.y = at.y;
            blob.z = at.z;
            blob.mesh.position.copy(at);
            if (t >= 1) return;
            await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
        }
    }

    /** One frame: advance the distortion, let the camera settle, draw. */
    private frame(): void {
        const elapsed = this.clock.getElapsedTime();
        for (const blob of this.blobs) {
            const material = blob.mesh.material as THREE.MeshStandardMaterial & {
                uDistortUniforms?: { uTime: { value: number } };
            };
            // Offset per blob so five spheres are not wobbling in unison.
            if (material.uDistortUniforms) {
                material.uDistortUniforms.uTime.value = elapsed + blob.id * 1.7;
            }
        }
        this.controls.update();
        this.renderer.render(this.scene, this.camera);
    }

    private resize(): void {
        const width = this.canvas.clientWidth;
        const height = this.canvas.clientHeight;
        if (width === 0 || height === 0) return;
        this.renderer.setSize(width, height, false);
        this.camera.aspect = width / height;
        this.camera.updateProjectionMatrix();
    }
}

/**
 * The nearest point to (x, y, z) within `reach` of the origin, per axis.
 *
 * Tool calls cannot move a blob's centre out of frame, so every blob stays at
 * least half visible, and a clamp is reported in the action log rather than
 * applied silently.
 */
export function contain(
    x: number,
    y: number,
    z: number,
    reach: { x: number; y: number; z: number },
): { x: number; y: number; z: number } {
    const clamp = (value: number, limit: number) =>
        Math.min(Math.max(value, -limit), limit);
    return {x: clamp(x, reach.x), y: clamp(y, reach.y), z: clamp(z, reach.z)};
}

// --- Reading a tool call's arguments -----------------------------------------
//
// Validate model-supplied arguments before changing the page. Invalid values
// return INVALID_ARGUMENT so the model can revise the call; coercion could
// instead change the scene with values such as `Number("a bit left") === NaN`.

/**
 * `value` as a finite number, or a status saying why it is not one.
 *
 * Empty values are invalid because `Number('')` is `0` although no movement
 * was requested. The handler supplies defaults for absent port values.
 */
export function finiteNumber(value: unknown, name: string, limit: number): number | Status {
    const empty = value === null || value === undefined || String(value).trim() === '';
    const numeric = empty ? NaN : typeof value === 'number' ? value : Number(String(value).trim());
    if (!Number.isFinite(numeric)) {
        return invalidArgumentError(
            `${name} must be a number of scene units; got ${JSON.stringify(value)}.`,
        );
    }
    if (Math.abs(numeric) > limit) {
        return invalidArgumentError(
            `${name} must be between -${limit} and ${limit}; got ${numeric}.`,
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
 * The scene's extent in words, for the model.
 *
 * Built from the constants rather than written out beside them, because a
 * description that disagrees with the clamp is worse than no description: the
 * model works to the numbers it is given and then has its calls silently
 * adjusted.
 */
const EXTENT =
    `+x is right, +y is up, +z is towards the camera. The visible box is` +
    ` ${VIEW_HEIGHT} units tall (y from ${-REACH.y} to ${REACH.y}),` +
    ` ${(2 * REACH.x).toFixed(2)} wide (x from ${(-REACH.x).toFixed(2)} to` +
    ` ${REACH.x.toFixed(2)}) and ${2 * REACH.z} deep (z from ${-REACH.z} to` +
    ` ${REACH.z}). A blob is ${2 * RADIUS} across, they start ${SPACING} apart` +
    ` along x centred on the origin, and one that would leave the box stops at` +
    ` its edge.`;

/**
 * A port per argument. Narration needs none: `log()` has its own. That port is
 * the tool's narration for the person watching: the backend keeps it out of the
 * model's result and records it with the turn.
 */
const DESCRIBE_SCENE_SCHEMA = new ActionSchema({
    name: 'describe_scene',
    description:
        'List the blobs on the page: their ids, colours and positions in world' +
        ' units. Call this before changing anything, to find out what is there.',
    outputs: {
        blobs: new ActionPortSchema({
            name: 'blobs',
            type: 'application/json',
            required: true,
            description: 'One `{id, x, y, z, radius, color}` per blob, in world units.',
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
    description: `Move blobs by an offset in world units. ${EXTENT}`,
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
            description:
                'How far to move them along x, right being positive (a number).' +
                ' Leave it out to not move along x.',
        }),
        dy: new ActionPortSchema({
            name: 'dy',
            type: 'application/json',
            unary: true,
            description:
                'How far to move them along y, up being positive (a number).' +
                ' Leave it out to not move along y.',
        }),
        dz: new ActionPortSchema({
            name: 'dz',
            type: 'application/json',
            unary: true,
            description:
                'How far to move them along z, towards the camera being positive' +
                ' (a number). Leave it out to keep the blobs in their plane.',
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
    shift_position: {
        ids: z.number().int(),
        dx: z.number(),
        dy: z.number(),
        dz: z.number(),
    },
} as const;

const READ_TIMEOUT_MS = 10_000;

/** Read a streaming input port to its end. */
async function readAll(action: Action, port: string): Promise<unknown[]> {
    const node = need(await action.getInput(port));
    const values: unknown[] = [];
    for (; ;) {
        const next = await node.next({timeoutMs: READ_TIMEOUT_MS});
        if (!isOk(next) || next === null) break;
        values.push(next);
    }
    return values;
}

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
                const node = need(await action.getOutput('blobs'));
                // Named fields rather than the blob itself: it holds the `THREE.Mesh`
                // it is, and what the model wants is the state, not the renderer.
                for (const {id, x, y, z, radius, color} of scene.blobs) {
                    need(await node.put({id, x, y, z, radius, color}));
                }
                const closed = await node.finalize();
                if (!isOk(closed)) return closed;
                await narrate(action, `Described ${scene.blobs.length} blob(s).`, onLog);
                return okStatus();
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

                blobs.forEach((blob, index) => scene.recolour(blob, wanted[index]!));
                const result = need(await action.getOutput('recoloured'));
                need(await result.finalize(blobs.length));
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
                const raw: Record<string, unknown> = {};
                for (const axis of ['dx', 'dy', 'dz'] as const) {
                    const node = need(await action.getInput(axis));
                    raw[axis] = need(
                        await node.consume({timeoutMs: READ_TIMEOUT_MS, allowNone: true}),
                    );
                }

                const blobs = blobsFor(scene, ids);
                if (isStatus(blobs)) return await refuse(action, blobs, onLog);
                // An omitted axis defaults to zero. Reject calls that omit every axis.
                if (raw.dx === null && raw.dy === null && raw.dz === null) {
                    return await refuse(
                        action,
                        invalidArgumentError('shift_position needs dx, dy or dz.'),
                        onLog,
                    );
                }
                const offset: Record<string, number> = {};
                // Bounded per axis at twice its reach: enough to cross the box
                // edge to edge and no further, so a plausible-looking number in
                // the wrong unit is refused rather than quietly clamped.
                const span = {
                    dx: scene.reach.x, dy: scene.reach.y, dz: scene.reach.z,
                };
                for (const axis of ['dx', 'dy', 'dz'] as const) {
                    if (raw[axis] === null) {
                        offset[axis] = 0;
                        continue;
                    }
                    const value = finiteNumber(raw[axis], axis, 2 * span[axis]);
                    if (isStatus(value)) return await refuse(action, value, onLog);
                    offset[axis] = value;
                }
                const {dx, dy, dz} = offset as { dx: number; dy: number; dz: number };

                // Nothing is written to a blob until every argument has been read: a
                // half-applied move is harder to undo than a refused one.
                const moves = blobs.map((blob) => ({
                    blob,
                    to: scene.contain(blob.x + dx, blob.y + dy, blob.z + dz),
                }));
                const held = moves.filter(
                    ({blob, to}) =>
                        to.x !== blob.x + dx || to.y !== blob.y + dy || to.z !== blob.z + dz,
                ).length;
                // Answer the model once the move is under way rather than after it: the
                // animation is for the person watching.
                void Promise.all(
                    moves.map(({blob, to}) => scene.glide(blob, to.x, to.y, to.z)),
                );
                const result = need(await action.getOutput('moved'));
                need(await result.finalize(moves.length));
                const edge = held > 0 ? ` ${held} stopped at the edge of the scene.` : '';
                await narrate(
                    action,
                    `Moved ${moves.length} blob(s) by (${dx}, ${dy}, ${dz}).${edge}`,
                    onLog,
                );
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
    'You are looking after a small 3D scene of coloured blobs in a web page. Use' +
    ' the tools to inspect and change it: call describe_scene when you need to' +
    ' know what is there, then set_color or shift_position to act. Do not describe' +
    ' what you would do — do it, then say in one sentence what you did. Pick' +
    ` colours yourself when none are given. Positions are world units: ${EXTENT}`;

class BrowserToolsDemo {
    private readonly backend = new BackendControls('tools');
    private readonly errors = document.querySelector<HTMLDivElement>('#tools-errors')!;
    private readonly messages = document.querySelector<HTMLDivElement>('#tools-messages')!;
    private readonly log = document.querySelector<HTMLDivElement>('#tools-log')!;
    private readonly scene = new Scene();
    private connection: Connection | null = null;

    /**
     * The session, with the page's own registry bound to it *before* the stream
     * is attached -- which is the whole of it. The backend asks this session what
     * it serves, so there is nothing to announce.
     */
    private async connected(): Promise<Connection> {
        if (this.connection !== null) return this.connection;
        const registry = pageRegistry(this.scene, (text) => addLine(this.log, text));
        const connection = await connect(this.backend.server.value.trim() || DEFAULT_SERVER_URL, registry);
        addLine(this.log, `serving: ${PAGE_TOOLS.map((one) => one.name).join(', ')}`, 'done');
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
            // which the model reads with `describe_scene` -- is the state that
            // matters.
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
            reportExampleSuccess('browser-tools');
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
    // Early connection check so the page tells the user right away.
    const serverInput = document.querySelector<HTMLInputElement>('#tools-server')!;
    const errors = document.querySelector<HTMLDivElement>('#tools-errors')!;
    void probeConnection(serverInput.value.trim() || DEFAULT_SERVER_URL, errors);
}

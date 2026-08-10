/**
 * Thin typed wrappers over the JS↔Kotlin bridge that `A11WebView.kt` injects
 * into the page. Each `window.__a11Bridge.*` function is backed by a
 * `JBCefJsQuery`; the Kotlin handler runs the real IDE work and returns a JSON
 * string (or rejects with an error message on the query's failure channel).
 *
 * The Kotlin side is the single source of truth for tool schemas and behavior;
 * this module never hard-codes a tool.
 */

/** One IDE-exposed action, as described by `IdeTools.listDescriptors()`. */
export interface PortDescriptor {
  name: string;
  type: string;
  required: boolean;
  unary: boolean;
  description?: string;
  /** JSON Schema for the port's value, when the Kotlin schema declares one. */
  schema?: Record<string, unknown>;
  /** An output for the user to read, never to be shown to the model. */
  user_facing?: boolean;
}

export interface ActionDescriptor {
  name: string;
  description: string;
  inputs: PortDescriptor[];
  outputs: PortDescriptor[];
  /** Output port → JSON field of the assembled result (`$` for the whole value). */
  output_to_json_field?: Record<string, string>;
}

/** Connection + provider config the page needs, from `A11Settings` / backend. */
export interface A11Config {
  url: string;
  provider: string;
  model: string;
  apiKey: string;
  baseUrl: string;
  allowedTools: string[];
  /** Full product name of the running IDE, e.g. "CLion" or "IntelliJ IDEA". */
  ide: string;
  /** That IDE's version, e.g. "2026.1". */
  ideVersion: string;
  /** The open project's name, as the IDE shows it. */
  projectName: string;
  /** Its base directory; null for a project without a single root. */
  projectPath: string | null;
}

interface RawBridge {
  listActions(): Promise<string>;
  runAction(name: string, inputs: unknown): Promise<string>;
  getConfig(): Promise<string>;
}

declare global {
  interface Window {
    __a11Bridge?: RawBridge;
    /** "chat" or "actions"; set by the injected bootstrap script. */
    __A11_VIEW?: string;
  }
}

function raw(): RawBridge {
  const bridge = window.__a11Bridge;
  if (!bridge) throw new Error('The A11 Kotlin bridge is not available.');
  return bridge;
}

/** The IDE-exposed actions and their schemas. */
export async function listActions(): Promise<ActionDescriptor[]> {
  return JSON.parse(await raw().listActions()) as ActionDescriptor[];
}

/**
 * Run one IDE action by name, passing its inputs keyed by port name (a list for
 * a streaming port); returns the action's parsed JSON result.
 */
export async function runAction(
  name: string,
  inputs: Record<string, unknown>,
): Promise<unknown> {
  return JSON.parse(await raw().runAction(name, inputs));
}

/** Backend URL and provider config for the chat session. */
export async function getConfig(): Promise<A11Config> {
  return JSON.parse(await raw().getConfig()) as A11Config;
}

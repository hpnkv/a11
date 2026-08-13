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

/**
 * One record about one range of one file, as the review flow produces it — on
 * `comments` when it carries a comment, on `patches` when it carries a patch.
 *
 * The two ports are one suggestion split in half so the sentence need not wait for
 * the diff, and `id` is what the IDE puts back together. Lines and columns are
 * 0-based, `end_column` exclusive — `get_error_highlights`' own numbers for a
 * reported range, the model's for a range it found itself.
 */
export interface HighlightNote {
  path: string;
  /**
   * What the flow called the suggestion this record belongs to. A comment and the
   * patch that fixes the same thing share one; a record with no id stands alone.
   */
  id?: string;
  /**
   * Whether the IDE reported this range or the model found it itself. The two are
   * underlined differently in the editor, since a found range has no squiggle of
   * the IDE's own underneath it.
   */
  origin?: 'reported' | 'found';
  /** Short enough for a popup; may be empty when there is only a patch. */
  comment?: string;
  /** A unified diff in the form `apply_patch` takes; may be empty. */
  patch?: string;
  start_line: number;
  start_column: number;
  end_line: number;
  end_column: number;
}

interface RawBridge {
  listActions(): Promise<string>;
  runAction(name: string, inputs: unknown): Promise<string>;
  getConfig(): Promise<string>;
  readFlow(name: string): Promise<string>;
  suggestOnHighlight(note: HighlightNote): Promise<string>;
  clearSuggestions(path: string): Promise<string>;
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

/**
 * The text of a flow the plugin ships, by bare name (no directory, no `.flow`).
 *
 * Returned as-is rather than as JSON: a flow is source, and the one thing the
 * page does with it is hand it to `flow_run`. The Kotlin side reads it from the
 * plugin's resources, so the file on disk in `scripts/` stays the only copy.
 */
export async function readFlow(name: string): Promise<string> {
  return raw().readFlow(name);
}

/**
 * Attach one record — a comment, or a patch — to the range of the file it is about,
 * so the IDE shows it on that range itself. A record whose `id` names a suggestion
 * already attached is merged into it rather than marking the range twice.
 *
 * Not a `runAction` call, because this is not one of the IDE *tools*: those are all
 * announced to the model, and a "write into the editor's popups" sink is the
 * plugin's own business. The Kotlin side keeps the suggestion, marks the range, and
 * renders the popup — see `highlights/HighlightSuggestions.kt`.
 */
export async function suggestOnHighlight(note: HighlightNote): Promise<void> {
  await raw().suggestOnHighlight(note);
}

/**
 * Drop the suggestions of a previous run: one file's, or every one of them when
 * `path` is omitted. A run replaces what the last one left rather than piling on
 * top of it.
 */
export async function clearSuggestions(path?: string): Promise<void> {
  await raw().clearSuggestions(path ?? '');
}

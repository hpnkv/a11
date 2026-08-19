/**
 * What this UI asks of the editor it is running in, and nothing more.
 *
 * **The seam.** Everything in this package is ordinary browser code: the chat,
 * the action explorer, the conversation list, the markdown renderer, the forms
 * built from a port schema. The six functions below are the whole of what any of
 * it needs from the host, and each host implements them its own way — JetBrains
 * over `JBCefJsQuery`, VSCode over `postMessage`. That is why one UI serves two
 * editors without either of them running the other's code.
 *
 * **The host is the single source of truth for tools.** Nothing here hard-codes a
 * tool: the schemas arrive from `listActions()` and execution goes back through
 * `runAction()`, so a tool added on the host side shows up here with no change.
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

/**
 * The editor, as this UI sees it.
 *
 * Every method answers with a JSON *string* rather than a value, because that is
 * what both hosts can carry: a `JBCefJsQuery` returns one, and a `postMessage`
 * channel is serialised anyway. Parsing happens once, in the wrappers below.
 */
export interface HostBridge {
  listActions(): Promise<string>;
  runAction(name: string, inputs: unknown): Promise<string>;
  getConfig(): Promise<string>;
  readFlow(name: string): Promise<string>;
  suggestOnHighlight(note: HighlightNote): Promise<string>;
  clearSuggestions(path: string): Promise<string>;
}

declare global {
  interface Window {
    /**
     * The JetBrains host, injected into the page by `A11WebView.kt` before this
     * bundle runs. Read by that host's own entry point, which hands it to
     * [setHost]; nothing in the shared UI looks at it.
     */
    __a11Bridge?: HostBridge;
    /** "chat" or "actions": which surface to mount. Set by the host. */
    __A11_VIEW?: string;
  }
}

let installed: HostBridge | undefined;

/**
 * Say which editor this is running in. Called once, by the host's entry point,
 * before anything is mounted.
 */
export function setHost(bridge: HostBridge): void {
  installed = bridge;
}

function raw(): HostBridge {
  if (!installed) throw new Error('No A11 editor bridge has been installed.');
  return installed;
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

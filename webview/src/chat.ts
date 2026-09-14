/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * The chat view: a scrolling transcript of turn bubbles, a composer, live token
 * streaming with markdown rendering, an auto-scroll-to-bottom that respects a
 * user who has scrolled up, and a "thinking" affordance fed by the `thoughts`
 * stream.
 *
 * Following the stream is a latched decision, not a per-token measurement: the
 * transcript sticks to the bottom until the user scrolls away from it, and
 * re-latches when they scroll back. Re-measuring after each token would unlatch
 * on any chunk that grew the transcript by more than the threshold in one go
 * (a code block, a markdown re-render) and silently stop following.
 */

import { A11ChatSession } from './a11client.js';
import { clearSuggestions, highlightFlow, suggestOnHighlight } from './bridge.js';
import type { MountedView } from './mount.js';
import {
  interactionText,
  isToolResultCarrier,
  toolLogs,
  toolOutputs,
  type ConversationSummary,
} from './conversations.js';
import { renderMarkdown } from './markdown.js';
import {renderFlowSource} from './flowRunner.js';
import {createConnectionStatus} from './connectionStatus.js';
import {renderDebugValue} from './outputPresentation.js';
import {
  BlockKind,
  isOk,
  presentInteraction,
  Role,
  toolStatuses,
  type Interaction,
} from '@curiositystack/a11';
import type { ToolActivity } from './ideTools.js';

/** How far from the bottom still counts as "following the stream". */
const NEAR_BOTTOM_PX = 48;

/**
 * The flow the "Suggest fixes" button runs, by the name the plugin bundles it
 * under (`src/main/resources/flows/suggest-fixes.flow`).
 */
const SUGGEST_FLOW = 'suggest-fixes';

/** Rows the composer opens with, and how tall it may grow while typing. */
const COMPOSER_ROWS = 3;
const COMPOSER_MAX_PX = 260;

/** A completed unary report input, as readable text. */
function completionText(
  arguments_: Record<string, unknown> | undefined,
  name: string,
): string {
  const value = arguments_?.[name];
  if (typeof value === 'string') return value.trim();
  if (Array.isArray(value)) return value.map(String).join('\n').trim();
  return value == null ? '' : String(value).trim();
}

/** One quiet nested section in an IDE completion report. */
function completionSection(title: string, value: string): HTMLElement {
  const section = document.createElement('section');
  section.className = 'completion-section';
  const heading = document.createElement('strong');
  heading.textContent = title;
  const body = document.createElement('div');
  body.textContent = value;
  section.append(heading, body);
  return section;
}

/** Compact call arguments for a collapsed tool card. */
function toolInputPreview(inputs: Record<string, unknown> | undefined): string {
  if (!inputs || Object.keys(inputs).length === 0) return '';
  return Object.entries(inputs)
    .slice(0, 4)
    .map(([name, value]) => `${name}: ${boundedValue(value)}`)
    .join(' · ')
    .slice(0, 320);
}

function boundedValue(value: unknown): string {
  if (value instanceof Uint8Array) return `<${value.byteLength} bytes>`;
  if (typeof value === 'string') {
    return JSON.stringify(value.length > 100 ? `${value.slice(0, 97)}…` : value);
  }
  if (value === null || typeof value !== 'object') return String(value);
  try {
    const text = JSON.stringify(value, (_key, nested) =>
      nested instanceof Uint8Array ? `<${nested.byteLength} bytes>` : nested,
    );
    return text.length > 120 ? `${text.slice(0, 117)}…` : text;
  } catch {
    return String(value);
  }
}

/** One expandable Inputs, Outputs, or Logs section in a tool card. */
function toolDetailSection(
  label: string,
  value: unknown,
  empty: string,
): HTMLElement {
  const section = document.createElement('section');
  section.className = 'tool-detail-section';
  const heading = document.createElement('strong');
  heading.textContent = label;
  const content = document.createElement('div');
  content.className = 'tool-detail-value';
  if (value === undefined || value === null || value === '') {
    content.classList.add('empty');
    content.textContent = empty;
  } else if (typeof value === 'string') {
    renderDebugValue(content, value, 'text/plain');
  } else {
    renderDebugValue(content, value);
  }
  section.append(heading, content);
  return section;
}

/**
 * One assistant turn's rendering: the turn's content as an ordered sequence of
 * blocks — streamed answer text, thinking panels, and tool-run boxes, in the
 * order they actually happened.
 *
 * The turn is one continuous stream interrupted by tool calls, so characters are
 * accumulated into the *current* block and anything else closes it: the next
 * token opens a fresh one below. That keeps "I'll check the selection" →
 * [get_selection] → "it defines a model class" reading in that order, rather
 * than collecting every box at the bottom.
 *
 * Thinking is a block like any other, not a panel pinned to the top, because
 * models do not all think first: some answer, then think, then answer again, and
 * some call tools while still thinking rather than after they have spoken. So a
 * thinking panel opens where the thoughts started, a tool run that happens while
 * it is open lands *inside* it, and the answer text that follows closes it.
 *
 * The order these arrive in is the order the model produced them: a backend
 * writes the deltas of one provider stream onto `thoughts` and `text_output` as
 * it reads them, both on the same action, so the wire carries them interleaved
 * and this side only has to render what it is handed. Nothing here looks at a
 * provider's events — that stays in the backend that speaks to that provider.
 */
export class AssistantBubble {
  private readonly indicator: HTMLDivElement;
  /**
   * Where streamed characters are currently landing, and how it renders. Null
   * between blocks — after a tool run, or when the kind of stream changed —
   * which is what makes the next character open a fresh block.
   */
  private sink: { element: HTMLElement; text: string[]; markdown: boolean } | null = null;
  /** The thinking panel the turn is inside, while it is thinking. */
  private thinking: { details: HTMLDetailsElement; summary: HTMLElement; tools: number } | null = null;
  private wroteAnything = false;
  /** Pending paint (a `requestAnimationFrame` handle), or 0 when up to date. */
  private frame = 0;
  private readonly toolElements = new Map<string, {
    box: HTMLElement;
    summary: HTMLSpanElement;
    body: HTMLDivElement;
    activity?: ToolActivity;
    state?: HTMLSpanElement;
    preview?: HTMLElement;
    completionState?: HTMLSpanElement;
    requestState?: HTMLSpanElement;
    requestTitle?: HTMLElement;
    requestButtons?: HTMLButtonElement[];
  }>();

  constructor(
    private readonly element: HTMLElement,
    private readonly onGrow: () => void,
    private readonly onAnswer?: (id: string, answer: string) => void,
  ) {
    // A working indicator shown until the first thought or answer token arrives.
    this.indicator = document.createElement('div');
    this.indicator.className = 'thinking';
    this.indicator.innerHTML = '<span class="dot"></span><span class="dot"></span><span class="dot"></span>';
    element.append(this.indicator);
  }

  /** Open a block for streamed characters at the end of `parent`. */
  private openSink(parent: HTMLElement, className: string, markdown: boolean): void {
    const element = document.createElement('div');
    element.className = className;
    parent.append(element);
    this.sink = { element, text: [], markdown };
  }

  appendToken(text: string): void {
    this.indicator.remove();
    // Answering is what ends a thinking phase; a later thought opens a new panel.
    this.closeThinking();
    if (!this.sink) this.openSink(this.element, 'bubble-body', true);
    this.sink!.text.push(text);
    this.wroteAnything = true;
    this.schedulePaint();
  }

  /**
   * Coalesce streamed text into one paint per frame.
   *
   * A token is not a unit of work worth a paint. Rendering each one meant
   * re-parsing the whole segment's markdown and replacing its subtree — so the
   * cost grew with the length of the answer, several times per frame — and then
   * reading `scrollHeight` to follow the stream, which forces the layout that
   * write just invalidated. Tokens arrive far faster than the display refreshes,
   * so most of that work was thrown away unseen, and what the user felt was the
   * scrolling stuttering against it. One render and one scroll per frame paints
   * the same thing at a fraction of the cost.
   */
  private schedulePaint(): void {
    if (this.frame) return;
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      this.paint();
    });
  }

  /**
   * Bring the DOM up to date with everything accumulated so far.
   *
   * Only the open block can have grown since the last frame — every closed one
   * was painted by the [flushPaint] that closed it — so there is exactly one
   * element to write.
   */
  private paint(): void {
    if (this.sink) {
      const text = this.sink.text.join('');
      if (this.sink.markdown) this.sink.element.innerHTML = renderMarkdown(text);
      else this.sink.element.textContent = text;
    }
    this.onGrow();
  }

  /**
   * Paint now, cancelling any frame already scheduled.
   *
   * Anything that closes a block or ends the turn has to go through this first:
   * the text accumulated since the last frame is still only in the array, and a
   * tool box appended in front of it would leave it unpainted for good.
   */
  private flushPaint(): void {
    if (this.frame) {
      cancelAnimationFrame(this.frame);
      this.frame = 0;
    }
    this.paint();
  }

  /**
   * Record one tool run at the point in the turn where it happened: a folded box
   * showing the log's first line, which opens to the rest rendered as markdown.
   * This is the plugin's own account of what the tool did — the model never sees
   * it, so it is free to be verbose.
   *
   * A run that happened while the model was thinking goes inside the open
   * thinking panel, since that is where in the turn it belongs; anything else
   * goes at the end of the bubble.
   */
  addToolRun(run: ToolActivity): void {
    this.indicator.remove();
    const existing = this.toolElements.get(run.id);
    if (existing) {
      const failed = run.status !== undefined && !isOk(run.status);
      existing.box.classList.toggle('failed', failed);
      if (existing.completionState) {
        existing.completionState.textContent = failed
          ? 'Report failed'
          : run.phase === 'started'
            ? 'Finalising…'
            : 'Recorded';
        existing.body.textContent = run.log ?? '';
        this.onGrow();
        return;
      }
      if (existing.requestState) {
        if (existing.requestTitle) {
          existing.requestTitle.textContent = failed
            ? 'Input request failed'
            : run.phase === 'started'
              ? 'Input required'
              : 'Input received';
        }
        existing.requestState.textContent = failed
          ? 'Request failed'
          : run.phase === 'started'
            ? 'Waiting…'
            : 'Answered';
        for (const button of existing.requestButtons ?? []) {
          button.disabled = failed || run.phase !== 'started';
        }
        existing.body.textContent = run.log ?? '';
        this.onGrow();
        return;
      }
      if (existing.activity) {
        existing.activity = {
          ...existing.activity,
          ...run,
          arguments: run.arguments ?? existing.activity.arguments,
          outputs: run.outputs ?? existing.activity.outputs,
        };
        this.updateToolDetails(existing);
        return;
      }
      existing.summary.textContent = failed
        ? run.status!.message || 'failed'
        : run.phase === 'started'
          ? 'running…'
          : (run.log ?? '').split('\n')[0]?.trim() || 'complete';
      const detail = (run.log ?? '').split('\n').slice(1).join('\n').trim();
      if (detail) existing.body.innerHTML = renderMarkdown(detail);
      this.onGrow();
      return;
    }
    if (run.tool === 'report_completion' && completionText(run.arguments, 'summary')) {
      this.addCompletionReport(run);
      return;
    }
    if (run.tool === 'request_user_input' && completionText(run.arguments, 'question')) {
      this.addInputRequest(run);
      return;
    }
    // The box goes after the text that led to it, so that text has to be on the
    // page before the box is appended.
    this.flushPaint();
    const box = document.createElement('details');
    box.className = 'tool-run';
    const head = document.createElement('summary');
    const label = document.createElement('span');
    label.className = 'tool-run-name';
    label.textContent = run.tool === 'run_flow' ? 'A11 Flow' : run.tool;
    const text = document.createElement('span');
    text.className = 'tool-run-summary';
    const state = document.createElement('span');
    state.className = 'tool-run-state';
    head.append(label, text, state);
    box.append(head);
    const body = document.createElement('div');
    body.className = 'tool-run-body';
    box.append(body);
    const entry = {box, summary: text, body, activity: run, state};
    this.toolElements.set(run.id, entry);
    this.updateToolDetails(entry);
    if (this.thinking) {
      this.thinking.details.append(box);
      this.thinking.tools += 1;
    } else {
      this.element.append(box);
    }
    // Whatever the model says or thinks next belongs after this box, not before
    // it — including a thought, which opens a fresh run below it in the panel.
    this.sink = null;
    this.wroteAnything = true;
    this.onGrow();
  }

  /** Refresh a generic tool card from the latest streamed call information. */
  private updateToolDetails(entry: {
    box: HTMLElement;
    summary: HTMLSpanElement;
    body: HTMLDivElement;
    activity?: ToolActivity;
    state?: HTMLSpanElement;
    preview?: HTMLElement;
  }): void {
    const run = entry.activity!;
    const isFlow = run.tool === 'run_flow';
    entry.box.classList.toggle('flow-run', isFlow);
    if (isFlow) {
      const name = entry.box.querySelector<HTMLElement>('.tool-run-name');
      if (name) name.textContent = run.phase === 'started' ? 'Running A11 Flow' : 'Ran A11 Flow';
    }
    const failed = run.status !== undefined && !isOk(run.status);
    entry.box.classList.toggle('failed', failed);
    entry.state!.textContent = failed
      ? 'Failed'
      : run.phase === 'started'
        ? 'running…'
        : 'completed';
    const source = isFlow && typeof run.arguments?.source === 'string'
      ? run.arguments.source
      : '';
    const inputs = source
      ? Object.fromEntries(Object.entries(run.arguments ?? {}).filter(([name]) => name !== 'source'))
      : run.arguments;
    const summary = (run.log ?? '').split('\n')[0]?.trim();
    entry.summary.textContent = failed
      ? run.status?.message || summary || 'The action failed.'
      : summary || toolInputPreview(inputs);

    entry.preview?.remove();
    entry.preview = undefined;
    if (source) {
      const preview = document.createElement('pre');
      preview.className = 'flow-preview';
      preview.textContent = source;
      entry.preview = preview;
      void highlightFlow(source).then((tokens) => {
        if (entry.activity?.arguments?.source === source) {
          renderFlowSource(preview, source, tokens);
          this.onGrow();
        }
      }).catch(() => undefined);
    }

    entry.body.innerHTML = '';
    if (entry.preview) {
      const flowSource = document.createElement('section');
      flowSource.className = 'tool-detail flow-source-detail';
      const heading = document.createElement('h4');
      heading.textContent = 'Flow';
      flowSource.append(heading, entry.preview);
      entry.body.append(flowSource);
    }
    entry.body.append(
      toolDetailSection('Inputs', inputs, source ? 'Flow source is shown above.' : 'No input values.'),
      toolDetailSection('Outputs', run.outputs, run.phase === 'started' ? 'Waiting for output…' : 'No output values.'),
      toolDetailSection('Logs', run.log, run.phase === 'started' ? 'Waiting for logs…' : 'Nothing logged.'),
    );
    if (failed) {
      const failure = document.createElement('div');
      failure.className = 'tool-run-failure';
      failure.textContent = run.status?.message || 'The action failed.';
      entry.body.append(failure);
    }
    this.onGrow();
  }

  /** Present a fully received report_completion call as the turn's outcome. */
  private addCompletionReport(run: ToolActivity): void {
    this.flushPaint();
    const failed = run.status !== undefined && !isOk(run.status);
    const report = document.createElement('article');
    report.className = `completion-report${failed ? ' failed' : ''}`;
    report.setAttribute('aria-label', 'Completion report');

    const header = document.createElement('div');
    header.className = 'completion-header';
    const icon = document.createElement('span');
    icon.className = 'completion-icon';
    icon.textContent = failed ? '!' : '✓';
    const heading = document.createElement('div');
    heading.className = 'completion-heading';
    const title = document.createElement('strong');
    title.textContent = failed ? 'Completion report failed' : 'Task completed';
    const subtitle = document.createElement('span');
    subtitle.textContent = 'Completion reported by the coding agent';
    heading.append(title, subtitle);
    const state = document.createElement('span');
    state.className = 'completion-state';
    state.textContent = failed
      ? 'Report failed'
      : run.phase === 'started'
        ? 'Finalising…'
        : 'Recorded';
    header.append(icon, heading, state);
    report.append(header);

    const content = document.createElement('div');
    content.className = 'completion-content';
    const summary = document.createElement('div');
    summary.className = 'completion-summary';
    summary.innerHTML = renderMarkdown(completionText(run.arguments, 'summary'));
    content.append(summary);
    const sections = document.createElement('div');
    sections.className = 'completion-sections';
    const checks = completionText(run.arguments, 'checks');
    const remaining = completionText(run.arguments, 'remaining');
    if (checks) sections.append(completionSection('Verification', checks));
    if (remaining) sections.append(completionSection('Remaining', remaining));
    if (sections.childElementCount > 0) content.append(sections);
    report.append(content);

    const technical = document.createElement('details');
    technical.className = 'completion-technical';
    const technicalSummary = document.createElement('summary');
    technicalSummary.textContent = 'Technical details';
    const body = document.createElement('div');
    body.className = 'completion-log';
    body.textContent = run.log ?? '';
    technical.append(technicalSummary, body);
    report.append(technical);

    const hiddenName = document.createElement('span');
    hiddenName.className = 'tool-run-name';
    hiddenName.textContent = run.tool;
    this.toolElements.set(run.id, {
      box: report,
      summary: hiddenName,
      body,
      completionState: state,
    });
    if (this.thinking) {
      this.thinking.details.append(report);
      this.thinking.tools += 1;
    } else {
      this.element.append(report);
    }
    this.sink = null;
    this.wroteAnything = true;
    this.onGrow();
  }

  /** Present a complete request_user_input call as an interactive question. */
  private addInputRequest(run: ToolActivity): void {
    this.flushPaint();
    const failed = run.status !== undefined && !isOk(run.status);
    const request = document.createElement('article');
    request.className = `input-request-report${failed ? ' failed' : ''}`;
    request.setAttribute('aria-label', 'User input request');

    const header = document.createElement('div');
    header.className = 'input-request-header';
    const icon = document.createElement('span');
    icon.className = 'input-request-icon';
    icon.textContent = failed ? '!' : '?';
    const heading = document.createElement('div');
    heading.className = 'input-request-heading';
    const title = document.createElement('strong');
    title.textContent = failed
      ? 'Input request failed'
      : run.phase === 'started'
        ? 'Input required'
        : 'Input received';
    const question = document.createElement('span');
    question.textContent = completionText(run.arguments, 'question');
    heading.append(title, question);
    const state = document.createElement('span');
    state.className = 'input-request-state';
    state.textContent = failed
      ? 'Request failed'
      : run.phase === 'started'
        ? 'Waiting…'
        : 'Answered';
    header.append(icon, heading, state);
    request.append(header);

    const choices = document.createElement('div');
    choices.className = 'input-request-choices';
    const options = Array.isArray(run.arguments?.options) ? run.arguments.options : [];
    const buttons: HTMLButtonElement[] = [];
    for (const [index, option] of options.entries()) {
      if (!option || typeof option !== 'object') continue;
      const record = option as Record<string, unknown>;
      const label = String(record.label ?? '').trim();
      if (!label) continue;
      const choice = document.createElement('button');
      choice.type = 'button';
      choice.className = 'input-request-choice';
      choice.disabled = failed || run.phase !== 'started' || !this.onAnswer;
      const choiceLabel = document.createElement('strong');
      choiceLabel.textContent = `${index + 1}. ${label}`;
      choice.append(choiceLabel);
      const description = String(record.description ?? '').trim();
      if (description) {
        const detail = document.createElement('span');
        detail.textContent = description;
        choice.append(detail);
      }
      choice.addEventListener('click', () => {
        for (const button of buttons) button.disabled = true;
        choice.disabled = true;
        this.onAnswer?.(run.id, label);
      });
      choices.append(choice);
      buttons.push(choice);
    }
    const allowFreeText = options.length === 0 || run.arguments?.allow_free_text !== false;
    if (allowFreeText) {
      const hint = document.createElement('p');
      hint.className = 'input-request-hint';
      hint.textContent = options.length
        ? 'A custom response is also accepted in the composer.'
        : 'Enter a free-text response in the composer.';
      choices.append(hint);
    }
    request.append(choices);

    const technical = document.createElement('details');
    technical.className = 'input-request-technical';
    const technicalSummary = document.createElement('summary');
    technicalSummary.textContent = 'Technical details';
    const body = document.createElement('div');
    body.className = 'input-request-log';
    body.textContent = run.log ?? '';
    technical.append(technicalSummary, body);
    request.append(technical);

    const hiddenName = document.createElement('span');
    hiddenName.className = 'tool-run-name';
    hiddenName.textContent = run.tool;
    this.toolElements.set(run.id, {
      box: request,
      summary: hiddenName,
      body,
      requestState: state,
      requestTitle: title,
      requestButtons: buttons,
    });
    if (this.thinking) {
      this.thinking.details.append(request);
      this.thinking.tools += 1;
    } else {
      this.element.append(request);
    }
    this.sink = null;
    this.wroteAnything = true;
    this.onGrow();
  }

  appendThought(text: string): void {
    this.indicator.remove();
    if (!this.thinking) {
      // The panel goes after whatever the turn has said so far, so that text has
      // to be painted before it is appended.
      this.flushPaint();
      this.sink = null;
      const details = document.createElement('details');
      details.className = 'thinking-details';
      details.open = true;
      const summary = document.createElement('summary');
      summary.textContent = 'Thinking...';
      details.append(summary);
      this.element.append(details);
      this.thinking = { details, summary, tools: 0 };
    }
    if (!this.sink) this.openSink(this.thinking.details, 'thoughts-body', false);
    this.sink!.text.push(text);
    this.schedulePaint();
  }

  /**
   * Collapse (but keep) the open thinking panel, and say what is folded away
   * inside it: with tool runs in there, an unlabelled "Thoughts" would hide the
   * only record that they happened.
   */
  private closeThinking(): void {
    if (!this.thinking) return;
    this.flushPaint();
    const { details, summary, tools } = this.thinking;
    details.open = false;
    summary.textContent = tools
      ? `Thoughts · ${tools} tool call${tools === 1 ? '' : 's'}`
      : 'Thoughts';
    this.thinking = null;
    this.sink = null;
  }

  finish(): void {
    this.indicator.remove();
    this.flushPaint();
    this.closeThinking();
    if (!this.wroteAnything) {
      const empty = document.createElement('div');
      empty.className = 'bubble-body muted';
      empty.textContent = '(no output)';
      this.element.append(empty);
    }
  }

  fail(message: string): void {
    this.indicator.remove();
    this.flushPaint();
    this.closeThinking();
    // Appended, not substituted: whatever the turn managed to say and do before
    // failing is part of the story of what went wrong.
    const error = document.createElement('div');
    error.className = 'bubble-body error';
    error.textContent = message;
    this.element.append(error);
    this.sink = null;
    this.onGrow();
  }
}

export class ChatView {
  private readonly transcript: HTMLDivElement;
  private readonly historyPanel: HTMLDivElement;
  private readonly historyButton: HTMLButtonElement;
  private readonly newChatButton: HTMLButtonElement;
  private readonly suggestButton: HTMLButtonElement;
  private readonly textarea: HTMLTextAreaElement;
  private readonly sendButton: HTMLButtonElement;
  private readonly connection = createConnectionStatus();
  private readonly root: HTMLElement;
  private session: A11ChatSession | null = null;
  private busy = false;
  private interruptible = false;
  private following = true;
  private historyOpen = false;
  /** The turn currently streaming, so tool runs land in the right bubble. */
  private active: AssistantBubble | null = null;
  private pendingInput: ({ id: string } & Record<string, unknown>) | null = null;

  constructor(root: HTMLElement) {
    this.root = root;
    root.classList.add('chat-view');

    const bar = document.createElement('div');
    bar.className = 'chat-bar';
    this.newChatButton = document.createElement('button');
    this.newChatButton.type = 'button';
    this.newChatButton.className = 'quiet';
    this.newChatButton.textContent = '+ New chat';
    this.historyButton = document.createElement('button');
    this.historyButton.type = 'button';
    this.historyButton.className = 'quiet';
    this.historyButton.textContent = 'History';
    this.suggestButton = document.createElement('button');
    this.suggestButton.type = 'button';
    this.suggestButton.className = 'quiet';
    this.suggestButton.textContent = 'Suggest fixes';
    this.suggestButton.title =
      "Review the file you're looking at: what the IDE underlines, and what it doesn't";
    bar.append(this.newChatButton, this.historyButton, this.suggestButton, this.connection.element);

    this.transcript = document.createElement('div');
    this.transcript.className = 'transcript';

    // The history takes the transcript's place rather than floating over it:
    // one column, nothing to position, and no way for the two to disagree about
    // which is scrollable. Which of the two is showing is a class on the view,
    // not the `hidden` attribute: both are `display: flex`, which outranks the
    // browser's `[hidden] { display: none }` and would leave both on screen.
    this.historyPanel = document.createElement('div');
    this.historyPanel.className = 'history-panel';

    const composer = document.createElement('form');
    composer.className = 'composer';
    this.textarea = document.createElement('textarea');
    this.textarea.rows = COMPOSER_ROWS;
    this.textarea.placeholder = 'Ask A11 about your project...  (Enter to send, Shift+Enter for newline)';
    this.sendButton = document.createElement('button');
    this.sendButton.type = 'submit';
    this.sendButton.textContent = 'Send';
    composer.append(this.textarea, this.sendButton);
    root.append(bar, this.transcript, this.historyPanel, composer);

    this.newChatButton.addEventListener('click', () => this.startNewChat());
    this.historyButton.addEventListener('click', () => void this.toggleHistory());
    this.suggestButton.addEventListener('click', () => void this.suggestFixes());
    composer.addEventListener('submit', (event) => {
      event.preventDefault();
      void this.submit();
    });
    this.textarea.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' && !event.shiftKey) {
        event.preventDefault();
        void this.submit();
      }
    });
    this.textarea.addEventListener('input', () => this.autoSizeTextarea());
    // The user leaving the bottom unlatches following; coming back re-latches it.
    this.transcript.addEventListener('scroll', () => {
      this.following = this.atBottom();
    });
  }

  private autoSizeTextarea(): void {
    this.textarea.style.height = 'auto';
    this.textarea.style.height = `${Math.min(this.textarea.scrollHeight, COMPOSER_MAX_PX)}px`;
  }

  private atBottom(): boolean {
    const { scrollTop, scrollHeight, clientHeight } = this.transcript;
    return scrollHeight - (scrollTop + clientHeight) <= NEAR_BOTTOM_PX;
  }

  /** Keep the newest content in view while the user is following the stream. */
  private follow(): void {
    if (this.following) this.transcript.scrollTop = this.transcript.scrollHeight;
  }

  private addBubble(kind: 'user' | 'assistant'): HTMLDivElement {
    const bubble = document.createElement('div');
    bubble.className = `bubble ${kind}`;
    this.transcript.append(bubble);
    this.follow();
    return bubble;
  }

  private setBusy(busy: boolean): void {
    this.busy = busy;
    this.sendButton.disabled = busy && !this.interruptible;
    this.sendButton.textContent = busy && this.interruptible ? 'Stop' : 'Send';
    this.sendButton.classList.toggle('stop', busy && this.interruptible);
    this.textarea.disabled = busy;
    this.newChatButton.disabled = busy;
    this.historyButton.disabled = busy;
    this.suggestButton.disabled = busy;
  }

  /** The session, created on first use; also what dials the backend. */
  private async ensureSession(): Promise<A11ChatSession> {
    if (!this.session) {
      // No config passed: the session reads the settings itself, before every
      // turn, so a provider or model changed mid-conversation takes effect on the
      // next message instead of on the next IDE restart.
      this.session = new A11ChatSession((run) => {
        this.active?.addToolRun(run);
        if (run.tool === 'request_user_input' && run.phase === 'started' && run.arguments) {
          this.pendingInput = {id: run.id, ...run.arguments};
          this.textarea.disabled = false;
          this.sendButton.disabled = false;
          this.sendButton.textContent = 'Send';
          this.sendButton.classList.remove('stop');
          this.textarea.placeholder = String(run.arguments.question ?? 'Type your answer…');
          this.textarea.focus();
        }
        if (
          run.tool === 'request_user_input' &&
          run.phase === 'finished' &&
          this.pendingInput?.id === run.id
        ) {
          this.pendingInput = null;
          this.textarea.placeholder = 'Ask A11 about your project...  (Enter to send, Shift+Enter for newline)';
          this.setBusy(this.busy);
        }
      }, (notice) => this.connection.update(notice));
    }
    return this.session;
  }

  private showHistory(open: boolean): void {
    this.historyOpen = open;
    this.root.classList.toggle('history-open', open);
    this.historyButton.classList.toggle('active', open);
  }

  /**
   * Start a fresh conversation.
   *
   * Public because an editor may offer this as a command of its own as well as a
   * button in the page -- VSCode's palette, a JetBrains action -- and both should
   * be the same action rather than two that drift.
   */
  newChat(): void {
    this.startNewChat();
  }

  private startNewChat(): void {
    if (this.busy) return;
    this.showHistory(false);
    this.session?.startNewConversation();
    this.transcript.innerHTML = '';
    this.following = true;
    this.textarea.focus();
  }

  /**
   * Open or close the history. The list is loaded on opening, not on mount:
   * loading it connects, and connecting is what starts the backend process.
   */
  private async toggleHistory(): Promise<void> {
    if (this.busy) return;
    if (this.historyOpen) {
      this.showHistory(false);
      return;
    }
    this.historyPanel.innerHTML = '';
    this.historyPanel.append(this.note('Loading...'));
    this.showHistory(true);
    try {
      const session = await this.ensureSession();
      this.renderHistory(await session.listConversations());
    } catch (error) {
      this.historyPanel.innerHTML = '';
      const message = this.note(error instanceof Error ? error.message : String(error));
      message.classList.add('error');
      this.historyPanel.append(message);
    }
  }

  private note(text: string): HTMLDivElement {
    const element = document.createElement('div');
    element.className = 'history-note';
    element.textContent = text;
    return element;
  }

  private renderHistory(conversations: ConversationSummary[]): void {
    this.historyPanel.innerHTML = '';
    if (conversations.length === 0) {
      this.historyPanel.append(this.note('No conversations yet.'));
      return;
    }
    const current = this.session?.conversationId;
    for (const conversation of conversations) {
      const item = document.createElement('button');
      item.type = 'button';
      item.className = 'history-item';
      if (conversation.id === current) item.classList.add('selected');
      const title = document.createElement('span');
      title.className = 'history-item-title';
      title.textContent = conversation.title;
      const when = document.createElement('span');
      when.className = 'history-item-date';
      when.textContent = formatWhen(conversation.started_at);
      item.append(title, when);
      item.addEventListener('click', () => void this.openConversation(conversation.id));
      this.historyPanel.append(item);
    }
  }

  private async openConversation(id: string): Promise<void> {
    if (this.busy) return;
    this.setBusy(true);
    try {
      const session = await this.ensureSession();
      const interactions = await session.loadConversation(id);
      this.transcript.innerHTML = '';
      await this.rehydrate(interactions);
      this.showHistory(false);
      this.following = true;
      this.follow();
      this.textarea.focus();
    } catch (error) {
      this.historyPanel.innerHTML = '';
      const message = this.note(error instanceof Error ? error.message : String(error));
      message.classList.add('error');
      this.historyPanel.append(message);
    } finally {
      this.setBusy(false);
    }
  }

  /**
   * Rebuild the transcript from a conversation's interactions.
   *
   * Backends record a tool round trip as an assistant interaction that made the
   * calls followed by a user-role one carrying their outputs, so the second is
   * skipped: the tool boxes under the assistant turn already stand for it.
   *
   * Those two interactions are also where the pieces of a tool box live apart —
   * the call's name on the first, its run log on the second — so the logs are
   * collected across the whole conversation first and matched by call id.
   */
  private async rehydrate(interactions: Interaction[]): Promise<void> {
    const logs: Record<string, string> = Object.assign({}, ...interactions.map(toolLogs));
    const statuses = Object.assign({}, ...interactions.map(toolStatuses));
    const outputs: Record<string, Record<string, unknown>> = Object.assign(
      {},
      ...(await Promise.all(interactions.map(toolOutputs))),
    );
    for (const interaction of interactions) {
      if (interaction.role === Role.SYSTEM) continue;
      const text = await interactionText(interaction);
      if (interaction.role === Role.ASSISTANT) {
        const bubble = new AssistantBubble(this.addBubble('assistant'), () => {});
        if (text) bubble.appendToken(text);
        const presented = await presentInteraction(interaction, logs, statuses);
        if (!isOk(presented)) throw new Error(presented.message);
        for (const block of presented.blocks) {
          if (block.kind !== BlockKind.TOOL_RUN) continue;
          bubble.addToolRun({
            id: block.id,
            tool: block.toolName,
            arguments: block.toolArguments,
            outputs: outputs[block.id],
            log: logs[block.id] ?? null,
            status: statuses[block.id],
            phase: 'finished',
          });
        }
        bubble.finish();
        continue;
      }
      if (isToolResultCarrier(interaction) || !text) continue;
      this.addBubble('user').textContent = text;
    }
  }

  private async submit(): Promise<void> {
    if (this.pendingInput) {
      const answer = this.textarea.value.trim();
      if (!answer) return;
      this.textarea.value = '';
      await this.answerPendingInput(this.pendingInput.id, answer);
      return;
    }
    if (this.busy) {
      if (this.interruptible && this.session?.interrupt()) {
        this.interruptible = false;
        this.sendButton.textContent = 'Stopping…';
        this.sendButton.classList.remove('stop');
        this.sendButton.disabled = true;
      }
      return;
    }
    const prompt = this.textarea.value.trim();
    if (!prompt) return;
    this.textarea.value = '';
    this.autoSizeTextarea();
    this.showHistory(false);
    this.interruptible = true;
    this.setBusy(true);
    // Sending is an explicit request to watch this turn.
    this.following = true;

    const userBubble = this.addBubble('user');
    userBubble.textContent = prompt;
    this.follow();

    const assistantElement = this.addBubble('assistant');
    const bubble = new AssistantBubble(
      assistantElement,
      () => this.follow(),
      (id, answer) => void this.answerPendingInput(id, answer),
    );
    this.active = bubble;

    try {
      const session = await this.ensureSession();
      const completed = await session.chat(prompt, {
        onToken: (text) => bubble.appendToken(text),
        onThought: (text) => bubble.appendThought(text),
      });
      if (!completed) bubble.appendToken('\n\n_Response stopped._');
      bubble.finish();
    } catch (error) {
      bubble.fail(error instanceof Error ? error.message : String(error));
    } finally {
      this.active = null;
      this.interruptible = false;
      this.setBusy(false);
      this.follow();
      this.textarea.focus();
    }
  }

  private async answerPendingInput(id: string, answer: string): Promise<void> {
    try {
      await (await this.ensureSession()).respondUserInput(id, answer);
      if (this.pendingInput?.id === id) {
        this.pendingInput = null;
        this.textarea.placeholder = 'Ask A11 about your project...  (Enter to send, Shift+Enter for newline)';
        this.setBusy(this.busy);
      }
    } catch (error) {
      this.active?.fail(error instanceof Error ? error.message : String(error));
    }
  }

  /**
   * Run the review flow: the file-wide paragraph into the transcript, and every
   * suggestion onto the range of the file it is about.
   *
   * There is no prompt to show, so the turn is one assistant bubble: the flow asks
   * the IDE what is open and what it underlines, asks the model to review it, and
   * the answers arrive as they are produced. Only the file-wide one is *about* the
   * transcript, though. A comment on line 42 read three scrolls away from line 42
   * is the wrong place for it, and a patch shown as a code block is an edit the
   * user has to make by hand -- so each suggestion goes back to the IDE, which
   * marks the range and shows the comment, a diff, and an Apply button in a popup
   * on the range itself.
   *
   * `comments` and `patches` are two ports of one flow and are read the same way,
   * into the same list. The split is what makes a suggestion appear in the editor
   * as a sentence first and gain its diff a moment later, rather than appearing
   * only once both are written; which of the two arrives first is the IDE's problem
   * and it merges them by `id`.
   *
   * What stays here is one line saying how many there are and where to find them,
   * because a run that annotated the editor and said nothing would look like a run
   * that did nothing.
   */
  private async suggestFixes(): Promise<void> {
    if (this.busy) return;
    this.showHistory(false);
    this.setBusy(true);
    // Pressing the button is an explicit request to watch what it does.
    this.following = true;

    const bubble = new AssistantBubble(this.addBubble('assistant'), () => this.follow());
    // Set so the flow's own IDE tool runs draw their boxes in this bubble.
    this.active = bubble;

    // This run replaces the last one's marks rather than adding to them: two
    // models' opinions on one warning, one of them about a file that has since
    // changed, is not twice the help.
    await clearSuggestions().catch(() => undefined);

    // The flow's output sinks are synchronous and the bridge call is not, so each
    // is collected and awaited before the summary is written -- otherwise the count
    // would be of the records *sent* rather than of the marks actually in the editor.
    const attaching: Promise<string | null>[] = [];
    try {
      const session = await this.ensureSession();
      await session.runFlow(SUGGEST_FLOW, {
        file_comment: (value) => {
          const text = String(value).trim();
          if (text) bubble.appendToken(`**What this file needs, taken together**\n\n${text}\n\n`);
        },
        comments: (value) => attaching.push(attachNote(value)),
        patches: (value) => attaching.push(attachNote(value)),
      });
      const keys = await Promise.all(attaching);
      // By suggestion and not by record: a comment and the patch that goes with it
      // are two values off two ports and one mark in the editor, so counting the
      // records would tell the user about twice as many places as there are.
      const marked = new Set(keys.filter((key): key is string => key !== null));
      bubble.appendToken(summarize(marked.size, keys.filter((key) => key === null).length));
      bubble.finish();
    } catch (error) {
      bubble.fail(error instanceof Error ? error.message : String(error));
    } finally {
      this.active = null;
      this.setBusy(false);
      this.follow();
    }
  }
}

/**
 * Hand one record to the IDE; the suggestion it belongs to when it took it, null
 * when it did not.
 *
 * The return value is what the caller counts, and it is a *suggestion* key rather
 * than a "yes": a comment and its patch are two records that mark one place, so
 * they return the same string and collapse in a set. A record the flow gave no id
 * is a suggestion of its own -- that is how the IDE treats it too -- so it gets a
 * key nothing else can share.
 *
 * A record the IDE refuses -- a path it cannot resolve, a range that is no longer
 * in the file -- is one mark missing, not a failed run: the other suggestions are
 * unaffected, and the flow's own work is already done. So it is counted out and
 * logged where a developer will see it, rather than thrown into the turn.
 */
async function attachNote(value: unknown): Promise<string | null> {
  const note = (value ?? {}) as Record<string, unknown>;
  const id = String(note.id ?? '').trim();
  try {
    await suggestOnHighlight({
      path: String(note.path ?? ''),
      id,
      origin: note.origin === 'found' ? 'found' : 'reported',
      comment: String(note.comment ?? ''),
      patch: String(note.patch ?? ''),
      start_line: Number(note.start_line ?? 0),
      start_column: Number(note.start_column ?? 0),
      end_line: Number(note.end_line ?? 0),
      end_column: Number(note.end_column ?? 0),
    });
    return id || `#${unkeyed++}`;
  } catch (error) {
    console.warn('A11 could not attach a suggestion', error);
    return null;
  }
}

/** Counter behind the key given to a record the flow left unkeyed. */
let unkeyed = 0;

/** The one line the transcript keeps: what landed in the editor, and how to see it. */
function summarize(marked: number, refused: number): string {
  if (marked === 0 && refused === 0) return '\n_Nothing in the file was worth a suggestion._\n';
  if (marked === 0) {
    return `\n_The model made ${refused} suggestion${refused === 1 ? '' : 's'},` +
      ' but none could be attached to the editor._\n';
  }
  const note = refused > 0 ? ` (${refused} could not be attached)` : '';
  return `\n_Marked ${marked} place${marked === 1 ? '' : 's'} in the editor${note}.` +
    ' Hover one for the comment and the fix._\n';
}

/**
 * When a conversation started, in the shortest form that still locates it: the
 * time for today, the date otherwise.
 */
function formatWhen(startedAtMillis: number): string {
  if (!startedAtMillis) return '';
  const when = new Date(startedAtMillis);
  const now = new Date();
  const sameDay =
    when.getFullYear() === now.getFullYear() &&
    when.getMonth() === now.getMonth() &&
    when.getDate() === now.getDate();
  return sameDay
    ? when.toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' })
    : when.toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
}

/** Mount the chat view into `root`. */
export function mountChat(root: HTMLElement): MountedView {
  const view = new ChatView(root);
  return {newChat: () => view.newChat()};
}

/**
 * Log levels, log-chunk metadata and the process-wide action log sink.
 *
 * The TypeScript twin of `cpp/a11/actions/log.h`. An {@link Action} narrates what
 * it is doing through `Action.log`, which turns the value it is handed into a
 * {@link Chunk} on the reserved {@link ACTION_LOG_OUTPUT} port; this module holds
 * the vocabulary everything that writes or reads such a chunk shares.
 */

import { Chunk } from './data.js';

/** The five severities every language A11 binds to agrees on, quietest first. */
export const LOG_LEVELS = ['debug', 'info', 'warning', 'error', 'critical'] as const;

/** One of {@link LOG_LEVELS}. */
export type LogLevel = (typeof LOG_LEVELS)[number];

/** The level a log written without one carries. */
export const DEFAULT_LOG_LEVEL: LogLevel = 'info';

/**
 * Chunk metadata attribute naming the log's level.
 *
 * The log attributes are unprefixed, unlike {@link CLOSE_STATUS_ATTRIBUTE},
 * because they only ever appear on the reserved log port: there is no user
 * metadata there to collide with.
 */
export const LOG_LEVEL_ATTRIBUTE = 'level';
/** Attribute marking a log as A11's own bookkeeping: `'true'` or `'false'`. */
export const LOG_INTERNAL_ATTRIBUTE = 'internal';
/** Attribute naming a free-form channel a consumer may filter on. */
export const LOG_CHANNEL_ATTRIBUTE = 'channel';
/** Attribute naming the source file the log came from. */
export const LOG_FILE_ATTRIBUTE = 'file';
/** Attribute naming the source line the log came from. */
export const LOG_LINENO_ATTRIBUTE = 'lineno';
/** The value {@link LOG_INTERNAL_ATTRIBUTE} takes when the log is internal. */
export const LOG_INTERNAL_TRUE = 'true';
/** The value it takes when the log is not. */
export const LOG_INTERNAL_FALSE = 'false';

/** Whether `name` is one of {@link LOG_LEVELS}, in either case. */
export function isLogLevel(name: string): boolean {
  return (LOG_LEVELS as readonly string[]).includes(name.toLowerCase());
}

/**
 * The canonical level `name` means, or `null` when it is not one.
 *
 * Accepts `warn` for `warning` and `fatal` for `critical` -- the two spellings
 * host languages differ on. An empty name is {@link DEFAULT_LOG_LEVEL}.
 */
export function parseLogLevel(name: string): LogLevel | null {
  if (name === '') return DEFAULT_LOG_LEVEL;
  const lowered = name.toLowerCase();
  if (lowered === 'warn') return 'warning';
  if (lowered === 'fatal') return 'critical';
  return (LOG_LEVELS as readonly string[]).includes(lowered)
    ? (lowered as LogLevel)
    : null;
}

/** Everything about a log other than the value being logged. */
export interface LogOptions {
  /** Level name; omitted is {@link DEFAULT_LOG_LEVEL}. */
  level?: string;
  /**
   * Media type hint for the serializer.
   *
   * Unlike C++, TypeScript distinguishes text from bytes already: a `string` is
   * `text/plain` and a `Uint8Array` is `application/octet-stream` through the
   * ordinary registry, so a log needs no special case here.
   */
  mimetype?: string;
  /** Logical channel; see {@link LOG_CHANNEL_ATTRIBUTE}. */
  channel?: string;
  /** Source file the log came from. */
  file?: string;
  /** Source line the log came from. */
  lineno?: number;
  /** Whether this is A11's own bookkeeping rather than a user-facing line. */
  internal?: boolean;
  /** Extra attributes, merged *before* the fields above. */
  metadata?: Readonly<Record<string, string | Uint8Array>> | ReadonlyMap<string, string | Uint8Array>;
}

/** One log as a sink sees it. */
export interface LogRecord {
  actionName: string;
  actionId: string;
  level: LogLevel;
  channel: string;
  file: string;
  lineno: number | null;
  internal: boolean;
  mimetype: string;
  data: Uint8Array;
  timestamp: Date | null;
}

/** What the process does with a log it consumes itself. */
export type ActionLogSink = (record: LogRecord) => void;

const decoder = new TextDecoder();

/**
 * Whether a log payload of this media type reads as a line of characters.
 *
 * Text and JSON do, so both print as themselves. Anything else is bytes, and a
 * log line is not the place to render a blob -- {@link logText} describes it
 * instead, and the payload is still on the record for a sink that wants it.
 */
export function isTextualLogMimetype(mimetype: string): boolean {
  if (mimetype.startsWith('text/')) return true;
  const media = mimetype.split(';', 1)[0];
  return media === 'application/json' || media.endsWith('+json');
}

/** A record as one line: its payload where that is text, a description if not. */
export function logText(record: LogRecord): string {
  return isTextualLogMimetype(record.mimetype)
    ? decoder.decode(record.data)
    : `<${record.data.byteLength} bytes of ${record.mimetype}>`;
}

/** The default sink: the console, at the method the level names. */
function reportThroughConsole(record: LogRecord): void {
  const where = record.channel === ''
    ? `[${record.actionName}]`
    : `[${record.actionName}/${record.channel}]`;
  const line = `${where} ${logText(record)}`;
  switch (record.level) {
    case 'debug':
      console.debug(line);
      return;
    case 'warning':
      console.warn(line);
      return;
    case 'error':
    case 'critical':
      console.error(line);
      return;
    default:
      console.info(line);
  }
}

let sink: ActionLogSink | null = null;

/**
 * Install `next` as the process's action log sink.
 *
 * One slot rather than one sink per interested party, so a caller that takes it
 * takes it from whoever had it -- which is what keeps a log from being reported
 * twice. `null` restores the console default.
 */
export function setActionLogSink(next: ActionLogSink | null): void {
  sink = next;
}

/** The installed sink, or the default. Never null. */
export function getActionLogSink(): ActionLogSink {
  return sink ?? reportThroughConsole;
}

/** Report `record` to the installed sink. Never throws. */
export function reportLog(record: LogRecord): void {
  try {
    getActionLogSink()(record);
  } catch {
    // A failure to log must never reach the code that was logging.
  }
}

function attribute(chunk: Chunk, key: string): string {
  const value = chunk.metadata?.attributes.get(key);
  return value === undefined ? '' : decoder.decode(value);
}

/**
 * Read a {@link LogRecord} back out of a log chunk.
 *
 * The inverse of what `Action.log` writes, so a consumer on the far end of a wire
 * reads the metadata the same way every other language does. A missing or
 * unknown level falls back to {@link DEFAULT_LOG_LEVEL}.
 */
export function logRecordFromChunk(
  chunk: Chunk,
  actionName = '',
  actionId = '',
): LogRecord {
  const lineno = Number.parseInt(attribute(chunk, LOG_LINENO_ATTRIBUTE), 10);
  return {
    actionName,
    actionId,
    level: parseLogLevel(attribute(chunk, LOG_LEVEL_ATTRIBUTE)) ?? DEFAULT_LOG_LEVEL,
    channel: attribute(chunk, LOG_CHANNEL_ATTRIBUTE),
    file: attribute(chunk, LOG_FILE_ATTRIBUTE),
    lineno: Number.isNaN(lineno) ? null : lineno,
    internal: attribute(chunk, LOG_INTERNAL_ATTRIBUTE) === LOG_INTERNAL_TRUE,
    mimetype: chunk.mimetype,
    data: chunk.data,
    timestamp: chunk.metadata?.timestamp ?? null,
  };
}

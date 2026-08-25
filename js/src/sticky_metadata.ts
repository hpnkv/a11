import { cloneChunk } from './chunk_store.js';
import { Chunk, ChunkMetadata, NodeFragment, WireMessage } from './data.js';

/**
 * Marks a frame whose fragment mimetypes were elided by the sender.
 *
 * Present only when something was actually elided, so a frame that gained
 * nothing carries no header either. A receiver expands whenever it sees this,
 * whatever its own options say: the sender has already dropped the bytes, and
 * the alternative to expanding is delivering chunks with a mimetype the
 * application never wrote.
 */
export const STICKY_METADATA_HEADER = 'x-a11-sticky-metadata';

const STICKY_MARKER = new Uint8Array([1]);

/** What a node's preceding fragment established, for the next one to inherit. */
interface StickyRun {
  /** The mimetype in force, as the receiver will see it after expansion. */
  mimetype: string;
  /** The sequence a contiguous successor must carry, or null if unsequenced. */
  nextSeq: number | null;
}

/**
 * Decide, for one fragment, what it may inherit and what it leaves behind.
 *
 * The single place the rule lives, so the two directions cannot drift: eliding
 * something the other side would not put back is silent data loss, and the only
 * way to be sure they agree is to have them ask the same function.
 *
 * The rule is the {@link ChunkStoreWriter} `stickyMimetype` rule: a run
 * continues only while a node's sequence numbers stay contiguous, because a gap
 * means the receiver may never have seen the fragment that established the
 * mimetype. A payload that is a node reference rather than a chunk carries no
 * mimetype and so ends the run it sits in.
 */
function considerFragment(
  runs: Map<string, StickyRun>,
  fragment: NodeFragment,
): { inherited: string; own: string; contiguous: boolean } {
  const run = runs.get(fragment.id);
  const contiguous =
    run !== undefined &&
    run.nextSeq !== null &&
    fragment.seq !== null &&
    fragment.seq === run.nextSeq;
  const own = fragment.data instanceof Chunk ? fragment.data.mimetype : '';
  const inherited = contiguous ? (run?.mimetype ?? '') : '';
  // What the next fragment on this node sees: this fragment's own mimetype, or
  // the inherited one when it has none of its own.
  runs.set(fragment.id, {
    mimetype: own !== '' ? own : inherited,
    nextSeq: fragment.seq === null ? null : fragment.seq + 1,
  });
  return { inherited, own, contiguous };
}

/**
 * Omit a fragment's mimetype when the fragment before it on the same node
 * already carried the same one.
 *
 * Elision is per node id because fragments for different nodes interleave
 * freely inside one frame -- especially after the sender folds several messages
 * together, which is what makes this worth doing at all.
 *
 * Returns the message unchanged when there was nothing to elide, so a frame that
 * cannot benefit allocates nothing and carries no marker.
 */
export function elideStickyMetadata(message: WireMessage): WireMessage {
  if (message.nodeFragments.length < 2) return message;
  const runs = new Map<string, StickyRun>();
  const fragments: NodeFragment[] = [];
  let elided = false;
  for (const fragment of message.nodeFragments) {
    const { inherited, own } = considerFragment(runs, fragment);
    if (own === '' || own !== inherited || !(fragment.data instanceof Chunk)) {
      fragments.push(fragment);
      continue;
    }
    // Cloned rather than edited: the caller still owns this chunk, and a
    // WireMessage the application holds must not lose its mimetype because the
    // transport found a cheaper way to send it.
    const stripped = cloneChunk(fragment.data);
    if (stripped.metadata !== null) {
      stripped.metadata.mimetype = '';
      if (stripped.metadata.timestamp === null && stripped.metadata.attributes.size === 0) {
        stripped.metadata = null;
      }
    }
    fragments.push(new NodeFragment({
      id: fragment.id,
      data: stripped,
      seq: fragment.seq,
      continued: fragment.continued,
    }));
    elided = true;
  }
  if (!elided) return message;
  const headers = new Map(message.headers);
  headers.set(STICKY_METADATA_HEADER, STICKY_MARKER);
  return new WireMessage({ nodeFragments: fragments, actions: message.actions, headers });
}

/**
 * Put back the mimetypes {@link elideStickyMetadata} left out.
 *
 * Mutates in place: the message was decoded from the wire moments ago and has
 * no other owner, and a copy per frame would give back what the elision saved.
 * A frame without the header is left entirely alone, which is what makes this
 * safe to run unconditionally on every inbound message.
 */
export function expandStickyMetadata(message: WireMessage): void {
  if (!message.headers.has(STICKY_METADATA_HEADER)) return;
  message.headers.delete(STICKY_METADATA_HEADER);
  const runs = new Map<string, StickyRun>();
  for (const fragment of message.nodeFragments) {
    const { inherited, own } = considerFragment(runs, fragment);
    if (own !== '' || inherited === '' || !(fragment.data instanceof Chunk)) continue;
    if (fragment.data.metadata === null) fragment.data.metadata = new ChunkMetadata();
    fragment.data.metadata.mimetype = inherited;
  }
}

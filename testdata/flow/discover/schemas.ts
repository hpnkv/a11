// Copyright 2026 The A11 Authors.
//
// What the TypeScript side of the ActionSchema scanner is pinned against. Not a
// module anybody imports: every declaration here is a shape
// `cpp/a11/flow/discover.cc` has to read, including the ones it is meant to read
// badly.

import {ActionPortSchema, ActionSchema} from '../../../js/src/index.js';

/** A module constant, as a real schema names a shared port. */
const NARRATION_PORT = 'narration';

/** The name of the action below, bound rather than written inline. */
const NAMED_ELSEWHERE = 'ts-reads-its-name-from-a-constant';

// The ordinary shape: one object argument, properties keyed with `:`.
export const SIMPLE = new ActionSchema({
  name: 'ts-simple',
  description: 'Return the supplied text unchanged.',
  inputs: {
    text: new ActionPortSchema({
      name: 'text',
      type: 'text/plain',
      unary: true,
      required: true,
    }),
  },
  outputs: {out: new ActionPortSchema({name: 'out', type: 'text/plain'})},
});

// A description joined with `+`, which is how TypeScript writes prose that
// outgrew its line, and a backtick literal with no interpolation in it.
export const PROSE = new ActionSchema({
  name: 'ts-prose',
  description:
    'A description that outgrew its line, written as two literals joined ' +
    'with a plus, which is one string.',
  inputs: {
    question: new ActionPortSchema({
      name: 'question',
      type: 'text/plain',
      description: `What to find out.`,
      required: true,
    }),
  },
  outputs: {
    answer: new ActionPortSchema({name: 'answer', type: 'text/plain'}),
    [NARRATION_PORT]: new ActionPortSchema({
      name: NARRATION_PORT,
      type: 'text/plain',
    }),
  },
});

// A name that is a constant of this file.
export const FROM_CONSTANT = new ActionSchema({
  name: NAMED_ELSEWHERE,
  description: 'Names itself with a constant declared above.',
});

// A name built at run time: not findable, and the test says so.
export function computed(suffix: string): ActionSchema {
  return new ActionSchema({
    name: `ts-computed-${suffix}`,
    description: 'Not findable.',
  });
}

/* A mention in a block comment is not a declaration:
   new ActionSchema({name: 'ts-in-a-comment', description: 'Not real.'}) */

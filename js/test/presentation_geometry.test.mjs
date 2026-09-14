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

import assert from "node:assert/strict";
import test from "node:test";

import {
  FollowTailController,
  HoverIntentController,
  STUDIO_WIRE_HOVER,
  concentricRadii,
  concentricRadiusExpression,
  listEdge,
  listEdgeRadii,
  placeOverlay,
  uniformInsets,
  uniformRadii,
} from "../dist/presentation/index.js";
import { isOk } from "../dist/index.js";

test("concentric radii subtract the adjacent inset and border per axis", () => {
  assert.deepEqual(
    concentricRadii(
      {
        topLeft: { x: 24, y: 20 },
        topRight: { x: 18, y: 20 },
        bottomRight: { x: 18, y: 16 },
        bottomLeft: { x: 24, y: 16 },
      },
      { top: 5, right: 6, bottom: 7, left: 8 },
      uniformInsets(1),
    ),
    {
      topLeft: { x: 15, y: 14 },
      topRight: { x: 11, y: 14 },
      bottomRight: { x: 11, y: 8 },
      bottomLeft: { x: 15, y: 8 },
    },
  );
  assert.deepEqual(
    concentricRadii(uniformRadii(4), uniformInsets(9)),
    uniformRadii(0),
  );
  assert.equal(
    concentricRadiusExpression("var(--panel-radius)", "var(--panel-inset)", 1),
    "max(0px, calc(var(--panel-radius) - var(--panel-inset) - 1px))",
  );
});

test("list edges handle first, middle, last, and a single item", () => {
  assert.deepEqual(
    [0, 1, 2].map((index) => listEdge(index, 3)),
    ["first", "middle", "last"],
  );
  assert.equal(listEdge(0, 1), "single");
  const outside = uniformRadii(12);
  const middle = uniformRadii(2);
  assert.deepEqual(listEdgeRadii(0, 1, outside, middle), outside);
  assert.deepEqual(listEdgeRadii(1, 3, outside, middle), middle);
});

test("overlay placement flips and clamps inside the viewport", () => {
  const viewport = {
    left: 0,
    top: 0,
    right: 320,
    bottom: 240,
    width: 320,
    height: 240,
  };
  assert.deepEqual(
    placeOverlay(
      { left: 270, top: 200, right: 300, bottom: 220, width: 30, height: 20 },
      { width: 120, height: 80 },
      viewport,
      { preferred: ["bottom", "top"], gap: 8, margin: 12 },
    ),
    {
      left: 188,
      top: 112,
      side: "top",
      maxWidth: 120,
      maxHeight: 80,
      transformOrigin: "bottom left",
    },
  );
});

test("hover intent preserves a card across handoff and delays switching", () => {
  let next = 0;
  const callbacks = new Map();
  const timer = {
    set(callback) {
      next += 1;
      callbacks.set(next, callback);
      return next;
    },
    clear(handle) {
      callbacks.delete(handle);
    },
  };
  const hover = new HoverIntentController(STUDIO_WIRE_HOVER, timer);
  hover.enterAnchor("one");
  assert.equal(hover.getSnapshot().active, "one");
  hover.leaveAnchor();
  hover.enterOverlay();
  for (const callback of callbacks.values()) callback();
  assert.equal(hover.getSnapshot().active, "one");
  hover.leaveOverlay();
  for (const callback of [...callbacks.values()]) callback();
  assert.equal(hover.getSnapshot().active, null);
  hover.enterAnchor("one");
  hover.enterAnchor("two");
  assert.equal(hover.getSnapshot().active, "one");
  for (const callback of [...callbacks.values()]) callback();
  assert.equal(hover.getSnapshot().active, "two");
});

test("follow-tail repins when the reader returns to the end", () => {
  const follow = new FollowTailController(24);
  assert.equal(
    follow.measure({ scrollHeight: 1000, scrollTop: 800, clientHeight: 100 }),
    false,
  );
  assert.equal(follow.shouldFollow(), false);
  assert.equal(
    follow.measure({ scrollHeight: 1000, scrollTop: 877, clientHeight: 100 }),
    true,
  );
  assert.equal(follow.shouldFollow(), true);
});

test("hover subscriber failures are statuses rather than exceptions", () => {
  const hover = new HoverIntentController(STUDIO_WIRE_HOVER);
  hover.subscribe(() => {
    throw new Error("detached renderer");
  });
  const result = hover.enterAnchor("one");
  assert.equal(isOk(result), false);
  assert.match(result.message, /subscriber/i);
});

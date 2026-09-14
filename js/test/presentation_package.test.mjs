/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import assert from "node:assert/strict";
import test from "node:test";

test("the base presentation subpath imports without React or browser globals", async () => {
  const presentation = await import("@curiositystack/a11/presentation");
  assert.equal(typeof presentation.FlowRunnerController, "function");
  assert.equal(typeof presentation.presentValue, "function");
  assert.equal(typeof presentation.concentricRadii, "function");
});

test("the opt-in browser subpath has no import-time DOM side effects", async () => {
  const browser = await import("@curiositystack/a11/presentation/browser");
  assert.equal(typeof browser.observeOverlay, "function");
});

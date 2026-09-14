/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

import { isOk, noexcept, type Status } from "../status.js";

/**
 * Consume values and preserve the iterator's terminal Status.
 *
 * Flow and action adapters use this pattern so failures terminate iteration as
 * data instead of rejecting the generator.
 */
export async function consumeStatusIterator<T>(
  iterator: AsyncIterator<T, Status>,
  consume: (value: T) => Status | Promise<Status>,
): Promise<Status> {
  for (;;) {
    const next = await noexcept(
      () => iterator.next(),
      "Could not read the next streamed presentation value.",
    );
    if (!isOk(next)) return next;
    if (next.done) return next.value;
    const consumed = await noexcept(
      () => consume(next.value),
      "Could not consume a streamed presentation value.",
    );
    if (!isOk(consumed)) return consumed;
  }
}

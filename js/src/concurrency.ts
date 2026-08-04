/**
 * Copyright 2026 The Action Engine Authors.
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

import {
  cancelledError,
  deadlineExceededError,
  failedPreconditionError,
  internalError,
  invalidArgumentError,
  isOk,
  isStatus,
  okStatus,
  outOfRangeError,
  resourceExhaustedError,
  statusFromUnknown,
  type NonOkStatus,
  type Status,
  type StatusOr,
} from './status.js';

class Waiter {
  private readonly promise: Promise<Status>;
  private readonly waiters: Set<Waiter>;
  private resolveInternal!: (value: Status) => void;
  private timeout: ReturnType<typeof setTimeout> | null = null;
  private timedOut = false;
  private settled = false;

  constructor(waiters: Set<Waiter>) {
    this.waiters = waiters;
    this.promise = new Promise((resolve) => {
      this.resolveInternal = resolve;
    });
  }

  async wait(
    timeout: number = -1,
    mutex: Mutex | null = null,
  ): Promise<StatusOr<boolean>> {
    try {
      if (!Number.isFinite(timeout) || timeout < -1) {
        return invalidArgumentError('Wait timeout must be -1 or a non-negative finite number.');
      }
      if (mutex !== null && !mutex.isLocked()) {
        return failedPreconditionError('Mutex must be locked before a condition wait.');
      }
      if (timeout >= 0) {
        this.timeout = setTimeout(() => {
          this.timedOut = true;
          this.resolve(okStatus());
        }, timeout);
      }

      this.waiters.add(this);
      let released = false;
      if (mutex !== null) {
        const status = mutex.release();
        if (!isOk(status)) return status;
        released = true;
      }
      const status = await this.promise;
      if (released) {
        const reacquired = await mutex!.acquire();
        if (!isOk(reacquired)) return reacquired;
      }
      return isOk(status) ? this.timedOut : status;
    } catch (error) {
      return statusFromUnknown(error, 'Waiting on a concurrency primitive raised an exception.');
    } finally {
      this.waiters.delete(this);
      if (this.timeout !== null) {
        clearTimeout(this.timeout);
        this.timeout = null;
      }
    }
  }

  cancel(status: NonOkStatus = cancelledError('Waiter was cancelled.')): Status {
    if (!isStatus(status) || isOk(status)) {
      return invalidArgumentError('Waiter cancellation requires a non-OK Status.');
    }
    return this.resolve(status);
  }

  notify(): Status {
    return this.resolve(okStatus());
  }

  private resolve(status: Status): Status {
    try {
      if (this.settled) return okStatus();
      this.settled = true;
      this.waiters.delete(this);
      if (this.timeout !== null) {
        clearTimeout(this.timeout);
        this.timeout = null;
      }
      this.resolveInternal(status);
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Resolving a concurrency waiter raised an exception.');
    }
  }
}

/** Cooperative FIFO mutex for JavaScript async state machines. */
export class Mutex {
  private locked = false;
  private readonly waiters = new Set<Waiter>();

  async acquire(): Promise<Status> {
    try {
      while (this.locked) {
        const waited = await new Waiter(this.waiters).wait();
        if (!isOk(waited)) return waited;
      }
      this.locked = true;
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Acquiring Mutex raised an exception.');
    }
  }

  async runExclusive<T>(
    callback: () => T | Promise<T>,
  ): Promise<StatusOr<T>> {
    if (typeof callback !== 'function') {
      return invalidArgumentError('Mutex callback must be callable.');
    }
    const acquired = await this.acquire();
    if (!isOk(acquired)) return acquired;
    let result: StatusOr<T>;
    try {
      result = await callback();
    } catch (error) {
      result = statusFromUnknown(error, 'Mutex callback raised an exception.');
    }
    const released = this.release();
    return isOk(released) ? result : released;
  }

  isLocked(): boolean {
    return this.locked;
  }

  async waitForUnlock(): Promise<Status> {
    try {
      while (this.locked) {
        const waited = await new Waiter(this.waiters).wait();
        if (!isOk(waited)) return waited;
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, 'Waiting for Mutex unlock raised an exception.');
    }
  }

  release(): Status {
    try {
      if (!this.locked) return failedPreconditionError('Mutex is not locked.');
      this.locked = false;
      const waiter = this.waiters.values().next().value;
      return waiter === undefined ? okStatus() : waiter.notify();
    } catch (error) {
      return statusFromUnknown(error, 'Releasing Mutex raised an exception.');
    }
  }

  cancel(status: NonOkStatus = cancelledError('Mutex waiters were cancelled.')): Status {
    if (!isStatus(status) || isOk(status)) {
      return invalidArgumentError('Mutex cancellation requires a non-OK Status.');
    }
    let first: Status = okStatus();
    for (const waiter of this.waiters) {
      const cancelled = waiter.cancel(status);
      if (isOk(first) && !isOk(cancelled)) first = cancelled;
    }
    this.waiters.clear();
    return first;
  }
}

/** Condition variable paired with a locked Mutex. */
export class CondVar {
  private readonly waiters = new Set<Waiter>();

  async wait(mutex: Mutex): Promise<Status> {
    if (!(mutex instanceof Mutex)) return invalidArgumentError('mutex must be a Mutex.');
    const waited = await new Waiter(this.waiters).wait(-1, mutex);
    return isOk(waited) ? okStatus() : waited;
  }

  async waitWithTimeout(
    mutex: Mutex,
    timeout: number,
  ): Promise<StatusOr<boolean>> {
    if (!(mutex instanceof Mutex)) return invalidArgumentError('mutex must be a Mutex.');
    return new Waiter(this.waiters).wait(timeout, mutex);
  }

  async waitWithDeadline(
    mutex: Mutex,
    deadline: DOMHighResTimeStamp,
  ): Promise<StatusOr<boolean>> {
    try {
      if (!Number.isFinite(deadline)) {
        return invalidArgumentError('Condition deadline must be finite.');
      }
      return this.waitWithTimeout(mutex, Math.max(0, deadline - performance.now()));
    } catch (error) {
      return statusFromUnknown(error, 'Reading condition deadline raised an exception.');
    }
  }

  notifyOne(): Status {
    try {
      const waiter = this.waiters.values().next().value;
      return waiter === undefined ? okStatus() : waiter.notify();
    } catch (error) {
      return statusFromUnknown(error, 'Notifying condition waiter raised an exception.');
    }
  }

  notifyAll(): Status {
    let first: Status = okStatus();
    try {
      for (const waiter of this.waiters) {
        const notified = waiter.notify();
        if (isOk(first) && !isOk(notified)) first = notified;
      }
      return first;
    } catch (error) {
      return statusFromUnknown(error, 'Notifying condition waiters raised an exception.');
    }
  }
}

/** Fixed-capacity double-ended queue with Status-returning bounds checks. */
export class Deque<ValueType> {
  readonly data: Array<ValueType | undefined>;
  head = 0;
  tail = 0;
  readonly capacity: number;
  size = 0;
  private readonly constructionStatus: Status;

  constructor(capacity: number = 65_536) {
    if (!Number.isSafeInteger(capacity) || capacity <= 0 || capacity > 0xffff_ffff) {
      this.capacity = 1;
      this.data = new Array(1);
      this.constructionStatus = invalidArgumentError(
        'Deque capacity must be a positive uint32 integer.',
      );
      return;
    }
    this.capacity = capacity;
    try {
      this.data = new Array(capacity);
      this.constructionStatus = okStatus();
    } catch (error) {
      this.data = [];
      this.constructionStatus = resourceExhaustedError(
        'Could not allocate Deque storage.',
        [],
        error,
      );
    }
  }

  getStatus(): Status { return this.constructionStatus; }

  addFront(value: ValueType): Status {
    if (!isOk(this.constructionStatus)) return this.constructionStatus;
    if (this.size >= this.capacity) return resourceExhaustedError('Deque capacity exceeded.');
    this.head = (this.head - 1 + this.capacity) % this.capacity;
    this.data[this.head] = value;
    ++this.size;
    return okStatus();
  }

  removeFront(): StatusOr<ValueType> {
    if (!isOk(this.constructionStatus)) return this.constructionStatus;
    if (this.size === 0) return outOfRangeError('Deque is empty.');
    const value = this.data[this.head]!;
    this.data[this.head] = undefined;
    this.head = (this.head + 1) % this.capacity;
    --this.size;
    return value;
  }

  peekFront(): StatusOr<ValueType | null> {
    if (!isOk(this.constructionStatus)) return this.constructionStatus;
    return this.size === 0 ? null : this.data[this.head]!;
  }

  addBack(value: ValueType): Status {
    if (!isOk(this.constructionStatus)) return this.constructionStatus;
    if (this.size >= this.capacity) return resourceExhaustedError('Deque capacity exceeded.');
    this.data[this.tail] = value;
    this.tail = (this.tail + 1) % this.capacity;
    ++this.size;
    return okStatus();
  }

  removeBack(): StatusOr<ValueType> {
    if (!isOk(this.constructionStatus)) return this.constructionStatus;
    if (this.size === 0) return outOfRangeError('Deque is empty.');
    this.tail = (this.tail - 1 + this.capacity) % this.capacity;
    const value = this.data[this.tail]!;
    this.data[this.tail] = undefined;
    --this.size;
    return value;
  }

  peekBack(): StatusOr<ValueType | null> {
    if (!isOk(this.constructionStatus)) return this.constructionStatus;
    return this.size === 0
      ? null
      : this.data[(this.tail - 1 + this.capacity) % this.capacity]!;
  }
}

/** Bounded cooperative channel whose promises always settle to StatusOr. */
export class Channel<ValueType> {
  private readonly deque: Deque<ValueType>;
  private readonly nBufferedMessages: number;
  private readonly mutex = new Mutex();
  private readonly cv = new CondVar();
  private closed = false;
  private readonly constructionStatus: Status;

  constructor(nBufferedMessages: number = 100) {
    if (!Number.isSafeInteger(nBufferedMessages) || nBufferedMessages <= 0) {
      this.nBufferedMessages = 1;
      this.deque = new Deque<ValueType>(1);
      this.constructionStatus = invalidArgumentError(
        'Channel buffer size must be a positive integer.',
      );
      return;
    }
    this.nBufferedMessages = nBufferedMessages;
    this.deque = new Deque<ValueType>(Math.max(nBufferedMessages, 100));
    this.constructionStatus = this.deque.getStatus();
  }

  getStatus(): Status { return this.constructionStatus; }

  async send(value: ValueType): Promise<Status> {
    if (!isOk(this.constructionStatus)) return this.constructionStatus;
    const result = await this.mutex.runExclusive<Status>(async () => {
      if (this.closed) return failedPreconditionError('Channel is closed.');
      while (!this.closed && this.deque.size >= this.nBufferedMessages) {
        const waited = await this.cv.wait(this.mutex);
        if (!isOk(waited)) return waited;
      }
      if (this.closed) return failedPreconditionError('Channel is closed.');
      const added = this.deque.addBack(value);
      if (!isOk(added)) return added;
      const notified = this.cv.notifyAll();
      return isOk(notified) ? okStatus() : notified;
    });
    return isStatus(result)
      ? result
      : internalError('Channel send returned an invalid Status.');
  }

  async sendNowait(value: ValueType): Promise<Status> {
    if (!isOk(this.constructionStatus)) return this.constructionStatus;
    const result = await this.mutex.runExclusive<Status>(() => {
      if (this.closed) return failedPreconditionError('Channel is closed.');
      if (this.deque.size >= this.nBufferedMessages) {
        return resourceExhaustedError('Channel buffer is full.');
      }
      const added = this.deque.addBack(value);
      if (!isOk(added)) return added;
      const notified = this.cv.notifyAll();
      return isOk(notified) ? okStatus() : notified;
    });
    return isStatus(result)
      ? result
      : internalError('Channel sendNowait returned an invalid Status.');
  }

  async receive(timeout: number = -1): Promise<StatusOr<ValueType>> {
    if (!isOk(this.constructionStatus)) return this.constructionStatus;
    return this.mutex.runExclusive<StatusOr<ValueType>>(async () => {
      while (!this.closed && this.deque.size === 0) {
        const waited = await this.cv.waitWithTimeout(this.mutex, timeout);
        if (!isOk(waited)) return waited;
        if (waited) {
          return deadlineExceededError('Timed out waiting for a Channel value.');
        }
      }
      if (this.closed && this.deque.size === 0) {
        return failedPreconditionError('Channel is closed.');
      }
      const value = this.deque.removeFront();
      if (!isOk(value)) return value;
      const notified = this.cv.notifyAll();
      return isOk(notified) ? value : notified;
    });
  }

  async close(): Promise<Status> {
    const result = await this.mutex.runExclusive<Status>(() => {
      this.closed = true;
      const notified = this.cv.notifyAll();
      return isOk(notified) ? okStatus() : notified;
    });
    return isStatus(result)
      ? result
      : internalError('Channel close returned an invalid Status.');
  }

  async isClosed(): Promise<StatusOr<boolean>> {
    return this.mutex.runExclusive(() => this.closed);
  }
}

/** Yield to the host event loop for at least `ms` milliseconds. */
export async function sleep(ms: number): Promise<Status> {
  if (!Number.isFinite(ms) || ms < 0) {
    return invalidArgumentError('Sleep duration must be non-negative and finite.');
  }
  try {
    await new Promise<void>((resolve) => setTimeout(resolve, ms));
    return okStatus();
  } catch (error) {
    return statusFromUnknown(error, 'Sleep raised an exception.');
  }
}

/** Sleep until an absolute JavaScript epoch-millisecond deadline. */
export async function sleepUntil(time: number): Promise<Status> {
  if (!Number.isFinite(time)) {
    return invalidArgumentError('Sleep deadline must be a finite epoch millisecond value.');
  }
  return sleep(Math.max(0, time - Date.now()));
}

/** A small externally-completable promise used by the stackless pumps. */
export class Deferred<T> {
  readonly promise: Promise<T>;
  private resolveInternal!: (value: T | PromiseLike<T>) => void;
  private settledInternal = false;

  constructor() {
    this.promise = new Promise<T>((resolve) => {
      this.resolveInternal = resolve;
    });
  }

  get settled(): boolean {
    return this.settledInternal;
  }

  resolve(value: T): Status {
    if (this.settledInternal) return okStatus();
    this.settledInternal = true;
    this.resolveInternal(value);
    return okStatus();
  }
}

/** One non-throwing unit of work for {@link CallbackScheduler}. */
export type ScheduledCallback = () => Status | Promise<Status>;

/**
 * Fair, bounded, stackless callback scheduler shared by reader/writer state
 * machines. A callback runs at most once per queue entry and failures are
 * reported through `onError` instead of escaping as unhandled rejections.
 */
export class CallbackScheduler {
  private readonly queue: Array<{
    callback: ScheduledCallback;
    onError?: (status: Status) => void;
  }> = [];
  private draining = false;

  constructor(private readonly callbacksPerTurn: number = 64) {}

  schedule(
    callback: ScheduledCallback,
    onError?: (status: Status) => void,
  ): Status {
    if (typeof callback !== 'function') {
      return statusFromUnknown(
        new TypeError('Scheduled callback must be callable.'),
      );
    }
    this.queue.push({ callback, onError });
    if (!this.draining) {
      this.draining = true;
      queueMicrotask(() => void this.drain());
    }
    return okStatus();
  }

  get pending(): number {
    return this.queue.length;
  }

  private async drain(): Promise<void> {
    let processed = 0;
    while (processed < this.callbacksPerTurn) {
      const entry = this.queue.shift();
      if (entry === undefined) break;
      let failure: Status | null = null;
      try {
        const status = await entry.callback();
        failure = !isStatus(status)
          ? internalError('Scheduled callback returned a non-Status value.')
          : isOk(status)
            ? null
            : status;
      } catch (error) {
        failure = statusFromUnknown(error);
      }
      if (failure !== null && entry.onError !== undefined) {
        try { entry.onError(failure); }
        catch { /* A scheduler error handler must not reject the pump. */ }
      }
      ++processed;
    }
    if (this.queue.length === 0) {
      this.draining = false;
      return;
    }
    queueMicrotask(() => void this.drain());
  }
}

/** Shared fair pump used by high-cardinality chunk readers and writers. */
export const storeCallbackScheduler = new CallbackScheduler();

/** Await a promise with an optional millisecond timeout, returning a Status. */
export async function waitFor<T>(
  promise: Promise<T>,
  timeoutMs?: number,
  timeoutMessage: string = 'Operation exceeded its deadline.',
): Promise<StatusOr<T>> {
  if (timeoutMs !== undefined && (!Number.isFinite(timeoutMs) || timeoutMs < 0)) {
    return deadlineExceededError(timeoutMessage);
  }
  if (timeoutMs === undefined) {
    try {
      return await promise;
    } catch (error) {
      return statusFromUnknown(error);
    }
  }
  const timeout = new Deferred<StatusOr<T>>();
  const timer = setTimeout(
    () => timeout.resolve(deadlineExceededError(timeoutMessage)),
    timeoutMs,
  );
  try {
    return await Promise.race([
      promise
        .then<StatusOr<T>>((value) => value)
        .catch((error: unknown) => statusFromUnknown(error)),
      timeout.promise,
    ]);
  } finally {
    clearTimeout(timer);
  }
}

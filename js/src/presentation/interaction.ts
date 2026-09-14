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

import { isOk, okStatus, statusFromUnknown, type Status } from "../status.js";

export interface Timer {
  set(callback: () => void, delayMs: number): unknown;
  clear(handle: unknown): void;
}

const defaultTimer: Timer = {
  set: (callback, delayMs) => globalThis.setTimeout(callback, delayMs),
  clear: (handle) =>
    globalThis.clearTimeout(handle as ReturnType<typeof setTimeout>),
};

export interface HoverPolicy {
  openMs: number;
  switchMs: number;
  handoffMs: number;
}

/** Studio's wire-preview timings. */
export const STUDIO_WIRE_HOVER: Readonly<HoverPolicy> = Object.freeze({
  openMs: 0,
  switchMs: 350,
  handoffMs: 500,
});

/** Studio's Flow-symbol hover timings and cache bound. */
export const STUDIO_FLOW_HOVER = Object.freeze({
  openMs: 250,
  switchMs: 250,
  handoffMs: 500,
  progressMs: 150,
  cacheEntries: 128,
});

export interface HoverSnapshot<T> {
  active: T | null;
  pending: T | null;
  anchorHeld: boolean;
  overlayHeld: boolean;
}

/**
 * Hover/focus intent shared by anchors and their detached overlay.
 *
 * Leaving an anchor starts a handoff window rather than closing immediately.
 * Crossing the gap into a body-level overlay therefore keeps it interactive.
 * Moving over a second anchor waits before replacing an already readable card.
 */
export class HoverIntentController<T> {
  private snapshot: HoverSnapshot<T> = {
    active: null,
    pending: null,
    anchorHeld: false,
    overlayHeld: false,
  };
  private readonly listeners = new Set<() => void>();
  private opening: unknown;
  private closing: unknown;

  constructor(
    private readonly policy: HoverPolicy,
    private readonly timer: Timer = defaultTimer,
  ) {}

  getSnapshot(): HoverSnapshot<T> {
    return this.snapshot;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  enterAnchor(value: T): Status {
    try {
      this.cancelClose();
      this.snapshot = { ...this.snapshot, anchorHeld: true, pending: value };
      const emitted = this.emit();
      if (!isOk(emitted)) return emitted;
      if (this.snapshot.active === value) {
        this.snapshot = { ...this.snapshot, pending: null };
        return this.emit();
      }
      this.cancelOpen();
      const delay =
        this.snapshot.active === null
          ? this.policy.openMs
          : this.policy.switchMs;
      if (delay <= 0) return this.open(value);
      this.opening = this.timer.set(() => {
        this.open(value);
      }, delay);
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not update hover intent.");
    }
  }

  leaveAnchor(): Status {
    try {
      this.snapshot = { ...this.snapshot, anchorHeld: false };
      const emitted = this.emit();
      if (!isOk(emitted)) return emitted;
      this.scheduleClose();
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not leave the hover anchor.");
    }
  }

  focusAnchor(value: T): Status {
    return this.enterAnchor(value);
  }

  blurAnchor(): Status {
    return this.leaveAnchor();
  }

  enterOverlay(): Status {
    try {
      this.cancelClose();
      this.snapshot = { ...this.snapshot, overlayHeld: true };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not enter the hover overlay.");
    }
  }

  leaveOverlay(): Status {
    try {
      this.snapshot = { ...this.snapshot, overlayHeld: false };
      const emitted = this.emit();
      if (!isOk(emitted)) return emitted;
      this.scheduleClose();
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not leave the hover overlay.");
    }
  }

  close(): Status {
    try {
      this.cancelOpen();
      this.cancelClose();
      this.snapshot = {
        active: null,
        pending: null,
        anchorHeld: false,
        overlayHeld: false,
      };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not close the hover overlay.");
    }
  }

  dispose(): Status {
    try {
      this.cancelOpen();
      this.cancelClose();
      this.listeners.clear();
      this.snapshot = {
        active: null,
        pending: null,
        anchorHeld: false,
        overlayHeld: false,
      };
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not dispose hover intent.");
    }
  }

  private open(value: T): Status {
    try {
      this.opening = undefined;
      if (!this.snapshot.anchorHeld || this.snapshot.pending !== value)
        return okStatus();
      this.snapshot = { ...this.snapshot, active: value, pending: null };
      return this.emit();
    } catch (error) {
      return statusFromUnknown(error, "Could not open the hover overlay.");
    }
  }

  private scheduleClose(): void {
    this.cancelOpen();
    this.cancelClose();
    if (this.snapshot.anchorHeld || this.snapshot.overlayHeld) return;
    this.closing = this.timer.set(() => {
      this.closing = undefined;
      if (this.snapshot.anchorHeld || this.snapshot.overlayHeld) return;
      this.snapshot = { ...this.snapshot, active: null, pending: null };
      this.emit();
    }, this.policy.handoffMs);
  }

  private cancelOpen(): void {
    if (this.opening !== undefined) this.timer.clear(this.opening);
    this.opening = undefined;
  }

  private cancelClose(): void {
    if (this.closing !== undefined) this.timer.clear(this.closing);
    this.closing = undefined;
  }

  private emit(): Status {
    for (const listener of this.listeners) {
      try {
        listener();
      } catch (error) {
        return statusFromUnknown(
          error,
          "A hover-intent subscriber could not receive an update.",
        );
      }
    }
    return okStatus();
  }
}

export interface ScrollMetrics {
  scrollHeight: number;
  scrollTop: number;
  clientHeight: number;
}

/** Studio thresholds for streamed transcript and wire panes. */
export const STUDIO_CHAT_BOTTOM_PX = 24;
export const STUDIO_WIRE_BOTTOM_PX = 48;

/** Symmetric follow-tail state: scrolling back to the end re-enables following. */
export class FollowTailController {
  private pinned = true;

  constructor(private readonly thresholdPx: number) {}

  getSnapshot(): boolean {
    return this.pinned;
  }

  measure(metrics: ScrollMetrics): boolean {
    this.pinned =
      metrics.scrollHeight - metrics.scrollTop - metrics.clientHeight <=
      this.thresholdPx;
    return this.pinned;
  }

  shouldFollow(): boolean {
    return this.pinned;
  }

  reset(pinned = true): void {
    this.pinned = pinned;
  }
}

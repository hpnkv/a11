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

import {
  type OverlayPlacement,
  type OverlayPlacementOptions,
  placeOverlay,
  type Rect,
} from "../geometry.js";
import {
  isOk,
  okStatus,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from "../../status.js";

export interface ObservedOverlay {
  update(): StatusOr<OverlayPlacement>;
  dispose(): Status;
}

export interface ObserveOverlayOptions extends OverlayPlacementOptions {
  root?: HTMLElement;
  onPlacement?: (placement: OverlayPlacement) => void;
}

const rectOf = (value: DOMRect | DOMRectReadOnly): Rect => ({
  left: value.left,
  top: value.top,
  right: value.right,
  bottom: value.bottom,
  width: value.width,
  height: value.height,
});

/**
 * Keep caller-created overlay content above clipping panels and beside an anchor.
 *
 * The helper owns positioning and observation only. The caller owns all content,
 * classes, visual depth, z-index, ARIA, focus, and animation.
 */
export function observeOverlay(
  anchor: HTMLElement,
  overlay: HTMLElement,
  options: ObserveOverlayOptions = {},
): StatusOr<ObservedOverlay> {
  try {
    const document = anchor.ownerDocument;
    const root = options.root ?? document.body;
    root.append(overlay);
    const update = (): StatusOr<OverlayPlacement> => {
      try {
        const view = document.defaultView;
        const width = view?.innerWidth ?? document.documentElement.clientWidth;
        const height =
          view?.innerHeight ?? document.documentElement.clientHeight;
        const placement = placeOverlay(
          rectOf(anchor.getBoundingClientRect()),
          rectOf(overlay.getBoundingClientRect()),
          { left: 0, top: 0, right: width, bottom: height, width, height },
          options,
        );
        overlay.style.position = "fixed";
        overlay.style.left = `${placement.left}px`;
        overlay.style.top = `${placement.top}px`;
        overlay.style.maxWidth = `${placement.maxWidth}px`;
        overlay.style.maxHeight = `${placement.maxHeight}px`;
        overlay.style.transformOrigin = placement.transformOrigin;
        overlay.dataset["a11OverlaySide"] = placement.side;
        options.onPlacement?.(placement);
        return placement;
      } catch (error) {
        return statusFromUnknown(error, "Could not position the overlay.");
      }
    };
    const onViewportChange = (): void => {
      update();
    };
    const view = document.defaultView;
    view?.addEventListener("resize", onViewportChange);
    document.addEventListener("scroll", onViewportChange, true);
    const Resize = view?.ResizeObserver;
    const observer = Resize ? new Resize(onViewportChange) : null;
    observer?.observe(anchor);
    observer?.observe(overlay);
    let disposed = false;
    const dispose = (): Status => {
      if (disposed) return okStatus();
      disposed = true;
      try {
        observer?.disconnect();
        view?.removeEventListener("resize", onViewportChange);
        document.removeEventListener("scroll", onViewportChange, true);
        overlay.remove();
        return okStatus();
      } catch (error) {
        return statusFromUnknown(error, "Could not dispose the overlay.");
      }
    };
    const initial = update();
    if (!isOk(initial)) {
      dispose();
      return initial;
    }
    return { update, dispose };
  } catch (error) {
    try {
      overlay.remove();
    } catch {
      // Preserve the first, more useful failure.
    }
    return statusFromUnknown(error, "Could not observe the overlay.");
  }
}

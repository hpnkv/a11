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

/** A rectangular inset, in the same unit as the geometry it modifies. */
export interface Insets<T> {
  top: T;
  right: T;
  bottom: T;
  left: T;
}

/** Horizontal and vertical radii of one CSS corner. */
export interface CornerRadius<T> {
  x: T;
  y: T;
}

/** The four independently addressable radii of a rounded rectangle. */
export interface CornerRadii<T> {
  topLeft: CornerRadius<T>;
  topRight: CornerRadius<T>;
  bottomRight: CornerRadius<T>;
  bottomLeft: CornerRadius<T>;
}

/** One value on every side. */
export function uniformInsets<T>(value: T): Insets<T> {
  return { top: value, right: value, bottom: value, left: value };
}

/** One circular radius on every corner. */
export function uniformRadii<T>(value: T): CornerRadii<T> {
  const radius = (): CornerRadius<T> => ({ x: value, y: value });
  return {
    topLeft: radius(),
    topRight: radius(),
    bottomRight: radius(),
    bottomLeft: radius(),
  };
}

const inner = (outer: number, inset: number, border: number): number =>
  Math.max(0, outer - inset - border);

/**
 * Radii for a surface inset inside another rounded rectangle.
 *
 * CSS derives an inner curve independently on each axis. The left/right inset
 * changes the horizontal radius; the top/bottom inset changes the vertical
 * radius. Keeping those axes separate also handles asymmetric and elliptical
 * corners without a renderer-specific shortcut.
 */
export function concentricRadii(
  outer: CornerRadii<number>,
  inset: Insets<number>,
  border: Insets<number> = uniformInsets(0),
): CornerRadii<number> {
  return {
    topLeft: {
      x: inner(outer.topLeft.x, inset.left, border.left),
      y: inner(outer.topLeft.y, inset.top, border.top),
    },
    topRight: {
      x: inner(outer.topRight.x, inset.right, border.right),
      y: inner(outer.topRight.y, inset.top, border.top),
    },
    bottomRight: {
      x: inner(outer.bottomRight.x, inset.right, border.right),
      y: inner(outer.bottomRight.y, inset.bottom, border.bottom),
    },
    bottomLeft: {
      x: inner(outer.bottomLeft.x, inset.left, border.left),
      y: inner(outer.bottomLeft.y, inset.bottom, border.bottom),
    },
  };
}

const cssTerm = (value: string | number): string =>
  typeof value === "number" ? `${value}px` : value;

/**
 * A CSS expression for the concentric inner radius of one axis.
 *
 * The caller owns the variables and where the expression is assigned. Returning
 * an expression rather than a style object keeps the helper useful in any
 * component system and retains live CSS custom-property updates.
 */
export function concentricRadiusExpression(
  outer: string | number,
  inset: string | number,
  border: string | number = 0,
): string {
  return `max(0px, calc(${cssTerm(outer)} - ${cssTerm(inset)} - ${cssTerm(border)}))`;
}

/** Which vertical edge of a list item touches the enclosing curve. */
export type ListEdge = "single" | "first" | "middle" | "last";

/** Classify a list position, handling the one-item list at both ends. */
export function listEdge(index: number, count: number): ListEdge {
  if (count <= 1) return "single";
  if (index <= 0) return "first";
  if (index >= count - 1) return "last";
  return "middle";
}

/**
 * Keep only the enclosing list's outside corners on an item.
 *
 * Middle corners use the caller's quieter radius, which can be zero or a small
 * local curve. A single item receives both the leading and trailing corners.
 */
export function listEdgeRadii<T>(
  index: number,
  count: number,
  outside: CornerRadii<T>,
  middle: CornerRadii<T>,
): CornerRadii<T> {
  const edge = listEdge(index, count);
  return {
    topLeft:
      edge === "single" || edge === "first" ? outside.topLeft : middle.topLeft,
    topRight:
      edge === "single" || edge === "first"
        ? outside.topRight
        : middle.topRight,
    bottomRight:
      edge === "single" || edge === "last"
        ? outside.bottomRight
        : middle.bottomRight,
    bottomLeft:
      edge === "single" || edge === "last"
        ? outside.bottomLeft
        : middle.bottomLeft,
  };
}

/** A DOM-independent rectangle in viewport coordinates. */
export interface Rect {
  left: number;
  top: number;
  right: number;
  bottom: number;
  width: number;
  height: number;
}

export type OverlaySide = "top" | "bottom" | "left" | "right";

export interface OverlayPlacementOptions {
  preferred?: readonly OverlaySide[];
  gap?: number;
  margin?: number;
  maxWidth?: number;
  maxHeight?: number;
}

export interface OverlayPlacement {
  left: number;
  top: number;
  side: OverlaySide;
  maxWidth: number;
  maxHeight: number;
  transformOrigin: string;
}

const available = (
  side: OverlaySide,
  anchor: Rect,
  viewport: Rect,
  gap: number,
  margin: number,
): number => {
  if (side === "top") return anchor.top - viewport.top - gap - margin;
  if (side === "bottom") return viewport.bottom - anchor.bottom - gap - margin;
  if (side === "left") return anchor.left - viewport.left - gap - margin;
  return viewport.right - anchor.right - gap - margin;
};

/**
 * Place an overlay beside an anchor and clamp it to the viewport.
 *
 * This function reads no DOM. Browser, canvas, and native IDE renderers can all
 * feed it rectangles and apply the returned fixed coordinates themselves.
 */
export function placeOverlay(
  anchor: Rect,
  overlay: Pick<Rect, "width" | "height">,
  viewport: Rect,
  options: OverlayPlacementOptions = {},
): OverlayPlacement {
  const gap = options.gap ?? 8;
  const margin = options.margin ?? 8;
  const preferred = options.preferred ?? ["bottom", "top", "right", "left"];
  const needed = (side: OverlaySide): number =>
    side === "top" || side === "bottom" ? overlay.height : overlay.width;
  const side =
    preferred.find(
      (candidate) =>
        available(candidate, anchor, viewport, gap, margin) >=
        needed(candidate),
    ) ??
    [...preferred].sort(
      (left, right) =>
        available(right, anchor, viewport, gap, margin) -
        available(left, anchor, viewport, gap, margin),
    )[0] ??
    "bottom";
  const maxWidth = Math.max(
    0,
    Math.min(
      options.maxWidth ?? overlay.width,
      viewport.width - 2 * margin,
      side === "left" || side === "right"
        ? available(side, anchor, viewport, gap, margin)
        : viewport.width - 2 * margin,
    ),
  );
  const maxHeight = Math.max(
    0,
    Math.min(
      options.maxHeight ?? overlay.height,
      viewport.height - 2 * margin,
      side === "top" || side === "bottom"
        ? available(side, anchor, viewport, gap, margin)
        : viewport.height - 2 * margin,
    ),
  );
  let left =
    side === "left"
      ? anchor.left - gap - overlay.width
      : side === "right"
        ? anchor.right + gap
        : anchor.left;
  let top =
    side === "top"
      ? anchor.top - gap - overlay.height
      : side === "bottom"
        ? anchor.bottom + gap
        : anchor.top;
  left = Math.max(
    viewport.left + margin,
    Math.min(left, viewport.right - margin - Math.min(overlay.width, maxWidth)),
  );
  top = Math.max(
    viewport.top + margin,
    Math.min(
      top,
      viewport.bottom - margin - Math.min(overlay.height, maxHeight),
    ),
  );
  const transformOrigin =
    side === "top"
      ? "bottom left"
      : side === "bottom"
        ? "top left"
        : side === "left"
          ? "top right"
          : "top left";
  return { left, top, side, maxWidth, maxHeight, transformOrigin };
}

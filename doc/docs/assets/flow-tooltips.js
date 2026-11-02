// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

const available = matchMedia("(min-width: 60em) and (hover: hover) and (pointer: fine)");
let modulePromise;
let hoverTimer;
let hideTimer;
let selectingTooltip = false;
let selectedTooltip = false;
let currentCode;
let currentOffset = -1;
let requestNumber = 0;

function flowModule() {
  modulePromise ??= import("./wasm/index.mjs")
    .then(binding => binding.createA11Wasm())
    .catch(error => { modulePromise = undefined; throw error; });
  return modulePromise;
}

function inline(parent, source, module, flowExample = false) {
  for (const part of source.split(/(`[^`]+`|\*\*[^*]+\*\*|\*[^*]+\*)/g)) {
    if (!part) continue;
    const code = part.startsWith("`") && part.endsWith("`");
    const strong = part.startsWith("**") && part.endsWith("**");
    const emphasis = !strong && part.startsWith("*") && part.endsWith("*");
    const element = code ? "code" : strong ? "strong" : emphasis ? "em" : "span";
    const node = document.createElement(element);
    const value = part.slice(code || emphasis ? 1 : strong ? 2 : 0,
      code || emphasis ? -1 : strong ? -2 : undefined);
    if (code && flowExample) appendFlowTokens(node, value, module);
    else node.textContent = value;
    parent.append(node);
  }
}

function appendFlowTokens(code, source, module) {
  const reply = module.flow({method: "tokens", source, offsets: "utf16"});
  const tokens = reply?.ok ? reply.result?.tokens : undefined;
  if (!Array.isArray(tokens)) {
    code.textContent = source;
    return;
  }
  let position = 0;
  for (const token of tokens) {
    if (!Number.isInteger(token.start) || !Number.isInteger(token.end) ||
        token.start < position || token.end > source.length) continue;
    code.append(document.createTextNode(source.slice(position, token.start)));
    const span = document.createElement("span");
    span.className = `a11-flow-token-${token.kind}`;
    span.textContent = source.slice(token.start, token.end);
    code.append(span);
    position = token.end;
  }
  code.append(document.createTextNode(source.slice(position)));
}

function flowCode(source, module) {
  const pre = document.createElement("pre");
  const code = document.createElement("code");
  pre.append(code);
  appendFlowTokens(code, source, module);
  return pre;
}

function content(display, module) {
  const root = document.createElement("div");
  root.className = "a11-flow-tooltip-content";
  if (!display.body?.trim()) root.classList.add("heading-only");
  const title = document.createElement("div");
  title.className = "a11-flow-tooltip-title";
  const label = document.createElement("code");
  label.textContent = display.label;
  title.append(label);
  const role = document.createElement("span");
  role.className = "a11-flow-tooltip-role";
  inline(role, display.role);
  title.append(role);
  if (display.type) {
    const type = document.createElement("span");
    type.className = display.trailing_type
      ? "a11-flow-tooltip-type trailing" : "a11-flow-tooltip-type";
    if (display.inferred_type) {
      const icon = document.createElementNS("http://www.w3.org/2000/svg", "svg");
      icon.setAttribute("viewBox", "0 0 24 24");
      icon.setAttribute("aria-label", "Inferred type");
      const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
      path.setAttribute("d", "M9 18h6m-5 4h4m-5-8a6 6 0 1 1 6 0c-.7.6-1 1.2-1 2H10c0-.8-.3-1.4-1-2Z");
      icon.append(path);
      type.append(icon);
    }
    const text = document.createElement("code");
    text.textContent = display.type;
    type.append(text);
    title.append(type);
  }
  root.append(title);
  const lines = (display.body ?? "").trim().split("\n");
  let paragraph = [];
  let list;
  const flush = () => {
    if (!paragraph.length) return;
    const p = document.createElement("p");
    const text = paragraph.join(" ");
    inline(p, text, module, /^\*\*Examples?:\*\*/.test(text));
    root.append(p);
    paragraph = [];
  };
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (line.startsWith("```a11flow")) {
      flush();
      const source = [];
      while (++i < lines.length && !lines[i].startsWith("```")) {
        source.push(lines[i]);
      }
      root.append(flowCode(source.join("\n"), module));
    } else if (line.startsWith("- ")) {
      flush();
      if (!list) { list = document.createElement("ul"); root.append(list); }
      const item = document.createElement("li");
      inline(item, line.slice(2));
      list.append(item);
    } else if (!line.trim()) {
      flush();
      list = undefined;
    } else {
      paragraph.push(line.trim());
    }
  }
  flush();
  return root;
}

function offsetAt(code, x, y) {
  const caret = document.caretPositionFromPoint?.(x, y);
  const range = caret ? null : document.caretRangeFromPoint?.(x, y);
  const node = caret?.offsetNode ?? range?.startContainer;
  const offset = caret?.offset ?? range?.startOffset;
  if (!node || offset === undefined || !code.contains(node)) return -1;
  const before = document.createRange();
  before.selectNodeContents(code);
  before.setEnd(node, offset);
  return before.toString().length;
}

function tooltip() {
  let box = document.querySelector(".a11-flow-tooltip");
  if (box) return box;
  box = document.createElement("aside");
  box.className = "a11-flow-tooltip";
  box.setAttribute("role", "tooltip");
  box.addEventListener("mouseenter", () => {
    clearTimeout(hideTimer);
    clearTimeout(hoverTimer);
    requestNumber++;
  });
  box.addEventListener("mouseleave", () => scheduleHide());
  box.addEventListener("pointerdown", () => {
    selectingTooltip = true;
    selectedTooltip = false;
    clearTimeout(hideTimer);
  });
  document.body.append(box);
  return box;
}

document.addEventListener("pointerup", () => {
  if (!selectingTooltip) return;
  selectingTooltip = false;
  const box = document.querySelector(".a11-flow-tooltip");
  const selection = document.getSelection();
  selectedTooltip = !!box && !!selection && !selection.isCollapsed &&
    box.contains(selection.anchorNode);
  if (selectedTooltip) return;
  if (!document.querySelector(".a11-flow-tooltip:hover")) scheduleHide();
});

document.addEventListener("pointerdown", event => {
  const box = document.querySelector(".a11-flow-tooltip");
  if (selectedTooltip && box && !box.contains(event.target)) hide();
});

function hide() {
  requestNumber++;
  clearTimeout(hoverTimer);
  clearTimeout(hideTimer);
  currentCode = undefined;
  currentOffset = -1;
  selectedTooltip = false;
  document.querySelector(".a11-flow-tooltip")?.remove();
}

function scheduleHide() {
  if (selectingTooltip || selectedTooltip) return;
  clearTimeout(hideTimer);
  hideTimer = setTimeout(hide, 220);
}

function show(reply, module, x, y) {
  if (!reply?.ok || !reply.result?.found) return;
  const box = tooltip();
  const display = reply.result.display ?? {label: reply.result.text ?? "",
    role: reply.result.kind ?? "", body: reply.result.markdown ?? ""};
  box.replaceChildren(content(display, module));
  box.style.visibility = "hidden";
  box.style.left = `${x + 14}px`;
  box.style.top = `${y + 18}px`;
  const rect = box.getBoundingClientRect();
  box.style.left = `${Math.max(8, Math.min(x + 14, innerWidth - rect.width - 8))}px`;
  box.style.top = `${Math.max(8, Math.min(y + 18, innerHeight - rect.height - 8))}px`;
  box.style.visibility = "visible";
}

document.addEventListener("mousemove", event => {
  if (!available.matches) return;
  if (selectingTooltip || selectedTooltip) return;
  if (event.target.closest?.(".a11-flow-tooltip")) return;
  const code = event.target.closest?.(".a11-flow.highlight code");
  if (!code) {
    if (currentCode) scheduleHide();
    return;
  }
  clearTimeout(hideTimer);
  const offset = offsetAt(code, event.clientX, event.clientY);
  if (offset < 0 || (code === currentCode && offset === currentOffset)) return;
  currentCode = code;
  currentOffset = offset;
  const request = ++requestNumber;
  clearTimeout(hoverTimer);
  hoverTimer = setTimeout(async () => {
    try {
      const module = await flowModule();
      if (request !== requestNumber || !available.matches) return;
      const reply = module.flow({method: "describe", source: code.textContent,
        offset, offsets: "utf16", snippet: true});
      if (request === requestNumber) {
        if (reply?.ok && reply.result?.found) show(reply, module, event.clientX, event.clientY);
        else hide();
      }
    } catch { hide(); }
  }, 180);
});

available.addEventListener("change", () => { if (!available.matches) hide(); });

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

/**
 * Render assistant markdown to a safe HTML fragment. `marked` does not sanitize
 * on its own, and although the content comes from our own local backend, we
 * still strip active content (scripts, event handlers, `javascript:` URLs) so a
 * model reply can never execute code in the IDE's embedded browser.
 */

import { marked } from 'marked';

marked.setOptions({ gfm: true, breaks: true });

const BLOCKED_TAGS = new Set(['SCRIPT', 'STYLE', 'IFRAME', 'OBJECT', 'EMBED', 'LINK', 'META', 'BASE']);

function sanitize(root: HTMLElement): void {
  for (const element of Array.from(root.querySelectorAll('*'))) {
    if (BLOCKED_TAGS.has(element.tagName)) {
      element.remove();
      continue;
    }
    for (const attribute of Array.from(element.attributes)) {
      const name = attribute.name.toLowerCase();
      const value = attribute.value.trim().toLowerCase();
      if (name.startsWith('on') || ((name === 'href' || name === 'src') && value.startsWith('javascript:'))) {
        element.removeAttribute(attribute.name);
      }
    }
  }
}

/** Render markdown to sanitized HTML. */
export function renderMarkdown(markdown: string): string {
  const container = document.createElement('div');
  container.innerHTML = marked.parse(markdown, { async: false });
  sanitize(container);
  return container.innerHTML;
}

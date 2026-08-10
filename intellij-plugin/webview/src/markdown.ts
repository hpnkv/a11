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

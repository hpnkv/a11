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

import hljs from 'highlight.js/lib/core';
import bash from 'highlight.js/lib/languages/bash';
import cpp from 'highlight.js/lib/languages/cpp';
import css from 'highlight.js/lib/languages/css';
import go from 'highlight.js/lib/languages/go';
import java from 'highlight.js/lib/languages/java';
import javascript from 'highlight.js/lib/languages/javascript';
import json from 'highlight.js/lib/languages/json';
import markdown from 'highlight.js/lib/languages/markdown';
import python from 'highlight.js/lib/languages/python';
import rust from 'highlight.js/lib/languages/rust';
import sql from 'highlight.js/lib/languages/sql';
import typescript from 'highlight.js/lib/languages/typescript';
import xml from 'highlight.js/lib/languages/xml';
import yaml from 'highlight.js/lib/languages/yaml';

for (const [name, grammar] of Object.entries({
  bash, shell: bash, cpp, css, go, java, javascript, js: javascript, json,
  markdown, python, py: python, rust, sql, typescript, ts: typescript, xml,
  html: xml, yaml,
})) {
  hljs.registerLanguage(name, grammar);
}

/** Highlight supported fenced code blocks already present in a safe DOM tree. */
export function highlightCodeBlocks(root: HTMLElement): void {
  for (const code of root.querySelectorAll<HTMLElement>('pre code')) {
    const language = /(?:language|lang)-([\w-]+)/.exec(code.className)?.[1];
    const source = code.textContent ?? '';
    code.innerHTML = language && hljs.getLanguage(language)
      ? hljs.highlight(source, {language, ignoreIllegals: true}).value
      : hljs.highlightAuto(source).value;
    code.classList.add('hljs');
  }
}

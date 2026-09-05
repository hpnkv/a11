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

(() => {
  const endpoint = document
    .querySelector('meta[name="a11-analytics-endpoint"]')
    ?.getAttribute('content')
    ?.trim();

  if (!endpoint || navigator.doNotTrack === '1') return;

  const count = (stage, title = document.title) => {
    const url = new URL(endpoint);
    url.searchParams.set('p', `/funnel/${stage}`);
    url.searchParams.set('t', title);
    fetch(url, {
      method: 'GET',
      mode: 'no-cors',
      cache: 'no-store',
      keepalive: true,
    }).catch(() => {});
  };

  count('visit');

  try {
    const previous = Number(localStorage.getItem('a11-last-visit') || 0);
    const oneDay = 24 * 60 * 60 * 1000;
    if (!previous) {
      localStorage.setItem('a11-last-visit', String(Date.now()));
    } else if (Date.now() - previous >= oneDay) {
      count('return');
      localStorage.setItem('a11-last-visit', String(Date.now()));
    }
  } catch {
    // Storage may be disabled. Page and click counts still work without it.
  }

  document.addEventListener('click', (event) => {
    if (!(event.target instanceof Element)) return;
    const link = event.target.closest('[data-a11-event]');
    if (!(link instanceof HTMLElement)) return;
    const stage = link.dataset.a11Event;
    if (stage) count(stage, link.textContent?.trim() || document.title);
  });

  window.addEventListener('a11:example-succeeded', (event) => {
    const name = event instanceof CustomEvent ? event.detail?.example : null;
    count('successful-example', name || 'Documentation example completed');
  });
})();

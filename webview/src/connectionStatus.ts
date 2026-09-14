/*
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

import type {GatewayConnectionNotice} from './a11client.js';

/** A compact, persistent indication that a Gateway-backed view is offline. */
export function createConnectionStatus(): {
  element: HTMLSpanElement;
  update: (notice: GatewayConnectionNotice) => void;
} {
  const element = document.createElement('span');
  element.className = 'connection-status';
  element.hidden = true;
  return {
    element,
    update(notice) {
      element.className = `connection-status ${notice.state}`;
      element.hidden = notice.state === 'connected';
      element.textContent = notice.state === 'connecting'
        ? 'Connecting to A11 Gateway…'
        : `Gateway connection lost — reconnecting${notice.message ? `: ${notice.message}` : '…'}`;
      element.title = notice.message ?? '';
    },
  };
}

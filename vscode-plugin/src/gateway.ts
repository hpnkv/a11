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

import WebSocket from 'ws';
import * as vscode from 'vscode';
import {completeUrl} from './settings.js';

/** A session-wide Gateway connection, established independently of panels. */
export class GatewayConnection implements vscode.Disposable {
  private socket: WebSocket | undefined;
  private timer: NodeJS.Timeout | undefined;
  private disposed = false;
  private notified = false;

  start(): void {
    this.ensureConnected();
  }

  /** Dial immediately when neither an open nor a pending socket exists. */
  ensureConnected(): void {
    if (this.disposed || this.socket) return;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = undefined;
    }
    this.connect();
  }

  private connect(): void {
    if (this.disposed) return;
    const configured = vscode.workspace.getConfiguration('a11').get<string>('gatewayUrl', '');
    let socket: WebSocket;
    try {
      socket = new WebSocket(completeUrl(configured));
    } catch (error) {
      this.failed(error instanceof Error ? error.message : String(error));
      this.scheduleRetry();
      return;
    }
    this.socket = socket;
    socket.once('open', () => { this.notified = false; });
    socket.once('unexpected-response', (_request, response) => {
      socket.close();
      this.failed(`HTTP ${response.statusCode}`);
    });
    socket.once('error', (error) => this.failed(error.message));
    socket.once('close', () => {
      if (this.socket === socket) this.socket = undefined;
      this.scheduleRetry();
    });
  }

  private scheduleRetry(): void {
    if (this.disposed || this.timer) return;
    this.timer = setTimeout(() => {
      this.timer = undefined;
      this.ensureConnected();
    }, 5_000);
  }

  private failed(reason: string): void {
    if (this.notified || this.disposed) return;
    this.notified = true;
    void vscode.window.showInformationMessage(
      `A11 Gateway is not available yet (${reason}). The extension will keep trying.`,
      'Open settings',
    ).then((choice) => {
      if (choice === 'Open settings') {
        void vscode.commands.executeCommand('workbench.action.openSettings', 'a11.gatewayUrl');
      }
    });
  }

  dispose(): void {
    this.disposed = true;
    if (this.timer) clearTimeout(this.timer);
    this.socket?.close();
    this.socket = undefined;
  }
}

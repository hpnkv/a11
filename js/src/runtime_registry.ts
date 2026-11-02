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

import {ActionSchema} from './action_schema.js';
import {ActionMessage} from './data.js';
import {A11Runtime} from './runtime_backend.js';
import {GraphAction, GraphSession} from './runtime_graph.js';
import {invalidArgumentError, isOk, okStatus, type Status, type StatusOr}
  from './status.js';

/** A TypeScript handler installed in the C++ action registry. */
export type ActionHandler = (action: GraphAction,
  context: {signal?: AbortSignal}) =>
  void | Status | Promise<void | Status>;

export interface MakeActionOptions {
  id?: string;
  session?: GraphSession;
}

/** Convenient access to the registry owned by one initialized runtime. */
export class ActionRegistry {
  constructor(private readonly runtime: A11Runtime) {}

  async register(name: string, schema: ActionSchema,
      handler: ActionHandler | null = null): Promise<Status> {
    if (!(schema instanceof ActionSchema)) {
      return invalidArgumentError('schema must be an ActionSchema.');
    }
    if (name !== schema.name) {
      return invalidArgumentError('Registry action name differs from schema name.');
    }
    if (handler !== null && typeof handler !== 'function') {
      return invalidArgumentError('handler must be callable or null.');
    }
    const registered = handler === null
      ? await this.runtime.registerSchema(schema)
      : await this.runtime.registerAction(schema, handler);
    return isOk(registered) ? okStatus() : registered;
  }

  async unregister(name: string): Promise<Status> {
    const graph = this.runtime.graph();
    return isOk(graph) ? graph.unregisterAction(name) : graph;
  }

  async isRegistered(name: string): Promise<StatusOr<boolean>> {
    const graph = this.runtime.graph();
    return isOk(graph) ? graph.isActionRegistered(name) : graph;
  }

  async getSchema(name: string): Promise<StatusOr<ActionSchema>> {
    const graph = this.runtime.graph();
    return isOk(graph) ? graph.getActionSchema(name) : graph;
  }

  async listRegisteredActions(): Promise<StatusOr<string[]>> {
    const graph = this.runtime.graph();
    return isOk(graph) ? graph.listRegisteredActions() : graph;
  }

  async makeAction(name: string, options: MakeActionOptions = {}):
      Promise<StatusOr<GraphAction>> {
    if (options.session !== undefined) {
      return options.session.createAction(name, options.id);
    }
    const graph = this.runtime.graph();
    return isOk(graph) ? graph.action(name, options.id) : graph;
  }

  async makeActionMessage(name: string, actionId?: string):
      Promise<StatusOr<ActionMessage>> {
    const graph = this.runtime.graph();
    return isOk(graph) ? graph.makeActionMessage(name, actionId) : graph;
  }
}

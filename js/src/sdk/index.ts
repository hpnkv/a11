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
 * A11 model-interaction SDK for TypeScript.
 *
 * Ports of the Python `a11/sdk/` layer: the portable {@link Interaction} and
 * peer/config notions, the action ⇄ JSON-Schema tool translation, the tool
 * runner, the header-routed `interact_with_llm` dispatcher, the `config`-port
 * schemas for the server backends, and the in-browser `interact_with_gemma`
 * backend.
 *
 * @packageDocumentation
 */

export * from './llm.js';
export * from './presentation.js';
export * from './jsonschema.js';
export * from './tool_adapter.js';
export * from './tool_runner.js';
export * from './config_schemas.js';
export * from './interact_with_llm.js';
export * from './gemma/interact_with_gemma.js';

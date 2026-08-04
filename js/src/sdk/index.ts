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
export * from './jsonschema.js';
export * from './tool_adapter.js';
export * from './tool_runner.js';
export * from './config_schemas.js';
export * from './interact_with_llm.js';
export * from './gemma/interact_with_gemma.js';

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
import {base64Decode, base64Encode} from './bytes.js';
import {fromRaw, toRaw} from './fragment_bridge.js';
import {schemaFromJson, schemaToJson, type SchemaEntry}
  from './schema_json.js';
import {invalidArgumentError, isOk, type StatusOr} from './status.js';

/** Describe an SDK action using the C++ registry's JSON boundary shape. */
export function actionSchemaToBridge(schema: ActionSchema):
    StatusOr<Record<string, unknown>> {
  const entry = schemaToJson(schema, true, 'all');
  const inputs = (entry.inputs ?? []).map(port => ({...port,
    autofills: [] as Array<Record<string, unknown> | null>}));
  for (const port of inputs) {
    const original = schema.inputs.get(port.name);
    if (original === undefined) continue;
    for (const fragment of original.autofills) {
      if (fragment === null) {
        port.autofills.push(null);
      } else {
        const raw = toRaw(fragment);
        if (!isOk(raw)) return raw;
        port.autofills.push(raw);
      }
    }
  }
  const headers = (entry.headers ?? []).map(header => ({...header,
    ...(schema.headers.get(header.name)?.defaultValue === null ? {} :
      {default_base64: base64Encode(
        schema.headers.get(header.name)!.defaultValue!)}),
  }));
  return {...entry, inputs, headers};
}

/** Decode a native action's full schema, including runtime-only defaults. */
export function actionSchemaFromBridge(value: Record<string, unknown>):
    StatusOr<ActionSchema> {
  const decoded = schemaFromJson(value as unknown as SchemaEntry);
  if (!isOk(decoded)) return decoded;
  if (Array.isArray(value.inputs)) {
    for (const entry of value.inputs) {
      if (typeof entry !== 'object' || entry === null) continue;
      const name = (entry as {name?: unknown}).name;
      if (typeof name !== 'string') continue;
      const port = decoded.inputs.get(name);
      if (port === undefined) continue;
      const autofills = (entry as {autofills?: unknown}).autofills;
      if (autofills === undefined) continue;
      if (!Array.isArray(autofills)) {
        return invalidArgumentError('Native input autofills must be an array.');
      }
      for (const fragment of autofills) {
        if (fragment === null) {
          port.autofills.push(null);
        } else if (typeof fragment === 'object' &&
            !Array.isArray(fragment)) {
          const raw = fromRaw(fragment as Record<string, unknown>);
          if (!isOk(raw)) return raw;
          port.autofills.push(raw);
        } else {
          return invalidArgumentError('Native input autofill is malformed.');
        }
      }
    }
  }
  if (Array.isArray(value.headers)) {
    for (const entry of value.headers) {
      if (typeof entry !== 'object' || entry === null) continue;
      const name = (entry as {name?: unknown}).name;
      const encoded = (entry as {default_base64?: unknown}).default_base64;
      if (typeof name !== 'string' || encoded === undefined) continue;
      if (typeof encoded !== 'string') {
        return invalidArgumentError('Native header default is malformed.');
      }
      const header = decoded.headers.get(name);
      if (header === undefined) continue;
      const bytes = base64Decode(encoded);
      if (!isOk(bytes)) return bytes;
      header.defaultValue = bytes;
    }
  }
  const valid = decoded.validate();
  return isOk(valid) ? decoded : valid;
}

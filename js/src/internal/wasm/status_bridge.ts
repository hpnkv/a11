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

import {StatusCode, isStatus, statusFromUnknown,
  type NonOkStatus, type Status}
  from '../../status.js';

/** Convert one native rejection to the SDK's structured status value. */
export function statusFromBridgeFailure(error: unknown,
    context: string): NonOkStatus {
  if (isStatus(error) && error.code !== StatusCode.OK) return error;
  if (typeof error === 'object' && error !== null) {
    const candidate = error as Record<string, unknown>;
    const value = candidate.status ?? candidate;
    if (typeof value === 'object' && value !== null) {
      const fields = value as Record<string, unknown>;
      const status = {code: fields.code, message: fields.message,
        ...(fields.details === undefined ? {} : {details: fields.details})};
      if (isStatus(status) && status.code !== StatusCode.OK) return status;
    }
  }
  const reason = error instanceof Error && error.message.trim()
    ? `${context} ${error.message}` : context;
  return statusFromUnknown(error, reason);
}

/** Validate a native status returned as a successful bridge value. */
export function statusFromBridgeValue(value: unknown,
    context: string): Status {
  if (isStatus(value)) return value;
  return statusFromUnknown(new TypeError('Native status is malformed.'), context);
}

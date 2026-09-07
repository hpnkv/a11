/** Structured A11 authorization metadata and Action header helpers. */

import { decode, encode } from '@msgpack/msgpack';

import { Action } from './action.js';
import { AUTHORIZATION_REFERENCE_HEADER } from './action_schema.js';
import { base64Decode, base64Encode } from './bytes.js';
import {
  invalidArgumentError,
  isOk,
  okStatus,
  statusFromUnknown,
  type Status,
  type StatusOr,
} from './status.js';

export const AUTHORIZATION_HEADER = 'x-a11-auth';
export const AUTHORIZE_ACTION = '__authorize__';
export const AUTHORIZATION_VERSION = 1;
export const MAX_AUTHORIZATION_BYTES = 16 * 1024;
export const MAX_AUTHORIZATION_HOPS = 8;

export interface AuthorizationEnvelope {
  readonly version: 1;
  readonly chain: readonly string[];
}

function validateEnvelope(value: AuthorizationEnvelope): Status {
  if (value.version !== AUTHORIZATION_VERSION) {
    return invalidArgumentError('Unsupported A11 authorization version.');
  }
  if (value.chain.length < 1 || value.chain.length > MAX_AUTHORIZATION_HOPS) {
    return invalidArgumentError('Invalid A11 authorization chain length.');
  }
  if (value.chain.some((part) =>
    typeof part !== 'string' || part.length === 0 || part.split('.').length !== 3)) {
    return invalidArgumentError('Every authorization statement must be compact JWS.');
  }
  return okStatus();
}

export function encodeAuthorization(
  value: AuthorizationEnvelope,
): StatusOr<Uint8Array> {
  const valid = validateEnvelope(value);
  if (!isOk(valid)) return valid;
  try {
    const result = encode([value.version, [...value.chain]]);
    return result.byteLength <= MAX_AUTHORIZATION_BYTES
      ? result
      : invalidArgumentError('The authorization value is too large.');
  } catch (error) {
    return statusFromUnknown(error, 'Could not encode authorization.');
  }
}

export function decodeAuthorization(
  bytes: Uint8Array,
): StatusOr<AuthorizationEnvelope> {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength > MAX_AUTHORIZATION_BYTES) {
    return invalidArgumentError('The authorization value must be bounded bytes.');
  }
  try {
    const value = decode(bytes);
    if (!Array.isArray(value) || value.length !== 2 || value[0] !== 1 ||
      !Array.isArray(value[1]) || value[1].some((part) => typeof part !== 'string')) {
      return invalidArgumentError('The authorization envelope must be [1, chain].');
    }
    const envelope: AuthorizationEnvelope = {
      version: 1,
      chain: value[1] as string[],
    };
    const valid = validateEnvelope(envelope);
    if (!isOk(valid)) return valid;
    const canonical = encodeAuthorization(envelope);
    if (!isOk(canonical) || canonical.length !== bytes.length ||
      canonical.some((part, index) => part !== bytes[index])) {
      return invalidArgumentError('The authorization envelope is not canonical.');
    }
    return envelope;
  } catch (error) {
    return invalidArgumentError(
      'The authorization value is not MessagePack.',
      [],
      error,
    );
  }
}

export function authorizationToText(
  value: AuthorizationEnvelope,
): StatusOr<string> {
  const encoded = encodeAuthorization(value);
  if (!isOk(encoded)) return encoded;
  const body = base64Encode(encoded)
    .replaceAll('+', '-')
    .replaceAll('/', '_')
    .replace(/=+$/, '');
  return `a11-auth/1.${body}`;
}

export function authorizationFromText(
  value: string,
): StatusOr<AuthorizationEnvelope> {
  const prefix = 'a11-auth/1.';
  if (!value.startsWith(prefix)) {
    return invalidArgumentError(`Expected ${prefix}.`);
  }
  const encodedBody = value.slice(prefix.length);
  if (!/^[A-Za-z0-9_-]*$/.test(encodedBody)) {
    return invalidArgumentError('The authorization value is not base64url.');
  }
  const body = encodedBody
    .replaceAll('-', '+')
    .replaceAll('_', '/');
  const decoded = base64Decode(body + '='.repeat((4 - body.length % 4) % 4));
  return isOk(decoded) ? decodeAuthorization(decoded) : decoded;
}

export function getAuthorization(
  action: Action,
): StatusOr<AuthorizationEnvelope | null> {
  const full = action.getHeader(AUTHORIZATION_HEADER);
  if (!isOk(full) || full === null) return full;
  const reference = action.getHeader(AUTHORIZATION_REFERENCE_HEADER);
  if (!isOk(reference)) return reference;
  return reference === null
    ? decodeAuthorization(full)
    : invalidArgumentError('An action cannot carry both authorization forms.');
}

export function setAuthorizationHeader(
  action: Action,
  value: AuthorizationEnvelope | null,
): Status {
  if (value === null) return action.removeHeader(AUTHORIZATION_HEADER);
  const encoded = encodeAuthorization(value);
  if (!isOk(encoded)) return encoded;
  const status = action.setHeader(AUTHORIZATION_HEADER, encoded);
  if (!isOk(status)) return status;
  return action.removeHeader(AUTHORIZATION_REFERENCE_HEADER);
}

export function setAuthorizationReference(
  action: Action,
  contextId: string | null,
): Status {
  if (contextId === null) {
    return action.removeHeader(AUTHORIZATION_REFERENCE_HEADER);
  }
  if (!/^[A-Za-z0-9_-]*$/.test(contextId)) {
    return invalidArgumentError('The context ID is not base64url.');
  }
  const body = contextId.replaceAll('-', '+').replaceAll('_', '/');
  const decoded = base64Decode(body + '='.repeat((4 - body.length % 4) % 4));
  if (!isOk(decoded) || decoded.length !== 16) {
    return invalidArgumentError(
      'An authorization context ID must contain 128 bits.',
    );
  }
  const status = action.setHeader(AUTHORIZATION_REFERENCE_HEADER, decoded);
  if (!isOk(status)) return status;
  return action.removeHeader(AUTHORIZATION_HEADER);
}

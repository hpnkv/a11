/** Structured A11 authorization metadata and Action header helpers. */

import {GraphAction} from './runtime_graph.js';
import {authorizationBridge} from './internal/wasm/native_authorization.js';
import {statusFromBridgeFailure} from './internal/wasm/status_bridge.js';
import { AUTHORIZATION_REFERENCE_HEADER } from './action_schema.js';
import {base64Decode} from './bytes.js';
import {
  failedPreconditionError,
  invalidArgumentError,
  isOk,
  type Status,
  type StatusOr,
} from './status.js';

/** Header carrying an inline authorization envelope. */
export const AUTHORIZATION_HEADER = 'x-a11-auth';
/** Reserved action that exchanges authorization context. */
export const AUTHORIZE_ACTION = '__authorize__';
/** Current authorization envelope version. */
export const AUTHORIZATION_VERSION = 1;
/** Maximum encoded envelope size accepted from a peer. */
export const MAX_AUTHORIZATION_BYTES = 16 * 1024;
/** Maximum compact-JWS delegation chain length. */
export const MAX_AUTHORIZATION_HOPS = 8;

/** Versioned chain of compact JWS authorization statements. */
export interface AuthorizationEnvelope {
  /** Wire format version. */
  readonly version: 1;
  /** Delegation statements ordered from issuer to current holder. */
  readonly chain: readonly string[];
}

/** Validate and encode an authorization envelope as canonical MessagePack. */
export function encodeAuthorization(
  value: AuthorizationEnvelope,
): StatusOr<Uint8Array> {
  const native = authorizationBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
  try { return native.encode(value); }
  catch (error) { return statusFromBridgeFailure(error,
      'Authorization encoding failed.'); }
}

/** Decode and validate a canonical MessagePack authorization envelope. */
export function decodeAuthorization(
  bytes: Uint8Array,
): StatusOr<AuthorizationEnvelope> {
  const native = authorizationBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
  try { return native.decode(bytes); }
  catch (error) { return statusFromBridgeFailure(error,
      'Authorization decoding failed.'); }
}

/** Encode an envelope as an `a11-auth/1` base64url value. */
export function authorizationToText(
  value: AuthorizationEnvelope,
): StatusOr<string> {
  const native = authorizationBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
  try { return native.toText(value); }
  catch (error) { return statusFromBridgeFailure(error,
      'Authorization text encoding failed.'); }
}

/** Decode an `a11-auth/1` base64url value. */
export function authorizationFromText(
  value: string,
): StatusOr<AuthorizationEnvelope> {
  const native = authorizationBridge();
  if (native === null) return failedPreconditionError(
    'Initialize A11 WASM before using A11 protocol codecs.');
  try { return native.fromText(value); }
  catch (error) { return statusFromBridgeFailure(error,
      'Authorization text decoding failed.'); }
}

/** Read the inline envelope from an action that carries no reference. */
export async function getAuthorization(action: GraphAction):
    Promise<StatusOr<AuthorizationEnvelope | null>> {
      const full = await action.getHeader(AUTHORIZATION_HEADER);
      if (!isOk(full) || full === null) return full;
      const reference = await action.getHeader(AUTHORIZATION_REFERENCE_HEADER);
      if (!isOk(reference)) return reference;
      return reference === null ? decodeAuthorization(full) :
        invalidArgumentError(
          'An action cannot carry both authorization forms.');
}

/** Set an inline envelope and remove any authorization reference. */
export async function setAuthorizationHeader(action: GraphAction,
    value: AuthorizationEnvelope | null): Promise<Status> {
      if (value === null) return action.removeHeader(AUTHORIZATION_HEADER);
      const encoded = encodeAuthorization(value);
      if (!isOk(encoded)) return encoded;
      const status = await action.setHeader(AUTHORIZATION_HEADER, encoded);
      return isOk(status)
        ? action.removeHeader(AUTHORIZATION_REFERENCE_HEADER) : status;
}

/** Set a 128-bit context reference and remove any inline envelope. */
export async function setAuthorizationReference(action: GraphAction,
    contextId: string | null): Promise<Status> {
      if (contextId === null) {
        return action.removeHeader(AUTHORIZATION_REFERENCE_HEADER);
      }
      if (!/^[A-Za-z0-9_-]*$/.test(contextId)) {
        return invalidArgumentError('The context ID is not base64url.');
      }
      const body = contextId.replaceAll('-', '+').replaceAll('_', '/');
      const decoded = base64Decode(
        body + '='.repeat((4 - body.length % 4) % 4));
      if (!isOk(decoded) || decoded.length !== 16) {
        return invalidArgumentError(
          'An authorization context ID must contain 128 bits.');
      }
      const status = await action.setHeader(
        AUTHORIZATION_REFERENCE_HEADER, decoded);
      return isOk(status) ? action.removeHeader(AUTHORIZATION_HEADER) : status;
}

import type {AuthorizationEnvelope} from '../../authorization.js';

export interface AuthorizationBridge {
  encode(value: AuthorizationEnvelope): Uint8Array;
  decode(bytes: Uint8Array): AuthorizationEnvelope;
  toText(value: AuthorizationEnvelope): string;
  fromText(value: string): AuthorizationEnvelope;
}

let initialized: AuthorizationBridge | null = null;

export function installAuthorizationBridge(bridge: AuthorizationBridge): void {
  initialized = bridge;
}

export function authorizationBridge(): AuthorizationBridge | null {
  return initialized;
}

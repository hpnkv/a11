import {type AuthorizationEnvelope,
  getAuthorization as get,
  setAuthorizationHeader as setHeader,
  setAuthorizationReference as setReference} from './authorization.js';
import {type GraphAction} from './runtime_graph.js';
import {type Status, type StatusOr} from './status.js';

/** Read an inline authorization envelope from a native action. */
export function getAuthorization(action: GraphAction):
    Promise<StatusOr<AuthorizationEnvelope | null>> {
  return get(action);
}

/** Set an inline authorization envelope on a native action. */
export function setAuthorizationHeader(action: GraphAction,
    value: AuthorizationEnvelope | null): Promise<Status> {
  return setHeader(action, value);
}

/** Set a context reference on a native action. */
export function setAuthorizationReference(action: GraphAction,
    contextId: string | null): Promise<Status> {
  return setReference(action, contextId);
}

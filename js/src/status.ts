/* eslint-disable no-redeclare -- TypeScript overload signatures. */

export enum StatusCode {
  OK = 0,
  CANCELLED = 1,
  UNKNOWN = 2,
  INVALID_ARGUMENT = 3,
  DEADLINE_EXCEEDED = 4,
  NOT_FOUND = 5,
  ALREADY_EXISTS = 6,
  PERMISSION_DENIED = 7,
  RESOURCE_EXHAUSTED = 8,
  FAILED_PRECONDITION = 9,
  ABORTED = 10,
  OUT_OF_RANGE = 11,
  UNIMPLEMENTED = 12,
  INTERNAL = 13,
  UNAVAILABLE = 14,
  DATA_LOSS = 15,
  UNAUTHENTICATED = 16,
}

const kDefaultStatusMessages = {
  [StatusCode.OK]: 'OK',
  [StatusCode.CANCELLED]: 'Cancelled',
  [StatusCode.UNKNOWN]: 'Unknown',
  [StatusCode.INVALID_ARGUMENT]: 'Invalid Argument',
  [StatusCode.DEADLINE_EXCEEDED]: 'Deadline Exceeded',
  [StatusCode.NOT_FOUND]: 'Not Found',
  [StatusCode.ALREADY_EXISTS]: 'Already Exists',
  [StatusCode.PERMISSION_DENIED]: 'Permission Denied',
  [StatusCode.RESOURCE_EXHAUSTED]: 'Resource Exhausted',
  [StatusCode.FAILED_PRECONDITION]: 'Failed Precondition',
  [StatusCode.ABORTED]: 'Aborted',
  [StatusCode.OUT_OF_RANGE]: 'Out of Range',
  [StatusCode.UNIMPLEMENTED]: 'Unimplemented',
  [StatusCode.INTERNAL]: 'Internal',
  [StatusCode.UNAVAILABLE]: 'Unavailable',
  [StatusCode.DATA_LOSS]: 'Data Loss',
  [StatusCode.UNAUTHENTICATED]: 'Unauthenticated',
};

export enum InvalidStatusAccessBehaviour {
  TERMINATE = 'TERMINATE',
  THROW = 'THROW',
}

let _invalidStatusAccessBehaviour: InvalidStatusAccessBehaviour =
  InvalidStatusAccessBehaviour.TERMINATE;

interface StatusBase {
  code: StatusCode;
  message: string;
  details?: object[];

  cause?: unknown;
}

export interface OkStatus extends StatusBase {
  code: StatusCode.OK;
}

export interface NonOkStatus extends StatusBase {
  code: Exclude<StatusCode, StatusCode.OK>;
}

export type Status = OkStatus | NonOkStatus;

export type StatusOr<Type> = NonOkStatus | Type;

const isStatusAndIsOk = (val: unknown): [boolean, boolean] => {
  try {
    if (typeof val !== 'object' || val === null) {
      return [false, true];
    }
    if (!('code' in val) || !('message' in val)) {
      return [false, true];
    }

    const candidate = val as Record<string, unknown>;
    const keys = Object.keys(candidate);
    if (
      keys.some(
        (key) =>
          key !== 'code' &&
          key !== 'message' &&
          key !== 'details' &&
          key !== 'cause',
      ) ||
      !Number.isInteger(candidate.code) ||
      (candidate.code as number) < StatusCode.OK ||
      (candidate.code as number) > StatusCode.UNAUTHENTICATED ||
      typeof candidate.message !== 'string'
    ) {
      return [false, true];
    }
    if (
      candidate.details !== undefined &&
      (!Array.isArray(candidate.details) ||
        candidate.details.some(
          (detail) => typeof detail !== 'object' || detail === null,
        ))
    ) {
      return [false, true];
    }
    return [true, candidate.code === StatusCode.OK];
  } catch {
    return [false, true];
  }
};

export const isStatus = (val: unknown): val is Status => {
  return isStatusAndIsOk(val)[0];
};

export function isOk(val: Status): val is OkStatus;

export function isOk<T>(val: StatusOr<T>): val is T;

export function isOk(val: unknown) {
  return isStatusAndIsOk(val)[1];
}

export function getError<T>(val: StatusOr<T>): NonOkStatus | null {
  const [valIsStatus, valIsOk] = isStatusAndIsOk(val);
  if (!valIsStatus) {
    return null;
  }
  if (valIsOk) {
    return null;
  }
  return val as NonOkStatus;
}

export function getValue<T>(val: StatusOr<T>): T {
  if (_invalidStatusAccessBehaviour === InvalidStatusAccessBehaviour.THROW) {
    return valueOrThrow(val);
  }
  return valueOrTerminate(val);
}

export function terminate(message_or_status: Status | string): never {
  let paramIsStatus = isStatus(message_or_status);
  let codeRepr: string = 'UNKNOWN';
  let message: string;
  if (paramIsStatus) {
    codeRepr = StatusCode[(message_or_status as Status).code];
    message = (message_or_status as Status).message;
  } else {
    message = message_or_status as string;
  }

  console.error(`[${codeRepr}] ${message}`);
  if (typeof process !== 'undefined' && process.abort) {
    process.abort();
  }

  if (paramIsStatus) {
    throw new StatusException(message_or_status as Status);
  }

  throw new Error(message);
}

export class StatusException extends Error {
  public _status: Status;

  constructor(status: Status, options?: ErrorOptions) {
    if (isOk(status)) {
      terminate(
        invalidArgumentError(
          'StatusException cannot be created for OK status.',
        ),
      );
    }
    if (options && options.cause && status.cause) {
      terminate(
        invalidArgumentError(
          'StatusException cannot have cause both set in status and options.',
        ),
      );
    }

    if (options && options.cause) {
      status = { ...status, cause: options.cause };
    }
    super(status.message, { ...options, cause: status.cause });

    const cleanStatus: Status = { code: status.code, message: status.message };
    if (status.details !== undefined) {
      cleanStatus.details = status.details;
    }
    if (status.cause !== undefined) {
      cleanStatus.cause = status.cause;
    }
    this._status = cleanStatus;
  }

  status() {
    return this._status;
  }
}

export interface CancelledError extends NonOkStatus {
  code: StatusCode.CANCELLED;
}

export const cancelledError = (
  message: string = kDefaultStatusMessages[StatusCode.CANCELLED],
  details?: object[],
  cause?: unknown,
): CancelledError => {
  const status: CancelledError = {
    code: StatusCode.CANCELLED,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export function isCancelledError(val: Status): val is CancelledError;

export function isCancelledError<T>(val: StatusOr<T>): val is CancelledError {
  return isStatus(val) && (val as Status).code === StatusCode.CANCELLED;
}

export class CancelledException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.CANCELLED],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.CANCELLED, message, details }, { cause });
  }
}

export interface UnknownError extends NonOkStatus {
  code: StatusCode.UNKNOWN;
}

export function isUnknownError(val: Status): val is UnknownError;

export function isUnknownError<T>(val: StatusOr<T>): val is UnknownError {
  return isStatus(val) && (val as Status).code === StatusCode.UNKNOWN;
}

export const unknownError = (
  message: string = kDefaultStatusMessages[StatusCode.UNKNOWN],
  details?: object[],
  cause?: unknown,
): UnknownError => {
  const status: UnknownError = {
    code: StatusCode.UNKNOWN,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class UnknownException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.UNKNOWN],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.UNKNOWN, message, details }, { cause });
  }
}

export interface InvalidArgumentError extends NonOkStatus {
  code: StatusCode.INVALID_ARGUMENT;
}

export function isInvalidArgumentError(
  val: Status,
): val is InvalidArgumentError;
export function isInvalidArgumentError<T>(
  val: StatusOr<T>,
): val is InvalidArgumentError {
  return isStatus(val) && (val as Status).code === StatusCode.INVALID_ARGUMENT;
}

export const invalidArgumentError = (
  message: string = kDefaultStatusMessages[StatusCode.INVALID_ARGUMENT],
  details?: object[],
  cause?: unknown,
): InvalidArgumentError => {
  const status: InvalidArgumentError = {
    code: StatusCode.INVALID_ARGUMENT,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class InvalidArgumentException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.INVALID_ARGUMENT],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.INVALID_ARGUMENT, message, details }, { cause });
  }
}

export interface DeadlineExceededError extends NonOkStatus {
  code: StatusCode.DEADLINE_EXCEEDED;
}

export function isDeadlineExceededError(
  val: Status,
): val is DeadlineExceededError;
export function isDeadlineExceededError<T>(
  val: StatusOr<T>,
): val is DeadlineExceededError {
  return isStatus(val) && (val as Status).code === StatusCode.DEADLINE_EXCEEDED;
}

export const deadlineExceededError = (
  message: string = kDefaultStatusMessages[StatusCode.DEADLINE_EXCEEDED],
  details?: object[],
  cause?: unknown,
): DeadlineExceededError => {
  const status: DeadlineExceededError = {
    code: StatusCode.DEADLINE_EXCEEDED,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class DeadlineExceededException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.DEADLINE_EXCEEDED],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.DEADLINE_EXCEEDED, message, details }, { cause });
  }
}

export interface NotFoundError extends NonOkStatus {
  code: StatusCode.NOT_FOUND;
}

export function isNotFoundError(val: Status): val is NotFoundError;

export function isNotFoundError<T>(val: StatusOr<T>): val is NotFoundError {
  return isStatus(val) && (val as Status).code === StatusCode.NOT_FOUND;
}

export const notFoundError = (
  message: string = kDefaultStatusMessages[StatusCode.NOT_FOUND],
  details?: object[],
  cause?: unknown,
): NotFoundError => {
  const status: NotFoundError = {
    code: StatusCode.NOT_FOUND,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class NotFoundException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.NOT_FOUND],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.NOT_FOUND, message, details }, { cause });
  }
}

export interface AlreadyExistsError extends NonOkStatus {
  code: StatusCode.ALREADY_EXISTS;
}

export function isAlreadyExistsError(val: Status): val is AlreadyExistsError;
export function isAlreadyExistsError<T>(
  val: StatusOr<T>,
): val is AlreadyExistsError {
  return isStatus(val) && (val as Status).code === StatusCode.ALREADY_EXISTS;
}

export const alreadyExistsError = (
  message: string = kDefaultStatusMessages[StatusCode.ALREADY_EXISTS],
  details?: object[],
  cause?: unknown,
): AlreadyExistsError => {
  const status: AlreadyExistsError = {
    code: StatusCode.ALREADY_EXISTS,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class AlreadyExistsException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.ALREADY_EXISTS],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.ALREADY_EXISTS, message, details }, { cause });
  }
}

export interface PermissionDeniedError extends NonOkStatus {
  code: StatusCode.PERMISSION_DENIED;
}

export function isPermissionDeniedError(
  val: Status,
): val is PermissionDeniedError;
export function isPermissionDeniedError<T>(
  val: StatusOr<T>,
): val is PermissionDeniedError {
  return isStatus(val) && (val as Status).code === StatusCode.PERMISSION_DENIED;
}

export const permissionDeniedError = (
  message: string = kDefaultStatusMessages[StatusCode.PERMISSION_DENIED],
  details?: object[],
  cause?: unknown,
): PermissionDeniedError => {
  const status: PermissionDeniedError = {
    code: StatusCode.PERMISSION_DENIED,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class PermissionDeniedException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.PERMISSION_DENIED],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.PERMISSION_DENIED, message, details }, { cause });
  }
}

export interface ResourceExhaustedError extends NonOkStatus {
  code: StatusCode.RESOURCE_EXHAUSTED;
}

export function isResourceExhaustedError(
  val: Status,
): val is ResourceExhaustedError;
export function isResourceExhaustedError<T>(
  val: StatusOr<T>,
): val is ResourceExhaustedError {
  return (
    isStatus(val) && (val as Status).code === StatusCode.RESOURCE_EXHAUSTED
  );
}

export const resourceExhaustedError = (
  message: string = kDefaultStatusMessages[StatusCode.RESOURCE_EXHAUSTED],
  details?: object[],
  cause?: unknown,
): ResourceExhaustedError => {
  const status: ResourceExhaustedError = {
    code: StatusCode.RESOURCE_EXHAUSTED,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class ResourceExhaustedException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.RESOURCE_EXHAUSTED],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.RESOURCE_EXHAUSTED, message, details }, { cause });
  }
}

export interface FailedPreconditionError extends NonOkStatus {
  code: StatusCode.FAILED_PRECONDITION;
}

export function isFailedPreconditionError(
  val: Status,
): val is FailedPreconditionError;
export function isFailedPreconditionError<T>(
  val: StatusOr<T>,
): val is FailedPreconditionError {
  return (
    isStatus(val) && (val as Status).code === StatusCode.FAILED_PRECONDITION
  );
}

export const failedPreconditionError = (
  message: string = kDefaultStatusMessages[StatusCode.FAILED_PRECONDITION],
  details?: object[],
  cause?: unknown,
): FailedPreconditionError => {
  const status: FailedPreconditionError = {
    code: StatusCode.FAILED_PRECONDITION,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class FailedPreconditionException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.FAILED_PRECONDITION],
    details?: object[],
    cause?: unknown,
  ) {
    super(
      { code: StatusCode.FAILED_PRECONDITION, message, details },
      { cause },
    );
  }
}

export interface AbortedError extends NonOkStatus {
  code: StatusCode.ABORTED;
}

export function isAbortedError(val: Status): val is AbortedError;
export function isAbortedError<T>(val: StatusOr<T>): val is AbortedError {
  return isStatus(val) && (val as Status).code === StatusCode.ABORTED;
}

export const abortedError = (
  message: string = kDefaultStatusMessages[StatusCode.ABORTED],
  details?: object[],
  cause?: unknown,
): AbortedError => {
  const status: AbortedError = {
    code: StatusCode.ABORTED,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class AbortedException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.ABORTED],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.ABORTED, message, details }, { cause });
  }
}

export interface OutOfRangeError extends NonOkStatus {
  code: StatusCode.OUT_OF_RANGE;
}

export function isOutOfRangeError(val: Status): val is OutOfRangeError;
export function isOutOfRangeError<T>(val: StatusOr<T>): val is OutOfRangeError {
  return isStatus(val) && (val as Status).code === StatusCode.OUT_OF_RANGE;
}

export const outOfRangeError = (
  message: string = kDefaultStatusMessages[StatusCode.OUT_OF_RANGE],
  details?: object[],
  cause?: unknown,
): OutOfRangeError => {
  const status: OutOfRangeError = {
    code: StatusCode.OUT_OF_RANGE,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class OutOfRangeException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.OUT_OF_RANGE],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.OUT_OF_RANGE, message, details }, { cause });
  }
}

export interface UnimplementedError extends NonOkStatus {
  code: StatusCode.UNIMPLEMENTED;
}

export function isUnimplementedError(val: Status): val is UnimplementedError;
export function isUnimplementedError<T>(
  val: StatusOr<T>,
): val is UnimplementedError {
  return isStatus(val) && (val as Status).code === StatusCode.UNIMPLEMENTED;
}

export const unimplementedError = (
  message: string = kDefaultStatusMessages[StatusCode.UNIMPLEMENTED],
  details?: object[],
  cause?: unknown,
): UnimplementedError => {
  const status: UnimplementedError = {
    code: StatusCode.UNIMPLEMENTED,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class UnimplementedException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.UNIMPLEMENTED],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.UNIMPLEMENTED, message, details }, { cause });
  }
}

export interface InternalError extends NonOkStatus {
  code: StatusCode.INTERNAL;
}

export function isInternalError(val: Status): val is InternalError;
export function isInternalError<T>(val: StatusOr<T>): val is InternalError {
  return isStatus(val) && (val as Status).code === StatusCode.INTERNAL;
}

export const internalError = (
  message: string = kDefaultStatusMessages[StatusCode.INTERNAL],
  details?: object[],
  cause?: unknown,
): InternalError => {
  const status: InternalError = {
    code: StatusCode.INTERNAL,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class InternalException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.INTERNAL],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.INTERNAL, message, details }, { cause });
  }
}

export interface UnavailableError extends NonOkStatus {
  code: StatusCode.UNAVAILABLE;
}

export function isUnavailableError(val: Status): val is UnavailableError;
export function isUnavailableError<T>(
  val: StatusOr<T>,
): val is UnavailableError {
  return isStatus(val) && (val as Status).code === StatusCode.UNAVAILABLE;
}

export const unavailableError = (
  message: string = kDefaultStatusMessages[StatusCode.UNAVAILABLE],
  details?: object[],
  cause?: unknown,
): UnavailableError => {
  const status: UnavailableError = {
    code: StatusCode.UNAVAILABLE,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class UnavailableException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.UNAVAILABLE],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.UNAVAILABLE, message, details }, { cause });
  }
}

export interface DataLossError extends NonOkStatus {
  code: StatusCode.DATA_LOSS;
}

export function isDataLossError(val: Status): val is DataLossError;
export function isDataLossError<T>(val: StatusOr<T>): val is DataLossError {
  return isStatus(val) && (val as Status).code === StatusCode.DATA_LOSS;
}

export const dataLossError = (
  message: string = kDefaultStatusMessages[StatusCode.DATA_LOSS],
  details?: object[],
  cause?: unknown,
): DataLossError => {
  const status: DataLossError = {
    code: StatusCode.DATA_LOSS,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class DataLossException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.DATA_LOSS],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.DATA_LOSS, message, details }, { cause });
  }
}

export interface UnauthenticatedError extends NonOkStatus {
  code: StatusCode.UNAUTHENTICATED;
}

export function isUnauthenticatedError(
  val: Status,
): val is UnauthenticatedError;
export function isUnauthenticatedError<T>(
  val: StatusOr<T>,
): val is UnauthenticatedError {
  return isStatus(val) && (val as Status).code === StatusCode.UNAUTHENTICATED;
}

export const unauthenticatedError = (
  message: string = kDefaultStatusMessages[StatusCode.UNAUTHENTICATED],
  details?: object[],
  cause?: unknown,
): UnauthenticatedError => {
  const status: UnauthenticatedError = {
    code: StatusCode.UNAUTHENTICATED,
    message,
  };
  if (details !== undefined) {
    status.details = details;
  }
  if (cause !== undefined) {
    status.cause = cause;
  }
  return status;
};

export class UnauthenticatedException extends StatusException {
  constructor(
    message: string = kDefaultStatusMessages[StatusCode.UNAUTHENTICATED],
    details?: object[],
    cause?: unknown,
  ) {
    super({ code: StatusCode.UNAUTHENTICATED, message, details }, { cause });
  }
}

export const okStatus = (message?: string): Status => ({
  code: StatusCode.OK,
  message: message ?? 'OK',
});

export function throwForError<T>(val: StatusOr<T>): asserts val is T;

export function throwForError(val: Status) {
  if (isOk(val)) return;
  switch (val.code) {
    case StatusCode.CANCELLED:
      throw new CancelledException(val.message, val.details, val.cause);
    case StatusCode.UNKNOWN:
      throw new UnknownException(val.message, val.details, val.cause);
    case StatusCode.INVALID_ARGUMENT:
      throw new InvalidArgumentException(val.message, val.details, val.cause);
    case StatusCode.DEADLINE_EXCEEDED:
      throw new DeadlineExceededException(val.message, val.details, val.cause);
    case StatusCode.NOT_FOUND:
      throw new NotFoundException(val.message, val.details, val.cause);
    case StatusCode.ALREADY_EXISTS:
      throw new AlreadyExistsException(val.message, val.details, val.cause);
    case StatusCode.PERMISSION_DENIED:
      throw new PermissionDeniedException(val.message, val.details, val.cause);
    case StatusCode.RESOURCE_EXHAUSTED:
      throw new ResourceExhaustedException(val.message, val.details, val.cause);
    case StatusCode.FAILED_PRECONDITION:
      throw new FailedPreconditionException(
        val.message,
        val.details,
        val.cause,
      );
    case StatusCode.ABORTED:
      throw new AbortedException(val.message, val.details, val.cause);
    case StatusCode.OUT_OF_RANGE:
      throw new OutOfRangeException(val.message, val.details, val.cause);
    case StatusCode.UNIMPLEMENTED:
      throw new UnimplementedException(val.message, val.details, val.cause);
    case StatusCode.INTERNAL:
      throw new InternalException(val.message, val.details, val.cause);
    case StatusCode.UNAVAILABLE:
      throw new UnavailableException(val.message, val.details, val.cause);
    case StatusCode.DATA_LOSS:
      throw new DataLossException(val.message, val.details, val.cause);
    case StatusCode.UNAUTHENTICATED:
      throw new UnauthenticatedException(val.message, val.details, val.cause);
  }
}

const kOkStatusSingleton = okStatus();

export function getStatusAndValue<T>(val: StatusOr<T>): [Status, T | unknown] {
  if (!isOk(val)) {
    return [val as Status, undefined];
  }
  return [{ ...kOkStatusSingleton }, val];
}

export function valueOrThrow<T>(val: StatusOr<T>): T {
  const [valIsStatus] = isStatusAndIsOk(val);
  if (!valIsStatus) {
    return val as T;
  }
  throwForError(val as Status);
  return val as T;
}

export function valueOrTerminate<T>(val: StatusOr<T>): T {
  const [valIsStatus, valIsOk] = isStatusAndIsOk(val);
  if (valIsStatus && !valIsOk) {
    terminate(val as Status);
  }
  return val as T;
}

function isPromiseLike<T>(value: unknown): value is PromiseLike<T> {
  return (
    value !== null &&
    typeof value === 'object' &&
    typeof (value as PromiseLike<T>).then === 'function'
  );
}

export function statusFromUnknown(
  err: unknown,
  message?: string,
  code: StatusCode = StatusCode.UNKNOWN,
): NonOkStatus {
  if (err instanceof StatusException) {
    return err.status() as NonOkStatus;
  }

  const errorCode = code === StatusCode.OK ? StatusCode.UNKNOWN : code;

  if (err instanceof Error) {
    return {
      code: errorCode,
      message: message ?? err.message,
      cause: err,
    };
  }

  return {
    code: errorCode,
    message: message ?? 'Unknown error.',
    cause: err,
  };
}

export function noexcept<T>(callback: () => T, message?: string): StatusOr<T>;

export function noexcept<T>(
  callback: () => Promise<T>,
  message?: string,
): Promise<StatusOr<T>>;

export function noexcept<T>(
  callback: () => T | Promise<T>,
  message?: string,
): StatusOr<T> | Promise<StatusOr<T>> {
  try {
    const result = callback();
    if (isPromiseLike(result)) {
      return result.catch((err) => statusFromUnknown(err, message));
    }
    return result;
  } catch (err) {
    return statusFromUnknown(err, message);
  }
}

export async function noexceptFetch(
  input: string | URL | globalThis.Request,
  init?: RequestInit,
): Promise<StatusOr<Response>> {
  try {
    return await globalThis.fetch(input, init);
  } catch (err) {
    if (
      typeof DOMException !== 'undefined' &&
      err instanceof DOMException &&
      err.name === 'AbortError'
    ) {
      return abortedError(err.message, [err], err);
    }
    if (err instanceof TypeError && err.message.includes('Failed to fetch')) {
      return internalError(
        `${err.message}. This is likely due to a network issue or CORS policy. Check console for more details.`,
        [],
        err,
      );
    }
    return statusFromUnknown(err, undefined, StatusCode.UNAVAILABLE);
  }
}

/** JSON representation shared by A11's HTTP and signalling boundaries. */
export interface StatusJson {
  code: number;
  message: string;
  details: object[];
}

export function statusToJson(status: Status): StatusJson {
  return {
    code: status.code,
    message: status.message,
    details: status.details ? [...status.details] : [],
  };
}

export function statusFromJson(value: unknown): StatusOr<Status> {
  try {
    if (typeof value !== 'object' || value === null) {
      return invalidArgumentError('JSON does not contain a valid Status.');
    }
    const candidate = value as Record<string, unknown>;
    if (
      !Number.isInteger(candidate.code) ||
      (candidate.code as number) < StatusCode.OK ||
      (candidate.code as number) > StatusCode.UNAUTHENTICATED ||
      typeof candidate.message !== 'string'
    ) {
      return invalidArgumentError('JSON does not contain a valid Status.');
    }
    const details = candidate.details ?? [];
    if (
      !Array.isArray(details) ||
      details.some(
        (detail) => typeof detail !== 'object' || detail === null,
      )
    ) {
      return invalidArgumentError('Status details must be an array of objects.');
    }
    return {
      code: candidate.code as StatusCode,
      message: candidate.message,
      details: details as object[],
    };
  } catch (error) {
    return invalidArgumentError(
      'JSON does not contain a readable Status.',
      [],
      error,
    );
  }
}

export function statusCodeFromHttp(httpCode: number): StatusCode {
  if (httpCode >= 200 && httpCode < 300) return StatusCode.OK;
  switch (httpCode) {
    case 400:
      return StatusCode.INVALID_ARGUMENT;
    case 401:
      return StatusCode.UNAUTHENTICATED;
    case 403:
      return StatusCode.PERMISSION_DENIED;
    case 404:
      return StatusCode.NOT_FOUND;
    case 409:
      return StatusCode.ABORTED;
    case 429:
      return StatusCode.RESOURCE_EXHAUSTED;
    case 501:
      return StatusCode.UNIMPLEMENTED;
    case 503:
      return StatusCode.UNAVAILABLE;
    default:
      if (httpCode >= 400 && httpCode < 500) {
        return StatusCode.FAILED_PRECONDITION;
      }
      if (httpCode >= 500 && httpCode < 600) return StatusCode.INTERNAL;
      return StatusCode.UNKNOWN;
  }
}

export function statusCodeFromWebSocket(closeCode: number): StatusCode {
  const privateCode = closeCode - 3999;
  if (closeCode === 1000) return StatusCode.OK;
  if (privateCode >= 1 && privateCode <= 15) {
    return privateCode as StatusCode;
  }
  if (closeCode === 4007) return StatusCode.UNAUTHENTICATED;
  switch (closeCode) {
    case 1001:
      return StatusCode.ABORTED;
    case 1002:
    case 1003:
    case 1007:
      return StatusCode.INVALID_ARGUMENT;
    case 1008:
      return StatusCode.PERMISSION_DENIED;
    case 1009:
      return StatusCode.RESOURCE_EXHAUSTED;
    case 1011:
      return StatusCode.INTERNAL;
    case 1012:
    case 1013:
      return StatusCode.UNAVAILABLE;
    default:
      return StatusCode.UNKNOWN;
  }
}

export function statusCodeToWebSocket(code: StatusCode): number {
  if (code === StatusCode.OK) return 1000;
  if (code >= StatusCode.CANCELLED && code <= StatusCode.DATA_LOSS) {
    return 3999 + code;
  }
  if (code === StatusCode.UNAUTHENTICATED) return 4007;
  return 4001;
}

/**
 * Converts a non-success Fetch response without parsing its body more than
 * once. A JSON A11 Status is preserved when the peer supplied one.
 */
export async function statusFromResponse(
  response: Response,
  operation: string = 'HTTP request',
): Promise<Status> {
  let httpStatus: number;
  try {
    if (typeof response !== 'object' || response === null) {
      return invalidArgumentError(`${operation} did not return a Response.`);
    }
    if (response.ok) return okStatus();
    httpStatus = response.status;
    if (!Number.isInteger(httpStatus)) {
      return dataLossError(`${operation} returned an invalid HTTP status.`);
    }
  } catch (error) {
    return statusFromUnknown(
      error,
      `${operation} returned an unreadable HTTP response.`,
      StatusCode.UNAVAILABLE,
    );
  }
  let body = '';
  try {
    body = await response.text();
  } catch (error) {
    return statusFromUnknown(
      error,
      `${operation} returned HTTP ${httpStatus}, and its response body could not be read.`,
      statusCodeFromHttp(httpStatus),
    );
  }
  if (body) {
    try {
      const decoded: unknown = JSON.parse(body);
      const parsed = statusFromJson(decoded);
      if (isStatus(parsed) && parsed.code !== StatusCode.OK) return parsed;
    } catch {
      // A plain-text response body is the error message used below.
    }
  }
  return {
    code: statusCodeFromHttp(httpStatus),
    message: body || `${operation} returned HTTP ${httpStatus}.`,
  };
}

var __create = Object.create;
var __defProp = Object.defineProperty;
var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
var __getOwnPropNames = Object.getOwnPropertyNames;
var __getProtoOf = Object.getPrototypeOf;
var __hasOwnProp = Object.prototype.hasOwnProperty;
var __commonJS = (cb, mod) => function __require() {
  return mod || (0, cb[__getOwnPropNames(cb)[0]])((mod = { exports: {} }).exports, mod), mod.exports;
};
var __copyProps = (to, from, except, desc) => {
  if (from && typeof from === "object" || typeof from === "function") {
    for (let key of __getOwnPropNames(from))
      if (!__hasOwnProp.call(to, key) && key !== except)
        __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
  }
  return to;
};
var __toESM = (mod, isNodeMode, target) => (target = mod != null ? __create(__getProtoOf(mod)) : {}, __copyProps(
  // If the importer is in node compatibility mode or this is not an ESM
  // file that has been converted to a CommonJS file using a Babel-
  // compatible transform (i.e. "__esModule" has not been set), then set
  // "default" to the CommonJS "module.exports" for node compatibility.
  isNodeMode || !mod || !mod.__esModule ? __defProp(target, "default", { value: mod, enumerable: true }) : target,
  mod
));

// node_modules/ws/browser.js
var require_browser = __commonJS({
  "node_modules/ws/browser.js"(exports, module) {
    "use strict";
    module.exports = function() {
      throw new Error(
        "ws does not work in the browser. Browser clients must use the native WebSocket object"
      );
    };
  }
});

// src/status.ts
var StatusCode = /* @__PURE__ */ ((StatusCode2) => {
  StatusCode2[StatusCode2["OK"] = 0] = "OK";
  StatusCode2[StatusCode2["CANCELLED"] = 1] = "CANCELLED";
  StatusCode2[StatusCode2["UNKNOWN"] = 2] = "UNKNOWN";
  StatusCode2[StatusCode2["INVALID_ARGUMENT"] = 3] = "INVALID_ARGUMENT";
  StatusCode2[StatusCode2["DEADLINE_EXCEEDED"] = 4] = "DEADLINE_EXCEEDED";
  StatusCode2[StatusCode2["NOT_FOUND"] = 5] = "NOT_FOUND";
  StatusCode2[StatusCode2["ALREADY_EXISTS"] = 6] = "ALREADY_EXISTS";
  StatusCode2[StatusCode2["PERMISSION_DENIED"] = 7] = "PERMISSION_DENIED";
  StatusCode2[StatusCode2["RESOURCE_EXHAUSTED"] = 8] = "RESOURCE_EXHAUSTED";
  StatusCode2[StatusCode2["FAILED_PRECONDITION"] = 9] = "FAILED_PRECONDITION";
  StatusCode2[StatusCode2["ABORTED"] = 10] = "ABORTED";
  StatusCode2[StatusCode2["OUT_OF_RANGE"] = 11] = "OUT_OF_RANGE";
  StatusCode2[StatusCode2["UNIMPLEMENTED"] = 12] = "UNIMPLEMENTED";
  StatusCode2[StatusCode2["INTERNAL"] = 13] = "INTERNAL";
  StatusCode2[StatusCode2["UNAVAILABLE"] = 14] = "UNAVAILABLE";
  StatusCode2[StatusCode2["DATA_LOSS"] = 15] = "DATA_LOSS";
  StatusCode2[StatusCode2["UNAUTHENTICATED"] = 16] = "UNAUTHENTICATED";
  return StatusCode2;
})(StatusCode || {});
var kDefaultStatusMessages = {
  [0 /* OK */]: "OK",
  [1 /* CANCELLED */]: "Cancelled",
  [2 /* UNKNOWN */]: "Unknown",
  [3 /* INVALID_ARGUMENT */]: "Invalid Argument",
  [4 /* DEADLINE_EXCEEDED */]: "Deadline Exceeded",
  [5 /* NOT_FOUND */]: "Not Found",
  [6 /* ALREADY_EXISTS */]: "Already Exists",
  [7 /* PERMISSION_DENIED */]: "Permission Denied",
  [8 /* RESOURCE_EXHAUSTED */]: "Resource Exhausted",
  [9 /* FAILED_PRECONDITION */]: "Failed Precondition",
  [10 /* ABORTED */]: "Aborted",
  [11 /* OUT_OF_RANGE */]: "Out of Range",
  [12 /* UNIMPLEMENTED */]: "Unimplemented",
  [13 /* INTERNAL */]: "Internal",
  [14 /* UNAVAILABLE */]: "Unavailable",
  [15 /* DATA_LOSS */]: "Data Loss",
  [16 /* UNAUTHENTICATED */]: "Unauthenticated"
};
var isStatusAndIsOk = (val) => {
  try {
    if (typeof val !== "object" || val === null) {
      return [false, true];
    }
    if (!("code" in val) || !("message" in val)) {
      return [false, true];
    }
    const candidate = val;
    const keys = Object.keys(candidate);
    if (keys.some(
      (key) => key !== "code" && key !== "message" && key !== "details" && key !== "cause"
    ) || !Number.isInteger(candidate.code) || candidate.code < 0 /* OK */ || candidate.code > 16 /* UNAUTHENTICATED */ || typeof candidate.message !== "string") {
      return [false, true];
    }
    if (candidate.details !== void 0 && (!Array.isArray(candidate.details) || candidate.details.some(
      (detail) => typeof detail !== "object" || detail === null
    ))) {
      return [false, true];
    }
    return [true, candidate.code === 0 /* OK */];
  } catch {
    return [false, true];
  }
};
var isStatus = (val) => {
  return isStatusAndIsOk(val)[0];
};
function isOk(val) {
  return isStatusAndIsOk(val)[1];
}
function terminate(message_or_status) {
  let paramIsStatus = isStatus(message_or_status);
  let codeRepr = "UNKNOWN";
  let message;
  if (paramIsStatus) {
    codeRepr = StatusCode[message_or_status.code];
    message = message_or_status.message;
  } else {
    message = message_or_status;
  }
  console.error(`[${codeRepr}] ${message}`);
  if (typeof process !== "undefined" && process.abort) {
    process.abort();
  }
  if (paramIsStatus) {
    throw new StatusException(message_or_status);
  }
  throw new Error(message);
}
var StatusException = class extends Error {
  /** Clean copy of the status represented by this exception. */
  _status;
  constructor(status, options) {
    if (isOk(status)) {
      terminate(
        invalidArgumentError(
          "StatusException cannot be created for OK status."
        )
      );
    }
    if (options && options.cause && status.cause) {
      terminate(
        invalidArgumentError(
          "StatusException cannot have cause both set in status and options."
        )
      );
    }
    if (options && options.cause) {
      status = { ...status, cause: options.cause };
    }
    super(status.message, { ...options, cause: status.cause });
    const cleanStatus = { code: status.code, message: status.message };
    if (status.details !== void 0) {
      cleanStatus.details = status.details;
    }
    if (status.cause !== void 0) {
      cleanStatus.cause = status.cause;
    }
    this._status = cleanStatus;
  }
  /** Return the structured status for forwarding to another A11 boundary. */
  status() {
    return this._status;
  }
};
var cancelledError = (message = kDefaultStatusMessages[1 /* CANCELLED */], details, cause) => {
  const status = {
    code: 1 /* CANCELLED */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var unknownError = (message = kDefaultStatusMessages[2 /* UNKNOWN */], details, cause) => {
  const status = {
    code: 2 /* UNKNOWN */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var invalidArgumentError = (message = kDefaultStatusMessages[3 /* INVALID_ARGUMENT */], details, cause) => {
  const status = {
    code: 3 /* INVALID_ARGUMENT */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var deadlineExceededError = (message = kDefaultStatusMessages[4 /* DEADLINE_EXCEEDED */], details, cause) => {
  const status = {
    code: 4 /* DEADLINE_EXCEEDED */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
function isNotFoundError(val) {
  return isStatus(val) && val.code === 5 /* NOT_FOUND */;
}
var notFoundError = (message = kDefaultStatusMessages[5 /* NOT_FOUND */], details, cause) => {
  const status = {
    code: 5 /* NOT_FOUND */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var alreadyExistsError = (message = kDefaultStatusMessages[6 /* ALREADY_EXISTS */], details, cause) => {
  const status = {
    code: 6 /* ALREADY_EXISTS */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var resourceExhaustedError = (message = kDefaultStatusMessages[8 /* RESOURCE_EXHAUSTED */], details, cause) => {
  const status = {
    code: 8 /* RESOURCE_EXHAUSTED */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var failedPreconditionError = (message = kDefaultStatusMessages[9 /* FAILED_PRECONDITION */], details, cause) => {
  const status = {
    code: 9 /* FAILED_PRECONDITION */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var abortedError = (message = kDefaultStatusMessages[10 /* ABORTED */], details, cause) => {
  const status = {
    code: 10 /* ABORTED */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var outOfRangeError = (message = kDefaultStatusMessages[11 /* OUT_OF_RANGE */], details, cause) => {
  const status = {
    code: 11 /* OUT_OF_RANGE */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var unimplementedError = (message = kDefaultStatusMessages[12 /* UNIMPLEMENTED */], details, cause) => {
  const status = {
    code: 12 /* UNIMPLEMENTED */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var internalError = (message = kDefaultStatusMessages[13 /* INTERNAL */], details, cause) => {
  const status = {
    code: 13 /* INTERNAL */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var unavailableError = (message = kDefaultStatusMessages[14 /* UNAVAILABLE */], details, cause) => {
  const status = {
    code: 14 /* UNAVAILABLE */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var dataLossError = (message = kDefaultStatusMessages[15 /* DATA_LOSS */], details, cause) => {
  const status = {
    code: 15 /* DATA_LOSS */,
    message
  };
  if (details !== void 0) {
    status.details = details;
  }
  if (cause !== void 0) {
    status.cause = cause;
  }
  return status;
};
var okStatus = (message) => ({
  code: 0 /* OK */,
  message: message ?? "OK"
});
var kOkStatusSingleton = okStatus();
function statusFromUnknown(err, message, code = 2 /* UNKNOWN */) {
  if (err instanceof StatusException) {
    return err.status();
  }
  const errorCode = code === 0 /* OK */ ? 2 /* UNKNOWN */ : code;
  if (err instanceof Error) {
    return {
      code: errorCode,
      message: message ?? err.message,
      cause: err
    };
  }
  return {
    code: errorCode,
    message: message ?? "Unknown error.",
    cause: err
  };
}
function statusToJson(status) {
  return {
    code: status.code,
    message: status.message,
    details: status.details ? [...status.details] : []
  };
}
function statusCodeFromWebSocket(closeCode) {
  const privateCode = closeCode - 3999;
  if (closeCode === 1e3) return 0 /* OK */;
  if (privateCode >= 1 && privateCode <= 15) {
    return privateCode;
  }
  if (closeCode === 4007) return 16 /* UNAUTHENTICATED */;
  switch (closeCode) {
    case 1001:
      return 10 /* ABORTED */;
    case 1002:
    case 1003:
    case 1007:
      return 3 /* INVALID_ARGUMENT */;
    case 1008:
      return 7 /* PERMISSION_DENIED */;
    case 1009:
      return 8 /* RESOURCE_EXHAUSTED */;
    case 1011:
      return 13 /* INTERNAL */;
    case 1012:
    case 1013:
      return 14 /* UNAVAILABLE */;
    default:
      return 2 /* UNKNOWN */;
  }
}

// src/bytes.ts
var encoder = new TextEncoder();
var decoder = new TextDecoder("utf-8", { fatal: true });
function toBytes(value) {
  try {
    if (typeof value === "string") return encoder.encode(value);
    if (value instanceof Uint8Array) return new Uint8Array(value);
    if (value instanceof ArrayBuffer) return new Uint8Array(value.slice(0));
    if (ArrayBuffer.isView(value)) {
      return new Uint8Array(
        value.buffer.slice(
          value.byteOffset,
          value.byteOffset + value.byteLength
        )
      );
    }
    return invalidArgumentError("Expected bytes, an ArrayBuffer, or a string.");
  } catch (error) {
    return statusFromUnknown(error, "Could not copy byte data.");
  }
}
async function toBytesAsync(value) {
  if (typeof Blob !== "undefined" && value instanceof Blob) {
    try {
      return new Uint8Array(await value.arrayBuffer());
    } catch (error) {
      return statusFromUnknown(error, "Could not read Blob data.");
    }
  }
  return toBytes(value);
}
function utf8Encode(value) {
  return encoder.encode(value);
}
function utf8Decode(value) {
  try {
    return decoder.decode(value);
  } catch (error) {
    return invalidArgumentError("Byte data is not valid UTF-8.", [], error);
  }
}
function concatBytes(parts) {
  let length = 0;
  for (const part of parts) length += part.byteLength;
  const result = new Uint8Array(length);
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.byteLength;
  }
  return result;
}
var base64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function base64Encode(bytes) {
  let result = "";
  for (let index = 0; index < bytes.length; index += 3) {
    const first = bytes[index] ?? 0;
    const second = bytes[index + 1] ?? 0;
    const third = bytes[index + 2] ?? 0;
    const value = first << 16 | second << 8 | third;
    result += base64Alphabet[value >>> 18 & 63];
    result += base64Alphabet[value >>> 12 & 63];
    result += index + 1 < bytes.length ? base64Alphabet[value >>> 6 & 63] : "=";
    result += index + 2 < bytes.length ? base64Alphabet[value & 63] : "=";
  }
  return result;
}
function base64Decode(value) {
  if (typeof value !== "string" || value.length % 4 !== 0 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(
    value
  )) {
    return invalidArgumentError("Value is not valid base64.");
  }
  const padding = value.endsWith("==") ? 2 : value.endsWith("=") ? 1 : 0;
  const output = new Uint8Array(value.length / 4 * 3 - padding);
  let outputIndex = 0;
  for (let index = 0; index < value.length; index += 4) {
    const a = base64Alphabet.indexOf(value[index] ?? "A");
    const b = base64Alphabet.indexOf(value[index + 1] ?? "A");
    const c = value[index + 2] === "=" ? 0 : base64Alphabet.indexOf(value[index + 2] ?? "A");
    const d = value[index + 3] === "=" ? 0 : base64Alphabet.indexOf(value[index + 3] ?? "A");
    const bits = a << 18 | b << 12 | c << 6 | d;
    if (outputIndex < output.length) output[outputIndex++] = bits >>> 16;
    if (outputIndex < output.length) output[outputIndex++] = bits >>> 8;
    if (outputIndex < output.length) output[outputIndex++] = bits;
  }
  return output;
}
function normalizeByteMap(values, validateKey) {
  const result = /* @__PURE__ */ new Map();
  if (values === void 0) return result;
  try {
    const entries = values instanceof Map ? values.entries() : Object.entries(values);
    for (const [key, source] of entries) {
      if (validateKey !== void 0 && !validateKey(key)) {
        return invalidArgumentError(`Invalid byte-map key: ${key}.`);
      }
      const bytes = toBytes(source);
      if (typeof bytes === "object" && "code" in bytes && "message" in bytes) {
        return bytes;
      }
      result.set(key, bytes);
    }
  } catch (error) {
    return statusFromUnknown(error, "Could not normalize byte map.");
  }
  return result;
}
function copyByteMap(values) {
  return new Map(
    [...values].map(([key, value]) => [key, new Uint8Array(value)])
  );
}
function randomId(prefix) {
  try {
    const value = globalThis.crypto?.randomUUID?.();
    if (value !== void 0) return `${prefix}${value.replaceAll("-", "")}`;
  } catch {
  }
  const random = Math.floor(Math.random() * Number.MAX_SAFE_INTEGER).toString(16);
  return `${prefix}${Date.now().toString(16)}${random}`;
}

// node_modules/@msgpack/msgpack/dist.esm/utils/utf8.mjs
function utf8Count(str) {
  const strLength = str.length;
  let byteLength = 0;
  let pos = 0;
  while (pos < strLength) {
    let value = str.charCodeAt(pos++);
    if ((value & 4294967168) === 0) {
      byteLength++;
      continue;
    } else if ((value & 4294965248) === 0) {
      byteLength += 2;
    } else {
      if (value >= 55296 && value <= 56319) {
        if (pos < strLength) {
          const extra = str.charCodeAt(pos);
          if ((extra & 64512) === 56320) {
            ++pos;
            value = ((value & 1023) << 10) + (extra & 1023) + 65536;
          }
        }
      }
      if ((value & 4294901760) === 0) {
        byteLength += 3;
      } else {
        byteLength += 4;
      }
    }
  }
  return byteLength;
}
function utf8EncodeJs(str, output, outputOffset) {
  const strLength = str.length;
  let offset = outputOffset;
  let pos = 0;
  while (pos < strLength) {
    let value = str.charCodeAt(pos++);
    if ((value & 4294967168) === 0) {
      output[offset++] = value;
      continue;
    } else if ((value & 4294965248) === 0) {
      output[offset++] = value >> 6 & 31 | 192;
    } else {
      if (value >= 55296 && value <= 56319) {
        if (pos < strLength) {
          const extra = str.charCodeAt(pos);
          if ((extra & 64512) === 56320) {
            ++pos;
            value = ((value & 1023) << 10) + (extra & 1023) + 65536;
          }
        }
      }
      if ((value & 4294901760) === 0) {
        output[offset++] = value >> 12 & 15 | 224;
        output[offset++] = value >> 6 & 63 | 128;
      } else {
        output[offset++] = value >> 18 & 7 | 240;
        output[offset++] = value >> 12 & 63 | 128;
        output[offset++] = value >> 6 & 63 | 128;
      }
    }
    output[offset++] = value & 63 | 128;
  }
}
var sharedTextEncoder = new TextEncoder();
var TEXT_ENCODER_THRESHOLD = 50;
function utf8EncodeTE(str, output, outputOffset) {
  sharedTextEncoder.encodeInto(str, output.subarray(outputOffset));
}
function utf8Encode2(str, output, outputOffset) {
  if (str.length > TEXT_ENCODER_THRESHOLD) {
    utf8EncodeTE(str, output, outputOffset);
  } else {
    utf8EncodeJs(str, output, outputOffset);
  }
}
var CHUNK_SIZE = 4096;
function utf8DecodeJs(bytes, inputOffset, byteLength) {
  let offset = inputOffset;
  const end = offset + byteLength;
  const units = [];
  let result = "";
  while (offset < end) {
    const byte1 = bytes[offset++];
    if ((byte1 & 128) === 0) {
      units.push(byte1);
    } else if ((byte1 & 224) === 192) {
      const byte2 = bytes[offset++] & 63;
      units.push((byte1 & 31) << 6 | byte2);
    } else if ((byte1 & 240) === 224) {
      const byte2 = bytes[offset++] & 63;
      const byte3 = bytes[offset++] & 63;
      units.push((byte1 & 31) << 12 | byte2 << 6 | byte3);
    } else if ((byte1 & 248) === 240) {
      const byte2 = bytes[offset++] & 63;
      const byte3 = bytes[offset++] & 63;
      const byte4 = bytes[offset++] & 63;
      let unit = (byte1 & 7) << 18 | byte2 << 12 | byte3 << 6 | byte4;
      if (unit > 65535) {
        unit -= 65536;
        units.push(unit >>> 10 & 1023 | 55296);
        unit = 56320 | unit & 1023;
      }
      units.push(unit);
    } else {
      units.push(byte1);
    }
    if (units.length >= CHUNK_SIZE) {
      result += String.fromCharCode(...units);
      units.length = 0;
    }
  }
  if (units.length > 0) {
    result += String.fromCharCode(...units);
  }
  return result;
}
var sharedTextDecoder = new TextDecoder();
var TEXT_DECODER_THRESHOLD = 200;
function utf8DecodeTD(bytes, inputOffset, byteLength) {
  const stringBytes = bytes.subarray(inputOffset, inputOffset + byteLength);
  return sharedTextDecoder.decode(stringBytes);
}
function utf8Decode2(bytes, inputOffset, byteLength) {
  if (byteLength > TEXT_DECODER_THRESHOLD) {
    return utf8DecodeTD(bytes, inputOffset, byteLength);
  } else {
    return utf8DecodeJs(bytes, inputOffset, byteLength);
  }
}

// node_modules/@msgpack/msgpack/dist.esm/ExtData.mjs
var ExtData = class {
  type;
  data;
  constructor(type, data) {
    this.type = type;
    this.data = data;
  }
};

// node_modules/@msgpack/msgpack/dist.esm/DecodeError.mjs
var DecodeError = class _DecodeError extends Error {
  constructor(message) {
    super(message);
    const proto = Object.create(_DecodeError.prototype);
    Object.setPrototypeOf(this, proto);
    Object.defineProperty(this, "name", {
      configurable: true,
      enumerable: false,
      value: _DecodeError.name
    });
  }
};

// node_modules/@msgpack/msgpack/dist.esm/utils/int.mjs
var UINT32_MAX = 4294967295;
function setUint64(view, offset, value) {
  const high = value / 4294967296;
  const low = value;
  view.setUint32(offset, high);
  view.setUint32(offset + 4, low);
}
function setInt64(view, offset, value) {
  const high = Math.floor(value / 4294967296);
  const low = value;
  view.setUint32(offset, high);
  view.setUint32(offset + 4, low);
}
function getInt64(view, offset) {
  const high = view.getInt32(offset);
  const low = view.getUint32(offset + 4);
  return high * 4294967296 + low;
}
function getUint64(view, offset) {
  const high = view.getUint32(offset);
  const low = view.getUint32(offset + 4);
  return high * 4294967296 + low;
}

// node_modules/@msgpack/msgpack/dist.esm/timestamp.mjs
var EXT_TIMESTAMP = -1;
var TIMESTAMP32_MAX_SEC = 4294967296 - 1;
var TIMESTAMP64_MAX_SEC = 17179869184 - 1;
function encodeTimeSpecToTimestamp({ sec, nsec }) {
  if (sec >= 0 && nsec >= 0 && sec <= TIMESTAMP64_MAX_SEC) {
    if (nsec === 0 && sec <= TIMESTAMP32_MAX_SEC) {
      const rv = new Uint8Array(4);
      const view = new DataView(rv.buffer);
      view.setUint32(0, sec);
      return rv;
    } else {
      const secHigh = sec / 4294967296;
      const secLow = sec & 4294967295;
      const rv = new Uint8Array(8);
      const view = new DataView(rv.buffer);
      view.setUint32(0, nsec << 2 | secHigh & 3);
      view.setUint32(4, secLow);
      return rv;
    }
  } else {
    const rv = new Uint8Array(12);
    const view = new DataView(rv.buffer);
    view.setUint32(0, nsec);
    setInt64(view, 4, sec);
    return rv;
  }
}
function encodeDateToTimeSpec(date) {
  const msec = date.getTime();
  const sec = Math.floor(msec / 1e3);
  const nsec = (msec - sec * 1e3) * 1e6;
  const nsecInSec = Math.floor(nsec / 1e9);
  return {
    sec: sec + nsecInSec,
    nsec: nsec - nsecInSec * 1e9
  };
}
function encodeTimestampExtension(object) {
  if (object instanceof Date) {
    const timeSpec = encodeDateToTimeSpec(object);
    return encodeTimeSpecToTimestamp(timeSpec);
  } else {
    return null;
  }
}
function decodeTimestampToTimeSpec(data) {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  switch (data.byteLength) {
    case 4: {
      const sec = view.getUint32(0);
      const nsec = 0;
      return { sec, nsec };
    }
    case 8: {
      const nsec30AndSecHigh2 = view.getUint32(0);
      const secLow32 = view.getUint32(4);
      const sec = (nsec30AndSecHigh2 & 3) * 4294967296 + secLow32;
      const nsec = nsec30AndSecHigh2 >>> 2;
      return { sec, nsec };
    }
    case 12: {
      const sec = getInt64(view, 4);
      const nsec = view.getUint32(0);
      return { sec, nsec };
    }
    default:
      throw new DecodeError(`Unrecognized data size for timestamp (expected 4, 8, or 12): ${data.length}`);
  }
}
function decodeTimestampExtension(data) {
  const timeSpec = decodeTimestampToTimeSpec(data);
  return new Date(timeSpec.sec * 1e3 + timeSpec.nsec / 1e6);
}
var timestampExtension = {
  type: EXT_TIMESTAMP,
  encode: encodeTimestampExtension,
  decode: decodeTimestampExtension
};

// node_modules/@msgpack/msgpack/dist.esm/ExtensionCodec.mjs
var ExtensionCodec = class _ExtensionCodec {
  static defaultCodec = new _ExtensionCodec();
  // ensures ExtensionCodecType<X> matches ExtensionCodec<X>
  // this will make type errors a lot more clear
  // eslint-disable-next-line @typescript-eslint/naming-convention
  __brand;
  // built-in extensions
  builtInEncoders = [];
  builtInDecoders = [];
  // custom extensions
  encoders = [];
  decoders = [];
  constructor() {
    this.register(timestampExtension);
  }
  register({ type, encode: encode2, decode: decode2 }) {
    if (type >= 0) {
      this.encoders[type] = encode2;
      this.decoders[type] = decode2;
    } else {
      const index = -1 - type;
      this.builtInEncoders[index] = encode2;
      this.builtInDecoders[index] = decode2;
    }
  }
  tryToEncode(object, context) {
    for (let i = 0; i < this.builtInEncoders.length; i++) {
      const encodeExt = this.builtInEncoders[i];
      if (encodeExt != null) {
        const data = encodeExt(object, context);
        if (data != null) {
          const type = -1 - i;
          return new ExtData(type, data);
        }
      }
    }
    for (let i = 0; i < this.encoders.length; i++) {
      const encodeExt = this.encoders[i];
      if (encodeExt != null) {
        const data = encodeExt(object, context);
        if (data != null) {
          const type = i;
          return new ExtData(type, data);
        }
      }
    }
    if (object instanceof ExtData) {
      return object;
    }
    return null;
  }
  decode(data, type, context) {
    const decodeExt = type < 0 ? this.builtInDecoders[-1 - type] : this.decoders[type];
    if (decodeExt) {
      return decodeExt(data, type, context);
    } else {
      return new ExtData(type, data);
    }
  }
};

// node_modules/@msgpack/msgpack/dist.esm/utils/typedArrays.mjs
function isArrayBufferLike(buffer) {
  return buffer instanceof ArrayBuffer || typeof SharedArrayBuffer !== "undefined" && buffer instanceof SharedArrayBuffer;
}
function ensureUint8Array(buffer) {
  if (buffer instanceof Uint8Array) {
    return buffer;
  } else if (ArrayBuffer.isView(buffer)) {
    return new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  } else if (isArrayBufferLike(buffer)) {
    return new Uint8Array(buffer);
  } else {
    return Uint8Array.from(buffer);
  }
}

// node_modules/@msgpack/msgpack/dist.esm/Encoder.mjs
var DEFAULT_MAX_DEPTH = 100;
var DEFAULT_INITIAL_BUFFER_SIZE = 2048;
var Encoder = class _Encoder {
  extensionCodec;
  context;
  useBigInt64;
  maxDepth;
  initialBufferSize;
  sortKeys;
  forceFloat32;
  ignoreUndefined;
  forceIntegerToFloat;
  pos;
  view;
  bytes;
  entered = false;
  constructor(options) {
    this.extensionCodec = options?.extensionCodec ?? ExtensionCodec.defaultCodec;
    this.context = options?.context;
    this.useBigInt64 = options?.useBigInt64 ?? false;
    this.maxDepth = options?.maxDepth ?? DEFAULT_MAX_DEPTH;
    this.initialBufferSize = options?.initialBufferSize ?? DEFAULT_INITIAL_BUFFER_SIZE;
    this.sortKeys = options?.sortKeys ?? false;
    this.forceFloat32 = options?.forceFloat32 ?? false;
    this.ignoreUndefined = options?.ignoreUndefined ?? false;
    this.forceIntegerToFloat = options?.forceIntegerToFloat ?? false;
    this.pos = 0;
    this.view = new DataView(new ArrayBuffer(this.initialBufferSize));
    this.bytes = new Uint8Array(this.view.buffer);
  }
  clone() {
    return new _Encoder({
      extensionCodec: this.extensionCodec,
      context: this.context,
      useBigInt64: this.useBigInt64,
      maxDepth: this.maxDepth,
      initialBufferSize: this.initialBufferSize,
      sortKeys: this.sortKeys,
      forceFloat32: this.forceFloat32,
      ignoreUndefined: this.ignoreUndefined,
      forceIntegerToFloat: this.forceIntegerToFloat
    });
  }
  reinitializeState() {
    this.pos = 0;
  }
  /**
   * This is almost equivalent to {@link Encoder#encode}, but it returns an reference of the encoder's internal buffer and thus much faster than {@link Encoder#encode}.
   *
   * @returns Encodes the object and returns a shared reference the encoder's internal buffer.
   */
  encodeSharedRef(object) {
    if (this.entered) {
      const instance = this.clone();
      return instance.encodeSharedRef(object);
    }
    try {
      this.entered = true;
      this.reinitializeState();
      this.doEncode(object, 1);
      return this.bytes.subarray(0, this.pos);
    } finally {
      this.entered = false;
    }
  }
  /**
   * @returns Encodes the object and returns a copy of the encoder's internal buffer.
   */
  encode(object) {
    if (this.entered) {
      const instance = this.clone();
      return instance.encode(object);
    }
    try {
      this.entered = true;
      this.reinitializeState();
      this.doEncode(object, 1);
      return this.bytes.slice(0, this.pos);
    } finally {
      this.entered = false;
    }
  }
  doEncode(object, depth) {
    if (depth > this.maxDepth) {
      throw new Error(`Too deep objects in depth ${depth}`);
    }
    if (object == null) {
      this.encodeNil();
    } else if (typeof object === "boolean") {
      this.encodeBoolean(object);
    } else if (typeof object === "number") {
      if (!this.forceIntegerToFloat) {
        this.encodeNumber(object);
      } else {
        this.encodeNumberAsFloat(object);
      }
    } else if (typeof object === "string") {
      this.encodeString(object);
    } else if (this.useBigInt64 && typeof object === "bigint") {
      this.encodeBigInt64(object);
    } else {
      this.encodeObject(object, depth);
    }
  }
  ensureBufferSizeToWrite(sizeToWrite) {
    const requiredSize = this.pos + sizeToWrite;
    if (this.view.byteLength < requiredSize) {
      this.resizeBuffer(requiredSize * 2);
    }
  }
  resizeBuffer(newSize) {
    const newBuffer = new ArrayBuffer(newSize);
    const newBytes = new Uint8Array(newBuffer);
    const newView = new DataView(newBuffer);
    newBytes.set(this.bytes);
    this.view = newView;
    this.bytes = newBytes;
  }
  encodeNil() {
    this.writeU8(192);
  }
  encodeBoolean(object) {
    if (object === false) {
      this.writeU8(194);
    } else {
      this.writeU8(195);
    }
  }
  encodeNumber(object) {
    if (!this.forceIntegerToFloat && Number.isSafeInteger(object)) {
      if (object >= 0) {
        if (object < 128) {
          this.writeU8(object);
        } else if (object < 256) {
          this.writeU8(204);
          this.writeU8(object);
        } else if (object < 65536) {
          this.writeU8(205);
          this.writeU16(object);
        } else if (object < 4294967296) {
          this.writeU8(206);
          this.writeU32(object);
        } else if (!this.useBigInt64) {
          this.writeU8(207);
          this.writeU64(object);
        } else {
          this.encodeNumberAsFloat(object);
        }
      } else {
        if (object >= -32) {
          this.writeU8(224 | object + 32);
        } else if (object >= -128) {
          this.writeU8(208);
          this.writeI8(object);
        } else if (object >= -32768) {
          this.writeU8(209);
          this.writeI16(object);
        } else if (object >= -2147483648) {
          this.writeU8(210);
          this.writeI32(object);
        } else if (!this.useBigInt64) {
          this.writeU8(211);
          this.writeI64(object);
        } else {
          this.encodeNumberAsFloat(object);
        }
      }
    } else {
      this.encodeNumberAsFloat(object);
    }
  }
  encodeNumberAsFloat(object) {
    if (this.forceFloat32) {
      this.writeU8(202);
      this.writeF32(object);
    } else {
      this.writeU8(203);
      this.writeF64(object);
    }
  }
  encodeBigInt64(object) {
    if (object >= BigInt(0)) {
      this.writeU8(207);
      this.writeBigUint64(object);
    } else {
      this.writeU8(211);
      this.writeBigInt64(object);
    }
  }
  writeStringHeader(byteLength) {
    if (byteLength < 32) {
      this.writeU8(160 + byteLength);
    } else if (byteLength < 256) {
      this.writeU8(217);
      this.writeU8(byteLength);
    } else if (byteLength < 65536) {
      this.writeU8(218);
      this.writeU16(byteLength);
    } else if (byteLength < 4294967296) {
      this.writeU8(219);
      this.writeU32(byteLength);
    } else {
      throw new Error(`Too long string: ${byteLength} bytes in UTF-8`);
    }
  }
  encodeString(object) {
    const maxHeaderSize = 1 + 4;
    const byteLength = utf8Count(object);
    this.ensureBufferSizeToWrite(maxHeaderSize + byteLength);
    this.writeStringHeader(byteLength);
    utf8Encode2(object, this.bytes, this.pos);
    this.pos += byteLength;
  }
  encodeObject(object, depth) {
    const ext = this.extensionCodec.tryToEncode(object, this.context);
    if (ext != null) {
      this.encodeExtension(ext);
    } else if (Array.isArray(object)) {
      this.encodeArray(object, depth);
    } else if (ArrayBuffer.isView(object)) {
      this.encodeBinary(object);
    } else if (typeof object === "object") {
      this.encodeMap(object, depth);
    } else {
      throw new Error(`Unrecognized object: ${Object.prototype.toString.apply(object)}`);
    }
  }
  encodeBinary(object) {
    const size = object.byteLength;
    if (size < 256) {
      this.writeU8(196);
      this.writeU8(size);
    } else if (size < 65536) {
      this.writeU8(197);
      this.writeU16(size);
    } else if (size < 4294967296) {
      this.writeU8(198);
      this.writeU32(size);
    } else {
      throw new Error(`Too large binary: ${size}`);
    }
    const bytes = ensureUint8Array(object);
    this.writeU8a(bytes);
  }
  encodeArray(object, depth) {
    const size = object.length;
    if (size < 16) {
      this.writeU8(144 + size);
    } else if (size < 65536) {
      this.writeU8(220);
      this.writeU16(size);
    } else if (size < 4294967296) {
      this.writeU8(221);
      this.writeU32(size);
    } else {
      throw new Error(`Too large array: ${size}`);
    }
    for (const item of object) {
      this.doEncode(item, depth + 1);
    }
  }
  countWithoutUndefined(object, keys) {
    let count = 0;
    for (const key of keys) {
      if (object[key] !== void 0) {
        count++;
      }
    }
    return count;
  }
  encodeMap(object, depth) {
    const keys = Object.keys(object);
    if (this.sortKeys) {
      keys.sort();
    }
    const size = this.ignoreUndefined ? this.countWithoutUndefined(object, keys) : keys.length;
    if (size < 16) {
      this.writeU8(128 + size);
    } else if (size < 65536) {
      this.writeU8(222);
      this.writeU16(size);
    } else if (size < 4294967296) {
      this.writeU8(223);
      this.writeU32(size);
    } else {
      throw new Error(`Too large map object: ${size}`);
    }
    for (const key of keys) {
      const value = object[key];
      if (!(this.ignoreUndefined && value === void 0)) {
        this.encodeString(key);
        this.doEncode(value, depth + 1);
      }
    }
  }
  encodeExtension(ext) {
    if (typeof ext.data === "function") {
      const data = ext.data(this.pos + 6);
      const size2 = data.length;
      if (size2 >= 4294967296) {
        throw new Error(`Too large extension object: ${size2}`);
      }
      this.writeU8(201);
      this.writeU32(size2);
      this.writeI8(ext.type);
      this.writeU8a(data);
      return;
    }
    const size = ext.data.length;
    if (size === 1) {
      this.writeU8(212);
    } else if (size === 2) {
      this.writeU8(213);
    } else if (size === 4) {
      this.writeU8(214);
    } else if (size === 8) {
      this.writeU8(215);
    } else if (size === 16) {
      this.writeU8(216);
    } else if (size < 256) {
      this.writeU8(199);
      this.writeU8(size);
    } else if (size < 65536) {
      this.writeU8(200);
      this.writeU16(size);
    } else if (size < 4294967296) {
      this.writeU8(201);
      this.writeU32(size);
    } else {
      throw new Error(`Too large extension object: ${size}`);
    }
    this.writeI8(ext.type);
    this.writeU8a(ext.data);
  }
  writeU8(value) {
    this.ensureBufferSizeToWrite(1);
    this.view.setUint8(this.pos, value);
    this.pos++;
  }
  writeU8a(values) {
    const size = values.length;
    this.ensureBufferSizeToWrite(size);
    this.bytes.set(values, this.pos);
    this.pos += size;
  }
  writeI8(value) {
    this.ensureBufferSizeToWrite(1);
    this.view.setInt8(this.pos, value);
    this.pos++;
  }
  writeU16(value) {
    this.ensureBufferSizeToWrite(2);
    this.view.setUint16(this.pos, value);
    this.pos += 2;
  }
  writeI16(value) {
    this.ensureBufferSizeToWrite(2);
    this.view.setInt16(this.pos, value);
    this.pos += 2;
  }
  writeU32(value) {
    this.ensureBufferSizeToWrite(4);
    this.view.setUint32(this.pos, value);
    this.pos += 4;
  }
  writeI32(value) {
    this.ensureBufferSizeToWrite(4);
    this.view.setInt32(this.pos, value);
    this.pos += 4;
  }
  writeF32(value) {
    this.ensureBufferSizeToWrite(4);
    this.view.setFloat32(this.pos, value);
    this.pos += 4;
  }
  writeF64(value) {
    this.ensureBufferSizeToWrite(8);
    this.view.setFloat64(this.pos, value);
    this.pos += 8;
  }
  writeU64(value) {
    this.ensureBufferSizeToWrite(8);
    setUint64(this.view, this.pos, value);
    this.pos += 8;
  }
  writeI64(value) {
    this.ensureBufferSizeToWrite(8);
    setInt64(this.view, this.pos, value);
    this.pos += 8;
  }
  writeBigUint64(value) {
    this.ensureBufferSizeToWrite(8);
    this.view.setBigUint64(this.pos, value);
    this.pos += 8;
  }
  writeBigInt64(value) {
    this.ensureBufferSizeToWrite(8);
    this.view.setBigInt64(this.pos, value);
    this.pos += 8;
  }
};

// node_modules/@msgpack/msgpack/dist.esm/encode.mjs
function encode(value, options) {
  const encoder2 = new Encoder(options);
  return encoder2.encodeSharedRef(value);
}

// node_modules/@msgpack/msgpack/dist.esm/utils/prettyByte.mjs
function prettyByte(byte) {
  return `${byte < 0 ? "-" : ""}0x${Math.abs(byte).toString(16).padStart(2, "0")}`;
}

// node_modules/@msgpack/msgpack/dist.esm/CachedKeyDecoder.mjs
var DEFAULT_MAX_KEY_LENGTH = 16;
var DEFAULT_MAX_LENGTH_PER_KEY = 16;
var CachedKeyDecoder = class {
  hit = 0;
  miss = 0;
  caches;
  maxKeyLength;
  maxLengthPerKey;
  constructor(maxKeyLength = DEFAULT_MAX_KEY_LENGTH, maxLengthPerKey = DEFAULT_MAX_LENGTH_PER_KEY) {
    this.maxKeyLength = maxKeyLength;
    this.maxLengthPerKey = maxLengthPerKey;
    this.caches = [];
    for (let i = 0; i < this.maxKeyLength; i++) {
      this.caches.push([]);
    }
  }
  canBeCached(byteLength) {
    return byteLength > 0 && byteLength <= this.maxKeyLength;
  }
  find(bytes, inputOffset, byteLength) {
    const records = this.caches[byteLength - 1];
    FIND_CHUNK: for (const record of records) {
      const recordBytes = record.bytes;
      for (let j = 0; j < byteLength; j++) {
        if (recordBytes[j] !== bytes[inputOffset + j]) {
          continue FIND_CHUNK;
        }
      }
      return record.str;
    }
    return null;
  }
  store(bytes, value) {
    const records = this.caches[bytes.length - 1];
    const record = { bytes, str: value };
    if (records.length >= this.maxLengthPerKey) {
      records[Math.random() * records.length | 0] = record;
    } else {
      records.push(record);
    }
  }
  decode(bytes, inputOffset, byteLength) {
    const cachedValue = this.find(bytes, inputOffset, byteLength);
    if (cachedValue != null) {
      this.hit++;
      return cachedValue;
    }
    this.miss++;
    const str = utf8DecodeJs(bytes, inputOffset, byteLength);
    const slicedCopyOfBytes = Uint8Array.prototype.slice.call(bytes, inputOffset, inputOffset + byteLength);
    this.store(slicedCopyOfBytes, str);
    return str;
  }
};

// node_modules/@msgpack/msgpack/dist.esm/Decoder.mjs
var STATE_ARRAY = "array";
var STATE_MAP_KEY = "map_key";
var STATE_MAP_VALUE = "map_value";
var mapKeyConverter = (key) => {
  if (typeof key === "string" || typeof key === "number") {
    return key;
  }
  throw new DecodeError("The type of key must be string or number but " + typeof key);
};
var StackPool = class {
  stack = [];
  stackHeadPosition = -1;
  get length() {
    return this.stackHeadPosition + 1;
  }
  top() {
    return this.stack[this.stackHeadPosition];
  }
  pushArrayState(size) {
    const state = this.getUninitializedStateFromPool();
    state.type = STATE_ARRAY;
    state.position = 0;
    state.size = size;
    state.array = new Array(size);
  }
  pushMapState(size) {
    const state = this.getUninitializedStateFromPool();
    state.type = STATE_MAP_KEY;
    state.readCount = 0;
    state.size = size;
    state.map = {};
  }
  getUninitializedStateFromPool() {
    this.stackHeadPosition++;
    if (this.stackHeadPosition === this.stack.length) {
      const partialState = {
        type: void 0,
        size: 0,
        array: void 0,
        position: 0,
        readCount: 0,
        map: void 0,
        key: null
      };
      this.stack.push(partialState);
    }
    return this.stack[this.stackHeadPosition];
  }
  release(state) {
    const topStackState = this.stack[this.stackHeadPosition];
    if (topStackState !== state) {
      throw new Error("Invalid stack state. Released state is not on top of the stack.");
    }
    if (state.type === STATE_ARRAY) {
      const partialState = state;
      partialState.size = 0;
      partialState.array = void 0;
      partialState.position = 0;
      partialState.type = void 0;
    }
    if (state.type === STATE_MAP_KEY || state.type === STATE_MAP_VALUE) {
      const partialState = state;
      partialState.size = 0;
      partialState.map = void 0;
      partialState.readCount = 0;
      partialState.type = void 0;
    }
    this.stackHeadPosition--;
  }
  reset() {
    this.stack.length = 0;
    this.stackHeadPosition = -1;
  }
};
var HEAD_BYTE_REQUIRED = -1;
var EMPTY_VIEW = new DataView(new ArrayBuffer(0));
var EMPTY_BYTES = new Uint8Array(EMPTY_VIEW.buffer);
try {
  EMPTY_VIEW.getInt8(0);
} catch (e) {
  if (!(e instanceof RangeError)) {
    throw new Error("This module is not supported in the current JavaScript engine because DataView does not throw RangeError on out-of-bounds access");
  }
}
var MORE_DATA = new RangeError("Insufficient data");
var sharedCachedKeyDecoder = new CachedKeyDecoder();
var Decoder = class _Decoder {
  extensionCodec;
  context;
  useBigInt64;
  rawStrings;
  maxStrLength;
  maxBinLength;
  maxArrayLength;
  maxMapLength;
  maxExtLength;
  keyDecoder;
  mapKeyConverter;
  totalPos = 0;
  pos = 0;
  view = EMPTY_VIEW;
  bytes = EMPTY_BYTES;
  headByte = HEAD_BYTE_REQUIRED;
  stack = new StackPool();
  entered = false;
  constructor(options) {
    this.extensionCodec = options?.extensionCodec ?? ExtensionCodec.defaultCodec;
    this.context = options?.context;
    this.useBigInt64 = options?.useBigInt64 ?? false;
    this.rawStrings = options?.rawStrings ?? false;
    this.maxStrLength = options?.maxStrLength ?? UINT32_MAX;
    this.maxBinLength = options?.maxBinLength ?? UINT32_MAX;
    this.maxArrayLength = options?.maxArrayLength ?? UINT32_MAX;
    this.maxMapLength = options?.maxMapLength ?? UINT32_MAX;
    this.maxExtLength = options?.maxExtLength ?? UINT32_MAX;
    this.keyDecoder = options?.keyDecoder !== void 0 ? options.keyDecoder : sharedCachedKeyDecoder;
    this.mapKeyConverter = options?.mapKeyConverter ?? mapKeyConverter;
  }
  clone() {
    return new _Decoder({
      extensionCodec: this.extensionCodec,
      context: this.context,
      useBigInt64: this.useBigInt64,
      rawStrings: this.rawStrings,
      maxStrLength: this.maxStrLength,
      maxBinLength: this.maxBinLength,
      maxArrayLength: this.maxArrayLength,
      maxMapLength: this.maxMapLength,
      maxExtLength: this.maxExtLength,
      keyDecoder: this.keyDecoder
    });
  }
  reinitializeState() {
    this.totalPos = 0;
    this.headByte = HEAD_BYTE_REQUIRED;
    this.stack.reset();
  }
  setBuffer(buffer) {
    const bytes = ensureUint8Array(buffer);
    this.bytes = bytes;
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    this.pos = 0;
  }
  appendBuffer(buffer) {
    if (this.headByte === HEAD_BYTE_REQUIRED && !this.hasRemaining(1)) {
      this.setBuffer(buffer);
    } else {
      const remainingData = this.bytes.subarray(this.pos);
      const newData = ensureUint8Array(buffer);
      const newBuffer = new Uint8Array(remainingData.length + newData.length);
      newBuffer.set(remainingData);
      newBuffer.set(newData, remainingData.length);
      this.setBuffer(newBuffer);
    }
  }
  hasRemaining(size) {
    return this.view.byteLength - this.pos >= size;
  }
  createExtraByteError(posToShow) {
    const { view, pos } = this;
    return new RangeError(`Extra ${view.byteLength - pos} of ${view.byteLength} byte(s) found at buffer[${posToShow}]`);
  }
  /**
   * @throws {@link DecodeError}
   * @throws {@link RangeError}
   */
  decode(buffer) {
    if (this.entered) {
      const instance = this.clone();
      return instance.decode(buffer);
    }
    try {
      this.entered = true;
      this.reinitializeState();
      this.setBuffer(buffer);
      const object = this.doDecodeSync();
      if (this.hasRemaining(1)) {
        throw this.createExtraByteError(this.pos);
      }
      return object;
    } finally {
      this.entered = false;
    }
  }
  *decodeMulti(buffer) {
    if (this.entered) {
      const instance = this.clone();
      yield* instance.decodeMulti(buffer);
      return;
    }
    try {
      this.entered = true;
      this.reinitializeState();
      this.setBuffer(buffer);
      while (this.hasRemaining(1)) {
        yield this.doDecodeSync();
      }
    } finally {
      this.entered = false;
    }
  }
  async decodeAsync(stream) {
    if (this.entered) {
      const instance = this.clone();
      return instance.decodeAsync(stream);
    }
    try {
      this.entered = true;
      let decoded = false;
      let object;
      for await (const buffer of stream) {
        if (decoded) {
          this.entered = false;
          throw this.createExtraByteError(this.totalPos);
        }
        this.appendBuffer(buffer);
        try {
          object = this.doDecodeSync();
          decoded = true;
        } catch (e) {
          if (!(e instanceof RangeError)) {
            throw e;
          }
        }
        this.totalPos += this.pos;
      }
      if (decoded) {
        if (this.hasRemaining(1)) {
          throw this.createExtraByteError(this.totalPos);
        }
        return object;
      }
      const { headByte, pos, totalPos } = this;
      throw new RangeError(`Insufficient data in parsing ${prettyByte(headByte)} at ${totalPos} (${pos} in the current buffer)`);
    } finally {
      this.entered = false;
    }
  }
  decodeArrayStream(stream) {
    return this.decodeMultiAsync(stream, true);
  }
  decodeStream(stream) {
    return this.decodeMultiAsync(stream, false);
  }
  async *decodeMultiAsync(stream, isArray) {
    if (this.entered) {
      const instance = this.clone();
      yield* instance.decodeMultiAsync(stream, isArray);
      return;
    }
    try {
      this.entered = true;
      let isArrayHeaderRequired = isArray;
      let arrayItemsLeft = -1;
      for await (const buffer of stream) {
        if (isArray && arrayItemsLeft === 0) {
          throw this.createExtraByteError(this.totalPos);
        }
        this.appendBuffer(buffer);
        if (isArrayHeaderRequired) {
          arrayItemsLeft = this.readArraySize();
          isArrayHeaderRequired = false;
          this.complete();
        }
        try {
          while (true) {
            yield this.doDecodeSync();
            if (--arrayItemsLeft === 0) {
              break;
            }
          }
        } catch (e) {
          if (!(e instanceof RangeError)) {
            throw e;
          }
        }
        this.totalPos += this.pos;
      }
    } finally {
      this.entered = false;
    }
  }
  doDecodeSync() {
    DECODE: while (true) {
      const headByte = this.readHeadByte();
      let object;
      if (headByte >= 224) {
        object = headByte - 256;
      } else if (headByte < 192) {
        if (headByte < 128) {
          object = headByte;
        } else if (headByte < 144) {
          const size = headByte - 128;
          if (size !== 0) {
            this.pushMapState(size);
            this.complete();
            continue DECODE;
          } else {
            object = {};
          }
        } else if (headByte < 160) {
          const size = headByte - 144;
          if (size !== 0) {
            this.pushArrayState(size);
            this.complete();
            continue DECODE;
          } else {
            object = [];
          }
        } else {
          const byteLength = headByte - 160;
          object = this.decodeString(byteLength, 0);
        }
      } else if (headByte === 192) {
        object = null;
      } else if (headByte === 194) {
        object = false;
      } else if (headByte === 195) {
        object = true;
      } else if (headByte === 202) {
        object = this.readF32();
      } else if (headByte === 203) {
        object = this.readF64();
      } else if (headByte === 204) {
        object = this.readU8();
      } else if (headByte === 205) {
        object = this.readU16();
      } else if (headByte === 206) {
        object = this.readU32();
      } else if (headByte === 207) {
        if (this.useBigInt64) {
          object = this.readU64AsBigInt();
        } else {
          object = this.readU64();
        }
      } else if (headByte === 208) {
        object = this.readI8();
      } else if (headByte === 209) {
        object = this.readI16();
      } else if (headByte === 210) {
        object = this.readI32();
      } else if (headByte === 211) {
        if (this.useBigInt64) {
          object = this.readI64AsBigInt();
        } else {
          object = this.readI64();
        }
      } else if (headByte === 217) {
        const byteLength = this.lookU8();
        object = this.decodeString(byteLength, 1);
      } else if (headByte === 218) {
        const byteLength = this.lookU16();
        object = this.decodeString(byteLength, 2);
      } else if (headByte === 219) {
        const byteLength = this.lookU32();
        object = this.decodeString(byteLength, 4);
      } else if (headByte === 220) {
        const size = this.readU16();
        if (size !== 0) {
          this.pushArrayState(size);
          this.complete();
          continue DECODE;
        } else {
          object = [];
        }
      } else if (headByte === 221) {
        const size = this.readU32();
        if (size !== 0) {
          this.pushArrayState(size);
          this.complete();
          continue DECODE;
        } else {
          object = [];
        }
      } else if (headByte === 222) {
        const size = this.readU16();
        if (size !== 0) {
          this.pushMapState(size);
          this.complete();
          continue DECODE;
        } else {
          object = {};
        }
      } else if (headByte === 223) {
        const size = this.readU32();
        if (size !== 0) {
          this.pushMapState(size);
          this.complete();
          continue DECODE;
        } else {
          object = {};
        }
      } else if (headByte === 196) {
        const size = this.lookU8();
        object = this.decodeBinary(size, 1);
      } else if (headByte === 197) {
        const size = this.lookU16();
        object = this.decodeBinary(size, 2);
      } else if (headByte === 198) {
        const size = this.lookU32();
        object = this.decodeBinary(size, 4);
      } else if (headByte === 212) {
        object = this.decodeExtension(1, 0);
      } else if (headByte === 213) {
        object = this.decodeExtension(2, 0);
      } else if (headByte === 214) {
        object = this.decodeExtension(4, 0);
      } else if (headByte === 215) {
        object = this.decodeExtension(8, 0);
      } else if (headByte === 216) {
        object = this.decodeExtension(16, 0);
      } else if (headByte === 199) {
        const size = this.lookU8();
        object = this.decodeExtension(size, 1);
      } else if (headByte === 200) {
        const size = this.lookU16();
        object = this.decodeExtension(size, 2);
      } else if (headByte === 201) {
        const size = this.lookU32();
        object = this.decodeExtension(size, 4);
      } else {
        throw new DecodeError(`Unrecognized type byte: ${prettyByte(headByte)}`);
      }
      this.complete();
      const stack = this.stack;
      while (stack.length > 0) {
        const state = stack.top();
        if (state.type === STATE_ARRAY) {
          state.array[state.position] = object;
          state.position++;
          if (state.position === state.size) {
            object = state.array;
            stack.release(state);
          } else {
            continue DECODE;
          }
        } else if (state.type === STATE_MAP_KEY) {
          if (object === "__proto__") {
            throw new DecodeError("The key __proto__ is not allowed");
          }
          state.key = this.mapKeyConverter(object);
          state.type = STATE_MAP_VALUE;
          continue DECODE;
        } else {
          state.map[state.key] = object;
          state.readCount++;
          if (state.readCount === state.size) {
            object = state.map;
            stack.release(state);
          } else {
            state.key = null;
            state.type = STATE_MAP_KEY;
            continue DECODE;
          }
        }
      }
      return object;
    }
  }
  readHeadByte() {
    if (this.headByte === HEAD_BYTE_REQUIRED) {
      this.headByte = this.readU8();
    }
    return this.headByte;
  }
  complete() {
    this.headByte = HEAD_BYTE_REQUIRED;
  }
  readArraySize() {
    const headByte = this.readHeadByte();
    switch (headByte) {
      case 220:
        return this.readU16();
      case 221:
        return this.readU32();
      default: {
        if (headByte < 160) {
          return headByte - 144;
        } else {
          throw new DecodeError(`Unrecognized array type byte: ${prettyByte(headByte)}`);
        }
      }
    }
  }
  pushMapState(size) {
    if (size > this.maxMapLength) {
      throw new DecodeError(`Max length exceeded: map length (${size}) > maxMapLengthLength (${this.maxMapLength})`);
    }
    this.stack.pushMapState(size);
  }
  pushArrayState(size) {
    if (size > this.maxArrayLength) {
      throw new DecodeError(`Max length exceeded: array length (${size}) > maxArrayLength (${this.maxArrayLength})`);
    }
    this.stack.pushArrayState(size);
  }
  decodeString(byteLength, headerOffset) {
    if (!this.rawStrings || this.stateIsMapKey()) {
      return this.decodeUtf8String(byteLength, headerOffset);
    }
    return this.decodeBinary(byteLength, headerOffset);
  }
  /**
   * @throws {@link RangeError}
   */
  decodeUtf8String(byteLength, headerOffset) {
    if (byteLength > this.maxStrLength) {
      throw new DecodeError(`Max length exceeded: UTF-8 byte length (${byteLength}) > maxStrLength (${this.maxStrLength})`);
    }
    if (this.bytes.byteLength < this.pos + headerOffset + byteLength) {
      throw MORE_DATA;
    }
    const offset = this.pos + headerOffset;
    let object;
    if (this.stateIsMapKey() && this.keyDecoder?.canBeCached(byteLength)) {
      object = this.keyDecoder.decode(this.bytes, offset, byteLength);
    } else {
      object = utf8Decode2(this.bytes, offset, byteLength);
    }
    this.pos += headerOffset + byteLength;
    return object;
  }
  stateIsMapKey() {
    if (this.stack.length > 0) {
      const state = this.stack.top();
      return state.type === STATE_MAP_KEY;
    }
    return false;
  }
  /**
   * @throws {@link RangeError}
   */
  decodeBinary(byteLength, headOffset) {
    if (byteLength > this.maxBinLength) {
      throw new DecodeError(`Max length exceeded: bin length (${byteLength}) > maxBinLength (${this.maxBinLength})`);
    }
    if (!this.hasRemaining(byteLength + headOffset)) {
      throw MORE_DATA;
    }
    const offset = this.pos + headOffset;
    const object = this.bytes.subarray(offset, offset + byteLength);
    this.pos += headOffset + byteLength;
    return object;
  }
  decodeExtension(size, headOffset) {
    if (size > this.maxExtLength) {
      throw new DecodeError(`Max length exceeded: ext length (${size}) > maxExtLength (${this.maxExtLength})`);
    }
    const extType = this.view.getInt8(this.pos + headOffset);
    const data = this.decodeBinary(
      size,
      headOffset + 1
      /* extType */
    );
    return this.extensionCodec.decode(data, extType, this.context);
  }
  lookU8() {
    return this.view.getUint8(this.pos);
  }
  lookU16() {
    return this.view.getUint16(this.pos);
  }
  lookU32() {
    return this.view.getUint32(this.pos);
  }
  readU8() {
    const value = this.view.getUint8(this.pos);
    this.pos++;
    return value;
  }
  readI8() {
    const value = this.view.getInt8(this.pos);
    this.pos++;
    return value;
  }
  readU16() {
    const value = this.view.getUint16(this.pos);
    this.pos += 2;
    return value;
  }
  readI16() {
    const value = this.view.getInt16(this.pos);
    this.pos += 2;
    return value;
  }
  readU32() {
    const value = this.view.getUint32(this.pos);
    this.pos += 4;
    return value;
  }
  readI32() {
    const value = this.view.getInt32(this.pos);
    this.pos += 4;
    return value;
  }
  readU64() {
    const value = getUint64(this.view, this.pos);
    this.pos += 8;
    return value;
  }
  readI64() {
    const value = getInt64(this.view, this.pos);
    this.pos += 8;
    return value;
  }
  readU64AsBigInt() {
    const value = this.view.getBigUint64(this.pos);
    this.pos += 8;
    return value;
  }
  readI64AsBigInt() {
    const value = this.view.getBigInt64(this.pos);
    this.pos += 8;
    return value;
  }
  readF32() {
    const value = this.view.getFloat32(this.pos);
    this.pos += 4;
    return value;
  }
  readF64() {
    const value = this.view.getFloat64(this.pos);
    this.pos += 8;
    return value;
  }
};

// node_modules/@msgpack/msgpack/dist.esm/decode.mjs
function decode(buffer, options) {
  const decoder2 = new Decoder(options);
  return decoder2.decode(buffer);
}
function decodeMulti(buffer, options) {
  const decoder2 = new Decoder(options);
  return decoder2.decodeMulti(buffer);
}

// src/msgpack_codec.ts
function encodeMsgpackFields(values) {
  try {
    return concatBytes(values.map((value) => encode(value)));
  } catch (error) {
    return statusFromUnknown(
      error,
      "Failed to encode MessagePack.",
      3
    );
  }
}
function decodeMsgpackFields(bytes, count, context) {
  try {
    const values = [...decodeMulti(bytes, { useBigInt64: true })];
    if (values.length !== count) {
      return invalidArgumentError(
        `${context} contains ${values.length} MessagePack fields; expected ${count}.`
      );
    }
    return values;
  } catch (error) {
    return invalidArgumentError(
      `Failed to decode ${context} MessagePack.`,
      [],
      error
    );
  }
}
function msgpackByteMap(values) {
  const result = {};
  for (const [key, value] of values) result[key] = value;
  return result;
}

// src/status_codec.ts
function packStatus(status) {
  if (typeof status !== "object" || status === null || !Number.isInteger(status.code) || status.code < 0 /* OK */ || status.code > 16 /* UNAUTHENTICATED */ || typeof status.message !== "string") {
    return invalidArgumentError("Value is not a canonical Status.");
  }
  const details = status.details ?? [];
  if (!Array.isArray(details) || details.some((detail) => typeof detail !== "object" || detail === null)) {
    return invalidArgumentError("Every Status detail must be an object.");
  }
  return encodeMsgpackFields([status.code, status.message, details]);
}
function decodeStatus(source) {
  const bytes = toBytes(source);
  if (!isOk(bytes)) return bytes;
  const fields = decodeMsgpackFields(bytes, 3, "Status");
  if (!isOk(fields)) return fields;
  const [code, message, rawDetails] = fields;
  const numericCode = typeof code === "bigint" ? Number(code) : code;
  if (!Number.isInteger(numericCode) || numericCode < 0 /* OK */ || numericCode > 16 /* UNAUTHENTICATED */ || typeof message !== "string" || rawDetails !== null && !Array.isArray(rawDetails)) {
    return invalidArgumentError(
      "MessagePack does not contain a valid Status."
    );
  }
  const details = rawDetails ?? [];
  if (details.some(
    (detail) => typeof detail !== "object" || detail === null || Array.isArray(detail)
  )) {
    return invalidArgumentError("Every Status detail must be an object.");
  }
  return {
    status: {
      code: numericCode,
      message,
      details
    }
  };
}

// src/data.ts
var UINT32_MAX2 = 4294967295;
var UINT32_RANGE = 4294967296;
var INT64_MIN = -(1n << 63n);
var INT64_MAX = (1n << 63n) - 1n;
var NAME_PATTERN = /^[A-Za-z0-9_](?:[A-Za-z0-9_#-]{0,253}[A-Za-z0-9_])?$/;
function validateName(name) {
  if (typeof name !== "string" || name.length < 1 || name.length > 255) {
    return invalidArgumentError(
      "name must contain between 1 and 255 characters"
    );
  }
  if (!NAME_PATTERN.test(name)) {
    return invalidArgumentError(
      "name must start and end with an ASCII letter, digit, or underscore and contain only [a-zA-Z0-9-_#]"
    );
  }
  return okStatus();
}
function validateOptionalName(value) {
  return value === "" ? okStatus() : validateName(value);
}
function asRecord(value, context) {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    return invalidArgumentError(`${context} must be an object.`);
  }
  return value;
}
function asString(value, field) {
  return typeof value === "string" ? value : invalidArgumentError(`${field} must be a string.`);
}
function asBinary(value, field) {
  return value instanceof Uint8Array ? new Uint8Array(value) : invalidArgumentError(`${field} must be MessagePack binary data.`);
}
function asUnsigned(value, field, maximum) {
  let numberValue;
  if (typeof value === "bigint") {
    if (value < 0n || value > BigInt(maximum)) {
      return outOfRangeError(`${field} exceeds its supported range.`);
    }
    numberValue = Number(value);
  } else if (typeof value === "number" && Number.isSafeInteger(value)) {
    numberValue = value;
  } else {
    return invalidArgumentError(`${field} must be a non-negative integer.`);
  }
  if (numberValue < 0 || numberValue > maximum) {
    return outOfRangeError(`${field} exceeds its supported range.`);
  }
  return numberValue;
}
function asSignedInt64(value, field) {
  if (typeof value === "bigint") {
    return value >= INT64_MIN && value <= INT64_MAX ? value : outOfRangeError(`${field} exceeds int64.`);
  }
  if (typeof value !== "number" || !Number.isSafeInteger(value)) {
    return invalidArgumentError(`${field} must be integer microseconds.`);
  }
  return value;
}
function decodeByteMap(value, field) {
  const object = asRecord(value, field);
  if (!isOk(object)) return object;
  const result = /* @__PURE__ */ new Map();
  for (const [key, raw] of Object.entries(object)) {
    const valid = validateName(key);
    if (!isOk(valid)) {
      return invalidArgumentError(`Invalid key in ${field}: ${valid.message}`);
    }
    const bytes = asBinary(raw, `${field}.${key}`);
    if (!isOk(bytes)) return bytes;
    result.set(key, bytes);
  }
  return result;
}
function byteMapFromJson(value, field) {
  if (value === void 0) return /* @__PURE__ */ new Map();
  const object = asRecord(value, field);
  if (!isOk(object)) return object;
  const result = /* @__PURE__ */ new Map();
  for (const [key, raw] of Object.entries(object)) {
    const valid = validateName(key);
    if (!isOk(valid)) return valid;
    if (typeof raw !== "string") {
      return invalidArgumentError(`${field}.${key} must be a base64 string.`);
    }
    const decoded = base64Decode(raw);
    if (!isOk(decoded)) return decoded;
    result.set(key, decoded);
  }
  return result;
}
function byteMapToJson(values) {
  return Object.fromEntries(
    [...values].map(([key, value]) => [key, base64Encode(value)])
  );
}
function isValidDate(value) {
  try {
    return value instanceof Date && Number.isFinite(value.getTime());
  } catch {
    return false;
  }
}
var ChunkMetadata = class _ChunkMetadata {
  mimetype;
  timestamp;
  attributes;
  constructor(options = {}) {
    this.mimetype = options.mimetype ?? "";
    this.timestamp = options.timestamp === void 0 || options.timestamp === null ? null : new Date(options.timestamp.getTime());
    this.attributes = options.attributes === void 0 ? /* @__PURE__ */ new Map() : copyByteMap(options.attributes);
  }
  /** Construct and validate metadata while normalizing byte-like attributes. */
  static create(options = {}) {
    try {
      if (typeof options.mimetype !== "undefined" && typeof options.mimetype !== "string") {
        return invalidArgumentError("ChunkMetadata.mimetype must be a string.");
      }
      if (options.timestamp !== void 0 && options.timestamp !== null && (!(options.timestamp instanceof Date) || !isValidDate(options.timestamp))) {
        return invalidArgumentError("ChunkMetadata.timestamp must be a valid Date or null.");
      }
      const attributes = normalizeByteMap(options.attributes, (key) => isOk(validateName(key)));
      if (!isOk(attributes)) return attributes;
      const result = new _ChunkMetadata({ ...options, attributes });
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError("Invalid ChunkMetadata options.", [], error);
    }
  }
  get approxBytes() {
    let result = this.mimetype.length + 9;
    for (const [key, value] of this.attributes) result += key.length + value.byteLength;
    return result;
  }
  validate() {
    try {
      if (typeof this.mimetype !== "string") {
        return invalidArgumentError("ChunkMetadata.mimetype must be a string.");
      }
      if (this.timestamp !== null && !isValidDate(this.timestamp)) {
        return invalidArgumentError("ChunkMetadata.timestamp must be a valid Date or null.");
      }
      if (!(this.attributes instanceof Map)) {
        return invalidArgumentError("ChunkMetadata.attributes must be a Map.");
      }
      for (const [key, value] of this.attributes) {
        const status = validateName(key);
        if (!isOk(status)) return status;
        if (!(value instanceof Uint8Array)) {
          return invalidArgumentError(
            "ChunkMetadata attribute values must be Uint8Array values."
          );
        }
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError("Invalid ChunkMetadata value.", [], error);
    }
  }
  getAttribute(key) {
    try {
      const valid = validateName(key);
      if (!isOk(valid)) return valid;
      const value = this.attributes.get(key);
      return value === void 0 ? notFoundError(`Attribute not found: ${key}`) : new Uint8Array(value);
    } catch (error) {
      return statusFromUnknown(error, "Could not read chunk attribute.");
    }
  }
  setAttribute(key, value) {
    const status = validateName(key);
    if (!isOk(status)) return status;
    try {
      this.attributes.set(key, new Uint8Array(value));
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Could not set chunk attribute.");
    }
  }
  toMsgpack() {
    const status = this.validate();
    if (!isOk(status)) return status;
    let timestamp = null;
    if (this.timestamp !== null) {
      const micros = BigInt(Math.trunc(this.timestamp.getTime())) * 1000n;
      timestamp = micros >= BigInt(Number.MIN_SAFE_INTEGER) && micros <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(micros) : micros;
    }
    return encodeMsgpackFields([
      this.mimetype,
      timestamp,
      msgpackByteMap(this.attributes)
    ]);
  }
  static fromMsgpack(bytes) {
    const fields = decodeMsgpackFields(bytes, 3, "ChunkMetadata");
    if (!isOk(fields)) return fields;
    const mimetype = asString(fields[0], "ChunkMetadata.mimetype");
    if (!isOk(mimetype)) return mimetype;
    let timestamp = null;
    if (fields[1] !== null) {
      const micros = asSignedInt64(fields[1], "ChunkMetadata.timestamp");
      if (!isOk(micros)) return micros;
      const milliseconds = typeof micros === "bigint" ? Number(micros / 1000n) : micros / 1e3;
      timestamp = new Date(milliseconds);
      if (!isValidDate(timestamp)) {
        return outOfRangeError("ChunkMetadata.timestamp is outside the Date range.");
      }
    }
    const attributes = decodeByteMap(fields[2], "ChunkMetadata.attributes");
    if (!isOk(attributes)) return attributes;
    return _ChunkMetadata.create({ mimetype, timestamp, attributes });
  }
};
var Chunk = class _Chunk {
  metadata;
  ref;
  data;
  constructor(options = {}) {
    this.metadata = options.metadata ?? null;
    this.ref = options.ref ?? "";
    this.data = options.data === void 0 ? new Uint8Array() : new Uint8Array(options.data);
  }
  static create(options = {}) {
    try {
      if (options.metadata !== void 0 && options.metadata !== null && !(options.metadata instanceof ChunkMetadata)) {
        return invalidArgumentError("Chunk.metadata must be ChunkMetadata or null.");
      }
      if (options.ref !== void 0 && typeof options.ref !== "string") {
        return invalidArgumentError("Chunk.ref must be a string.");
      }
      if (options.data !== void 0 && !(options.data instanceof Uint8Array)) {
        return invalidArgumentError("Chunk.data must be Uint8Array.");
      }
      const result = new _Chunk(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError("Invalid Chunk options.", [], error);
    }
  }
  get approxBytes() {
    return this.ref.length + this.data.byteLength + (this.metadata?.approxBytes ?? 1) + 5;
  }
  get mimetype() {
    return this.metadata?.mimetype ?? "";
  }
  get isEmpty() {
    return this.ref === "" && this.data.byteLength === 0;
  }
  /** Whether this is the explicit null chunk used as a final terminator. */
  get isNull() {
    return this.isEmpty && this.mimetype === "application/octet-stream";
  }
  validate() {
    try {
      if (typeof this.ref !== "string") return invalidArgumentError("Chunk.ref must be a string.");
      if (!(this.data instanceof Uint8Array)) return invalidArgumentError("Chunk.data must be Uint8Array.");
      if (this.ref !== "" && this.data.byteLength !== 0) {
        return invalidArgumentError("Only one of ref or data may be set");
      }
      if (this.metadata !== null && !(this.metadata instanceof ChunkMetadata)) {
        return invalidArgumentError("Chunk.metadata must be ChunkMetadata or null.");
      }
      return this.metadata === null ? okStatus() : this.metadata.validate();
    } catch (error) {
      return invalidArgumentError("Invalid Chunk value.", [], error);
    }
  }
  toMsgpack() {
    const status = this.validate();
    if (!isOk(status)) return status;
    const metadata = this.metadata?.toMsgpack() ?? null;
    if (metadata !== null && !isOk(metadata)) return metadata;
    return encodeMsgpackFields([metadata, this.ref, this.data]);
  }
  static fromMsgpack(bytes) {
    const fields = decodeMsgpackFields(bytes, 3, "Chunk");
    if (!isOk(fields)) return fields;
    let metadata = null;
    if (fields[0] !== null) {
      const encoded = asBinary(fields[0], "Chunk.metadata");
      if (!isOk(encoded)) return encoded;
      const decoded = ChunkMetadata.fromMsgpack(encoded);
      if (!isOk(decoded)) return decoded;
      metadata = decoded;
    }
    const ref = asString(fields[1], "Chunk.ref");
    if (!isOk(ref)) return ref;
    const data = asBinary(fields[2], "Chunk.data");
    if (!isOk(data)) return data;
    return _Chunk.create({ metadata, ref, data });
  }
};
var NodeRef = class _NodeRef {
  id;
  offset;
  length;
  constructor(options) {
    this.id = options.id;
    this.offset = options.offset ?? 0;
    this.length = options.length ?? null;
  }
  static create(options) {
    try {
      if (typeof options !== "object" || options === null) {
        return invalidArgumentError("NodeRef options must be an object.");
      }
      const result = new _NodeRef(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError("Invalid NodeRef options.", [], error);
    }
  }
  get approxBytes() {
    return this.id.length + 5 + (this.length === null ? 0 : 8);
  }
  validate() {
    try {
      const nameStatus = validateName(this.id);
      if (!isOk(nameStatus)) return nameStatus;
      if (!Number.isInteger(this.offset) || this.offset < 0 || this.offset > UINT32_MAX2) {
        return invalidArgumentError("NodeRef.offset must be a uint32 integer.");
      }
      if (this.length !== null && (!Number.isInteger(this.length) || this.length < 0 || this.length > UINT32_RANGE || this.length + this.offset > UINT32_RANGE)) {
        return invalidArgumentError("Offset + length must not exceed 2^32");
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError("Invalid NodeRef value.", [], error);
    }
  }
  toMsgpack() {
    const status = this.validate();
    return isOk(status) ? encodeMsgpackFields([this.id, this.offset, this.length]) : status;
  }
  static fromMsgpack(bytes) {
    const fields = decodeMsgpackFields(bytes, 3, "NodeRef");
    if (!isOk(fields)) return fields;
    const id = asString(fields[0], "NodeRef.id");
    if (!isOk(id)) return id;
    const offset = asUnsigned(fields[1], "NodeRef.offset", UINT32_MAX2);
    if (!isOk(offset)) return offset;
    let length = null;
    if (fields[2] !== null) {
      const decoded = asUnsigned(fields[2], "NodeRef.length", UINT32_RANGE);
      if (!isOk(decoded)) return decoded;
      length = decoded;
    }
    return _NodeRef.create({ id, offset, length });
  }
};
var NodeFragment = class _NodeFragment {
  id;
  data;
  seq;
  continued;
  constructor(options = {}) {
    this.id = options.id ?? "";
    this.data = options.data ?? new Chunk();
    this.seq = options.seq ?? null;
    this.continued = options.continued ?? false;
  }
  static create(options = {}) {
    try {
      const result = new _NodeFragment(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError("Invalid NodeFragment options.", [], error);
    }
  }
  get approxBytes() {
    return this.id.length + this.data.approxBytes + (this.seq === null ? 1 : 4) + 6;
  }
  validate() {
    try {
      const idStatus = validateOptionalName(this.id);
      if (!isOk(idStatus)) return idStatus;
      if (!(this.data instanceof Chunk) && !(this.data instanceof NodeRef)) {
        return invalidArgumentError("NodeFragment.data must be a Chunk or NodeRef.");
      }
      if (this.seq !== null && (!Number.isInteger(this.seq) || this.seq < 0 || this.seq > UINT32_MAX2)) {
        return invalidArgumentError("NodeFragment.seq must be a uint32 integer or null.");
      }
      if (typeof this.continued !== "boolean") {
        return invalidArgumentError("NodeFragment.continued must be boolean.");
      }
      return this.data.validate();
    } catch (error) {
      return invalidArgumentError("Invalid NodeFragment value.", [], error);
    }
  }
  getChunk() {
    return this.data instanceof Chunk ? this.data : failedPreconditionError("Data is not a Chunk");
  }
  getNodeRef() {
    return this.data instanceof NodeRef ? this.data : failedPreconditionError("Data is not a NodeRef");
  }
  toMsgpack() {
    const status = this.validate();
    if (!isOk(status)) return status;
    const encoded = this.data.toMsgpack();
    if (!isOk(encoded)) return encoded;
    return encodeMsgpackFields([
      this.id,
      this.data instanceof Chunk ? 0 : 1,
      encoded,
      this.seq,
      this.continued
    ]);
  }
  static fromMsgpack(bytes) {
    const fields = decodeMsgpackFields(bytes, 5, "NodeFragment");
    if (!isOk(fields)) return fields;
    const id = asString(fields[0], "NodeFragment.id");
    if (!isOk(id)) return id;
    const variant = asUnsigned(fields[1], "NodeFragment.data index", 1);
    if (!isOk(variant)) return variant;
    const encoded = asBinary(fields[2], "NodeFragment.data");
    if (!isOk(encoded)) return encoded;
    const data = variant === 0 ? Chunk.fromMsgpack(encoded) : NodeRef.fromMsgpack(encoded);
    if (!isOk(data)) return data;
    let seq = null;
    if (fields[3] !== null) {
      const parsed = asUnsigned(fields[3], "NodeFragment.seq", UINT32_MAX2);
      if (!isOk(parsed)) return parsed;
      seq = parsed;
    }
    if (typeof fields[4] !== "boolean") {
      return invalidArgumentError("NodeFragment.continued must be bool.");
    }
    return _NodeFragment.create({ id, data, seq, continued: fields[4] });
  }
};
var Port = class _Port {
  constructor(name = "", id = "") {
    this.name = name;
    this.id = id;
  }
  static create(name = "", id = "") {
    try {
      const result = new _Port(name, id);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError("Invalid Port value.", [], error);
    }
  }
  get approxBytes() {
    return this.name.length + this.id.length + 1;
  }
  validate() {
    try {
      const nameStatus = validateOptionalName(this.name);
      return isOk(nameStatus) ? validateOptionalName(this.id) : nameStatus;
    } catch (error) {
      return invalidArgumentError("Invalid Port value.", [], error);
    }
  }
  toMsgpack() {
    const status = this.validate();
    return isOk(status) ? encodeMsgpackFields([this.name, this.id]) : status;
  }
  static fromMsgpack(bytes) {
    const fields = decodeMsgpackFields(bytes, 2, "Port");
    if (!isOk(fields)) return fields;
    const name = asString(fields[0], "Port.name");
    if (!isOk(name)) return name;
    const id = asString(fields[1], "Port.id");
    return isOk(id) ? _Port.create(name, id) : id;
  }
};
var ActionMessage = class _ActionMessage {
  id;
  name;
  inputs;
  outputs;
  headers;
  constructor(options = {}) {
    this.id = options.id ?? "";
    this.name = options.name ?? "";
    this.inputs = [...options.inputs ?? []];
    this.outputs = [...options.outputs ?? []];
    this.headers = options.headers === void 0 ? /* @__PURE__ */ new Map() : copyByteMap(options.headers);
  }
  static create(options = {}) {
    try {
      const result = new _ActionMessage(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError("Invalid ActionMessage options.", [], error);
    }
  }
  get approxBytes() {
    let result = this.id.length + this.name.length + 8;
    for (const port of [...this.inputs, ...this.outputs]) result += port.approxBytes;
    for (const [key, value] of this.headers) result += key.length + value.byteLength;
    return result;
  }
  validate() {
    try {
      for (const name of [this.id, this.name]) {
        const status = validateOptionalName(name);
        if (!isOk(status)) return status;
      }
      if (!Array.isArray(this.inputs) || !Array.isArray(this.outputs)) {
        return invalidArgumentError("Action ports must be arrays.");
      }
      for (const port of [...this.inputs, ...this.outputs]) {
        if (!(port instanceof Port)) return invalidArgumentError("Action ports must be Port values.");
        const status = port.validate();
        if (!isOk(status)) return status;
      }
      if (!(this.headers instanceof Map)) {
        return invalidArgumentError("ActionMessage.headers must be a Map.");
      }
      for (const [key, value] of this.headers) {
        const status = validateName(key);
        if (!isOk(status)) return status;
        if (!(value instanceof Uint8Array)) {
          return invalidArgumentError(
            "ActionMessage header values must be Uint8Array values."
          );
        }
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError("Invalid ActionMessage value.", [], error);
    }
  }
  toMsgpack() {
    const status = this.validate();
    if (!isOk(status)) return status;
    const inputs = [];
    const outputs = [];
    for (const [ports, encoded] of [[this.inputs, inputs], [this.outputs, outputs]]) {
      for (const port of ports) {
        const bytes = port.toMsgpack();
        if (!isOk(bytes)) return bytes;
        encoded.push(bytes);
      }
    }
    return encodeMsgpackFields([this.id, this.name, inputs, outputs, msgpackByteMap(this.headers)]);
  }
  static fromMsgpack(bytes) {
    const fields = decodeMsgpackFields(bytes, 5, "ActionMessage");
    if (!isOk(fields)) return fields;
    const id = asString(fields[0], "ActionMessage.id");
    if (!isOk(id)) return id;
    const name = asString(fields[1], "ActionMessage.name");
    if (!isOk(name)) return name;
    const decodedPorts = [];
    for (const [index, field] of [[2, "inputs"], [3, "outputs"]]) {
      const raw = fields[index];
      if (!Array.isArray(raw)) return invalidArgumentError(`ActionMessage.${field} must be a list.`);
      const ports = [];
      for (const item of raw) {
        const encoded = asBinary(item, `ActionMessage.${field}`);
        if (!isOk(encoded)) return encoded;
        const port = Port.fromMsgpack(encoded);
        if (!isOk(port)) return port;
        ports.push(port);
      }
      decodedPorts.push(ports);
    }
    const headers = decodeByteMap(fields[4], "ActionMessage.headers");
    if (!isOk(headers)) return headers;
    return _ActionMessage.create({ id, name, inputs: decodedPorts[0], outputs: decodedPorts[1], headers });
  }
};
var WireMessage = class _WireMessage {
  /** Current MessagePack/JSON wire schema version. */
  static VERSION = 1;
  nodeFragments;
  actions;
  headers;
  constructor(options = {}) {
    this.nodeFragments = [...options.nodeFragments ?? []];
    this.actions = [...options.actions ?? []];
    this.headers = options.headers === void 0 ? /* @__PURE__ */ new Map() : copyByteMap(options.headers);
  }
  static create(options = {}) {
    try {
      const result = new _WireMessage(options);
      const status = result.validate();
      return isOk(status) ? result : status;
    } catch (error) {
      return invalidArgumentError("Invalid WireMessage options.", [], error);
    }
  }
  get approxBytes() {
    let result = 8;
    for (const fragment of this.nodeFragments) result += fragment.approxBytes;
    for (const action of this.actions) result += action.approxBytes;
    for (const [key, value] of this.headers) result += key.length + value.byteLength;
    return result;
  }
  validate() {
    try {
      if (!Array.isArray(this.nodeFragments)) {
        return invalidArgumentError("WireMessage.nodeFragments must be an array.");
      }
      if (!Array.isArray(this.actions)) {
        return invalidArgumentError("WireMessage.actions must be an array.");
      }
      for (const fragment of this.nodeFragments) {
        if (!(fragment instanceof NodeFragment)) return invalidArgumentError("WireMessage.nodeFragments must contain NodeFragment values.");
        const status = fragment.validate();
        if (!isOk(status)) return status;
      }
      for (const action of this.actions) {
        if (!(action instanceof ActionMessage)) return invalidArgumentError("WireMessage.actions must contain ActionMessage values.");
        const status = action.validate();
        if (!isOk(status)) return status;
      }
      if (!(this.headers instanceof Map)) {
        return invalidArgumentError("WireMessage.headers must be a Map.");
      }
      for (const [key, value] of this.headers) {
        const status = validateName(key);
        if (!isOk(status)) return status;
        if (!(value instanceof Uint8Array)) {
          return invalidArgumentError(
            "WireMessage header values must be Uint8Array values."
          );
        }
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError("Invalid WireMessage value.", [], error);
    }
  }
  /** Whether this carries only headers and can serve as a lifecycle marker. */
  get isHalfClose() {
    return this.actions.length === 0 && this.nodeFragments.length === 0;
  }
  toMsgpack() {
    const status = this.validate();
    if (!isOk(status)) return status;
    const fragments = [];
    const actions = [];
    for (const fragment of this.nodeFragments) {
      const bytes = fragment.toMsgpack();
      if (!isOk(bytes)) return bytes;
      fragments.push(bytes);
    }
    for (const action of this.actions) {
      const bytes = action.toMsgpack();
      if (!isOk(bytes)) return bytes;
      actions.push(bytes);
    }
    return encodeMsgpackFields([_WireMessage.VERSION, fragments, actions, msgpackByteMap(this.headers)]);
  }
  static fromMsgpack(bytes) {
    const fields = decodeMsgpackFields(bytes, 4, "WireMessage");
    if (!isOk(fields)) return fields;
    const version = asUnsigned(fields[0], "WireMessage.version", UINT32_MAX2);
    if (!isOk(version)) return version;
    if (version !== _WireMessage.VERSION) {
      return invalidArgumentError(`Invalid serialized WireMessage version: ${version}`);
    }
    const fragments = [];
    const actions = [];
    for (const [index, target, decode2, field] of [
      [1, fragments, NodeFragment.fromMsgpack, "node_fragments"],
      [2, actions, ActionMessage.fromMsgpack, "actions"]
    ]) {
      const raw = fields[index];
      if (!Array.isArray(raw)) return invalidArgumentError(`WireMessage.${field} must be a list.`);
      for (const item of raw) {
        const encoded = asBinary(item, `WireMessage.${field}`);
        if (!isOk(encoded)) return encoded;
        const value = decode2(encoded);
        if (!isOk(value)) return value;
        target.push(value);
      }
    }
    const headers = decodeByteMap(fields[3], "WireMessage.headers");
    if (!isOk(headers)) return headers;
    return _WireMessage.create({ nodeFragments: fragments, actions, headers });
  }
  toJsonValue() {
    const status = this.validate();
    if (!isOk(status)) return status;
    return wireMessageToJsonValue(this);
  }
  toJson() {
    const value = this.toJsonValue();
    if (!isOk(value)) return value;
    try {
      return JSON.stringify(value);
    } catch (error) {
      return statusFromUnknown(error, "Failed to serialize WireMessage JSON.");
    }
  }
  static fromJson(value) {
    if (typeof value !== "string") {
      return invalidArgumentError("WireMessage JSON must be a string.");
    }
    try {
      return wireMessageFromJsonValue(JSON.parse(value));
    } catch (error) {
      return invalidArgumentError("Failed to parse WireMessage JSON.", [], error);
    }
  }
};
function makeHalfCloseMessage(headers = /* @__PURE__ */ new Map()) {
  return new WireMessage({ headers });
}
function makeNullChunk() {
  return new Chunk({ metadata: new ChunkMetadata({ mimetype: "application/octet-stream" }) });
}
function metadataToJson(metadata) {
  const result = { mimetype: metadata.mimetype };
  if (metadata.timestamp !== null) result.timestamp = metadata.timestamp.toISOString();
  if (metadata.attributes.size > 0) result.attributes = byteMapToJson(metadata.attributes);
  return result;
}
function chunkToJson(chunk) {
  const result = {};
  if (chunk.metadata !== null) result.metadata = metadataToJson(chunk.metadata);
  if (chunk.ref !== "") result.ref = chunk.ref;
  result.data = base64Encode(chunk.data);
  return result;
}
function fragmentToJson(fragment) {
  const result = {};
  if (fragment.id !== "") result.id = fragment.id;
  result.data = fragment.data instanceof Chunk ? chunkToJson(fragment.data) : {
    id: fragment.data.id,
    ...fragment.data.offset === 0 ? {} : { offset: fragment.data.offset },
    ...fragment.data.length === null ? {} : { length: fragment.data.length }
  };
  if (fragment.seq !== null) result.seq = fragment.seq;
  if (fragment.continued) result.continued = true;
  return result;
}
function portToJson(port) {
  return {
    ...port.name === "" ? {} : { name: port.name },
    ...port.id === "" ? {} : { id: port.id }
  };
}
function actionToJson(action) {
  return {
    id: action.id,
    name: action.name,
    ...action.inputs.length === 0 ? {} : { inputs: action.inputs.map(portToJson) },
    ...action.outputs.length === 0 ? {} : { outputs: action.outputs.map(portToJson) },
    ...action.headers.size === 0 ? {} : { headers: byteMapToJson(action.headers) }
  };
}
function wireMessageToJsonValue(message) {
  try {
    if (!(message instanceof WireMessage)) {
      return invalidArgumentError("message must be a WireMessage.");
    }
    const validation = message.validate();
    if (!isOk(validation)) return validation;
    return {
      ...message.nodeFragments.length === 0 ? {} : { node_fragments: message.nodeFragments.map(fragmentToJson) },
      ...message.actions.length === 0 ? {} : { actions: message.actions.map(actionToJson) },
      ...message.headers.size === 0 ? {} : { headers: byteMapToJson(message.headers) }
    };
  } catch (error) {
    return statusFromUnknown(error, "Serializing WireMessage JSON value raised.");
  }
}
function metadataFromJson(value) {
  const object = asRecord(value, "ChunkMetadata");
  if (!isOk(object)) return object;
  const mimetype = object.mimetype === void 0 ? "" : asString(object.mimetype, "ChunkMetadata.mimetype");
  if (!isOk(mimetype)) return mimetype;
  let timestamp = null;
  if (object.timestamp !== void 0 && object.timestamp !== null) {
    if (typeof object.timestamp !== "string") return invalidArgumentError("ChunkMetadata.timestamp must be an RFC 3339 string or null.");
    timestamp = new Date(object.timestamp);
    if (!isValidDate(timestamp)) return invalidArgumentError("Invalid ChunkMetadata.timestamp.");
  }
  const attributes = byteMapFromJson(object.attributes, "ChunkMetadata.attributes");
  if (!isOk(attributes)) return attributes;
  return ChunkMetadata.create({ mimetype, timestamp, attributes });
}
function chunkFromJson(value) {
  const object = asRecord(value, "Chunk");
  if (!isOk(object)) return object;
  let metadata = null;
  if (object.metadata !== void 0 && object.metadata !== null) {
    const decoded = metadataFromJson(object.metadata);
    if (!isOk(decoded)) return decoded;
    metadata = decoded;
  }
  const ref = object.ref === void 0 ? "" : asString(object.ref, "Chunk.ref");
  if (!isOk(ref)) return ref;
  let data = new Uint8Array();
  if (object.data !== void 0) {
    if (typeof object.data !== "string") return invalidArgumentError("Chunk.data must be a base64 string.");
    const decoded = base64Decode(object.data);
    if (!isOk(decoded)) return decoded;
    data = decoded;
  }
  return Chunk.create({ metadata, ref, data });
}
function nodeRefFromJson(value) {
  const id = value.id === void 0 ? "" : asString(value.id, "NodeRef.id");
  if (!isOk(id)) return id;
  const offset = value.offset === void 0 ? 0 : asUnsigned(value.offset, "NodeRef.offset", UINT32_MAX2);
  if (!isOk(offset)) return offset;
  let length = null;
  if (value.length !== void 0 && value.length !== null) {
    const decoded = asUnsigned(value.length, "NodeRef.length", UINT32_RANGE);
    if (!isOk(decoded)) return decoded;
    length = decoded;
  }
  return NodeRef.create({ id, offset, length });
}
function fragmentFromJson(value) {
  const object = asRecord(value, "NodeFragment");
  if (!isOk(object)) return object;
  const id = object.id === void 0 ? "" : asString(object.id, "NodeFragment.id");
  if (!isOk(id)) return id;
  const dataObject = asRecord(object.data, "NodeFragment.data");
  if (!isOk(dataObject)) return dataObject;
  const isNodeRef = dataObject.id !== void 0 && dataObject.data === void 0 && dataObject.ref === void 0 && dataObject.metadata === void 0;
  const data = isNodeRef ? nodeRefFromJson(dataObject) : chunkFromJson(dataObject);
  if (!isOk(data)) return data;
  let seq = null;
  if (object.seq !== void 0 && object.seq !== null) {
    const decoded = asUnsigned(object.seq, "NodeFragment.seq", UINT32_MAX2);
    if (!isOk(decoded)) return decoded;
    seq = decoded;
  }
  if (object.continued !== void 0 && typeof object.continued !== "boolean") {
    return invalidArgumentError("NodeFragment.continued must be a boolean.");
  }
  return NodeFragment.create({ id, data, seq, continued: object.continued });
}
function portFromJson(value) {
  const object = asRecord(value, "Port");
  if (!isOk(object)) return object;
  const name = object.name === void 0 ? "" : asString(object.name, "Port.name");
  if (!isOk(name)) return name;
  const id = object.id === void 0 ? "" : asString(object.id, "Port.id");
  return isOk(id) ? Port.create(name, id) : id;
}
function actionFromJson(value) {
  const object = asRecord(value, "ActionMessage");
  if (!isOk(object)) return object;
  const id = object.id === void 0 ? "" : asString(object.id, "ActionMessage.id");
  if (!isOk(id)) return id;
  const name = object.name === void 0 ? "" : asString(object.name, "ActionMessage.name");
  if (!isOk(name)) return name;
  const ports = [[], []];
  for (const [index, field] of [[0, "inputs"], [1, "outputs"]]) {
    const raw = object[field];
    if (raw === void 0) continue;
    if (!Array.isArray(raw)) return invalidArgumentError(`ActionMessage.${field} must be an array.`);
    for (const item of raw) {
      const port = portFromJson(item);
      if (!isOk(port)) return port;
      ports[index].push(port);
    }
  }
  const headers = byteMapFromJson(object.headers, "ActionMessage.headers");
  if (!isOk(headers)) return headers;
  return ActionMessage.create({ id, name, inputs: ports[0], outputs: ports[1], headers });
}
function wireMessageFromJsonValue(value) {
  try {
    return wireMessageFromJsonValueUnchecked(value);
  } catch (error) {
    return invalidArgumentError("Invalid WireMessage JSON value.", [], error);
  }
}
function wireMessageFromJsonValueUnchecked(value) {
  const object = asRecord(value, "WireMessage");
  if (!isOk(object)) return object;
  const fragments = [];
  const actions = [];
  if (object.node_fragments !== void 0) {
    if (!Array.isArray(object.node_fragments)) return invalidArgumentError("WireMessage.node_fragments must be an array.");
    for (const item of object.node_fragments) {
      const fragment = fragmentFromJson(item);
      if (!isOk(fragment)) return fragment;
      fragments.push(fragment);
    }
  }
  if (object.actions !== void 0) {
    if (!Array.isArray(object.actions)) return invalidArgumentError("WireMessage.actions must be an array.");
    for (const item of object.actions) {
      const action = actionFromJson(item);
      if (!isOk(action)) return action;
      actions.push(action);
    }
  }
  const headers = byteMapFromJson(object.headers, "WireMessage.headers");
  if (!isOk(headers)) return headers;
  return WireMessage.create({ nodeFragments: fragments, actions, headers });
}

// src/serial_tags.ts
var CHUNK_METADATA_TAG = "a11.ChunkMetadata";
var CHUNK_TAG = "a11.Chunk";
var NODE_REF_TAG = "a11.NodeRef";
var NODE_FRAGMENT_TAG = "a11.NodeFragment";
var PORT_TAG = "a11.Port";
var ACTION_MESSAGE_TAG = "a11.ActionMessage";
var WIRE_MESSAGE_TAG = "a11.WireMessage";
var STATUS_TAG = "a11.Status";

// src/wire_values.ts
var A11_SERIAL_TAG = Symbol.for("a11.serialTag");
function tagValue(value, tag) {
  Object.defineProperty(value, A11_SERIAL_TAG, {
    value: tag,
    enumerable: false,
    writable: true,
    configurable: true
  });
  return value;
}
function valueTag(value) {
  if (typeof value !== "object" || value === null) return null;
  const tag = value[A11_SERIAL_TAG];
  return typeof tag === "string" && tag !== "" ? tag : null;
}
function testTagged(tag) {
  return (value) => valueTag(value) === tag;
}
var codecs = [];
var byTag = /* @__PURE__ */ new Map();
function registerWireValueCodec(codec) {
  if (typeof codec?.tag !== "string" || codec.tag === "") {
    return invalidArgumentError("A wire value codec tag must be non-empty.");
  }
  if (typeof codec.test !== "function" || typeof codec.dump !== "function" || typeof codec.load !== "function") {
    return invalidArgumentError(
      `The wire value codec for ${codec.tag} must provide test, dump and load.`
    );
  }
  const existing = byTag.get(codec.tag);
  if (existing !== void 0) {
    if (existing === codec) return okStatus();
    return alreadyExistsError(`A wire value codec for ${codec.tag} is already registered.`);
  }
  byTag.set(codec.tag, codec);
  codecs.push(codec);
  return okStatus();
}
function wireValueCodecFor(value) {
  for (const codec of codecs) if (codec.test(value)) return codec;
  return null;
}
function wireValueCodecs() {
  return codecs;
}
function wireValueCodecCount() {
  return codecs.length;
}
function put(fields, key, value, omit) {
  if (!omit) fields[key] = value;
}
function readString(fields, key, fallback = "") {
  const value = fields[key];
  return typeof value === "string" ? value : fallback;
}
function readBytes(value) {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (typeof value === "string") {
    const decoded = base64Decode(value);
    return isOk(decoded) ? decoded : new Uint8Array();
  }
  return new Uint8Array();
}
function readByteMap(value) {
  const result = /* @__PURE__ */ new Map();
  if (value instanceof Map) {
    for (const [key, item] of value) result.set(String(key), readBytes(item));
  } else if (typeof value === "object" && value !== null) {
    for (const [key, item] of Object.entries(value)) result.set(key, readBytes(item));
  }
  return result;
}
function byteMapFields(map) {
  const result = {};
  for (const [key, value] of map) result[key] = value;
  return result;
}
function dumpChunkMetadata(value) {
  const fields = { mimetype: value.mimetype };
  put(fields, "timestamp", value.timestamp, value.timestamp === null);
  put(fields, "attributes", byteMapFields(value.attributes), value.attributes.size === 0);
  return fields;
}
function loadChunkMetadata(fields) {
  const raw = fields["timestamp"];
  let timestamp = raw instanceof Date ? raw : null;
  if (typeof raw === "string") {
    const parsed = new Date(raw);
    if (Number.isFinite(parsed.getTime())) timestamp = parsed;
  }
  return new ChunkMetadata({
    mimetype: readString(fields, "mimetype"),
    timestamp,
    attributes: readByteMap(fields["attributes"])
  });
}
function dumpChunk(value) {
  const fields = { data: value.data };
  if (value.metadata !== null) {
    const metadata = dumpChunkMetadata(value.metadata);
    if (!isOk(metadata)) return metadata;
    fields["metadata"] = metadata;
  }
  put(fields, "ref", value.ref, value.ref === "");
  return fields;
}
function loadChunk(fields) {
  const metadata = fields["metadata"];
  let parsed = null;
  if (metadata instanceof ChunkMetadata) {
    parsed = metadata;
  } else if (typeof metadata === "object" && metadata !== null) {
    const loaded = loadChunkMetadata(metadata);
    if (!isOk(loaded)) return loaded;
    parsed = loaded;
  }
  return new Chunk({
    data: readBytes(fields["data"]),
    metadata: parsed,
    ref: readString(fields, "ref")
  });
}
function dumpNodeRef(value) {
  const fields = { id: value.id };
  put(fields, "offset", value.offset, value.offset === 0);
  put(fields, "length", value.length, value.length === null);
  return fields;
}
function loadNodeRef(fields) {
  const length = fields["length"];
  const offset = fields["offset"];
  return new NodeRef({
    id: readString(fields, "id"),
    offset: typeof offset === "number" ? offset : Number(offset ?? 0),
    length: length === null || length === void 0 ? null : Number(length)
  });
}
function dumpNodeFragment(value) {
  const data = value.data instanceof NodeRef ? dumpNodeRef(value.data) : dumpChunk(value.data);
  if (!isOk(data)) return data;
  const fields = { data };
  put(fields, "id", value.id, value.id === "");
  put(fields, "seq", value.seq, value.seq === null);
  put(fields, "continued", value.continued, !value.continued);
  return fields;
}
function loadNodeFragment(fields) {
  const raw = fields["data"];
  let data;
  if (raw instanceof Chunk || raw instanceof NodeRef) {
    data = raw;
  } else if (typeof raw === "object" && raw !== null) {
    const inner = raw;
    const loaded = "data" in inner || "metadata" in inner ? loadChunk(inner) : loadNodeRef(inner);
    if (!isOk(loaded)) return loaded;
    data = loaded;
  } else {
    data = new Chunk();
  }
  const seq = fields["seq"];
  return new NodeFragment({
    id: readString(fields, "id"),
    data,
    seq: seq === null || seq === void 0 ? null : Number(seq),
    continued: fields["continued"] === true
  });
}
function dumpPort(value) {
  const fields = {};
  put(fields, "name", value.name, value.name === "");
  put(fields, "id", value.id, value.id === "");
  return fields;
}
function loadPort(fields) {
  return new Port(readString(fields, "name"), readString(fields, "id"));
}
function dumpPorts(ports) {
  const result = [];
  for (const port of ports) {
    const dumped = dumpPort(port);
    if (!isOk(dumped)) return dumped;
    result.push(dumped);
  }
  return result;
}
function loadPorts(value) {
  if (!Array.isArray(value)) return [];
  return value.map(
    (entry) => entry instanceof Port ? entry : new Port(
      readString(entry ?? {}, "name"),
      readString(entry ?? {}, "id")
    )
  );
}
function dumpActionMessage(value) {
  const inputs = dumpPorts(value.inputs);
  if (!isOk(inputs)) return inputs;
  const outputs = dumpPorts(value.outputs);
  if (!isOk(outputs)) return outputs;
  const fields = { id: value.id, name: value.name };
  put(fields, "inputs", inputs, inputs.length === 0);
  put(fields, "outputs", outputs, outputs.length === 0);
  put(fields, "headers", byteMapFields(value.headers), value.headers.size === 0);
  return fields;
}
function loadActionMessage(fields) {
  return new ActionMessage({
    id: readString(fields, "id"),
    name: readString(fields, "name"),
    inputs: loadPorts(fields["inputs"]),
    outputs: loadPorts(fields["outputs"]),
    headers: readByteMap(fields["headers"])
  });
}
function dumpWireMessage(value) {
  const actions = [];
  for (const action of value.actions) {
    const dumped = dumpActionMessage(action);
    if (!isOk(dumped)) return dumped;
    actions.push(dumped);
  }
  const fragments = [];
  for (const fragment of value.nodeFragments) {
    const dumped = dumpNodeFragment(fragment);
    if (!isOk(dumped)) return dumped;
    fragments.push(dumped);
  }
  const fields = {};
  put(fields, "actions", actions, actions.length === 0);
  put(fields, "node_fragments", fragments, fragments.length === 0);
  put(fields, "headers", byteMapFields(value.headers), value.headers.size === 0);
  return fields;
}
function loadWireMessage(fields) {
  const actions = [];
  for (const entry of Array.isArray(fields["actions"]) ? fields["actions"] : []) {
    const loaded = entry instanceof ActionMessage ? entry : loadActionMessage(entry ?? {});
    if (!isOk(loaded)) return loaded;
    actions.push(loaded);
  }
  const fragments = [];
  for (const entry of Array.isArray(fields["node_fragments"]) ? fields["node_fragments"] : []) {
    const loaded = entry instanceof NodeFragment ? entry : loadNodeFragment(entry ?? {});
    if (!isOk(loaded)) return loaded;
    fragments.push(loaded);
  }
  return new WireMessage({
    actions,
    nodeFragments: fragments,
    headers: readByteMap(fields["headers"])
  });
}
function dumpStatus(value) {
  const fields = { code: value.code, message: value.message };
  const details = value.details ?? [];
  put(fields, "details", details, details.length === 0);
  return fields;
}
function loadStatus(fields) {
  const code = fields["code"];
  const details = fields["details"];
  const status = {
    code: typeof code === "number" ? code : Number(code ?? 0),
    message: readString(fields, "message")
  };
  if (Array.isArray(details) && details.length > 0) {
    status.details = details;
  }
  return tagValue(status, STATUS_TAG);
}
function install() {
  const entries = [
    {
      tag: CHUNK_METADATA_TAG,
      test: (value) => value instanceof ChunkMetadata,
      dump: dumpChunkMetadata,
      load: loadChunkMetadata
    },
    {
      tag: CHUNK_TAG,
      test: (value) => value instanceof Chunk,
      dump: dumpChunk,
      load: loadChunk
    },
    {
      tag: NODE_REF_TAG,
      test: (value) => value instanceof NodeRef,
      dump: dumpNodeRef,
      load: loadNodeRef
    },
    {
      tag: NODE_FRAGMENT_TAG,
      test: (value) => value instanceof NodeFragment,
      dump: dumpNodeFragment,
      load: loadNodeFragment
    },
    {
      tag: PORT_TAG,
      test: (value) => value instanceof Port,
      dump: dumpPort,
      load: loadPort
    },
    {
      tag: ACTION_MESSAGE_TAG,
      test: (value) => value instanceof ActionMessage,
      dump: dumpActionMessage,
      load: loadActionMessage
    },
    {
      tag: WIRE_MESSAGE_TAG,
      test: (value) => value instanceof WireMessage,
      dump: dumpWireMessage,
      load: loadWireMessage
    },
    {
      tag: STATUS_TAG,
      test: testTagged(STATUS_TAG),
      dump: dumpStatus,
      load: loadStatus
    }
  ];
  for (const codec of entries) registerWireValueCodec(codec);
}
install();

// src/serialization.ts
var JSON_MIMETYPE = "application/json";
var MSGPACK_MIMETYPE = "application/x-msgpack";
var OCTET_STREAM_MIMETYPE = "application/octet-stream";
var TEXT_MIMETYPE = "text/plain";
var SELF_DESCRIBING_MEDIA_TYPES = /* @__PURE__ */ new Set([
  TEXT_MIMETYPE,
  OCTET_STREAM_MIMETYPE
]);
var MIME_TOKEN = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
function parseMimetype(value, patterns = false) {
  if (typeof value !== "string" || value.trim() === "") {
    return invalidArgumentError("Mimetype must be a non-empty string.");
  }
  const [rawMediaType, ...rawParameters] = value.split(";");
  const mediaType = (rawMediaType ?? "").trim().toLowerCase();
  const mediaParts = mediaType.split("/");
  const validPart = (part) => part.length > 0 && (patterns ? /^[!#$%&'*+.^_`|~0-9A-Za-z?*\[\]-]+$/.test(part) : MIME_TOKEN.test(part));
  if (mediaParts.length !== 2 || !validPart(mediaParts[0] ?? "") || !validPart(mediaParts[1] ?? "")) {
    return invalidArgumentError(`Invalid mimetype: ${value}.`);
  }
  const parameters = /* @__PURE__ */ new Map();
  for (const raw of rawParameters) {
    const separator = raw.indexOf("=");
    if (separator < 1) return invalidArgumentError(`Invalid mimetype parameter in ${value}.`);
    const name = raw.slice(0, separator).trim().toLowerCase();
    let parameter = raw.slice(separator + 1).trim();
    if (parameter.startsWith('"') && parameter.endsWith('"')) {
      parameter = parameter.slice(1, -1).replaceAll('\\"', '"').replaceAll("\\\\", "\\");
    }
    if (!MIME_TOKEN.test(name) || parameter === "" || parameters.has(name)) {
      return invalidArgumentError(`Invalid mimetype parameter in ${value}.`);
    }
    parameters.set(name, parameter);
  }
  return { mediaType, parameters };
}
var GENERIC_TAGS = /* @__PURE__ */ new Set([
  "object",
  "array",
  "string",
  "integer",
  "number",
  "boolean",
  "null"
]);
function formatMimetype(mimetype, tag) {
  const parameters = [...mimetype.parameters].filter(([name]) => name !== "type");
  if (!GENERIC_TAGS.has(tag) && !SELF_DESCRIBING_MEDIA_TYPES.has(mimetype.mediaType)) {
    parameters.push(["type", encodeURIComponent(tag)]);
  }
  return `${mimetype.mediaType}${parameters.map(([name, value]) => `;${name}=${value}`).join("")}`;
}
function wildcardMatches(value, pattern) {
  const escaped = pattern.replace(/[.+^${}()|\\]/g, "\\$&").replaceAll("*", ".*").replaceAll("?", ".");
  try {
    return new RegExp(`^${escaped}$`).test(value);
  } catch {
    return false;
  }
}
function mimetypeMatches(actual, selection) {
  if (!wildcardMatches(actual.mediaType, selection.mediaType)) return false;
  for (const [name, expected] of selection.parameters) {
    if (name === "type") continue;
    const value = actual.parameters.get(name);
    if (value === void 0 || !wildcardMatches(value, expected)) return false;
  }
  return true;
}
function registrationMatches(registered, selection) {
  if (!wildcardMatches(registered.mediaType, selection.mediaType)) return false;
  for (const [name, expected] of selection.parameters) {
    if (name === "type") continue;
    const value = registered.parameters.get(name);
    if (value !== void 0 && !wildcardMatches(value, expected)) return false;
  }
  return true;
}
function canonicalJsonTag(value) {
  if (value === null) return "null";
  switch (typeof value) {
    case "boolean":
      return "boolean";
    case "string":
      return "string";
    case "number":
      return Number.isInteger(value) ? "integer" : "number";
    case "object":
      if (Array.isArray(value)) return "array";
      if (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null) return "object";
  }
  return null;
}
function toWire(value, binary, seen = /* @__PURE__ */ new Set()) {
  if (value === null || typeof value === "boolean" || typeof value === "string") return value;
  if (typeof value === "number") return value;
  if (typeof value === "bigint") {
    if (!binary) return value;
    if (value < -(2n ** 63n) || value > 2n ** 64n - 1n) {
      return invalidArgumentError(
        "MessagePack cannot represent integers outside the 64-bit range; use JSON for arbitrary-precision integers."
      );
    }
    return value;
  }
  if (value instanceof Uint8Array) return binary ? value : bytesToBase64(value);
  if (value instanceof ArrayBuffer) return toWire(new Uint8Array(value), binary, seen);
  if (value instanceof Date) {
    if (!Number.isFinite(value.getTime())) return invalidArgumentError("Cannot serialize an invalid Date.");
    return value.toISOString();
  }
  if (typeof value !== "object") {
    return invalidArgumentError(`Values of type ${typeof value} cannot be serialized by the default codecs.`);
  }
  if (seen.has(value)) return invalidArgumentError("Cyclic values cannot be serialized.");
  seen.add(value);
  try {
    if (Array.isArray(value) || value instanceof Set) {
      const result2 = [];
      for (const item of value) {
        const encoded = toWire(item, binary, seen);
        if (!isOk(encoded)) return encoded;
        result2.push(encoded);
      }
      return result2;
    }
    if (value instanceof Map) {
      const result2 = {};
      for (const [key, item] of value) {
        const encoded = toWire(item, binary, seen);
        if (!isOk(encoded)) return encoded;
        result2[String(key)] = encoded;
      }
      return result2;
    }
    const wireValue = wireValueCodecFor(value);
    if (wireValue !== null) {
      const dumped = wireValue.dump(value);
      if (!isOk(dumped)) return dumped;
      return toWire(dumped, binary, seen);
    }
    if (Object.getPrototypeOf(value) !== Object.prototype && Object.getPrototypeOf(value) !== null) {
      return invalidArgumentError(`Objects of type ${value.constructor?.name ?? "unknown"} cannot be serialized by the default codecs.`);
    }
    const result = {};
    for (const [key, item] of Object.entries(value)) {
      const encoded = toWire(item, binary, seen);
      if (!isOk(encoded)) return encoded;
      result[key] = encoded;
    }
    return result;
  } finally {
    seen.delete(value);
  }
}
function bytesToBase64(bytes) {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  if (typeof btoa === "function") return btoa(binary);
  return Buffer.from(bytes).toString("base64");
}
function base64ToBytes(value) {
  try {
    if (typeof atob === "function") {
      const decoded = atob(value);
      return Uint8Array.from(decoded, (character) => character.charCodeAt(0));
    }
    return new Uint8Array(Buffer.from(value, "base64"));
  } catch (error) {
    return invalidArgumentError("Invalid base64 byte data.", [], error);
  }
}
function jsonSerialize(value) {
  const wire = toWire(value, false);
  if (!isOk(wire)) return wire;
  try {
    return utf8Encode(JSON.stringify(wire, (_key, item) => typeof item === "bigint" ? Number(item) : item));
  } catch (error) {
    return invalidArgumentError("Failed to serialize JSON.", [], error);
  }
}
function jsonDeserialize(data) {
  const text = utf8Decode(data);
  if (!isOk(text)) return text;
  try {
    return JSON.parse(text);
  } catch (error) {
    return invalidArgumentError("Invalid JSON data.", [], error);
  }
}
function msgpackSerialize(value) {
  const wire = toWire(value, true);
  if (!isOk(wire)) return wire;
  try {
    return encode(wire);
  } catch (error) {
    return invalidArgumentError("Failed to serialize MessagePack.", [], error);
  }
}
function msgpackDeserialize(data) {
  try {
    return decode(data, { useBigInt64: true });
  } catch (error) {
    return invalidArgumentError("Invalid MessagePack data.", [], error);
  }
}
function decodePayload(data, mimetype) {
  return mimetype === JSON_MIMETYPE ? jsonDeserialize(data) : msgpackDeserialize(data);
}
function deserializeWireType(tag, data, mimetype) {
  const decoded = decodePayload(data, mimetype);
  if (!isOk(decoded)) return decoded;
  if (tag === "datetime") {
    if (decoded instanceof Date) return decoded;
    if (typeof decoded !== "string") {
      return invalidArgumentError("Serialized datetime must be a string.");
    }
    const date = new Date(decoded);
    return Number.isFinite(date.getTime()) ? date : invalidArgumentError("Serialized datetime is invalid.");
  }
  if (tag === "set") {
    if (decoded instanceof Set) return decoded;
    return Array.isArray(decoded) ? new Set(decoded) : invalidArgumentError("Serialized set must be an array.");
  }
  if (tag === "map") {
    if (decoded instanceof Map) return decoded;
    if (typeof decoded !== "object" || decoded === null || Array.isArray(decoded)) {
      return invalidArgumentError("Serialized map must be an object.");
    }
    return new Map(Object.entries(decoded));
  }
  if (tag === "bigint") {
    if (typeof decoded === "bigint") return decoded;
    if (typeof decoded === "number" && Number.isInteger(decoded)) return BigInt(decoded);
    return invalidArgumentError("Serialized bigint must be an integer.");
  }
  return decoded;
}
function deserializeBytes(data, mimetype) {
  const decoded = decodePayload(data, mimetype);
  if (!isOk(decoded)) return decoded;
  if (decoded instanceof Uint8Array) return decoded;
  if (typeof decoded === "string") return base64ToBytes(decoded);
  return invalidArgumentError("Serialized bytes did not decode to byte data.");
}
var SerializationRegistry = class {
  codecs = [];
  nextOrder = 0;
  wireValueCache = null;
  wireValueGeneration = -1;
  constructor(options = {}) {
    if (options.registerDefaults ?? false) this.installDefaults();
  }
  /** Add one codec; duplicate tag/media-type pairs are rejected. */
  register(codec) {
    try {
      if (typeof codec.tag !== "string" || codec.tag === "") return invalidArgumentError("Serialization codec tag must be non-empty.");
      if (typeof codec.test !== "function" || typeof codec.serialize !== "function" || typeof codec.deserialize !== "function") {
        return invalidArgumentError("Serialization codec callbacks must be functions.");
      }
      const parsed = parseMimetype(codec.mimetype);
      if (!isOk(parsed)) return parsed;
      if (this.codecs.some((registered) => registered.tag === codec.tag && registered.parsed.mediaType === parsed.mediaType)) {
        return alreadyExistsError(`A codec for ${codec.tag} and ${parsed.mediaType} is already registered.`);
      }
      this.codecs.push({ ...codec, parsed, order: this.nextOrder++ });
      return { code: 0, message: "OK" };
    } catch (error) {
      return statusFromUnknown(error, "Could not register serialization codec.");
    }
  }
  /** Install JSON, MessagePack, bytes, Blob, Date, Set, Map, and bigint codecs. */
  registerDefaults() {
    return this.installDefaults();
  }
  installDefaults() {
    const textStatus = this.register({
      tag: "string",
      mimetype: TEXT_MIMETYPE,
      test: (value) => typeof value === "string",
      // utf8Decode is the strict one: it returns InvalidArgument rather
      // than substituting U+FFFD, so a peer's encoding bug is reported
      // where it arrives instead of becoming silent corruption.
      serialize: (value) => utf8Encode(value),
      deserialize: (data) => utf8Decode(data)
    });
    if (!isOk(textStatus)) return textStatus;
    const jsonTags = ["null", "boolean", "integer", "number", "string", "array", "object"];
    for (const mimetype of [JSON_MIMETYPE, MSGPACK_MIMETYPE]) {
      for (const tag of jsonTags) {
        const status2 = this.register({
          tag,
          mimetype,
          test: (value) => canonicalJsonTag(value) === tag,
          serialize: (value) => mimetype === JSON_MIMETYPE ? jsonSerialize(value) : msgpackSerialize(value),
          deserialize: (data) => mimetype === JSON_MIMETYPE ? jsonDeserialize(data) : msgpackDeserialize(data)
        });
        if (!isOk(status2)) return status2;
      }
    }
    let status = this.register({
      tag: "bytes",
      mimetype: OCTET_STREAM_MIMETYPE,
      test: (value) => value instanceof Uint8Array,
      serialize: (value) => value,
      deserialize: (data) => new Uint8Array(data)
    });
    if (!isOk(status)) return status;
    status = this.register({
      tag: "arraybuffer",
      mimetype: OCTET_STREAM_MIMETYPE,
      test: (value) => value instanceof ArrayBuffer,
      serialize: (value) => new Uint8Array(value),
      deserialize: (data) => new Uint8Array(data).buffer
    });
    if (!isOk(status)) return status;
    if (typeof Blob !== "undefined") {
      status = this.register({
        tag: "blob",
        mimetype: "application/octet-stream",
        test: (value) => value instanceof Blob,
        serialize: (value) => value,
        deserialize: (data, chunk) => new Blob([new Uint8Array(data).buffer], { type: mediaTypeOf(chunk.mimetype) })
      });
      if (!isOk(status)) return status;
    }
    for (const mimetype of [JSON_MIMETYPE, MSGPACK_MIMETYPE]) {
      status = this.register({
        tag: "bytes",
        mimetype,
        test: (value) => value instanceof Uint8Array,
        serialize: (value) => mimetype === JSON_MIMETYPE ? jsonSerialize(value) : msgpackSerialize(value),
        deserialize: (data) => deserializeBytes(data, mimetype)
      });
      if (!isOk(status)) return status;
    }
    for (const mimetype of [JSON_MIMETYPE, MSGPACK_MIMETYPE]) {
      const wireTypes = [
        ["bigint", (value) => typeof value === "bigint"],
        ["datetime", (value) => value instanceof Date],
        ["set", (value) => value instanceof Set],
        ["map", (value) => value instanceof Map]
      ];
      for (const [tag, test] of wireTypes) {
        status = this.register({
          tag,
          mimetype,
          test: (value) => test(value),
          serialize: (value) => mimetype === JSON_MIMETYPE ? jsonSerialize(value) : msgpackSerialize(value),
          deserialize: (data) => deserializeWireType(tag, data, mimetype)
        });
        if (!isOk(status)) return status;
      }
    }
    return { code: 0, message: "OK" };
  }
  /**
   * Codecs for the class-tagged types, derived from the wire-value registry.
   *
   * They are derived rather than registered because that registry grows on
   * import: an SDK module adds `a11.sdk.Interaction` whenever the application
   * first pulls it in, which is routinely after this registry was built. They
   * sort ahead of everything else, since a value that knows its own class must
   * not be claimed by the generic `object` codec that also matches it.
   */
  wireValueCodecs() {
    if (this.wireValueCache !== null && this.wireValueGeneration === wireValueCodecCount()) {
      return this.wireValueCache;
    }
    const derived = [];
    for (const wireValue of wireValueCodecs()) {
      for (const mimetype of [JSON_MIMETYPE, MSGPACK_MIMETYPE]) {
        const parsed = parseMimetype(mimetype);
        if (!isOk(parsed)) continue;
        const json = mimetype === JSON_MIMETYPE;
        derived.push({
          tag: wireValue.tag,
          mimetype,
          parsed,
          order: -1,
          test: (value) => wireValue.test(value),
          serialize: (value) => json ? jsonSerialize(value) : msgpackSerialize(value),
          deserialize: (data) => {
            const decoded = json ? jsonDeserialize(data) : msgpackDeserialize(data);
            if (!isOk(decoded)) return decoded;
            if (typeof decoded !== "object" || decoded === null || Array.isArray(decoded)) {
              return invalidArgumentError(`A ${wireValue.tag} payload must be an object.`);
            }
            return wireValue.load(decoded);
          }
        });
      }
    }
    this.wireValueCache = derived;
    this.wireValueGeneration = wireValueCodecCount();
    return derived;
  }
  /** Select a matching serializer and produce a tagged, owned Chunk. */
  async toChunk(value, mimetype = "") {
    try {
      if (typeof value?.then === "function") {
        return invalidArgumentError(
          "A Promise cannot be serialized: await the value before writing it."
        );
      }
      const selection = mimetype === "" ? null : parseMimetype(mimetype, true);
      if (selection !== null && !isOk(selection)) return selection;
      const candidates = [...this.wireValueCodecs(), ...this.codecs].filter(
        (codec2) => codec2.test(value) && (selection === null || registrationMatches(codec2.parsed, selection))
      ).sort((left, right) => left.order - right.order);
      if (candidates.length === 0) {
        return notFoundError(`No serializer is registered for the value${mimetype ? ` and ${mimetype}` : ""}.`);
      }
      const codec = candidates[0];
      let serializedResult;
      try {
        serializedResult = await codec.serialize(value);
      } catch (error) {
        return statusFromUnknown(error, `Serializer for ${codec.tag} failed.`);
      }
      if (!isOk(serializedResult)) return serializedResult;
      const serialized = serializedResult;
      const exactMimetype = formatMimetype(codec.parsed, codec.tag);
      if (serialized instanceof Chunk) {
        return Chunk.create({
          metadata: new ChunkMetadata({
            mimetype: exactMimetype,
            timestamp: serialized.metadata?.timestamp ?? null,
            attributes: serialized.metadata?.attributes
          }),
          ref: serialized.ref,
          data: serialized.data
        });
      }
      const data = await toBytesAsync(serialized);
      if (!isOk(data)) return data;
      const blobType = typeof Blob !== "undefined" && value instanceof Blob && value.type !== "" ? parseMimetype(value.type) : null;
      const outputMimetype = blobType !== null && isOk(blobType) ? formatMimetype(blobType, codec.tag) : exactMimetype;
      return Chunk.create({ metadata: new ChunkMetadata({ mimetype: outputMimetype }), data });
    } catch (error) {
      return statusFromUnknown(error, "Could not serialize value.");
    }
  }
  /**
   * Select a decoder from the chunk's metadata and return a typed value.
   *
   * `mimetypePatterns` constrains the *representation*. Which type comes back
   * is the chunk's `;type=`, or `expectedTag` when the caller names one. A
   * chunk with no type parameter is not underspecified — it holds exactly what
   * its format describes, and decodes to that.
   */
  async fromChunk(chunk, mimetypePatterns = "", expectedTag) {
    try {
      const validation = chunk.validate();
      if (!isOk(validation)) return validation;
      if (chunk.metadata === null || chunk.mimetype === "") {
        return invalidArgumentError("The chunk has no mimetype.");
      }
      const actual = parseMimetype(chunk.mimetype);
      if (!isOk(actual)) return actual;
      const encodedTagRaw = actual.parameters.get("type");
      let encodedTag;
      try {
        encodedTag = encodedTagRaw === void 0 ? void 0 : decodeURIComponent(encodedTagRaw);
      } catch (error) {
        return invalidArgumentError("The chunk contains an invalid encoded type tag.", [], error);
      }
      if (expectedTag !== void 0 && encodedTag !== void 0 && encodedTag !== expectedTag) {
        return invalidArgumentError(`The chunk contains ${encodedTag}, not ${expectedTag}.`);
      }
      const requested = typeof mimetypePatterns === "string" ? mimetypePatterns === "" ? [] : [mimetypePatterns] : [...mimetypePatterns];
      let selected = requested.length === 0;
      for (const pattern of requested) {
        const parsed = parseMimetype(pattern, true);
        if (!isOk(parsed)) return parsed;
        if (mimetypeMatches(actual, parsed)) selected = true;
      }
      if (!selected) {
        return notFoundError(
          `The chunk mimetype ${chunk.mimetype} does not match the requested patterns.`
        );
      }
      const wanted = encodedTag ?? expectedTag;
      const byMediaType = [...this.wireValueCodecs(), ...this.codecs].filter(
        (codec2) => (
          // A Blob carries its concrete browser media type (for example,
          // image/png) on the chunk. Its stable `blob` tag selects the binary
          // decoder independently of that concrete media type.
          wanted === "blob" && codec2.tag === "blob" || registrationMatches(codec2.parsed, actual)
        )
      );
      const generic = byMediaType.filter((codec2) => GENERIC_TAGS.has(codec2.tag));
      let candidates = wanted === void 0 ? generic : byMediaType.filter((codec2) => codec2.tag === wanted);
      if (candidates.length === 0 && expectedTag === void 0) {
        candidates = generic.length > 0 ? generic : byMediaType;
      }
      if (candidates.length === 0) {
        return notFoundError(`No deserializer is registered for ${chunk.mimetype}.`);
      }
      const codec = candidates.sort((left, right) => left.order - right.order)[0];
      try {
        const value = await codec.deserialize(new Uint8Array(chunk.data), chunk);
        return value;
      } catch (error) {
        return statusFromUnknown(error, `Deserializer for ${codec.tag} failed.`);
      }
    } catch (error) {
      return statusFromUnknown(error, "Could not deserialize chunk.");
    }
  }
};
function mediaTypeOf(mimetype) {
  const parsed = parseMimetype(mimetype);
  return isOk(parsed) ? parsed.mediaType : OCTET_STREAM_MIMETYPE;
}
var globalRegistry = new SerializationRegistry({ registerDefaults: true });
function getGlobalSerializationRegistry() {
  return globalRegistry;
}

// src/concurrency.ts
async function sleep(ms) {
  if (!Number.isFinite(ms) || ms < 0) {
    return invalidArgumentError("Sleep duration must be non-negative and finite.");
  }
  try {
    await new Promise((resolve) => setTimeout(resolve, ms));
    return okStatus();
  } catch (error) {
    return statusFromUnknown(error, "Sleep raised an exception.");
  }
}
var Deferred = class {
  promise;
  resolveInternal;
  settledInternal = false;
  constructor() {
    this.promise = new Promise((resolve) => {
      this.resolveInternal = resolve;
    });
  }
  get settled() {
    return this.settledInternal;
  }
  resolve(value) {
    if (this.settledInternal) return okStatus();
    this.settledInternal = true;
    this.resolveInternal(value);
    return okStatus();
  }
};
var CallbackScheduler = class {
  constructor(callbacksPerTurn = 64) {
    this.callbacksPerTurn = callbacksPerTurn;
  }
  queue = [];
  draining = false;
  schedule(callback, onError) {
    if (typeof callback !== "function") {
      return statusFromUnknown(
        new TypeError("Scheduled callback must be callable.")
      );
    }
    this.queue.push({ callback, onError });
    if (!this.draining) {
      this.draining = true;
      queueMicrotask(() => void this.drain());
    }
    return okStatus();
  }
  get pending() {
    return this.queue.length;
  }
  async drain() {
    let processed = 0;
    while (processed < this.callbacksPerTurn) {
      const entry = this.queue.shift();
      if (entry === void 0) break;
      let failure = null;
      try {
        const status = await entry.callback();
        failure = !isStatus(status) ? internalError("Scheduled callback returned a non-Status value.") : isOk(status) ? null : status;
      } catch (error) {
        failure = statusFromUnknown(error);
      }
      if (failure !== null && entry.onError !== void 0) {
        try {
          entry.onError(failure);
        } catch {
        }
      }
      ++processed;
    }
    if (this.queue.length === 0) {
      this.draining = false;
      return;
    }
    queueMicrotask(() => void this.drain());
  }
};
var storeCallbackScheduler = new CallbackScheduler();

// src/chunk_store.ts
var UINT32_MAX3 = 4294967295;
function deadlineMillis(deadline) {
  try {
    if (deadline === void 0 || deadline === null) return null;
    const value = deadline instanceof Date ? deadline.getTime() : deadline;
    return Number.isFinite(value) ? value : invalidArgumentError("Deadline must be a finite epoch millisecond value, a Date, or null.");
  } catch (error) {
    return invalidArgumentError("Deadline could not be read.", [], error);
  }
}
function validateUint(value, field, maximum = Number.MAX_SAFE_INTEGER) {
  return Number.isSafeInteger(value) && value >= 0 && value <= maximum ? okStatus() : invalidArgumentError(`${field} must be a non-negative integer no greater than ${maximum}.`);
}
function cloneMetadata(metadata) {
  return metadata === null ? null : new ChunkMetadata({
    mimetype: metadata.mimetype,
    timestamp: metadata.timestamp,
    attributes: metadata.attributes
  });
}
function cloneChunk(chunk) {
  return new Chunk({
    metadata: cloneMetadata(chunk.metadata),
    ref: chunk.ref,
    data: chunk.data
  });
}
function cloneFragment(fragment) {
  const data = fragment.data instanceof Chunk ? cloneChunk(fragment.data) : new NodeRef({
    id: fragment.data.id,
    offset: fragment.data.offset,
    length: fragment.data.length
  });
  return new NodeFragment({
    id: fragment.id,
    data,
    seq: fragment.seq,
    continued: fragment.continued
  });
}
function hasChunkStoreShape(value) {
  if (typeof value !== "object" || value === null) return false;
  try {
    const candidate = value;
    return [
      "get",
      "getByArrivalOrder",
      "next",
      "put",
      "putMany",
      "clearData",
      "getSeqForArrivalOrder",
      "getFinalSeq",
      "closeWritesWithStatus",
      "size",
      "getId"
    ].every((name) => typeof candidate[name] === "function");
  } catch {
    return false;
  }
}
var LocalChunkStore = class _LocalChunkStore {
  constructor(nodeId) {
    this.nodeId = nodeId;
  }
  chunks = /* @__PURE__ */ new Map();
  seqToArrivalOrder = /* @__PURE__ */ new Map();
  arrivalOrderToSeq = /* @__PURE__ */ new Map();
  waiters = /* @__PURE__ */ new Set();
  totalChunksPut = 0;
  totalChunksRead = 0;
  finalSeq = null;
  terminalStatus = null;
  /** Create an empty stream log for a validated node id. */
  static create(nodeId) {
    const status = validateName(nodeId);
    return isOk(status) ? new _LocalChunkStore(nodeId) : status;
  }
  getId() {
    return this.nodeId;
  }
  fragmentFor(seq) {
    const chunk = this.chunks.get(seq);
    if (chunk === void 0) {
      if (this.terminalStatus === null) return notFoundError("Fragment is not available yet.");
      if (!isOk(this.terminalStatus)) return this.terminalStatus;
      return notFoundError(`Chunk store closed without seq ${seq}`);
    }
    return new NodeFragment({
      id: this.nodeId,
      data: cloneChunk(chunk),
      seq,
      continued: this.finalSeq === null || seq < this.finalSeq
    });
  }
  notifyChange() {
    for (const waiter of this.waiters) {
      if (waiter.timer !== null) clearTimeout(waiter.timer);
      waiter.deferred.resolve();
    }
    this.waiters.clear();
  }
  async waitForChange(deadline, message) {
    if (deadline !== null && deadline <= Date.now()) return deadlineExceededError(message);
    const waiter = { deferred: new Deferred(), timer: null };
    this.waiters.add(waiter);
    if (deadline !== null) {
      waiter.timer = setTimeout(() => waiter.deferred.resolve(), Math.max(0, deadline - Date.now()));
    }
    try {
      await waiter.deferred.promise;
      if (deadline !== null && Date.now() >= deadline && this.waiters.has(waiter)) {
        return deadlineExceededError(message);
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Chunk store wait failed.");
    } finally {
      this.waiters.delete(waiter);
      if (waiter.timer !== null) clearTimeout(waiter.timer);
    }
  }
  async get(seq, deadline) {
    const seqStatus = validateUint(seq, "seq", UINT32_MAX3);
    if (!isOk(seqStatus)) return seqStatus;
    const parsedDeadline = deadlineMillis(deadline);
    if (!isOk(parsedDeadline)) return parsedDeadline;
    try {
      while (true) {
        if (this.chunks.has(seq) || this.terminalStatus !== null) return this.fragmentFor(seq);
        const wait = await this.waitForChange(
          parsedDeadline,
          "Chunk store fragment was not available before the deadline"
        );
        if (!isOk(wait)) return wait;
      }
    } catch (error) {
      return statusFromUnknown(error, "Chunk store get failed.");
    }
  }
  async getByArrivalOrder(arrivalOrder, deadline) {
    const orderStatus = validateUint(arrivalOrder, "arrivalOrder");
    if (!isOk(orderStatus)) return orderStatus;
    const parsedDeadline = deadlineMillis(deadline);
    if (!isOk(parsedDeadline)) return parsedDeadline;
    try {
      while (true) {
        const seq = this.arrivalOrderToSeq.get(arrivalOrder);
        if (seq !== void 0) {
          if (!this.chunks.has(seq)) return dataLossError("Chunk store index references a missing chunk");
          return this.fragmentFor(seq);
        }
        if (this.terminalStatus !== null) {
          return isOk(this.terminalStatus) ? notFoundError(`Chunk store closed without arrival order ${arrivalOrder}`) : this.terminalStatus;
        }
        const wait = await this.waitForChange(
          parsedDeadline,
          "Chunk store fragment was not available before the deadline"
        );
        if (!isOk(wait)) return wait;
      }
    } catch (error) {
      return statusFromUnknown(error, "Chunk store arrival-order get failed.");
    }
  }
  async next(deadline, limit = 1) {
    const limitStatus = validateUint(limit, "limit", 4294967296);
    if (!isOk(limitStatus)) return limitStatus;
    if (limit === 0) return invalidArgumentError("limit must be positive");
    const parsedDeadline = deadlineMillis(deadline);
    if (!isOk(parsedDeadline)) return parsedDeadline;
    const result = [];
    try {
      while (true) {
        if (this.totalChunksRead > UINT32_MAX3) return [...result, null];
        if (this.finalSeq !== null && this.totalChunksRead > this.finalSeq) {
          if (this.terminalStatus !== null && !isOk(this.terminalStatus) && result.length === 0) {
            return this.terminalStatus;
          }
          return [...result, null];
        }
        const expected = this.totalChunksRead;
        const chunk = this.chunks.get(expected);
        if (chunk !== void 0 && result.length < limit) {
          ++this.totalChunksRead;
          result.push(new NodeFragment({
            id: this.nodeId,
            data: cloneChunk(chunk),
            seq: expected,
            continued: this.finalSeq === null || expected < this.finalSeq
          }));
          continue;
        }
        if (result.length === limit) return result;
        if (this.terminalStatus !== null) {
          if (!isOk(this.terminalStatus) && result.length === 0) return this.terminalStatus;
          return [...result, null];
        }
        const wait = await this.waitForChange(
          parsedDeadline,
          "Expected seq was not available before the deadline"
        );
        if (!isOk(wait)) return result.length > 0 ? result : wait;
      }
    } catch (error) {
      return statusFromUnknown(error, "Chunk store next failed.");
    }
  }
  async put(fragment) {
    const result = await this.putMany([fragment]);
    if (!isOk(result)) return result;
    return result.length === 1 ? result[0] : dataLossError("PutMany did not return exactly one sequence");
  }
  async putMany(fragments) {
    try {
      if (!Array.isArray(fragments)) return invalidArgumentError("fragments must be an array.");
      let anyExplicit = false;
      let allExplicit = true;
      const explicit = /* @__PURE__ */ new Set();
      for (const fragment of fragments) {
        if (!(fragment instanceof NodeFragment)) return invalidArgumentError("fragments must contain NodeFragment values.");
        const validation = fragment.validate();
        if (!isOk(validation)) return validation;
        anyExplicit ||= fragment.seq !== null;
        allExplicit &&= fragment.seq !== null;
        if (fragment.seq !== null) {
          if (explicit.has(fragment.seq)) return invalidArgumentError(`Explicit seq ${fragment.seq} occurs more than once`);
          explicit.add(fragment.seq);
        }
        if (!(fragment.data instanceof Chunk)) return unimplementedError("LocalChunkStore supports Chunk payloads, not NodeRef");
      }
      if (anyExplicit !== allExplicit) return invalidArgumentError("Sequence numbers must be set on every fragment or none");
      if (this.terminalStatus !== null) return failedPreconditionError(`Chunk store ${this.nodeId} is closed for writes`);
      if (fragments.length === 0) return [];
      const assigned = [];
      if (allExplicit) {
        for (const fragment of fragments) assigned.push(fragment.seq);
      } else {
        let candidate = this.totalChunksPut;
        for (let index = 0; index < fragments.length; ++index) {
          while (candidate <= UINT32_MAX3 && this.chunks.has(candidate)) ++candidate;
          if (candidate > UINT32_MAX3) return resourceExhaustedError("Maximum implicit sequence number exceeded");
          assigned.push(candidate++);
        }
      }
      for (const seq of assigned) {
        if (this.chunks.has(seq)) return alreadyExistsError(`A fragment with seq ${seq} already exists`);
      }
      let batchFinal = null;
      let sawFinal = false;
      for (let index = 0; index < fragments.length; ++index) {
        if (fragments[index].continued) {
          if (sawFinal && !allExplicit) return invalidArgumentError("The final implicit fragment must be last");
          continue;
        }
        if (sawFinal) return invalidArgumentError("More than one fragment in the batch is marked final");
        sawFinal = true;
        batchFinal = assigned[index];
      }
      if (batchFinal !== null && this.finalSeq !== null && batchFinal !== this.finalSeq) {
        return failedPreconditionError("The chunk store already has a different final sequence");
      }
      const pendingFinal = batchFinal ?? this.finalSeq;
      if (pendingFinal !== null) {
        if (assigned.some((seq) => seq > pendingFinal)) return invalidArgumentError("A fragment sequence exceeds the final sequence");
        if ([...this.chunks.keys()].some((seq) => seq > pendingFinal)) {
          return invalidArgumentError("An existing fragment exceeds the proposed final sequence");
        }
      }
      for (let index = 0; index < fragments.length; ++index) {
        const seq = assigned[index];
        const arrival = this.totalChunksPut + index;
        this.chunks.set(seq, cloneChunk(fragments[index].data));
        this.arrivalOrderToSeq.set(arrival, seq);
        this.seqToArrivalOrder.set(seq, arrival);
      }
      this.totalChunksPut += fragments.length;
      this.finalSeq = pendingFinal;
      this.notifyChange();
      return assigned;
    } catch (error) {
      return statusFromUnknown(error, "Chunk store put failed.");
    }
  }
  async clearData(seq) {
    const seqStatus = validateUint(seq, "seq", UINT32_MAX3);
    if (!isOk(seqStatus)) return seqStatus;
    try {
      const found = this.chunks.get(seq);
      if (found === void 0) return notFoundError(`No fragment with seq ${seq} exists`);
      const original = cloneChunk(found);
      this.chunks.set(seq, new Chunk({ metadata: cloneMetadata(found.metadata), ref: "__tombstone__" }));
      return new NodeFragment({
        id: this.nodeId,
        data: original,
        seq,
        continued: this.finalSeq === null || seq < this.finalSeq
      });
    } catch (error) {
      return statusFromUnknown(error, "Chunk store clear failed.");
    }
  }
  async getSeqForArrivalOrder(arrivalOrder) {
    const status = validateUint(arrivalOrder, "arrivalOrder");
    if (!isOk(status)) return status;
    const seq = this.arrivalOrderToSeq.get(arrivalOrder);
    return seq === void 0 ? notFoundError(`No fragment has arrival order ${arrivalOrder}`) : seq;
  }
  async getFinalSeq() {
    return this.finalSeq;
  }
  async closeWritesWithStatus(status, returnStatusIfAlreadyClosed = false) {
    try {
      if (!isStatus(status)) {
        return invalidArgumentError("status must be an A11 Status.");
      }
      if (typeof returnStatusIfAlreadyClosed !== "boolean") {
        return invalidArgumentError(
          "returnStatusIfAlreadyClosed must be boolean."
        );
      }
      if (this.terminalStatus !== null) {
        return returnStatusIfAlreadyClosed ? this.terminalStatus : failedPreconditionError("Chunk store is already closed for writes");
      }
      this.terminalStatus = { ...status, details: status.details ? [...status.details] : void 0 };
      this.notifyChange();
      return this.terminalStatus;
    } catch (error) {
      return statusFromUnknown(error, "Chunk store close failed.");
    }
  }
  async size() {
    return this.chunks.size;
  }
};

// src/chunk_store_reader.ts
var UINT32_MAX4 = 4294967295;
var UINT32_RANGE2 = 4294967296;
function normalizeOptions(options) {
  try {
    if (typeof options !== "object" || options === null) {
      return invalidArgumentError("Reader options must be an object.");
    }
    const result = {
      ordered: options.ordered ?? true,
      popChunks: options.popChunks ?? false,
      numChunksToBuffer: options.numChunksToBuffer ?? 32,
      offset: options.offset ?? 0,
      maxChunksToRead: options.maxChunksToRead ?? null,
      stickyMimetype: options.stickyMimetype ?? false
    };
    if (typeof result.ordered !== "boolean" || typeof result.popChunks !== "boolean" || typeof result.stickyMimetype !== "boolean") {
      return invalidArgumentError("Reader ordered, popChunks, and stickyMimetype options must be boolean.");
    }
    if (!Number.isSafeInteger(result.numChunksToBuffer) || result.numChunksToBuffer < 0 || result.numChunksToBuffer > UINT32_RANGE2) {
      return outOfRangeError("numChunksToBuffer must be between 0 and 2^32.");
    }
    if (!Number.isSafeInteger(result.offset) || result.offset < 0 || result.offset > UINT32_MAX4) {
      return outOfRangeError("offset must be a uint32 integer.");
    }
    if (result.maxChunksToRead !== null && (!Number.isSafeInteger(result.maxChunksToRead) || result.maxChunksToRead < 0 || result.maxChunksToRead > UINT32_RANGE2)) {
      return outOfRangeError("maxChunksToRead must be between 0 and 2^32 or null.");
    }
    return result;
  } catch (error) {
    return invalidArgumentError("Reader options could not be read.", [], error);
  }
}
var ChunkStoreReader = class _ChunkStoreReader {
  store;
  options;
  position;
  chunksRead = 0;
  currentMimetype = "";
  status = null;
  buffer = [];
  pendingReads = [];
  operation = "none";
  queued = false;
  generation = 0;
  done = new Deferred();
  constructor(store, options) {
    this.store = store;
    this.options = Object.freeze({ ...options });
    this.position = options.offset;
  }
  /** Validate options, create the cursor, and start its lazy fetch pump. */
  static create(store, options = {}) {
    try {
      if (!hasChunkStoreShape(store)) {
        return invalidArgumentError("store must implement ChunkStore.");
      }
      const normalized = normalizeOptions(options);
      if (!isOk(normalized)) return normalized;
      const reader = new _ChunkStoreReader(store, normalized);
      const started = reader.ensureStarted();
      return isOk(started) ? reader : started;
    } catch (error) {
      return statusFromUnknown(error, "ChunkStoreReader could not be created.");
    }
  }
  /** Number of prefetched fragments currently ready for callers. */
  get bufferSize() {
    return this.buffer.length;
  }
  /** Reader terminal status, or OK while it is healthy and still running. */
  getStatus() {
    return this.status ?? okStatus();
  }
  /** Schedule fetching before the first `next()`; safe to call repeatedly. */
  ensureStarted() {
    return this.wake();
  }
  /** Stop fetching and release pending readers with cancellation. */
  cancel() {
    if (this.status === null) this.status = abortedError("ChunkStoreReader was cancelled");
    this.collectAvailable();
    this.completeDone();
    return okStatus();
  }
  /** Await end-of-sequence, cancellation, or a store error. */
  wait() {
    return this.done.promise;
  }
  /** Await the next fragment; `null` is a clean logical end of the sequence. */
  next(timeoutMs) {
    if (timeoutMs !== void 0 && (!Number.isFinite(timeoutMs) || timeoutMs < 0)) {
      return Promise.resolve(invalidArgumentError("timeoutMs must be non-negative or omitted."));
    }
    const request = {
      active: true,
      deferred: new Deferred(),
      timer: null
    };
    if (timeoutMs !== void 0) {
      request.timer = setTimeout(() => {
        if (!request.active) return;
        request.active = false;
        request.deferred.resolve(deadlineExceededError("ChunkStoreReader next timed out before a fragment was available"));
        this.wake();
      }, timeoutMs);
    }
    this.pendingReads.push(request);
    this.wake();
    return request.deferred.promise;
  }
  /** Iterate to the clean reader end, yielding one terminal error when needed. */
  async *values(timeoutMs) {
    while (true) {
      const result = await this.next(timeoutMs);
      if (!isOk(result)) {
        yield result;
        return;
      }
      if (result === null) return;
      yield result;
    }
  }
  [Symbol.asyncIterator]() {
    return this.values();
  }
  wake() {
    if (this.queued || this.operation !== "none") return okStatus();
    this.queued = true;
    return storeCallbackScheduler.schedule(
      () => this.drive(),
      (status) => this.fail(status)
    );
  }
  drive() {
    this.queued = false;
    this.collectAvailable();
    if (this.status !== null || this.operation !== "none") {
      this.completeDone();
      return okStatus();
    }
    if (this.options.maxChunksToRead === 0) {
      this.status = okStatus();
      this.collectAvailable();
      this.completeDone();
      return okStatus();
    }
    const pendingCount = this.pendingReads.filter((request) => request.active).length;
    if (this.buffer.length >= this.options.numChunksToBuffer + pendingCount) return okStatus();
    if (this.position > UINT32_MAX4) {
      this.fail(outOfRangeError("ChunkStoreReader position exceeds the sequence range"));
      return okStatus();
    }
    this.operation = "fetch";
    const generation = ++this.generation;
    let pending;
    try {
      pending = this.options.ordered ? this.store.get(this.position) : this.store.getByArrivalOrder(this.position);
    } catch (error) {
      this.fetchDone(generation, statusFromUnknown(error, "ChunkStore reader fetch raised an exception"));
      return okStatus();
    }
    Promise.resolve(pending).then((result) => this.fetchDone(generation, result)).catch((error) => this.fetchDone(generation, statusFromUnknown(error, "ChunkStore reader fetch rejected")));
    return okStatus();
  }
  fetchDone(generation, result) {
    try {
      if (this.operation !== "fetch" || generation !== this.generation) return;
      this.operation = "none";
      if (this.status !== null) {
        this.wake();
        return;
      }
      if (isStatus(result) && !isOk(result)) {
        this.status = isNotFoundError(result) && this.options.maxChunksToRead === null ? okStatus() : result;
        this.wake();
        return;
      }
      if (!(result instanceof NodeFragment)) {
        this.fail(dataLossError("ChunkStore returned a value that is not a NodeFragment"));
        return;
      }
      const validation = result.validate();
      if (!isStatus(validation) || !isOk(validation)) {
        this.fail(
          isStatus(validation) && !isOk(validation) ? validation : dataLossError("NodeFragment validation returned an invalid status")
        );
        return;
      }
      if (result.seq === null) {
        this.fail(dataLossError("ChunkStore returned a fragment without a sequence number"));
        return;
      }
      if (!this.options.popChunks) {
        this.finishFragment(result);
        this.wake();
        return;
      }
      this.operation = "clear";
      const clearGeneration = ++this.generation;
      let pending;
      try {
        pending = this.store.clearData(result.seq);
      } catch (error) {
        this.clearDone(clearGeneration, statusFromUnknown(error, "ChunkStore clear raised an exception"));
        return;
      }
      Promise.resolve(pending).then((cleared) => this.clearDone(clearGeneration, cleared)).catch((error) => this.clearDone(clearGeneration, statusFromUnknown(error, "ChunkStore clear rejected")));
    } catch (error) {
      this.operation = "none";
      this.fail(statusFromUnknown(error, "Processing ChunkStore fetch result raised an exception"));
    }
  }
  clearDone(generation, result) {
    try {
      if (this.operation !== "clear" || generation !== this.generation) return;
      this.operation = "none";
      if (this.status === null) {
        if (isStatus(result) && !isOk(result)) this.status = result;
        else if (!(result instanceof NodeFragment)) {
          this.status = dataLossError("ChunkStore clearData returned a value that is not a NodeFragment");
        } else {
          const validation = result.validate();
          if (!isStatus(validation) || !isOk(validation)) {
            this.status = isStatus(validation) && !isOk(validation) ? validation : dataLossError("NodeFragment validation returned an invalid status");
          } else {
            this.finishFragment(result);
          }
        }
      }
      this.wake();
    } catch (error) {
      this.operation = "none";
      this.fail(statusFromUnknown(error, "Processing ChunkStore clear result raised an exception"));
    }
  }
  finishFragment(fragment) {
    if (this.options.ordered && this.options.stickyMimetype && fragment.data instanceof Chunk) {
      const mimetype = fragment.data.mimetype;
      if (mimetype !== "") {
        if (mimetype !== this.currentMimetype) this.currentMimetype = mimetype;
      } else if (this.currentMimetype !== "") {
        if (fragment.data.metadata === null) fragment.data.metadata = new ChunkMetadata();
        fragment.data.metadata.mimetype = this.currentMimetype;
      }
    }
    this.buffer.push(fragment);
    ++this.chunksRead;
    ++this.position;
    if (this.options.maxChunksToRead !== null && this.chunksRead === this.options.maxChunksToRead) {
      this.status = okStatus();
    } else if (this.options.ordered && !fragment.continued) {
      this.status = this.options.maxChunksToRead === null ? okStatus() : outOfRangeError(
        `The final fragment arrived after ${this.chunksRead} chunks, before maxChunksToRead=${this.options.maxChunksToRead}`
      );
    }
    this.collectAvailable();
  }
  collectAvailable() {
    while (this.buffer.length > 0) {
      const request2 = this.popRequest();
      if (request2 === null) break;
      const fragment = this.buffer.shift();
      this.resolveRequest(request2, fragment);
    }
    if (this.status === null || this.buffer.length > 0) return;
    let request;
    while ((request = this.popRequest()) !== null) {
      this.resolveRequest(request, isOk(this.status) ? null : this.status);
    }
  }
  popRequest() {
    while (this.pendingReads.length > 0) {
      const request = this.pendingReads.shift();
      if (!request.active) continue;
      request.active = false;
      return request;
    }
    return null;
  }
  resolveRequest(request, value) {
    if (request.timer !== null) clearTimeout(request.timer);
    request.deferred.resolve(value);
  }
  fail(status) {
    if (this.status === null) this.status = status;
    this.collectAvailable();
    this.completeDone();
  }
  completeDone() {
    if (this.status !== null && !this.done.settled) this.done.resolve(this.status);
  }
};

// src/action_schema.ts
var ACTION_STATUS_MIMETYPE = "application/x-a11-status";
var CLOSE_STATUS_ATTRIBUTE = "a11-close";
var ACTION_STATUS_OUTPUT = "__status__";
var ACTION_DISPATCH_STATUS_OUTPUT = "__dispatch_status__";
var CANCEL_ACTION_NAME = "__cancel__";
var CANCEL_ACTION_HEADER = "__action";
var ACTION_HEADER_PREFIX = "x-a11-";
var WHOLE_JSON_OUTPUT = "$";
var ActionPortSchema = class _ActionPortSchema {
  name;
  type;
  description;
  required;
  unary;
  autofills;
  constructor(options) {
    this.name = options.name;
    this.type = options.type;
    this.description = options.description ?? "";
    this.required = options.required ?? false;
    this.unary = options.unary ?? false;
    this.autofills = [...options.autofills ?? []];
  }
  static create(options) {
    try {
      const result = new _ActionPortSchema(options);
      const validation = result.validate();
      return isOk(validation) ? result : validation;
    } catch (error) {
      return invalidArgumentError("Invalid ActionPortSchema options.", [], error);
    }
  }
  validate() {
    try {
      const name = validateName(this.name);
      if (!isOk(name)) return name;
      if (typeof this.type !== "string" || this.type.length === 0) {
        return invalidArgumentError("Action port type must not be empty.");
      }
      if (typeof this.description !== "string") {
        return invalidArgumentError("Action port description must be a string.");
      }
      if (typeof this.required !== "boolean" || typeof this.unary !== "boolean") {
        return invalidArgumentError("Action port required and unary flags must be boolean.");
      }
      if (!Array.isArray(this.autofills)) {
        return invalidArgumentError("Action port autofills must be an array.");
      }
      for (const fragment of this.autofills) {
        if (fragment === null) continue;
        if (!(fragment instanceof NodeFragment)) {
          return invalidArgumentError("Each Action port autofill must be a NodeFragment or null.");
        }
        const validation = fragment.validate();
        if (!isOk(validation)) return validation;
      }
      return okStatus();
    } catch (error) {
      return invalidArgumentError("Invalid ActionPortSchema value.", [], error);
    }
  }
};
var ActionHeaderSchema = class _ActionHeaderSchema {
  name;
  description;
  defaultValue;
  constructionStatus = okStatus();
  constructor(options) {
    this.name = options.name;
    this.description = options.description ?? "";
    if (options.defaultValue === void 0 || options.defaultValue === null) {
      this.defaultValue = null;
    } else {
      const bytes = toBytes(options.defaultValue);
      if (isOk(bytes)) this.defaultValue = bytes;
      else {
        this.defaultValue = null;
        this.constructionStatus = bytes;
      }
    }
  }
  static create(options) {
    try {
      const result = new _ActionHeaderSchema(options);
      const validation = result.validate();
      return isOk(validation) ? result : validation;
    } catch (error) {
      return invalidArgumentError("Invalid ActionHeaderSchema options.", [], error);
    }
  }
  validate() {
    try {
      if (!isOk(this.constructionStatus)) return this.constructionStatus;
      const name = validateName(this.name);
      if (!isOk(name)) return name;
      if (this.defaultValue !== null && !(this.defaultValue instanceof Uint8Array)) {
        return invalidArgumentError(
          "Action header defaultValue must be byte data or null."
        );
      }
      return typeof this.description === "string" ? okStatus() : invalidArgumentError("Action header description must be a string.");
    } catch (error) {
      return invalidArgumentError("Invalid ActionHeaderSchema value.", [], error);
    }
  }
};
function collectionEntries(value) {
  return value instanceof Map ? value.entries() : Object.entries(value ?? {});
}
var ActionSchema = class _ActionSchema {
  name;
  description;
  inputs;
  outputs;
  headers;
  outputToJsonField;
  constructor(options) {
    this.name = options.name;
    this.description = options.description ?? "";
    this.inputs = new Map(collectionEntries(options.inputs));
    this.outputs = new Map(collectionEntries(options.outputs));
    this.headers = new Map(collectionEntries(options.headers));
    this.outputToJsonField = new Map(collectionEntries(options.outputToJsonField));
  }
  /** Construct and validate a schema before registering it. */
  static create(options) {
    try {
      const result = new _ActionSchema(options);
      const validation = result.validate();
      return isOk(validation) ? result : validation;
    } catch (error) {
      return invalidArgumentError("Could not create ActionSchema.", [], error);
    }
  }
  validate() {
    try {
      return this.validateUnchecked();
    } catch (error) {
      return invalidArgumentError("Invalid ActionSchema value.", [], error);
    }
  }
  validateUnchecked() {
    const name = validateName(this.name);
    if (!isOk(name)) return name;
    if (typeof this.description !== "string") {
      return invalidArgumentError("Action description must be a string.");
    }
    const reserved = /* @__PURE__ */ new Set([ACTION_STATUS_OUTPUT, ACTION_DISPATCH_STATUS_OUTPUT]);
    for (const ports of [this.inputs, this.outputs]) {
      if (!(ports instanceof Map)) return invalidArgumentError("Action ports must be a Map.");
      for (const [key, port] of ports) {
        const validKey = validateName(key);
        if (!isOk(validKey)) return validKey;
        if (!(port instanceof ActionPortSchema)) {
          return invalidArgumentError(`Action port '${key}' must be an ActionPortSchema.`);
        }
        const validPort = port.validate();
        if (!isOk(validPort)) return validPort;
        if (key !== port.name) {
          return invalidArgumentError(
            `Action port key '${key}' does not match port name '${port.name}'.`
          );
        }
        if (reserved.has(key)) {
          return invalidArgumentError(`Action port name '${key}' is reserved.`);
        }
      }
    }
    for (const [key, header] of this.headers) {
      const validKey = validateName(key);
      if (!isOk(validKey)) return validKey;
      if (!(header instanceof ActionHeaderSchema)) {
        return invalidArgumentError(`Action header '${key}' must be an ActionHeaderSchema.`);
      }
      const validHeader = header.validate();
      if (!isOk(validHeader)) return validHeader;
      if (key !== header.name) {
        return invalidArgumentError(
          `Action header key '${key}' does not match header name '${header.name}'.`
        );
      }
    }
    let wholeValues = 0;
    for (const [output, field] of this.outputToJsonField) {
      if (!this.outputs.has(output)) {
        return notFoundError(`Output '${output}' is not in the Action schema.`);
      }
      if (field === WHOLE_JSON_OUTPUT) ++wholeValues;
      else {
        const validField = validateName(field);
        if (!isOk(validField)) return validField;
      }
    }
    if (wholeValues > 1 || wholeValues === 1 && this.outputToJsonField.size !== 1) {
      return failedPreconditionError(
        "Only one output can map to the complete JSON value."
      );
    }
    return okStatus();
  }
  /** Map an output into a JSON field, or `$` as the whole result value. */
  mapOutputToJson(outputName, fieldName = "") {
    try {
      const valid = validateName(outputName);
      if (!isOk(valid)) return valid;
      if (!this.outputs.has(outputName)) {
        return notFoundError(`Output '${outputName}' is not in the Action schema.`);
      }
      const field = fieldName || outputName;
      if (field === WHOLE_JSON_OUTPUT) {
        if (this.outputToJsonField.size > 0 && !(this.outputToJsonField.size === 1 && this.outputToJsonField.get(outputName) === WHOLE_JSON_OUTPUT)) {
          return failedPreconditionError(
            "Only one output can map to the complete JSON value."
          );
        }
      } else {
        const validField = validateName(field);
        if (!isOk(validField)) return validField;
      }
      this.outputToJsonField.set(outputName, field);
      return okStatus();
    } catch (error) {
      return invalidArgumentError("Mapping Action output raised.", [], error);
    }
  }
};
function statusToChunk(status, closing = false) {
  const bytes = packStatus(status);
  if (!isOk(bytes)) return bytes;
  const attributes = /* @__PURE__ */ new Map();
  if (closing) attributes.set(CLOSE_STATUS_ATTRIBUTE, Uint8Array.from([49]));
  return Chunk.create({
    metadata: new ChunkMetadata({ mimetype: ACTION_STATUS_MIMETYPE, attributes }),
    data: bytes
  });
}
function decodeStatusChunk(chunk) {
  if (!(chunk instanceof Chunk)) return invalidArgumentError("chunk must be a Chunk.");
  const validation = chunk.validate();
  if (!isOk(validation)) return validation;
  if (!isStatusChunk(chunk)) {
    return invalidArgumentError("Chunk does not contain an Action status.");
  }
  return decodeStatus(chunk.data);
}
function isStatusChunk(chunk) {
  return chunk instanceof Chunk && chunk.mimetype === ACTION_STATUS_MIMETYPE;
}
function isCloseStatusChunk(chunk) {
  return isStatusChunk(chunk) && chunk.metadata?.attributes.has(CLOSE_STATUS_ATTRIBUTE) === true;
}

// src/chunk_store_writer.ts
var UINT32_MAX5 = 4294967295;
var UINT32_RANGE3 = 4294967296;
function normalizeOptions2(options) {
  try {
    if (typeof options !== "object" || options === null) {
      return invalidArgumentError("Writer options must be an object.");
    }
    const result = {
      offset: options.offset ?? 0,
      maxChunksToWriteAtOnce: options.maxChunksToWriteAtOnce ?? 8,
      numChunksToBuffer: options.numChunksToBuffer ?? null,
      stickyMimetype: options.stickyMimetype ?? false
    };
    if (typeof result.stickyMimetype !== "boolean") {
      return invalidArgumentError("stickyMimetype must be boolean.");
    }
    if (!Number.isSafeInteger(result.offset) || result.offset < 0 || result.offset > UINT32_MAX5) {
      return outOfRangeError("offset must be a uint32 integer.");
    }
    if (!Number.isSafeInteger(result.maxChunksToWriteAtOnce) || result.maxChunksToWriteAtOnce <= 0) {
      return invalidArgumentError("maxChunksToWriteAtOnce must be positive.");
    }
    if (result.maxChunksToWriteAtOnce > UINT32_RANGE3) return outOfRangeError("maxChunksToWriteAtOnce exceeds 2^32.");
    if (result.numChunksToBuffer !== null && (!Number.isSafeInteger(result.numChunksToBuffer) || result.numChunksToBuffer <= 0)) {
      return invalidArgumentError("numChunksToBuffer must be positive or null.");
    }
    if (result.numChunksToBuffer !== null && result.numChunksToBuffer > UINT32_RANGE3) {
      return outOfRangeError("numChunksToBuffer exceeds 2^32.");
    }
    return result;
  } catch (error) {
    return invalidArgumentError("Writer options could not be read.", [], error);
  }
}
var ChunkStoreWriter = class _ChunkStoreWriter {
  store;
  options;
  nextOffsetSeq;
  nextStickySeq;
  currentMimetype = "";
  queue = [];
  pendingQueue = [];
  outstanding = 0;
  status = null;
  closing = false;
  stopStatus = null;
  lifecycle = "none";
  lifecycleDone = null;
  drainWaiters = [];
  streams = [];
  operation = "none";
  queued = false;
  generation = 0;
  activeBatch = [];
  constructor(store, options) {
    this.store = store;
    this.options = Object.freeze({ ...options });
    this.nextOffsetSeq = options.offset;
    this.nextStickySeq = options.offset;
  }
  /** Validate options and create a writer over the supplied store. */
  static create(store, options = {}) {
    try {
      if (!hasChunkStoreShape(store)) {
        return invalidArgumentError("store must implement ChunkStore.");
      }
      const normalized = normalizeOptions2(options);
      return isOk(normalized) ? new _ChunkStoreWriter(store, normalized) : normalized;
    } catch (error) {
      return statusFromUnknown(error, "ChunkStoreWriter could not be created.");
    }
  }
  /** Number of admitted and backpressured chunks still awaiting completion. */
  get queueSize() {
    return this.queue.length + this.pendingQueue.length;
  }
  /** Terminal status, or `null` while the writer remains open. */
  getStatus() {
    return this.status;
  }
  /** Requested abort/cancel status, or `null` for a graceful lifecycle. */
  getAbortStatus() {
    return this.stopStatus;
  }
  /** Whether ordinary writes can still be accepted. */
  isWritable() {
    return this.status === null && !this.closing;
  }
  /** Schedule the flush pump before the first write; safe to call repeatedly. */
  ensureStarted() {
    return this.wake();
  }
  /** Enqueue a chunk and expose admission and persistence as separate promises. */
  enqueueChunk(chunk, seq = null, final = false, ensureStarted = true) {
    const admission = new Deferred();
    const confirmation = new Deferred();
    const failed = (status) => {
      admission.resolve(status);
      confirmation.resolve(status);
      return { admitted: admission.promise, confirmation: confirmation.promise };
    };
    if (!(chunk instanceof Chunk)) return failed(invalidArgumentError("chunk must be a Chunk."));
    const validation = chunk.validate();
    if (!isOk(validation)) return failed(validation);
    if (seq !== null && (!Number.isSafeInteger(seq) || seq < 0 || seq > UINT32_MAX5)) {
      return failed(invalidArgumentError("seq must be a uint32 integer or null."));
    }
    if (typeof final !== "boolean") return failed(invalidArgumentError("final must be boolean."));
    if (this.status !== null) {
      return failed(isOk(this.status) ? failedPreconditionError("ChunkStoreWriter is closed") : this.status);
    }
    if (this.closing) return failed(this.stopStatus ?? failedPreconditionError("ChunkStoreWriter is closing"));
    const requestedSeq = seq;
    if (seq === null && this.options.offset !== 0) {
      if (this.nextOffsetSeq > UINT32_MAX5) return failed(resourceExhaustedError("Maximum writer sequence number exceeded"));
      seq = this.nextOffsetSeq++;
    }
    if (this.options.stickyMimetype) {
      chunk = cloneChunk(chunk);
      const explicitSequenceGap = requestedSeq !== null && requestedSeq !== this.nextStickySeq;
      const mimetype = chunk.mimetype;
      if (explicitSequenceGap || mimetype !== this.currentMimetype) {
        this.currentMimetype = mimetype;
      } else if (chunk.metadata !== null) {
        chunk.metadata.mimetype = "";
        if (chunk.metadata.timestamp === null && chunk.metadata.attributes.size === 0) {
          chunk.metadata = null;
        }
      }
      this.nextStickySeq = requestedSeq === null ? this.nextStickySeq + 1 : requestedSeq + 1;
    }
    const element = {
      chunk,
      seq,
      continued: !final,
      admission,
      confirmation
    };
    if (this.options.numChunksToBuffer === null || this.outstanding < this.options.numChunksToBuffer) {
      this.queue.push(element);
      ++this.outstanding;
      admission.resolve(okStatus());
      if (ensureStarted) this.wake();
    } else {
      this.pendingQueue.push(element);
    }
    return { admitted: admission.promise, confirmation: confirmation.promise };
  }
  /** Apply backpressure, persist a chunk, and return its confirmed sequence. */
  async putChunk(chunk, seq = null, final = false) {
    const write = this.enqueueChunk(chunk, seq, final, true);
    const admitted = await write.admitted;
    if (!isOk(admitted)) return admitted;
    return write.confirmation;
  }
  /** Alias for {@link putChunk}. */
  put(chunk, seq = null, final = false) {
    return this.putChunk(chunk, seq, final);
  }
  /** Await all currently outstanding writes without closing the writer. */
  waitForBufferToDrain() {
    if (this.status !== null && !isOk(this.status)) return Promise.resolve(this.status);
    if (this.outstanding === 0 && this.pendingQueue.length === 0) return Promise.resolve(okStatus());
    const waiter = new Deferred();
    this.drainWaiters.push(waiter);
    this.wake();
    return waiter.promise;
  }
  /**
   * Flush queued chunks and close the backing store to further writes.
   *
   * This does not append a final fragment. Mark the last write `final` (or use
   * {@link AsyncNode.putNullFinal}) before draining when readers need a final
   * sequence number to identify the logical end of the stream.
   */
  drainAndClose() {
    if (this.lifecycle !== "none") {
      return this.lifecycle === "close" && this.lifecycleDone !== null ? this.lifecycleDone.promise : Promise.resolve(failedPreconditionError("ChunkStoreWriter is already being aborted"));
    }
    if (this.status !== null) {
      return Promise.resolve(isOk(this.status) ? failedPreconditionError("ChunkStoreWriter has already stopped") : this.status);
    }
    this.closing = true;
    this.lifecycle = "close";
    this.lifecycleDone = new Deferred();
    this.wake();
    return this.lifecycleDone.promise;
  }
  /** Reject queued writes and seal the store with a non-OK producer status. */
  abortWithStatus(status) {
    if (!isStatus(status)) {
      return Promise.resolve(invalidArgumentError("Abort status must be an A11 Status"));
    }
    if (isOk(status)) return Promise.resolve(invalidArgumentError("Abort status must be non-OK"));
    if (this.lifecycle !== "none") {
      return this.lifecycle === "abort" && this.lifecycleDone !== null ? this.lifecycleDone.promise : Promise.resolve(failedPreconditionError("ChunkStoreWriter is already being closed"));
    }
    if (this.status !== null) return Promise.resolve(isOk(this.status) ? failedPreconditionError("ChunkStoreWriter has already stopped") : this.status);
    this.closing = true;
    this.stopStatus = status;
    this.lifecycle = "abort";
    this.lifecycleDone = new Deferred();
    this.wake();
    return this.lifecycleDone.promise;
  }
  /** Abandon queued work immediately without persisting a store error status. */
  cancel() {
    if (this.lifecycle !== "none") {
      return this.lifecycle === "cancel" && this.lifecycleDone !== null ? this.lifecycleDone.promise : Promise.resolve(failedPreconditionError("ChunkStoreWriter is already stopping"));
    }
    if (this.status !== null) return Promise.resolve(failedPreconditionError("ChunkStoreWriter has already stopped"));
    this.closing = true;
    this.stopStatus = abortedError("ChunkStoreWriter was cancelled");
    this.lifecycle = "cancel";
    this.lifecycleDone = new Deferred();
    this.wake();
    return this.lifecycleDone.promise;
  }
  /** Tee stored fragments to a stream; a send failure stops later writes. */
  attachStream(stream) {
    try {
      if (stream === null || typeof stream !== "object" || typeof stream.send !== "function" || typeof stream.getId !== "function") {
        return invalidArgumentError("stream must implement WritableWireStream.");
      }
      const id = stream.getId();
      if (typeof id !== "string" || id.length === 0) {
        return invalidArgumentError("WritableWireStream.getId() must return a non-empty string.");
      }
    } catch (error) {
      return statusFromUnknown(error, "WireStream getId raised an exception");
    }
    if (!this.streams.includes(stream)) this.streams.push(stream);
    return okStatus();
  }
  /** Stop teeing future stored fragments to a previously attached stream. */
  detachStream(stream) {
    const index = this.streams.indexOf(stream);
    if (index >= 0) this.streams.splice(index, 1);
    return okStatus();
  }
  wake() {
    if (this.queued || this.operation !== "none") return okStatus();
    this.queued = true;
    return storeCallbackScheduler.schedule(
      () => this.drive(),
      (status) => this.fail(status)
    );
  }
  drive() {
    this.queued = false;
    if (this.operation !== "none") return okStatus();
    if (this.stopStatus !== null && this.status === null) {
      this.rejectElements(this.queue.splice(0), this.stopStatus);
      this.rejectElements(this.pendingQueue.splice(0), this.stopStatus);
      this.outstanding = 0;
      this.status = this.stopStatus;
      this.resolveDrainWaiters(this.stopStatus);
    }
    if (this.lifecycle === "cancel" && this.status !== null) {
      this.finishLifecycle(okStatus());
      return okStatus();
    }
    if (this.lifecycle === "abort" && this.status !== null) {
      return this.startClose(this.stopStatus);
    }
    if (this.status !== null && !isOk(this.status)) {
      this.finishLifecycle(this.status);
      return okStatus();
    }
    if (this.queue.length > 0) return this.startWrite();
    if (this.lifecycle === "close" && this.outstanding === 0 && this.pendingQueue.length === 0) {
      return this.startClose(okStatus());
    }
    if (this.outstanding === 0 && this.pendingQueue.length === 0) this.resolveDrainWaiters(okStatus());
    return okStatus();
  }
  startWrite() {
    const implicit = this.queue[0].seq === null;
    this.activeBatch = [];
    while (this.queue.length > 0 && this.activeBatch.length < this.options.maxChunksToWriteAtOnce && this.queue[0].seq === null === implicit) {
      this.activeBatch.push(this.queue.shift());
    }
    this.operation = "write";
    const generation = ++this.generation;
    let id;
    try {
      id = this.store.getId();
    } catch (error) {
      this.writeDone(generation, statusFromUnknown(error, "ChunkStore getId raised an exception"));
      return okStatus();
    }
    if (!isOk(id)) {
      this.writeDone(generation, id);
      return okStatus();
    }
    if (typeof id !== "string" || id.length === 0) {
      this.writeDone(generation, dataLossError("ChunkStore getId returned an invalid node id"));
      return okStatus();
    }
    const fragments = this.activeBatch.map((element) => new NodeFragment({
      id,
      data: element.chunk,
      seq: element.seq,
      continued: element.continued
    }));
    let pending;
    try {
      pending = this.store.putMany(fragments);
    } catch (error) {
      this.writeDone(generation, statusFromUnknown(error, "ChunkStore putMany raised an exception"));
      return okStatus();
    }
    Promise.resolve(pending).then((result) => this.writeDone(generation, result)).catch((error) => this.writeDone(generation, statusFromUnknown(error, "ChunkStore putMany rejected")));
    return okStatus();
  }
  writeDone(generation, result) {
    try {
      if (this.operation !== "write" || generation !== this.generation) return;
      this.operation = "none";
      let operationStatus = okStatus();
      if (isStatus(result) && !isOk(result)) operationStatus = result;
      else if (!Array.isArray(result)) operationStatus = dataLossError("ChunkStore putMany returned a value that is not an array");
      else if (result.some((seq) => !Number.isSafeInteger(seq) || seq < 0 || seq > UINT32_MAX5)) {
        operationStatus = dataLossError("ChunkStore putMany returned an invalid sequence number");
      } else if (result.length !== this.activeBatch.length) operationStatus = dataLossError("ChunkStore putMany returned the wrong number of sequences");
      else {
        for (let index = 0; index < result.length; ++index) {
          const explicit = this.activeBatch[index].seq;
          if (explicit !== null && explicit !== result[index]) {
            operationStatus = dataLossError("ChunkStore changed an explicit sequence number");
            break;
          }
        }
      }
      let teeStatus = okStatus();
      if (isOk(operationStatus) && this.streams.length > 0 && Array.isArray(result)) {
        let id;
        try {
          id = this.store.getId();
        } catch (error) {
          id = statusFromUnknown(error, "ChunkStore getId raised an exception");
        }
        if (!isOk(id)) teeStatus = id;
        else if (typeof id !== "string" || id.length === 0) {
          teeStatus = dataLossError("ChunkStore getId returned an invalid node id");
        } else {
          const message = new WireMessage({
            nodeFragments: this.activeBatch.map((element, index) => new NodeFragment({
              id,
              data: element.chunk,
              seq: result[index],
              continued: element.continued
            }))
          });
          for (const stream of this.streams) {
            try {
              const returned = stream.send(message);
              teeStatus = isStatus(returned) ? returned : dataLossError("WireStream send returned an invalid status");
            } catch (error) {
              teeStatus = statusFromUnknown(error, "WireStream send raised an exception");
            }
            if (!isOk(teeStatus)) break;
          }
        }
      }
      const completed = this.activeBatch.splice(0);
      this.outstanding -= completed.length;
      if (isOk(operationStatus) && Array.isArray(result)) {
        completed.forEach((element, index) => element.confirmation.resolve(result[index]));
      } else {
        this.rejectElements(
          completed,
          operationStatus
        );
      }
      if (this.stopStatus !== null) this.status = this.stopStatus;
      else if (!isOk(operationStatus)) this.status = operationStatus;
      else if (!isOk(teeStatus)) this.status = teeStatus;
      if (this.status !== null && !isOk(this.status)) {
        this.rejectElements(this.queue.splice(0), this.status);
        this.rejectElements(this.pendingQueue.splice(0), this.status);
        this.outstanding = 0;
        this.resolveDrainWaiters(this.status);
      } else {
        this.admitPending();
        if (this.outstanding === 0) this.resolveDrainWaiters(okStatus());
      }
      this.wake();
    } catch (error) {
      this.operation = "none";
      this.fail(statusFromUnknown(error, "Processing ChunkStore putMany result raised an exception"));
    }
  }
  /**
   * Tell attached streams that this writer closed.
   *
   * A peer ends a node on a not-continued fragment and closing writes none, so
   * the graceful path sends one closure marker after the last teed batch —
   * draining is already synchronised with the tee, since the close only starts
   * once every batch has gone out. Aborts send nothing here; the action layer
   * already fans failures out.
   */
  teeClose(closeStatus) {
    if (this.lifecycle !== "close" || this.streams.length === 0) return okStatus();
    let id;
    try {
      id = this.store.getId();
    } catch (error) {
      id = statusFromUnknown(error, "ChunkStore getId raised an exception");
    }
    if (!isOk(id)) return id;
    if (typeof id !== "string" || id.length === 0) {
      return dataLossError("ChunkStore getId returned an invalid node id");
    }
    const marker = statusToChunk(closeStatus, true);
    if (!isOk(marker)) return marker;
    const message = new WireMessage({
      nodeFragments: [new NodeFragment({ id, data: marker, seq: 0, continued: false })]
    });
    for (const stream of this.streams) {
      try {
        const returned = stream.send(message);
        if (isStatus(returned) && !isOk(returned)) return returned;
      } catch (error) {
        return statusFromUnknown(error, "WireStream send raised an exception");
      }
    }
    return okStatus();
  }
  startClose(requested) {
    if (this.operation === "close") return okStatus();
    this.operation = "close";
    const generation = ++this.generation;
    const teeStatus = this.teeClose(requested);
    let pending;
    try {
      pending = this.store.closeWritesWithStatus(requested);
    } catch (error) {
      this.closeDone(generation, requested, teeStatus, statusFromUnknown(error, "ChunkStore close raised an exception"));
      return okStatus();
    }
    Promise.resolve(pending).then((result) => this.closeDone(generation, requested, teeStatus, result)).catch((error) => this.closeDone(generation, requested, teeStatus, statusFromUnknown(error, "ChunkStore close rejected")));
    return okStatus();
  }
  closeDone(generation, requested, teeStatus, returned) {
    try {
      if (this.operation !== "close" || generation !== this.generation) return;
      this.operation = "none";
      if (!isStatus(returned)) {
        this.status = dataLossError("ChunkStore closeWritesWithStatus returned an invalid status");
        this.finishLifecycle(this.status);
        return;
      }
      if (returned.code !== requested.code) {
        this.status = dataLossError("ChunkStore closed with a different status than requested");
        this.finishLifecycle(this.status);
        return;
      }
      if (!isOk(teeStatus)) {
        this.status = teeStatus;
        this.closing = false;
        this.finishLifecycle(teeStatus);
        return;
      }
      this.status = requested;
      this.closing = false;
      this.finishLifecycle(okStatus());
    } catch (error) {
      this.operation = "none";
      this.fail(statusFromUnknown(error, "Processing ChunkStore close result raised an exception"));
    }
  }
  admitPending() {
    while (this.pendingQueue.length > 0 && (this.options.numChunksToBuffer === null || this.outstanding < this.options.numChunksToBuffer)) {
      const element = this.pendingQueue.shift();
      this.queue.push(element);
      ++this.outstanding;
      element.admission.resolve(okStatus());
    }
  }
  rejectElements(elements, status) {
    for (const element of elements) {
      element.admission.resolve(status);
      element.confirmation.resolve(status);
    }
  }
  resolveDrainWaiters(status) {
    for (const waiter of this.drainWaiters.splice(0)) waiter.resolve(status);
  }
  finishLifecycle(status) {
    if (this.lifecycleDone !== null && !this.lifecycleDone.settled) this.lifecycleDone.resolve(status);
  }
  fail(status) {
    if (isOk(status)) return;
    this.status = status;
    this.rejectElements(this.activeBatch.splice(0), status);
    this.rejectElements(this.queue.splice(0), status);
    this.rejectElements(this.pendingQueue.splice(0), status);
    this.outstanding = 0;
    this.resolveDrainWaiters(status);
    this.finishLifecycle(status);
  }
};

// src/async_node.ts
var AsyncNode = class _AsyncNode {
  /** Ordered storage shared by the node's reader and writer. */
  chunkStore;
  /** Owning node map, or `null` for a standalone node. */
  nodeMap;
  registry;
  readerOptions;
  writerOptions;
  readerInternal;
  writerInternal;
  expectedMimetypePatterns = "";
  expectedTag;
  constructor(store, reader, writer, options) {
    this.chunkStore = store;
    this.nodeMap = options.nodeMap ?? null;
    this.registry = options.serializationRegistry ?? getGlobalSerializationRegistry();
    this.readerOptions = { ...options.readerOptions ?? {} };
    this.writerOptions = { ...options.writerOptions ?? {} };
    this.readerInternal = reader;
    this.writerInternal = writer;
  }
  /** Build a node over an existing store, preserving its data and id. */
  static fromStore(store, options = {}) {
    try {
      if (!hasChunkStoreShape(store)) {
        return invalidArgumentError("store must implement ChunkStore.");
      }
      if (!(options.serializationRegistry ?? getGlobalSerializationRegistry() instanceof SerializationRegistry)) {
        return invalidArgumentError("serializationRegistry must be a SerializationRegistry.");
      }
      const reader = ChunkStoreReader.create(store, options.readerOptions);
      if (!isOk(reader)) return reader;
      const writer = ChunkStoreWriter.create(store, options.writerOptions);
      if (!isOk(writer)) {
        reader.cancel();
        return writer;
      }
      return new _AsyncNode(store, reader, writer, options);
    } catch (error) {
      return statusFromUnknown(error, "AsyncNode could not be created.");
    }
  }
  /** Create a backing store for `nodeId`, then build its reader and writer. */
  static async create(nodeId, options = {}) {
    const validation = validateName(nodeId);
    if (!isOk(validation)) return validation;
    const factory = options.chunkStoreFactory ?? ((id) => LocalChunkStore.create(id));
    try {
      const store = await factory(nodeId);
      if (!isOk(store)) return store;
      return _AsyncNode.fromStore(store, options);
    } catch (error) {
      return statusFromUnknown(error, "Chunk-store factory raised an exception.");
    }
  }
  /** Return the stable id used by fragments, actions, and sessions. */
  getId() {
    try {
      const id = this.chunkStore.getId();
      if (isStatus(id) && !isOk(id)) return id;
      return typeof id === "string" ? id : internalError("ChunkStore.getId() returned an invalid value.");
    } catch (error) {
      return statusFromUnknown(error, "ChunkStore.getId() raised an exception.");
    }
  }
  /** Low-level consuming cursor; prefer `next` for typed values. */
  get reader() {
    return this.readerInternal;
  }
  /** Low-level producing cursor; prefer `put` for typed values. */
  get writer() {
    return this.writerInternal;
  }
  /** Codec registry currently used at the application boundary. */
  get serializationRegistry() {
    return this.registry;
  }
  /** Replace the codecs used by subsequent typed reads and writes. */
  setSerializationRegistry(registry) {
    if (!(registry instanceof SerializationRegistry)) return invalidArgumentError("registry must be a SerializationRegistry.");
    this.registry = registry;
    return okStatus();
  }
  /** Set default MIME/type constraints for subsequent typed reads. */
  setExpectedTypes(mimetypePatterns = "", expectedTag) {
    try {
      if (typeof mimetypePatterns !== "string" && !Array.isArray(mimetypePatterns)) {
        return invalidArgumentError("mimetypePatterns must be a string or string array.");
      }
      if (Array.isArray(mimetypePatterns) && mimetypePatterns.some((item) => typeof item !== "string")) {
        return invalidArgumentError("Every mimetype pattern must be a string.");
      }
      if (expectedTag !== void 0 && (typeof expectedTag !== "string" || expectedTag === "")) {
        return invalidArgumentError("expectedTag must be a non-empty string or omitted.");
      }
      this.expectedMimetypePatterns = mimetypePatterns;
      this.expectedTag = expectedTag;
      return okStatus();
    } catch (error) {
      return invalidArgumentError("Expected type options could not be read.", [], error);
    }
  }
  getReaderOptions() {
    return { ...this.readerOptions };
  }
  getWriterOptions() {
    return { ...this.writerOptions };
  }
  /** Replace and rewind the independent read cursor, optionally from an offset. */
  resetReader(options) {
    try {
      const nextOptions = options ?? this.readerOptions;
      const reader = ChunkStoreReader.create(this.chunkStore, nextOptions);
      if (!isOk(reader)) return reader;
      this.readerInternal.cancel();
      this.readerInternal = reader;
      this.readerOptions = { ...reader.options };
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Resetting AsyncNode reader raised an exception.");
    }
  }
  setReaderOptions(options) {
    return this.resetReader(options);
  }
  /** Replace writer policy before any write has started. */
  setWriterOptions(options) {
    try {
      if (this.writerInternal.queueSize !== 0 || !this.writerInternal.isWritable()) {
        return failedPreconditionError("Writer options can only be changed before writing starts.");
      }
      const writer = ChunkStoreWriter.create(this.chunkStore, options);
      if (!isOk(writer)) return writer;
      void this.writerInternal.cancel();
      this.writerInternal = writer;
      this.writerOptions = { ...writer.options };
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Changing AsyncNode writer options raised an exception.");
    }
  }
  getReaderStatus() {
    return this.readerInternal.getStatus();
  }
  getWriterStatus() {
    return this.writerInternal.getStatus() ?? okStatus();
  }
  getWriterAbortStatus() {
    return this.writerInternal.getAbortStatus();
  }
  async isWritable() {
    return this.writerInternal.isWritable();
  }
  /** Persist a raw chunk, optionally at an explicit sequence and/or as final. */
  putChunk(chunk, seq = null, final = false) {
    return this.writerInternal.putChunk(chunk, seq, final);
  }
  /** Persist a fragment carrying its own sequence and continuation marker. */
  putFragment(fragment) {
    if (!(fragment instanceof NodeFragment)) return Promise.resolve(invalidArgumentError("fragment must be a NodeFragment."));
    if (!(fragment.data instanceof Chunk)) return Promise.resolve(unimplementedError("AsyncNode writers do not resolve NodeRef payloads."));
    return this.putChunk(fragment.data, fragment.seq, !fragment.continued);
  }
  /** Serialize and persist one application value, respecting writer backpressure. */
  async put(value, options = {}) {
    try {
      if (typeof options !== "object" || options === null) {
        return invalidArgumentError("AsyncNode put options must be an object.");
      }
      const seq = options.seq ?? null;
      const final = options.final ?? false;
      const mimetype = options.mimetype ?? "";
      if (typeof final !== "boolean" || typeof mimetype !== "string") {
        return invalidArgumentError("final must be boolean and mimetype must be a string.");
      }
      if (value instanceof NodeFragment) {
        if (seq !== null || final || mimetype !== "") return invalidArgumentError("seq, final, and mimetype are carried by a NodeFragment and cannot be supplied separately.");
        return this.putFragment(value);
      }
      if (value instanceof Chunk) {
        if (mimetype !== "") return invalidArgumentError("mimetype cannot be supplied with a raw Chunk.");
        return this.putChunk(value, seq, final);
      }
      const chunk = await this.registry.toChunk(value, mimetype);
      if (!isOk(chunk)) return chunk;
      return this.putChunk(chunk, seq, final);
    } catch (error) {
      return statusFromUnknown(error, "Writing AsyncNode value raised an exception.");
    }
  }
  /** Write the last application value and establish the final sequence. */
  putFinal(value, seq = null, mimetype = "") {
    return this.put(value, { seq, final: true, mimetype });
  }
  /** Establish a final sequence with an explicit null marker and no value. */
  putNullFinal(seq = null) {
    return this.putChunk(makeNullChunk(), seq, true);
  }
  /** Read the next raw fragment, or `null` at the clean end of sequence. */
  nextFragment(timeoutMs) {
    return this.readerInternal.next(timeoutMs);
  }
  /** Read the next inline chunk without deserializing its payload. */
  async nextChunk(timeoutMs) {
    const fragment = await this.nextFragment(timeoutMs);
    if (!isOk(fragment) || fragment === null) return fragment;
    return fragment.getChunk();
  }
  /**
   * The next fragment carrying a value, or `null` once the node ends.
   *
   * A null chunk is a marker, not a value: a final one says the node is
   * finished, and a non-final one says nothing at all. Neither is something a
   * reader asked for, so both are skipped here rather than surfaced as a value
   * or rejected — which is what lets a node be closed with nothing in it.
   */
  async nextValueFragment(timeoutMs) {
    const started = Date.now();
    for (; ; ) {
      const remaining = timeoutMs === void 0 ? void 0 : Math.max(0, timeoutMs - (Date.now() - started));
      const fragment = await this.nextFragment(remaining);
      if (!isOk(fragment) || fragment === null) return fragment;
      const chunk = fragment.getChunk();
      if (!isOk(chunk)) return chunk;
      if (!chunk.isNull) return fragment;
      if (!fragment.continued) return null;
    }
  }
  /** Read one value, or `null` at finality, clean closure, or the reader limit. */
  async next(optionsOrTimeout = {}) {
    try {
      const options = typeof optionsOrTimeout === "number" ? { timeoutMs: optionsOrTimeout } : optionsOrTimeout;
      if (typeof options !== "object" || options === null) {
        return invalidArgumentError("AsyncNode next options must be an object or timeout in milliseconds.");
      }
      const fragment = await this.nextValueFragment(options.timeoutMs);
      if (!isOk(fragment) || fragment === null) return fragment;
      const chunk = fragment.getChunk();
      if (!isOk(chunk)) return chunk;
      return this.registry.fromChunk(
        chunk,
        options.mimetypePatterns ?? this.expectedMimetypePatterns,
        options.expectedTag ?? this.expectedTag
      );
    } catch (error) {
      return statusFromUnknown(error, "Reading AsyncNode value raised an exception.");
    }
  }
  /**
   * Consume exactly one whole value's fragment and validate its terminator.
   * Use this for unary action ports; streaming ports should call `next`.
   */
  async consumeFragment(options = {}) {
    try {
      return await this.consumeFragmentInternal(options);
    } catch (error) {
      return statusFromUnknown(error, "Consuming AsyncNode fragment raised an exception.");
    }
  }
  async consumeFragmentInternal(options) {
    if (typeof options !== "object" || options === null) {
      return invalidArgumentError("AsyncNode consume options must be an object.");
    }
    if (options.allowNone !== void 0 && typeof options.allowNone !== "boolean") {
      return invalidArgumentError("allowNone must be boolean or omitted.");
    }
    if ((this.readerOptions.ordered ?? true) !== true) return failedPreconditionError("consume() requires an ordered reader.");
    const started = Date.now();
    const first = await this.nextValueFragment(options.timeoutMs);
    if (!isOk(first)) return first;
    if (first === null) return options.allowNone ? null : failedPreconditionError("AsyncNode is empty at the current reader offset.");
    if (!first.continued) return first;
    const remaining = options.timeoutMs === void 0 ? void 0 : Math.max(0, options.timeoutMs - (Date.now() - started));
    const terminator = await this.nextFragment(remaining);
    if (!isOk(terminator)) return terminator;
    if (terminator === null) return failedPreconditionError("A continued consumed value must be followed by a null final chunk.");
    const terminatorChunk = terminator.getChunk();
    if (!isOk(terminatorChunk)) return terminatorChunk;
    if (terminator.continued || !terminatorChunk.isNull) {
      return failedPreconditionError("The only fragment allowed after a consumed value is a null final chunk.");
    }
    return first;
  }
  async consumeChunk(options = {}) {
    const fragment = await this.consumeFragment(options);
    if (!isOk(fragment) || fragment === null) return fragment;
    return fragment.getChunk();
  }
  /** Consume and deserialize exactly one whole unary value. */
  async consume(options = {}) {
    try {
      const fragment = await this.consumeFragment(options);
      if (!isOk(fragment) || fragment === null) return fragment;
      if (options.raw !== void 0 && options.raw !== "fragment" && options.raw !== "chunk") {
        return invalidArgumentError("raw must be 'fragment', 'chunk', or omitted.");
      }
      if (options.raw === "fragment") return fragment;
      const chunk = fragment.getChunk();
      if (!isOk(chunk)) return chunk;
      if (options.raw === "chunk") return chunk;
      return this.registry.fromChunk(
        chunk,
        options.mimetypePatterns ?? this.expectedMimetypePatterns,
        options.expectedTag ?? this.expectedTag
      );
    } catch (error) {
      return statusFromUnknown(error, "Consuming AsyncNode value raised an exception.");
    }
  }
  /** Iterate to the clean reader end, yielding at most one terminal error. */
  async *values(options = {}) {
    while (true) {
      const value = await this.next(options);
      if (!isOk(value)) {
        yield value;
        return;
      }
      if (value === null) return;
      yield value;
    }
  }
  [Symbol.asyncIterator]() {
    return this.values();
  }
  /** Await outstanding writes without closing or adding a final marker. */
  waitForBufferToDrain() {
    return this.writerInternal.waitForBufferToDrain();
  }
  /**
   * Flush queued writes and close the writer without adding a final fragment.
   *
   * Call {@link putFinal} or {@link putNullFinal} first when readers must
   * synchronise on a definite end-of-stream sequence.
   */
  drainAndClose() {
    return this.writerInternal.drainAndClose();
  }
  /** Fail the producing half so readers observe a structured terminal error. */
  abortWithStatus(status) {
    return this.writerInternal.abortWithStatus(status);
  }
  /** Tee stored writes to a transport; `send` admission is not peer delivery. */
  attachStream(stream) {
    return this.writerInternal.attachStream(stream);
  }
  /** Stop mirroring writes to one transport. */
  detachStream(stream) {
    return this.writerInternal.detachStream(stream);
  }
  /** Stop this node's independent consuming cursor. */
  cancelReader() {
    return this.readerInternal.cancel();
  }
  /** Abandon queued writes and stop the producing cursor. */
  cancelWriter() {
    return this.writerInternal.cancel();
  }
  /** Cancel both halves of this local node object. */
  async cancel() {
    this.readerInternal.cancel();
    return this.writerInternal.cancel();
  }
};
var NodeMap = class {
  constructor(factory = (id) => LocalChunkStore.create(id)) {
    this.factory = factory;
  }
  nodes = /* @__PURE__ */ new Map();
  /** Return the canonical node, creating its store on first access. */
  async get(nodeId) {
    const validation = validateName(nodeId);
    if (!isOk(validation)) return validation;
    const existing = this.nodes.get(nodeId);
    if (existing !== void 0) return existing;
    try {
      const store = await this.factory(nodeId);
      if (!isOk(store)) return store;
      const node = AsyncNode.fromStore(store, { nodeMap: this });
      if (!isOk(node)) return node;
      const raced = this.nodes.get(nodeId);
      if (raced !== void 0) return raced;
      this.nodes.set(nodeId, node);
      return node;
    } catch (error) {
      return statusFromUnknown(error, "Chunk-store factory raised an exception.");
    }
  }
  /** Return an existing node without invoking the store factory. */
  getIfExists(nodeId) {
    const validation = validateName(nodeId);
    return isOk(validation) ? this.nodes.get(nodeId) ?? null : validation;
  }
  /** Remove a node, optionally only if it is the expected instance. */
  discard(nodeId, expected) {
    const validation = validateName(nodeId);
    if (!isOk(validation)) return validation;
    const found = this.nodes.get(nodeId);
    if (found === void 0 || expected !== void 0 && found !== expected) return null;
    this.nodes.delete(nodeId);
    return found;
  }
  contains(nodeId) {
    return this.nodes.has(nodeId);
  }
  get size() {
    return this.nodes.size;
  }
  entries() {
    return this.nodes.entries();
  }
};

// src/wire_stream.ts
var ABORT_STATUS_HEADER = "x-a11-abort-status";
var MAX_SINGLE_WIRE_MESSAGE_SIZE = 32 * 1024 * 1024;
function wireDeadlineMillis(value) {
  try {
    if (value === void 0 || value === null) return null;
    const result = value instanceof Date ? value.getTime() : value;
    if (!Number.isFinite(result)) {
      return invalidArgumentError("deadline must be a finite Date, number, or null.");
    }
    return result;
  } catch (error) {
    return invalidArgumentError(
      "deadline must be a readable Date, number, or null.",
      [],
      error
    );
  }
}
function normalizeWireStreamOptions(options = {}) {
  try {
    return normalizeWireStreamOptionsUnchecked(options);
  } catch (error) {
    return invalidArgumentError(
      "WireStream options could not be read.",
      [],
      error
    );
  }
}
function normalizeWireStreamOptionsUnchecked(options) {
  const empty = new WireMessage().toMsgpack();
  if (!isOk(empty)) return empty;
  const deadline = wireDeadlineMillis(options.deadline);
  if (!isOk(deadline)) return deadline;
  const result = {
    maxBufferedIncomingMessages: options.maxBufferedIncomingMessages ?? 100,
    maxSingleMessageSize: options.maxSingleMessageSize ?? MAX_SINGLE_WIRE_MESSAGE_SIZE,
    maxBufferedIncomingBytes: options.maxBufferedIncomingBytes ?? 32 * 1024 * 1024,
    messageTimeoutMs: options.messageTimeoutMs ?? null,
    deadline
  };
  if (!Number.isSafeInteger(result.maxBufferedIncomingMessages) || result.maxBufferedIncomingMessages < 1 || result.maxBufferedIncomingMessages > 1024) {
    return invalidArgumentError(
      "maxBufferedIncomingMessages must be an integer in [1, 1024]."
    );
  }
  if (!Number.isSafeInteger(result.maxSingleMessageSize) || result.maxSingleMessageSize < empty.byteLength || result.maxSingleMessageSize > MAX_SINGLE_WIRE_MESSAGE_SIZE) {
    return invalidArgumentError(
      "maxSingleMessageSize is outside the supported range."
    );
  }
  if (!Number.isSafeInteger(result.maxBufferedIncomingBytes) || result.maxBufferedIncomingBytes < empty.byteLength) {
    return invalidArgumentError(
      "maxBufferedIncomingBytes is smaller than an empty message."
    );
  }
  if (result.messageTimeoutMs !== null && (!Number.isFinite(result.messageTimeoutMs) || result.messageTimeoutMs < 0)) {
    return invalidArgumentError(
      "messageTimeoutMs must be a non-negative finite number or null."
    );
  }
  return result;
}
function normalizeWireHeaders(headers = void 0) {
  try {
    const values = normalizeByteMap(headers);
    if (!isOk(values)) return values;
    const result = /* @__PURE__ */ new Map();
    for (const [key, value] of values) {
      if (typeof key !== "string") {
        return invalidArgumentError("Wire header names must be strings.");
      }
      const folded = key.toLowerCase();
      const valid = validateName(folded);
      if (!isOk(valid)) return valid;
      result.set(folded, value);
    }
    return result;
  } catch (error) {
    return statusFromUnknown(error, "Normalizing WireStream headers raised.");
  }
}
function returnedStatus(value, operation) {
  return isStatus(value) ? value : internalError(`${operation} returned a non-Status value.`);
}
async function invokeWireCallback(callback, message) {
  if (callback === void 0) return okStatus();
  if (typeof callback !== "function") {
    return invalidArgumentError("WireStream callback must be callable.");
  }
  try {
    const result = message === void 0 ? await callback() : await callback(message);
    return result === void 0 ? okStatus() : returnedStatus(result, "WireStream callback");
  } catch (error) {
    return statusFromUnknown(error, "WireStream callback raised an exception.");
  }
}

// src/action.ts
function firstError(first, next) {
  if (!isStatus(next)) {
    return isOk(first) ? internalError("A Status-returning Action operation returned an invalid value.") : first;
  }
  return isOk(first) && !isOk(next) ? next : first;
}
function hasRegistryShape(value) {
  if (typeof value !== "object" || value === null) return false;
  try {
    const candidate = value;
    return typeof candidate.getSchema === "function" && typeof candidate.getHandler === "function";
  } catch {
    return false;
  }
}
function hasSessionShape(value) {
  if (typeof value !== "object" || value === null) return false;
  try {
    const candidate = value;
    return typeof candidate.getNodeMap === "function" && typeof candidate.getActionRegistry === "function" && typeof candidate.send === "function" && typeof candidate.trackAction === "function" && typeof candidate.untrackAction === "function" && (candidate.acquireActionSlot === void 0 || typeof candidate.acquireActionSlot === "function") && (candidate.releaseActionSlot === void 0 || typeof candidate.releaseActionSlot === "function");
  } catch {
    return false;
  }
}
function validateActionSettings(settings) {
  try {
    if (typeof settings !== "object" || settings === null || Array.isArray(settings)) {
      return invalidArgumentError("settings must be an object.");
    }
    const candidate = settings;
    for (const name of [
      "bindStreamsOnInputsByDefault",
      "bindStreamsOnOutputsByDefault",
      "clearInputsAfterRun",
      "clearOutputsAfterRun"
    ]) {
      if (candidate[name] !== void 0 && typeof candidate[name] !== "boolean") {
        return invalidArgumentError(`Action setting ${name} must be boolean.`);
      }
    }
    return okStatus();
  } catch (error) {
    return invalidArgumentError("Action settings could not be read.", [], error);
  }
}
var Action = class _Action {
  schema;
  id;
  handler;
  nodeMap;
  stream;
  session;
  registry;
  settings;
  headers = /* @__PURE__ */ new Map();
  inputIds = /* @__PURE__ */ new Map();
  outputIds = /* @__PURE__ */ new Map();
  inputNodes = /* @__PURE__ */ new Set();
  outputNodes = /* @__PURE__ */ new Set();
  boundNodes = /* @__PURE__ */ new Set();
  mode = "none";
  completionStatus = null;
  dispatchStatus = null;
  cancelRequested = false;
  finishing = false;
  inputAutofillsApplied = false;
  tracked = false;
  done = new Deferred();
  dispatched = new Deferred();
  cancelController = new AbortController();
  cancelCallbacks = [];
  parent = null;
  constructor(schema, options, id) {
    this.schema = schema;
    this.id = id;
    this.handler = options.handler ?? null;
    this.session = options.session ?? null;
    this.nodeMap = options.nodeMap ?? this.session?.getNodeMap() ?? new NodeMap();
    this.stream = options.stream ?? null;
    this.registry = options.registry ?? this.session?.getActionRegistry() ?? null;
    this.settings = { ...options.settings ?? {} };
    for (const [name, header] of schema.headers) {
      if (header.defaultValue !== null) {
        this.headers.set(name.toLowerCase(), new Uint8Array(header.defaultValue));
      }
    }
    this.remapDefaultPorts();
  }
  /** Validate a schema/options bundle and create a configurable action. */
  static create(schema, options = {}) {
    try {
      if (!(schema instanceof ActionSchema)) {
        return invalidArgumentError("schema must be an ActionSchema.");
      }
      if (typeof options !== "object" || options === null) {
        return invalidArgumentError("Action options must be an object.");
      }
      const validation = schema.validate();
      if (!isOk(validation)) return validation;
      if (options.handler !== void 0 && options.handler !== null && typeof options.handler !== "function") {
        return invalidArgumentError("handler must be callable or null.");
      }
      if (options.nodeMap !== void 0 && !(options.nodeMap instanceof NodeMap)) {
        return invalidArgumentError("nodeMap must be a NodeMap.");
      }
      const settings = validateActionSettings(options.settings ?? {});
      if (!isOk(settings)) return settings;
      const id = options.id || randomId("action-");
      const validId = validateName(id);
      if (!isOk(validId)) return validId;
      const action = new _Action(schema, options, id);
      const remapped = action.remapDefaultPorts();
      return isOk(remapped) ? action : remapped;
    } catch (error) {
      return statusFromUnknown(error, "Creating Action raised an exception.");
    }
  }
  /** Derive the stable `action-id#port-name` id for one action port node. */
  static makeNodeId(actionId, nodeName) {
    const validAction = validateName(actionId);
    if (!isOk(validAction)) return validAction;
    const validNode = validateName(nodeName);
    if (!isOk(validNode)) return validNode;
    const result = `${actionId}#${nodeName}`;
    const validResult = validateName(result);
    return isOk(validResult) ? result : validResult;
  }
  /** AbortSignal a handler can observe for cooperative cancellation. */
  get signal() {
    return this.cancelController.signal;
  }
  getId() {
    return this.id;
  }
  getSchema() {
    return this.schema;
  }
  getHandler() {
    return this.handler;
  }
  hasHandler() {
    return this.handler !== null;
  }
  getSettings() {
    return { ...this.settings };
  }
  getNodeMap() {
    return this.nodeMap;
  }
  getStream() {
    return this.stream;
  }
  getRegistry() {
    return this.registry;
  }
  getSession() {
    return this.session;
  }
  /** Completion status; OK is provisional until `isDone()` becomes true. */
  getStatus() {
    return this.completionStatus ?? okStatus();
  }
  /** Remote acknowledgement status, or `null` before it arrives. */
  getDispatchStatus() {
    return this.dispatchStatus;
  }
  isDone() {
    return this.completionStatus !== null;
  }
  hasBeenRun() {
    return this.mode === "run";
  }
  hasBeenCalled() {
    return this.mode === "call";
  }
  isCancelled() {
    return this.cancelRequested || this.completionStatus?.code === cancelledError().code;
  }
  /** Replace the call id and remap default port ids before starting. */
  setId(id) {
    const validation = validateName(id);
    if (!isOk(validation)) return validation;
    if (this.mode !== "none") {
      return failedPreconditionError("Cannot change Action id after it has started.");
    }
    const previous = this.id;
    this.id = id;
    const remapped = this.remapDefaultPorts();
    if (!isOk(remapped)) {
      this.id = previous;
      this.remapDefaultPorts();
    }
    return remapped;
  }
  /** Replace the callable contract and remap ports before starting. */
  setSchema(schema) {
    if (!(schema instanceof ActionSchema)) return invalidArgumentError("schema must be an ActionSchema.");
    const validation = schema.validate();
    if (!isOk(validation)) return validation;
    if (this.mode !== "none") {
      return failedPreconditionError("Cannot change Action schema after it has started.");
    }
    this.schema = schema;
    return this.remapDefaultPorts();
  }
  /** Bind the application implementation invoked by a local run. */
  bindHandler(handler) {
    if (typeof handler !== "function") return invalidArgumentError("handler must be callable.");
    if (this.mode !== "none") {
      return failedPreconditionError("Cannot change Action handler after it has started.");
    }
    this.handler = handler;
    return okStatus();
  }
  setSettings(settings) {
    const validation = validateActionSettings(settings);
    if (!isOk(validation)) return validation;
    try {
      this.settings = { ...settings };
      return okStatus();
    } catch (error) {
      return invalidArgumentError("Action settings could not be copied.", [], error);
    }
  }
  bindStreamsOnInputsByDefault(bind) {
    if (typeof bind !== "boolean") return invalidArgumentError("bind must be boolean.");
    this.settings.bindStreamsOnInputsByDefault = bind;
    return okStatus();
  }
  bindStreamsOnOutputsByDefault(bind) {
    if (typeof bind !== "boolean") return invalidArgumentError("bind must be boolean.");
    this.settings.bindStreamsOnOutputsByDefault = bind;
    return okStatus();
  }
  clearInputsAfterRun(clear = true) {
    if (typeof clear !== "boolean") return invalidArgumentError("clear must be boolean.");
    this.settings.clearInputsAfterRun = clear;
    return okStatus();
  }
  clearOutputsAfterRun(clear = true) {
    if (typeof clear !== "boolean") return invalidArgumentError("clear must be boolean.");
    this.settings.clearOutputsAfterRun = clear;
    return okStatus();
  }
  bindNodeMap(nodeMap) {
    if (!(nodeMap instanceof NodeMap)) return invalidArgumentError("nodeMap must be a NodeMap.");
    this.nodeMap = nodeMap;
    return okStatus();
  }
  bindRegistry(registry) {
    if (registry !== null && !hasRegistryShape(registry)) {
      return invalidArgumentError("registry must implement ActionRegistryLike or be null.");
    }
    this.registry = registry;
    return okStatus();
  }
  /** Move lifetime/routing ownership to a session, retaining active tracking. */
  bindSession(session) {
    if (session !== null && !hasSessionShape(session)) {
      return invalidArgumentError("session must implement ActionSessionContext or be null.");
    }
    if (!this.tracked || this.session === session) {
      this.session = session;
      return okStatus();
    }
    const previous = this.session;
    if (session !== null) {
      let tracked;
      try {
        tracked = session.trackAction(this);
      } catch (error) {
        return statusFromUnknown(error, "Tracking Action in Session raised an exception.");
      }
      if (!isStatus(tracked)) {
        return internalError("Session.trackAction() returned a non-Status value.");
      }
      if (!isOk(tracked)) return tracked;
    }
    try {
      previous?.untrackAction(this);
    } catch (error) {
      if (session !== null) {
        try {
          session.untrackAction(this);
        } catch {
        }
      }
      return statusFromUnknown(error, "Removing Action from Session raised an exception.");
    }
    this.session = session;
    this.tracked = session !== null;
    return okStatus();
  }
  /** Bind remote dispatch and reattach all already stream-bound port nodes. */
  bindStream(stream) {
    if (stream === this.stream) return okStatus();
    const previous = this.stream;
    const rebound = [];
    for (const node of this.boundNodes) {
      let status = previous === null ? okStatus() : node.detachStream(previous);
      if (isOk(status) && stream !== null) status = node.attachStream(stream);
      if (!isOk(status)) {
        for (const completed of rebound) {
          if (stream !== null) completed.detachStream(stream);
          if (previous !== null) completed.attachStream(previous);
        }
        return status;
      }
      rebound.push(node);
    }
    this.stream = stream;
    return okStatus();
  }
  async getNode(nodeId) {
    return this.nodeMap.get(nodeId);
  }
  /** Open a named input node and optionally mirror local writes to the peer. */
  async getInput(name, bindStream) {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    const id = this.inputIds.get(name);
    if (id === void 0) return notFoundError(`Action input '${name}' is not mapped.`);
    const node = await this.nodeMap.get(id);
    if (!isOk(node)) return node;
    this.inputNodes.add(node);
    const bind = bindStream ?? this.settings.bindStreamsOnInputsByDefault ?? this.mode !== "run";
    const attached = this.attachStreamIfRequested(node, bind);
    return isOk(attached) ? node : attached;
  }
  /** Open a named output node and optionally mirror local writes to the peer. */
  async getOutput(name, bindStream) {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    const id = this.outputIds.get(name);
    if (id === void 0) return notFoundError(`Action output '${name}' is not mapped.`);
    const node = await this.nodeMap.get(id);
    if (!isOk(node)) return node;
    this.outputNodes.add(node);
    const bind = bindStream ?? this.settings.bindStreamsOnOutputsByDefault ?? this.mode === "run";
    const attached = this.attachStreamIfRequested(
      node,
      bind && name !== ACTION_STATUS_OUTPUT && name !== ACTION_DISPATCH_STATUS_OUTPUT
    );
    return isOk(attached) ? node : attached;
  }
  async getPort(name) {
    const input = this.inputIds.has(name);
    const output = this.outputIds.has(name);
    if (input && output) {
      return failedPreconditionError(
        "Action port is both an input and output; select one explicitly."
      );
    }
    if (input) return this.getInput(name);
    if (output) return this.getOutput(name);
    return notFoundError("Action port is not mapped.");
  }
  containsPort(name) {
    return this.inputIds.has(name) || this.outputIds.has(name);
  }
  /** Snapshot the call id, name, port mappings, and headers for dispatch. */
  getActionMessage() {
    return new ActionMessage({
      id: this.id,
      name: this.schema.name,
      inputs: [...this.schema.inputs.keys()].map(
        (name) => new Port(name, this.inputIds.get(name) ?? "")
      ),
      outputs: [...this.schema.outputs.keys()].map(
        (name) => new Port(name, this.outputIds.get(name) ?? "")
      ),
      headers: copyByteMap(this.headers)
    });
  }
  /** Adopt validated caller-supplied port node ids before local execution. */
  mapPortsFromMessage(message) {
    if (!(message instanceof ActionMessage)) return invalidArgumentError("message must be an ActionMessage.");
    if (this.mode !== "none") {
      return failedPreconditionError("Cannot remap Action ports after it has started.");
    }
    const validation = message.validate();
    if (!isOk(validation)) return validation;
    const inputStatus = this.validateMessagePorts(message.inputs, this.schema.inputs, "input");
    if (!isOk(inputStatus)) return inputStatus;
    const outputStatus = this.validateMessagePorts(message.outputs, this.schema.outputs, "output");
    if (!isOk(outputStatus)) return outputStatus;
    for (const port of message.inputs) this.inputIds.set(port.name, port.id);
    for (const port of message.outputs) this.outputIds.set(port.name, port.id);
    return okStatus();
  }
  getHeaders() {
    return copyByteMap(this.headers);
  }
  getHeader(name) {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    const value = this.headers.get(name.toLowerCase());
    return value === void 0 ? null : new Uint8Array(value);
  }
  hasHeader(name) {
    return isOk(validateName(name)) && this.headers.has(name.toLowerCase());
  }
  setHeader(name, value) {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    const normalized = normalizeByteMap(/* @__PURE__ */ new Map([[name, value]]));
    if (!isOk(normalized)) return normalized;
    this.headers.set(name.toLowerCase(), normalized.get(name));
    return okStatus();
  }
  removeHeader(name) {
    const valid = validateName(name);
    if (!isOk(valid)) return valid;
    this.headers.delete(name.toLowerCase());
    return okStatus();
  }
  forwardHeader(target, name) {
    if (!(target instanceof _Action)) return invalidArgumentError("target must be an Action.");
    const value = this.getHeader(name);
    if (!isOk(value)) return value;
    return value === null ? okStatus() : target.setHeader(name, value);
  }
  /** Copy framework-scoped metadata to a nested action. */
  forwardHeadersWithPrefix(target, prefix = ACTION_HEADER_PREFIX) {
    if (!(target instanceof _Action)) return invalidArgumentError("target must be an Action.");
    const folded = prefix.toLowerCase();
    for (const [name, value] of this.headers) {
      if (name.startsWith(folded)) {
        const status = target.setHeader(name, value);
        if (!isOk(status)) return status;
      }
    }
    return okStatus();
  }
  /**
   * Create a child action from a schema or registered name.
   *
   * With `propagateIo`, the child shares the parent's node map, stream, and
   * session, while retaining its own id, derived port ids, and AbortSignal.
   * The registry is shared in either mode, and framework headers are forwarded
   * when `forwardHeaders` is true. A shared session supplies nested concurrency
   * limits and session-wide abort; cancelling only the parent is not recursive.
   */
  makeNested(schemaOrName, propagateIo = true, forwardHeaders = true) {
    try {
      let schema;
      let handler = null;
      if (typeof schemaOrName === "string") {
        if (this.registry === null) {
          return failedPreconditionError("Cannot resolve a nested Action without a registry.");
        }
        const found = this.registry.getSchema(schemaOrName);
        if (isStatus(found) && !isOk(found)) return found;
        if (!(found instanceof ActionSchema)) {
          return internalError("Action registry returned an invalid schema.");
        }
        schema = found;
        const foundHandler = this.registry.getHandler(schemaOrName);
        if (isStatus(foundHandler) && !isOk(foundHandler)) {
          handler = null;
        } else if (typeof foundHandler === "function") {
          handler = foundHandler;
        } else {
          return internalError("Action registry returned an invalid handler.");
        }
      } else {
        schema = schemaOrName;
      }
      if (typeof propagateIo !== "boolean" || typeof forwardHeaders !== "boolean") {
        return invalidArgumentError("Nested Action propagation options must be boolean.");
      }
      const child = _Action.create(schema, {
        handler,
        nodeMap: propagateIo ? this.nodeMap : void 0,
        stream: propagateIo ? this.stream : null,
        session: propagateIo ? this.session : null,
        registry: this.registry
      });
      if (!isOk(child)) return child;
      child.parent = this;
      if (forwardHeaders) {
        const forwarded = this.forwardHeadersWithPrefix(child);
        if (!isOk(forwarded)) return forwarded;
      }
      return child;
    } catch (error) {
      return statusFromUnknown(error, "Creating nested Action raised an exception.");
    }
  }
  /** Start the bound handler locally and return immediately. */
  run() {
    if (this.handler === null) {
      return failedPreconditionError("Action handler has not been set.");
    }
    const begun = this.begin("run");
    if (!isOk(begun)) return begun;
    const tracked = this.trackInSession();
    if (!isOk(tracked)) {
      this.mode = "none";
      return tracked;
    }
    queueMicrotask(() => void this.runHandler());
    return this;
  }
  /** Queue this action for remote dispatch; use `waitForDispatch` for acceptance. */
  async call(wireHeaders = /* @__PURE__ */ new Map()) {
    try {
      return await this.callInternal(wireHeaders);
    } catch (error) {
      this.untrackFromSession();
      if (this.mode === "call" && this.completionStatus === null) this.mode = "none";
      return statusFromUnknown(error, "Calling Action raised an exception.");
    }
  }
  async callInternal(wireHeaders) {
    const headers = normalizeWireHeaders(wireHeaders);
    if (!isOk(headers)) return headers;
    const begun = this.begin("call");
    if (!isOk(begun)) return begun;
    const tracked = this.trackInSession();
    if (!isOk(tracked)) {
      this.mode = "none";
      return tracked;
    }
    const autofills = this.collectAutofillFragments();
    if (!isOk(autofills)) {
      this.untrackFromSession();
      this.mode = "none";
      return autofills;
    }
    const message = new WireMessage({
      nodeFragments: autofills,
      actions: [this.getActionMessage()],
      headers
    });
    let sent;
    try {
      if (this.stream !== null) sent = this.stream.send(message);
      else if (this.session !== null) sent = this.session.send(message);
      else sent = failedPreconditionError(
        "Calling an Action requires an attached WireStream or Session."
      );
    } catch (error) {
      sent = statusFromUnknown(error, "Sending Action call raised an exception.");
    }
    if (!isStatus(sent)) {
      sent = internalError("Action transport send() returned a non-Status value.");
    }
    if (!isOk(sent)) {
      this.untrackFromSession();
      this.mode = "none";
      return sent;
    }
    return this;
  }
  /** Await the remote dispatch acknowledgement, not handler completion. */
  async waitForDispatch(timeoutMs) {
    if (this.mode !== "call") {
      return failedPreconditionError("Only a called Action has a dispatch status.");
    }
    return this.waitForStatus(this.dispatched.promise, timeoutMs, "Action dispatch timed out.");
  }
  /** Await terminal local/remote completion after all lifecycle cleanup. */
  async wait(timeoutMs) {
    if (this.mode === "none") return failedPreconditionError("Action has not been run or called.");
    const status = await this.waitForStatus(this.done.promise, timeoutMs, "Action wait timed out.");
    return isOk(status) ? this : status;
  }
  /**
   * Request cooperative cancellation once.
   *
   * Local handlers observe {@link signal}; remote calls also send the reserved
   * cancellation action. `cancel()` initiates the transition—await `wait()` if
   * teardown and output abort propagation must be complete.
   */
  cancel() {
    if (this.completionStatus !== null || this.finishing || this.cancelRequested) {
      return okStatus();
    }
    this.cancelRequested = true;
    this.cancelController.abort();
    let first = okStatus();
    for (const callback of this.cancelCallbacks) {
      try {
        const result = callback(this);
        if (result !== void 0) first = firstError(first, result);
      } catch (error) {
        first = firstError(first, statusFromUnknown(error, "Action cancel callback raised."));
      }
    }
    const cancelled = cancelledError("Action was cancelled.");
    if (this.mode === "call") {
      first = firstError(first, this.sendRemoteCancel());
      this.completeCall(cancelled, false);
      queueMicrotask(() => void this.abortLocalCallOutputs(cancelled));
    } else if (this.mode === "run") {
      queueMicrotask(() => void this.finishRun(cancelled));
    } else if (this.mode === "none") {
      this.mode = "cancelled";
      this.completionStatus = cancelled;
      this.done.resolve(cancelled);
    }
    return first;
  }
  setOnCancelled(callback) {
    if (typeof callback !== "function") return invalidArgumentError("callback must be callable.");
    this.cancelCallbacks.push(callback);
    return okStatus();
  }
  /** Session protocol hook for the reserved dispatch-status node. */
  setDispatchStatus(status) {
    if (!isStatus(status)) return invalidArgumentError("status must be an A11 Status.");
    if (this.mode !== "call" || this.dispatchStatus !== null) return okStatus();
    this.dispatchStatus = status;
    this.dispatched.resolve(status);
    return okStatus();
  }
  /** Session protocol hook for the reserved completion-status node. */
  setCompletionStatus(status) {
    if (!isStatus(status)) return invalidArgumentError("status must be an A11 Status.");
    if (this.mode !== "call") return failedPreconditionError("Action is not a call.");
    if (this.dispatchStatus === null) {
      this.dispatchStatus = okStatus();
      this.dispatched.resolve(this.dispatchStatus);
    }
    if (this.completionStatus === null) this.completeCall(status, true);
    else if (this.cancelRequested) this.untrackFromSession();
    return okStatus();
  }
  /** Materialize schema autofills into empty input nodes before a handler runs. */
  async applyInputAutofills() {
    try {
      return await this.applyInputAutofillsInternal();
    } catch (error) {
      return statusFromUnknown(error, "Applying Action input autofills raised an exception.");
    }
  }
  async applyInputAutofillsInternal() {
    if (this.inputAutofillsApplied) return okStatus();
    const work = [];
    for (const [name, port] of this.schema.inputs) {
      if (port.autofills.length > 0) {
        const id = this.inputIds.get(name);
        if (id !== void 0) work.push([id, port.autofills]);
      }
    }
    const nodes = [];
    for (const [id] of work) {
      const node = await this.nodeMap.get(id);
      if (!isOk(node)) return node;
      const writable = await node.isWritable();
      if (!isOk(writable)) return writable;
      if (!writable) return failedPreconditionError(`Autofilled input '${id}' is not writable.`);
      const size = await node.chunkStore.size();
      if (!isOk(size)) return size;
      if (size !== 0) return failedPreconditionError(`Autofilled input '${id}' already contains data.`);
      this.inputNodes.add(node);
      nodes.push(node);
    }
    for (let index = 0; index < work.length; ++index) {
      const [id, autofills] = work[index];
      const node = nodes[index];
      for (const autofill of autofills) {
        const stored = autofill === null ? await node.putNullFinal() : await node.putFragment(new NodeFragment({
          id,
          data: autofill.data,
          seq: autofill.seq,
          continued: autofill.continued
        }));
        if (!isOk(stored)) return stored;
      }
      const closed = await node.drainAndClose();
      if (!isOk(closed)) return closed;
    }
    this.inputAutofillsApplied = true;
    return okStatus();
  }
  begin(mode) {
    if (this.cancelRequested) return cancelledError("Action was cancelled.");
    if (this.mode !== "none") return failedPreconditionError("Action has already started.");
    this.mode = mode;
    return okStatus();
  }
  remapDefaultPorts() {
    this.inputIds.clear();
    this.outputIds.clear();
    for (const name of this.schema.inputs.keys()) {
      const id = _Action.makeNodeId(this.id, name);
      if (!isOk(id)) return id;
      this.inputIds.set(name, id);
    }
    for (const name of this.schema.outputs.keys()) {
      const id = _Action.makeNodeId(this.id, name);
      if (!isOk(id)) return id;
      this.outputIds.set(name, id);
    }
    for (const name of [ACTION_STATUS_OUTPUT, ACTION_DISPATCH_STATUS_OUTPUT]) {
      const id = _Action.makeNodeId(this.id, name);
      if (!isOk(id)) return id;
      this.outputIds.set(name, id);
    }
    return okStatus();
  }
  attachStreamIfRequested(node, bind) {
    if (!bind || this.stream === null) return okStatus();
    const attached = node.attachStream(this.stream);
    if (isOk(attached)) this.boundNodes.add(node);
    return attached;
  }
  validateMessagePorts(ports, schemaPorts, kind) {
    const seen = /* @__PURE__ */ new Set();
    for (const port of ports) {
      const valid = port.validate();
      if (!isOk(valid)) return valid;
      if (!schemaPorts.has(port.name)) {
        return failedPreconditionError(`Unknown Action ${kind} port '${port.name}'.`);
      }
      if (seen.has(port.name)) {
        return invalidArgumentError(`Action ${kind} port '${port.name}' is duplicated.`);
      }
      seen.add(port.name);
    }
    return okStatus();
  }
  collectAutofillFragments() {
    const fragments = [];
    for (const [name, port] of this.schema.inputs) {
      if (port.autofills.length === 0) continue;
      const id = this.inputIds.get(name);
      if (id === void 0) continue;
      const start = fragments.length;
      for (const autofill of port.autofills) {
        fragments.push(new NodeFragment({
          id,
          data: autofill?.data ?? makeNullChunk(),
          seq: autofill?.seq ?? null,
          continued: autofill?.continued ?? false
        }));
      }
      if (fragments.length > start) fragments[fragments.length - 1].continued = false;
    }
    return fragments;
  }
  async runHandler() {
    let status = okStatus();
    const nested = this.parent !== null;
    let acquired = false;
    try {
      if (this.session?.acquireActionSlot !== void 0) {
        const acquiredStatus = await this.session.acquireActionSlot(
          nested,
          this.signal
        );
        status = isStatus(acquiredStatus) ? acquiredStatus : internalError(
          "Session.acquireActionSlot() returned a non-Status value."
        );
        acquired = isOk(status);
      }
      if (isOk(status)) {
        if (this.cancelRequested) {
          status = cancelledError("Action was cancelled.");
        } else {
          status = await this.applyInputAutofills();
          if (isOk(status)) {
            try {
              const result = await this.handler(this);
              if (result === void 0) status = okStatus();
              else if (isStatus(result)) status = result;
              else status = internalError(
                "Action handler returned a value that is not a Status."
              );
            } catch (error) {
              status = statusFromUnknown(
                error,
                "Action handler raised an exception."
              );
            }
          }
        }
      }
    } catch (error) {
      status = statusFromUnknown(error, "Running Action handler raised an exception.");
    } finally {
      if (acquired) {
        try {
          this.session?.releaseActionSlot?.(nested);
        } catch (error) {
          status = firstError(
            status,
            statusFromUnknown(error, "Releasing Session Action slot raised.")
          );
        }
      }
    }
    if (this.cancelRequested) status = cancelledError("Action was cancelled.");
    await this.finishRun(status);
  }
  async finishRun(initialStatus) {
    if (this.finishing || this.completionStatus !== null) return okStatus();
    this.finishing = true;
    try {
      let finalStatus = initialStatus;
      const outputStatus = await this.finishOutputNodes(finalStatus);
      if (isOk(finalStatus) && !isOk(outputStatus)) finalStatus = outputStatus;
      let communicated = await this.communicateStatus(finalStatus);
      if (isOk(finalStatus) && !isOk(communicated)) {
        finalStatus = communicated;
        await this.finishOutputNodes(finalStatus);
        communicated = await this.communicateStatus(finalStatus);
      }
      await this.releaseNodesAfterRun();
      this.completionStatus = finalStatus;
      this.done.resolve(finalStatus);
      this.untrackFromSession();
      return communicated;
    } catch (error) {
      const failure = firstError(
        initialStatus,
        statusFromUnknown(error, "Finishing Action run raised an exception.")
      );
      this.completionStatus = failure;
      this.done.resolve(failure);
      this.untrackFromSession();
      return failure;
    }
  }
  async finishOutputNodes(status) {
    const ids = [...this.schema.outputs.keys()].map((name) => this.outputIds.get(name)).filter((id) => id !== void 0);
    let first = okStatus();
    if (!isOk(status) && this.stream !== null && ids.length > 0) {
      const chunk = statusToChunk(status);
      if (isOk(chunk)) {
        try {
          const sent = this.stream.send(new WireMessage({
            nodeFragments: ids.map((id) => new NodeFragment({
              id,
              data: chunk,
              seq: 0,
              continued: false
            }))
          }));
          first = firstError(
            first,
            isStatus(sent) ? sent : internalError("WireStream.send() returned a non-Status value.")
          );
        } catch (error) {
          first = firstError(
            first,
            statusFromUnknown(error, "Sending Action output status raised.")
          );
        }
      } else first = firstError(first, chunk);
    }
    for (const id of ids) {
      const node = await this.nodeMap.get(id);
      if (!isOk(node)) {
        first = firstError(first, node);
        continue;
      }
      this.outputNodes.add(node);
      const writable = await node.isWritable();
      if (!isOk(writable)) {
        first = firstError(first, writable);
        continue;
      }
      if (!writable) continue;
      const closed = isOk(status) ? await node.drainAndClose() : await node.abortWithStatus(status);
      first = firstError(first, closed);
    }
    return first;
  }
  async communicateStatus(status) {
    const chunk = statusToChunk(status);
    if (!isOk(chunk)) return chunk;
    const id = this.outputIds.get(ACTION_STATUS_OUTPUT);
    if (id === void 0) return internalError("Action status output is not mapped.");
    const node = await this.nodeMap.get(id);
    if (!isOk(node)) return node;
    const writable = await node.isWritable();
    if (!isOk(writable)) return writable;
    if (!writable) return failedPreconditionError("Action status node was already finalized.");
    if (this.stream !== null) {
      const attached = node.attachStream(this.stream);
      if (!isOk(attached)) return attached;
      this.boundNodes.add(node);
    }
    const stored = await node.putFragment(new NodeFragment({
      id,
      data: chunk,
      seq: 0,
      continued: false
    }));
    if (!isOk(stored)) return stored;
    return node.drainAndClose();
  }
  async releaseNodesAfterRun() {
    let first = this.detachBoundNodes();
    if (this.settings.clearInputsAfterRun) {
      for (const id of this.inputIds.values()) {
        const removed = this.nodeMap.discard(id);
        if (!isOk(removed)) first = firstError(first, removed);
        else removed?.cancelReader();
      }
    }
    if (this.settings.clearOutputsAfterRun) {
      for (const id of this.outputIds.values()) {
        const removed = this.nodeMap.discard(id);
        if (!isOk(removed)) first = firstError(first, removed);
      }
    }
    this.inputNodes.clear();
    this.outputNodes.clear();
    return first;
  }
  detachBoundNodes() {
    let first = okStatus();
    if (this.stream !== null) {
      for (const node of this.boundNodes) {
        first = firstError(first, node.detachStream(this.stream));
      }
    }
    this.boundNodes.clear();
    return first;
  }
  trackInSession() {
    if (this.session === null || this.tracked) return okStatus();
    let status;
    try {
      status = this.session.trackAction(this);
    } catch (error) {
      return statusFromUnknown(error, "Tracking Action in Session raised an exception.");
    }
    if (!isStatus(status)) {
      return internalError("Session.trackAction() returned a non-Status value.");
    }
    if (isOk(status)) this.tracked = true;
    return status;
  }
  untrackFromSession() {
    if (!this.tracked) return;
    this.tracked = false;
    try {
      this.session?.untrackAction(this);
    } catch {
    }
  }
  sendRemoteCancel() {
    try {
      const cancel = new ActionMessage({
        id: randomId("action-"),
        name: CANCEL_ACTION_NAME,
        headers: /* @__PURE__ */ new Map([[CANCEL_ACTION_HEADER, new TextEncoder().encode(this.id)]])
      });
      const message = new WireMessage({ actions: [cancel] });
      const sent = this.stream !== null ? this.stream.send(message) : this.session !== null ? this.session.send(message) : failedPreconditionError(
        "Cancelling a called Action requires a WireStream or Session."
      );
      return isStatus(sent) ? sent : internalError("Action transport send() returned a non-Status value.");
    } catch (error) {
      return statusFromUnknown(error, "Sending remote Action cancellation raised.");
    }
  }
  completeCall(status, removeFromSession) {
    if (this.completionStatus === null) {
      this.completionStatus = status;
      this.detachBoundNodes();
      this.done.resolve(status);
    }
    if (removeFromSession) this.untrackFromSession();
  }
  async abortLocalCallOutputs(status) {
    try {
      for (const name of this.schema.outputs.keys()) {
        const id = this.outputIds.get(name);
        if (id === void 0) continue;
        const node = await this.nodeMap.get(id);
        if (!isOk(node)) continue;
        const writable = await node.isWritable();
        if (isOk(writable) && writable) await node.abortWithStatus(status);
      }
    } catch {
    }
  }
  async waitForStatus(promise, timeoutMs, message) {
    if (timeoutMs !== void 0 && (!Number.isFinite(timeoutMs) || timeoutMs < 0)) {
      return invalidArgumentError("timeoutMs must be a non-negative finite number.");
    }
    if (timeoutMs === void 0) return promise;
    const timeout = new Deferred();
    const timer = setTimeout(() => timeout.resolve(deadlineExceededError(message)), timeoutMs);
    try {
      return await Promise.race([promise, timeout.promise]);
    } catch (error) {
      return statusFromUnknown(error, "Waiting for Action raised an exception.");
    } finally {
      clearTimeout(timer);
    }
  }
};

// src/action_registry.ts
function cloneSchema(schema, clearAutofills = false) {
  try {
    const inputs = /* @__PURE__ */ new Map();
    const outputs = /* @__PURE__ */ new Map();
    for (const [name, port] of schema.inputs) {
      inputs.set(name, new ActionPortSchema({
        name: port.name,
        type: port.type,
        description: port.description,
        required: port.required,
        unary: port.unary,
        autofills: clearAutofills ? [] : port.autofills.map(
          (fragment) => fragment === null ? null : cloneFragment(fragment)
        )
      }));
    }
    for (const [name, port] of schema.outputs) {
      outputs.set(name, new ActionPortSchema({
        name: port.name,
        type: port.type,
        description: port.description,
        required: port.required,
        unary: port.unary,
        autofills: clearAutofills ? [] : port.autofills.map(
          (fragment) => fragment === null ? null : cloneFragment(fragment)
        )
      }));
    }
    const headers = /* @__PURE__ */ new Map();
    for (const [name, header] of schema.headers) {
      headers.set(name, new ActionHeaderSchema({
        name: header.name,
        description: header.description,
        defaultValue: header.defaultValue === null ? null : new Uint8Array(header.defaultValue)
      }));
    }
    const copy = new ActionSchema({
      name: schema.name,
      description: schema.description,
      inputs,
      outputs,
      headers,
      outputToJsonField: new Map(schema.outputToJsonField)
    });
    const validation = copy.validate();
    return isOk(validation) ? copy : validation;
  } catch (error) {
    return statusFromUnknown(error, "Copying ActionSchema raised an exception.");
  }
}
var ActionRegistry = class _ActionRegistry {
  registrations = /* @__PURE__ */ new Map();
  /** Register or replace one named schema/handler pair. */
  register(actionName, schema, handler = null) {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      if (actionName === CANCEL_ACTION_NAME) {
        return invalidArgumentError("The cancel Action name is reserved.");
      }
      if (!(schema instanceof ActionSchema)) {
        return invalidArgumentError("schema must be an ActionSchema.");
      }
      if (handler !== null && typeof handler !== "function") {
        return invalidArgumentError("handler must be callable or null.");
      }
      const validation = schema.validate();
      if (!isOk(validation)) return validation;
      if (schema.name !== actionName) {
        return invalidArgumentError(
          "Registry Action name does not match schema name."
        );
      }
      const stored = cloneSchema(schema);
      if (!isOk(stored)) return stored;
      this.registrations.set(actionName, { schema: stored, handler });
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Registering Action raised an exception.");
    }
  }
  /** Remove an action so future calls are no longer dispatchable. */
  unregister(actionName) {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      if (!this.registrations.delete(actionName)) {
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Unregistering Action raised an exception.");
    }
  }
  isRegistered(actionName) {
    try {
      return isOk(validateName(actionName)) && this.registrations.has(actionName);
    } catch {
      return false;
    }
  }
  /** Return an isolated copy of a registered callable contract. */
  getSchema(actionName) {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      const registration = this.registrations.get(actionName);
      if (registration === void 0) {
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      return cloneSchema(registration.schema);
    } catch (error) {
      return statusFromUnknown(error, "Looking up Action schema raised an exception.");
    }
  }
  /** Return the local implementation, or NotFound for remote-only entries. */
  getHandler(actionName) {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      const registration = this.registrations.get(actionName);
      if (registration === void 0) {
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      return registration.handler ?? notFoundError(
        `Action '${actionName}' is registered without a handler.`
      );
    } catch (error) {
      return statusFromUnknown(error, "Looking up Action handler raised an exception.");
    }
  }
  /** Instantiate a configurable action bound to this registry. */
  makeAction(actionName, options = {}) {
    try {
      const validName = validateName(actionName);
      if (!isOk(validName)) return validName;
      const registration = this.registrations.get(actionName);
      if (registration === void 0) {
        return notFoundError(`Action '${actionName}' is not registered.`);
      }
      const schema = cloneSchema(registration.schema);
      if (!isOk(schema)) return schema;
      return Action.create(schema, {
        id: options.id,
        handler: registration.handler,
        nodeMap: options.nodeMap,
        stream: options.stream,
        session: options.session,
        registry: this
      });
    } catch (error) {
      return statusFromUnknown(error, "Creating registered Action raised an exception.");
    }
  }
  /** Build the initial wire description for a registered action call. */
  makeActionMessage(actionName, actionId = "") {
    const action = this.makeAction(actionName, {
      ...actionId === "" ? {} : { id: actionId }
    });
    return isOk(action) ? action.getActionMessage() : action;
  }
  /** Snapshot registered names in insertion order. */
  listRegisteredActions() {
    try {
      return [...this.registrations.keys()];
    } catch {
      return [];
    }
  }
  /**
   * Clone registrations, normally removing all input and output autofills.
   * Use the cleared copy before sharing a registry across an agent or trust
   * boundary so context-specific defaults do not cross implicitly.
   */
  copy(clearAutofills = true) {
    if (typeof clearAutofills !== "boolean") {
      return invalidArgumentError("clearAutofills must be boolean.");
    }
    const result = new _ActionRegistry();
    try {
      for (const [name, registration] of this.registrations) {
        const schema = cloneSchema(registration.schema, clearAutofills);
        if (!isOk(schema)) return schema;
        result.registrations.set(name, {
          schema,
          handler: registration.handler
        });
      }
      return result;
    } catch (error) {
      return statusFromUnknown(error, "Copying ActionRegistry raised an exception.");
    }
  }
};

// src/session.ts
var SESSION_STATUS_HEADER = "x-a11-session-status";
var MAX_SINGLE_MESSAGE_SIZE = MAX_SINGLE_WIRE_MESSAGE_SIZE;
var SESSION_STREAM_ABORT_MESSAGE = "Session has aborted its streams";
var ActionLimiter = class {
  constructor(maximum) {
    this.maximum = maximum;
  }
  active = 0;
  waiters = [];
  terminalStatus = null;
  acquire(signal) {
    if (this.terminalStatus !== null) return Promise.resolve(this.terminalStatus);
    if (signal?.aborted) {
      return Promise.resolve(cancelledError("Action was cancelled while waiting to run."));
    }
    if (this.active < this.maximum) {
      ++this.active;
      return Promise.resolve(okStatus());
    }
    const waiter = { deferred: new Deferred(), signal };
    if (signal !== void 0) {
      waiter.onAbort = () => {
        const index = this.waiters.indexOf(waiter);
        if (index < 0) return;
        this.waiters.splice(index, 1);
        waiter.deferred.resolve(
          cancelledError("Action was cancelled while waiting to run.")
        );
      };
      try {
        signal.addEventListener("abort", waiter.onAbort, { once: true });
      } catch {
      }
    }
    this.waiters.push(waiter);
    return waiter.deferred.promise;
  }
  release() {
    if (this.active > 0) --this.active;
    while (this.waiters.length > 0 && this.terminalStatus === null) {
      const waiter = this.waiters.shift();
      this.removeAbortListener(waiter);
      if (waiter.signal?.aborted) {
        waiter.deferred.resolve(
          cancelledError("Action was cancelled while waiting to run.")
        );
        continue;
      }
      ++this.active;
      waiter.deferred.resolve(okStatus());
      break;
    }
  }
  cancel(status) {
    if (this.terminalStatus !== null) return;
    this.terminalStatus = status;
    for (const waiter of this.waiters.splice(0)) {
      this.removeAbortListener(waiter);
      waiter.deferred.resolve(status);
    }
  }
  removeAbortListener(waiter) {
    if (waiter.signal === void 0 || waiter.onAbort === void 0) return;
    try {
      waiter.signal.removeEventListener("abort", waiter.onAbort);
    } catch {
    }
  }
};
function isPositiveIntegerInRange(value, maximum) {
  return Number.isSafeInteger(value) && value >= 1 && value <= maximum;
}
function normalizeSessionOptions(options = {}) {
  try {
    const empty = new WireMessage().toMsgpack();
    if (!isOk(empty)) return empty;
    const deadline = wireDeadlineMillis(options.deadline);
    if (!isOk(deadline)) return deadline;
    const result = {
      maxBufferedMessagesTotal: options.maxBufferedMessagesTotal ?? 256,
      maxBufferedMessagesPerStream: options.maxBufferedMessagesPerStream ?? 32,
      maxConcurrentRootActions: options.maxConcurrentRootActions ?? 32,
      maxConcurrentNestedActions: options.maxConcurrentNestedActions ?? 128,
      maxSingleMessageSize: options.maxSingleMessageSize ?? MAX_SINGLE_MESSAGE_SIZE,
      maxBufferedBytesTotal: options.maxBufferedBytesTotal ?? 32 * 1024 * 1024,
      maxBufferedBytesPerStream: options.maxBufferedBytesPerStream ?? 4 * 1024 * 1024,
      noStreamTimeoutMs: options.noStreamTimeoutMs === void 0 ? 3e4 : options.noStreamTimeoutMs,
      deadline
    };
    if (!isPositiveIntegerInRange(result.maxBufferedMessagesTotal, 1024) || !isPositiveIntegerInRange(result.maxBufferedMessagesPerStream, 1024)) {
      return invalidArgumentError(
        "Session message limits must be integers between 1 and 1024."
      );
    }
    if (!isPositiveIntegerInRange(result.maxConcurrentRootActions, 65536) || !isPositiveIntegerInRange(result.maxConcurrentNestedActions, 65536)) {
      return invalidArgumentError(
        "Session Action limits must be integers between 1 and 65536."
      );
    }
    if (!Number.isSafeInteger(result.maxSingleMessageSize) || result.maxSingleMessageSize < empty.byteLength || result.maxSingleMessageSize > MAX_SINGLE_MESSAGE_SIZE) {
      return invalidArgumentError("Invalid maxSingleMessageSize.");
    }
    if (!Number.isSafeInteger(result.maxBufferedBytesTotal) || result.maxBufferedBytesTotal < empty.byteLength || !Number.isSafeInteger(result.maxBufferedBytesPerStream) || result.maxBufferedBytesPerStream < empty.byteLength) {
      return invalidArgumentError(
        "Session byte limits are smaller than an empty WireMessage."
      );
    }
    if (result.noStreamTimeoutMs !== null && (!Number.isFinite(result.noStreamTimeoutMs) || result.noStreamTimeoutMs < 0)) {
      return invalidArgumentError(
        "noStreamTimeoutMs must be a non-negative finite number or null."
      );
    }
    return result;
  } catch (error) {
    return statusFromUnknown(error, "Validating Session options raised an exception.");
  }
}
function normalizeSessionHeaders(headers = void 0) {
  return normalizeWireHeaders(headers);
}
function initializeSession(options) {
  try {
    const normalizedOptions = normalizeSessionOptions(options);
    if (!isOk(normalizedOptions)) return normalizedOptions;
    const headers = normalizeSessionHeaders(options.headers);
    if (!isOk(headers)) return headers;
    const id = options.id || randomId("session-");
    const validId = validateName(id);
    if (!isOk(validId)) return validId;
    if (options.onStreamMessage !== void 0 && typeof options.onStreamMessage !== "function") {
      return invalidArgumentError("onStreamMessage must be callable.");
    }
    if (options.onStreamDone !== void 0 && typeof options.onStreamDone !== "function") {
      return invalidArgumentError("onStreamDone must be callable.");
    }
    if (options.nodeMap !== void 0 && !(options.nodeMap instanceof NodeMap)) {
      return invalidArgumentError("nodeMap must be a NodeMap.");
    }
    if (options.actionRegistry !== void 0 && options.actionRegistry !== null && !(options.actionRegistry instanceof ActionRegistry)) {
      return invalidArgumentError(
        "actionRegistry must be an ActionRegistry or null."
      );
    }
    return {
      id,
      headers,
      options: normalizedOptions,
      nodeMap: options.nodeMap ?? new NodeMap(),
      actionRegistry: options.actionRegistry ?? null,
      onStreamMessage: options.onStreamMessage,
      onStreamDone: options.onStreamDone
    };
  } catch (error) {
    return statusFromUnknown(error, "Creating Session configuration raised an exception.");
  }
}
function hasWireStreamShape(value) {
  if (typeof value !== "object" || value === null) return false;
  try {
    const candidate = value;
    return [
      "send",
      "start",
      "accept",
      "halfClose",
      "drainOutgoingMessages",
      "abort",
      "setDeadline",
      "getDeadline",
      "getStatus",
      "getTrailers",
      "getId",
      "getImpl",
      "wait"
    ].every((name) => typeof candidate[name] === "function");
  } catch {
    return false;
  }
}
function firstError2(first, candidate) {
  if (!isStatus(candidate)) {
    return isOk(first) ? internalError("A Status-returning operation returned an invalid value.") : first;
  }
  return isOk(first) && !isOk(candidate) ? candidate : first;
}
function sessionStreamAbortStatus() {
  return abortedError(SESSION_STREAM_ABORT_MESSAGE);
}
function isSessionStreamAbortStatus(status) {
  const expected = sessionStreamAbortStatus();
  return status.code === expected.code && status.message === expected.message;
}
function specialActionNode(nodeId) {
  for (const name of [ACTION_DISPATCH_STATUS_OUTPUT, ACTION_STATUS_OUTPUT]) {
    const suffix = `#${name}`;
    if (nodeId.length > suffix.length && nodeId.endsWith(suffix)) {
      return [nodeId.slice(0, -suffix.length), name];
    }
  }
  return null;
}
function aggregateDispatchFailures(failures, total) {
  const first = failures[0].status;
  const sameCode = failures.every((failure) => failure.status.code === first.code);
  const base = sameCode ? first : unknownError();
  return {
    ...base,
    message: `Failed to dispatch ${failures.length} of ${total} WireMessage elements.`,
    details: failures.map((failure) => ({
      element_type: failure.elementType,
      element_index: failure.elementIndex,
      status: statusToJson(failure.status)
    }))
  };
}
async function invokeSessionMessageCallback(callback, message, stream, session) {
  try {
    const result = await callback(message, stream, session);
    if (result === void 0) return okStatus();
    return isStatus(result) ? result : internalError("Session message callback returned a non-Status value.");
  } catch (error) {
    return statusFromUnknown(error, "Session message callback raised an exception.");
  }
}
async function invokeSessionDoneCallback(callback, stream, session) {
  try {
    const result = await callback(stream, session);
    if (result === void 0) return okStatus();
    return isStatus(result) ? result : internalError("Session done callback returned a non-Status value.");
  } catch (error) {
    return statusFromUnknown(error, "Session done callback raised an exception.");
  }
}
var Session = class _Session {
  id;
  headers;
  options;
  nodeMap;
  actionRegistry;
  onStreamMessage;
  onStreamDone;
  streamStates = /* @__PURE__ */ new Map();
  streamsById = /* @__PURE__ */ new Map();
  streamOrder = [];
  roundRobinIndex = 0;
  bufferedMessages = 0;
  bufferedBytes = 0;
  activeActions = /* @__PURE__ */ new Map();
  rootLimiter;
  nestedLimiter;
  phase = "open";
  sessionStatus = okStatus();
  remoteClosed = false;
  destroyed = false;
  doneDeferred = new Deferred();
  stateWaiters = [];
  noStreamTimer = null;
  deadlineTimer = null;
  constructor(initialized) {
    this.id = initialized.id;
    this.headers = initialized.headers;
    this.options = initialized.options;
    this.nodeMap = initialized.nodeMap;
    this.actionRegistry = initialized.actionRegistry;
    this.rootLimiter = new ActionLimiter(
      initialized.options.maxConcurrentRootActions
    );
    this.nestedLimiter = new ActionLimiter(
      initialized.options.maxConcurrentNestedActions
    );
    this.onStreamMessage = initialized.onStreamMessage ?? (async (message, stream) => message === null ? okStatus() : this.dispatchWireMessage(message, stream));
    this.onStreamDone = initialized.onStreamDone ?? (() => okStatus());
    this.scheduleDeadlineTimer();
    this.scheduleNoStreamTimer();
  }
  /** Create an open session and start deadline/no-stream supervision. */
  static create(options = {}) {
    try {
      const initialized = initializeSession(options);
      return isOk(initialized) ? new _Session(initialized) : initialized;
    } catch (error) {
      return statusFromUnknown(error, "Constructing Session raised an exception.");
    }
  }
  setStreamCallbacks(onMessage, onDone) {
    if (typeof onMessage !== "function" || typeof onDone !== "function") {
      return invalidArgumentError("Session stream callbacks must be callable.");
    }
    this.onStreamMessage = onMessage;
    this.onStreamDone = onDone;
    return okStatus();
  }
  getId() {
    return this.id;
  }
  getHeaders() {
    return copyByteMap(this.headers);
  }
  getOptions() {
    return { ...this.options };
  }
  getNodeMap() {
    return this.nodeMap;
  }
  getActionRegistry() {
    return this.actionRegistry;
  }
  /**
   * Replace the node map and rebind active actions.
   * Existing fragments remain in the old map, so prefer configuring this
   * before traffic starts rather than splitting a live action's state.
   */
  setNodeMap(nodeMap) {
    if (!(nodeMap instanceof NodeMap)) {
      return invalidArgumentError("nodeMap must be a NodeMap.");
    }
    this.nodeMap = nodeMap;
    let first = okStatus();
    for (const action of this.activeActions.values()) {
      first = firstError2(first, action.bindNodeMap(nodeMap));
    }
    return first;
  }
  /**
   * Replace the registry and rebind active actions for later name resolution.
   * Prefer configuring it before dispatch so one operation does not observe
   * registrations from different registry versions.
   */
  setActionRegistry(registry) {
    if (registry !== null && !(registry instanceof ActionRegistry)) {
      return invalidArgumentError(
        "registry must be an ActionRegistry or null."
      );
    }
    this.actionRegistry = registry;
    let first = okStatus();
    for (const action of this.activeActions.values()) {
      first = firstError2(first, action.bindRegistry(registry));
    }
    return first;
  }
  /** Snapshot the streams currently attached to this session. */
  streams() {
    try {
      const result = [];
      for (const stream of this.streamOrder) {
        const state = this.streamStates.get(stream);
        if (state !== void 0) result.push([state.id, state.stream]);
      }
      return result;
    } catch (error) {
      return statusFromUnknown(error, "Listing Session streams raised an exception.");
    }
  }
  getStream(streamId) {
    try {
      const state = this.streamsById.get(streamId);
      return state?.stream ?? notFoundError(
        `Stream '${streamId}' is not attached to the Session.`
      );
    } catch (error) {
      return statusFromUnknown(error, "Looking up Session stream raised an exception.");
    }
  }
  /** Snapshot actions currently tracked as in-flight. */
  actions() {
    try {
      return [...this.activeActions];
    } catch {
      return [];
    }
  }
  getAction(actionId) {
    try {
      return this.activeActions.get(actionId) ?? notFoundError(
        `Action '${actionId}' is not active in the Session.`
      );
    } catch (error) {
      return statusFromUnknown(error, "Looking up Session Action raised an exception.");
    }
  }
  /** Request cooperative cancellation of one active action. */
  cancelAction(actionId) {
    const action = this.getAction(actionId);
    return isOk(action) ? action.cancel() : action;
  }
  /** Request cancellation of every action without waiting for teardown. */
  cancelAllActions() {
    let first = okStatus();
    for (const action of this.activeActions.values()) {
      first = firstError2(first, action.cancel());
    }
    return first;
  }
  /** Await all actions observed during the wait and aggregate their failures. */
  async awaitAllActions(timeoutMs) {
    try {
      if (timeoutMs !== void 0 && (!Number.isFinite(timeoutMs) || timeoutMs < 0)) {
        return invalidArgumentError(
          "timeoutMs must be a non-negative finite number."
        );
      }
      const deadline = timeoutMs === void 0 ? null : Date.now() + timeoutMs;
      const observed = /* @__PURE__ */ new Set();
      const failures = [];
      while (true) {
        const pending = [...this.activeActions.values()].filter(
          (action) => !observed.has(action)
        );
        if (pending.length === 0) break;
        for (const action of pending) {
          observed.add(action);
          const remaining = deadline === null ? void 0 : Math.max(0, deadline - Date.now());
          const result = await action.wait(remaining);
          if (!isOk(result)) {
            if (!action.isDone() && result.code === deadlineExceededError().code) {
              return result;
            }
            failures.push(result);
          }
        }
      }
      if (failures.length === 0) return okStatus();
      const code = failures.every((failure) => failure.code === failures[0].code) ? failures[0].code : unknownError().code;
      const base = code === failures[0].code ? failures[0] : unknownError();
      return {
        ...base,
        message: `${failures.length} Actions completed with errors.`,
        details: failures.map((status) => ({ status: statusToJson(status) }))
      };
    } catch (error) {
      return statusFromUnknown(error, "Waiting for Session Actions raised an exception.");
    }
  }
  /** Track an action so concurrency, cancellation, and shutdown include it. */
  trackAction(action) {
    if (!(action instanceof Action)) {
      return invalidArgumentError("action must be an Action.");
    }
    if (this.phase !== "open" || this.remoteClosed) {
      return failedPreconditionError(
        "Session is no longer accepting Actions."
      );
    }
    const id = action.getId();
    const found = this.activeActions.get(id);
    if (found !== void 0 && found !== action) {
      return alreadyExistsError(
        `Action '${id}' already exists in the Session.`
      );
    }
    this.activeActions.set(id, action);
    this.notifyStateChanged();
    return okStatus();
  }
  untrackAction(action) {
    try {
      const id = action.getId();
      if (this.activeActions.get(id) === action) {
        this.activeActions.delete(id);
        this.notifyStateChanged();
      }
    } catch {
    }
  }
  acquireActionSlot(nested, signal) {
    try {
      return (nested ? this.nestedLimiter : this.rootLimiter).acquire(signal);
    } catch (error) {
      return Promise.resolve(
        statusFromUnknown(error, "Acquiring a Session Action slot raised.")
      );
    }
  }
  releaseActionSlot(nested) {
    try {
      (nested ? this.nestedLimiter : this.rootLimiter).release();
    } catch {
    }
  }
  /** Apply a received fragment, including reserved action-status nodes. */
  async dispatchNodeFragment(fragment) {
    try {
      if (!(fragment instanceof NodeFragment)) {
        return invalidArgumentError("fragment must be a NodeFragment.");
      }
      const validation = fragment.validate();
      if (!isOk(validation)) return validation;
      const special = specialActionNode(fragment.id);
      const chunk = fragment.getChunk();
      let protocolStatus = null;
      let action = null;
      if (isOk(chunk) && isCloseStatusChunk(chunk)) {
        const decoded = decodeStatusChunk(chunk);
        if (!isOk(decoded)) return decoded;
        const seq = fragment.seq ?? 0;
        const mirror = this.nodeMap.getIfExists(fragment.id);
        if (!isOk(mirror)) return mirror;
        if (mirror === null) return seq;
        const writable = await mirror.isWritable();
        if (!isOk(writable)) return writable;
        if (!writable) return seq;
        const applied = isOk(decoded.status) ? await mirror.drainAndClose() : await mirror.abortWithStatus(decoded.status);
        if (!isOk(applied)) return applied;
        return seq;
      }
      if (special !== null) {
        if (!isOk(chunk) || !isStatusChunk(chunk)) {
          return invalidArgumentError(
            "An Action status node requires a status Chunk."
          );
        }
        const decoded = decodeStatusChunk(chunk);
        if (!isOk(decoded)) return decoded;
        protocolStatus = decoded.status;
        const found = this.getAction(special[0]);
        if (!isOk(found)) {
          return notFoundError("Received status for an unknown Action.");
        }
        action = found;
      }
      const node = await this.nodeMap.get(fragment.id);
      if (!isOk(node)) return node;
      if (isOk(chunk) && isStatusChunk(chunk) && special === null) {
        const decoded = decodeStatusChunk(chunk);
        if (!isOk(decoded)) return decoded;
        if (isOk(decoded.status)) {
          return invalidArgumentError(
            "An ordinary node cannot be aborted with an OK status."
          );
        }
        const writable = await node.isWritable();
        if (!isOk(writable)) return writable;
        if (writable) {
          const closed = await node.abortWithStatus(decoded.status);
          if (!isOk(closed)) return closed;
        }
        return fragment.seq ?? 0;
      }
      if (node.getWriterAbortStatus() !== null) return fragment.seq ?? 0;
      const stored = await node.putFragment(fragment);
      if (!isOk(stored)) {
        const abortStatus = node.getWriterAbortStatus();
        if (abortStatus !== null && abortStatus.code === stored.code && abortStatus.message === stored.message) {
          return fragment.seq ?? 0;
        }
        return stored;
      }
      if (special !== null && action !== null && protocolStatus !== null) {
        const applied = special[1] === ACTION_DISPATCH_STATUS_OUTPUT ? action.setDispatchStatus(protocolStatus) : action.setCompletionStatus(protocolStatus);
        if (!isOk(applied)) return applied;
      }
      return stored;
    } catch (error) {
      return statusFromUnknown(error, "Dispatching NodeFragment raised an exception.");
    }
  }
  /** Resolve, acknowledge, and start one inbound registered action call. */
  async dispatchActionMessage(message, originStream = null) {
    try {
      const validation = message.validate();
      if (!isOk(validation)) return validation;
      if (message.name === CANCEL_ACTION_NAME) {
        let encodedId = null;
        for (const [name, value] of message.headers) {
          if (name.toLowerCase() === CANCEL_ACTION_HEADER) {
            encodedId = value;
            break;
          }
        }
        if (encodedId === null) {
          return invalidArgumentError(
            "Cancel Action requires the __action header."
          );
        }
        const actionId = utf8Decode(encodedId);
        if (!isOk(actionId)) return actionId;
        const validId = validateName(actionId);
        if (!isOk(validId)) return validId;
        const action2 = this.getAction(actionId);
        if (isOk(action2)) return action2.cancel();
        return action2.code === notFoundError().code ? okStatus() : action2;
      }
      let dispatchStatus = okStatus();
      let action = null;
      if (this.phase !== "open" || this.remoteClosed) {
        dispatchStatus = failedPreconditionError(
          "Session is no longer accepting Actions."
        );
      } else if (this.activeActions.has(message.id)) {
        dispatchStatus = alreadyExistsError(
          "Action already exists in the Session."
        );
      } else if (this.actionRegistry === null) {
        dispatchStatus = failedPreconditionError(
          "Session has no ActionRegistry."
        );
      } else {
        const created = this.actionRegistry.makeAction(message.name, {
          id: message.id,
          nodeMap: this.nodeMap,
          stream: originStream,
          session: this
        });
        if (!isOk(created)) dispatchStatus = created;
        else action = created;
      }
      if (isOk(dispatchStatus) && action !== null) {
        dispatchStatus = action.mapPortsFromMessage(message);
        for (const [name, value] of message.headers) {
          if (!isOk(dispatchStatus)) break;
          dispatchStatus = action.setHeader(name, value);
        }
        if (isOk(dispatchStatus)) dispatchStatus = action.clearInputsAfterRun();
        if (isOk(dispatchStatus)) dispatchStatus = action.clearOutputsAfterRun();
        if (isOk(dispatchStatus)) {
          dispatchStatus = await action.applyInputAutofills();
        }
        if (isOk(dispatchStatus)) {
          const started = action.run();
          dispatchStatus = isOk(started) ? okStatus() : started;
        }
      }
      if (originStream !== null) {
        const chunk = statusToChunk(dispatchStatus);
        if (!isOk(chunk)) return chunk;
        const dispatchId = Action.makeNodeId(
          message.id,
          ACTION_DISPATCH_STATUS_OUTPUT
        );
        if (!isOk(dispatchId)) return dispatchId;
        const fragments = [new NodeFragment({
          id: dispatchId,
          data: chunk,
          seq: 0,
          continued: false
        })];
        if (!isOk(dispatchStatus)) {
          const statusId = Action.makeNodeId(message.id, ACTION_STATUS_OUTPUT);
          if (!isOk(statusId)) return statusId;
          fragments.push(new NodeFragment({
            id: statusId,
            data: chunk,
            seq: 0,
            continued: false
          }));
        }
        let sent;
        try {
          sent = originStream.send(new WireMessage({ nodeFragments: fragments }));
        } catch (error) {
          sent = statusFromUnknown(error, "Sending Action dispatch status raised.");
        }
        return isStatus(sent) ? sent : internalError("WireStream.send() returned an invalid Status.");
      }
      return dispatchStatus;
    } catch (error) {
      return statusFromUnknown(error, "Dispatching ActionMessage raised an exception.");
    }
  }
  async dispatchAction(action) {
    try {
      if (!(action instanceof Action)) {
        return invalidArgumentError("action must be an Action.");
      }
      if (action.getRegistry() === null) {
        const boundRegistry = action.bindRegistry(this.actionRegistry);
        if (!isOk(boundRegistry)) return boundRegistry;
      }
      const bound = action.bindSession(this);
      if (!isOk(bound)) return bound;
      const started = action.run();
      return isOk(started) ? okStatus() : started;
    } catch (error) {
      return statusFromUnknown(error, "Dispatching Action raised an exception.");
    }
  }
  /** Dispatch every action and fragment in one validated inbound message. */
  async dispatchWireMessage(message, originStream = null) {
    try {
      if (!(message instanceof WireMessage)) {
        return invalidArgumentError("message must be a WireMessage.");
      }
      const validation = message.validate();
      if (!isOk(validation)) return validation;
      const failures = [];
      for (let index = 0; index < message.actions.length; ++index) {
        const status = await this.dispatchActionMessage(
          message.actions[index],
          originStream
        );
        if (!isOk(status)) {
          failures.push({
            elementType: "action_message",
            elementIndex: index,
            status
          });
        }
      }
      for (let index = 0; index < message.nodeFragments.length; ++index) {
        const fragment = message.nodeFragments[index];
        const status = await this.dispatchNodeFragment(fragment);
        if (!isOk(status)) {
          const separator = fragment.id.indexOf("#");
          if (separator >= 0) {
            this.cancelAction(fragment.id.slice(0, separator));
          }
          failures.push({
            elementType: "node_fragment",
            elementIndex: index,
            status
          });
        }
      }
      return failures.length === 0 ? okStatus() : aggregateDispatchFailures(
        failures,
        message.actions.length + message.nodeFragments.length
      );
    } catch (error) {
      return statusFromUnknown(error, "Dispatching WireMessage raised an exception.");
    }
  }
  /**
   * Attach and drive one transport endpoint.
   *
   * The returned status covers the stream startup handshake. The session keeps
   * pumping it afterwards; await {@link done} for connection-wide completion.
   */
  async addStream(stream, mode = "start" /* START */) {
    let state = null;
    try {
      if (!hasWireStreamShape(stream)) {
        return invalidArgumentError("stream must implement WireStream.");
      }
      if (mode !== "start" /* START */ && mode !== "accept" /* ACCEPT */) {
        return invalidArgumentError("mode must be StreamMode.START or ACCEPT.");
      }
      if (this.deadlineExpired()) {
        this.abort(deadlineExceededError("The Session deadline has been exceeded."));
      }
      let streamId;
      try {
        streamId = stream.getId();
      } catch (error) {
        return statusFromUnknown(error, "WireStream.getId() raised an exception.");
      }
      const validId = validateName(streamId);
      if (!isOk(validId)) return validId;
      if (this.phase !== "open" || this.remoteClosed) {
        return failedPreconditionError(
          "No streams can be attached after the Session ends."
        );
      }
      if (this.streamsById.has(streamId) || this.streamStates.has(stream)) {
        return alreadyExistsError("Stream is already attached to the Session.");
      }
      state = {
        stream,
        id: streamId,
        outstandingMessages: 0,
        outstandingBytes: 0,
        pendingMessages: [],
        messagePumpRunning: false,
        acceptingMessages: true,
        remoteHalfClosed: false,
        halfCloseDelivered: false,
        doneStarted: false,
        done: false
      };
      this.streamsById.set(streamId, state);
      this.streamStates.set(stream, state);
      this.streamOrder.push(stream);
      this.clearNoStreamTimer();
      this.notifyStateChanged();
      const onMessage = async (message) => this.handleStreamMessage(state, message);
      const onDone = async () => this.handleStreamDone(state);
      let startup;
      try {
        startup = mode === "start" /* START */ ? await stream.start(onMessage, onDone) : await stream.accept(onMessage, onDone);
      } catch (error) {
        startup = statusFromUnknown(error, "WireStream startup raised an exception.");
      }
      if (!isStatus(startup)) {
        const invalid = internalError("WireStream startup returned an invalid Status.");
        this.removeStream(state);
        return invalid;
      }
      if (!isOk(startup)) this.removeStream(state);
      return startup;
    } catch (error) {
      if (state !== null) this.removeStream(state);
      return statusFromUnknown(error, "Attaching Session stream raised an exception.");
    }
  }
  /** Queue a message on a named stream or round-robin across active streams. */
  send(message, streamId = "") {
    try {
      if (!(message instanceof WireMessage)) {
        return invalidArgumentError("message must be a WireMessage.");
      }
      const validation = message.validate();
      if (!isOk(validation)) return validation;
      if (this.phase !== "open") {
        return failedPreconditionError(
          "Messages cannot be sent after the Session ends."
        );
      }
      let stream;
      if (streamId !== "") {
        const found = this.streamsById.get(streamId);
        if (found === void 0) {
          return notFoundError("Session stream was not found.");
        }
        stream = found.stream;
      } else {
        const available = this.streamOrder.map((candidate) => this.streamStates.get(candidate)).filter(
          (candidate) => candidate !== void 0 && !candidate.done && !candidate.doneStarted
        );
        if (available.length === 0) {
          return notFoundError("Session has no attached streams.");
        }
        const index = this.roundRobinIndex % available.length;
        this.roundRobinIndex = (index + 1) % available.length;
        stream = available[index].stream;
      }
      try {
        const sent = stream.send(message);
        return isStatus(sent) ? sent : internalError("WireStream.send() returned an invalid Status.");
      } catch (error) {
        return statusFromUnknown(error, "WireStream.send() raised an exception.");
      }
    } catch (error) {
      return statusFromUnknown(error, "Sending Session message raised an exception.");
    }
  }
  /**
   * Begin clean shutdown and half-close every active stream with OK trailers.
   * Existing inbound work may still arrive and attached streams must still
   * finish before {@link done} resolves.
   */
  halfClose() {
    try {
      if (this.phase !== "open") return okStatus();
      if (this.deadlineExpired()) {
        return this.abort(
          deadlineExceededError("The Session deadline has been exceeded.")
        );
      }
      const packed = packStatus(okStatus());
      if (!isOk(packed)) return packed;
      const trailers = copyByteMap(this.headers);
      trailers.set(SESSION_STATUS_HEADER, packed);
      this.phase = "closing";
      this.sessionStatus = okStatus();
      this.clearTimers();
      this.notifyStateChanged();
      let first = okStatus();
      for (const state of this.streamStates.values()) {
        if (state.done || state.doneStarted) continue;
        try {
          const closed = state.stream.halfClose(trailers);
          first = firstError2(first, closed);
        } catch (error) {
          first = firstError2(
            first,
            statusFromUnknown(error, "WireStream.halfClose() raised.")
          );
        }
      }
      this.finishIfPossible();
      return first;
    } catch (error) {
      return statusFromUnknown(error, "Half-closing Session raised an exception.");
    }
  }
  /** Cancel actions and end the session with a structured non-OK status. */
  abort(status) {
    try {
      if (!isStatus(status) || isOk(status)) {
        return invalidArgumentError("An aborted Session needs a non-OK status.");
      }
      if (this.phase !== "open") return okStatus();
      if (this.deadlineExpired()) {
        status = deadlineExceededError("The Session deadline has been exceeded.");
      }
      const sessionStatus = packStatus(status);
      if (!isOk(sessionStatus)) return sessionStatus;
      const streamAbort = sessionStreamAbortStatus();
      const packedStreamAbort = packStatus(streamAbort);
      if (!isOk(packedStreamAbort)) return packedStreamAbort;
      const headers = copyByteMap(this.headers);
      headers.set(SESSION_STATUS_HEADER, sessionStatus);
      headers.set(ABORT_STATUS_HEADER, packedStreamAbort);
      const terminal = new WireMessage({ headers });
      this.phase = "aborted";
      this.sessionStatus = status;
      this.clearTimers();
      const cancellation = cancelledError("Session was aborted.");
      this.rootLimiter.cancel(cancellation);
      this.nestedLimiter.cancel(cancellation);
      for (const state of this.streamStates.values()) {
        state.acceptingMessages = false;
        this.clearPendingMessages(state);
      }
      this.notifyStateChanged();
      this.cancelAllActions();
      let first = okStatus();
      for (const state of this.streamStates.values()) {
        if (state.done || state.doneStarted) continue;
        let sent;
        try {
          sent = state.stream.send(terminal);
        } catch (error) {
          sent = statusFromUnknown(error, "Sending Session abort raised.");
        }
        if (!isStatus(sent)) {
          sent = internalError("WireStream.send() returned an invalid Status.");
        }
        if (!isOk(sent)) {
          first = firstError2(first, sent);
          try {
            const aborted = state.stream.abort(streamAbort);
            first = firstError2(first, aborted);
          } catch (error) {
            first = firstError2(
              first,
              statusFromUnknown(error, "WireStream.abort() raised.")
            );
          }
        }
      }
      this.finishIfPossible();
      return first;
    } catch (error) {
      return statusFromUnknown(error, "Aborting Session raised an exception.");
    }
  }
  /** Whether either endpoint has ended the session for new work. */
  isClosed() {
    return this.remoteClosed || this.phase !== "open";
  }
  /** Whether every attached stream has completed and state is fully quiescent. */
  isDone() {
    return this.destroyed;
  }
  /** Await full cleanup and receive the session's terminal status. */
  done() {
    return this.doneDeferred.promise;
  }
  getStatus() {
    if (this.phase === "open" && this.deadlineExpired()) {
      this.abort(deadlineExceededError("The Session deadline has been exceeded."));
    }
    return this.sessionStatus;
  }
  getDeadline() {
    return this.options.deadline;
  }
  setDeadline(deadline = null) {
    const normalized = wireDeadlineMillis(deadline);
    if (!isOk(normalized)) return normalized;
    this.options.deadline = normalized;
    this.scheduleDeadlineTimer();
    this.notifyStateChanged();
    return this.phase === "open" && this.deadlineExpired() ? this.abort(
      deadlineExceededError("The Session deadline has been exceeded.")
    ) : okStatus();
  }
  streamIdOf(stream) {
    return this.streamStates.get(stream)?.id ?? "";
  }
  async handleStreamMessage(state, message) {
    try {
      if (message === null) return this.handleRemoteHalfClose(state);
      if (!(message instanceof WireMessage)) {
        return invalidArgumentError("WireStream delivered a non-WireMessage value.");
      }
      if (state.remoteHalfClosed) {
        return failedPreconditionError(
          "WireStream delivered data after its remote half-close."
        );
      }
      const encoded = message.toMsgpack();
      if (!isOk(encoded)) return encoded;
      const size = encoded.byteLength;
      if (size > this.options.maxSingleMessageSize) {
        return outOfRangeError(
          "Incoming WireMessage exceeds maxSingleMessageSize."
        );
      }
      while (true) {
        if (!state.acceptingMessages || this.phase === "aborted") {
          return okStatus();
        }
        const countsFit = this.bufferedMessages < this.options.maxBufferedMessagesTotal && state.outstandingMessages < this.options.maxBufferedMessagesPerStream;
        const totalBytesFit = this.bufferedBytes === 0 || this.bufferedBytes + size <= this.options.maxBufferedBytesTotal;
        const streamBytesFit = state.outstandingBytes === 0 || state.outstandingBytes + size <= this.options.maxBufferedBytesPerStream;
        if (countsFit && totalBytesFit && streamBytesFit) break;
        await this.waitForStateChange();
      }
      ++this.bufferedMessages;
      this.bufferedBytes += size;
      ++state.outstandingMessages;
      state.outstandingBytes += size;
      state.pendingMessages.push({ message, size });
      if (!state.messagePumpRunning) {
        state.messagePumpRunning = true;
        queueMicrotask(() => void this.processStreamMessages(state));
      }
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "Handling Session stream message raised.");
    }
  }
  async handleRemoteHalfClose(state) {
    state.remoteHalfClosed = true;
    while (state.acceptingMessages && this.phase !== "aborted" && state.outstandingMessages > 0) {
      await this.waitForStateChange();
    }
    if (!state.acceptingMessages || this.phase === "aborted" || state.halfCloseDelivered) {
      return okStatus();
    }
    const trailers = this.safeGetTrailers(state.stream);
    if (trailers !== null) {
      const encoded = trailers.get(SESSION_STATUS_HEADER);
      if (encoded !== void 0) {
        const decoded = decodeStatus(encoded);
        if (!isOk(decoded)) return decoded;
        if (!isOk(decoded.status)) {
          return failedPreconditionError(
            "A peer must abort, not half-close, a failed Session."
          );
        }
        this.remoteClosed = true;
        this.notifyStateChanged();
      }
    }
    state.halfCloseDelivered = true;
    return invokeSessionMessageCallback(
      this.onStreamMessage,
      null,
      state.stream,
      this
    );
  }
  async processStreamMessages(state) {
    try {
      while (state.pendingMessages.length > 0) {
        const buffered = state.pendingMessages.shift();
        let callbackStatus;
        if (this.phase === "aborted" || !state.acceptingMessages) {
          callbackStatus = okStatus();
        } else {
          callbackStatus = await invokeSessionMessageCallback(
            this.onStreamMessage,
            buffered.message,
            state.stream,
            this
          );
        }
        this.releaseBufferedMessage(state, buffered.size);
        if (!isOk(callbackStatus)) {
          state.acceptingMessages = false;
          this.clearPendingMessages(state);
          state.messagePumpRunning = false;
          this.notifyStateChanged();
          if (this.phase !== "aborted") {
            try {
              state.stream.abort(callbackStatus);
            } catch {
            }
          }
          return;
        }
      }
      state.messagePumpRunning = false;
      this.notifyStateChanged();
    } catch (error) {
      state.acceptingMessages = false;
      this.clearPendingMessages(state);
      state.messagePumpRunning = false;
      const status = statusFromUnknown(error, "Session message pump raised.");
      try {
        state.stream.abort(status);
      } catch {
      }
      this.notifyStateChanged();
    }
  }
  async handleStreamDone(state) {
    try {
      if (state.done || state.doneStarted) return okStatus();
      state.doneStarted = true;
      const streamStatus = this.safeGetStatus(state.stream);
      if (isSessionStreamAbortStatus(streamStatus) && this.phase !== "aborted") {
        this.remoteClosed = true;
        this.phase = "aborted";
        this.sessionStatus = this.sessionStatusFromTrailers(state.stream) ?? streamStatus;
        this.clearTimers();
        for (const candidate of this.streamStates.values()) {
          candidate.acceptingMessages = false;
          this.clearPendingMessages(candidate);
        }
        const cancellation = cancelledError("Remote Session was aborted.");
        this.rootLimiter.cancel(cancellation);
        this.nestedLimiter.cancel(cancellation);
        this.cancelAllActions();
      } else if (!isOk(streamStatus)) {
        state.acceptingMessages = false;
      }
      this.notifyStateChanged();
      while (state.outstandingMessages > 0) await this.waitForStateChange();
      const callbackStatus = await invokeSessionDoneCallback(
        this.onStreamDone,
        state.stream,
        this
      );
      this.removeStream(state);
      return callbackStatus;
    } catch (error) {
      this.removeStream(state);
      return statusFromUnknown(error, "Handling Session stream completion raised.");
    }
  }
  removeStream(state) {
    if (state.done) return;
    state.done = true;
    state.acceptingMessages = false;
    this.clearPendingMessages(state);
    this.streamStates.delete(state.stream);
    if (this.streamsById.get(state.id) === state) this.streamsById.delete(state.id);
    const index = this.streamOrder.indexOf(state.stream);
    if (index >= 0) this.streamOrder.splice(index, 1);
    if (this.streamStates.size === 0 && this.phase === "open" && !this.remoteClosed) {
      this.scheduleNoStreamTimer();
    }
    this.notifyStateChanged();
    this.finishIfPossible();
  }
  releaseBufferedMessage(state, size) {
    this.bufferedMessages = Math.max(0, this.bufferedMessages - 1);
    this.bufferedBytes = Math.max(0, this.bufferedBytes - size);
    state.outstandingMessages = Math.max(0, state.outstandingMessages - 1);
    state.outstandingBytes = Math.max(0, state.outstandingBytes - size);
    this.notifyStateChanged();
  }
  clearPendingMessages(state) {
    for (const buffered of state.pendingMessages.splice(0)) {
      this.bufferedMessages = Math.max(0, this.bufferedMessages - 1);
      this.bufferedBytes = Math.max(0, this.bufferedBytes - buffered.size);
      state.outstandingMessages = Math.max(0, state.outstandingMessages - 1);
      state.outstandingBytes = Math.max(0, state.outstandingBytes - buffered.size);
    }
    this.notifyStateChanged();
  }
  finishIfPossible() {
    if (this.phase === "open" && !this.remoteClosed || this.streamStates.size !== 0 || this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.clearTimers();
    this.doneDeferred.resolve(this.sessionStatus);
    this.notifyStateChanged();
  }
  notifyStateChanged() {
    const waiters = this.stateWaiters;
    this.stateWaiters = [];
    for (const waiter of waiters) waiter.resolve(void 0);
  }
  waitForStateChange() {
    const waiter = new Deferred();
    this.stateWaiters.push(waiter);
    return waiter.promise;
  }
  safeGetStatus(stream) {
    try {
      const status = stream.getStatus();
      return isStatus(status) ? status : internalError("WireStream.getStatus() returned an invalid Status.");
    } catch (error) {
      return statusFromUnknown(error, "WireStream.getStatus() raised.");
    }
  }
  safeGetTrailers(stream) {
    try {
      const trailers = stream.getTrailers();
      return trailers === null ? null : copyByteMap(trailers);
    } catch {
      return null;
    }
  }
  sessionStatusFromTrailers(stream) {
    const encoded = this.safeGetTrailers(stream)?.get(SESSION_STATUS_HEADER);
    if (encoded === void 0) return null;
    const decoded = decodeStatus(encoded);
    return isOk(decoded) ? decoded.status : null;
  }
  deadlineExpired() {
    return this.options.deadline !== null && this.options.deadline <= Date.now();
  }
  scheduleDeadlineTimer() {
    if (this.deadlineTimer !== null) clearTimeout(this.deadlineTimer);
    this.deadlineTimer = null;
    if (this.options.deadline === null || this.phase !== "open") return;
    const delay = Math.max(0, this.options.deadline - Date.now());
    this.deadlineTimer = setTimeout(() => {
      this.deadlineTimer = null;
      if (this.phase === "open") {
        this.abort(
          deadlineExceededError("The Session deadline has been exceeded.")
        );
      }
    }, delay);
    this.unrefTimer(this.deadlineTimer);
  }
  scheduleNoStreamTimer() {
    this.clearNoStreamTimer();
    if (this.options.noStreamTimeoutMs === null || this.phase !== "open" || this.remoteClosed || this.streamStates.size !== 0) {
      return;
    }
    this.noStreamTimer = setTimeout(() => {
      this.noStreamTimer = null;
      if (this.phase === "open" && !this.remoteClosed && this.streamStates.size === 0) {
        this.halfClose();
      }
    }, this.options.noStreamTimeoutMs);
    this.unrefTimer(this.noStreamTimer);
  }
  clearNoStreamTimer() {
    if (this.noStreamTimer !== null) clearTimeout(this.noStreamTimer);
    this.noStreamTimer = null;
  }
  clearTimers() {
    this.clearNoStreamTimer();
    if (this.deadlineTimer !== null) clearTimeout(this.deadlineTimer);
    this.deadlineTimer = null;
  }
  unrefTimer(timer) {
    try {
      timer.unref?.();
    } catch {
    }
  }
};

// src/byte_chunking.ts
var COMPLETE_METADATA_SIZE = 9;
var CHUNK_METADATA_SIZE = 13;
var FIRST_CHUNK_METADATA_SIZE = 17;
var MINIMUM_BYTE_PACKET_SIZE = FIRST_CHUNK_METADATA_SIZE + 1;
var UINT32_MAX6 = 4294967295;
var UINT64_MAX = 0xffffffffffffffffn;
function normalizeByteChunkingOptions(options = {}) {
  try {
    if (typeof options !== "object" || options === null) {
      return invalidArgumentError("Byte chunking options must be an object.");
    }
    const result = {
      packetSize: options.packetSize ?? 64 * 1024,
      maxMessageSize: options.maxMessageSize ?? 32 * 1024 * 1024,
      maxPendingMessages: options.maxPendingMessages ?? 64,
      maxPendingBytes: options.maxPendingBytes ?? 64 * 1024 * 1024
    };
    for (const [name, value] of Object.entries(result)) {
      if (!Number.isSafeInteger(value) || value <= 0) {
        return invalidArgumentError(`${name} must be a positive safe integer.`);
      }
    }
    if (result.packetSize < MINIMUM_BYTE_PACKET_SIZE) {
      return invalidArgumentError(
        `packetSize must be at least ${MINIMUM_BYTE_PACKET_SIZE}.`
      );
    }
    if (result.packetSize > result.maxMessageSize + COMPLETE_METADATA_SIZE) {
      return invalidArgumentError(
        "packetSize must not exceed maxMessageSize plus complete-packet metadata."
      );
    }
    return result;
  } catch (error) {
    return invalidArgumentError("Byte chunking options could not be read.", [], error);
  }
}
function normalizeTransientId(value) {
  if (typeof value === "bigint") {
    return value >= 0n && value <= UINT64_MAX ? value : outOfRangeError("transientId must be an unsigned 64-bit integer.");
  }
  if (!Number.isSafeInteger(value) || value < 0) {
    return invalidArgumentError(
      "transientId must be a non-negative safe integer or bigint."
    );
  }
  return BigInt(value);
}
function setUint64LittleEndian(view, offset, value) {
  view.setUint32(offset, Number(value & 0xffffffffn), true);
  view.setUint32(offset + 4, Number(value >> 32n), true);
}
function getUint64LittleEndian(view, offset) {
  const low = BigInt(view.getUint32(offset, true));
  const high = BigInt(view.getUint32(offset + 4, true));
  return low | high << 32n;
}
function completePacket(payload, transientId) {
  const result = new Uint8Array(payload.byteLength + COMPLETE_METADATA_SIZE);
  result.set(payload);
  const view = new DataView(result.buffer);
  setUint64LittleEndian(view, payload.byteLength, transientId);
  result[result.length - 1] = 0 /* COMPLETE_BYTES */;
  return result;
}
function chunkPacket(payload, transientId, sequence, packetCount) {
  const metadataSize = packetCount === null ? CHUNK_METADATA_SIZE : FIRST_CHUNK_METADATA_SIZE;
  const result = new Uint8Array(payload.byteLength + metadataSize);
  result.set(payload);
  const view = new DataView(result.buffer);
  let offset = payload.byteLength;
  if (packetCount !== null) {
    view.setUint32(offset, packetCount, true);
    offset += 4;
  }
  view.setUint32(offset, sequence, true);
  setUint64LittleEndian(view, offset + 4, transientId);
  result[result.length - 1] = packetCount === null ? 1 /* BYTE_CHUNK */ : 2 /* LENGTH_SUFFIXED_BYTE_CHUNK */;
  return result;
}
function splitBytesIntoPackets(source, transientId, packetSize = 64 * 1024) {
  const bytes = toBytes(source);
  if (!isOk(bytes)) return bytes;
  const id = normalizeTransientId(transientId);
  if (!isOk(id)) return id;
  if (!Number.isSafeInteger(packetSize) || packetSize < MINIMUM_BYTE_PACKET_SIZE) {
    return invalidArgumentError(
      `packetSize must be an integer of at least ${MINIMUM_BYTE_PACKET_SIZE}.`
    );
  }
  try {
    if (bytes.byteLength <= packetSize - COMPLETE_METADATA_SIZE) {
      return [completePacket(bytes, id)];
    }
    const firstPayloadSize = packetSize - FIRST_CHUNK_METADATA_SIZE;
    const laterPayloadSize = packetSize - CHUNK_METADATA_SIZE;
    const laterCount = Math.ceil(
      (bytes.byteLength - firstPayloadSize) / laterPayloadSize
    );
    if (!Number.isSafeInteger(laterCount) || laterCount >= UINT32_MAX6) {
      return outOfRangeError("Byte message requires too many packets.");
    }
    const packetCount = laterCount + 1;
    const packets = [
      chunkPacket(bytes.subarray(0, firstPayloadSize), id, 0, packetCount)
    ];
    let offset = firstPayloadSize;
    for (let sequence = 1; offset < bytes.byteLength; ++sequence) {
      const end = Math.min(bytes.byteLength, offset + laterPayloadSize);
      packets.push(chunkPacket(bytes.subarray(offset, end), id, sequence, null));
      offset = end;
    }
    return packets;
  } catch (error) {
    return statusFromUnknown(error, "Failed to split byte message.");
  }
}
function parseBytePacket(source) {
  const packet = toBytes(source);
  if (!isOk(packet)) return packet;
  if (packet.byteLength < COMPLETE_METADATA_SIZE) {
    return invalidArgumentError(
      "Byte packet is shorter than complete-packet metadata."
    );
  }
  try {
    const rawType = packet[packet.length - 1];
    if (rawType !== 0 /* COMPLETE_BYTES */ && rawType !== 1 /* BYTE_CHUNK */ && rawType !== 2 /* LENGTH_SUFFIXED_BYTE_CHUNK */) {
      return invalidArgumentError("Byte packet has an unknown type.");
    }
    const type = rawType;
    const metadataSize = type === 0 /* COMPLETE_BYTES */ ? COMPLETE_METADATA_SIZE : type === 1 /* BYTE_CHUNK */ ? CHUNK_METADATA_SIZE : FIRST_CHUNK_METADATA_SIZE;
    if (packet.byteLength < metadataSize) {
      return invalidArgumentError(
        "Byte packet is shorter than its declared metadata."
      );
    }
    const view = new DataView(
      packet.buffer,
      packet.byteOffset,
      packet.byteLength
    );
    const transientOffset = packet.byteLength - COMPLETE_METADATA_SIZE;
    let sequence = 0;
    let packetCount = 0;
    if (type !== 0 /* COMPLETE_BYTES */) {
      const sequenceOffset = transientOffset - 4;
      sequence = view.getUint32(sequenceOffset, true);
      if (type === 2 /* LENGTH_SUFFIXED_BYTE_CHUNK */) {
        packetCount = view.getUint32(sequenceOffset - 4, true);
        if (sequence !== 0 || packetCount === 0) {
          return invalidArgumentError(
            "First byte chunk must have sequence zero and a positive count."
          );
        }
      }
    }
    return {
      type,
      payload: packet.slice(0, packet.byteLength - metadataSize),
      transientId: getUint64LittleEndian(view, transientOffset),
      sequence,
      packetCount
    };
  } catch (error) {
    return statusFromUnknown(error, "Failed to parse byte packet.");
  }
}
var ByteReassembler = class _ByteReassembler {
  options;
  pending = /* @__PURE__ */ new Map();
  pendingBytesInternal = 0;
  constructor(options) {
    this.options = Object.freeze({ ...options });
  }
  /** Validate limits and create an empty reassembly table. */
  static create(options = {}) {
    try {
      const normalized = normalizeByteChunkingOptions(options);
      return isOk(normalized) ? new _ByteReassembler(normalized) : normalized;
    } catch (error) {
      return statusFromUnknown(error, "Creating ByteReassembler raised an exception.");
    }
  }
  /** Number of transient ids still waiting for packets. */
  get pendingMessageCount() {
    return this.pending.size;
  }
  /** Aggregate payload bytes retained for incomplete messages. */
  get pendingByteCount() {
    return this.pendingBytesInternal;
  }
  /** Discard partial messages when a channel resets or aborts. */
  clear() {
    this.pending.clear();
    this.pendingBytesInternal = 0;
    return okStatus();
  }
  /** Admit one packet and return a full message when this completes one. */
  feed(source) {
    const serialized = toBytes(source);
    if (!isOk(serialized)) return serialized;
    if (serialized.byteLength > this.options.packetSize) {
      return outOfRangeError("Incoming byte packet exceeds packetSize.");
    }
    const packet = parseBytePacket(serialized);
    if (!isOk(packet)) return packet;
    if (packet.payload.byteLength > this.options.maxMessageSize) {
      return outOfRangeError("Incoming byte message exceeds its limit.");
    }
    if (packet.type === 0 /* COMPLETE_BYTES */) {
      if (this.pending.has(packet.transientId)) {
        return alreadyExistsError(
          "Complete byte packet collides with pending chunks."
        );
      }
      return packet.payload;
    }
    if (packet.sequence > this.options.maxMessageSize) {
      return outOfRangeError(
        "Byte chunk sequence exceeds the configured message bound."
      );
    }
    if (packet.type === 2 /* LENGTH_SUFFIXED_BYTE_CHUNK */ && packet.packetCount > this.options.maxMessageSize + 1) {
      return outOfRangeError(
        "Byte message declares an unreasonable packet count."
      );
    }
    if (this.pendingBytesInternal + packet.payload.byteLength > this.options.maxPendingBytes) {
      return resourceExhaustedError(
        "Pending byte chunks exceed maxPendingBytes."
      );
    }
    let pending = this.pending.get(packet.transientId);
    if (pending === void 0) {
      if (this.pending.size >= this.options.maxPendingMessages) {
        return resourceExhaustedError(
          "Too many byte messages are pending reassembly."
        );
      }
      pending = { packetCount: null, chunks: /* @__PURE__ */ new Map(), byteCount: 0 };
      this.pending.set(packet.transientId, pending);
    }
    if (packet.type === 2 /* LENGTH_SUFFIXED_BYTE_CHUNK */) {
      if (pending.packetCount !== null && pending.packetCount !== packet.packetCount) {
        return invalidArgumentError(
          "Byte message has conflicting packet counts."
        );
      }
      for (const sequence of pending.chunks.keys()) {
        if (sequence >= packet.packetCount) {
          this.dropPending(packet.transientId, pending);
          return outOfRangeError(
            "Byte chunk sequence exceeds the declared packet count."
          );
        }
      }
      pending.packetCount = packet.packetCount;
    }
    if (pending.packetCount !== null && packet.sequence >= pending.packetCount) {
      return outOfRangeError(
        "Byte chunk sequence exceeds the declared packet count."
      );
    }
    if (pending.chunks.has(packet.sequence)) {
      return alreadyExistsError("Duplicate byte chunk sequence.");
    }
    if (pending.byteCount + packet.payload.byteLength > this.options.maxMessageSize) {
      return outOfRangeError(
        "Reassembled byte message exceeds maxMessageSize."
      );
    }
    pending.chunks.set(packet.sequence, packet.payload);
    pending.byteCount += packet.payload.byteLength;
    this.pendingBytesInternal += packet.payload.byteLength;
    if (pending.packetCount === null || pending.chunks.size !== pending.packetCount) {
      return null;
    }
    const parts = [];
    for (let sequence = 0; sequence < pending.packetCount; ++sequence) {
      const part = pending.chunks.get(sequence);
      if (part === void 0) return null;
      parts.push(part);
    }
    const result = concatBytes(parts);
    this.dropPending(packet.transientId, pending);
    return result;
  }
  dropPending(id, pending) {
    this.pendingBytesInternal -= pending.byteCount;
    this.pending.delete(id);
  }
};

// src/channel_wire_stream.ts
var ChannelEndpointRole = /* @__PURE__ */ ((ChannelEndpointRole2) => {
  ChannelEndpointRole2["CLIENT"] = "client";
  ChannelEndpointRole2["SERVER"] = "server";
  ChannelEndpointRole2["EITHER"] = "either";
  return ChannelEndpointRole2;
})(ChannelEndpointRole || {});
function normalizeChannelFramingOptions(options = {}, maxMessageSize = 32 * 1024 * 1024) {
  try {
    return normalizeChannelFramingOptionsUnchecked(options, maxMessageSize);
  } catch (error) {
    return invalidArgumentError(
      "Channel framing options could not be read.",
      [],
      error
    );
  }
}
function normalizeChannelFramingOptionsUnchecked(options, maxMessageSize) {
  const result = {
    splitSize: options.splitSize ?? 64 * 1024,
    maxPendingMessages: options.maxPendingMessages ?? 64,
    maxPendingBytes: options.maxPendingBytes ?? 64 * 1024 * 1024
  };
  if (!Number.isSafeInteger(result.splitSize) || result.splitSize < MINIMUM_BYTE_PACKET_SIZE || result.splitSize > 1024 * 1024) {
    return invalidArgumentError(
      `splitSize must be an integer in [${MINIMUM_BYTE_PACKET_SIZE}, 1048576].`
    );
  }
  if (!Number.isSafeInteger(result.maxPendingMessages) || result.maxPendingMessages <= 0 || !Number.isSafeInteger(result.maxPendingBytes) || result.maxPendingBytes <= 0) {
    return invalidArgumentError(
      "Channel pending reassembly limits must be positive integers."
    );
  }
  if (result.splitSize > maxMessageSize + 9) {
    return invalidArgumentError(
      "splitSize must not exceed maxSingleMessageSize plus metadata."
    );
  }
  return result;
}
function hasBinaryChannelShape(value) {
  if (typeof value !== "object" || value === null) return false;
  try {
    const candidate = value;
    return [
      "setCallbacks",
      "resetCallbacks",
      "open",
      "isOpen",
      "send",
      "bufferedAmount",
      "waitForBufferedAmountLow",
      "close",
      "getImpl"
    ].every((name) => typeof candidate[name] === "function");
  } catch {
    return false;
  }
}
var ChannelWireStream = class _ChannelWireStream {
  constructor(channel, id, role, options, framing, reassembler) {
    this.channel = channel;
    this.id = id;
    this.role = role;
    this.options = { ...options };
    this.framing = Object.freeze({ ...framing });
    this.reassembler = reassembler;
    this.armTiming();
  }
  /** Normalized application-message and timing limits. */
  options;
  /** Normalized packet reassembly limits. */
  framing;
  reassembler;
  started = false;
  opened = false;
  finished = false;
  doneCalled = false;
  onMessage;
  onDone;
  status = okStatus();
  trailers = null;
  localEnd = "none";
  localEndSent = "none";
  remoteHalfClosed = false;
  remoteAborted = false;
  nextMessageId = 0n;
  outgoing = [];
  outgoingPumpRunning = false;
  incoming = [];
  incomingBytes = 0;
  incomingPumpRunning = false;
  drainDone = new Deferred();
  finishedDone = new Deferred();
  deadlineTimer = null;
  activityTimer = null;
  lastActivity = Date.now();
  /**
   * Wrap a binary channel with A11 framing and lifecycle semantics.
   *
   * Construction validates the stream id and all limits without opening the
   * channel. Opening happens later in {@link start} or {@link accept}.
   */
  static create(channel, id, role = "either" /* EITHER */, options = {}, framingOptions = {}) {
    try {
      return _ChannelWireStream.createUnchecked(
        channel,
        id,
        role,
        options,
        framingOptions
      );
    } catch (error) {
      return statusFromUnknown(
        error,
        "Creating ChannelWireStream raised an exception."
      );
    }
  }
  static createUnchecked(channel, id, role, options, framingOptions) {
    if (!hasBinaryChannelShape(channel)) {
      return invalidArgumentError("channel must implement BinaryChannel.");
    }
    const validId = validateName(id);
    if (!isOk(validId)) return validId;
    if (!Object.values(ChannelEndpointRole).includes(role)) {
      return invalidArgumentError("role is not a valid ChannelEndpointRole.");
    }
    const normalized = normalizeWireStreamOptions(options);
    if (!isOk(normalized)) return normalized;
    const framing = normalizeChannelFramingOptions(
      framingOptions,
      normalized.maxSingleMessageSize
    );
    if (!isOk(framing)) return framing;
    const reassembler = ByteReassembler.create({
      packetSize: framing.splitSize,
      maxMessageSize: normalized.maxSingleMessageSize,
      maxPendingMessages: framing.maxPendingMessages,
      maxPendingBytes: framing.maxPendingBytes
    });
    if (!isOk(reassembler)) return reassembler;
    return new _ChannelWireStream(
      channel,
      id,
      role,
      normalized,
      framing,
      reassembler
    );
  }
  send(message) {
    if (!(message instanceof WireMessage)) {
      return invalidArgumentError("message must be a WireMessage.");
    }
    const validation = message.validate();
    if (!isOk(validation)) return validation;
    let end = "none";
    if (message.isHalfClose) {
      const normalized = normalizeWireHeaders(message.headers);
      if (!isOk(normalized)) return normalized;
      message = new WireMessage({ headers: normalized });
      end = normalized.has(ABORT_STATUS_HEADER) ? "abort" : "half-close";
    }
    const bytes = message.toMsgpack();
    if (!isOk(bytes)) return bytes;
    if (bytes.byteLength > this.options.maxSingleMessageSize) {
      return outOfRangeError(
        "Outgoing WireMessage exceeds maxSingleMessageSize."
      );
    }
    if (this.remoteAborted) {
      return failedPreconditionError("The peer aborted the stream.");
    }
    if (this.localEnd !== "none" || this.finished) {
      return failedPreconditionError("This endpoint has already terminated.");
    }
    if (this.deadlineExpired()) {
      this.forceAbort(
        deadlineExceededError("WireStream deadline exceeded."),
        false
      );
      return failedPreconditionError("WireStream deadline exceeded.");
    }
    this.localEnd = end;
    if (end === "abort") {
      this.status = abortedError("The stream was aborted by this endpoint.");
    }
    this.outgoing.push({
      bytes,
      end,
      messageId: this.nextMessageId
    });
    this.nextMessageId = this.nextMessageId + 1n & 0xffffffffffffffffn;
    this.markActivity();
    this.scheduleOutgoingPump();
    return okStatus();
  }
  start(onMessage, onDone) {
    return this.startEndpoint(false, onMessage, onDone);
  }
  accept(onMessage, onDone) {
    return this.startEndpoint(true, onMessage, onDone);
  }
  async startEndpoint(accept, onMessage, onDone) {
    if (onMessage !== void 0 && typeof onMessage !== "function") {
      return invalidArgumentError("onMessage must be callable.");
    }
    if (onDone !== void 0 && typeof onDone !== "function") {
      return invalidArgumentError("onDone must be callable.");
    }
    if (this.started) {
      return failedPreconditionError("WireStream is already started.");
    }
    if (accept && this.role === "client" /* CLIENT */ || !accept && this.role === "server" /* SERVER */) {
      return unimplementedError(
        accept ? "This WireStream cannot accept." : "This WireStream cannot start as a client."
      );
    }
    this.started = true;
    this.onMessage = onMessage;
    this.onDone = onDone;
    this.lastActivity = Date.now();
    const configured = this.configureChannel();
    if (!isOk(configured)) {
      this.finish(configured);
      return configured;
    }
    if (this.deadlineExpired()) {
      const status = deadlineExceededError("WireStream deadline exceeded.");
      this.forceAbort(status, false);
      return status;
    }
    try {
      const opened = await this.channel.open();
      if (!isStatus(opened)) {
        const status = internalError(
          "BinaryChannel.open() returned a non-Status value."
        );
        this.finish(status);
        return status;
      }
      if (!isOk(opened)) {
        this.finish(opened);
        return opened;
      }
      const reportedOpen = this.channel.isOpen();
      if (typeof reportedOpen !== "boolean") {
        const status = internalError(
          "BinaryChannel.isOpen() returned a non-boolean value."
        );
        this.finish(status);
        return status;
      }
      this.opened = reportedOpen;
      if (!this.opened) {
        const status = unavailableError(
          "Binary channel did not report open after startup."
        );
        this.finish(status);
        return status;
      }
      this.markActivity();
      this.scheduleOutgoingPump();
      this.scheduleIncomingPump();
      return okStatus();
    } catch (error) {
      const status = statusFromUnknown(
        error,
        "Binary channel startup raised an exception."
      );
      this.finish(status);
      return status;
    }
  }
  halfClose(trailers) {
    if (this.localEnd !== "none" || this.finished) return okStatus();
    const normalized = normalizeWireHeaders(trailers);
    if (!isOk(normalized)) return normalized;
    return this.send(makeHalfCloseMessage(normalized));
  }
  drainOutgoingMessages() {
    if (this.localEnd !== "half-close") {
      return Promise.resolve(
        failedPreconditionError(
          "drainOutgoingMessages() requires halfClose() first."
        )
      );
    }
    if (this.localEndSent === "half-close") return Promise.resolve(okStatus());
    return this.drainDone.promise;
  }
  abort(status) {
    if (!isStatus(status) || isOk(status)) {
      return invalidArgumentError("Abort status must be non-OK.");
    }
    if (this.localEnd !== "none" || this.finished) return okStatus();
    const packed = packStatus(status);
    if (!isOk(packed)) return packed;
    return this.send(
      makeHalfCloseMessage(/* @__PURE__ */ new Map([[ABORT_STATUS_HEADER, packed]]))
    );
  }
  setDeadline(deadline) {
    const parsed = wireDeadlineMillis(deadline);
    if (!isOk(parsed)) return parsed;
    this.options.deadline = parsed;
    this.armTiming();
    if (this.deadlineExpired() && !this.finished) {
      this.forceAbort(
        deadlineExceededError("WireStream deadline exceeded."),
        true
      );
    }
    return okStatus();
  }
  getDeadline() {
    return this.options.deadline;
  }
  getStatus() {
    if (this.deadlineExpired() && !this.finished) {
      this.forceAbort(
        deadlineExceededError("WireStream deadline exceeded."),
        true
      );
    }
    return this.status;
  }
  getTrailers() {
    return this.trailers === null ? null : copyByteMap(this.trailers);
  }
  getId() {
    return this.id;
  }
  getImpl() {
    try {
      return this.channel.getImpl();
    } catch {
      return null;
    }
  }
  /** Await terminal stream completion, including the done callback. */
  wait() {
    return this.finishedDone.promise;
  }
  configureChannel() {
    try {
      const status = this.channel.setCallbacks({
        onOpen: () => {
          this.opened = true;
          this.markActivity();
          this.scheduleOutgoingPump();
        },
        onMessage: (packet) => this.handlePacket(packet),
        onError: (status2) => this.finish(status2),
        onClosed: () => this.handleChannelClosed(),
        onBufferedAmountLow: () => void 0
      });
      return isStatus(status) ? status : internalError(
        "BinaryChannel.setCallbacks() returned a non-Status value."
      );
    } catch (error) {
      return statusFromUnknown(
        error,
        "Configuring binary channel callbacks raised an exception."
      );
    }
  }
  handlePacket(packet) {
    if (this.finished) return;
    try {
      const complete = this.reassembler.feed(packet);
      if (!isOk(complete)) {
        this.forceAbort(complete, true);
        return;
      }
      if (complete === null) return;
      if (complete.byteLength > this.options.maxSingleMessageSize) {
        this.forceAbort(
          outOfRangeError(
            "Incoming WireMessage exceeds maxSingleMessageSize."
          ),
          true
        );
        return;
      }
      if (this.incoming.length >= this.options.maxBufferedIncomingMessages || this.incoming.length > 0 && this.incomingBytes + complete.byteLength > this.options.maxBufferedIncomingBytes) {
        this.forceAbort(
          resourceExhaustedError(
            "Incoming WireMessage buffer capacity was exceeded."
          ),
          true
        );
        return;
      }
      this.incoming.push({ bytes: complete, receivedAt: Date.now() });
      this.incomingBytes += complete.byteLength;
      this.markActivity();
      this.scheduleIncomingPump();
    } catch (error) {
      this.forceAbort(
        statusFromUnknown(error, "Receiving channel data raised an exception."),
        true
      );
    }
  }
  scheduleOutgoingPump() {
    if (this.outgoingPumpRunning || !this.started || !this.opened || this.finished) {
      return;
    }
    this.outgoingPumpRunning = true;
    queueMicrotask(() => void this.pumpOutgoing());
  }
  async pumpOutgoing() {
    try {
      while (!this.finished && this.started && this.opened) {
        const outbound = this.outgoing.shift();
        if (outbound === void 0) break;
        const packets = splitBytesIntoPackets(
          outbound.bytes,
          outbound.messageId,
          this.framing.splitSize
        );
        if (!isOk(packets)) {
          this.finish(packets);
          return;
        }
        for (const packet of packets) {
          let sent;
          try {
            sent = this.channel.send(packet);
          } catch (error) {
            sent = statusFromUnknown(
              error,
              "Binary channel send raised an exception."
            );
          }
          if (!isStatus(sent)) {
            sent = internalError(
              "BinaryChannel.send() returned a non-Status value."
            );
          }
          if (!isOk(sent)) {
            this.finish(sent);
            return;
          }
        }
        this.markActivity();
        if (outbound.end !== "none") {
          const drained = await this.waitForChannelDrain();
          if (!isOk(drained)) {
            this.finish(drained);
            return;
          }
          this.localEndSent = outbound.end;
          if (outbound.end === "half-close") {
            this.drainDone.resolve(okStatus());
          }
          this.maybeFinish();
          return;
        }
      }
    } catch (error) {
      this.finish(
        statusFromUnknown(error, "WireStream sender raised an exception.")
      );
    } finally {
      this.outgoingPumpRunning = false;
      if (this.outgoing.length > 0) this.scheduleOutgoingPump();
    }
  }
  async waitForChannelDrain() {
    while (!this.finished) {
      let amount;
      try {
        amount = this.channel.bufferedAmount();
      } catch (error) {
        return statusFromUnknown(
          error,
          "Reading binary channel buffered amount raised an exception."
        );
      }
      if (!isOk(amount)) return amount;
      if (!Number.isFinite(amount) || amount < 0) {
        return internalError(
          "BinaryChannel.bufferedAmount() returned an invalid amount."
        );
      }
      if (amount === 0) return okStatus();
      let waited;
      try {
        waited = await this.channel.waitForBufferedAmountLow();
      } catch (error) {
        waited = statusFromUnknown(
          error,
          "Waiting for binary channel drain raised an exception."
        );
      }
      if (!isStatus(waited)) {
        waited = internalError(
          "BinaryChannel.waitForBufferedAmountLow() returned a non-Status value."
        );
      }
      if (!isOk(waited)) return waited;
      const yielded = await sleep(0);
      if (!isOk(yielded)) return yielded;
    }
    return isOk(this.status) ? failedPreconditionError("WireStream finished before transport drain.") : this.status;
  }
  scheduleIncomingPump() {
    if (this.incomingPumpRunning || !this.started || this.finished || this.incoming.length === 0) {
      return;
    }
    this.incomingPumpRunning = true;
    queueMicrotask(() => void this.pumpIncoming());
  }
  async pumpIncoming() {
    try {
      while (!this.finished) {
        const incoming = this.incoming.shift();
        if (incoming === void 0) break;
        this.incomingBytes -= incoming.bytes.byteLength;
        const message = WireMessage.fromMsgpack(incoming.bytes);
        if (!isOk(message)) {
          this.forceAbort(message, true);
          return;
        }
        this.markActivity();
        if (!message.isHalfClose) {
          if (this.remoteHalfClosed || this.remoteAborted) {
            this.forceAbort(
              failedPreconditionError(
                "Peer sent data after a terminal message."
              ),
              true
            );
            return;
          }
          const callbackStatus2 = await this.invokeMessageWithTimeout(message);
          if (!isOk(callbackStatus2)) {
            this.forceAbort(callbackStatus2, true);
            return;
          }
          continue;
        }
        const headers = normalizeWireHeaders(message.headers);
        if (!isOk(headers)) {
          this.forceAbort(headers, true);
          return;
        }
        const abortBytes = headers.get(ABORT_STATUS_HEADER);
        if (abortBytes !== void 0) {
          const decoded = decodeStatus(abortBytes);
          let remoteStatus;
          if (!isOk(decoded)) {
            remoteStatus = decoded;
          } else if (isOk(decoded.status)) {
            remoteStatus = abortedError(decoded.status.message);
          } else {
            remoteStatus = decoded.status;
          }
          this.remoteAborted = true;
          this.trailers = null;
          if (isOk(this.status)) this.status = remoteStatus;
          this.finish();
          return;
        }
        this.remoteHalfClosed = true;
        this.trailers = headers;
        const callbackStatus = await this.invokeMessageWithTimeout(null);
        if (!isOk(callbackStatus)) {
          this.forceAbort(callbackStatus, true);
          return;
        }
        this.maybeFinish();
        return;
      }
    } catch (error) {
      this.forceAbort(
        statusFromUnknown(error, "WireStream receiver raised an exception."),
        true
      );
    } finally {
      this.incomingPumpRunning = false;
      if (this.incoming.length > 0) this.scheduleIncomingPump();
    }
  }
  async invokeMessageWithTimeout(message) {
    const callback = invokeWireCallback(this.onMessage, message);
    const timeoutMs = this.options.messageTimeoutMs;
    if (timeoutMs === null) return callback;
    const timeout = new Deferred();
    const timer = setTimeout(
      () => timeout.resolve(
        deadlineExceededError("Timed out delivering a WireStream message.")
      ),
      timeoutMs
    );
    try {
      return await Promise.race([callback, timeout.promise]);
    } finally {
      clearTimeout(timer);
    }
  }
  forceAbort(status, canCommunicate) {
    if (this.finished || this.remoteAborted || this.localEnd === "abort") return;
    this.status = status;
    this.trailers = null;
    this.localEnd = "abort";
    this.outgoing.splice(0);
    if (canCommunicate && this.opened) {
      const packed = packStatus(status);
      if (isOk(packed)) {
        const message = makeHalfCloseMessage(
          /* @__PURE__ */ new Map([[ABORT_STATUS_HEADER, packed]])
        );
        const bytes = message.toMsgpack();
        if (isOk(bytes)) {
          this.outgoing.push({
            bytes,
            end: "abort",
            messageId: this.nextMessageId++
          });
          this.scheduleOutgoingPump();
          return;
        }
      }
    }
    this.finish();
  }
  maybeFinish() {
    if (this.remoteAborted || this.localEndSent === "abort" || this.localEndSent === "half-close" && this.remoteHalfClosed) {
      this.finish();
    }
  }
  finish(terminalError) {
    if (this.finished) return;
    this.finished = true;
    if (terminalError !== void 0 && isOk(this.status)) {
      this.status = terminalError;
    }
    this.clearTiming();
    this.incoming.splice(0);
    this.incomingBytes = 0;
    this.reassembler.clear();
    if (this.localEnd === "half-close" && this.localEndSent !== "half-close") {
      this.drainDone.resolve(
        isOk(this.status) ? failedPreconditionError(
          "WireStream finished before its half-close was drained."
        ) : this.status
      );
    }
    if (!isOk(this.status)) {
      try {
        this.channel.close();
      } catch {
      }
    }
    if (!this.doneCalled) {
      this.doneCalled = true;
      queueMicrotask(() => void this.invokeDone());
    }
  }
  async invokeDone() {
    const callbackStatus = await invokeWireCallback(this.onDone);
    if (!isOk(callbackStatus) && isOk(this.status)) this.status = callbackStatus;
    let cleanupStatus = okStatus();
    try {
      const closed = this.channel.close();
      cleanupStatus = isStatus(closed) ? closed : internalError("BinaryChannel.close() returned a non-Status value.");
    } catch (error) {
      cleanupStatus = statusFromUnknown(
        error,
        "BinaryChannel.close() raised an exception."
      );
    }
    try {
      const reset = this.channel.resetCallbacks();
      if (!isStatus(reset)) {
        if (isOk(cleanupStatus)) {
          cleanupStatus = internalError(
            "BinaryChannel.resetCallbacks() returned a non-Status value."
          );
        }
      } else if (!isOk(reset) && isOk(cleanupStatus)) {
        cleanupStatus = reset;
      }
    } catch (error) {
      if (isOk(cleanupStatus)) {
        cleanupStatus = statusFromUnknown(
          error,
          "BinaryChannel.resetCallbacks() raised an exception."
        );
      }
    }
    if (!isOk(cleanupStatus) && isOk(this.status)) this.status = cleanupStatus;
    this.finishedDone.resolve(this.status);
  }
  handleChannelClosed() {
    this.opened = false;
    if (this.finished) return;
    const expected = this.remoteAborted || this.localEndSent === "abort" || this.localEndSent === "half-close" && this.remoteHalfClosed;
    if (expected) this.maybeFinish();
    else this.finish(unavailableError("Channel closed before A11 termination."));
  }
  markActivity() {
    this.lastActivity = Date.now();
    this.armTiming();
  }
  deadlineExpired() {
    return this.options.deadline !== null && this.options.deadline <= Date.now();
  }
  armTiming() {
    this.clearTiming();
    if (this.finished) return;
    const now = Date.now();
    if (this.options.deadline !== null) {
      this.deadlineTimer = setTimeout(() => {
        this.forceAbort(
          deadlineExceededError("WireStream deadline exceeded."),
          true
        );
      }, Math.max(0, this.options.deadline - now));
    }
    if (this.options.messageTimeoutMs !== null && this.started) {
      this.activityTimer = setTimeout(() => {
        this.forceAbort(
          deadlineExceededError("Timed out waiting for WireStream activity."),
          true
        );
      }, Math.max(0, this.lastActivity + this.options.messageTimeoutMs - now));
    }
  }
  clearTiming() {
    if (this.deadlineTimer !== null) clearTimeout(this.deadlineTimer);
    if (this.activityTimer !== null) clearTimeout(this.activityTimer);
    this.deadlineTimer = null;
    this.activityTimer = null;
  }
};

// src/websocket_wire_stream.ts
function normalizeWebSocketOptions(options = {}) {
  try {
    if (typeof options !== "object" || options === null) {
      return invalidArgumentError("WebSocket options must be an object.");
    }
    const headers = {};
    for (const [rawName, value] of Object.entries(options.headers ?? {})) {
      const name = rawName.toLowerCase();
      if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name)) {
        return invalidArgumentError(`Invalid WebSocket header name: ${rawName}.`);
      }
      if (typeof value !== "string" || /[\r\n\0]/.test(value)) {
        return invalidArgumentError(
          `WebSocket header ${rawName} must be a safe string.`
        );
      }
      headers[name] = value;
    }
    const protocols = options.protocols;
    if (protocols !== void 0 && typeof protocols !== "string" && (!Array.isArray(protocols) || protocols.some((protocol) => typeof protocol !== "string"))) {
      return invalidArgumentError("protocols must be a string or string array.");
    }
    const maxBufferedAmount = options.maxBufferedAmount ?? 16 * 1024 * 1024;
    const bufferedAmountLowThreshold = options.bufferedAmountLowThreshold ?? 64 * 1024;
    if (!Number.isSafeInteger(maxBufferedAmount) || maxBufferedAmount <= 0) {
      return invalidArgumentError("maxBufferedAmount must be a positive integer.");
    }
    if (!Number.isSafeInteger(bufferedAmountLowThreshold) || bufferedAmountLowThreshold < 0 || bufferedAmountLowThreshold > maxBufferedAmount) {
      return invalidArgumentError(
        "bufferedAmountLowThreshold must be in [0, maxBufferedAmount]."
      );
    }
    if (options.webSocketFactory !== void 0 && typeof options.webSocketFactory !== "function") {
      return invalidArgumentError("webSocketFactory must be callable.");
    }
    return {
      headers: Object.freeze(headers),
      protocols,
      framing: { ...options.framing ?? {} },
      maxBufferedAmount,
      bufferedAmountLowThreshold,
      webSocketFactory: options.webSocketFactory
    };
  } catch (error) {
    return invalidArgumentError("Could not normalize WebSocket options.", [], error);
  }
}
function hasWebSocketShape(value) {
  if (typeof value !== "object" || value === null) return false;
  try {
    const candidate = value;
    return typeof candidate.addEventListener === "function" && typeof candidate.send === "function" && typeof candidate.close === "function";
  } catch {
    return false;
  }
}
function validateWebSocketUrl(url) {
  if (typeof url !== "string") {
    return invalidArgumentError("WebSocket URL must be a string.");
  }
  try {
    const parsed = new URL(url);
    if (parsed.protocol !== "ws:" && parsed.protocol !== "wss:") {
      return invalidArgumentError("WebSocket URL must start with ws:// or wss://.");
    }
    if (!parsed.hostname) {
      return invalidArgumentError("WebSocket URL host must not be empty.");
    }
    return okStatus();
  } catch (error) {
    return invalidArgumentError("WebSocket URL is invalid.", [], error);
  }
}
function hasBrowserWebSocket() {
  return typeof globalThis.WebSocket === "function";
}
var WebSocketBinaryChannel = class {
  constructor(url, options) {
    this.url = url;
    this.options = options;
  }
  callbacks = null;
  socket = null;
  opening = null;
  messageChain = Promise.resolve();
  closed = false;
  lowWaiters = [];
  pollTimer = null;
  setCallbacks(callbacks) {
    this.callbacks = callbacks;
    return okStatus();
  }
  resetCallbacks() {
    this.callbacks = null;
    return okStatus();
  }
  async open() {
    try {
      if (this.isOpen()) return okStatus();
      if (this.opening !== null) return this.opening.promise;
      if (this.closed) {
        return failedPreconditionError("WebSocket channel is closed.");
      }
      this.opening = new Deferred();
      const socket = await this.makeSocket();
      if (!isOk(socket)) {
        this.opening.resolve(socket);
        return socket;
      }
      if (!hasWebSocketShape(socket)) {
        const status = invalidArgumentError(
          "WebSocket factory returned an invalid WebSocket object."
        );
        this.opening.resolve(status);
        return status;
      }
      this.socket = socket;
      socket.binaryType = "arraybuffer";
      socket.addEventListener("open", () => this.handleOpen());
      socket.addEventListener("message", (event) => {
        try {
          this.handleMessage(event.data);
        } catch (error) {
          this.signalCallbackError(error, "Reading WebSocket message event failed.");
        }
      });
      socket.addEventListener("error", () => this.handleError());
      socket.addEventListener("close", (event) => {
        try {
          this.handleClose(event.code ?? 1006, event.reason ?? "");
        } catch (error) {
          this.signalCallbackError(error, "Reading WebSocket close event failed.");
        }
      });
      if (socket.readyState === 1) this.handleOpen();
    } catch (error) {
      const status = statusFromUnknown(
        error,
        "Creating WebSocket raised an exception."
      );
      this.opening?.resolve(status);
    }
    return this.opening?.promise ?? Promise.resolve(
      unavailableError("WebSocket startup did not initialize.")
    );
  }
  isOpen() {
    try {
      return this.socket?.readyState === 1 && !this.closed;
    } catch {
      return false;
    }
  }
  send(packet) {
    try {
      if (!(packet instanceof Uint8Array)) {
        return invalidArgumentError("WebSocket packet must be a Uint8Array.");
      }
      const socket = this.socket;
      if (socket === null || socket.readyState !== 1 || this.closed) {
        return failedPreconditionError("WebSocket is not open.");
      }
      const bufferedAmount = socket.bufferedAmount;
      if (!Number.isFinite(bufferedAmount) || bufferedAmount < 0) {
        return unavailableError("WebSocket reported an invalid bufferedAmount.");
      }
      if (bufferedAmount + packet.byteLength > this.options.maxBufferedAmount) {
        return resourceExhaustedError(
          "WebSocket buffered amount would exceed maxBufferedAmount."
        );
      }
      socket.send(packet);
      this.scheduleLowPoll();
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "WebSocket send raised an exception.");
    }
  }
  bufferedAmount() {
    const socket = this.socket;
    if (socket === null) return failedPreconditionError("WebSocket is not created.");
    try {
      const amount = socket.bufferedAmount;
      return Number.isFinite(amount) && amount >= 0 ? amount : unavailableError("WebSocket reported an invalid bufferedAmount.");
    } catch (error) {
      return statusFromUnknown(
        error,
        "Reading WebSocket bufferedAmount raised an exception."
      );
    }
  }
  waitForBufferedAmountLow() {
    const amount = this.bufferedAmount();
    if (!isOk(amount)) return Promise.resolve(amount);
    if (amount <= this.options.bufferedAmountLowThreshold) {
      return Promise.resolve(okStatus());
    }
    const waiter = new Deferred();
    this.lowWaiters.push(waiter);
    this.scheduleLowPoll();
    return waiter.promise;
  }
  close() {
    try {
      if (this.closed) return okStatus();
      this.closed = true;
      if (this.pollTimer !== null) clearTimeout(this.pollTimer);
      this.pollTimer = null;
      for (const waiter of this.lowWaiters.splice(0)) {
        waiter.resolve(failedPreconditionError("WebSocket was closed."));
      }
      const socket = this.socket;
      if (socket === null || socket.readyState === 3) return okStatus();
      socket.close(1e3, "A11 stream complete");
      return okStatus();
    } catch (error) {
      return statusFromUnknown(error, "WebSocket close raised an exception.");
    }
  }
  getImpl() {
    return this.socket;
  }
  async makeSocket() {
    if (this.options.webSocketFactory !== void 0) {
      try {
        return await this.options.webSocketFactory(
          this.url,
          this.options.protocols,
          this.options.headers
        );
      } catch (error) {
        return statusFromUnknown(
          error,
          "Custom WebSocket factory raised an exception."
        );
      }
    }
    const hasHeaders = Object.keys(this.options.headers).length > 0;
    const isNode = typeof process !== "undefined" && process.versions?.node !== void 0;
    if (hasBrowserWebSocket() && !(isNode && hasHeaders)) {
      if (hasHeaders && !isNode) {
        return unimplementedError(
          "Browser WebSocket does not support custom HTTP headers; use a query parameter or subprotocol."
        );
      }
      try {
        const protocols = this.options.protocols;
        const socket = protocols === void 0 ? new globalThis.WebSocket(this.url) : new globalThis.WebSocket(this.url, protocols);
        return socket;
      } catch (error) {
        return statusFromUnknown(error, "Browser WebSocket construction failed.");
      }
    }
    try {
      const module = await Promise.resolve().then(() => __toESM(require_browser(), 1));
      const protocols = this.options.protocols;
      const socket = protocols === void 0 ? new module.WebSocket(this.url, { headers: this.options.headers }) : new module.WebSocket(
        this.url,
        protocols,
        { headers: this.options.headers }
      );
      return socket;
    } catch (error) {
      return statusFromUnknown(error, "Node.js WebSocket construction failed.");
    }
  }
  handleOpen() {
    if (this.closed) return;
    this.opening?.resolve(okStatus());
    try {
      this.callbacks?.onOpen();
    } catch (error) {
      this.signalCallbackError(error, "WebSocket open callback raised an exception.");
    }
  }
  handleMessage(data) {
    this.messageChain = this.messageChain.then(async () => {
      const source = this.asByteSource(data);
      if (!isOk(source)) {
        this.callbacks?.onError(source);
        return;
      }
      const bytes = await toBytesAsync(source);
      if (!isOk(bytes)) {
        this.callbacks?.onError(bytes);
        return;
      }
      try {
        this.callbacks?.onMessage(bytes);
      } catch (error) {
        this.signalCallbackError(
          error,
          "WebSocket message callback raised an exception."
        );
      }
    }).catch((error) => {
      this.signalCallbackError(error, "WebSocket message processing failed.");
    });
  }
  asByteSource(data) {
    if (data instanceof ArrayBuffer || ArrayBuffer.isView(data) || typeof Blob !== "undefined" && data instanceof Blob) {
      return data;
    }
    return invalidArgumentError("A11 WebSocket messages must be binary.");
  }
  handleError() {
    if (this.closed) return;
    const status = unavailableError("WebSocket transport reported an error.");
    this.opening?.resolve(status);
    try {
      this.callbacks?.onError(status);
    } catch {
    }
  }
  handleClose(code, reason) {
    if (!Number.isInteger(code)) code = 1006;
    if (typeof reason !== "string") reason = "";
    const wasOpening = this.opening !== null && !this.opening.settled;
    this.closed = true;
    if (wasOpening) {
      this.opening?.resolve({
        code: statusCodeFromWebSocket(code),
        message: reason || `WebSocket closed during startup (${code}).`
      });
    }
    for (const waiter of this.lowWaiters.splice(0)) {
      waiter.resolve(unavailableError("WebSocket closed before its buffer drained."));
    }
    try {
      this.callbacks?.onClosed();
    } catch {
    }
  }
  signalCallbackError(error, message) {
    const status = statusFromUnknown(error, message);
    try {
      this.callbacks?.onError(status);
    } catch {
    }
  }
  scheduleLowPoll() {
    if (this.pollTimer !== null || this.lowWaiters.length === 0) return;
    this.pollTimer = setTimeout(() => {
      this.pollTimer = null;
      const amount = this.bufferedAmount();
      if (!isOk(amount)) {
        for (const waiter of this.lowWaiters.splice(0)) waiter.resolve(amount);
        return;
      }
      if (amount <= this.options.bufferedAmountLowThreshold) {
        for (const waiter of this.lowWaiters.splice(0)) waiter.resolve(okStatus());
        try {
          this.callbacks?.onBufferedAmountLow();
        } catch {
        }
        return;
      }
      this.scheduleLowPoll();
    }, 4);
  }
};
var WebSocketWireStream = class _WebSocketWireStream {
  constructor(stream) {
    this.stream = stream;
  }
  /** Create a client endpoint without opening its socket until {@link start}. */
  static createClient(url, options = {}, websocketOptions = {}) {
    try {
      const validUrl = validateWebSocketUrl(url);
      if (!isOk(validUrl)) return validUrl;
      const normalized = normalizeWebSocketOptions(websocketOptions);
      if (!isOk(normalized)) return normalized;
      const channel = new WebSocketBinaryChannel(url, normalized);
      const stream = ChannelWireStream.create(
        channel,
        randomId("ws-"),
        "client" /* CLIENT */,
        options,
        normalized.framing
      );
      return isOk(stream) ? new _WebSocketWireStream(stream) : stream;
    } catch (error) {
      return statusFromUnknown(error, "Creating WebSocketWireStream raised an exception.");
    }
  }
  /** Alias for {@link createClient}; connection still begins in {@link start}. */
  static connect(url, options = {}, websocketOptions = {}) {
    return _WebSocketWireStream.createClient(url, options, websocketOptions);
  }
  send(message) {
    return this.stream.send(message);
  }
  start(onMessage, onDone) {
    return this.stream.start(onMessage, onDone);
  }
  accept(onMessage, onDone) {
    return this.stream.accept(onMessage, onDone);
  }
  halfClose(trailers) {
    return this.stream.halfClose(trailers);
  }
  drainOutgoingMessages() {
    return this.stream.drainOutgoingMessages();
  }
  abort(status) {
    return this.stream.abort(status);
  }
  setDeadline(deadline) {
    return this.stream.setDeadline(deadline);
  }
  getDeadline() {
    return this.stream.getDeadline();
  }
  getStatus() {
    return this.stream.getStatus();
  }
  getTrailers() {
    return this.stream.getTrailers();
  }
  getId() {
    return this.stream.getId();
  }
  getImpl() {
    return this.stream.getImpl();
  }
  wait() {
    return this.stream.wait();
  }
};

// demo/demo_support.ts
var need = (value) => {
  if (!isOk(value)) throw new Error(`${StatusCode[value.code]}: ${value.message}`);
  return value;
};
var READ_TIMEOUT_MS = 3e5;
var DEFAULT_SERVER_URL = "wss://a11.services:9443/a11-demos";
function webSocketUrl(url) {
  return url.trim().replace(/^http(s?):\/\//i, "ws$1://");
}
async function connect(url, registry = new ActionRegistry()) {
  const session = need(Session.create({ actionRegistry: registry, noStreamTimeoutMs: null }));
  const stream = need(WebSocketWireStream.connect(webSocketUrl(url)));
  need(await session.addStream(stream, "start" /* START */));
  return { session, stream };
}
function makeCall(connection, schema) {
  return need(
    Action.create(schema, {
      session: connection.session,
      stream: connection.stream,
      nodeMap: connection.session.getNodeMap()
    })
  );
}
async function readPort(action, port, onValue, timeoutMs = READ_TIMEOUT_MS) {
  const node = need(await action.getOutput(port, false));
  for (; ; ) {
    const next = need(await node.next({ timeoutMs }));
    if (next === null) return;
    onValue(next);
  }
}
var REGISTER_TOOLS_SCHEMA = new ActionSchema({
  name: "__register_tools__",
  description: "Announce the caller's tool schemas for reverse dispatch.",
  inputs: { tools: new ActionPortSchema({ name: "tools", type: "application/json", required: true }) },
  outputs: { ok: new ActionPortSchema({ name: "ok", type: "application/json", required: true }) }
});
function addLine(container, text, kind = "") {
  const line = document.createElement("div");
  line.className = `a11-log-line ${kind}`.trim();
  line.textContent = text;
  container.append(line);
  container.scrollTop = container.scrollHeight;
}
function showError(region, error) {
  region.textContent = error instanceof Error ? error.message : String(error);
}
async function whileBusy(form, work) {
  const controls = [...form.elements];
  const wasDisabled = controls.map((control) => control.disabled);
  for (const control of controls) control.disabled = true;
  try {
    return await work();
  } finally {
    controls.forEach((control, index) => {
      control.disabled = wasDisabled[index] ?? false;
    });
  }
}

// demo/generative_media.ts
var TEXT_TO_IMAGE_SCHEMA = new ActionSchema({
  name: "text_to_image",
  description: "Draw an image from a prompt, reporting progress as it goes.",
  inputs: {
    request: new ActionPortSchema({ name: "request", type: "application/json", unary: true, required: true })
  },
  outputs: {
    image: new ActionPortSchema({ name: "image", type: "image/png", unary: true, required: true }),
    progress: new ActionPortSchema({ name: "progress", type: "application/json" })
  }
});
var GenerativeMediaDemo = class {
  server = document.querySelector("#media-server");
  steps = document.querySelector("#media-steps");
  seed = document.querySelector("#media-seed");
  errors = document.querySelector("#media-errors");
  bar = document.querySelector("#media-progress");
  status = document.querySelector("#media-status");
  image = document.querySelector("#media-image");
  objectUrl = null;
  async draw(prompt) {
    this.errors.textContent = "";
    this.status.replaceChildren();
    const steps = Number(this.steps.value) || 20;
    this.bar.max = steps;
    this.bar.value = 0;
    try {
      const connection = await connect(this.server.value.trim() || DEFAULT_SERVER_URL);
      const call = makeCall(connection, TEXT_TO_IMAGE_SCHEMA);
      need(await call.call());
      const request = need(await call.getInput("request"));
      const seed = this.seed.value.trim();
      need(
        await request.putFinal({
          prompt,
          num_inference_steps: steps,
          ...seed ? { seed: Number(seed) } : {}
        })
      );
      need(await request.drainAndClose());
      const progress = readPort(call, "progress", (value) => {
        const step = value;
        this.bar.value = step.step;
        this.status.textContent = `step ${step.step} of ${step.steps}`;
      });
      const node = need(await call.getOutput("image", false));
      const chunk = need(await node.nextChunk(9e5));
      await progress;
      need(await call.wait(6e4));
      if (chunk === null) {
        addLine(this.status, "The backend produced no image.");
        return;
      }
      if (this.objectUrl !== null) URL.revokeObjectURL(this.objectUrl);
      this.objectUrl = URL.createObjectURL(
        new Blob([chunk.data], { type: chunk.mimetype || "image/png" })
      );
      this.image.src = this.objectUrl;
      this.image.hidden = false;
      this.status.textContent = `${chunk.data.byteLength} bytes of ${chunk.mimetype}`;
      connection.session.halfClose();
    } catch (error) {
      showError(this.errors, error);
    }
  }
};
var root = document.querySelector("#media-demo");
if (root) {
  const demo = new GenerativeMediaDemo();
  const form = document.querySelector("#media-form");
  const input = document.querySelector("#media-prompt");
  form.onsubmit = (event) => {
    event.preventDefault();
    const prompt = input.value.trim();
    if (!prompt) return;
    void whileBusy(form, () => demo.draw(prompt));
  };
}

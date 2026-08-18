var __create = Object.create;
var __defProp = Object.defineProperty;
var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
var __getOwnPropNames = Object.getOwnPropertyNames;
var __getProtoOf = Object.getPrototypeOf;
var __hasOwnProp = Object.prototype.hasOwnProperty;
var __commonJS = (cb, mod) => function __require() {
  return mod || (0, cb[__getOwnPropNames(cb)[0]])((mod = { exports: {} }).exports, mod), mod.exports;
};
var __export = (target, all) => {
  for (var name in all)
    __defProp(target, name, { get: all[name], enumerable: true });
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

// node_modules/zod/v4/classic/external.js
var external_exports = {};
__export(external_exports, {
  $brand: () => $brand,
  $input: () => $input,
  $output: () => $output,
  NEVER: () => NEVER,
  TimePrecision: () => TimePrecision,
  ZodAny: () => ZodAny,
  ZodArray: () => ZodArray,
  ZodBase64: () => ZodBase64,
  ZodBase64URL: () => ZodBase64URL,
  ZodBigInt: () => ZodBigInt,
  ZodBigIntFormat: () => ZodBigIntFormat,
  ZodBoolean: () => ZodBoolean,
  ZodCIDRv4: () => ZodCIDRv4,
  ZodCIDRv6: () => ZodCIDRv6,
  ZodCUID: () => ZodCUID,
  ZodCUID2: () => ZodCUID2,
  ZodCatch: () => ZodCatch,
  ZodCodec: () => ZodCodec,
  ZodCustom: () => ZodCustom,
  ZodCustomStringFormat: () => ZodCustomStringFormat,
  ZodDate: () => ZodDate,
  ZodDefault: () => ZodDefault,
  ZodDiscriminatedUnion: () => ZodDiscriminatedUnion,
  ZodE164: () => ZodE164,
  ZodEmail: () => ZodEmail,
  ZodEmoji: () => ZodEmoji,
  ZodEnum: () => ZodEnum,
  ZodError: () => ZodError,
  ZodExactOptional: () => ZodExactOptional,
  ZodFile: () => ZodFile,
  ZodFirstPartyTypeKind: () => ZodFirstPartyTypeKind,
  ZodFunction: () => ZodFunction,
  ZodGUID: () => ZodGUID,
  ZodIPv4: () => ZodIPv4,
  ZodIPv6: () => ZodIPv6,
  ZodISODate: () => ZodISODate,
  ZodISODateTime: () => ZodISODateTime,
  ZodISODuration: () => ZodISODuration,
  ZodISOTime: () => ZodISOTime,
  ZodIntersection: () => ZodIntersection,
  ZodIssueCode: () => ZodIssueCode,
  ZodJWT: () => ZodJWT,
  ZodKSUID: () => ZodKSUID,
  ZodLazy: () => ZodLazy,
  ZodLiteral: () => ZodLiteral,
  ZodMAC: () => ZodMAC,
  ZodMap: () => ZodMap,
  ZodNaN: () => ZodNaN,
  ZodNanoID: () => ZodNanoID,
  ZodNever: () => ZodNever,
  ZodNonOptional: () => ZodNonOptional,
  ZodNull: () => ZodNull,
  ZodNullable: () => ZodNullable,
  ZodNumber: () => ZodNumber,
  ZodNumberFormat: () => ZodNumberFormat,
  ZodObject: () => ZodObject,
  ZodOptional: () => ZodOptional,
  ZodPipe: () => ZodPipe,
  ZodPrefault: () => ZodPrefault,
  ZodPreprocess: () => ZodPreprocess,
  ZodPromise: () => ZodPromise,
  ZodReadonly: () => ZodReadonly,
  ZodRealError: () => ZodRealError,
  ZodRecord: () => ZodRecord,
  ZodSet: () => ZodSet,
  ZodString: () => ZodString,
  ZodStringFormat: () => ZodStringFormat,
  ZodSuccess: () => ZodSuccess,
  ZodSymbol: () => ZodSymbol,
  ZodTemplateLiteral: () => ZodTemplateLiteral,
  ZodTransform: () => ZodTransform,
  ZodTuple: () => ZodTuple,
  ZodType: () => ZodType,
  ZodULID: () => ZodULID,
  ZodURL: () => ZodURL,
  ZodUUID: () => ZodUUID,
  ZodUndefined: () => ZodUndefined,
  ZodUnion: () => ZodUnion,
  ZodUnknown: () => ZodUnknown,
  ZodVoid: () => ZodVoid,
  ZodXID: () => ZodXID,
  ZodXor: () => ZodXor,
  _ZodString: () => _ZodString,
  _default: () => _default2,
  _function: () => _function,
  any: () => any,
  array: () => array,
  base64: () => base642,
  base64url: () => base64url2,
  bigint: () => bigint2,
  boolean: () => boolean2,
  catch: () => _catch2,
  check: () => check,
  cidrv4: () => cidrv42,
  cidrv6: () => cidrv62,
  clone: () => clone,
  codec: () => codec,
  coerce: () => coerce_exports,
  config: () => config,
  core: () => core_exports2,
  cuid: () => cuid3,
  cuid2: () => cuid22,
  custom: () => custom,
  date: () => date3,
  decode: () => decode2,
  decodeAsync: () => decodeAsync2,
  describe: () => describe2,
  discriminatedUnion: () => discriminatedUnion,
  e164: () => e1642,
  email: () => email2,
  emoji: () => emoji2,
  encode: () => encode2,
  encodeAsync: () => encodeAsync2,
  endsWith: () => _endsWith,
  enum: () => _enum2,
  exactOptional: () => exactOptional,
  file: () => file,
  flattenError: () => flattenError,
  float32: () => float32,
  float64: () => float64,
  formatError: () => formatError,
  fromJSONSchema: () => fromJSONSchema,
  function: () => _function,
  getErrorMap: () => getErrorMap,
  globalRegistry: () => globalRegistry,
  gt: () => _gt,
  gte: () => _gte,
  guid: () => guid2,
  hash: () => hash,
  hex: () => hex2,
  hostname: () => hostname2,
  httpUrl: () => httpUrl,
  includes: () => _includes,
  instanceof: () => _instanceof,
  int: () => int,
  int32: () => int32,
  int64: () => int64,
  intersection: () => intersection,
  invertCodec: () => invertCodec,
  ipv4: () => ipv42,
  ipv6: () => ipv62,
  iso: () => iso_exports,
  json: () => json,
  jwt: () => jwt,
  keyof: () => keyof,
  ksuid: () => ksuid2,
  lazy: () => lazy,
  length: () => _length,
  literal: () => literal,
  locales: () => locales_exports,
  looseObject: () => looseObject,
  looseRecord: () => looseRecord,
  lowercase: () => _lowercase,
  lt: () => _lt,
  lte: () => _lte,
  mac: () => mac2,
  map: () => map,
  maxLength: () => _maxLength,
  maxSize: () => _maxSize,
  meta: () => meta2,
  mime: () => _mime,
  minLength: () => _minLength,
  minSize: () => _minSize,
  multipleOf: () => _multipleOf,
  nan: () => nan,
  nanoid: () => nanoid2,
  nativeEnum: () => nativeEnum,
  negative: () => _negative,
  never: () => never,
  nonnegative: () => _nonnegative,
  nonoptional: () => nonoptional,
  nonpositive: () => _nonpositive,
  normalize: () => _normalize,
  null: () => _null3,
  nullable: () => nullable,
  nullish: () => nullish2,
  number: () => number2,
  object: () => object,
  optional: () => optional,
  overwrite: () => _overwrite,
  parse: () => parse2,
  parseAsync: () => parseAsync2,
  partialRecord: () => partialRecord,
  pipe: () => pipe,
  positive: () => _positive,
  prefault: () => prefault,
  preprocess: () => preprocess,
  prettifyError: () => prettifyError,
  promise: () => promise,
  property: () => _property,
  readonly: () => readonly,
  record: () => record,
  refine: () => refine,
  regex: () => _regex,
  regexes: () => regexes_exports,
  registry: () => registry,
  safeDecode: () => safeDecode2,
  safeDecodeAsync: () => safeDecodeAsync2,
  safeEncode: () => safeEncode2,
  safeEncodeAsync: () => safeEncodeAsync2,
  safeParse: () => safeParse2,
  safeParseAsync: () => safeParseAsync2,
  set: () => set,
  setErrorMap: () => setErrorMap,
  size: () => _size,
  slugify: () => _slugify,
  startsWith: () => _startsWith,
  strictObject: () => strictObject,
  string: () => string2,
  stringFormat: () => stringFormat,
  stringbool: () => stringbool,
  success: () => success,
  superRefine: () => superRefine,
  symbol: () => symbol,
  templateLiteral: () => templateLiteral,
  toJSONSchema: () => toJSONSchema,
  toLowerCase: () => _toLowerCase,
  toUpperCase: () => _toUpperCase,
  transform: () => transform,
  treeifyError: () => treeifyError,
  trim: () => _trim,
  tuple: () => tuple,
  uint32: () => uint32,
  uint64: () => uint64,
  ulid: () => ulid2,
  undefined: () => _undefined3,
  union: () => union,
  unknown: () => unknown,
  uppercase: () => _uppercase,
  url: () => url,
  util: () => util_exports,
  uuid: () => uuid2,
  uuidv4: () => uuidv4,
  uuidv6: () => uuidv6,
  uuidv7: () => uuidv7,
  void: () => _void2,
  xid: () => xid2,
  xor: () => xor
});

// node_modules/zod/v4/core/index.js
var core_exports2 = {};
__export(core_exports2, {
  $ZodAny: () => $ZodAny,
  $ZodArray: () => $ZodArray,
  $ZodAsyncError: () => $ZodAsyncError,
  $ZodBase64: () => $ZodBase64,
  $ZodBase64URL: () => $ZodBase64URL,
  $ZodBigInt: () => $ZodBigInt,
  $ZodBigIntFormat: () => $ZodBigIntFormat,
  $ZodBoolean: () => $ZodBoolean,
  $ZodCIDRv4: () => $ZodCIDRv4,
  $ZodCIDRv6: () => $ZodCIDRv6,
  $ZodCUID: () => $ZodCUID,
  $ZodCUID2: () => $ZodCUID2,
  $ZodCatch: () => $ZodCatch,
  $ZodCheck: () => $ZodCheck,
  $ZodCheckBigIntFormat: () => $ZodCheckBigIntFormat,
  $ZodCheckEndsWith: () => $ZodCheckEndsWith,
  $ZodCheckGreaterThan: () => $ZodCheckGreaterThan,
  $ZodCheckIncludes: () => $ZodCheckIncludes,
  $ZodCheckLengthEquals: () => $ZodCheckLengthEquals,
  $ZodCheckLessThan: () => $ZodCheckLessThan,
  $ZodCheckLowerCase: () => $ZodCheckLowerCase,
  $ZodCheckMaxLength: () => $ZodCheckMaxLength,
  $ZodCheckMaxSize: () => $ZodCheckMaxSize,
  $ZodCheckMimeType: () => $ZodCheckMimeType,
  $ZodCheckMinLength: () => $ZodCheckMinLength,
  $ZodCheckMinSize: () => $ZodCheckMinSize,
  $ZodCheckMultipleOf: () => $ZodCheckMultipleOf,
  $ZodCheckNumberFormat: () => $ZodCheckNumberFormat,
  $ZodCheckOverwrite: () => $ZodCheckOverwrite,
  $ZodCheckProperty: () => $ZodCheckProperty,
  $ZodCheckRegex: () => $ZodCheckRegex,
  $ZodCheckSizeEquals: () => $ZodCheckSizeEquals,
  $ZodCheckStartsWith: () => $ZodCheckStartsWith,
  $ZodCheckStringFormat: () => $ZodCheckStringFormat,
  $ZodCheckUpperCase: () => $ZodCheckUpperCase,
  $ZodCodec: () => $ZodCodec,
  $ZodCustom: () => $ZodCustom,
  $ZodCustomStringFormat: () => $ZodCustomStringFormat,
  $ZodDate: () => $ZodDate,
  $ZodDefault: () => $ZodDefault,
  $ZodDiscriminatedUnion: () => $ZodDiscriminatedUnion,
  $ZodE164: () => $ZodE164,
  $ZodEmail: () => $ZodEmail,
  $ZodEmoji: () => $ZodEmoji,
  $ZodEncodeError: () => $ZodEncodeError,
  $ZodEnum: () => $ZodEnum,
  $ZodError: () => $ZodError,
  $ZodExactOptional: () => $ZodExactOptional,
  $ZodFile: () => $ZodFile,
  $ZodFunction: () => $ZodFunction,
  $ZodGUID: () => $ZodGUID,
  $ZodIPv4: () => $ZodIPv4,
  $ZodIPv6: () => $ZodIPv6,
  $ZodISODate: () => $ZodISODate,
  $ZodISODateTime: () => $ZodISODateTime,
  $ZodISODuration: () => $ZodISODuration,
  $ZodISOTime: () => $ZodISOTime,
  $ZodIntersection: () => $ZodIntersection,
  $ZodJWT: () => $ZodJWT,
  $ZodKSUID: () => $ZodKSUID,
  $ZodLazy: () => $ZodLazy,
  $ZodLiteral: () => $ZodLiteral,
  $ZodMAC: () => $ZodMAC,
  $ZodMap: () => $ZodMap,
  $ZodNaN: () => $ZodNaN,
  $ZodNanoID: () => $ZodNanoID,
  $ZodNever: () => $ZodNever,
  $ZodNonOptional: () => $ZodNonOptional,
  $ZodNull: () => $ZodNull,
  $ZodNullable: () => $ZodNullable,
  $ZodNumber: () => $ZodNumber,
  $ZodNumberFormat: () => $ZodNumberFormat,
  $ZodObject: () => $ZodObject,
  $ZodObjectJIT: () => $ZodObjectJIT,
  $ZodOptional: () => $ZodOptional,
  $ZodPipe: () => $ZodPipe,
  $ZodPrefault: () => $ZodPrefault,
  $ZodPreprocess: () => $ZodPreprocess,
  $ZodPromise: () => $ZodPromise,
  $ZodReadonly: () => $ZodReadonly,
  $ZodRealError: () => $ZodRealError,
  $ZodRecord: () => $ZodRecord,
  $ZodRegistry: () => $ZodRegistry,
  $ZodSet: () => $ZodSet,
  $ZodString: () => $ZodString,
  $ZodStringFormat: () => $ZodStringFormat,
  $ZodSuccess: () => $ZodSuccess,
  $ZodSymbol: () => $ZodSymbol,
  $ZodTemplateLiteral: () => $ZodTemplateLiteral,
  $ZodTransform: () => $ZodTransform,
  $ZodTuple: () => $ZodTuple,
  $ZodType: () => $ZodType,
  $ZodULID: () => $ZodULID,
  $ZodURL: () => $ZodURL,
  $ZodUUID: () => $ZodUUID,
  $ZodUndefined: () => $ZodUndefined,
  $ZodUnion: () => $ZodUnion,
  $ZodUnknown: () => $ZodUnknown,
  $ZodVoid: () => $ZodVoid,
  $ZodXID: () => $ZodXID,
  $ZodXor: () => $ZodXor,
  $brand: () => $brand,
  $constructor: () => $constructor,
  $input: () => $input,
  $output: () => $output,
  Doc: () => Doc,
  JSONSchema: () => json_schema_exports,
  JSONSchemaGenerator: () => JSONSchemaGenerator,
  NEVER: () => NEVER,
  TimePrecision: () => TimePrecision,
  _any: () => _any,
  _array: () => _array,
  _base64: () => _base64,
  _base64url: () => _base64url,
  _bigint: () => _bigint,
  _boolean: () => _boolean,
  _catch: () => _catch,
  _check: () => _check,
  _cidrv4: () => _cidrv4,
  _cidrv6: () => _cidrv6,
  _coercedBigint: () => _coercedBigint,
  _coercedBoolean: () => _coercedBoolean,
  _coercedDate: () => _coercedDate,
  _coercedNumber: () => _coercedNumber,
  _coercedString: () => _coercedString,
  _cuid: () => _cuid,
  _cuid2: () => _cuid2,
  _custom: () => _custom,
  _date: () => _date,
  _decode: () => _decode,
  _decodeAsync: () => _decodeAsync,
  _default: () => _default,
  _discriminatedUnion: () => _discriminatedUnion,
  _e164: () => _e164,
  _email: () => _email,
  _emoji: () => _emoji2,
  _encode: () => _encode,
  _encodeAsync: () => _encodeAsync,
  _endsWith: () => _endsWith,
  _enum: () => _enum,
  _file: () => _file,
  _float32: () => _float32,
  _float64: () => _float64,
  _gt: () => _gt,
  _gte: () => _gte,
  _guid: () => _guid,
  _includes: () => _includes,
  _int: () => _int,
  _int32: () => _int32,
  _int64: () => _int64,
  _intersection: () => _intersection,
  _ipv4: () => _ipv4,
  _ipv6: () => _ipv6,
  _isoDate: () => _isoDate,
  _isoDateTime: () => _isoDateTime,
  _isoDuration: () => _isoDuration,
  _isoTime: () => _isoTime,
  _jwt: () => _jwt,
  _ksuid: () => _ksuid,
  _lazy: () => _lazy,
  _length: () => _length,
  _literal: () => _literal,
  _lowercase: () => _lowercase,
  _lt: () => _lt,
  _lte: () => _lte,
  _mac: () => _mac,
  _map: () => _map,
  _max: () => _lte,
  _maxLength: () => _maxLength,
  _maxSize: () => _maxSize,
  _mime: () => _mime,
  _min: () => _gte,
  _minLength: () => _minLength,
  _minSize: () => _minSize,
  _multipleOf: () => _multipleOf,
  _nan: () => _nan,
  _nanoid: () => _nanoid,
  _nativeEnum: () => _nativeEnum,
  _negative: () => _negative,
  _never: () => _never,
  _nonnegative: () => _nonnegative,
  _nonoptional: () => _nonoptional,
  _nonpositive: () => _nonpositive,
  _normalize: () => _normalize,
  _null: () => _null2,
  _nullable: () => _nullable,
  _number: () => _number,
  _optional: () => _optional,
  _overwrite: () => _overwrite,
  _parse: () => _parse,
  _parseAsync: () => _parseAsync,
  _pipe: () => _pipe,
  _positive: () => _positive,
  _promise: () => _promise,
  _property: () => _property,
  _readonly: () => _readonly,
  _record: () => _record,
  _refine: () => _refine,
  _regex: () => _regex,
  _safeDecode: () => _safeDecode,
  _safeDecodeAsync: () => _safeDecodeAsync,
  _safeEncode: () => _safeEncode,
  _safeEncodeAsync: () => _safeEncodeAsync,
  _safeParse: () => _safeParse,
  _safeParseAsync: () => _safeParseAsync,
  _set: () => _set,
  _size: () => _size,
  _slugify: () => _slugify,
  _startsWith: () => _startsWith,
  _string: () => _string,
  _stringFormat: () => _stringFormat,
  _stringbool: () => _stringbool,
  _success: () => _success,
  _superRefine: () => _superRefine,
  _symbol: () => _symbol,
  _templateLiteral: () => _templateLiteral,
  _toLowerCase: () => _toLowerCase,
  _toUpperCase: () => _toUpperCase,
  _transform: () => _transform,
  _trim: () => _trim,
  _tuple: () => _tuple,
  _uint32: () => _uint32,
  _uint64: () => _uint64,
  _ulid: () => _ulid,
  _undefined: () => _undefined2,
  _union: () => _union,
  _unknown: () => _unknown,
  _uppercase: () => _uppercase,
  _url: () => _url,
  _uuid: () => _uuid,
  _uuidv4: () => _uuidv4,
  _uuidv6: () => _uuidv6,
  _uuidv7: () => _uuidv7,
  _void: () => _void,
  _xid: () => _xid,
  _xor: () => _xor,
  clone: () => clone,
  config: () => config,
  createStandardJSONSchemaMethod: () => createStandardJSONSchemaMethod,
  createToJSONSchemaMethod: () => createToJSONSchemaMethod,
  decode: () => decode,
  decodeAsync: () => decodeAsync,
  describe: () => describe,
  encode: () => encode,
  encodeAsync: () => encodeAsync,
  extractDefs: () => extractDefs,
  finalize: () => finalize,
  flattenError: () => flattenError,
  formatError: () => formatError,
  globalConfig: () => globalConfig,
  globalRegistry: () => globalRegistry,
  initializeContext: () => initializeContext,
  isValidBase64: () => isValidBase64,
  isValidBase64URL: () => isValidBase64URL,
  isValidJWT: () => isValidJWT,
  locales: () => locales_exports,
  meta: () => meta,
  parse: () => parse,
  parseAsync: () => parseAsync,
  prettifyError: () => prettifyError,
  process: () => process2,
  regexes: () => regexes_exports,
  registry: () => registry,
  safeDecode: () => safeDecode,
  safeDecodeAsync: () => safeDecodeAsync,
  safeEncode: () => safeEncode,
  safeEncodeAsync: () => safeEncodeAsync,
  safeParse: () => safeParse,
  safeParseAsync: () => safeParseAsync,
  toDotPath: () => toDotPath,
  toJSONSchema: () => toJSONSchema,
  treeifyError: () => treeifyError,
  util: () => util_exports,
  version: () => version
});

// node_modules/zod/v4/core/core.js
var _a;
var NEVER = /* @__PURE__ */ Object.freeze({
  status: "aborted"
});
// @__NO_SIDE_EFFECTS__
function $constructor(name, initializer3, params) {
  function init(inst, def) {
    if (!inst._zod) {
      Object.defineProperty(inst, "_zod", {
        value: {
          def,
          constr: _,
          traits: /* @__PURE__ */ new Set()
        },
        enumerable: false
      });
    }
    if (inst._zod.traits.has(name)) {
      return;
    }
    inst._zod.traits.add(name);
    initializer3(inst, def);
    const proto = _.prototype;
    const keys = Object.keys(proto);
    for (let i = 0; i < keys.length; i++) {
      const k = keys[i];
      if (!(k in inst)) {
        inst[k] = proto[k].bind(inst);
      }
    }
  }
  const Parent = params?.Parent ?? Object;
  class Definition extends Parent {
  }
  Object.defineProperty(Definition, "name", { value: name });
  function _(def) {
    var _a3;
    const inst = params?.Parent ? new Definition() : this;
    init(inst, def);
    (_a3 = inst._zod).deferred ?? (_a3.deferred = []);
    for (const fn of inst._zod.deferred) {
      fn();
    }
    return inst;
  }
  Object.defineProperty(_, "init", { value: init });
  Object.defineProperty(_, Symbol.hasInstance, {
    value: (inst) => {
      if (params?.Parent && inst instanceof params.Parent)
        return true;
      return inst?._zod?.traits?.has(name);
    }
  });
  Object.defineProperty(_, "name", { value: name });
  return _;
}
var $brand = Symbol("zod_brand");
var $ZodAsyncError = class extends Error {
  constructor() {
    super(`Encountered Promise during synchronous parse. Use .parseAsync() instead.`);
  }
};
var $ZodEncodeError = class extends Error {
  constructor(name) {
    super(`Encountered unidirectional transform during encode: ${name}`);
    this.name = "ZodEncodeError";
  }
};
(_a = globalThis).__zod_globalConfig ?? (_a.__zod_globalConfig = {});
var globalConfig = globalThis.__zod_globalConfig;
function config(newConfig) {
  if (newConfig)
    Object.assign(globalConfig, newConfig);
  return globalConfig;
}

// node_modules/zod/v4/core/util.js
var util_exports = {};
__export(util_exports, {
  BIGINT_FORMAT_RANGES: () => BIGINT_FORMAT_RANGES,
  Class: () => Class,
  NUMBER_FORMAT_RANGES: () => NUMBER_FORMAT_RANGES,
  aborted: () => aborted,
  allowsEval: () => allowsEval,
  assert: () => assert,
  assertEqual: () => assertEqual,
  assertIs: () => assertIs,
  assertNever: () => assertNever,
  assertNotEqual: () => assertNotEqual,
  assignProp: () => assignProp,
  base64ToUint8Array: () => base64ToUint8Array,
  base64urlToUint8Array: () => base64urlToUint8Array,
  cached: () => cached,
  captureStackTrace: () => captureStackTrace,
  cleanEnum: () => cleanEnum,
  cleanRegex: () => cleanRegex,
  clone: () => clone,
  cloneDef: () => cloneDef,
  createTransparentProxy: () => createTransparentProxy,
  defineLazy: () => defineLazy,
  esc: () => esc,
  escapeRegex: () => escapeRegex,
  explicitlyAborted: () => explicitlyAborted,
  extend: () => extend,
  finalizeIssue: () => finalizeIssue,
  floatSafeRemainder: () => floatSafeRemainder,
  getElementAtPath: () => getElementAtPath,
  getEnumValues: () => getEnumValues,
  getLengthableOrigin: () => getLengthableOrigin,
  getParsedType: () => getParsedType,
  getSizableOrigin: () => getSizableOrigin,
  hexToUint8Array: () => hexToUint8Array,
  isObject: () => isObject,
  isPlainObject: () => isPlainObject,
  issue: () => issue,
  joinValues: () => joinValues,
  jsonStringifyReplacer: () => jsonStringifyReplacer,
  merge: () => merge,
  mergeDefs: () => mergeDefs,
  normalizeParams: () => normalizeParams,
  nullish: () => nullish,
  numKeys: () => numKeys,
  objectClone: () => objectClone,
  omit: () => omit,
  optionalKeys: () => optionalKeys,
  parsedType: () => parsedType,
  partial: () => partial,
  pick: () => pick,
  prefixIssues: () => prefixIssues,
  primitiveTypes: () => primitiveTypes,
  promiseAllObject: () => promiseAllObject,
  propertyKeyTypes: () => propertyKeyTypes,
  randomString: () => randomString,
  required: () => required,
  safeExtend: () => safeExtend,
  shallowClone: () => shallowClone,
  slugify: () => slugify,
  stringifyPrimitive: () => stringifyPrimitive,
  uint8ArrayToBase64: () => uint8ArrayToBase64,
  uint8ArrayToBase64url: () => uint8ArrayToBase64url,
  uint8ArrayToHex: () => uint8ArrayToHex,
  unwrapMessage: () => unwrapMessage
});
function assertEqual(val) {
  return val;
}
function assertNotEqual(val) {
  return val;
}
function assertIs(_arg) {
}
function assertNever(_x) {
  throw new Error("Unexpected value in exhaustive check");
}
function assert(_) {
}
function getEnumValues(entries) {
  const numericValues = Object.values(entries).filter((v) => typeof v === "number");
  const values = Object.entries(entries).filter(([k, _]) => numericValues.indexOf(+k) === -1).map(([_, v]) => v);
  return values;
}
function joinValues(array2, separator = "|") {
  return array2.map((val) => stringifyPrimitive(val)).join(separator);
}
function jsonStringifyReplacer(_, value) {
  if (typeof value === "bigint")
    return value.toString();
  return value;
}
function cached(getter) {
  const set2 = false;
  return {
    get value() {
      if (!set2) {
        const value = getter();
        Object.defineProperty(this, "value", { value });
        return value;
      }
      throw new Error("cached value already set");
    }
  };
}
function nullish(input) {
  return input === null || input === void 0;
}
function cleanRegex(source) {
  const start = source.startsWith("^") ? 1 : 0;
  const end = source.endsWith("$") ? source.length - 1 : source.length;
  return source.slice(start, end);
}
function floatSafeRemainder(val, step) {
  const ratio = val / step;
  const roundedRatio = Math.round(ratio);
  const tolerance = Number.EPSILON * Math.max(Math.abs(ratio), 1);
  if (Math.abs(ratio - roundedRatio) < tolerance)
    return 0;
  return ratio - roundedRatio;
}
var EVALUATING = /* @__PURE__ */ Symbol("evaluating");
function defineLazy(object2, key, getter) {
  let value = void 0;
  Object.defineProperty(object2, key, {
    get() {
      if (value === EVALUATING) {
        return void 0;
      }
      if (value === void 0) {
        value = EVALUATING;
        value = getter();
      }
      return value;
    },
    set(v) {
      Object.defineProperty(object2, key, {
        value: v
        // configurable: true,
      });
    },
    configurable: true
  });
}
function objectClone(obj) {
  return Object.create(Object.getPrototypeOf(obj), Object.getOwnPropertyDescriptors(obj));
}
function assignProp(target, prop, value) {
  Object.defineProperty(target, prop, {
    value,
    writable: true,
    enumerable: true,
    configurable: true
  });
}
function mergeDefs(...defs) {
  const mergedDescriptors = {};
  for (const def of defs) {
    const descriptors = Object.getOwnPropertyDescriptors(def);
    Object.assign(mergedDescriptors, descriptors);
  }
  return Object.defineProperties({}, mergedDescriptors);
}
function cloneDef(schema) {
  return mergeDefs(schema._zod.def);
}
function getElementAtPath(obj, path) {
  if (!path)
    return obj;
  return path.reduce((acc, key) => acc?.[key], obj);
}
function promiseAllObject(promisesObj) {
  const keys = Object.keys(promisesObj);
  const promises = keys.map((key) => promisesObj[key]);
  return Promise.all(promises).then((results) => {
    const resolvedObj = {};
    for (let i = 0; i < keys.length; i++) {
      resolvedObj[keys[i]] = results[i];
    }
    return resolvedObj;
  });
}
function randomString(length = 10) {
  const chars = "abcdefghijklmnopqrstuvwxyz";
  let str = "";
  for (let i = 0; i < length; i++) {
    str += chars[Math.floor(Math.random() * chars.length)];
  }
  return str;
}
function esc(str) {
  return JSON.stringify(str);
}
function slugify(input) {
  return input.toLowerCase().trim().replace(/[^\w\s-]/g, "").replace(/[\s_-]+/g, "-").replace(/^-+|-+$/g, "");
}
var captureStackTrace = "captureStackTrace" in Error ? Error.captureStackTrace : (..._args) => {
};
function isObject(data) {
  return typeof data === "object" && data !== null && !Array.isArray(data);
}
var allowsEval = /* @__PURE__ */ cached(() => {
  if (globalConfig.jitless) {
    return false;
  }
  if (typeof navigator !== "undefined" && navigator?.userAgent?.includes("Cloudflare")) {
    return false;
  }
  try {
    const F = Function;
    new F("");
    return true;
  } catch (_) {
    return false;
  }
});
function isPlainObject(o) {
  if (isObject(o) === false)
    return false;
  const ctor = o.constructor;
  if (ctor === void 0)
    return true;
  if (typeof ctor !== "function")
    return true;
  const prot = ctor.prototype;
  if (isObject(prot) === false)
    return false;
  if (Object.prototype.hasOwnProperty.call(prot, "isPrototypeOf") === false) {
    return false;
  }
  return true;
}
function shallowClone(o) {
  if (isPlainObject(o))
    return { ...o };
  if (Array.isArray(o))
    return [...o];
  if (o instanceof Map)
    return new Map(o);
  if (o instanceof Set)
    return new Set(o);
  return o;
}
function numKeys(data) {
  let keyCount = 0;
  for (const key in data) {
    if (Object.prototype.hasOwnProperty.call(data, key)) {
      keyCount++;
    }
  }
  return keyCount;
}
var getParsedType = (data) => {
  const t = typeof data;
  switch (t) {
    case "undefined":
      return "undefined";
    case "string":
      return "string";
    case "number":
      return Number.isNaN(data) ? "nan" : "number";
    case "boolean":
      return "boolean";
    case "function":
      return "function";
    case "bigint":
      return "bigint";
    case "symbol":
      return "symbol";
    case "object":
      if (Array.isArray(data)) {
        return "array";
      }
      if (data === null) {
        return "null";
      }
      if (data.then && typeof data.then === "function" && data.catch && typeof data.catch === "function") {
        return "promise";
      }
      if (typeof Map !== "undefined" && data instanceof Map) {
        return "map";
      }
      if (typeof Set !== "undefined" && data instanceof Set) {
        return "set";
      }
      if (typeof Date !== "undefined" && data instanceof Date) {
        return "date";
      }
      if (typeof File !== "undefined" && data instanceof File) {
        return "file";
      }
      return "object";
    default:
      throw new Error(`Unknown data type: ${t}`);
  }
};
var propertyKeyTypes = /* @__PURE__ */ new Set(["string", "number", "symbol"]);
var primitiveTypes = /* @__PURE__ */ new Set([
  "string",
  "number",
  "bigint",
  "boolean",
  "symbol",
  "undefined"
]);
function escapeRegex(str) {
  return str.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
function clone(inst, def, params) {
  const cl = new inst._zod.constr(def ?? inst._zod.def);
  if (!def || params?.parent)
    cl._zod.parent = inst;
  return cl;
}
function normalizeParams(_params) {
  const params = _params;
  if (!params)
    return {};
  if (typeof params === "string")
    return { error: () => params };
  if (params?.message !== void 0) {
    if (params?.error !== void 0)
      throw new Error("Cannot specify both `message` and `error` params");
    params.error = params.message;
  }
  delete params.message;
  if (typeof params.error === "string")
    return { ...params, error: () => params.error };
  return params;
}
function createTransparentProxy(getter) {
  let target;
  return new Proxy({}, {
    get(_, prop, receiver) {
      target ?? (target = getter());
      return Reflect.get(target, prop, receiver);
    },
    set(_, prop, value, receiver) {
      target ?? (target = getter());
      return Reflect.set(target, prop, value, receiver);
    },
    has(_, prop) {
      target ?? (target = getter());
      return Reflect.has(target, prop);
    },
    deleteProperty(_, prop) {
      target ?? (target = getter());
      return Reflect.deleteProperty(target, prop);
    },
    ownKeys(_) {
      target ?? (target = getter());
      return Reflect.ownKeys(target);
    },
    getOwnPropertyDescriptor(_, prop) {
      target ?? (target = getter());
      return Reflect.getOwnPropertyDescriptor(target, prop);
    },
    defineProperty(_, prop, descriptor) {
      target ?? (target = getter());
      return Reflect.defineProperty(target, prop, descriptor);
    }
  });
}
function stringifyPrimitive(value) {
  if (typeof value === "bigint")
    return value.toString() + "n";
  if (typeof value === "string")
    return `"${value}"`;
  return `${value}`;
}
function optionalKeys(shape) {
  return Object.keys(shape).filter((k) => {
    return shape[k]._zod.optin === "optional" && shape[k]._zod.optout === "optional";
  });
}
var NUMBER_FORMAT_RANGES = {
  safeint: [Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER],
  int32: [-2147483648, 2147483647],
  uint32: [0, 4294967295],
  float32: [-34028234663852886e22, 34028234663852886e22],
  float64: [-Number.MAX_VALUE, Number.MAX_VALUE]
};
var BIGINT_FORMAT_RANGES = {
  int64: [/* @__PURE__ */ BigInt("-9223372036854775808"), /* @__PURE__ */ BigInt("9223372036854775807")],
  uint64: [/* @__PURE__ */ BigInt(0), /* @__PURE__ */ BigInt("18446744073709551615")]
};
function pick(schema, mask) {
  const currDef = schema._zod.def;
  const checks = currDef.checks;
  const hasChecks = checks && checks.length > 0;
  if (hasChecks) {
    throw new Error(".pick() cannot be used on object schemas containing refinements");
  }
  const def = mergeDefs(schema._zod.def, {
    get shape() {
      const newShape = {};
      for (const key in mask) {
        if (!(key in currDef.shape)) {
          throw new Error(`Unrecognized key: "${key}"`);
        }
        if (!mask[key])
          continue;
        newShape[key] = currDef.shape[key];
      }
      assignProp(this, "shape", newShape);
      return newShape;
    },
    checks: []
  });
  return clone(schema, def);
}
function omit(schema, mask) {
  const currDef = schema._zod.def;
  const checks = currDef.checks;
  const hasChecks = checks && checks.length > 0;
  if (hasChecks) {
    throw new Error(".omit() cannot be used on object schemas containing refinements");
  }
  const def = mergeDefs(schema._zod.def, {
    get shape() {
      const newShape = { ...schema._zod.def.shape };
      for (const key in mask) {
        if (!(key in currDef.shape)) {
          throw new Error(`Unrecognized key: "${key}"`);
        }
        if (!mask[key])
          continue;
        delete newShape[key];
      }
      assignProp(this, "shape", newShape);
      return newShape;
    },
    checks: []
  });
  return clone(schema, def);
}
function extend(schema, shape) {
  if (!isPlainObject(shape)) {
    throw new Error("Invalid input to extend: expected a plain object");
  }
  const checks = schema._zod.def.checks;
  const hasChecks = checks && checks.length > 0;
  if (hasChecks) {
    const existingShape = schema._zod.def.shape;
    for (const key in shape) {
      if (Object.getOwnPropertyDescriptor(existingShape, key) !== void 0) {
        throw new Error("Cannot overwrite keys on object schemas containing refinements. Use `.safeExtend()` instead.");
      }
    }
  }
  const def = mergeDefs(schema._zod.def, {
    get shape() {
      const _shape = { ...schema._zod.def.shape, ...shape };
      assignProp(this, "shape", _shape);
      return _shape;
    }
  });
  return clone(schema, def);
}
function safeExtend(schema, shape) {
  if (!isPlainObject(shape)) {
    throw new Error("Invalid input to safeExtend: expected a plain object");
  }
  const def = mergeDefs(schema._zod.def, {
    get shape() {
      const _shape = { ...schema._zod.def.shape, ...shape };
      assignProp(this, "shape", _shape);
      return _shape;
    }
  });
  return clone(schema, def);
}
function merge(a, b) {
  if (a._zod.def.checks?.length) {
    throw new Error(".merge() cannot be used on object schemas containing refinements. Use .safeExtend() instead.");
  }
  const def = mergeDefs(a._zod.def, {
    get shape() {
      const _shape = { ...a._zod.def.shape, ...b._zod.def.shape };
      assignProp(this, "shape", _shape);
      return _shape;
    },
    get catchall() {
      return b._zod.def.catchall;
    },
    checks: b._zod.def.checks ?? []
  });
  return clone(a, def);
}
function partial(Class2, schema, mask) {
  const currDef = schema._zod.def;
  const checks = currDef.checks;
  const hasChecks = checks && checks.length > 0;
  if (hasChecks) {
    throw new Error(".partial() cannot be used on object schemas containing refinements");
  }
  const def = mergeDefs(schema._zod.def, {
    get shape() {
      const oldShape = schema._zod.def.shape;
      const shape = { ...oldShape };
      if (mask) {
        for (const key in mask) {
          if (!(key in oldShape)) {
            throw new Error(`Unrecognized key: "${key}"`);
          }
          if (!mask[key])
            continue;
          shape[key] = Class2 ? new Class2({
            type: "optional",
            innerType: oldShape[key]
          }) : oldShape[key];
        }
      } else {
        for (const key in oldShape) {
          shape[key] = Class2 ? new Class2({
            type: "optional",
            innerType: oldShape[key]
          }) : oldShape[key];
        }
      }
      assignProp(this, "shape", shape);
      return shape;
    },
    checks: []
  });
  return clone(schema, def);
}
function required(Class2, schema, mask) {
  const def = mergeDefs(schema._zod.def, {
    get shape() {
      const oldShape = schema._zod.def.shape;
      const shape = { ...oldShape };
      if (mask) {
        for (const key in mask) {
          if (!(key in shape)) {
            throw new Error(`Unrecognized key: "${key}"`);
          }
          if (!mask[key])
            continue;
          shape[key] = new Class2({
            type: "nonoptional",
            innerType: oldShape[key]
          });
        }
      } else {
        for (const key in oldShape) {
          shape[key] = new Class2({
            type: "nonoptional",
            innerType: oldShape[key]
          });
        }
      }
      assignProp(this, "shape", shape);
      return shape;
    }
  });
  return clone(schema, def);
}
function aborted(x, startIndex = 0) {
  if (x.aborted === true)
    return true;
  for (let i = startIndex; i < x.issues.length; i++) {
    if (x.issues[i]?.continue !== true) {
      return true;
    }
  }
  return false;
}
function explicitlyAborted(x, startIndex = 0) {
  if (x.aborted === true)
    return true;
  for (let i = startIndex; i < x.issues.length; i++) {
    if (x.issues[i]?.continue === false) {
      return true;
    }
  }
  return false;
}
function prefixIssues(path, issues) {
  return issues.map((iss) => {
    var _a3;
    (_a3 = iss).path ?? (_a3.path = []);
    iss.path.unshift(path);
    return iss;
  });
}
function unwrapMessage(message) {
  return typeof message === "string" ? message : message?.message;
}
function finalizeIssue(iss, ctx, config2) {
  const message = iss.message ? iss.message : unwrapMessage(iss.inst?._zod.def?.error?.(iss)) ?? unwrapMessage(ctx?.error?.(iss)) ?? unwrapMessage(config2.customError?.(iss)) ?? unwrapMessage(config2.localeError?.(iss)) ?? "Invalid input";
  const { inst: _inst, continue: _continue, input: _input, ...rest } = iss;
  rest.path ?? (rest.path = []);
  rest.message = message;
  if (ctx?.reportInput) {
    rest.input = _input;
  }
  return rest;
}
function getSizableOrigin(input) {
  if (input instanceof Set)
    return "set";
  if (input instanceof Map)
    return "map";
  if (input instanceof File)
    return "file";
  return "unknown";
}
function getLengthableOrigin(input) {
  if (Array.isArray(input))
    return "array";
  if (typeof input === "string")
    return "string";
  return "unknown";
}
function parsedType(data) {
  const t = typeof data;
  switch (t) {
    case "number": {
      return Number.isNaN(data) ? "nan" : "number";
    }
    case "object": {
      if (data === null) {
        return "null";
      }
      if (Array.isArray(data)) {
        return "array";
      }
      const obj = data;
      if (obj && Object.getPrototypeOf(obj) !== Object.prototype && "constructor" in obj && obj.constructor) {
        return obj.constructor.name;
      }
    }
  }
  return t;
}
function issue(...args) {
  const [iss, input, inst] = args;
  if (typeof iss === "string") {
    return {
      message: iss,
      code: "custom",
      input,
      inst
    };
  }
  return { ...iss };
}
function cleanEnum(obj) {
  return Object.entries(obj).filter(([k, _]) => {
    return Number.isNaN(Number.parseInt(k, 10));
  }).map((el) => el[1]);
}
function base64ToUint8Array(base643) {
  const binaryString = atob(base643);
  const bytes = new Uint8Array(binaryString.length);
  for (let i = 0; i < binaryString.length; i++) {
    bytes[i] = binaryString.charCodeAt(i);
  }
  return bytes;
}
function uint8ArrayToBase64(bytes) {
  let binaryString = "";
  for (let i = 0; i < bytes.length; i++) {
    binaryString += String.fromCharCode(bytes[i]);
  }
  return btoa(binaryString);
}
function base64urlToUint8Array(base64url3) {
  const base643 = base64url3.replace(/-/g, "+").replace(/_/g, "/");
  const padding = "=".repeat((4 - base643.length % 4) % 4);
  return base64ToUint8Array(base643 + padding);
}
function uint8ArrayToBase64url(bytes) {
  return uint8ArrayToBase64(bytes).replace(/\+/g, "-").replace(/\//g, "_").replace(/=/g, "");
}
function hexToUint8Array(hex3) {
  const cleanHex = hex3.replace(/^0x/, "");
  if (cleanHex.length % 2 !== 0) {
    throw new Error("Invalid hex string length");
  }
  const bytes = new Uint8Array(cleanHex.length / 2);
  for (let i = 0; i < cleanHex.length; i += 2) {
    bytes[i / 2] = Number.parseInt(cleanHex.slice(i, i + 2), 16);
  }
  return bytes;
}
function uint8ArrayToHex(bytes) {
  return Array.from(bytes).map((b) => b.toString(16).padStart(2, "0")).join("");
}
var Class = class {
  constructor(..._args) {
  }
};

// node_modules/zod/v4/core/errors.js
var initializer = (inst, def) => {
  inst.name = "$ZodError";
  Object.defineProperty(inst, "_zod", {
    value: inst._zod,
    enumerable: false
  });
  Object.defineProperty(inst, "issues", {
    value: def,
    enumerable: false
  });
  inst.message = JSON.stringify(def, jsonStringifyReplacer, 2);
  Object.defineProperty(inst, "toString", {
    value: () => inst.message,
    enumerable: false
  });
};
var $ZodError = $constructor("$ZodError", initializer);
var $ZodRealError = $constructor("$ZodError", initializer, { Parent: Error });
function flattenError(error51, mapper = (issue2) => issue2.message) {
  const fieldErrors = {};
  const formErrors = [];
  for (const sub of error51.issues) {
    if (sub.path.length > 0) {
      fieldErrors[sub.path[0]] = fieldErrors[sub.path[0]] || [];
      fieldErrors[sub.path[0]].push(mapper(sub));
    } else {
      formErrors.push(mapper(sub));
    }
  }
  return { formErrors, fieldErrors };
}
function formatError(error51, mapper = (issue2) => issue2.message) {
  const fieldErrors = { _errors: [] };
  const processError = (error52, path = []) => {
    for (const issue2 of error52.issues) {
      if (issue2.code === "invalid_union" && issue2.errors.length) {
        issue2.errors.map((issues) => processError({ issues }, [...path, ...issue2.path]));
      } else if (issue2.code === "invalid_key") {
        processError({ issues: issue2.issues }, [...path, ...issue2.path]);
      } else if (issue2.code === "invalid_element") {
        processError({ issues: issue2.issues }, [...path, ...issue2.path]);
      } else {
        const fullpath = [...path, ...issue2.path];
        if (fullpath.length === 0) {
          fieldErrors._errors.push(mapper(issue2));
        } else {
          let curr = fieldErrors;
          let i = 0;
          while (i < fullpath.length) {
            const el = fullpath[i];
            const terminal = i === fullpath.length - 1;
            if (!terminal) {
              curr[el] = curr[el] || { _errors: [] };
            } else {
              curr[el] = curr[el] || { _errors: [] };
              curr[el]._errors.push(mapper(issue2));
            }
            curr = curr[el];
            i++;
          }
        }
      }
    }
  };
  processError(error51);
  return fieldErrors;
}
function treeifyError(error51, mapper = (issue2) => issue2.message) {
  const result = { errors: [] };
  const processError = (error52, path = []) => {
    var _a3, _b;
    for (const issue2 of error52.issues) {
      if (issue2.code === "invalid_union" && issue2.errors.length) {
        issue2.errors.map((issues) => processError({ issues }, [...path, ...issue2.path]));
      } else if (issue2.code === "invalid_key") {
        processError({ issues: issue2.issues }, [...path, ...issue2.path]);
      } else if (issue2.code === "invalid_element") {
        processError({ issues: issue2.issues }, [...path, ...issue2.path]);
      } else {
        const fullpath = [...path, ...issue2.path];
        if (fullpath.length === 0) {
          result.errors.push(mapper(issue2));
          continue;
        }
        let curr = result;
        let i = 0;
        while (i < fullpath.length) {
          const el = fullpath[i];
          const terminal = i === fullpath.length - 1;
          if (typeof el === "string") {
            curr.properties ?? (curr.properties = {});
            (_a3 = curr.properties)[el] ?? (_a3[el] = { errors: [] });
            curr = curr.properties[el];
          } else {
            curr.items ?? (curr.items = []);
            (_b = curr.items)[el] ?? (_b[el] = { errors: [] });
            curr = curr.items[el];
          }
          if (terminal) {
            curr.errors.push(mapper(issue2));
          }
          i++;
        }
      }
    }
  };
  processError(error51);
  return result;
}
function toDotPath(_path) {
  const segs = [];
  const path = _path.map((seg) => typeof seg === "object" ? seg.key : seg);
  for (const seg of path) {
    if (typeof seg === "number")
      segs.push(`[${seg}]`);
    else if (typeof seg === "symbol")
      segs.push(`[${JSON.stringify(String(seg))}]`);
    else if (/[^\w$]/.test(seg))
      segs.push(`[${JSON.stringify(seg)}]`);
    else {
      if (segs.length)
        segs.push(".");
      segs.push(seg);
    }
  }
  return segs.join("");
}
function prettifyError(error51) {
  const lines = [];
  const issues = [...error51.issues].sort((a, b) => (a.path ?? []).length - (b.path ?? []).length);
  for (const issue2 of issues) {
    lines.push(`\u2716 ${issue2.message}`);
    if (issue2.path?.length)
      lines.push(`  \u2192 at ${toDotPath(issue2.path)}`);
  }
  return lines.join("\n");
}

// node_modules/zod/v4/core/parse.js
var _parse = (_Err) => (schema, value, _ctx, _params) => {
  const ctx = _ctx ? { ..._ctx, async: false } : { async: false };
  const result = schema._zod.run({ value, issues: [] }, ctx);
  if (result instanceof Promise) {
    throw new $ZodAsyncError();
  }
  if (result.issues.length) {
    const e = new (_params?.Err ?? _Err)(result.issues.map((iss) => finalizeIssue(iss, ctx, config())));
    captureStackTrace(e, _params?.callee);
    throw e;
  }
  return result.value;
};
var parse = /* @__PURE__ */ _parse($ZodRealError);
var _parseAsync = (_Err) => async (schema, value, _ctx, params) => {
  const ctx = _ctx ? { ..._ctx, async: true } : { async: true };
  let result = schema._zod.run({ value, issues: [] }, ctx);
  if (result instanceof Promise)
    result = await result;
  if (result.issues.length) {
    const e = new (params?.Err ?? _Err)(result.issues.map((iss) => finalizeIssue(iss, ctx, config())));
    captureStackTrace(e, params?.callee);
    throw e;
  }
  return result.value;
};
var parseAsync = /* @__PURE__ */ _parseAsync($ZodRealError);
var _safeParse = (_Err) => (schema, value, _ctx) => {
  const ctx = _ctx ? { ..._ctx, async: false } : { async: false };
  const result = schema._zod.run({ value, issues: [] }, ctx);
  if (result instanceof Promise) {
    throw new $ZodAsyncError();
  }
  return result.issues.length ? {
    success: false,
    error: new (_Err ?? $ZodError)(result.issues.map((iss) => finalizeIssue(iss, ctx, config())))
  } : { success: true, data: result.value };
};
var safeParse = /* @__PURE__ */ _safeParse($ZodRealError);
var _safeParseAsync = (_Err) => async (schema, value, _ctx) => {
  const ctx = _ctx ? { ..._ctx, async: true } : { async: true };
  let result = schema._zod.run({ value, issues: [] }, ctx);
  if (result instanceof Promise)
    result = await result;
  return result.issues.length ? {
    success: false,
    error: new _Err(result.issues.map((iss) => finalizeIssue(iss, ctx, config())))
  } : { success: true, data: result.value };
};
var safeParseAsync = /* @__PURE__ */ _safeParseAsync($ZodRealError);
var _encode = (_Err) => (schema, value, _ctx) => {
  const ctx = _ctx ? { ..._ctx, direction: "backward" } : { direction: "backward" };
  return _parse(_Err)(schema, value, ctx);
};
var encode = /* @__PURE__ */ _encode($ZodRealError);
var _decode = (_Err) => (schema, value, _ctx) => {
  return _parse(_Err)(schema, value, _ctx);
};
var decode = /* @__PURE__ */ _decode($ZodRealError);
var _encodeAsync = (_Err) => async (schema, value, _ctx) => {
  const ctx = _ctx ? { ..._ctx, direction: "backward" } : { direction: "backward" };
  return _parseAsync(_Err)(schema, value, ctx);
};
var encodeAsync = /* @__PURE__ */ _encodeAsync($ZodRealError);
var _decodeAsync = (_Err) => async (schema, value, _ctx) => {
  return _parseAsync(_Err)(schema, value, _ctx);
};
var decodeAsync = /* @__PURE__ */ _decodeAsync($ZodRealError);
var _safeEncode = (_Err) => (schema, value, _ctx) => {
  const ctx = _ctx ? { ..._ctx, direction: "backward" } : { direction: "backward" };
  return _safeParse(_Err)(schema, value, ctx);
};
var safeEncode = /* @__PURE__ */ _safeEncode($ZodRealError);
var _safeDecode = (_Err) => (schema, value, _ctx) => {
  return _safeParse(_Err)(schema, value, _ctx);
};
var safeDecode = /* @__PURE__ */ _safeDecode($ZodRealError);
var _safeEncodeAsync = (_Err) => async (schema, value, _ctx) => {
  const ctx = _ctx ? { ..._ctx, direction: "backward" } : { direction: "backward" };
  return _safeParseAsync(_Err)(schema, value, ctx);
};
var safeEncodeAsync = /* @__PURE__ */ _safeEncodeAsync($ZodRealError);
var _safeDecodeAsync = (_Err) => async (schema, value, _ctx) => {
  return _safeParseAsync(_Err)(schema, value, _ctx);
};
var safeDecodeAsync = /* @__PURE__ */ _safeDecodeAsync($ZodRealError);

// node_modules/zod/v4/core/regexes.js
var regexes_exports = {};
__export(regexes_exports, {
  base64: () => base64,
  base64url: () => base64url,
  bigint: () => bigint,
  boolean: () => boolean,
  browserEmail: () => browserEmail,
  cidrv4: () => cidrv4,
  cidrv6: () => cidrv6,
  cuid: () => cuid,
  cuid2: () => cuid2,
  date: () => date,
  datetime: () => datetime,
  domain: () => domain,
  duration: () => duration,
  e164: () => e164,
  email: () => email,
  emoji: () => emoji,
  extendedDuration: () => extendedDuration,
  guid: () => guid,
  hex: () => hex,
  hostname: () => hostname,
  html5Email: () => html5Email,
  httpProtocol: () => httpProtocol,
  idnEmail: () => idnEmail,
  integer: () => integer,
  ipv4: () => ipv4,
  ipv6: () => ipv6,
  ksuid: () => ksuid,
  lowercase: () => lowercase,
  mac: () => mac,
  md5_base64: () => md5_base64,
  md5_base64url: () => md5_base64url,
  md5_hex: () => md5_hex,
  nanoid: () => nanoid,
  null: () => _null,
  number: () => number,
  rfc5322Email: () => rfc5322Email,
  sha1_base64: () => sha1_base64,
  sha1_base64url: () => sha1_base64url,
  sha1_hex: () => sha1_hex,
  sha256_base64: () => sha256_base64,
  sha256_base64url: () => sha256_base64url,
  sha256_hex: () => sha256_hex,
  sha384_base64: () => sha384_base64,
  sha384_base64url: () => sha384_base64url,
  sha384_hex: () => sha384_hex,
  sha512_base64: () => sha512_base64,
  sha512_base64url: () => sha512_base64url,
  sha512_hex: () => sha512_hex,
  string: () => string,
  time: () => time,
  ulid: () => ulid,
  undefined: () => _undefined,
  unicodeEmail: () => unicodeEmail,
  uppercase: () => uppercase,
  uuid: () => uuid,
  uuid4: () => uuid4,
  uuid6: () => uuid6,
  uuid7: () => uuid7,
  xid: () => xid
});
var cuid = /^[cC][0-9a-z]{6,}$/;
var cuid2 = /^[0-9a-z]+$/;
var ulid = /^[0-9A-HJKMNP-TV-Za-hjkmnp-tv-z]{26}$/;
var xid = /^[0-9a-vA-V]{20}$/;
var ksuid = /^[A-Za-z0-9]{27}$/;
var nanoid = /^[a-zA-Z0-9_-]{21}$/;
var duration = /^P(?:(\d+W)|(?!.*W)(?=\d|T\d)(\d+Y)?(\d+M)?(\d+D)?(T(?=\d)(\d+H)?(\d+M)?(\d+([.,]\d+)?S)?)?)$/;
var extendedDuration = /^[-+]?P(?!$)(?:(?:[-+]?\d+Y)|(?:[-+]?\d+[.,]\d+Y$))?(?:(?:[-+]?\d+M)|(?:[-+]?\d+[.,]\d+M$))?(?:(?:[-+]?\d+W)|(?:[-+]?\d+[.,]\d+W$))?(?:(?:[-+]?\d+D)|(?:[-+]?\d+[.,]\d+D$))?(?:T(?=[\d+-])(?:(?:[-+]?\d+H)|(?:[-+]?\d+[.,]\d+H$))?(?:(?:[-+]?\d+M)|(?:[-+]?\d+[.,]\d+M$))?(?:[-+]?\d+(?:[.,]\d+)?S)?)??$/;
var guid = /^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$/;
var uuid = (version2) => {
  if (!version2)
    return /^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-8][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}|00000000-0000-0000-0000-000000000000|ffffffff-ffff-ffff-ffff-ffffffffffff)$/;
  return new RegExp(`^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-${version2}[0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12})$`);
};
var uuid4 = /* @__PURE__ */ uuid(4);
var uuid6 = /* @__PURE__ */ uuid(6);
var uuid7 = /* @__PURE__ */ uuid(7);
var email = /^(?!\.)(?!.*\.\.)([A-Za-z0-9_'+\-\.]*)[A-Za-z0-9_+-]@([A-Za-z0-9][A-Za-z0-9\-]*\.)+[A-Za-z]{2,}$/;
var html5Email = /^[a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$/;
var rfc5322Email = /^(([^<>()\[\]\\.,;:\s@"]+(\.[^<>()\[\]\\.,;:\s@"]+)*)|(".+"))@((\[[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}])|(([a-zA-Z\-0-9]+\.)+[a-zA-Z]{2,}))$/;
var unicodeEmail = /^[^\s@"]{1,64}@[^\s@]{1,255}$/u;
var idnEmail = unicodeEmail;
var browserEmail = /^[a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$/;
var _emoji = `^(\\p{Extended_Pictographic}|\\p{Emoji_Component})+$`;
function emoji() {
  return new RegExp(_emoji, "u");
}
var ipv4 = /^(?:(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])\.){3}(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])$/;
var ipv6 = /^(([0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,7}:|([0-9a-fA-F]{1,4}:){1,6}:[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,5}(:[0-9a-fA-F]{1,4}){1,2}|([0-9a-fA-F]{1,4}:){1,4}(:[0-9a-fA-F]{1,4}){1,3}|([0-9a-fA-F]{1,4}:){1,3}(:[0-9a-fA-F]{1,4}){1,4}|([0-9a-fA-F]{1,4}:){1,2}(:[0-9a-fA-F]{1,4}){1,5}|[0-9a-fA-F]{1,4}:((:[0-9a-fA-F]{1,4}){1,6})|:((:[0-9a-fA-F]{1,4}){1,7}|:))$/;
var mac = (delimiter) => {
  const escapedDelim = escapeRegex(delimiter ?? ":");
  return new RegExp(`^(?:[0-9A-F]{2}${escapedDelim}){5}[0-9A-F]{2}$|^(?:[0-9a-f]{2}${escapedDelim}){5}[0-9a-f]{2}$`);
};
var cidrv4 = /^((25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])\.){3}(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])\/([0-9]|[1-2][0-9]|3[0-2])$/;
var cidrv6 = /^(([0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}|::|([0-9a-fA-F]{1,4})?::([0-9a-fA-F]{1,4}:?){0,6})\/(12[0-8]|1[01][0-9]|[1-9]?[0-9])$/;
var base64 = /^$|^(?:[0-9a-zA-Z+/]{4})*(?:(?:[0-9a-zA-Z+/]{2}==)|(?:[0-9a-zA-Z+/]{3}=))?$/;
var base64url = /^[A-Za-z0-9_-]*$/;
var hostname = /^(?=.{1,253}\.?$)[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[-0-9a-zA-Z]{0,61}[0-9a-zA-Z])?)*\.?$/;
var domain = /^([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,}$/;
var httpProtocol = /^https?$/;
var e164 = /^\+[1-9]\d{6,14}$/;
var dateSource = `(?:(?:\\d\\d[2468][048]|\\d\\d[13579][26]|\\d\\d0[48]|[02468][048]00|[13579][26]00)-02-29|\\d{4}-(?:(?:0[13578]|1[02])-(?:0[1-9]|[12]\\d|3[01])|(?:0[469]|11)-(?:0[1-9]|[12]\\d|30)|(?:02)-(?:0[1-9]|1\\d|2[0-8])))`;
var date = /* @__PURE__ */ new RegExp(`^${dateSource}$`);
function timeSource(args) {
  const hhmm = `(?:[01]\\d|2[0-3]):[0-5]\\d`;
  const regex = typeof args.precision === "number" ? args.precision === -1 ? `${hhmm}` : args.precision === 0 ? `${hhmm}:[0-5]\\d` : `${hhmm}:[0-5]\\d\\.\\d{${args.precision}}` : `${hhmm}(?::[0-5]\\d(?:\\.\\d+)?)?`;
  return regex;
}
function time(args) {
  return new RegExp(`^${timeSource(args)}$`);
}
function datetime(args) {
  const time3 = timeSource({ precision: args.precision });
  const opts = ["Z"];
  if (args.local)
    opts.push("");
  if (args.offset)
    opts.push(`([+-](?:[01]\\d|2[0-3]):[0-5]\\d)`);
  const timeRegex = `${time3}(?:${opts.join("|")})`;
  return new RegExp(`^${dateSource}T(?:${timeRegex})$`);
}
var string = (params) => {
  const regex = params ? `[\\s\\S]{${params?.minimum ?? 0},${params?.maximum ?? ""}}` : `[\\s\\S]*`;
  return new RegExp(`^${regex}$`);
};
var bigint = /^-?\d+n?$/;
var integer = /^-?\d+$/;
var number = /^-?\d+(?:\.\d+)?$/;
var boolean = /^(?:true|false)$/i;
var _null = /^null$/i;
var _undefined = /^undefined$/i;
var lowercase = /^[^A-Z]*$/;
var uppercase = /^[^a-z]*$/;
var hex = /^[0-9a-fA-F]*$/;
function fixedBase64(bodyLength, padding) {
  return new RegExp(`^[A-Za-z0-9+/]{${bodyLength}}${padding}$`);
}
function fixedBase64url(length) {
  return new RegExp(`^[A-Za-z0-9_-]{${length}}$`);
}
var md5_hex = /^[0-9a-fA-F]{32}$/;
var md5_base64 = /* @__PURE__ */ fixedBase64(22, "==");
var md5_base64url = /* @__PURE__ */ fixedBase64url(22);
var sha1_hex = /^[0-9a-fA-F]{40}$/;
var sha1_base64 = /* @__PURE__ */ fixedBase64(27, "=");
var sha1_base64url = /* @__PURE__ */ fixedBase64url(27);
var sha256_hex = /^[0-9a-fA-F]{64}$/;
var sha256_base64 = /* @__PURE__ */ fixedBase64(43, "=");
var sha256_base64url = /* @__PURE__ */ fixedBase64url(43);
var sha384_hex = /^[0-9a-fA-F]{96}$/;
var sha384_base64 = /* @__PURE__ */ fixedBase64(64, "");
var sha384_base64url = /* @__PURE__ */ fixedBase64url(64);
var sha512_hex = /^[0-9a-fA-F]{128}$/;
var sha512_base64 = /* @__PURE__ */ fixedBase64(86, "==");
var sha512_base64url = /* @__PURE__ */ fixedBase64url(86);

// node_modules/zod/v4/core/checks.js
var $ZodCheck = /* @__PURE__ */ $constructor("$ZodCheck", (inst, def) => {
  var _a3;
  inst._zod ?? (inst._zod = {});
  inst._zod.def = def;
  (_a3 = inst._zod).onattach ?? (_a3.onattach = []);
});
var numericOriginMap = {
  number: "number",
  bigint: "bigint",
  object: "date"
};
var $ZodCheckLessThan = /* @__PURE__ */ $constructor("$ZodCheckLessThan", (inst, def) => {
  $ZodCheck.init(inst, def);
  const origin = numericOriginMap[typeof def.value];
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    const curr = (def.inclusive ? bag.maximum : bag.exclusiveMaximum) ?? Number.POSITIVE_INFINITY;
    if (def.value < curr) {
      if (def.inclusive)
        bag.maximum = def.value;
      else
        bag.exclusiveMaximum = def.value;
    }
  });
  inst._zod.check = (payload) => {
    if (def.inclusive ? payload.value <= def.value : payload.value < def.value) {
      return;
    }
    payload.issues.push({
      origin,
      code: "too_big",
      maximum: typeof def.value === "object" ? def.value.getTime() : def.value,
      input: payload.value,
      inclusive: def.inclusive,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckGreaterThan = /* @__PURE__ */ $constructor("$ZodCheckGreaterThan", (inst, def) => {
  $ZodCheck.init(inst, def);
  const origin = numericOriginMap[typeof def.value];
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    const curr = (def.inclusive ? bag.minimum : bag.exclusiveMinimum) ?? Number.NEGATIVE_INFINITY;
    if (def.value > curr) {
      if (def.inclusive)
        bag.minimum = def.value;
      else
        bag.exclusiveMinimum = def.value;
    }
  });
  inst._zod.check = (payload) => {
    if (def.inclusive ? payload.value >= def.value : payload.value > def.value) {
      return;
    }
    payload.issues.push({
      origin,
      code: "too_small",
      minimum: typeof def.value === "object" ? def.value.getTime() : def.value,
      input: payload.value,
      inclusive: def.inclusive,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckMultipleOf = /* @__PURE__ */ $constructor("$ZodCheckMultipleOf", (inst, def) => {
  $ZodCheck.init(inst, def);
  inst._zod.onattach.push((inst2) => {
    var _a3;
    (_a3 = inst2._zod.bag).multipleOf ?? (_a3.multipleOf = def.value);
  });
  inst._zod.check = (payload) => {
    if (typeof payload.value !== typeof def.value)
      throw new Error("Cannot mix number and bigint in multiple_of check.");
    const isMultiple = typeof payload.value === "bigint" ? payload.value % def.value === BigInt(0) : floatSafeRemainder(payload.value, def.value) === 0;
    if (isMultiple)
      return;
    payload.issues.push({
      origin: typeof payload.value,
      code: "not_multiple_of",
      divisor: def.value,
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckNumberFormat = /* @__PURE__ */ $constructor("$ZodCheckNumberFormat", (inst, def) => {
  $ZodCheck.init(inst, def);
  def.format = def.format || "float64";
  const isInt = def.format?.includes("int");
  const origin = isInt ? "int" : "number";
  const [minimum, maximum] = NUMBER_FORMAT_RANGES[def.format];
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    bag.format = def.format;
    bag.minimum = minimum;
    bag.maximum = maximum;
    if (isInt)
      bag.pattern = integer;
  });
  inst._zod.check = (payload) => {
    const input = payload.value;
    if (isInt) {
      if (!Number.isInteger(input)) {
        payload.issues.push({
          expected: origin,
          format: def.format,
          code: "invalid_type",
          continue: false,
          input,
          inst
        });
        return;
      }
      if (!Number.isSafeInteger(input)) {
        if (input > 0) {
          payload.issues.push({
            input,
            code: "too_big",
            maximum: Number.MAX_SAFE_INTEGER,
            note: "Integers must be within the safe integer range.",
            inst,
            origin,
            inclusive: true,
            continue: !def.abort
          });
        } else {
          payload.issues.push({
            input,
            code: "too_small",
            minimum: Number.MIN_SAFE_INTEGER,
            note: "Integers must be within the safe integer range.",
            inst,
            origin,
            inclusive: true,
            continue: !def.abort
          });
        }
        return;
      }
    }
    if (input < minimum) {
      payload.issues.push({
        origin: "number",
        input,
        code: "too_small",
        minimum,
        inclusive: true,
        inst,
        continue: !def.abort
      });
    }
    if (input > maximum) {
      payload.issues.push({
        origin: "number",
        input,
        code: "too_big",
        maximum,
        inclusive: true,
        inst,
        continue: !def.abort
      });
    }
  };
});
var $ZodCheckBigIntFormat = /* @__PURE__ */ $constructor("$ZodCheckBigIntFormat", (inst, def) => {
  $ZodCheck.init(inst, def);
  const [minimum, maximum] = BIGINT_FORMAT_RANGES[def.format];
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    bag.format = def.format;
    bag.minimum = minimum;
    bag.maximum = maximum;
  });
  inst._zod.check = (payload) => {
    const input = payload.value;
    if (input < minimum) {
      payload.issues.push({
        origin: "bigint",
        input,
        code: "too_small",
        minimum,
        inclusive: true,
        inst,
        continue: !def.abort
      });
    }
    if (input > maximum) {
      payload.issues.push({
        origin: "bigint",
        input,
        code: "too_big",
        maximum,
        inclusive: true,
        inst,
        continue: !def.abort
      });
    }
  };
});
var $ZodCheckMaxSize = /* @__PURE__ */ $constructor("$ZodCheckMaxSize", (inst, def) => {
  var _a3;
  $ZodCheck.init(inst, def);
  (_a3 = inst._zod.def).when ?? (_a3.when = (payload) => {
    const val = payload.value;
    return !nullish(val) && val.size !== void 0;
  });
  inst._zod.onattach.push((inst2) => {
    const curr = inst2._zod.bag.maximum ?? Number.POSITIVE_INFINITY;
    if (def.maximum < curr)
      inst2._zod.bag.maximum = def.maximum;
  });
  inst._zod.check = (payload) => {
    const input = payload.value;
    const size = input.size;
    if (size <= def.maximum)
      return;
    payload.issues.push({
      origin: getSizableOrigin(input),
      code: "too_big",
      maximum: def.maximum,
      inclusive: true,
      input,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckMinSize = /* @__PURE__ */ $constructor("$ZodCheckMinSize", (inst, def) => {
  var _a3;
  $ZodCheck.init(inst, def);
  (_a3 = inst._zod.def).when ?? (_a3.when = (payload) => {
    const val = payload.value;
    return !nullish(val) && val.size !== void 0;
  });
  inst._zod.onattach.push((inst2) => {
    const curr = inst2._zod.bag.minimum ?? Number.NEGATIVE_INFINITY;
    if (def.minimum > curr)
      inst2._zod.bag.minimum = def.minimum;
  });
  inst._zod.check = (payload) => {
    const input = payload.value;
    const size = input.size;
    if (size >= def.minimum)
      return;
    payload.issues.push({
      origin: getSizableOrigin(input),
      code: "too_small",
      minimum: def.minimum,
      inclusive: true,
      input,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckSizeEquals = /* @__PURE__ */ $constructor("$ZodCheckSizeEquals", (inst, def) => {
  var _a3;
  $ZodCheck.init(inst, def);
  (_a3 = inst._zod.def).when ?? (_a3.when = (payload) => {
    const val = payload.value;
    return !nullish(val) && val.size !== void 0;
  });
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    bag.minimum = def.size;
    bag.maximum = def.size;
    bag.size = def.size;
  });
  inst._zod.check = (payload) => {
    const input = payload.value;
    const size = input.size;
    if (size === def.size)
      return;
    const tooBig = size > def.size;
    payload.issues.push({
      origin: getSizableOrigin(input),
      ...tooBig ? { code: "too_big", maximum: def.size } : { code: "too_small", minimum: def.size },
      inclusive: true,
      exact: true,
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckMaxLength = /* @__PURE__ */ $constructor("$ZodCheckMaxLength", (inst, def) => {
  var _a3;
  $ZodCheck.init(inst, def);
  (_a3 = inst._zod.def).when ?? (_a3.when = (payload) => {
    const val = payload.value;
    return !nullish(val) && val.length !== void 0;
  });
  inst._zod.onattach.push((inst2) => {
    const curr = inst2._zod.bag.maximum ?? Number.POSITIVE_INFINITY;
    if (def.maximum < curr)
      inst2._zod.bag.maximum = def.maximum;
  });
  inst._zod.check = (payload) => {
    const input = payload.value;
    const length = input.length;
    if (length <= def.maximum)
      return;
    const origin = getLengthableOrigin(input);
    payload.issues.push({
      origin,
      code: "too_big",
      maximum: def.maximum,
      inclusive: true,
      input,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckMinLength = /* @__PURE__ */ $constructor("$ZodCheckMinLength", (inst, def) => {
  var _a3;
  $ZodCheck.init(inst, def);
  (_a3 = inst._zod.def).when ?? (_a3.when = (payload) => {
    const val = payload.value;
    return !nullish(val) && val.length !== void 0;
  });
  inst._zod.onattach.push((inst2) => {
    const curr = inst2._zod.bag.minimum ?? Number.NEGATIVE_INFINITY;
    if (def.minimum > curr)
      inst2._zod.bag.minimum = def.minimum;
  });
  inst._zod.check = (payload) => {
    const input = payload.value;
    const length = input.length;
    if (length >= def.minimum)
      return;
    const origin = getLengthableOrigin(input);
    payload.issues.push({
      origin,
      code: "too_small",
      minimum: def.minimum,
      inclusive: true,
      input,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckLengthEquals = /* @__PURE__ */ $constructor("$ZodCheckLengthEquals", (inst, def) => {
  var _a3;
  $ZodCheck.init(inst, def);
  (_a3 = inst._zod.def).when ?? (_a3.when = (payload) => {
    const val = payload.value;
    return !nullish(val) && val.length !== void 0;
  });
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    bag.minimum = def.length;
    bag.maximum = def.length;
    bag.length = def.length;
  });
  inst._zod.check = (payload) => {
    const input = payload.value;
    const length = input.length;
    if (length === def.length)
      return;
    const origin = getLengthableOrigin(input);
    const tooBig = length > def.length;
    payload.issues.push({
      origin,
      ...tooBig ? { code: "too_big", maximum: def.length } : { code: "too_small", minimum: def.length },
      inclusive: true,
      exact: true,
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckStringFormat = /* @__PURE__ */ $constructor("$ZodCheckStringFormat", (inst, def) => {
  var _a3, _b;
  $ZodCheck.init(inst, def);
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    bag.format = def.format;
    if (def.pattern) {
      bag.patterns ?? (bag.patterns = /* @__PURE__ */ new Set());
      bag.patterns.add(def.pattern);
    }
  });
  if (def.pattern)
    (_a3 = inst._zod).check ?? (_a3.check = (payload) => {
      def.pattern.lastIndex = 0;
      if (def.pattern.test(payload.value))
        return;
      payload.issues.push({
        origin: "string",
        code: "invalid_format",
        format: def.format,
        input: payload.value,
        ...def.pattern ? { pattern: def.pattern.toString() } : {},
        inst,
        continue: !def.abort
      });
    });
  else
    (_b = inst._zod).check ?? (_b.check = () => {
    });
});
var $ZodCheckRegex = /* @__PURE__ */ $constructor("$ZodCheckRegex", (inst, def) => {
  $ZodCheckStringFormat.init(inst, def);
  inst._zod.check = (payload) => {
    def.pattern.lastIndex = 0;
    if (def.pattern.test(payload.value))
      return;
    payload.issues.push({
      origin: "string",
      code: "invalid_format",
      format: "regex",
      input: payload.value,
      pattern: def.pattern.toString(),
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckLowerCase = /* @__PURE__ */ $constructor("$ZodCheckLowerCase", (inst, def) => {
  def.pattern ?? (def.pattern = lowercase);
  $ZodCheckStringFormat.init(inst, def);
});
var $ZodCheckUpperCase = /* @__PURE__ */ $constructor("$ZodCheckUpperCase", (inst, def) => {
  def.pattern ?? (def.pattern = uppercase);
  $ZodCheckStringFormat.init(inst, def);
});
var $ZodCheckIncludes = /* @__PURE__ */ $constructor("$ZodCheckIncludes", (inst, def) => {
  $ZodCheck.init(inst, def);
  const escapedRegex = escapeRegex(def.includes);
  const pattern = new RegExp(typeof def.position === "number" ? `^.{${def.position}}${escapedRegex}` : escapedRegex);
  def.pattern = pattern;
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    bag.patterns ?? (bag.patterns = /* @__PURE__ */ new Set());
    bag.patterns.add(pattern);
  });
  inst._zod.check = (payload) => {
    if (payload.value.includes(def.includes, def.position))
      return;
    payload.issues.push({
      origin: "string",
      code: "invalid_format",
      format: "includes",
      includes: def.includes,
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckStartsWith = /* @__PURE__ */ $constructor("$ZodCheckStartsWith", (inst, def) => {
  $ZodCheck.init(inst, def);
  const pattern = new RegExp(`^${escapeRegex(def.prefix)}.*`);
  def.pattern ?? (def.pattern = pattern);
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    bag.patterns ?? (bag.patterns = /* @__PURE__ */ new Set());
    bag.patterns.add(pattern);
  });
  inst._zod.check = (payload) => {
    if (payload.value.startsWith(def.prefix))
      return;
    payload.issues.push({
      origin: "string",
      code: "invalid_format",
      format: "starts_with",
      prefix: def.prefix,
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckEndsWith = /* @__PURE__ */ $constructor("$ZodCheckEndsWith", (inst, def) => {
  $ZodCheck.init(inst, def);
  const pattern = new RegExp(`.*${escapeRegex(def.suffix)}$`);
  def.pattern ?? (def.pattern = pattern);
  inst._zod.onattach.push((inst2) => {
    const bag = inst2._zod.bag;
    bag.patterns ?? (bag.patterns = /* @__PURE__ */ new Set());
    bag.patterns.add(pattern);
  });
  inst._zod.check = (payload) => {
    if (payload.value.endsWith(def.suffix))
      return;
    payload.issues.push({
      origin: "string",
      code: "invalid_format",
      format: "ends_with",
      suffix: def.suffix,
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
function handleCheckPropertyResult(result, payload, property) {
  if (result.issues.length) {
    payload.issues.push(...prefixIssues(property, result.issues));
  }
}
var $ZodCheckProperty = /* @__PURE__ */ $constructor("$ZodCheckProperty", (inst, def) => {
  $ZodCheck.init(inst, def);
  inst._zod.check = (payload) => {
    const result = def.schema._zod.run({
      value: payload.value[def.property],
      issues: []
    }, {});
    if (result instanceof Promise) {
      return result.then((result2) => handleCheckPropertyResult(result2, payload, def.property));
    }
    handleCheckPropertyResult(result, payload, def.property);
    return;
  };
});
var $ZodCheckMimeType = /* @__PURE__ */ $constructor("$ZodCheckMimeType", (inst, def) => {
  $ZodCheck.init(inst, def);
  const mimeSet = new Set(def.mime);
  inst._zod.onattach.push((inst2) => {
    inst2._zod.bag.mime = def.mime;
  });
  inst._zod.check = (payload) => {
    if (mimeSet.has(payload.value.type))
      return;
    payload.issues.push({
      code: "invalid_value",
      values: def.mime,
      input: payload.value.type,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCheckOverwrite = /* @__PURE__ */ $constructor("$ZodCheckOverwrite", (inst, def) => {
  $ZodCheck.init(inst, def);
  inst._zod.check = (payload) => {
    payload.value = def.tx(payload.value);
  };
});

// node_modules/zod/v4/core/doc.js
var Doc = class {
  constructor(args = []) {
    this.content = [];
    this.indent = 0;
    if (this)
      this.args = args;
  }
  indented(fn) {
    this.indent += 1;
    fn(this);
    this.indent -= 1;
  }
  write(arg) {
    if (typeof arg === "function") {
      arg(this, { execution: "sync" });
      arg(this, { execution: "async" });
      return;
    }
    const content = arg;
    const lines = content.split("\n").filter((x) => x);
    const minIndent = Math.min(...lines.map((x) => x.length - x.trimStart().length));
    const dedented = lines.map((x) => x.slice(minIndent)).map((x) => " ".repeat(this.indent * 2) + x);
    for (const line of dedented) {
      this.content.push(line);
    }
  }
  compile() {
    const F = Function;
    const args = this?.args;
    const content = this?.content ?? [``];
    const lines = [...content.map((x) => `  ${x}`)];
    return new F(...args, lines.join("\n"));
  }
};

// node_modules/zod/v4/core/versions.js
var version = {
  major: 4,
  minor: 4,
  patch: 3
};

// node_modules/zod/v4/core/schemas.js
var $ZodType = /* @__PURE__ */ $constructor("$ZodType", (inst, def) => {
  var _a3;
  inst ?? (inst = {});
  inst._zod.def = def;
  inst._zod.bag = inst._zod.bag || {};
  inst._zod.version = version;
  const checks = [...inst._zod.def.checks ?? []];
  if (inst._zod.traits.has("$ZodCheck")) {
    checks.unshift(inst);
  }
  for (const ch of checks) {
    for (const fn of ch._zod.onattach) {
      fn(inst);
    }
  }
  if (checks.length === 0) {
    (_a3 = inst._zod).deferred ?? (_a3.deferred = []);
    inst._zod.deferred?.push(() => {
      inst._zod.run = inst._zod.parse;
    });
  } else {
    const runChecks = (payload, checks2, ctx) => {
      let isAborted = aborted(payload);
      let asyncResult;
      for (const ch of checks2) {
        if (ch._zod.def.when) {
          if (explicitlyAborted(payload))
            continue;
          const shouldRun = ch._zod.def.when(payload);
          if (!shouldRun)
            continue;
        } else if (isAborted) {
          continue;
        }
        const currLen = payload.issues.length;
        const _ = ch._zod.check(payload);
        if (_ instanceof Promise && ctx?.async === false) {
          throw new $ZodAsyncError();
        }
        if (asyncResult || _ instanceof Promise) {
          asyncResult = (asyncResult ?? Promise.resolve()).then(async () => {
            await _;
            const nextLen = payload.issues.length;
            if (nextLen === currLen)
              return;
            if (!isAborted)
              isAborted = aborted(payload, currLen);
          });
        } else {
          const nextLen = payload.issues.length;
          if (nextLen === currLen)
            continue;
          if (!isAborted)
            isAborted = aborted(payload, currLen);
        }
      }
      if (asyncResult) {
        return asyncResult.then(() => {
          return payload;
        });
      }
      return payload;
    };
    const handleCanaryResult = (canary, payload, ctx) => {
      if (aborted(canary)) {
        canary.aborted = true;
        return canary;
      }
      const checkResult = runChecks(payload, checks, ctx);
      if (checkResult instanceof Promise) {
        if (ctx.async === false)
          throw new $ZodAsyncError();
        return checkResult.then((checkResult2) => inst._zod.parse(checkResult2, ctx));
      }
      return inst._zod.parse(checkResult, ctx);
    };
    inst._zod.run = (payload, ctx) => {
      if (ctx.skipChecks) {
        return inst._zod.parse(payload, ctx);
      }
      if (ctx.direction === "backward") {
        const canary = inst._zod.parse({ value: payload.value, issues: [] }, { ...ctx, skipChecks: true });
        if (canary instanceof Promise) {
          return canary.then((canary2) => {
            return handleCanaryResult(canary2, payload, ctx);
          });
        }
        return handleCanaryResult(canary, payload, ctx);
      }
      const result = inst._zod.parse(payload, ctx);
      if (result instanceof Promise) {
        if (ctx.async === false)
          throw new $ZodAsyncError();
        return result.then((result2) => runChecks(result2, checks, ctx));
      }
      return runChecks(result, checks, ctx);
    };
  }
  defineLazy(inst, "~standard", () => ({
    validate: (value) => {
      try {
        const r = safeParse(inst, value);
        return r.success ? { value: r.data } : { issues: r.error?.issues };
      } catch (_) {
        return safeParseAsync(inst, value).then((r) => r.success ? { value: r.data } : { issues: r.error?.issues });
      }
    },
    vendor: "zod",
    version: 1
  }));
});
var $ZodString = /* @__PURE__ */ $constructor("$ZodString", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.pattern = [...inst?._zod.bag?.patterns ?? []].pop() ?? string(inst._zod.bag);
  inst._zod.parse = (payload, _) => {
    if (def.coerce)
      try {
        payload.value = String(payload.value);
      } catch (_2) {
      }
    if (typeof payload.value === "string")
      return payload;
    payload.issues.push({
      expected: "string",
      code: "invalid_type",
      input: payload.value,
      inst
    });
    return payload;
  };
});
var $ZodStringFormat = /* @__PURE__ */ $constructor("$ZodStringFormat", (inst, def) => {
  $ZodCheckStringFormat.init(inst, def);
  $ZodString.init(inst, def);
});
var $ZodGUID = /* @__PURE__ */ $constructor("$ZodGUID", (inst, def) => {
  def.pattern ?? (def.pattern = guid);
  $ZodStringFormat.init(inst, def);
});
var $ZodUUID = /* @__PURE__ */ $constructor("$ZodUUID", (inst, def) => {
  if (def.version) {
    const versionMap = {
      v1: 1,
      v2: 2,
      v3: 3,
      v4: 4,
      v5: 5,
      v6: 6,
      v7: 7,
      v8: 8
    };
    const v = versionMap[def.version];
    if (v === void 0)
      throw new Error(`Invalid UUID version: "${def.version}"`);
    def.pattern ?? (def.pattern = uuid(v));
  } else
    def.pattern ?? (def.pattern = uuid());
  $ZodStringFormat.init(inst, def);
});
var $ZodEmail = /* @__PURE__ */ $constructor("$ZodEmail", (inst, def) => {
  def.pattern ?? (def.pattern = email);
  $ZodStringFormat.init(inst, def);
});
var $ZodURL = /* @__PURE__ */ $constructor("$ZodURL", (inst, def) => {
  $ZodStringFormat.init(inst, def);
  inst._zod.check = (payload) => {
    try {
      const trimmed = payload.value.trim();
      if (!def.normalize && def.protocol?.source === httpProtocol.source) {
        if (!/^https?:\/\//i.test(trimmed)) {
          payload.issues.push({
            code: "invalid_format",
            format: "url",
            note: "Invalid URL format",
            input: payload.value,
            inst,
            continue: !def.abort
          });
          return;
        }
      }
      const url2 = new URL(trimmed);
      if (def.hostname) {
        def.hostname.lastIndex = 0;
        if (!def.hostname.test(url2.hostname)) {
          payload.issues.push({
            code: "invalid_format",
            format: "url",
            note: "Invalid hostname",
            pattern: def.hostname.source,
            input: payload.value,
            inst,
            continue: !def.abort
          });
        }
      }
      if (def.protocol) {
        def.protocol.lastIndex = 0;
        if (!def.protocol.test(url2.protocol.endsWith(":") ? url2.protocol.slice(0, -1) : url2.protocol)) {
          payload.issues.push({
            code: "invalid_format",
            format: "url",
            note: "Invalid protocol",
            pattern: def.protocol.source,
            input: payload.value,
            inst,
            continue: !def.abort
          });
        }
      }
      if (def.normalize) {
        payload.value = url2.href;
      } else {
        payload.value = trimmed;
      }
      return;
    } catch (_) {
      payload.issues.push({
        code: "invalid_format",
        format: "url",
        input: payload.value,
        inst,
        continue: !def.abort
      });
    }
  };
});
var $ZodEmoji = /* @__PURE__ */ $constructor("$ZodEmoji", (inst, def) => {
  def.pattern ?? (def.pattern = emoji());
  $ZodStringFormat.init(inst, def);
});
var $ZodNanoID = /* @__PURE__ */ $constructor("$ZodNanoID", (inst, def) => {
  def.pattern ?? (def.pattern = nanoid);
  $ZodStringFormat.init(inst, def);
});
var $ZodCUID = /* @__PURE__ */ $constructor("$ZodCUID", (inst, def) => {
  def.pattern ?? (def.pattern = cuid);
  $ZodStringFormat.init(inst, def);
});
var $ZodCUID2 = /* @__PURE__ */ $constructor("$ZodCUID2", (inst, def) => {
  def.pattern ?? (def.pattern = cuid2);
  $ZodStringFormat.init(inst, def);
});
var $ZodULID = /* @__PURE__ */ $constructor("$ZodULID", (inst, def) => {
  def.pattern ?? (def.pattern = ulid);
  $ZodStringFormat.init(inst, def);
});
var $ZodXID = /* @__PURE__ */ $constructor("$ZodXID", (inst, def) => {
  def.pattern ?? (def.pattern = xid);
  $ZodStringFormat.init(inst, def);
});
var $ZodKSUID = /* @__PURE__ */ $constructor("$ZodKSUID", (inst, def) => {
  def.pattern ?? (def.pattern = ksuid);
  $ZodStringFormat.init(inst, def);
});
var $ZodISODateTime = /* @__PURE__ */ $constructor("$ZodISODateTime", (inst, def) => {
  def.pattern ?? (def.pattern = datetime(def));
  $ZodStringFormat.init(inst, def);
});
var $ZodISODate = /* @__PURE__ */ $constructor("$ZodISODate", (inst, def) => {
  def.pattern ?? (def.pattern = date);
  $ZodStringFormat.init(inst, def);
});
var $ZodISOTime = /* @__PURE__ */ $constructor("$ZodISOTime", (inst, def) => {
  def.pattern ?? (def.pattern = time(def));
  $ZodStringFormat.init(inst, def);
});
var $ZodISODuration = /* @__PURE__ */ $constructor("$ZodISODuration", (inst, def) => {
  def.pattern ?? (def.pattern = duration);
  $ZodStringFormat.init(inst, def);
});
var $ZodIPv4 = /* @__PURE__ */ $constructor("$ZodIPv4", (inst, def) => {
  def.pattern ?? (def.pattern = ipv4);
  $ZodStringFormat.init(inst, def);
  inst._zod.bag.format = `ipv4`;
});
var $ZodIPv6 = /* @__PURE__ */ $constructor("$ZodIPv6", (inst, def) => {
  def.pattern ?? (def.pattern = ipv6);
  $ZodStringFormat.init(inst, def);
  inst._zod.bag.format = `ipv6`;
  inst._zod.check = (payload) => {
    try {
      new URL(`http://[${payload.value}]`);
    } catch {
      payload.issues.push({
        code: "invalid_format",
        format: "ipv6",
        input: payload.value,
        inst,
        continue: !def.abort
      });
    }
  };
});
var $ZodMAC = /* @__PURE__ */ $constructor("$ZodMAC", (inst, def) => {
  def.pattern ?? (def.pattern = mac(def.delimiter));
  $ZodStringFormat.init(inst, def);
  inst._zod.bag.format = `mac`;
});
var $ZodCIDRv4 = /* @__PURE__ */ $constructor("$ZodCIDRv4", (inst, def) => {
  def.pattern ?? (def.pattern = cidrv4);
  $ZodStringFormat.init(inst, def);
});
var $ZodCIDRv6 = /* @__PURE__ */ $constructor("$ZodCIDRv6", (inst, def) => {
  def.pattern ?? (def.pattern = cidrv6);
  $ZodStringFormat.init(inst, def);
  inst._zod.check = (payload) => {
    const parts = payload.value.split("/");
    try {
      if (parts.length !== 2)
        throw new Error();
      const [address, prefix] = parts;
      if (!prefix)
        throw new Error();
      const prefixNum = Number(prefix);
      if (`${prefixNum}` !== prefix)
        throw new Error();
      if (prefixNum < 0 || prefixNum > 128)
        throw new Error();
      new URL(`http://[${address}]`);
    } catch {
      payload.issues.push({
        code: "invalid_format",
        format: "cidrv6",
        input: payload.value,
        inst,
        continue: !def.abort
      });
    }
  };
});
function isValidBase64(data) {
  if (data === "")
    return true;
  if (/\s/.test(data))
    return false;
  if (data.length % 4 !== 0)
    return false;
  try {
    atob(data);
    return true;
  } catch {
    return false;
  }
}
var $ZodBase64 = /* @__PURE__ */ $constructor("$ZodBase64", (inst, def) => {
  def.pattern ?? (def.pattern = base64);
  $ZodStringFormat.init(inst, def);
  inst._zod.bag.contentEncoding = "base64";
  inst._zod.check = (payload) => {
    if (isValidBase64(payload.value))
      return;
    payload.issues.push({
      code: "invalid_format",
      format: "base64",
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
function isValidBase64URL(data) {
  if (!base64url.test(data))
    return false;
  const base643 = data.replace(/[-_]/g, (c) => c === "-" ? "+" : "/");
  const padded = base643.padEnd(Math.ceil(base643.length / 4) * 4, "=");
  return isValidBase64(padded);
}
var $ZodBase64URL = /* @__PURE__ */ $constructor("$ZodBase64URL", (inst, def) => {
  def.pattern ?? (def.pattern = base64url);
  $ZodStringFormat.init(inst, def);
  inst._zod.bag.contentEncoding = "base64url";
  inst._zod.check = (payload) => {
    if (isValidBase64URL(payload.value))
      return;
    payload.issues.push({
      code: "invalid_format",
      format: "base64url",
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodE164 = /* @__PURE__ */ $constructor("$ZodE164", (inst, def) => {
  def.pattern ?? (def.pattern = e164);
  $ZodStringFormat.init(inst, def);
});
function isValidJWT(token, algorithm = null) {
  try {
    const tokensParts = token.split(".");
    if (tokensParts.length !== 3)
      return false;
    const [header] = tokensParts;
    if (!header)
      return false;
    const parsedHeader = JSON.parse(atob(header));
    if ("typ" in parsedHeader && parsedHeader?.typ !== "JWT")
      return false;
    if (!parsedHeader.alg)
      return false;
    if (algorithm && (!("alg" in parsedHeader) || parsedHeader.alg !== algorithm))
      return false;
    return true;
  } catch {
    return false;
  }
}
var $ZodJWT = /* @__PURE__ */ $constructor("$ZodJWT", (inst, def) => {
  $ZodStringFormat.init(inst, def);
  inst._zod.check = (payload) => {
    if (isValidJWT(payload.value, def.alg))
      return;
    payload.issues.push({
      code: "invalid_format",
      format: "jwt",
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodCustomStringFormat = /* @__PURE__ */ $constructor("$ZodCustomStringFormat", (inst, def) => {
  $ZodStringFormat.init(inst, def);
  inst._zod.check = (payload) => {
    if (def.fn(payload.value))
      return;
    payload.issues.push({
      code: "invalid_format",
      format: def.format,
      input: payload.value,
      inst,
      continue: !def.abort
    });
  };
});
var $ZodNumber = /* @__PURE__ */ $constructor("$ZodNumber", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.pattern = inst._zod.bag.pattern ?? number;
  inst._zod.parse = (payload, _ctx) => {
    if (def.coerce)
      try {
        payload.value = Number(payload.value);
      } catch (_) {
      }
    const input = payload.value;
    if (typeof input === "number" && !Number.isNaN(input) && Number.isFinite(input)) {
      return payload;
    }
    const received = typeof input === "number" ? Number.isNaN(input) ? "NaN" : !Number.isFinite(input) ? "Infinity" : void 0 : void 0;
    payload.issues.push({
      expected: "number",
      code: "invalid_type",
      input,
      inst,
      ...received ? { received } : {}
    });
    return payload;
  };
});
var $ZodNumberFormat = /* @__PURE__ */ $constructor("$ZodNumberFormat", (inst, def) => {
  $ZodCheckNumberFormat.init(inst, def);
  $ZodNumber.init(inst, def);
});
var $ZodBoolean = /* @__PURE__ */ $constructor("$ZodBoolean", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.pattern = boolean;
  inst._zod.parse = (payload, _ctx) => {
    if (def.coerce)
      try {
        payload.value = Boolean(payload.value);
      } catch (_) {
      }
    const input = payload.value;
    if (typeof input === "boolean")
      return payload;
    payload.issues.push({
      expected: "boolean",
      code: "invalid_type",
      input,
      inst
    });
    return payload;
  };
});
var $ZodBigInt = /* @__PURE__ */ $constructor("$ZodBigInt", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.pattern = bigint;
  inst._zod.parse = (payload, _ctx) => {
    if (def.coerce)
      try {
        payload.value = BigInt(payload.value);
      } catch (_) {
      }
    if (typeof payload.value === "bigint")
      return payload;
    payload.issues.push({
      expected: "bigint",
      code: "invalid_type",
      input: payload.value,
      inst
    });
    return payload;
  };
});
var $ZodBigIntFormat = /* @__PURE__ */ $constructor("$ZodBigIntFormat", (inst, def) => {
  $ZodCheckBigIntFormat.init(inst, def);
  $ZodBigInt.init(inst, def);
});
var $ZodSymbol = /* @__PURE__ */ $constructor("$ZodSymbol", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, _ctx) => {
    const input = payload.value;
    if (typeof input === "symbol")
      return payload;
    payload.issues.push({
      expected: "symbol",
      code: "invalid_type",
      input,
      inst
    });
    return payload;
  };
});
var $ZodUndefined = /* @__PURE__ */ $constructor("$ZodUndefined", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.pattern = _undefined;
  inst._zod.values = /* @__PURE__ */ new Set([void 0]);
  inst._zod.parse = (payload, _ctx) => {
    const input = payload.value;
    if (typeof input === "undefined")
      return payload;
    payload.issues.push({
      expected: "undefined",
      code: "invalid_type",
      input,
      inst
    });
    return payload;
  };
});
var $ZodNull = /* @__PURE__ */ $constructor("$ZodNull", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.pattern = _null;
  inst._zod.values = /* @__PURE__ */ new Set([null]);
  inst._zod.parse = (payload, _ctx) => {
    const input = payload.value;
    if (input === null)
      return payload;
    payload.issues.push({
      expected: "null",
      code: "invalid_type",
      input,
      inst
    });
    return payload;
  };
});
var $ZodAny = /* @__PURE__ */ $constructor("$ZodAny", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload) => payload;
});
var $ZodUnknown = /* @__PURE__ */ $constructor("$ZodUnknown", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload) => payload;
});
var $ZodNever = /* @__PURE__ */ $constructor("$ZodNever", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, _ctx) => {
    payload.issues.push({
      expected: "never",
      code: "invalid_type",
      input: payload.value,
      inst
    });
    return payload;
  };
});
var $ZodVoid = /* @__PURE__ */ $constructor("$ZodVoid", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, _ctx) => {
    const input = payload.value;
    if (typeof input === "undefined")
      return payload;
    payload.issues.push({
      expected: "void",
      code: "invalid_type",
      input,
      inst
    });
    return payload;
  };
});
var $ZodDate = /* @__PURE__ */ $constructor("$ZodDate", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, _ctx) => {
    if (def.coerce) {
      try {
        payload.value = new Date(payload.value);
      } catch (_err) {
      }
    }
    const input = payload.value;
    const isDate = input instanceof Date;
    const isValidDate2 = isDate && !Number.isNaN(input.getTime());
    if (isValidDate2)
      return payload;
    payload.issues.push({
      expected: "date",
      code: "invalid_type",
      input,
      ...isDate ? { received: "Invalid Date" } : {},
      inst
    });
    return payload;
  };
});
function handleArrayResult(result, final, index) {
  if (result.issues.length) {
    final.issues.push(...prefixIssues(index, result.issues));
  }
  final.value[index] = result.value;
}
var $ZodArray = /* @__PURE__ */ $constructor("$ZodArray", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, ctx) => {
    const input = payload.value;
    if (!Array.isArray(input)) {
      payload.issues.push({
        expected: "array",
        code: "invalid_type",
        input,
        inst
      });
      return payload;
    }
    payload.value = Array(input.length);
    const proms = [];
    for (let i = 0; i < input.length; i++) {
      const item = input[i];
      const result = def.element._zod.run({
        value: item,
        issues: []
      }, ctx);
      if (result instanceof Promise) {
        proms.push(result.then((result2) => handleArrayResult(result2, payload, i)));
      } else {
        handleArrayResult(result, payload, i);
      }
    }
    if (proms.length) {
      return Promise.all(proms).then(() => payload);
    }
    return payload;
  };
});
function handlePropertyResult(result, final, key, input, isOptionalIn, isOptionalOut) {
  const isPresent = key in input;
  if (result.issues.length) {
    if (isOptionalIn && isOptionalOut && !isPresent) {
      return;
    }
    final.issues.push(...prefixIssues(key, result.issues));
  }
  if (!isPresent && !isOptionalIn) {
    if (!result.issues.length) {
      final.issues.push({
        code: "invalid_type",
        expected: "nonoptional",
        input: void 0,
        path: [key]
      });
    }
    return;
  }
  if (result.value === void 0) {
    if (isPresent) {
      final.value[key] = void 0;
    }
  } else {
    final.value[key] = result.value;
  }
}
function normalizeDef(def) {
  const keys = Object.keys(def.shape);
  for (const k of keys) {
    if (!def.shape?.[k]?._zod?.traits?.has("$ZodType")) {
      throw new Error(`Invalid element at key "${k}": expected a Zod schema`);
    }
  }
  const okeys = optionalKeys(def.shape);
  return {
    ...def,
    keys,
    keySet: new Set(keys),
    numKeys: keys.length,
    optionalKeys: new Set(okeys)
  };
}
function handleCatchall(proms, input, payload, ctx, def, inst) {
  const unrecognized = [];
  const keySet = def.keySet;
  const _catchall = def.catchall._zod;
  const t = _catchall.def.type;
  const isOptionalIn = _catchall.optin === "optional";
  const isOptionalOut = _catchall.optout === "optional";
  for (const key in input) {
    if (key === "__proto__")
      continue;
    if (keySet.has(key))
      continue;
    if (t === "never") {
      unrecognized.push(key);
      continue;
    }
    const r = _catchall.run({ value: input[key], issues: [] }, ctx);
    if (r instanceof Promise) {
      proms.push(r.then((r2) => handlePropertyResult(r2, payload, key, input, isOptionalIn, isOptionalOut)));
    } else {
      handlePropertyResult(r, payload, key, input, isOptionalIn, isOptionalOut);
    }
  }
  if (unrecognized.length) {
    payload.issues.push({
      code: "unrecognized_keys",
      keys: unrecognized,
      input,
      inst
    });
  }
  if (!proms.length)
    return payload;
  return Promise.all(proms).then(() => {
    return payload;
  });
}
var $ZodObject = /* @__PURE__ */ $constructor("$ZodObject", (inst, def) => {
  $ZodType.init(inst, def);
  const desc = Object.getOwnPropertyDescriptor(def, "shape");
  if (!desc?.get) {
    const sh = def.shape;
    Object.defineProperty(def, "shape", {
      get: () => {
        const newSh = { ...sh };
        Object.defineProperty(def, "shape", {
          value: newSh
        });
        return newSh;
      }
    });
  }
  const _normalized = cached(() => normalizeDef(def));
  defineLazy(inst._zod, "propValues", () => {
    const shape = def.shape;
    const propValues = {};
    for (const key in shape) {
      const field = shape[key]._zod;
      if (field.values) {
        propValues[key] ?? (propValues[key] = /* @__PURE__ */ new Set());
        for (const v of field.values)
          propValues[key].add(v);
      }
    }
    return propValues;
  });
  const isObject2 = isObject;
  const catchall = def.catchall;
  let value;
  inst._zod.parse = (payload, ctx) => {
    value ?? (value = _normalized.value);
    const input = payload.value;
    if (!isObject2(input)) {
      payload.issues.push({
        expected: "object",
        code: "invalid_type",
        input,
        inst
      });
      return payload;
    }
    payload.value = {};
    const proms = [];
    const shape = value.shape;
    for (const key of value.keys) {
      const el = shape[key];
      const isOptionalIn = el._zod.optin === "optional";
      const isOptionalOut = el._zod.optout === "optional";
      const r = el._zod.run({ value: input[key], issues: [] }, ctx);
      if (r instanceof Promise) {
        proms.push(r.then((r2) => handlePropertyResult(r2, payload, key, input, isOptionalIn, isOptionalOut)));
      } else {
        handlePropertyResult(r, payload, key, input, isOptionalIn, isOptionalOut);
      }
    }
    if (!catchall) {
      return proms.length ? Promise.all(proms).then(() => payload) : payload;
    }
    return handleCatchall(proms, input, payload, ctx, _normalized.value, inst);
  };
});
var $ZodObjectJIT = /* @__PURE__ */ $constructor("$ZodObjectJIT", (inst, def) => {
  $ZodObject.init(inst, def);
  const superParse = inst._zod.parse;
  const _normalized = cached(() => normalizeDef(def));
  const generateFastpass = (shape) => {
    const doc = new Doc(["shape", "payload", "ctx"]);
    const normalized = _normalized.value;
    const parseStr = (key) => {
      const k = esc(key);
      return `shape[${k}]._zod.run({ value: input[${k}], issues: [] }, ctx)`;
    };
    doc.write(`const input = payload.value;`);
    const ids = /* @__PURE__ */ Object.create(null);
    let counter = 0;
    for (const key of normalized.keys) {
      ids[key] = `key_${counter++}`;
    }
    doc.write(`const newResult = {};`);
    for (const key of normalized.keys) {
      const id = ids[key];
      const k = esc(key);
      const schema = shape[key];
      const isOptionalIn = schema?._zod?.optin === "optional";
      const isOptionalOut = schema?._zod?.optout === "optional";
      doc.write(`const ${id} = ${parseStr(key)};`);
      if (isOptionalIn && isOptionalOut) {
        doc.write(`
        if (${id}.issues.length) {
          if (${k} in input) {
            payload.issues = payload.issues.concat(${id}.issues.map(iss => ({
              ...iss,
              path: iss.path ? [${k}, ...iss.path] : [${k}]
            })));
          }
        }
        
        if (${id}.value === undefined) {
          if (${k} in input) {
            newResult[${k}] = undefined;
          }
        } else {
          newResult[${k}] = ${id}.value;
        }
        
      `);
      } else if (!isOptionalIn) {
        doc.write(`
        const ${id}_present = ${k} in input;
        if (${id}.issues.length) {
          payload.issues = payload.issues.concat(${id}.issues.map(iss => ({
            ...iss,
            path: iss.path ? [${k}, ...iss.path] : [${k}]
          })));
        }
        if (!${id}_present && !${id}.issues.length) {
          payload.issues.push({
            code: "invalid_type",
            expected: "nonoptional",
            input: undefined,
            path: [${k}]
          });
        }

        if (${id}_present) {
          if (${id}.value === undefined) {
            newResult[${k}] = undefined;
          } else {
            newResult[${k}] = ${id}.value;
          }
        }

      `);
      } else {
        doc.write(`
        if (${id}.issues.length) {
          payload.issues = payload.issues.concat(${id}.issues.map(iss => ({
            ...iss,
            path: iss.path ? [${k}, ...iss.path] : [${k}]
          })));
        }
        
        if (${id}.value === undefined) {
          if (${k} in input) {
            newResult[${k}] = undefined;
          }
        } else {
          newResult[${k}] = ${id}.value;
        }
        
      `);
      }
    }
    doc.write(`payload.value = newResult;`);
    doc.write(`return payload;`);
    const fn = doc.compile();
    return (payload, ctx) => fn(shape, payload, ctx);
  };
  let fastpass;
  const isObject2 = isObject;
  const jit = !globalConfig.jitless;
  const allowsEval2 = allowsEval;
  const fastEnabled = jit && allowsEval2.value;
  const catchall = def.catchall;
  let value;
  inst._zod.parse = (payload, ctx) => {
    value ?? (value = _normalized.value);
    const input = payload.value;
    if (!isObject2(input)) {
      payload.issues.push({
        expected: "object",
        code: "invalid_type",
        input,
        inst
      });
      return payload;
    }
    if (jit && fastEnabled && ctx?.async === false && ctx.jitless !== true) {
      if (!fastpass)
        fastpass = generateFastpass(def.shape);
      payload = fastpass(payload, ctx);
      if (!catchall)
        return payload;
      return handleCatchall([], input, payload, ctx, value, inst);
    }
    return superParse(payload, ctx);
  };
});
function handleUnionResults(results, final, inst, ctx) {
  for (const result of results) {
    if (result.issues.length === 0) {
      final.value = result.value;
      return final;
    }
  }
  const nonaborted = results.filter((r) => !aborted(r));
  if (nonaborted.length === 1) {
    final.value = nonaborted[0].value;
    return nonaborted[0];
  }
  final.issues.push({
    code: "invalid_union",
    input: final.value,
    inst,
    errors: results.map((result) => result.issues.map((iss) => finalizeIssue(iss, ctx, config())))
  });
  return final;
}
var $ZodUnion = /* @__PURE__ */ $constructor("$ZodUnion", (inst, def) => {
  $ZodType.init(inst, def);
  defineLazy(inst._zod, "optin", () => def.options.some((o) => o._zod.optin === "optional") ? "optional" : void 0);
  defineLazy(inst._zod, "optout", () => def.options.some((o) => o._zod.optout === "optional") ? "optional" : void 0);
  defineLazy(inst._zod, "values", () => {
    if (def.options.every((o) => o._zod.values)) {
      return new Set(def.options.flatMap((option) => Array.from(option._zod.values)));
    }
    return void 0;
  });
  defineLazy(inst._zod, "pattern", () => {
    if (def.options.every((o) => o._zod.pattern)) {
      const patterns = def.options.map((o) => o._zod.pattern);
      return new RegExp(`^(${patterns.map((p) => cleanRegex(p.source)).join("|")})$`);
    }
    return void 0;
  });
  const first = def.options.length === 1 ? def.options[0]._zod.run : null;
  inst._zod.parse = (payload, ctx) => {
    if (first) {
      return first(payload, ctx);
    }
    let async = false;
    const results = [];
    for (const option of def.options) {
      const result = option._zod.run({
        value: payload.value,
        issues: []
      }, ctx);
      if (result instanceof Promise) {
        results.push(result);
        async = true;
      } else {
        if (result.issues.length === 0)
          return result;
        results.push(result);
      }
    }
    if (!async)
      return handleUnionResults(results, payload, inst, ctx);
    return Promise.all(results).then((results2) => {
      return handleUnionResults(results2, payload, inst, ctx);
    });
  };
});
function handleExclusiveUnionResults(results, final, inst, ctx) {
  const successes = results.filter((r) => r.issues.length === 0);
  if (successes.length === 1) {
    final.value = successes[0].value;
    return final;
  }
  if (successes.length === 0) {
    final.issues.push({
      code: "invalid_union",
      input: final.value,
      inst,
      errors: results.map((result) => result.issues.map((iss) => finalizeIssue(iss, ctx, config())))
    });
  } else {
    final.issues.push({
      code: "invalid_union",
      input: final.value,
      inst,
      errors: [],
      inclusive: false
    });
  }
  return final;
}
var $ZodXor = /* @__PURE__ */ $constructor("$ZodXor", (inst, def) => {
  $ZodUnion.init(inst, def);
  def.inclusive = false;
  const first = def.options.length === 1 ? def.options[0]._zod.run : null;
  inst._zod.parse = (payload, ctx) => {
    if (first) {
      return first(payload, ctx);
    }
    let async = false;
    const results = [];
    for (const option of def.options) {
      const result = option._zod.run({
        value: payload.value,
        issues: []
      }, ctx);
      if (result instanceof Promise) {
        results.push(result);
        async = true;
      } else {
        results.push(result);
      }
    }
    if (!async)
      return handleExclusiveUnionResults(results, payload, inst, ctx);
    return Promise.all(results).then((results2) => {
      return handleExclusiveUnionResults(results2, payload, inst, ctx);
    });
  };
});
var $ZodDiscriminatedUnion = /* @__PURE__ */ $constructor("$ZodDiscriminatedUnion", (inst, def) => {
  def.inclusive = false;
  $ZodUnion.init(inst, def);
  const _super = inst._zod.parse;
  defineLazy(inst._zod, "propValues", () => {
    const propValues = {};
    for (const option of def.options) {
      const pv = option._zod.propValues;
      if (!pv || Object.keys(pv).length === 0)
        throw new Error(`Invalid discriminated union option at index "${def.options.indexOf(option)}"`);
      for (const [k, v] of Object.entries(pv)) {
        if (!propValues[k])
          propValues[k] = /* @__PURE__ */ new Set();
        for (const val of v) {
          propValues[k].add(val);
        }
      }
    }
    return propValues;
  });
  const disc = cached(() => {
    const opts = def.options;
    const map2 = /* @__PURE__ */ new Map();
    for (const o of opts) {
      const values = o._zod.propValues?.[def.discriminator];
      if (!values || values.size === 0)
        throw new Error(`Invalid discriminated union option at index "${def.options.indexOf(o)}"`);
      for (const v of values) {
        if (map2.has(v)) {
          throw new Error(`Duplicate discriminator value "${String(v)}"`);
        }
        map2.set(v, o);
      }
    }
    return map2;
  });
  inst._zod.parse = (payload, ctx) => {
    const input = payload.value;
    if (!isObject(input)) {
      payload.issues.push({
        code: "invalid_type",
        expected: "object",
        input,
        inst
      });
      return payload;
    }
    const opt = disc.value.get(input?.[def.discriminator]);
    if (opt) {
      return opt._zod.run(payload, ctx);
    }
    if (def.unionFallback || ctx.direction === "backward") {
      return _super(payload, ctx);
    }
    payload.issues.push({
      code: "invalid_union",
      errors: [],
      note: "No matching discriminator",
      discriminator: def.discriminator,
      options: Array.from(disc.value.keys()),
      input,
      path: [def.discriminator],
      inst
    });
    return payload;
  };
});
var $ZodIntersection = /* @__PURE__ */ $constructor("$ZodIntersection", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, ctx) => {
    const input = payload.value;
    const left = def.left._zod.run({ value: input, issues: [] }, ctx);
    const right = def.right._zod.run({ value: input, issues: [] }, ctx);
    const async = left instanceof Promise || right instanceof Promise;
    if (async) {
      return Promise.all([left, right]).then(([left2, right2]) => {
        return handleIntersectionResults(payload, left2, right2);
      });
    }
    return handleIntersectionResults(payload, left, right);
  };
});
function mergeValues(a, b) {
  if (a === b) {
    return { valid: true, data: a };
  }
  if (a instanceof Date && b instanceof Date && +a === +b) {
    return { valid: true, data: a };
  }
  if (isPlainObject(a) && isPlainObject(b)) {
    const bKeys = Object.keys(b);
    const sharedKeys = Object.keys(a).filter((key) => bKeys.indexOf(key) !== -1);
    const newObj = { ...a, ...b };
    for (const key of sharedKeys) {
      const sharedValue = mergeValues(a[key], b[key]);
      if (!sharedValue.valid) {
        return {
          valid: false,
          mergeErrorPath: [key, ...sharedValue.mergeErrorPath]
        };
      }
      newObj[key] = sharedValue.data;
    }
    return { valid: true, data: newObj };
  }
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) {
      return { valid: false, mergeErrorPath: [] };
    }
    const newArray = [];
    for (let index = 0; index < a.length; index++) {
      const itemA = a[index];
      const itemB = b[index];
      const sharedValue = mergeValues(itemA, itemB);
      if (!sharedValue.valid) {
        return {
          valid: false,
          mergeErrorPath: [index, ...sharedValue.mergeErrorPath]
        };
      }
      newArray.push(sharedValue.data);
    }
    return { valid: true, data: newArray };
  }
  return { valid: false, mergeErrorPath: [] };
}
function handleIntersectionResults(result, left, right) {
  const unrecKeys = /* @__PURE__ */ new Map();
  let unrecIssue;
  for (const iss of left.issues) {
    if (iss.code === "unrecognized_keys") {
      unrecIssue ?? (unrecIssue = iss);
      for (const k of iss.keys) {
        if (!unrecKeys.has(k))
          unrecKeys.set(k, {});
        unrecKeys.get(k).l = true;
      }
    } else {
      result.issues.push(iss);
    }
  }
  for (const iss of right.issues) {
    if (iss.code === "unrecognized_keys") {
      for (const k of iss.keys) {
        if (!unrecKeys.has(k))
          unrecKeys.set(k, {});
        unrecKeys.get(k).r = true;
      }
    } else {
      result.issues.push(iss);
    }
  }
  const bothKeys = [...unrecKeys].filter(([, f]) => f.l && f.r).map(([k]) => k);
  if (bothKeys.length && unrecIssue) {
    result.issues.push({ ...unrecIssue, keys: bothKeys });
  }
  if (aborted(result))
    return result;
  const merged = mergeValues(left.value, right.value);
  if (!merged.valid) {
    throw new Error(`Unmergable intersection. Error path: ${JSON.stringify(merged.mergeErrorPath)}`);
  }
  result.value = merged.data;
  return result;
}
var $ZodTuple = /* @__PURE__ */ $constructor("$ZodTuple", (inst, def) => {
  $ZodType.init(inst, def);
  const items = def.items;
  inst._zod.parse = (payload, ctx) => {
    const input = payload.value;
    if (!Array.isArray(input)) {
      payload.issues.push({
        input,
        inst,
        expected: "tuple",
        code: "invalid_type"
      });
      return payload;
    }
    payload.value = [];
    const proms = [];
    const optinStart = getTupleOptStart(items, "optin");
    const optoutStart = getTupleOptStart(items, "optout");
    if (!def.rest) {
      if (input.length < optinStart) {
        payload.issues.push({
          code: "too_small",
          minimum: optinStart,
          inclusive: true,
          input,
          inst,
          origin: "array"
        });
        return payload;
      }
      if (input.length > items.length) {
        payload.issues.push({
          code: "too_big",
          maximum: items.length,
          inclusive: true,
          input,
          inst,
          origin: "array"
        });
      }
    }
    const itemResults = new Array(items.length);
    for (let i = 0; i < items.length; i++) {
      const r = items[i]._zod.run({ value: input[i], issues: [] }, ctx);
      if (r instanceof Promise) {
        proms.push(r.then((rr) => {
          itemResults[i] = rr;
        }));
      } else {
        itemResults[i] = r;
      }
    }
    if (def.rest) {
      let i = items.length - 1;
      const rest = input.slice(items.length);
      for (const el of rest) {
        i++;
        const result = def.rest._zod.run({ value: el, issues: [] }, ctx);
        if (result instanceof Promise) {
          proms.push(result.then((r) => handleTupleResult(r, payload, i)));
        } else {
          handleTupleResult(result, payload, i);
        }
      }
    }
    if (proms.length) {
      return Promise.all(proms).then(() => handleTupleResults(itemResults, payload, items, input, optoutStart));
    }
    return handleTupleResults(itemResults, payload, items, input, optoutStart);
  };
});
function getTupleOptStart(items, key) {
  for (let i = items.length - 1; i >= 0; i--) {
    if (items[i]._zod[key] !== "optional")
      return i + 1;
  }
  return 0;
}
function handleTupleResult(result, final, index) {
  if (result.issues.length) {
    final.issues.push(...prefixIssues(index, result.issues));
  }
  final.value[index] = result.value;
}
function handleTupleResults(itemResults, final, items, input, optoutStart) {
  for (let i = 0; i < items.length; i++) {
    const r = itemResults[i];
    const isPresent = i < input.length;
    if (r.issues.length) {
      if (!isPresent && i >= optoutStart) {
        final.value.length = i;
        break;
      }
      final.issues.push(...prefixIssues(i, r.issues));
    }
    final.value[i] = r.value;
  }
  for (let i = final.value.length - 1; i >= input.length; i--) {
    if (items[i]._zod.optout === "optional" && final.value[i] === void 0) {
      final.value.length = i;
    } else {
      break;
    }
  }
  return final;
}
var $ZodRecord = /* @__PURE__ */ $constructor("$ZodRecord", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, ctx) => {
    const input = payload.value;
    if (!isPlainObject(input)) {
      payload.issues.push({
        expected: "record",
        code: "invalid_type",
        input,
        inst
      });
      return payload;
    }
    const proms = [];
    const values = def.keyType._zod.values;
    if (values) {
      payload.value = {};
      const recordKeys = /* @__PURE__ */ new Set();
      for (const key of values) {
        if (typeof key === "string" || typeof key === "number" || typeof key === "symbol") {
          recordKeys.add(typeof key === "number" ? key.toString() : key);
          const keyResult = def.keyType._zod.run({ value: key, issues: [] }, ctx);
          if (keyResult instanceof Promise) {
            throw new Error("Async schemas not supported in object keys currently");
          }
          if (keyResult.issues.length) {
            payload.issues.push({
              code: "invalid_key",
              origin: "record",
              issues: keyResult.issues.map((iss) => finalizeIssue(iss, ctx, config())),
              input: key,
              path: [key],
              inst
            });
            continue;
          }
          const outKey = keyResult.value;
          const result = def.valueType._zod.run({ value: input[key], issues: [] }, ctx);
          if (result instanceof Promise) {
            proms.push(result.then((result2) => {
              if (result2.issues.length) {
                payload.issues.push(...prefixIssues(key, result2.issues));
              }
              payload.value[outKey] = result2.value;
            }));
          } else {
            if (result.issues.length) {
              payload.issues.push(...prefixIssues(key, result.issues));
            }
            payload.value[outKey] = result.value;
          }
        }
      }
      let unrecognized;
      for (const key in input) {
        if (!recordKeys.has(key)) {
          unrecognized = unrecognized ?? [];
          unrecognized.push(key);
        }
      }
      if (unrecognized && unrecognized.length > 0) {
        payload.issues.push({
          code: "unrecognized_keys",
          input,
          inst,
          keys: unrecognized
        });
      }
    } else {
      payload.value = {};
      for (const key of Reflect.ownKeys(input)) {
        if (key === "__proto__")
          continue;
        if (!Object.prototype.propertyIsEnumerable.call(input, key))
          continue;
        let keyResult = def.keyType._zod.run({ value: key, issues: [] }, ctx);
        if (keyResult instanceof Promise) {
          throw new Error("Async schemas not supported in object keys currently");
        }
        const checkNumericKey = typeof key === "string" && number.test(key) && keyResult.issues.length;
        if (checkNumericKey) {
          const retryResult = def.keyType._zod.run({ value: Number(key), issues: [] }, ctx);
          if (retryResult instanceof Promise) {
            throw new Error("Async schemas not supported in object keys currently");
          }
          if (retryResult.issues.length === 0) {
            keyResult = retryResult;
          }
        }
        if (keyResult.issues.length) {
          if (def.mode === "loose") {
            payload.value[key] = input[key];
          } else {
            payload.issues.push({
              code: "invalid_key",
              origin: "record",
              issues: keyResult.issues.map((iss) => finalizeIssue(iss, ctx, config())),
              input: key,
              path: [key],
              inst
            });
          }
          continue;
        }
        const result = def.valueType._zod.run({ value: input[key], issues: [] }, ctx);
        if (result instanceof Promise) {
          proms.push(result.then((result2) => {
            if (result2.issues.length) {
              payload.issues.push(...prefixIssues(key, result2.issues));
            }
            payload.value[keyResult.value] = result2.value;
          }));
        } else {
          if (result.issues.length) {
            payload.issues.push(...prefixIssues(key, result.issues));
          }
          payload.value[keyResult.value] = result.value;
        }
      }
    }
    if (proms.length) {
      return Promise.all(proms).then(() => payload);
    }
    return payload;
  };
});
var $ZodMap = /* @__PURE__ */ $constructor("$ZodMap", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, ctx) => {
    const input = payload.value;
    if (!(input instanceof Map)) {
      payload.issues.push({
        expected: "map",
        code: "invalid_type",
        input,
        inst
      });
      return payload;
    }
    const proms = [];
    payload.value = /* @__PURE__ */ new Map();
    for (const [key, value] of input) {
      const keyResult = def.keyType._zod.run({ value: key, issues: [] }, ctx);
      const valueResult = def.valueType._zod.run({ value, issues: [] }, ctx);
      if (keyResult instanceof Promise || valueResult instanceof Promise) {
        proms.push(Promise.all([keyResult, valueResult]).then(([keyResult2, valueResult2]) => {
          handleMapResult(keyResult2, valueResult2, payload, key, input, inst, ctx);
        }));
      } else {
        handleMapResult(keyResult, valueResult, payload, key, input, inst, ctx);
      }
    }
    if (proms.length)
      return Promise.all(proms).then(() => payload);
    return payload;
  };
});
function handleMapResult(keyResult, valueResult, final, key, input, inst, ctx) {
  if (keyResult.issues.length) {
    if (propertyKeyTypes.has(typeof key)) {
      final.issues.push(...prefixIssues(key, keyResult.issues));
    } else {
      final.issues.push({
        code: "invalid_key",
        origin: "map",
        input,
        inst,
        issues: keyResult.issues.map((iss) => finalizeIssue(iss, ctx, config()))
      });
    }
  }
  if (valueResult.issues.length) {
    if (propertyKeyTypes.has(typeof key)) {
      final.issues.push(...prefixIssues(key, valueResult.issues));
    } else {
      final.issues.push({
        origin: "map",
        code: "invalid_element",
        input,
        inst,
        key,
        issues: valueResult.issues.map((iss) => finalizeIssue(iss, ctx, config()))
      });
    }
  }
  final.value.set(keyResult.value, valueResult.value);
}
var $ZodSet = /* @__PURE__ */ $constructor("$ZodSet", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, ctx) => {
    const input = payload.value;
    if (!(input instanceof Set)) {
      payload.issues.push({
        input,
        inst,
        expected: "set",
        code: "invalid_type"
      });
      return payload;
    }
    const proms = [];
    payload.value = /* @__PURE__ */ new Set();
    for (const item of input) {
      const result = def.valueType._zod.run({ value: item, issues: [] }, ctx);
      if (result instanceof Promise) {
        proms.push(result.then((result2) => handleSetResult(result2, payload)));
      } else
        handleSetResult(result, payload);
    }
    if (proms.length)
      return Promise.all(proms).then(() => payload);
    return payload;
  };
});
function handleSetResult(result, final) {
  if (result.issues.length) {
    final.issues.push(...result.issues);
  }
  final.value.add(result.value);
}
var $ZodEnum = /* @__PURE__ */ $constructor("$ZodEnum", (inst, def) => {
  $ZodType.init(inst, def);
  const values = getEnumValues(def.entries);
  const valuesSet = new Set(values);
  inst._zod.values = valuesSet;
  inst._zod.pattern = new RegExp(`^(${values.filter((k) => propertyKeyTypes.has(typeof k)).map((o) => typeof o === "string" ? escapeRegex(o) : o.toString()).join("|")})$`);
  inst._zod.parse = (payload, _ctx) => {
    const input = payload.value;
    if (valuesSet.has(input)) {
      return payload;
    }
    payload.issues.push({
      code: "invalid_value",
      values,
      input,
      inst
    });
    return payload;
  };
});
var $ZodLiteral = /* @__PURE__ */ $constructor("$ZodLiteral", (inst, def) => {
  $ZodType.init(inst, def);
  if (def.values.length === 0) {
    throw new Error("Cannot create literal schema with no valid values");
  }
  const values = new Set(def.values);
  inst._zod.values = values;
  inst._zod.pattern = new RegExp(`^(${def.values.map((o) => typeof o === "string" ? escapeRegex(o) : o ? escapeRegex(o.toString()) : String(o)).join("|")})$`);
  inst._zod.parse = (payload, _ctx) => {
    const input = payload.value;
    if (values.has(input)) {
      return payload;
    }
    payload.issues.push({
      code: "invalid_value",
      values: def.values,
      input,
      inst
    });
    return payload;
  };
});
var $ZodFile = /* @__PURE__ */ $constructor("$ZodFile", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, _ctx) => {
    const input = payload.value;
    if (input instanceof File)
      return payload;
    payload.issues.push({
      expected: "file",
      code: "invalid_type",
      input,
      inst
    });
    return payload;
  };
});
var $ZodTransform = /* @__PURE__ */ $constructor("$ZodTransform", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.optin = "optional";
  inst._zod.parse = (payload, ctx) => {
    if (ctx.direction === "backward") {
      throw new $ZodEncodeError(inst.constructor.name);
    }
    const _out = def.transform(payload.value, payload);
    if (ctx.async) {
      const output = _out instanceof Promise ? _out : Promise.resolve(_out);
      return output.then((output2) => {
        payload.value = output2;
        payload.fallback = true;
        return payload;
      });
    }
    if (_out instanceof Promise) {
      throw new $ZodAsyncError();
    }
    payload.value = _out;
    payload.fallback = true;
    return payload;
  };
});
function handleOptionalResult(result, input) {
  if (input === void 0 && (result.issues.length || result.fallback)) {
    return { issues: [], value: void 0 };
  }
  return result;
}
var $ZodOptional = /* @__PURE__ */ $constructor("$ZodOptional", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.optin = "optional";
  inst._zod.optout = "optional";
  defineLazy(inst._zod, "values", () => {
    return def.innerType._zod.values ? /* @__PURE__ */ new Set([...def.innerType._zod.values, void 0]) : void 0;
  });
  defineLazy(inst._zod, "pattern", () => {
    const pattern = def.innerType._zod.pattern;
    return pattern ? new RegExp(`^(${cleanRegex(pattern.source)})?$`) : void 0;
  });
  inst._zod.parse = (payload, ctx) => {
    if (def.innerType._zod.optin === "optional") {
      const input = payload.value;
      const result = def.innerType._zod.run(payload, ctx);
      if (result instanceof Promise)
        return result.then((r) => handleOptionalResult(r, input));
      return handleOptionalResult(result, input);
    }
    if (payload.value === void 0) {
      return payload;
    }
    return def.innerType._zod.run(payload, ctx);
  };
});
var $ZodExactOptional = /* @__PURE__ */ $constructor("$ZodExactOptional", (inst, def) => {
  $ZodOptional.init(inst, def);
  defineLazy(inst._zod, "values", () => def.innerType._zod.values);
  defineLazy(inst._zod, "pattern", () => def.innerType._zod.pattern);
  inst._zod.parse = (payload, ctx) => {
    return def.innerType._zod.run(payload, ctx);
  };
});
var $ZodNullable = /* @__PURE__ */ $constructor("$ZodNullable", (inst, def) => {
  $ZodType.init(inst, def);
  defineLazy(inst._zod, "optin", () => def.innerType._zod.optin);
  defineLazy(inst._zod, "optout", () => def.innerType._zod.optout);
  defineLazy(inst._zod, "pattern", () => {
    const pattern = def.innerType._zod.pattern;
    return pattern ? new RegExp(`^(${cleanRegex(pattern.source)}|null)$`) : void 0;
  });
  defineLazy(inst._zod, "values", () => {
    return def.innerType._zod.values ? /* @__PURE__ */ new Set([...def.innerType._zod.values, null]) : void 0;
  });
  inst._zod.parse = (payload, ctx) => {
    if (payload.value === null)
      return payload;
    return def.innerType._zod.run(payload, ctx);
  };
});
var $ZodDefault = /* @__PURE__ */ $constructor("$ZodDefault", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.optin = "optional";
  defineLazy(inst._zod, "values", () => def.innerType._zod.values);
  inst._zod.parse = (payload, ctx) => {
    if (ctx.direction === "backward") {
      return def.innerType._zod.run(payload, ctx);
    }
    if (payload.value === void 0) {
      payload.value = def.defaultValue;
      return payload;
    }
    const result = def.innerType._zod.run(payload, ctx);
    if (result instanceof Promise) {
      return result.then((result2) => handleDefaultResult(result2, def));
    }
    return handleDefaultResult(result, def);
  };
});
function handleDefaultResult(payload, def) {
  if (payload.value === void 0) {
    payload.value = def.defaultValue;
  }
  return payload;
}
var $ZodPrefault = /* @__PURE__ */ $constructor("$ZodPrefault", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.optin = "optional";
  defineLazy(inst._zod, "values", () => def.innerType._zod.values);
  inst._zod.parse = (payload, ctx) => {
    if (ctx.direction === "backward") {
      return def.innerType._zod.run(payload, ctx);
    }
    if (payload.value === void 0) {
      payload.value = def.defaultValue;
    }
    return def.innerType._zod.run(payload, ctx);
  };
});
var $ZodNonOptional = /* @__PURE__ */ $constructor("$ZodNonOptional", (inst, def) => {
  $ZodType.init(inst, def);
  defineLazy(inst._zod, "values", () => {
    const v = def.innerType._zod.values;
    return v ? new Set([...v].filter((x) => x !== void 0)) : void 0;
  });
  inst._zod.parse = (payload, ctx) => {
    const result = def.innerType._zod.run(payload, ctx);
    if (result instanceof Promise) {
      return result.then((result2) => handleNonOptionalResult(result2, inst));
    }
    return handleNonOptionalResult(result, inst);
  };
});
function handleNonOptionalResult(payload, inst) {
  if (!payload.issues.length && payload.value === void 0) {
    payload.issues.push({
      code: "invalid_type",
      expected: "nonoptional",
      input: payload.value,
      inst
    });
  }
  return payload;
}
var $ZodSuccess = /* @__PURE__ */ $constructor("$ZodSuccess", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, ctx) => {
    if (ctx.direction === "backward") {
      throw new $ZodEncodeError("ZodSuccess");
    }
    const result = def.innerType._zod.run(payload, ctx);
    if (result instanceof Promise) {
      return result.then((result2) => {
        payload.value = result2.issues.length === 0;
        return payload;
      });
    }
    payload.value = result.issues.length === 0;
    return payload;
  };
});
var $ZodCatch = /* @__PURE__ */ $constructor("$ZodCatch", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.optin = "optional";
  defineLazy(inst._zod, "optout", () => def.innerType._zod.optout);
  defineLazy(inst._zod, "values", () => def.innerType._zod.values);
  inst._zod.parse = (payload, ctx) => {
    if (ctx.direction === "backward") {
      return def.innerType._zod.run(payload, ctx);
    }
    const result = def.innerType._zod.run(payload, ctx);
    if (result instanceof Promise) {
      return result.then((result2) => {
        payload.value = result2.value;
        if (result2.issues.length) {
          payload.value = def.catchValue({
            ...payload,
            error: {
              issues: result2.issues.map((iss) => finalizeIssue(iss, ctx, config()))
            },
            input: payload.value
          });
          payload.issues = [];
          payload.fallback = true;
        }
        return payload;
      });
    }
    payload.value = result.value;
    if (result.issues.length) {
      payload.value = def.catchValue({
        ...payload,
        error: {
          issues: result.issues.map((iss) => finalizeIssue(iss, ctx, config()))
        },
        input: payload.value
      });
      payload.issues = [];
      payload.fallback = true;
    }
    return payload;
  };
});
var $ZodNaN = /* @__PURE__ */ $constructor("$ZodNaN", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, _ctx) => {
    if (typeof payload.value !== "number" || !Number.isNaN(payload.value)) {
      payload.issues.push({
        input: payload.value,
        inst,
        expected: "nan",
        code: "invalid_type"
      });
      return payload;
    }
    return payload;
  };
});
var $ZodPipe = /* @__PURE__ */ $constructor("$ZodPipe", (inst, def) => {
  $ZodType.init(inst, def);
  defineLazy(inst._zod, "values", () => def.in._zod.values);
  defineLazy(inst._zod, "optin", () => def.in._zod.optin);
  defineLazy(inst._zod, "optout", () => def.out._zod.optout);
  defineLazy(inst._zod, "propValues", () => def.in._zod.propValues);
  inst._zod.parse = (payload, ctx) => {
    if (ctx.direction === "backward") {
      const right = def.out._zod.run(payload, ctx);
      if (right instanceof Promise) {
        return right.then((right2) => handlePipeResult(right2, def.in, ctx));
      }
      return handlePipeResult(right, def.in, ctx);
    }
    const left = def.in._zod.run(payload, ctx);
    if (left instanceof Promise) {
      return left.then((left2) => handlePipeResult(left2, def.out, ctx));
    }
    return handlePipeResult(left, def.out, ctx);
  };
});
function handlePipeResult(left, next, ctx) {
  if (left.issues.length) {
    left.aborted = true;
    return left;
  }
  return next._zod.run({ value: left.value, issues: left.issues, fallback: left.fallback }, ctx);
}
var $ZodCodec = /* @__PURE__ */ $constructor("$ZodCodec", (inst, def) => {
  $ZodType.init(inst, def);
  defineLazy(inst._zod, "values", () => def.in._zod.values);
  defineLazy(inst._zod, "optin", () => def.in._zod.optin);
  defineLazy(inst._zod, "optout", () => def.out._zod.optout);
  defineLazy(inst._zod, "propValues", () => def.in._zod.propValues);
  inst._zod.parse = (payload, ctx) => {
    const direction = ctx.direction || "forward";
    if (direction === "forward") {
      const left = def.in._zod.run(payload, ctx);
      if (left instanceof Promise) {
        return left.then((left2) => handleCodecAResult(left2, def, ctx));
      }
      return handleCodecAResult(left, def, ctx);
    } else {
      const right = def.out._zod.run(payload, ctx);
      if (right instanceof Promise) {
        return right.then((right2) => handleCodecAResult(right2, def, ctx));
      }
      return handleCodecAResult(right, def, ctx);
    }
  };
});
function handleCodecAResult(result, def, ctx) {
  if (result.issues.length) {
    result.aborted = true;
    return result;
  }
  const direction = ctx.direction || "forward";
  if (direction === "forward") {
    const transformed = def.transform(result.value, result);
    if (transformed instanceof Promise) {
      return transformed.then((value) => handleCodecTxResult(result, value, def.out, ctx));
    }
    return handleCodecTxResult(result, transformed, def.out, ctx);
  } else {
    const transformed = def.reverseTransform(result.value, result);
    if (transformed instanceof Promise) {
      return transformed.then((value) => handleCodecTxResult(result, value, def.in, ctx));
    }
    return handleCodecTxResult(result, transformed, def.in, ctx);
  }
}
function handleCodecTxResult(left, value, nextSchema, ctx) {
  if (left.issues.length) {
    left.aborted = true;
    return left;
  }
  return nextSchema._zod.run({ value, issues: left.issues }, ctx);
}
var $ZodPreprocess = /* @__PURE__ */ $constructor("$ZodPreprocess", (inst, def) => {
  $ZodPipe.init(inst, def);
});
var $ZodReadonly = /* @__PURE__ */ $constructor("$ZodReadonly", (inst, def) => {
  $ZodType.init(inst, def);
  defineLazy(inst._zod, "propValues", () => def.innerType._zod.propValues);
  defineLazy(inst._zod, "values", () => def.innerType._zod.values);
  defineLazy(inst._zod, "optin", () => def.innerType?._zod?.optin);
  defineLazy(inst._zod, "optout", () => def.innerType?._zod?.optout);
  inst._zod.parse = (payload, ctx) => {
    if (ctx.direction === "backward") {
      return def.innerType._zod.run(payload, ctx);
    }
    const result = def.innerType._zod.run(payload, ctx);
    if (result instanceof Promise) {
      return result.then(handleReadonlyResult);
    }
    return handleReadonlyResult(result);
  };
});
function handleReadonlyResult(payload) {
  payload.value = Object.freeze(payload.value);
  return payload;
}
var $ZodTemplateLiteral = /* @__PURE__ */ $constructor("$ZodTemplateLiteral", (inst, def) => {
  $ZodType.init(inst, def);
  const regexParts = [];
  for (const part of def.parts) {
    if (typeof part === "object" && part !== null) {
      if (!part._zod.pattern) {
        throw new Error(`Invalid template literal part, no pattern found: ${[...part._zod.traits].shift()}`);
      }
      const source = part._zod.pattern instanceof RegExp ? part._zod.pattern.source : part._zod.pattern;
      if (!source)
        throw new Error(`Invalid template literal part: ${part._zod.traits}`);
      const start = source.startsWith("^") ? 1 : 0;
      const end = source.endsWith("$") ? source.length - 1 : source.length;
      regexParts.push(source.slice(start, end));
    } else if (part === null || primitiveTypes.has(typeof part)) {
      regexParts.push(escapeRegex(`${part}`));
    } else {
      throw new Error(`Invalid template literal part: ${part}`);
    }
  }
  inst._zod.pattern = new RegExp(`^${regexParts.join("")}$`);
  inst._zod.parse = (payload, _ctx) => {
    if (typeof payload.value !== "string") {
      payload.issues.push({
        input: payload.value,
        inst,
        expected: "string",
        code: "invalid_type"
      });
      return payload;
    }
    inst._zod.pattern.lastIndex = 0;
    if (!inst._zod.pattern.test(payload.value)) {
      payload.issues.push({
        input: payload.value,
        inst,
        code: "invalid_format",
        format: def.format ?? "template_literal",
        pattern: inst._zod.pattern.source
      });
      return payload;
    }
    return payload;
  };
});
var $ZodFunction = /* @__PURE__ */ $constructor("$ZodFunction", (inst, def) => {
  $ZodType.init(inst, def);
  inst._def = def;
  inst._zod.def = def;
  inst.implement = (func) => {
    if (typeof func !== "function") {
      throw new Error("implement() must be called with a function");
    }
    return function(...args) {
      const parsedArgs = inst._def.input ? parse(inst._def.input, args) : args;
      const result = Reflect.apply(func, this, parsedArgs);
      if (inst._def.output) {
        return parse(inst._def.output, result);
      }
      return result;
    };
  };
  inst.implementAsync = (func) => {
    if (typeof func !== "function") {
      throw new Error("implementAsync() must be called with a function");
    }
    return async function(...args) {
      const parsedArgs = inst._def.input ? await parseAsync(inst._def.input, args) : args;
      const result = await Reflect.apply(func, this, parsedArgs);
      if (inst._def.output) {
        return await parseAsync(inst._def.output, result);
      }
      return result;
    };
  };
  inst._zod.parse = (payload, _ctx) => {
    if (typeof payload.value !== "function") {
      payload.issues.push({
        code: "invalid_type",
        expected: "function",
        input: payload.value,
        inst
      });
      return payload;
    }
    const hasPromiseOutput = inst._def.output && inst._def.output._zod.def.type === "promise";
    if (hasPromiseOutput) {
      payload.value = inst.implementAsync(payload.value);
    } else {
      payload.value = inst.implement(payload.value);
    }
    return payload;
  };
  inst.input = (...args) => {
    const F = inst.constructor;
    if (Array.isArray(args[0])) {
      return new F({
        type: "function",
        input: new $ZodTuple({
          type: "tuple",
          items: args[0],
          rest: args[1]
        }),
        output: inst._def.output
      });
    }
    return new F({
      type: "function",
      input: args[0],
      output: inst._def.output
    });
  };
  inst.output = (output) => {
    const F = inst.constructor;
    return new F({
      type: "function",
      input: inst._def.input,
      output
    });
  };
  return inst;
});
var $ZodPromise = /* @__PURE__ */ $constructor("$ZodPromise", (inst, def) => {
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, ctx) => {
    return Promise.resolve(payload.value).then((inner) => def.innerType._zod.run({ value: inner, issues: [] }, ctx));
  };
});
var $ZodLazy = /* @__PURE__ */ $constructor("$ZodLazy", (inst, def) => {
  $ZodType.init(inst, def);
  defineLazy(inst._zod, "innerType", () => {
    const d = def;
    if (!d._cachedInner)
      d._cachedInner = def.getter();
    return d._cachedInner;
  });
  defineLazy(inst._zod, "pattern", () => inst._zod.innerType?._zod?.pattern);
  defineLazy(inst._zod, "propValues", () => inst._zod.innerType?._zod?.propValues);
  defineLazy(inst._zod, "optin", () => inst._zod.innerType?._zod?.optin ?? void 0);
  defineLazy(inst._zod, "optout", () => inst._zod.innerType?._zod?.optout ?? void 0);
  inst._zod.parse = (payload, ctx) => {
    const inner = inst._zod.innerType;
    return inner._zod.run(payload, ctx);
  };
});
var $ZodCustom = /* @__PURE__ */ $constructor("$ZodCustom", (inst, def) => {
  $ZodCheck.init(inst, def);
  $ZodType.init(inst, def);
  inst._zod.parse = (payload, _) => {
    return payload;
  };
  inst._zod.check = (payload) => {
    const input = payload.value;
    const r = def.fn(input);
    if (r instanceof Promise) {
      return r.then((r2) => handleRefineResult(r2, payload, input, inst));
    }
    handleRefineResult(r, payload, input, inst);
    return;
  };
});
function handleRefineResult(result, payload, input, inst) {
  if (!result) {
    const _iss = {
      code: "custom",
      input,
      inst,
      // incorporates params.error into issue reporting
      path: [...inst._zod.def.path ?? []],
      // incorporates params.error into issue reporting
      continue: !inst._zod.def.abort
      // params: inst._zod.def.params,
    };
    if (inst._zod.def.params)
      _iss.params = inst._zod.def.params;
    payload.issues.push(issue(_iss));
  }
}

// node_modules/zod/v4/locales/index.js
var locales_exports = {};
__export(locales_exports, {
  ar: () => ar_default,
  az: () => az_default,
  be: () => be_default,
  bg: () => bg_default,
  ca: () => ca_default,
  cs: () => cs_default,
  da: () => da_default,
  de: () => de_default,
  el: () => el_default,
  en: () => en_default,
  eo: () => eo_default,
  es: () => es_default,
  fa: () => fa_default,
  fi: () => fi_default,
  fr: () => fr_default,
  frCA: () => fr_CA_default,
  he: () => he_default,
  hr: () => hr_default,
  hu: () => hu_default,
  hy: () => hy_default,
  id: () => id_default,
  is: () => is_default,
  it: () => it_default,
  ja: () => ja_default,
  ka: () => ka_default,
  kh: () => kh_default,
  km: () => km_default,
  ko: () => ko_default,
  lt: () => lt_default,
  mk: () => mk_default,
  ms: () => ms_default,
  nl: () => nl_default,
  no: () => no_default,
  ota: () => ota_default,
  pl: () => pl_default,
  ps: () => ps_default,
  pt: () => pt_default,
  ro: () => ro_default,
  ru: () => ru_default,
  sl: () => sl_default,
  sv: () => sv_default,
  ta: () => ta_default,
  th: () => th_default,
  tr: () => tr_default,
  ua: () => ua_default,
  uk: () => uk_default,
  ur: () => ur_default,
  uz: () => uz_default,
  vi: () => vi_default,
  yo: () => yo_default,
  zhCN: () => zh_CN_default,
  zhTW: () => zh_TW_default
});

// node_modules/zod/v4/locales/ar.js
var error = () => {
  const Sizable = {
    string: { unit: "\u062D\u0631\u0641", verb: "\u0623\u0646 \u064A\u062D\u0648\u064A" },
    file: { unit: "\u0628\u0627\u064A\u062A", verb: "\u0623\u0646 \u064A\u062D\u0648\u064A" },
    array: { unit: "\u0639\u0646\u0635\u0631", verb: "\u0623\u0646 \u064A\u062D\u0648\u064A" },
    set: { unit: "\u0639\u0646\u0635\u0631", verb: "\u0623\u0646 \u064A\u062D\u0648\u064A" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0645\u062F\u062E\u0644",
    email: "\u0628\u0631\u064A\u062F \u0625\u0644\u0643\u062A\u0631\u0648\u0646\u064A",
    url: "\u0631\u0627\u0628\u0637",
    emoji: "\u0625\u064A\u0645\u0648\u062C\u064A",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "\u062A\u0627\u0631\u064A\u062E \u0648\u0648\u0642\u062A \u0628\u0645\u0639\u064A\u0627\u0631 ISO",
    date: "\u062A\u0627\u0631\u064A\u062E \u0628\u0645\u0639\u064A\u0627\u0631 ISO",
    time: "\u0648\u0642\u062A \u0628\u0645\u0639\u064A\u0627\u0631 ISO",
    duration: "\u0645\u062F\u0629 \u0628\u0645\u0639\u064A\u0627\u0631 ISO",
    ipv4: "\u0639\u0646\u0648\u0627\u0646 IPv4",
    ipv6: "\u0639\u0646\u0648\u0627\u0646 IPv6",
    cidrv4: "\u0645\u062F\u0649 \u0639\u0646\u0627\u0648\u064A\u0646 \u0628\u0635\u064A\u063A\u0629 IPv4",
    cidrv6: "\u0645\u062F\u0649 \u0639\u0646\u0627\u0648\u064A\u0646 \u0628\u0635\u064A\u063A\u0629 IPv6",
    base64: "\u0646\u064E\u0635 \u0628\u062A\u0631\u0645\u064A\u0632 base64-encoded",
    base64url: "\u0646\u064E\u0635 \u0628\u062A\u0631\u0645\u064A\u0632 base64url-encoded",
    json_string: "\u0646\u064E\u0635 \u0639\u0644\u0649 \u0647\u064A\u0626\u0629 JSON",
    e164: "\u0631\u0642\u0645 \u0647\u0627\u062A\u0641 \u0628\u0645\u0639\u064A\u0627\u0631 E.164",
    jwt: "JWT",
    template_literal: "\u0645\u062F\u062E\u0644"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u0645\u062F\u062E\u0644\u0627\u062A \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644\u0629: \u064A\u0641\u062A\u0631\u0636 \u0625\u062F\u062E\u0627\u0644 instanceof ${issue2.expected}\u060C \u0648\u0644\u0643\u0646 \u062A\u0645 \u0625\u062F\u062E\u0627\u0644 ${received}`;
        }
        return `\u0645\u062F\u062E\u0644\u0627\u062A \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644\u0629: \u064A\u0641\u062A\u0631\u0636 \u0625\u062F\u062E\u0627\u0644 ${expected}\u060C \u0648\u0644\u0643\u0646 \u062A\u0645 \u0625\u062F\u062E\u0627\u0644 ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u0645\u062F\u062E\u0644\u0627\u062A \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644\u0629: \u064A\u0641\u062A\u0631\u0636 \u0625\u062F\u062E\u0627\u0644 ${stringifyPrimitive(issue2.values[0])}`;
        return `\u0627\u062E\u062A\u064A\u0627\u0631 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644: \u064A\u062A\u0648\u0642\u0639 \u0627\u0646\u062A\u0642\u0627\u0621 \u0623\u062D\u062F \u0647\u0630\u0647 \u0627\u0644\u062E\u064A\u0627\u0631\u0627\u062A: ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return ` \u0623\u0643\u0628\u0631 \u0645\u0646 \u0627\u0644\u0644\u0627\u0632\u0645: \u064A\u0641\u062A\u0631\u0636 \u0623\u0646 \u062A\u0643\u0648\u0646 ${issue2.origin ?? "\u0627\u0644\u0642\u064A\u0645\u0629"} ${adj} ${issue2.maximum.toString()} ${sizing.unit ?? "\u0639\u0646\u0635\u0631"}`;
        return `\u0623\u0643\u0628\u0631 \u0645\u0646 \u0627\u0644\u0644\u0627\u0632\u0645: \u064A\u0641\u062A\u0631\u0636 \u0623\u0646 \u062A\u0643\u0648\u0646 ${issue2.origin ?? "\u0627\u0644\u0642\u064A\u0645\u0629"} ${adj} ${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u0623\u0635\u063A\u0631 \u0645\u0646 \u0627\u0644\u0644\u0627\u0632\u0645: \u064A\u0641\u062A\u0631\u0636 \u0644\u0640 ${issue2.origin} \u0623\u0646 \u064A\u0643\u0648\u0646 ${adj} ${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u0623\u0635\u063A\u0631 \u0645\u0646 \u0627\u0644\u0644\u0627\u0632\u0645: \u064A\u0641\u062A\u0631\u0636 \u0644\u0640 ${issue2.origin} \u0623\u0646 \u064A\u0643\u0648\u0646 ${adj} ${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u0646\u064E\u0635 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644: \u064A\u062C\u0628 \u0623\u0646 \u064A\u0628\u062F\u0623 \u0628\u0640 "${issue2.prefix}"`;
        if (_issue.format === "ends_with")
          return `\u0646\u064E\u0635 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644: \u064A\u062C\u0628 \u0623\u0646 \u064A\u0646\u062A\u0647\u064A \u0628\u0640 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u0646\u064E\u0635 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644: \u064A\u062C\u0628 \u0623\u0646 \u064A\u062A\u0636\u0645\u0651\u064E\u0646 "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u0646\u064E\u0635 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644: \u064A\u062C\u0628 \u0623\u0646 \u064A\u0637\u0627\u0628\u0642 \u0627\u0644\u0646\u0645\u0637 ${_issue.pattern}`;
        return `${FormatDictionary[_issue.format] ?? issue2.format} \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644`;
      }
      case "not_multiple_of":
        return `\u0631\u0642\u0645 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644: \u064A\u062C\u0628 \u0623\u0646 \u064A\u0643\u0648\u0646 \u0645\u0646 \u0645\u0636\u0627\u0639\u0641\u0627\u062A ${issue2.divisor}`;
      case "unrecognized_keys":
        return `\u0645\u0639\u0631\u0641${issue2.keys.length > 1 ? "\u0627\u062A" : ""} \u063A\u0631\u064A\u0628${issue2.keys.length > 1 ? "\u0629" : ""}: ${joinValues(issue2.keys, "\u060C ")}`;
      case "invalid_key":
        return `\u0645\u0639\u0631\u0641 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644 \u0641\u064A ${issue2.origin}`;
      case "invalid_union":
        return "\u0645\u062F\u062E\u0644 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644";
      case "invalid_element":
        return `\u0645\u062F\u062E\u0644 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644 \u0641\u064A ${issue2.origin}`;
      default:
        return "\u0645\u062F\u062E\u0644 \u063A\u064A\u0631 \u0645\u0642\u0628\u0648\u0644";
    }
  };
};
function ar_default() {
  return {
    localeError: error()
  };
}

// node_modules/zod/v4/locales/az.js
var error2 = () => {
  const Sizable = {
    string: { unit: "simvol", verb: "olmal\u0131d\u0131r" },
    file: { unit: "bayt", verb: "olmal\u0131d\u0131r" },
    array: { unit: "element", verb: "olmal\u0131d\u0131r" },
    set: { unit: "element", verb: "olmal\u0131d\u0131r" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "input",
    email: "email address",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO datetime",
    date: "ISO date",
    time: "ISO time",
    duration: "ISO duration",
    ipv4: "IPv4 address",
    ipv6: "IPv6 address",
    cidrv4: "IPv4 range",
    cidrv6: "IPv6 range",
    base64: "base64-encoded string",
    base64url: "base64url-encoded string",
    json_string: "JSON string",
    e164: "E.164 number",
    jwt: "JWT",
    template_literal: "input"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Yanl\u0131\u015F d\u0259y\u0259r: g\xF6zl\u0259nil\u0259n instanceof ${issue2.expected}, daxil olan ${received}`;
        }
        return `Yanl\u0131\u015F d\u0259y\u0259r: g\xF6zl\u0259nil\u0259n ${expected}, daxil olan ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Yanl\u0131\u015F d\u0259y\u0259r: g\xF6zl\u0259nil\u0259n ${stringifyPrimitive(issue2.values[0])}`;
        return `Yanl\u0131\u015F se\xE7im: a\u015Fa\u011F\u0131dak\u0131lardan biri olmal\u0131d\u0131r: ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\xC7ox b\xF6y\xFCk: g\xF6zl\u0259nil\u0259n ${issue2.origin ?? "d\u0259y\u0259r"} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "element"}`;
        return `\xC7ox b\xF6y\xFCk: g\xF6zl\u0259nil\u0259n ${issue2.origin ?? "d\u0259y\u0259r"} ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\xC7ox ki\xE7ik: g\xF6zl\u0259nil\u0259n ${issue2.origin} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        return `\xC7ox ki\xE7ik: g\xF6zl\u0259nil\u0259n ${issue2.origin} ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Yanl\u0131\u015F m\u0259tn: "${_issue.prefix}" il\u0259 ba\u015Flamal\u0131d\u0131r`;
        if (_issue.format === "ends_with")
          return `Yanl\u0131\u015F m\u0259tn: "${_issue.suffix}" il\u0259 bitm\u0259lidir`;
        if (_issue.format === "includes")
          return `Yanl\u0131\u015F m\u0259tn: "${_issue.includes}" daxil olmal\u0131d\u0131r`;
        if (_issue.format === "regex")
          return `Yanl\u0131\u015F m\u0259tn: ${_issue.pattern} \u015Fablonuna uy\u011Fun olmal\u0131d\u0131r`;
        return `Yanl\u0131\u015F ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Yanl\u0131\u015F \u0259d\u0259d: ${issue2.divisor} il\u0259 b\xF6l\xFCn\u0259 bil\u0259n olmal\u0131d\u0131r`;
      case "unrecognized_keys":
        return `Tan\u0131nmayan a\xE7ar${issue2.keys.length > 1 ? "lar" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `${issue2.origin} daxilind\u0259 yanl\u0131\u015F a\xE7ar`;
      case "invalid_union":
        return "Yanl\u0131\u015F d\u0259y\u0259r";
      case "invalid_element":
        return `${issue2.origin} daxilind\u0259 yanl\u0131\u015F d\u0259y\u0259r`;
      default:
        return `Yanl\u0131\u015F d\u0259y\u0259r`;
    }
  };
};
function az_default() {
  return {
    localeError: error2()
  };
}

// node_modules/zod/v4/locales/be.js
function getBelarusianPlural(count, one, few, many) {
  const absCount = Math.abs(count);
  const lastDigit = absCount % 10;
  const lastTwoDigits = absCount % 100;
  if (lastTwoDigits >= 11 && lastTwoDigits <= 19) {
    return many;
  }
  if (lastDigit === 1) {
    return one;
  }
  if (lastDigit >= 2 && lastDigit <= 4) {
    return few;
  }
  return many;
}
var error3 = () => {
  const Sizable = {
    string: {
      unit: {
        one: "\u0441\u0456\u043C\u0432\u0430\u043B",
        few: "\u0441\u0456\u043C\u0432\u0430\u043B\u044B",
        many: "\u0441\u0456\u043C\u0432\u0430\u043B\u0430\u045E"
      },
      verb: "\u043C\u0435\u0446\u044C"
    },
    array: {
      unit: {
        one: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442",
        few: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442\u044B",
        many: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442\u0430\u045E"
      },
      verb: "\u043C\u0435\u0446\u044C"
    },
    set: {
      unit: {
        one: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442",
        few: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442\u044B",
        many: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442\u0430\u045E"
      },
      verb: "\u043C\u0435\u0446\u044C"
    },
    file: {
      unit: {
        one: "\u0431\u0430\u0439\u0442",
        few: "\u0431\u0430\u0439\u0442\u044B",
        many: "\u0431\u0430\u0439\u0442\u0430\u045E"
      },
      verb: "\u043C\u0435\u0446\u044C"
    }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0443\u0432\u043E\u0434",
    email: "email \u0430\u0434\u0440\u0430\u0441",
    url: "URL",
    emoji: "\u044D\u043C\u043E\u0434\u0437\u0456",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO \u0434\u0430\u0442\u0430 \u0456 \u0447\u0430\u0441",
    date: "ISO \u0434\u0430\u0442\u0430",
    time: "ISO \u0447\u0430\u0441",
    duration: "ISO \u043F\u0440\u0430\u0446\u044F\u0433\u043B\u0430\u0441\u0446\u044C",
    ipv4: "IPv4 \u0430\u0434\u0440\u0430\u0441",
    ipv6: "IPv6 \u0430\u0434\u0440\u0430\u0441",
    cidrv4: "IPv4 \u0434\u044B\u044F\u043F\u0430\u0437\u043E\u043D",
    cidrv6: "IPv6 \u0434\u044B\u044F\u043F\u0430\u0437\u043E\u043D",
    base64: "\u0440\u0430\u0434\u043E\u043A \u0443 \u0444\u0430\u0440\u043C\u0430\u0446\u0435 base64",
    base64url: "\u0440\u0430\u0434\u043E\u043A \u0443 \u0444\u0430\u0440\u043C\u0430\u0446\u0435 base64url",
    json_string: "JSON \u0440\u0430\u0434\u043E\u043A",
    e164: "\u043D\u0443\u043C\u0430\u0440 E.164",
    jwt: "JWT",
    template_literal: "\u0443\u0432\u043E\u0434"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u043B\u0456\u043A",
    array: "\u043C\u0430\u0441\u0456\u045E"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u045E\u0432\u043E\u0434: \u0447\u0430\u043A\u0430\u045E\u0441\u044F instanceof ${issue2.expected}, \u0430\u0442\u0440\u044B\u043C\u0430\u043D\u0430 ${received}`;
        }
        return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u045E\u0432\u043E\u0434: \u0447\u0430\u043A\u0430\u045E\u0441\u044F ${expected}, \u0430\u0442\u0440\u044B\u043C\u0430\u043D\u0430 ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u045E\u0432\u043E\u0434: \u0447\u0430\u043A\u0430\u043B\u0430\u0441\u044F ${stringifyPrimitive(issue2.values[0])}`;
        return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u0432\u0430\u0440\u044B\u044F\u043D\u0442: \u0447\u0430\u043A\u0430\u045E\u0441\u044F \u0430\u0434\u0437\u0456\u043D \u0437 ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          const maxValue = Number(issue2.maximum);
          const unit = getBelarusianPlural(maxValue, sizing.unit.one, sizing.unit.few, sizing.unit.many);
          return `\u0417\u0430\u043D\u0430\u0434\u0442\u0430 \u0432\u044F\u043B\u0456\u043A\u0456: \u0447\u0430\u043A\u0430\u043B\u0430\u0441\u044F, \u0448\u0442\u043E ${issue2.origin ?? "\u0437\u043D\u0430\u0447\u044D\u043D\u043D\u0435"} \u043F\u0430\u0432\u0456\u043D\u043D\u0430 ${sizing.verb} ${adj}${issue2.maximum.toString()} ${unit}`;
        }
        return `\u0417\u0430\u043D\u0430\u0434\u0442\u0430 \u0432\u044F\u043B\u0456\u043A\u0456: \u0447\u0430\u043A\u0430\u043B\u0430\u0441\u044F, \u0448\u0442\u043E ${issue2.origin ?? "\u0437\u043D\u0430\u0447\u044D\u043D\u043D\u0435"} \u043F\u0430\u0432\u0456\u043D\u043D\u0430 \u0431\u044B\u0446\u044C ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          const minValue = Number(issue2.minimum);
          const unit = getBelarusianPlural(minValue, sizing.unit.one, sizing.unit.few, sizing.unit.many);
          return `\u0417\u0430\u043D\u0430\u0434\u0442\u0430 \u043C\u0430\u043B\u044B: \u0447\u0430\u043A\u0430\u043B\u0430\u0441\u044F, \u0448\u0442\u043E ${issue2.origin} \u043F\u0430\u0432\u0456\u043D\u043D\u0430 ${sizing.verb} ${adj}${issue2.minimum.toString()} ${unit}`;
        }
        return `\u0417\u0430\u043D\u0430\u0434\u0442\u0430 \u043C\u0430\u043B\u044B: \u0447\u0430\u043A\u0430\u043B\u0430\u0441\u044F, \u0448\u0442\u043E ${issue2.origin} \u043F\u0430\u0432\u0456\u043D\u043D\u0430 \u0431\u044B\u0446\u044C ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u0440\u0430\u0434\u043E\u043A: \u043F\u0430\u0432\u0456\u043D\u0435\u043D \u043F\u0430\u0447\u044B\u043D\u0430\u0446\u0446\u0430 \u0437 "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u0440\u0430\u0434\u043E\u043A: \u043F\u0430\u0432\u0456\u043D\u0435\u043D \u0437\u0430\u043A\u0430\u043D\u0447\u0432\u0430\u0446\u0446\u0430 \u043D\u0430 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u0440\u0430\u0434\u043E\u043A: \u043F\u0430\u0432\u0456\u043D\u0435\u043D \u0437\u043C\u044F\u0448\u0447\u0430\u0446\u044C "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u0440\u0430\u0434\u043E\u043A: \u043F\u0430\u0432\u0456\u043D\u0435\u043D \u0430\u0434\u043F\u0430\u0432\u044F\u0434\u0430\u0446\u044C \u0448\u0430\u0431\u043B\u043E\u043D\u0443 ${_issue.pattern}`;
        return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u043B\u0456\u043A: \u043F\u0430\u0432\u0456\u043D\u0435\u043D \u0431\u044B\u0446\u044C \u043A\u0440\u0430\u0442\u043D\u044B\u043C ${issue2.divisor}`;
      case "unrecognized_keys":
        return `\u041D\u0435\u0440\u0430\u0441\u043F\u0430\u0437\u043D\u0430\u043D\u044B ${issue2.keys.length > 1 ? "\u043A\u043B\u044E\u0447\u044B" : "\u043A\u043B\u044E\u0447"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u043A\u043B\u044E\u0447 \u0443 ${issue2.origin}`;
      case "invalid_union":
        return "\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u045E\u0432\u043E\u0434";
      case "invalid_element":
        return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u0430\u0435 \u0437\u043D\u0430\u0447\u044D\u043D\u043D\u0435 \u045E ${issue2.origin}`;
      default:
        return `\u041D\u044F\u043F\u0440\u0430\u0432\u0456\u043B\u044C\u043D\u044B \u045E\u0432\u043E\u0434`;
    }
  };
};
function be_default() {
  return {
    localeError: error3()
  };
}

// node_modules/zod/v4/locales/bg.js
var error4 = () => {
  const Sizable = {
    string: { unit: "\u0441\u0438\u043C\u0432\u043E\u043B\u0430", verb: "\u0434\u0430 \u0441\u044A\u0434\u044A\u0440\u0436\u0430" },
    file: { unit: "\u0431\u0430\u0439\u0442\u0430", verb: "\u0434\u0430 \u0441\u044A\u0434\u044A\u0440\u0436\u0430" },
    array: { unit: "\u0435\u043B\u0435\u043C\u0435\u043D\u0442\u0430", verb: "\u0434\u0430 \u0441\u044A\u0434\u044A\u0440\u0436\u0430" },
    set: { unit: "\u0435\u043B\u0435\u043C\u0435\u043D\u0442\u0430", verb: "\u0434\u0430 \u0441\u044A\u0434\u044A\u0440\u0436\u0430" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0432\u0445\u043E\u0434",
    email: "\u0438\u043C\u0435\u0439\u043B \u0430\u0434\u0440\u0435\u0441",
    url: "URL",
    emoji: "\u0435\u043C\u043E\u0434\u0436\u0438",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO \u0432\u0440\u0435\u043C\u0435",
    date: "ISO \u0434\u0430\u0442\u0430",
    time: "ISO \u0432\u0440\u0435\u043C\u0435",
    duration: "ISO \u043F\u0440\u043E\u0434\u044A\u043B\u0436\u0438\u0442\u0435\u043B\u043D\u043E\u0441\u0442",
    ipv4: "IPv4 \u0430\u0434\u0440\u0435\u0441",
    ipv6: "IPv6 \u0430\u0434\u0440\u0435\u0441",
    cidrv4: "IPv4 \u0434\u0438\u0430\u043F\u0430\u0437\u043E\u043D",
    cidrv6: "IPv6 \u0434\u0438\u0430\u043F\u0430\u0437\u043E\u043D",
    base64: "base64-\u043A\u043E\u0434\u0438\u0440\u0430\u043D \u043D\u0438\u0437",
    base64url: "base64url-\u043A\u043E\u0434\u0438\u0440\u0430\u043D \u043D\u0438\u0437",
    json_string: "JSON \u043D\u0438\u0437",
    e164: "E.164 \u043D\u043E\u043C\u0435\u0440",
    jwt: "JWT",
    template_literal: "\u0432\u0445\u043E\u0434"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0447\u0438\u0441\u043B\u043E",
    array: "\u043C\u0430\u0441\u0438\u0432"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u0432\u0445\u043E\u0434: \u043E\u0447\u0430\u043A\u0432\u0430\u043D instanceof ${issue2.expected}, \u043F\u043E\u043B\u0443\u0447\u0435\u043D ${received}`;
        }
        return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u0432\u0445\u043E\u0434: \u043E\u0447\u0430\u043A\u0432\u0430\u043D ${expected}, \u043F\u043E\u043B\u0443\u0447\u0435\u043D ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u0432\u0445\u043E\u0434: \u043E\u0447\u0430\u043A\u0432\u0430\u043D ${stringifyPrimitive(issue2.values[0])}`;
        return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u043D\u0430 \u043E\u043F\u0446\u0438\u044F: \u043E\u0447\u0430\u043A\u0432\u0430\u043D\u043E \u0435\u0434\u043D\u043E \u043E\u0442 ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u0422\u0432\u044A\u0440\u0434\u0435 \u0433\u043E\u043B\u044F\u043C\u043E: \u043E\u0447\u0430\u043A\u0432\u0430 \u0441\u0435 ${issue2.origin ?? "\u0441\u0442\u043E\u0439\u043D\u043E\u0441\u0442"} \u0434\u0430 \u0441\u044A\u0434\u044A\u0440\u0436\u0430 ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u0435\u043B\u0435\u043C\u0435\u043D\u0442\u0430"}`;
        return `\u0422\u0432\u044A\u0440\u0434\u0435 \u0433\u043E\u043B\u044F\u043C\u043E: \u043E\u0447\u0430\u043A\u0432\u0430 \u0441\u0435 ${issue2.origin ?? "\u0441\u0442\u043E\u0439\u043D\u043E\u0441\u0442"} \u0434\u0430 \u0431\u044A\u0434\u0435 ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u0422\u0432\u044A\u0440\u0434\u0435 \u043C\u0430\u043B\u043A\u043E: \u043E\u0447\u0430\u043A\u0432\u0430 \u0441\u0435 ${issue2.origin} \u0434\u0430 \u0441\u044A\u0434\u044A\u0440\u0436\u0430 ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u0422\u0432\u044A\u0440\u0434\u0435 \u043C\u0430\u043B\u043A\u043E: \u043E\u0447\u0430\u043A\u0432\u0430 \u0441\u0435 ${issue2.origin} \u0434\u0430 \u0431\u044A\u0434\u0435 ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u043D\u0438\u0437: \u0442\u0440\u044F\u0431\u0432\u0430 \u0434\u0430 \u0437\u0430\u043F\u043E\u0447\u0432\u0430 \u0441 "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u043D\u0438\u0437: \u0442\u0440\u044F\u0431\u0432\u0430 \u0434\u0430 \u0437\u0430\u0432\u044A\u0440\u0448\u0432\u0430 \u0441 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u043D\u0438\u0437: \u0442\u0440\u044F\u0431\u0432\u0430 \u0434\u0430 \u0432\u043A\u043B\u044E\u0447\u0432\u0430 "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u043D\u0438\u0437: \u0442\u0440\u044F\u0431\u0432\u0430 \u0434\u0430 \u0441\u044A\u0432\u043F\u0430\u0434\u0430 \u0441 ${_issue.pattern}`;
        let invalid_adj = "\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D";
        if (_issue.format === "emoji")
          invalid_adj = "\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u043D\u043E";
        if (_issue.format === "datetime")
          invalid_adj = "\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u043D\u043E";
        if (_issue.format === "date")
          invalid_adj = "\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u043D\u0430";
        if (_issue.format === "time")
          invalid_adj = "\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u043D\u043E";
        if (_issue.format === "duration")
          invalid_adj = "\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u043D\u0430";
        return `${invalid_adj} ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u043D\u043E \u0447\u0438\u0441\u043B\u043E: \u0442\u0440\u044F\u0431\u0432\u0430 \u0434\u0430 \u0431\u044A\u0434\u0435 \u043A\u0440\u0430\u0442\u043D\u043E \u043D\u0430 ${issue2.divisor}`;
      case "unrecognized_keys":
        return `\u041D\u0435\u0440\u0430\u0437\u043F\u043E\u0437\u043D\u0430\u0442${issue2.keys.length > 1 ? "\u0438" : ""} \u043A\u043B\u044E\u0447${issue2.keys.length > 1 ? "\u043E\u0432\u0435" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u043A\u043B\u044E\u0447 \u0432 ${issue2.origin}`;
      case "invalid_union":
        return "\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u0432\u0445\u043E\u0434";
      case "invalid_element":
        return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u043D\u0430 \u0441\u0442\u043E\u0439\u043D\u043E\u0441\u0442 \u0432 ${issue2.origin}`;
      default:
        return `\u041D\u0435\u0432\u0430\u043B\u0438\u0434\u0435\u043D \u0432\u0445\u043E\u0434`;
    }
  };
};
function bg_default() {
  return {
    localeError: error4()
  };
}

// node_modules/zod/v4/locales/ca.js
var error5 = () => {
  const Sizable = {
    string: { unit: "car\xE0cters", verb: "contenir" },
    file: { unit: "bytes", verb: "contenir" },
    array: { unit: "elements", verb: "contenir" },
    set: { unit: "elements", verb: "contenir" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "entrada",
    email: "adre\xE7a electr\xF2nica",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "data i hora ISO",
    date: "data ISO",
    time: "hora ISO",
    duration: "durada ISO",
    ipv4: "adre\xE7a IPv4",
    ipv6: "adre\xE7a IPv6",
    cidrv4: "rang IPv4",
    cidrv6: "rang IPv6",
    base64: "cadena codificada en base64",
    base64url: "cadena codificada en base64url",
    json_string: "cadena JSON",
    e164: "n\xFAmero E.164",
    jwt: "JWT",
    template_literal: "entrada"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Tipus inv\xE0lid: s'esperava instanceof ${issue2.expected}, s'ha rebut ${received}`;
        }
        return `Tipus inv\xE0lid: s'esperava ${expected}, s'ha rebut ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Valor inv\xE0lid: s'esperava ${stringifyPrimitive(issue2.values[0])}`;
        return `Opci\xF3 inv\xE0lida: s'esperava una de ${joinValues(issue2.values, " o ")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "com a m\xE0xim" : "menys de";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Massa gran: s'esperava que ${issue2.origin ?? "el valor"} contingu\xE9s ${adj} ${issue2.maximum.toString()} ${sizing.unit ?? "elements"}`;
        return `Massa gran: s'esperava que ${issue2.origin ?? "el valor"} fos ${adj} ${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? "com a m\xEDnim" : "m\xE9s de";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Massa petit: s'esperava que ${issue2.origin} contingu\xE9s ${adj} ${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Massa petit: s'esperava que ${issue2.origin} fos ${adj} ${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `Format inv\xE0lid: ha de comen\xE7ar amb "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `Format inv\xE0lid: ha d'acabar amb "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Format inv\xE0lid: ha d'incloure "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Format inv\xE0lid: ha de coincidir amb el patr\xF3 ${_issue.pattern}`;
        return `Format inv\xE0lid per a ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `N\xFAmero inv\xE0lid: ha de ser m\xFAltiple de ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Clau${issue2.keys.length > 1 ? "s" : ""} no reconeguda${issue2.keys.length > 1 ? "s" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Clau inv\xE0lida a ${issue2.origin}`;
      case "invalid_union":
        return "Entrada inv\xE0lida";
      // Could also be "Tipus d'unió invàlid" but "Entrada invàlida" is more general
      case "invalid_element":
        return `Element inv\xE0lid a ${issue2.origin}`;
      default:
        return `Entrada inv\xE0lida`;
    }
  };
};
function ca_default() {
  return {
    localeError: error5()
  };
}

// node_modules/zod/v4/locales/cs.js
var error6 = () => {
  const Sizable = {
    string: { unit: "znak\u016F", verb: "m\xEDt" },
    file: { unit: "bajt\u016F", verb: "m\xEDt" },
    array: { unit: "prvk\u016F", verb: "m\xEDt" },
    set: { unit: "prvk\u016F", verb: "m\xEDt" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "regul\xE1rn\xED v\xFDraz",
    email: "e-mailov\xE1 adresa",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "datum a \u010Das ve form\xE1tu ISO",
    date: "datum ve form\xE1tu ISO",
    time: "\u010Das ve form\xE1tu ISO",
    duration: "doba trv\xE1n\xED ISO",
    ipv4: "IPv4 adresa",
    ipv6: "IPv6 adresa",
    cidrv4: "rozsah IPv4",
    cidrv6: "rozsah IPv6",
    base64: "\u0159et\u011Bzec zak\xF3dovan\xFD ve form\xE1tu base64",
    base64url: "\u0159et\u011Bzec zak\xF3dovan\xFD ve form\xE1tu base64url",
    json_string: "\u0159et\u011Bzec ve form\xE1tu JSON",
    e164: "\u010D\xEDslo E.164",
    jwt: "JWT",
    template_literal: "vstup"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u010D\xEDslo",
    string: "\u0159et\u011Bzec",
    function: "funkce",
    array: "pole"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Neplatn\xFD vstup: o\u010Dek\xE1v\xE1no instanceof ${issue2.expected}, obdr\u017Eeno ${received}`;
        }
        return `Neplatn\xFD vstup: o\u010Dek\xE1v\xE1no ${expected}, obdr\u017Eeno ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Neplatn\xFD vstup: o\u010Dek\xE1v\xE1no ${stringifyPrimitive(issue2.values[0])}`;
        return `Neplatn\xE1 mo\u017Enost: o\u010Dek\xE1v\xE1na jedna z hodnot ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Hodnota je p\u0159\xEDli\u0161 velk\xE1: ${issue2.origin ?? "hodnota"} mus\xED m\xEDt ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "prvk\u016F"}`;
        }
        return `Hodnota je p\u0159\xEDli\u0161 velk\xE1: ${issue2.origin ?? "hodnota"} mus\xED b\xFDt ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Hodnota je p\u0159\xEDli\u0161 mal\xE1: ${issue2.origin ?? "hodnota"} mus\xED m\xEDt ${adj}${issue2.minimum.toString()} ${sizing.unit ?? "prvk\u016F"}`;
        }
        return `Hodnota je p\u0159\xEDli\u0161 mal\xE1: ${issue2.origin ?? "hodnota"} mus\xED b\xFDt ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Neplatn\xFD \u0159et\u011Bzec: mus\xED za\u010D\xEDnat na "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Neplatn\xFD \u0159et\u011Bzec: mus\xED kon\u010Dit na "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Neplatn\xFD \u0159et\u011Bzec: mus\xED obsahovat "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Neplatn\xFD \u0159et\u011Bzec: mus\xED odpov\xEDdat vzoru ${_issue.pattern}`;
        return `Neplatn\xFD form\xE1t ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Neplatn\xE9 \u010D\xEDslo: mus\xED b\xFDt n\xE1sobkem ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Nezn\xE1m\xE9 kl\xED\u010De: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Neplatn\xFD kl\xED\u010D v ${issue2.origin}`;
      case "invalid_union":
        return "Neplatn\xFD vstup";
      case "invalid_element":
        return `Neplatn\xE1 hodnota v ${issue2.origin}`;
      default:
        return `Neplatn\xFD vstup`;
    }
  };
};
function cs_default() {
  return {
    localeError: error6()
  };
}

// node_modules/zod/v4/locales/da.js
var error7 = () => {
  const Sizable = {
    string: { unit: "tegn", verb: "havde" },
    file: { unit: "bytes", verb: "havde" },
    array: { unit: "elementer", verb: "indeholdt" },
    set: { unit: "elementer", verb: "indeholdt" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "input",
    email: "e-mailadresse",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO dato- og klokkesl\xE6t",
    date: "ISO-dato",
    time: "ISO-klokkesl\xE6t",
    duration: "ISO-varighed",
    ipv4: "IPv4-omr\xE5de",
    ipv6: "IPv6-omr\xE5de",
    cidrv4: "IPv4-spektrum",
    cidrv6: "IPv6-spektrum",
    base64: "base64-kodet streng",
    base64url: "base64url-kodet streng",
    json_string: "JSON-streng",
    e164: "E.164-nummer",
    jwt: "JWT",
    template_literal: "input"
  };
  const TypeDictionary = {
    nan: "NaN",
    string: "streng",
    number: "tal",
    boolean: "boolean",
    array: "liste",
    object: "objekt",
    set: "s\xE6t",
    file: "fil"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Ugyldigt input: forventede instanceof ${issue2.expected}, fik ${received}`;
        }
        return `Ugyldigt input: forventede ${expected}, fik ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Ugyldig v\xE6rdi: forventede ${stringifyPrimitive(issue2.values[0])}`;
        return `Ugyldigt valg: forventede en af f\xF8lgende ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        const origin = TypeDictionary[issue2.origin] ?? issue2.origin;
        if (sizing)
          return `For stor: forventede ${origin ?? "value"} ${sizing.verb} ${adj} ${issue2.maximum.toString()} ${sizing.unit ?? "elementer"}`;
        return `For stor: forventede ${origin ?? "value"} havde ${adj} ${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        const origin = TypeDictionary[issue2.origin] ?? issue2.origin;
        if (sizing) {
          return `For lille: forventede ${origin} ${sizing.verb} ${adj} ${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `For lille: forventede ${origin} havde ${adj} ${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Ugyldig streng: skal starte med "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Ugyldig streng: skal ende med "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Ugyldig streng: skal indeholde "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Ugyldig streng: skal matche m\xF8nsteret ${_issue.pattern}`;
        return `Ugyldig ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Ugyldigt tal: skal v\xE6re deleligt med ${issue2.divisor}`;
      case "unrecognized_keys":
        return `${issue2.keys.length > 1 ? "Ukendte n\xF8gler" : "Ukendt n\xF8gle"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Ugyldig n\xF8gle i ${issue2.origin}`;
      case "invalid_union":
        return "Ugyldigt input: matcher ingen af de tilladte typer";
      case "invalid_element":
        return `Ugyldig v\xE6rdi i ${issue2.origin}`;
      default:
        return `Ugyldigt input`;
    }
  };
};
function da_default() {
  return {
    localeError: error7()
  };
}

// node_modules/zod/v4/locales/de.js
var error8 = () => {
  const Sizable = {
    string: { unit: "Zeichen", verb: "zu haben" },
    file: { unit: "Bytes", verb: "zu haben" },
    array: { unit: "Elemente", verb: "zu haben" },
    set: { unit: "Elemente", verb: "zu haben" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "Eingabe",
    email: "E-Mail-Adresse",
    url: "URL",
    emoji: "Emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO-Datum und -Uhrzeit",
    date: "ISO-Datum",
    time: "ISO-Uhrzeit",
    duration: "ISO-Dauer",
    ipv4: "IPv4-Adresse",
    ipv6: "IPv6-Adresse",
    cidrv4: "IPv4-Bereich",
    cidrv6: "IPv6-Bereich",
    base64: "Base64-codierter String",
    base64url: "Base64-URL-codierter String",
    json_string: "JSON-String",
    e164: "E.164-Nummer",
    jwt: "JWT",
    template_literal: "Eingabe"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "Zahl",
    array: "Array"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Ung\xFCltige Eingabe: erwartet instanceof ${issue2.expected}, erhalten ${received}`;
        }
        return `Ung\xFCltige Eingabe: erwartet ${expected}, erhalten ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Ung\xFCltige Eingabe: erwartet ${stringifyPrimitive(issue2.values[0])}`;
        return `Ung\xFCltige Option: erwartet eine von ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Zu gro\xDF: erwartet, dass ${issue2.origin ?? "Wert"} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "Elemente"} hat`;
        return `Zu gro\xDF: erwartet, dass ${issue2.origin ?? "Wert"} ${adj}${issue2.maximum.toString()} ist`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Zu klein: erwartet, dass ${issue2.origin} ${adj}${issue2.minimum.toString()} ${sizing.unit} hat`;
        }
        return `Zu klein: erwartet, dass ${issue2.origin} ${adj}${issue2.minimum.toString()} ist`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Ung\xFCltiger String: muss mit "${_issue.prefix}" beginnen`;
        if (_issue.format === "ends_with")
          return `Ung\xFCltiger String: muss mit "${_issue.suffix}" enden`;
        if (_issue.format === "includes")
          return `Ung\xFCltiger String: muss "${_issue.includes}" enthalten`;
        if (_issue.format === "regex")
          return `Ung\xFCltiger String: muss dem Muster ${_issue.pattern} entsprechen`;
        return `Ung\xFCltig: ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Ung\xFCltige Zahl: muss ein Vielfaches von ${issue2.divisor} sein`;
      case "unrecognized_keys":
        return `${issue2.keys.length > 1 ? "Unbekannte Schl\xFCssel" : "Unbekannter Schl\xFCssel"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Ung\xFCltiger Schl\xFCssel in ${issue2.origin}`;
      case "invalid_union":
        return "Ung\xFCltige Eingabe";
      case "invalid_element":
        return `Ung\xFCltiger Wert in ${issue2.origin}`;
      default:
        return `Ung\xFCltige Eingabe`;
    }
  };
};
function de_default() {
  return {
    localeError: error8()
  };
}

// node_modules/zod/v4/locales/el.js
var error9 = () => {
  const Sizable = {
    string: { unit: "\u03C7\u03B1\u03C1\u03B1\u03BA\u03C4\u03AE\u03C1\u03B5\u03C2", verb: "\u03BD\u03B1 \u03AD\u03C7\u03B5\u03B9" },
    file: { unit: "bytes", verb: "\u03BD\u03B1 \u03AD\u03C7\u03B5\u03B9" },
    array: { unit: "\u03C3\u03C4\u03BF\u03B9\u03C7\u03B5\u03AF\u03B1", verb: "\u03BD\u03B1 \u03AD\u03C7\u03B5\u03B9" },
    set: { unit: "\u03C3\u03C4\u03BF\u03B9\u03C7\u03B5\u03AF\u03B1", verb: "\u03BD\u03B1 \u03AD\u03C7\u03B5\u03B9" },
    map: { unit: "\u03BA\u03B1\u03C4\u03B1\u03C7\u03C9\u03C1\u03AE\u03C3\u03B5\u03B9\u03C2", verb: "\u03BD\u03B1 \u03AD\u03C7\u03B5\u03B9" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u03B5\u03AF\u03C3\u03BF\u03B4\u03BF\u03C2",
    email: "\u03B4\u03B9\u03B5\u03CD\u03B8\u03C5\u03BD\u03C3\u03B7 email",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO \u03B7\u03BC\u03B5\u03C1\u03BF\u03BC\u03B7\u03BD\u03AF\u03B1 \u03BA\u03B1\u03B9 \u03CE\u03C1\u03B1",
    date: "ISO \u03B7\u03BC\u03B5\u03C1\u03BF\u03BC\u03B7\u03BD\u03AF\u03B1",
    time: "ISO \u03CE\u03C1\u03B1",
    duration: "ISO \u03B4\u03B9\u03AC\u03C1\u03BA\u03B5\u03B9\u03B1",
    ipv4: "\u03B4\u03B9\u03B5\u03CD\u03B8\u03C5\u03BD\u03C3\u03B7 IPv4",
    ipv6: "\u03B4\u03B9\u03B5\u03CD\u03B8\u03C5\u03BD\u03C3\u03B7 IPv6",
    mac: "\u03B4\u03B9\u03B5\u03CD\u03B8\u03C5\u03BD\u03C3\u03B7 MAC",
    cidrv4: "\u03B5\u03CD\u03C1\u03BF\u03C2 IPv4",
    cidrv6: "\u03B5\u03CD\u03C1\u03BF\u03C2 IPv6",
    base64: "\u03C3\u03C5\u03BC\u03B2\u03BF\u03BB\u03BF\u03C3\u03B5\u03B9\u03C1\u03AC \u03BA\u03C9\u03B4\u03B9\u03BA\u03BF\u03C0\u03BF\u03B9\u03B7\u03BC\u03AD\u03BD\u03B7 \u03C3\u03B5 base64",
    base64url: "\u03C3\u03C5\u03BC\u03B2\u03BF\u03BB\u03BF\u03C3\u03B5\u03B9\u03C1\u03AC \u03BA\u03C9\u03B4\u03B9\u03BA\u03BF\u03C0\u03BF\u03B9\u03B7\u03BC\u03AD\u03BD\u03B7 \u03C3\u03B5 base64url",
    json_string: "\u03C3\u03C5\u03BC\u03B2\u03BF\u03BB\u03BF\u03C3\u03B5\u03B9\u03C1\u03AC JSON",
    e164: "\u03B1\u03C1\u03B9\u03B8\u03BC\u03CC\u03C2 E.164",
    jwt: "JWT",
    template_literal: "\u03B5\u03AF\u03C3\u03BF\u03B4\u03BF\u03C2"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (typeof issue2.expected === "string" && /^[A-Z]/.test(issue2.expected)) {
          return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03B5\u03AF\u03C3\u03BF\u03B4\u03BF\u03C2: \u03B1\u03BD\u03B1\u03BC\u03B5\u03BD\u03CC\u03C4\u03B1\u03BD instanceof ${issue2.expected}, \u03BB\u03AE\u03C6\u03B8\u03B7\u03BA\u03B5 ${received}`;
        }
        return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03B5\u03AF\u03C3\u03BF\u03B4\u03BF\u03C2: \u03B1\u03BD\u03B1\u03BC\u03B5\u03BD\u03CC\u03C4\u03B1\u03BD ${expected}, \u03BB\u03AE\u03C6\u03B8\u03B7\u03BA\u03B5 ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03B5\u03AF\u03C3\u03BF\u03B4\u03BF\u03C2: \u03B1\u03BD\u03B1\u03BC\u03B5\u03BD\u03CC\u03C4\u03B1\u03BD ${stringifyPrimitive(issue2.values[0])}`;
        return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03B5\u03C0\u03B9\u03BB\u03BF\u03B3\u03AE: \u03B1\u03BD\u03B1\u03BC\u03B5\u03BD\u03CC\u03C4\u03B1\u03BD \u03AD\u03BD\u03B1 \u03B1\u03C0\u03CC ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u03A0\u03BF\u03BB\u03CD \u03BC\u03B5\u03B3\u03AC\u03BB\u03BF: \u03B1\u03BD\u03B1\u03BC\u03B5\u03BD\u03CC\u03C4\u03B1\u03BD ${issue2.origin ?? "\u03C4\u03B9\u03BC\u03AE"} \u03BD\u03B1 \u03AD\u03C7\u03B5\u03B9 ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u03C3\u03C4\u03BF\u03B9\u03C7\u03B5\u03AF\u03B1"}`;
        return `\u03A0\u03BF\u03BB\u03CD \u03BC\u03B5\u03B3\u03AC\u03BB\u03BF: \u03B1\u03BD\u03B1\u03BC\u03B5\u03BD\u03CC\u03C4\u03B1\u03BD ${issue2.origin ?? "\u03C4\u03B9\u03BC\u03AE"} \u03BD\u03B1 \u03B5\u03AF\u03BD\u03B1\u03B9 ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u03A0\u03BF\u03BB\u03CD \u03BC\u03B9\u03BA\u03C1\u03CC: \u03B1\u03BD\u03B1\u03BC\u03B5\u03BD\u03CC\u03C4\u03B1\u03BD ${issue2.origin} \u03BD\u03B1 \u03AD\u03C7\u03B5\u03B9 ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u03A0\u03BF\u03BB\u03CD \u03BC\u03B9\u03BA\u03C1\u03CC: \u03B1\u03BD\u03B1\u03BC\u03B5\u03BD\u03CC\u03C4\u03B1\u03BD ${issue2.origin} \u03BD\u03B1 \u03B5\u03AF\u03BD\u03B1\u03B9 ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03C3\u03C5\u03BC\u03B2\u03BF\u03BB\u03BF\u03C3\u03B5\u03B9\u03C1\u03AC: \u03C0\u03C1\u03AD\u03C0\u03B5\u03B9 \u03BD\u03B1 \u03BE\u03B5\u03BA\u03B9\u03BD\u03AC \u03BC\u03B5 "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03C3\u03C5\u03BC\u03B2\u03BF\u03BB\u03BF\u03C3\u03B5\u03B9\u03C1\u03AC: \u03C0\u03C1\u03AD\u03C0\u03B5\u03B9 \u03BD\u03B1 \u03C4\u03B5\u03BB\u03B5\u03B9\u03CE\u03BD\u03B5\u03B9 \u03BC\u03B5 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03C3\u03C5\u03BC\u03B2\u03BF\u03BB\u03BF\u03C3\u03B5\u03B9\u03C1\u03AC: \u03C0\u03C1\u03AD\u03C0\u03B5\u03B9 \u03BD\u03B1 \u03C0\u03B5\u03C1\u03B9\u03AD\u03C7\u03B5\u03B9 "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03C3\u03C5\u03BC\u03B2\u03BF\u03BB\u03BF\u03C3\u03B5\u03B9\u03C1\u03AC: \u03C0\u03C1\u03AD\u03C0\u03B5\u03B9 \u03BD\u03B1 \u03C4\u03B1\u03B9\u03C1\u03B9\u03AC\u03B6\u03B5\u03B9 \u03BC\u03B5 \u03C4\u03BF \u03BC\u03BF\u03C4\u03AF\u03B2\u03BF ${_issue.pattern}`;
        return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03BF: ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03BF\u03C2 \u03B1\u03C1\u03B9\u03B8\u03BC\u03CC\u03C2: \u03C0\u03C1\u03AD\u03C0\u03B5\u03B9 \u03BD\u03B1 \u03B5\u03AF\u03BD\u03B1\u03B9 \u03C0\u03BF\u03BB\u03BB\u03B1\u03C0\u03BB\u03AC\u03C3\u03B9\u03BF \u03C4\u03BF\u03C5 ${issue2.divisor}`;
      case "unrecognized_keys":
        return `\u0386\u03B3\u03BD\u03C9\u03C3\u03C4${issue2.keys.length > 1 ? "\u03B1" : "\u03BF"} \u03BA\u03BB\u03B5\u03B9\u03B4${issue2.keys.length > 1 ? "\u03B9\u03AC" : "\u03AF"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03BF \u03BA\u03BB\u03B5\u03B9\u03B4\u03AF \u03C3\u03C4\u03BF ${issue2.origin}`;
      case "invalid_union":
        return "\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03B5\u03AF\u03C3\u03BF\u03B4\u03BF\u03C2";
      case "invalid_element":
        return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03C4\u03B9\u03BC\u03AE \u03C3\u03C4\u03BF ${issue2.origin}`;
      default:
        return `\u039C\u03B7 \u03AD\u03B3\u03BA\u03C5\u03C1\u03B7 \u03B5\u03AF\u03C3\u03BF\u03B4\u03BF\u03C2`;
    }
  };
};
function el_default() {
  return {
    localeError: error9()
  };
}

// node_modules/zod/v4/locales/en.js
var error10 = () => {
  const Sizable = {
    string: { unit: "characters", verb: "to have" },
    file: { unit: "bytes", verb: "to have" },
    array: { unit: "items", verb: "to have" },
    set: { unit: "items", verb: "to have" },
    map: { unit: "entries", verb: "to have" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "input",
    email: "email address",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO datetime",
    date: "ISO date",
    time: "ISO time",
    duration: "ISO duration",
    ipv4: "IPv4 address",
    ipv6: "IPv6 address",
    mac: "MAC address",
    cidrv4: "IPv4 range",
    cidrv6: "IPv6 range",
    base64: "base64-encoded string",
    base64url: "base64url-encoded string",
    json_string: "JSON string",
    e164: "E.164 number",
    jwt: "JWT",
    template_literal: "input"
  };
  const TypeDictionary = {
    // Compatibility: "nan" -> "NaN" for display
    nan: "NaN"
    // All other type names omitted - they fall back to raw values via ?? operator
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        return `Invalid input: expected ${expected}, received ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Invalid input: expected ${stringifyPrimitive(issue2.values[0])}`;
        return `Invalid option: expected one of ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Too big: expected ${issue2.origin ?? "value"} to have ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elements"}`;
        return `Too big: expected ${issue2.origin ?? "value"} to be ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Too small: expected ${issue2.origin} to have ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Too small: expected ${issue2.origin} to be ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `Invalid string: must start with "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `Invalid string: must end with "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Invalid string: must include "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Invalid string: must match pattern ${_issue.pattern}`;
        return `Invalid ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Invalid number: must be a multiple of ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Unrecognized key${issue2.keys.length > 1 ? "s" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Invalid key in ${issue2.origin}`;
      case "invalid_union":
        if (issue2.options && Array.isArray(issue2.options) && issue2.options.length > 0) {
          const opts = issue2.options.map((o) => `'${o}'`).join(" | ");
          return `Invalid discriminator value. Expected ${opts}`;
        }
        return "Invalid input";
      case "invalid_element":
        return `Invalid value in ${issue2.origin}`;
      default:
        return `Invalid input`;
    }
  };
};
function en_default() {
  return {
    localeError: error10()
  };
}

// node_modules/zod/v4/locales/eo.js
var error11 = () => {
  const Sizable = {
    string: { unit: "karaktrojn", verb: "havi" },
    file: { unit: "bajtojn", verb: "havi" },
    array: { unit: "elementojn", verb: "havi" },
    set: { unit: "elementojn", verb: "havi" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "enigo",
    email: "retadreso",
    url: "URL",
    emoji: "emo\u011Dio",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO-datotempo",
    date: "ISO-dato",
    time: "ISO-tempo",
    duration: "ISO-da\u016Dro",
    ipv4: "IPv4-adreso",
    ipv6: "IPv6-adreso",
    cidrv4: "IPv4-rango",
    cidrv6: "IPv6-rango",
    base64: "64-ume kodita karaktraro",
    base64url: "URL-64-ume kodita karaktraro",
    json_string: "JSON-karaktraro",
    e164: "E.164-nombro",
    jwt: "JWT",
    template_literal: "enigo"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "nombro",
    array: "tabelo",
    null: "senvalora"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Nevalida enigo: atendi\u011Dis instanceof ${issue2.expected}, ricevi\u011Dis ${received}`;
        }
        return `Nevalida enigo: atendi\u011Dis ${expected}, ricevi\u011Dis ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Nevalida enigo: atendi\u011Dis ${stringifyPrimitive(issue2.values[0])}`;
        return `Nevalida opcio: atendi\u011Dis unu el ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Tro granda: atendi\u011Dis ke ${issue2.origin ?? "valoro"} havu ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elementojn"}`;
        return `Tro granda: atendi\u011Dis ke ${issue2.origin ?? "valoro"} havu ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Tro malgranda: atendi\u011Dis ke ${issue2.origin} havu ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Tro malgranda: atendi\u011Dis ke ${issue2.origin} estu ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Nevalida karaktraro: devas komenci\u011Di per "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Nevalida karaktraro: devas fini\u011Di per "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Nevalida karaktraro: devas inkluzivi "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Nevalida karaktraro: devas kongrui kun la modelo ${_issue.pattern}`;
        return `Nevalida ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Nevalida nombro: devas esti oblo de ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Nekonata${issue2.keys.length > 1 ? "j" : ""} \u015Dlosilo${issue2.keys.length > 1 ? "j" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Nevalida \u015Dlosilo en ${issue2.origin}`;
      case "invalid_union":
        return "Nevalida enigo";
      case "invalid_element":
        return `Nevalida valoro en ${issue2.origin}`;
      default:
        return `Nevalida enigo`;
    }
  };
};
function eo_default() {
  return {
    localeError: error11()
  };
}

// node_modules/zod/v4/locales/es.js
var error12 = () => {
  const Sizable = {
    string: { unit: "caracteres", verb: "tener" },
    file: { unit: "bytes", verb: "tener" },
    array: { unit: "elementos", verb: "tener" },
    set: { unit: "elementos", verb: "tener" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "entrada",
    email: "direcci\xF3n de correo electr\xF3nico",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "fecha y hora ISO",
    date: "fecha ISO",
    time: "hora ISO",
    duration: "duraci\xF3n ISO",
    ipv4: "direcci\xF3n IPv4",
    ipv6: "direcci\xF3n IPv6",
    cidrv4: "rango IPv4",
    cidrv6: "rango IPv6",
    base64: "cadena codificada en base64",
    base64url: "URL codificada en base64",
    json_string: "cadena JSON",
    e164: "n\xFAmero E.164",
    jwt: "JWT",
    template_literal: "entrada"
  };
  const TypeDictionary = {
    nan: "NaN",
    string: "texto",
    number: "n\xFAmero",
    boolean: "booleano",
    array: "arreglo",
    object: "objeto",
    set: "conjunto",
    file: "archivo",
    date: "fecha",
    bigint: "n\xFAmero grande",
    symbol: "s\xEDmbolo",
    undefined: "indefinido",
    null: "nulo",
    function: "funci\xF3n",
    map: "mapa",
    record: "registro",
    tuple: "tupla",
    enum: "enumeraci\xF3n",
    union: "uni\xF3n",
    literal: "literal",
    promise: "promesa",
    void: "vac\xEDo",
    never: "nunca",
    unknown: "desconocido",
    any: "cualquiera"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Entrada inv\xE1lida: se esperaba instanceof ${issue2.expected}, recibido ${received}`;
        }
        return `Entrada inv\xE1lida: se esperaba ${expected}, recibido ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Entrada inv\xE1lida: se esperaba ${stringifyPrimitive(issue2.values[0])}`;
        return `Opci\xF3n inv\xE1lida: se esperaba una de ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        const origin = TypeDictionary[issue2.origin] ?? issue2.origin;
        if (sizing)
          return `Demasiado grande: se esperaba que ${origin ?? "valor"} tuviera ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elementos"}`;
        return `Demasiado grande: se esperaba que ${origin ?? "valor"} fuera ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        const origin = TypeDictionary[issue2.origin] ?? issue2.origin;
        if (sizing) {
          return `Demasiado peque\xF1o: se esperaba que ${origin} tuviera ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Demasiado peque\xF1o: se esperaba que ${origin} fuera ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Cadena inv\xE1lida: debe comenzar con "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Cadena inv\xE1lida: debe terminar en "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Cadena inv\xE1lida: debe incluir "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Cadena inv\xE1lida: debe coincidir con el patr\xF3n ${_issue.pattern}`;
        return `Inv\xE1lido ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `N\xFAmero inv\xE1lido: debe ser m\xFAltiplo de ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Llave${issue2.keys.length > 1 ? "s" : ""} desconocida${issue2.keys.length > 1 ? "s" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Llave inv\xE1lida en ${TypeDictionary[issue2.origin] ?? issue2.origin}`;
      case "invalid_union":
        return "Entrada inv\xE1lida";
      case "invalid_element":
        return `Valor inv\xE1lido en ${TypeDictionary[issue2.origin] ?? issue2.origin}`;
      default:
        return `Entrada inv\xE1lida`;
    }
  };
};
function es_default() {
  return {
    localeError: error12()
  };
}

// node_modules/zod/v4/locales/fa.js
var error13 = () => {
  const Sizable = {
    string: { unit: "\u06A9\u0627\u0631\u0627\u06A9\u062A\u0631", verb: "\u062F\u0627\u0634\u062A\u0647 \u0628\u0627\u0634\u062F" },
    file: { unit: "\u0628\u0627\u06CC\u062A", verb: "\u062F\u0627\u0634\u062A\u0647 \u0628\u0627\u0634\u062F" },
    array: { unit: "\u0622\u06CC\u062A\u0645", verb: "\u062F\u0627\u0634\u062A\u0647 \u0628\u0627\u0634\u062F" },
    set: { unit: "\u0622\u06CC\u062A\u0645", verb: "\u062F\u0627\u0634\u062A\u0647 \u0628\u0627\u0634\u062F" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0648\u0631\u0648\u062F\u06CC",
    email: "\u0622\u062F\u0631\u0633 \u0627\u06CC\u0645\u06CC\u0644",
    url: "URL",
    emoji: "\u0627\u06CC\u0645\u0648\u062C\u06CC",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "\u062A\u0627\u0631\u06CC\u062E \u0648 \u0632\u0645\u0627\u0646 \u0627\u06CC\u0632\u0648",
    date: "\u062A\u0627\u0631\u06CC\u062E \u0627\u06CC\u0632\u0648",
    time: "\u0632\u0645\u0627\u0646 \u0627\u06CC\u0632\u0648",
    duration: "\u0645\u062F\u062A \u0632\u0645\u0627\u0646 \u0627\u06CC\u0632\u0648",
    ipv4: "IPv4 \u0622\u062F\u0631\u0633",
    ipv6: "IPv6 \u0622\u062F\u0631\u0633",
    cidrv4: "IPv4 \u062F\u0627\u0645\u0646\u0647",
    cidrv6: "IPv6 \u062F\u0627\u0645\u0646\u0647",
    base64: "base64-encoded \u0631\u0634\u062A\u0647",
    base64url: "base64url-encoded \u0631\u0634\u062A\u0647",
    json_string: "JSON \u0631\u0634\u062A\u0647",
    e164: "E.164 \u0639\u062F\u062F",
    jwt: "JWT",
    template_literal: "\u0648\u0631\u0648\u062F\u06CC"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0639\u062F\u062F",
    array: "\u0622\u0631\u0627\u06CC\u0647"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u0648\u0631\u0648\u062F\u06CC \u0646\u0627\u0645\u0639\u062A\u0628\u0631: \u0645\u06CC\u200C\u0628\u0627\u06CC\u0633\u062A instanceof ${issue2.expected} \u0645\u06CC\u200C\u0628\u0648\u062F\u060C ${received} \u062F\u0631\u06CC\u0627\u0641\u062A \u0634\u062F`;
        }
        return `\u0648\u0631\u0648\u062F\u06CC \u0646\u0627\u0645\u0639\u062A\u0628\u0631: \u0645\u06CC\u200C\u0628\u0627\u06CC\u0633\u062A ${expected} \u0645\u06CC\u200C\u0628\u0648\u062F\u060C ${received} \u062F\u0631\u06CC\u0627\u0641\u062A \u0634\u062F`;
      }
      case "invalid_value":
        if (issue2.values.length === 1) {
          return `\u0648\u0631\u0648\u062F\u06CC \u0646\u0627\u0645\u0639\u062A\u0628\u0631: \u0645\u06CC\u200C\u0628\u0627\u06CC\u0633\u062A ${stringifyPrimitive(issue2.values[0])} \u0645\u06CC\u200C\u0628\u0648\u062F`;
        }
        return `\u06AF\u0632\u06CC\u0646\u0647 \u0646\u0627\u0645\u0639\u062A\u0628\u0631: \u0645\u06CC\u200C\u0628\u0627\u06CC\u0633\u062A \u06CC\u06A9\u06CC \u0627\u0632 ${joinValues(issue2.values, "|")} \u0645\u06CC\u200C\u0628\u0648\u062F`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u062E\u06CC\u0644\u06CC \u0628\u0632\u0631\u06AF: ${issue2.origin ?? "\u0645\u0642\u062F\u0627\u0631"} \u0628\u0627\u06CC\u062F ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u0639\u0646\u0635\u0631"} \u0628\u0627\u0634\u062F`;
        }
        return `\u062E\u06CC\u0644\u06CC \u0628\u0632\u0631\u06AF: ${issue2.origin ?? "\u0645\u0642\u062F\u0627\u0631"} \u0628\u0627\u06CC\u062F ${adj}${issue2.maximum.toString()} \u0628\u0627\u0634\u062F`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u062E\u06CC\u0644\u06CC \u06A9\u0648\u0686\u06A9: ${issue2.origin} \u0628\u0627\u06CC\u062F ${adj}${issue2.minimum.toString()} ${sizing.unit} \u0628\u0627\u0634\u062F`;
        }
        return `\u062E\u06CC\u0644\u06CC \u06A9\u0648\u0686\u06A9: ${issue2.origin} \u0628\u0627\u06CC\u062F ${adj}${issue2.minimum.toString()} \u0628\u0627\u0634\u062F`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u0631\u0634\u062A\u0647 \u0646\u0627\u0645\u0639\u062A\u0628\u0631: \u0628\u0627\u06CC\u062F \u0628\u0627 "${_issue.prefix}" \u0634\u0631\u0648\u0639 \u0634\u0648\u062F`;
        }
        if (_issue.format === "ends_with") {
          return `\u0631\u0634\u062A\u0647 \u0646\u0627\u0645\u0639\u062A\u0628\u0631: \u0628\u0627\u06CC\u062F \u0628\u0627 "${_issue.suffix}" \u062A\u0645\u0627\u0645 \u0634\u0648\u062F`;
        }
        if (_issue.format === "includes") {
          return `\u0631\u0634\u062A\u0647 \u0646\u0627\u0645\u0639\u062A\u0628\u0631: \u0628\u0627\u06CC\u062F \u0634\u0627\u0645\u0644 "${_issue.includes}" \u0628\u0627\u0634\u062F`;
        }
        if (_issue.format === "regex") {
          return `\u0631\u0634\u062A\u0647 \u0646\u0627\u0645\u0639\u062A\u0628\u0631: \u0628\u0627\u06CC\u062F \u0628\u0627 \u0627\u0644\u06AF\u0648\u06CC ${_issue.pattern} \u0645\u0637\u0627\u0628\u0642\u062A \u062F\u0627\u0634\u062A\u0647 \u0628\u0627\u0634\u062F`;
        }
        return `${FormatDictionary[_issue.format] ?? issue2.format} \u0646\u0627\u0645\u0639\u062A\u0628\u0631`;
      }
      case "not_multiple_of":
        return `\u0639\u062F\u062F \u0646\u0627\u0645\u0639\u062A\u0628\u0631: \u0628\u0627\u06CC\u062F \u0645\u0636\u0631\u0628 ${issue2.divisor} \u0628\u0627\u0634\u062F`;
      case "unrecognized_keys":
        return `\u06A9\u0644\u06CC\u062F${issue2.keys.length > 1 ? "\u0647\u0627\u06CC" : ""} \u0646\u0627\u0634\u0646\u0627\u0633: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u06A9\u0644\u06CC\u062F \u0646\u0627\u0634\u0646\u0627\u0633 \u062F\u0631 ${issue2.origin}`;
      case "invalid_union":
        return `\u0648\u0631\u0648\u062F\u06CC \u0646\u0627\u0645\u0639\u062A\u0628\u0631`;
      case "invalid_element":
        return `\u0645\u0642\u062F\u0627\u0631 \u0646\u0627\u0645\u0639\u062A\u0628\u0631 \u062F\u0631 ${issue2.origin}`;
      default:
        return `\u0648\u0631\u0648\u062F\u06CC \u0646\u0627\u0645\u0639\u062A\u0628\u0631`;
    }
  };
};
function fa_default() {
  return {
    localeError: error13()
  };
}

// node_modules/zod/v4/locales/fi.js
var error14 = () => {
  const Sizable = {
    string: { unit: "merkki\xE4", subject: "merkkijonon" },
    file: { unit: "tavua", subject: "tiedoston" },
    array: { unit: "alkiota", subject: "listan" },
    set: { unit: "alkiota", subject: "joukon" },
    number: { unit: "", subject: "luvun" },
    bigint: { unit: "", subject: "suuren kokonaisluvun" },
    int: { unit: "", subject: "kokonaisluvun" },
    date: { unit: "", subject: "p\xE4iv\xE4m\xE4\xE4r\xE4n" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "s\xE4\xE4nn\xF6llinen lauseke",
    email: "s\xE4hk\xF6postiosoite",
    url: "URL-osoite",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO-aikaleima",
    date: "ISO-p\xE4iv\xE4m\xE4\xE4r\xE4",
    time: "ISO-aika",
    duration: "ISO-kesto",
    ipv4: "IPv4-osoite",
    ipv6: "IPv6-osoite",
    cidrv4: "IPv4-alue",
    cidrv6: "IPv6-alue",
    base64: "base64-koodattu merkkijono",
    base64url: "base64url-koodattu merkkijono",
    json_string: "JSON-merkkijono",
    e164: "E.164-luku",
    jwt: "JWT",
    template_literal: "templaattimerkkijono"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Virheellinen tyyppi: odotettiin instanceof ${issue2.expected}, oli ${received}`;
        }
        return `Virheellinen tyyppi: odotettiin ${expected}, oli ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Virheellinen sy\xF6te: t\xE4ytyy olla ${stringifyPrimitive(issue2.values[0])}`;
        return `Virheellinen valinta: t\xE4ytyy olla yksi seuraavista: ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Liian suuri: ${sizing.subject} t\xE4ytyy olla ${adj}${issue2.maximum.toString()} ${sizing.unit}`.trim();
        }
        return `Liian suuri: arvon t\xE4ytyy olla ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Liian pieni: ${sizing.subject} t\xE4ytyy olla ${adj}${issue2.minimum.toString()} ${sizing.unit}`.trim();
        }
        return `Liian pieni: arvon t\xE4ytyy olla ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Virheellinen sy\xF6te: t\xE4ytyy alkaa "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Virheellinen sy\xF6te: t\xE4ytyy loppua "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Virheellinen sy\xF6te: t\xE4ytyy sis\xE4lt\xE4\xE4 "${_issue.includes}"`;
        if (_issue.format === "regex") {
          return `Virheellinen sy\xF6te: t\xE4ytyy vastata s\xE4\xE4nn\xF6llist\xE4 lauseketta ${_issue.pattern}`;
        }
        return `Virheellinen ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Virheellinen luku: t\xE4ytyy olla luvun ${issue2.divisor} monikerta`;
      case "unrecognized_keys":
        return `${issue2.keys.length > 1 ? "Tuntemattomat avaimet" : "Tuntematon avain"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return "Virheellinen avain tietueessa";
      case "invalid_union":
        return "Virheellinen unioni";
      case "invalid_element":
        return "Virheellinen arvo joukossa";
      default:
        return `Virheellinen sy\xF6te`;
    }
  };
};
function fi_default() {
  return {
    localeError: error14()
  };
}

// node_modules/zod/v4/locales/fr.js
var error15 = () => {
  const Sizable = {
    string: { unit: "caract\xE8res", verb: "avoir" },
    file: { unit: "octets", verb: "avoir" },
    array: { unit: "\xE9l\xE9ments", verb: "avoir" },
    set: { unit: "\xE9l\xE9ments", verb: "avoir" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "entr\xE9e",
    email: "adresse e-mail",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "date et heure ISO",
    date: "date ISO",
    time: "heure ISO",
    duration: "dur\xE9e ISO",
    ipv4: "adresse IPv4",
    ipv6: "adresse IPv6",
    cidrv4: "plage IPv4",
    cidrv6: "plage IPv6",
    base64: "cha\xEEne encod\xE9e en base64",
    base64url: "cha\xEEne encod\xE9e en base64url",
    json_string: "cha\xEEne JSON",
    e164: "num\xE9ro E.164",
    jwt: "JWT",
    template_literal: "entr\xE9e"
  };
  const TypeDictionary = {
    string: "cha\xEEne",
    number: "nombre",
    int: "entier",
    boolean: "bool\xE9en",
    bigint: "grand entier",
    symbol: "symbole",
    undefined: "ind\xE9fini",
    null: "null",
    never: "jamais",
    void: "vide",
    date: "date",
    array: "tableau",
    object: "objet",
    tuple: "tuple",
    record: "enregistrement",
    map: "carte",
    set: "ensemble",
    file: "fichier",
    nonoptional: "non-optionnel",
    nan: "NaN",
    function: "fonction"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Entr\xE9e invalide : instanceof ${issue2.expected} attendu, ${received} re\xE7u`;
        }
        return `Entr\xE9e invalide : ${expected} attendu, ${received} re\xE7u`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Entr\xE9e invalide : ${stringifyPrimitive(issue2.values[0])} attendu`;
        return `Option invalide : une valeur parmi ${joinValues(issue2.values, "|")} attendue`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Trop grand : ${TypeDictionary[issue2.origin] ?? "valeur"} doit ${sizing.verb} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\xE9l\xE9ment(s)"}`;
        return `Trop grand : ${TypeDictionary[issue2.origin] ?? "valeur"} doit \xEAtre ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Trop petit : ${TypeDictionary[issue2.origin] ?? "valeur"} doit ${sizing.verb} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        return `Trop petit : ${TypeDictionary[issue2.origin] ?? "valeur"} doit \xEAtre ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Cha\xEEne invalide : doit commencer par "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Cha\xEEne invalide : doit se terminer par "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Cha\xEEne invalide : doit inclure "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Cha\xEEne invalide : doit correspondre au mod\xE8le ${_issue.pattern}`;
        return `${FormatDictionary[_issue.format] ?? issue2.format} invalide`;
      }
      case "not_multiple_of":
        return `Nombre invalide : doit \xEAtre un multiple de ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Cl\xE9${issue2.keys.length > 1 ? "s" : ""} non reconnue${issue2.keys.length > 1 ? "s" : ""} : ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Cl\xE9 invalide dans ${issue2.origin}`;
      case "invalid_union":
        return "Entr\xE9e invalide";
      case "invalid_element":
        return `Valeur invalide dans ${issue2.origin}`;
      default:
        return `Entr\xE9e invalide`;
    }
  };
};
function fr_default() {
  return {
    localeError: error15()
  };
}

// node_modules/zod/v4/locales/fr-CA.js
var error16 = () => {
  const Sizable = {
    string: { unit: "caract\xE8res", verb: "avoir" },
    file: { unit: "octets", verb: "avoir" },
    array: { unit: "\xE9l\xE9ments", verb: "avoir" },
    set: { unit: "\xE9l\xE9ments", verb: "avoir" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "entr\xE9e",
    email: "adresse courriel",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "date-heure ISO",
    date: "date ISO",
    time: "heure ISO",
    duration: "dur\xE9e ISO",
    ipv4: "adresse IPv4",
    ipv6: "adresse IPv6",
    cidrv4: "plage IPv4",
    cidrv6: "plage IPv6",
    base64: "cha\xEEne encod\xE9e en base64",
    base64url: "cha\xEEne encod\xE9e en base64url",
    json_string: "cha\xEEne JSON",
    e164: "num\xE9ro E.164",
    jwt: "JWT",
    template_literal: "entr\xE9e"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Entr\xE9e invalide : attendu instanceof ${issue2.expected}, re\xE7u ${received}`;
        }
        return `Entr\xE9e invalide : attendu ${expected}, re\xE7u ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Entr\xE9e invalide : attendu ${stringifyPrimitive(issue2.values[0])}`;
        return `Option invalide : attendu l'une des valeurs suivantes ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "\u2264" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Trop grand : attendu que ${issue2.origin ?? "la valeur"} ait ${adj}${issue2.maximum.toString()} ${sizing.unit}`;
        return `Trop grand : attendu que ${issue2.origin ?? "la valeur"} soit ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? "\u2265" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Trop petit : attendu que ${issue2.origin} ait ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Trop petit : attendu que ${issue2.origin} soit ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `Cha\xEEne invalide : doit commencer par "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `Cha\xEEne invalide : doit se terminer par "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Cha\xEEne invalide : doit inclure "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Cha\xEEne invalide : doit correspondre au motif ${_issue.pattern}`;
        return `${FormatDictionary[_issue.format] ?? issue2.format} invalide`;
      }
      case "not_multiple_of":
        return `Nombre invalide : doit \xEAtre un multiple de ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Cl\xE9${issue2.keys.length > 1 ? "s" : ""} non reconnue${issue2.keys.length > 1 ? "s" : ""} : ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Cl\xE9 invalide dans ${issue2.origin}`;
      case "invalid_union":
        return "Entr\xE9e invalide";
      case "invalid_element":
        return `Valeur invalide dans ${issue2.origin}`;
      default:
        return `Entr\xE9e invalide`;
    }
  };
};
function fr_CA_default() {
  return {
    localeError: error16()
  };
}

// node_modules/zod/v4/locales/he.js
var error17 = () => {
  const TypeNames = {
    string: { label: "\u05DE\u05D7\u05E8\u05D5\u05D6\u05EA", gender: "f" },
    number: { label: "\u05DE\u05E1\u05E4\u05E8", gender: "m" },
    boolean: { label: "\u05E2\u05E8\u05DA \u05D1\u05D5\u05DC\u05D9\u05D0\u05E0\u05D9", gender: "m" },
    bigint: { label: "BigInt", gender: "m" },
    date: { label: "\u05EA\u05D0\u05E8\u05D9\u05DA", gender: "m" },
    array: { label: "\u05DE\u05E2\u05E8\u05DA", gender: "m" },
    object: { label: "\u05D0\u05D5\u05D1\u05D9\u05D9\u05E7\u05D8", gender: "m" },
    null: { label: "\u05E2\u05E8\u05DA \u05E8\u05D9\u05E7 (null)", gender: "m" },
    undefined: { label: "\u05E2\u05E8\u05DA \u05DC\u05D0 \u05DE\u05D5\u05D2\u05D3\u05E8 (undefined)", gender: "m" },
    symbol: { label: "\u05E1\u05D9\u05DE\u05D1\u05D5\u05DC (Symbol)", gender: "m" },
    function: { label: "\u05E4\u05D5\u05E0\u05E7\u05E6\u05D9\u05D4", gender: "f" },
    map: { label: "\u05DE\u05E4\u05D4 (Map)", gender: "f" },
    set: { label: "\u05E7\u05D1\u05D5\u05E6\u05D4 (Set)", gender: "f" },
    file: { label: "\u05E7\u05D5\u05D1\u05E5", gender: "m" },
    promise: { label: "Promise", gender: "m" },
    NaN: { label: "NaN", gender: "m" },
    unknown: { label: "\u05E2\u05E8\u05DA \u05DC\u05D0 \u05D9\u05D3\u05D5\u05E2", gender: "m" },
    value: { label: "\u05E2\u05E8\u05DA", gender: "m" }
  };
  const Sizable = {
    string: { unit: "\u05EA\u05D5\u05D5\u05D9\u05DD", shortLabel: "\u05E7\u05E6\u05E8", longLabel: "\u05D0\u05E8\u05D5\u05DA" },
    file: { unit: "\u05D1\u05D9\u05D9\u05D8\u05D9\u05DD", shortLabel: "\u05E7\u05D8\u05DF", longLabel: "\u05D2\u05D3\u05D5\u05DC" },
    array: { unit: "\u05E4\u05E8\u05D9\u05D8\u05D9\u05DD", shortLabel: "\u05E7\u05D8\u05DF", longLabel: "\u05D2\u05D3\u05D5\u05DC" },
    set: { unit: "\u05E4\u05E8\u05D9\u05D8\u05D9\u05DD", shortLabel: "\u05E7\u05D8\u05DF", longLabel: "\u05D2\u05D3\u05D5\u05DC" },
    number: { unit: "", shortLabel: "\u05E7\u05D8\u05DF", longLabel: "\u05D2\u05D3\u05D5\u05DC" }
    // no unit
  };
  const typeEntry = (t) => t ? TypeNames[t] : void 0;
  const typeLabel = (t) => {
    const e = typeEntry(t);
    if (e)
      return e.label;
    return t ?? TypeNames.unknown.label;
  };
  const withDefinite = (t) => `\u05D4${typeLabel(t)}`;
  const verbFor = (t) => {
    const e = typeEntry(t);
    const gender = e?.gender ?? "m";
    return gender === "f" ? "\u05E6\u05E8\u05D9\u05DB\u05D4 \u05DC\u05D4\u05D9\u05D5\u05EA" : "\u05E6\u05E8\u05D9\u05DA \u05DC\u05D4\u05D9\u05D5\u05EA";
  };
  const getSizing = (origin) => {
    if (!origin)
      return null;
    return Sizable[origin] ?? null;
  };
  const FormatDictionary = {
    regex: { label: "\u05E7\u05DC\u05D8", gender: "m" },
    email: { label: "\u05DB\u05EA\u05D5\u05D1\u05EA \u05D0\u05D9\u05DE\u05D9\u05D9\u05DC", gender: "f" },
    url: { label: "\u05DB\u05EA\u05D5\u05D1\u05EA \u05E8\u05E9\u05EA", gender: "f" },
    emoji: { label: "\u05D0\u05D9\u05DE\u05D5\u05D2'\u05D9", gender: "m" },
    uuid: { label: "UUID", gender: "m" },
    nanoid: { label: "nanoid", gender: "m" },
    guid: { label: "GUID", gender: "m" },
    cuid: { label: "cuid", gender: "m" },
    cuid2: { label: "cuid2", gender: "m" },
    ulid: { label: "ULID", gender: "m" },
    xid: { label: "XID", gender: "m" },
    ksuid: { label: "KSUID", gender: "m" },
    datetime: { label: "\u05EA\u05D0\u05E8\u05D9\u05DA \u05D5\u05D6\u05DE\u05DF ISO", gender: "m" },
    date: { label: "\u05EA\u05D0\u05E8\u05D9\u05DA ISO", gender: "m" },
    time: { label: "\u05D6\u05DE\u05DF ISO", gender: "m" },
    duration: { label: "\u05DE\u05E9\u05DA \u05D6\u05DE\u05DF ISO", gender: "m" },
    ipv4: { label: "\u05DB\u05EA\u05D5\u05D1\u05EA IPv4", gender: "f" },
    ipv6: { label: "\u05DB\u05EA\u05D5\u05D1\u05EA IPv6", gender: "f" },
    cidrv4: { label: "\u05D8\u05D5\u05D5\u05D7 IPv4", gender: "m" },
    cidrv6: { label: "\u05D8\u05D5\u05D5\u05D7 IPv6", gender: "m" },
    base64: { label: "\u05DE\u05D7\u05E8\u05D5\u05D6\u05EA \u05D1\u05D1\u05E1\u05D9\u05E1 64", gender: "f" },
    base64url: { label: "\u05DE\u05D7\u05E8\u05D5\u05D6\u05EA \u05D1\u05D1\u05E1\u05D9\u05E1 64 \u05DC\u05DB\u05EA\u05D5\u05D1\u05D5\u05EA \u05E8\u05E9\u05EA", gender: "f" },
    json_string: { label: "\u05DE\u05D7\u05E8\u05D5\u05D6\u05EA JSON", gender: "f" },
    e164: { label: "\u05DE\u05E1\u05E4\u05E8 E.164", gender: "m" },
    jwt: { label: "JWT", gender: "m" },
    ends_with: { label: "\u05E7\u05DC\u05D8", gender: "m" },
    includes: { label: "\u05E7\u05DC\u05D8", gender: "m" },
    lowercase: { label: "\u05E7\u05DC\u05D8", gender: "m" },
    starts_with: { label: "\u05E7\u05DC\u05D8", gender: "m" },
    uppercase: { label: "\u05E7\u05DC\u05D8", gender: "m" }
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expectedKey = issue2.expected;
        const expected = TypeDictionary[expectedKey ?? ""] ?? typeLabel(expectedKey);
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? TypeNames[receivedType]?.label ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u05E7\u05DC\u05D8 \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF: \u05E6\u05E8\u05D9\u05DA \u05DC\u05D4\u05D9\u05D5\u05EA instanceof ${issue2.expected}, \u05D4\u05EA\u05E7\u05D1\u05DC ${received}`;
        }
        return `\u05E7\u05DC\u05D8 \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF: \u05E6\u05E8\u05D9\u05DA \u05DC\u05D4\u05D9\u05D5\u05EA ${expected}, \u05D4\u05EA\u05E7\u05D1\u05DC ${received}`;
      }
      case "invalid_value": {
        if (issue2.values.length === 1) {
          return `\u05E2\u05E8\u05DA \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF: \u05D4\u05E2\u05E8\u05DA \u05D7\u05D9\u05D9\u05D1 \u05DC\u05D4\u05D9\u05D5\u05EA ${stringifyPrimitive(issue2.values[0])}`;
        }
        const stringified = issue2.values.map((v) => stringifyPrimitive(v));
        if (issue2.values.length === 2) {
          return `\u05E2\u05E8\u05DA \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF: \u05D4\u05D0\u05E4\u05E9\u05E8\u05D5\u05D9\u05D5\u05EA \u05D4\u05DE\u05EA\u05D0\u05D9\u05DE\u05D5\u05EA \u05D4\u05DF ${stringified[0]} \u05D0\u05D5 ${stringified[1]}`;
        }
        const lastValue = stringified[stringified.length - 1];
        const restValues = stringified.slice(0, -1).join(", ");
        return `\u05E2\u05E8\u05DA \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF: \u05D4\u05D0\u05E4\u05E9\u05E8\u05D5\u05D9\u05D5\u05EA \u05D4\u05DE\u05EA\u05D0\u05D9\u05DE\u05D5\u05EA \u05D4\u05DF ${restValues} \u05D0\u05D5 ${lastValue}`;
      }
      case "too_big": {
        const sizing = getSizing(issue2.origin);
        const subject = withDefinite(issue2.origin ?? "value");
        if (issue2.origin === "string") {
          return `${sizing?.longLabel ?? "\u05D0\u05E8\u05D5\u05DA"} \u05DE\u05D3\u05D9: ${subject} \u05E6\u05E8\u05D9\u05DB\u05D4 \u05DC\u05D4\u05DB\u05D9\u05DC ${issue2.maximum.toString()} ${sizing?.unit ?? ""} ${issue2.inclusive ? "\u05D0\u05D5 \u05E4\u05D7\u05D5\u05EA" : "\u05DC\u05DB\u05DC \u05D4\u05D9\u05D5\u05EA\u05E8"}`.trim();
        }
        if (issue2.origin === "number") {
          const comparison = issue2.inclusive ? `\u05E7\u05D8\u05DF \u05D0\u05D5 \u05E9\u05D5\u05D5\u05D4 \u05DC-${issue2.maximum}` : `\u05E7\u05D8\u05DF \u05DE-${issue2.maximum}`;
          return `\u05D2\u05D3\u05D5\u05DC \u05DE\u05D3\u05D9: ${subject} \u05E6\u05E8\u05D9\u05DA \u05DC\u05D4\u05D9\u05D5\u05EA ${comparison}`;
        }
        if (issue2.origin === "array" || issue2.origin === "set") {
          const verb = issue2.origin === "set" ? "\u05E6\u05E8\u05D9\u05DB\u05D4" : "\u05E6\u05E8\u05D9\u05DA";
          const comparison = issue2.inclusive ? `${issue2.maximum} ${sizing?.unit ?? ""} \u05D0\u05D5 \u05E4\u05D7\u05D5\u05EA` : `\u05E4\u05D7\u05D5\u05EA \u05DE-${issue2.maximum} ${sizing?.unit ?? ""}`;
          return `\u05D2\u05D3\u05D5\u05DC \u05DE\u05D3\u05D9: ${subject} ${verb} \u05DC\u05D4\u05DB\u05D9\u05DC ${comparison}`.trim();
        }
        const adj = issue2.inclusive ? "<=" : "<";
        const be = verbFor(issue2.origin ?? "value");
        if (sizing?.unit) {
          return `${sizing.longLabel} \u05DE\u05D3\u05D9: ${subject} ${be} ${adj}${issue2.maximum.toString()} ${sizing.unit}`;
        }
        return `${sizing?.longLabel ?? "\u05D2\u05D3\u05D5\u05DC"} \u05DE\u05D3\u05D9: ${subject} ${be} ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const sizing = getSizing(issue2.origin);
        const subject = withDefinite(issue2.origin ?? "value");
        if (issue2.origin === "string") {
          return `${sizing?.shortLabel ?? "\u05E7\u05E6\u05E8"} \u05DE\u05D3\u05D9: ${subject} \u05E6\u05E8\u05D9\u05DB\u05D4 \u05DC\u05D4\u05DB\u05D9\u05DC ${issue2.minimum.toString()} ${sizing?.unit ?? ""} ${issue2.inclusive ? "\u05D0\u05D5 \u05D9\u05D5\u05EA\u05E8" : "\u05DC\u05E4\u05D7\u05D5\u05EA"}`.trim();
        }
        if (issue2.origin === "number") {
          const comparison = issue2.inclusive ? `\u05D2\u05D3\u05D5\u05DC \u05D0\u05D5 \u05E9\u05D5\u05D5\u05D4 \u05DC-${issue2.minimum}` : `\u05D2\u05D3\u05D5\u05DC \u05DE-${issue2.minimum}`;
          return `\u05E7\u05D8\u05DF \u05DE\u05D3\u05D9: ${subject} \u05E6\u05E8\u05D9\u05DA \u05DC\u05D4\u05D9\u05D5\u05EA ${comparison}`;
        }
        if (issue2.origin === "array" || issue2.origin === "set") {
          const verb = issue2.origin === "set" ? "\u05E6\u05E8\u05D9\u05DB\u05D4" : "\u05E6\u05E8\u05D9\u05DA";
          if (issue2.minimum === 1 && issue2.inclusive) {
            const singularPhrase = issue2.origin === "set" ? "\u05DC\u05E4\u05D7\u05D5\u05EA \u05E4\u05E8\u05D9\u05D8 \u05D0\u05D7\u05D3" : "\u05DC\u05E4\u05D7\u05D5\u05EA \u05E4\u05E8\u05D9\u05D8 \u05D0\u05D7\u05D3";
            return `\u05E7\u05D8\u05DF \u05DE\u05D3\u05D9: ${subject} ${verb} \u05DC\u05D4\u05DB\u05D9\u05DC ${singularPhrase}`;
          }
          const comparison = issue2.inclusive ? `${issue2.minimum} ${sizing?.unit ?? ""} \u05D0\u05D5 \u05D9\u05D5\u05EA\u05E8` : `\u05D9\u05D5\u05EA\u05E8 \u05DE-${issue2.minimum} ${sizing?.unit ?? ""}`;
          return `\u05E7\u05D8\u05DF \u05DE\u05D3\u05D9: ${subject} ${verb} \u05DC\u05D4\u05DB\u05D9\u05DC ${comparison}`.trim();
        }
        const adj = issue2.inclusive ? ">=" : ">";
        const be = verbFor(issue2.origin ?? "value");
        if (sizing?.unit) {
          return `${sizing.shortLabel} \u05DE\u05D3\u05D9: ${subject} ${be} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `${sizing?.shortLabel ?? "\u05E7\u05D8\u05DF"} \u05DE\u05D3\u05D9: ${subject} ${be} ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u05D4\u05DE\u05D7\u05E8\u05D5\u05D6\u05EA \u05D7\u05D9\u05D9\u05D1\u05EA \u05DC\u05D4\u05EA\u05D7\u05D9\u05DC \u05D1 "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `\u05D4\u05DE\u05D7\u05E8\u05D5\u05D6\u05EA \u05D7\u05D9\u05D9\u05D1\u05EA \u05DC\u05D4\u05E1\u05EA\u05D9\u05D9\u05DD \u05D1 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u05D4\u05DE\u05D7\u05E8\u05D5\u05D6\u05EA \u05D7\u05D9\u05D9\u05D1\u05EA \u05DC\u05DB\u05DC\u05D5\u05DC "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u05D4\u05DE\u05D7\u05E8\u05D5\u05D6\u05EA \u05D7\u05D9\u05D9\u05D1\u05EA \u05DC\u05D4\u05EA\u05D0\u05D9\u05DD \u05DC\u05EA\u05D1\u05E0\u05D9\u05EA ${_issue.pattern}`;
        const nounEntry = FormatDictionary[_issue.format];
        const noun = nounEntry?.label ?? _issue.format;
        const gender = nounEntry?.gender ?? "m";
        const adjective = gender === "f" ? "\u05EA\u05E7\u05D9\u05E0\u05D4" : "\u05EA\u05E7\u05D9\u05DF";
        return `${noun} \u05DC\u05D0 ${adjective}`;
      }
      case "not_multiple_of":
        return `\u05DE\u05E1\u05E4\u05E8 \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF: \u05D7\u05D9\u05D9\u05D1 \u05DC\u05D4\u05D9\u05D5\u05EA \u05DE\u05DB\u05E4\u05DC\u05D4 \u05E9\u05DC ${issue2.divisor}`;
      case "unrecognized_keys":
        return `\u05DE\u05E4\u05EA\u05D7${issue2.keys.length > 1 ? "\u05D5\u05EA" : ""} \u05DC\u05D0 \u05DE\u05D6\u05D5\u05D4${issue2.keys.length > 1 ? "\u05D9\u05DD" : "\u05D4"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key": {
        return `\u05E9\u05D3\u05D4 \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF \u05D1\u05D0\u05D5\u05D1\u05D9\u05D9\u05E7\u05D8`;
      }
      case "invalid_union":
        return "\u05E7\u05DC\u05D8 \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF";
      case "invalid_element": {
        const place = withDefinite(issue2.origin ?? "array");
        return `\u05E2\u05E8\u05DA \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF \u05D1${place}`;
      }
      default:
        return `\u05E7\u05DC\u05D8 \u05DC\u05D0 \u05EA\u05E7\u05D9\u05DF`;
    }
  };
};
function he_default() {
  return {
    localeError: error17()
  };
}

// node_modules/zod/v4/locales/hr.js
var error18 = () => {
  const Sizable = {
    string: { unit: "znakova", verb: "imati" },
    file: { unit: "bajtova", verb: "imati" },
    array: { unit: "stavki", verb: "imati" },
    set: { unit: "stavki", verb: "imati" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "unos",
    email: "email adresa",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO datum i vrijeme",
    date: "ISO datum",
    time: "ISO vrijeme",
    duration: "ISO trajanje",
    ipv4: "IPv4 adresa",
    ipv6: "IPv6 adresa",
    cidrv4: "IPv4 raspon",
    cidrv6: "IPv6 raspon",
    base64: "base64 kodirani tekst",
    base64url: "base64url kodirani tekst",
    json_string: "JSON tekst",
    e164: "E.164 broj",
    jwt: "JWT",
    template_literal: "unos"
  };
  const TypeDictionary = {
    nan: "NaN",
    string: "tekst",
    number: "broj",
    boolean: "boolean",
    array: "niz",
    object: "objekt",
    set: "skup",
    file: "datoteka",
    date: "datum",
    bigint: "bigint",
    symbol: "simbol",
    undefined: "undefined",
    null: "null",
    function: "funkcija",
    map: "mapa"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Neispravan unos: o\u010Dekuje se instanceof ${issue2.expected}, a primljeno je ${received}`;
        }
        return `Neispravan unos: o\u010Dekuje se ${expected}, a primljeno je ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Neispravna vrijednost: o\u010Dekivano ${stringifyPrimitive(issue2.values[0])}`;
        return `Neispravna opcija: o\u010Dekivano jedno od ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        const origin = TypeDictionary[issue2.origin] ?? issue2.origin;
        if (sizing)
          return `Preveliko: o\u010Dekivano da ${origin ?? "vrijednost"} ima ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elemenata"}`;
        return `Preveliko: o\u010Dekivano da ${origin ?? "vrijednost"} bude ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        const origin = TypeDictionary[issue2.origin] ?? issue2.origin;
        if (sizing) {
          return `Premalo: o\u010Dekivano da ${origin} ima ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Premalo: o\u010Dekivano da ${origin} bude ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Neispravan tekst: mora zapo\u010Dinjati s "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Neispravan tekst: mora zavr\u0161avati s "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Neispravan tekst: mora sadr\u017Eavati "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Neispravan tekst: mora odgovarati uzorku ${_issue.pattern}`;
        return `Neispravna ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Neispravan broj: mora biti vi\u0161ekratnik od ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Neprepoznat${issue2.keys.length > 1 ? "i klju\u010Devi" : " klju\u010D"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Neispravan klju\u010D u ${TypeDictionary[issue2.origin] ?? issue2.origin}`;
      case "invalid_union":
        return "Neispravan unos";
      case "invalid_element":
        return `Neispravna vrijednost u ${TypeDictionary[issue2.origin] ?? issue2.origin}`;
      default:
        return `Neispravan unos`;
    }
  };
};
function hr_default() {
  return {
    localeError: error18()
  };
}

// node_modules/zod/v4/locales/hu.js
var error19 = () => {
  const Sizable = {
    string: { unit: "karakter", verb: "legyen" },
    file: { unit: "byte", verb: "legyen" },
    array: { unit: "elem", verb: "legyen" },
    set: { unit: "elem", verb: "legyen" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "bemenet",
    email: "email c\xEDm",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO id\u0151b\xE9lyeg",
    date: "ISO d\xE1tum",
    time: "ISO id\u0151",
    duration: "ISO id\u0151intervallum",
    ipv4: "IPv4 c\xEDm",
    ipv6: "IPv6 c\xEDm",
    cidrv4: "IPv4 tartom\xE1ny",
    cidrv6: "IPv6 tartom\xE1ny",
    base64: "base64-k\xF3dolt string",
    base64url: "base64url-k\xF3dolt string",
    json_string: "JSON string",
    e164: "E.164 sz\xE1m",
    jwt: "JWT",
    template_literal: "bemenet"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "sz\xE1m",
    array: "t\xF6mb"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\xC9rv\xE9nytelen bemenet: a v\xE1rt \xE9rt\xE9k instanceof ${issue2.expected}, a kapott \xE9rt\xE9k ${received}`;
        }
        return `\xC9rv\xE9nytelen bemenet: a v\xE1rt \xE9rt\xE9k ${expected}, a kapott \xE9rt\xE9k ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\xC9rv\xE9nytelen bemenet: a v\xE1rt \xE9rt\xE9k ${stringifyPrimitive(issue2.values[0])}`;
        return `\xC9rv\xE9nytelen opci\xF3: valamelyik \xE9rt\xE9k v\xE1rt ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `T\xFAl nagy: ${issue2.origin ?? "\xE9rt\xE9k"} m\xE9rete t\xFAl nagy ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elem"}`;
        return `T\xFAl nagy: a bemeneti \xE9rt\xE9k ${issue2.origin ?? "\xE9rt\xE9k"} t\xFAl nagy: ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `T\xFAl kicsi: a bemeneti \xE9rt\xE9k ${issue2.origin} m\xE9rete t\xFAl kicsi ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `T\xFAl kicsi: a bemeneti \xE9rt\xE9k ${issue2.origin} t\xFAl kicsi ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\xC9rv\xE9nytelen string: "${_issue.prefix}" \xE9rt\xE9kkel kell kezd\u0151dnie`;
        if (_issue.format === "ends_with")
          return `\xC9rv\xE9nytelen string: "${_issue.suffix}" \xE9rt\xE9kkel kell v\xE9gz\u0151dnie`;
        if (_issue.format === "includes")
          return `\xC9rv\xE9nytelen string: "${_issue.includes}" \xE9rt\xE9ket kell tartalmaznia`;
        if (_issue.format === "regex")
          return `\xC9rv\xE9nytelen string: ${_issue.pattern} mint\xE1nak kell megfelelnie`;
        return `\xC9rv\xE9nytelen ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\xC9rv\xE9nytelen sz\xE1m: ${issue2.divisor} t\xF6bbsz\xF6r\xF6s\xE9nek kell lennie`;
      case "unrecognized_keys":
        return `Ismeretlen kulcs${issue2.keys.length > 1 ? "s" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\xC9rv\xE9nytelen kulcs ${issue2.origin}`;
      case "invalid_union":
        return "\xC9rv\xE9nytelen bemenet";
      case "invalid_element":
        return `\xC9rv\xE9nytelen \xE9rt\xE9k: ${issue2.origin}`;
      default:
        return `\xC9rv\xE9nytelen bemenet`;
    }
  };
};
function hu_default() {
  return {
    localeError: error19()
  };
}

// node_modules/zod/v4/locales/hy.js
function getArmenianPlural(count, one, many) {
  return Math.abs(count) === 1 ? one : many;
}
function withDefiniteArticle(word) {
  if (!word)
    return "";
  const vowels = ["\u0561", "\u0565", "\u0568", "\u056B", "\u0578", "\u0578\u0582", "\u0585"];
  const lastChar = word[word.length - 1];
  return word + (vowels.includes(lastChar) ? "\u0576" : "\u0568");
}
var error20 = () => {
  const Sizable = {
    string: {
      unit: {
        one: "\u0576\u0577\u0561\u0576",
        many: "\u0576\u0577\u0561\u0576\u0576\u0565\u0580"
      },
      verb: "\u0578\u0582\u0576\u0565\u0576\u0561\u056C"
    },
    file: {
      unit: {
        one: "\u0562\u0561\u0575\u0569",
        many: "\u0562\u0561\u0575\u0569\u0565\u0580"
      },
      verb: "\u0578\u0582\u0576\u0565\u0576\u0561\u056C"
    },
    array: {
      unit: {
        one: "\u057F\u0561\u0580\u0580",
        many: "\u057F\u0561\u0580\u0580\u0565\u0580"
      },
      verb: "\u0578\u0582\u0576\u0565\u0576\u0561\u056C"
    },
    set: {
      unit: {
        one: "\u057F\u0561\u0580\u0580",
        many: "\u057F\u0561\u0580\u0580\u0565\u0580"
      },
      verb: "\u0578\u0582\u0576\u0565\u0576\u0561\u056C"
    }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0574\u0578\u0582\u057F\u0584",
    email: "\u0567\u056C. \u0570\u0561\u057D\u0581\u0565",
    url: "URL",
    emoji: "\u0567\u0574\u0578\u057B\u056B",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO \u0561\u0574\u057D\u0561\u0569\u056B\u057E \u0587 \u056A\u0561\u0574",
    date: "ISO \u0561\u0574\u057D\u0561\u0569\u056B\u057E",
    time: "ISO \u056A\u0561\u0574",
    duration: "ISO \u057F\u0587\u0578\u0572\u0578\u0582\u0569\u0575\u0578\u0582\u0576",
    ipv4: "IPv4 \u0570\u0561\u057D\u0581\u0565",
    ipv6: "IPv6 \u0570\u0561\u057D\u0581\u0565",
    cidrv4: "IPv4 \u0574\u056B\u057B\u0561\u056F\u0561\u0575\u0584",
    cidrv6: "IPv6 \u0574\u056B\u057B\u0561\u056F\u0561\u0575\u0584",
    base64: "base64 \u0571\u0587\u0561\u0579\u0561\u0583\u0578\u057E \u057F\u0578\u0572",
    base64url: "base64url \u0571\u0587\u0561\u0579\u0561\u0583\u0578\u057E \u057F\u0578\u0572",
    json_string: "JSON \u057F\u0578\u0572",
    e164: "E.164 \u0570\u0561\u0574\u0561\u0580",
    jwt: "JWT",
    template_literal: "\u0574\u0578\u0582\u057F\u0584"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0569\u056B\u057E",
    array: "\u0566\u0561\u0576\u0563\u057E\u0561\u056E"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u054D\u056D\u0561\u056C \u0574\u0578\u0582\u057F\u0584\u0561\u0563\u0580\u0578\u0582\u0574\u2024 \u057D\u057A\u0561\u057D\u057E\u0578\u0582\u0574 \u0567\u0580 instanceof ${issue2.expected}, \u057D\u057F\u0561\u0581\u057E\u0565\u056C \u0567 ${received}`;
        }
        return `\u054D\u056D\u0561\u056C \u0574\u0578\u0582\u057F\u0584\u0561\u0563\u0580\u0578\u0582\u0574\u2024 \u057D\u057A\u0561\u057D\u057E\u0578\u0582\u0574 \u0567\u0580 ${expected}, \u057D\u057F\u0561\u0581\u057E\u0565\u056C \u0567 ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u054D\u056D\u0561\u056C \u0574\u0578\u0582\u057F\u0584\u0561\u0563\u0580\u0578\u0582\u0574\u2024 \u057D\u057A\u0561\u057D\u057E\u0578\u0582\u0574 \u0567\u0580 ${stringifyPrimitive(issue2.values[1])}`;
        return `\u054D\u056D\u0561\u056C \u057F\u0561\u0580\u0562\u0565\u0580\u0561\u056F\u2024 \u057D\u057A\u0561\u057D\u057E\u0578\u0582\u0574 \u0567\u0580 \u0570\u0565\u057F\u0587\u0575\u0561\u056C\u0576\u0565\u0580\u056B\u0581 \u0574\u0565\u056F\u0568\u055D ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          const maxValue = Number(issue2.maximum);
          const unit = getArmenianPlural(maxValue, sizing.unit.one, sizing.unit.many);
          return `\u0549\u0561\u0583\u0561\u0566\u0561\u0576\u0581 \u0574\u0565\u056E \u0561\u0580\u056A\u0565\u0584\u2024 \u057D\u057A\u0561\u057D\u057E\u0578\u0582\u0574 \u0567, \u0578\u0580 ${withDefiniteArticle(issue2.origin ?? "\u0561\u0580\u056A\u0565\u0584")} \u056F\u0578\u0582\u0576\u0565\u0576\u0561 ${adj}${issue2.maximum.toString()} ${unit}`;
        }
        return `\u0549\u0561\u0583\u0561\u0566\u0561\u0576\u0581 \u0574\u0565\u056E \u0561\u0580\u056A\u0565\u0584\u2024 \u057D\u057A\u0561\u057D\u057E\u0578\u0582\u0574 \u0567, \u0578\u0580 ${withDefiniteArticle(issue2.origin ?? "\u0561\u0580\u056A\u0565\u0584")} \u056C\u056B\u0576\u056B ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          const minValue = Number(issue2.minimum);
          const unit = getArmenianPlural(minValue, sizing.unit.one, sizing.unit.many);
          return `\u0549\u0561\u0583\u0561\u0566\u0561\u0576\u0581 \u0583\u0578\u0584\u0580 \u0561\u0580\u056A\u0565\u0584\u2024 \u057D\u057A\u0561\u057D\u057E\u0578\u0582\u0574 \u0567, \u0578\u0580 ${withDefiniteArticle(issue2.origin)} \u056F\u0578\u0582\u0576\u0565\u0576\u0561 ${adj}${issue2.minimum.toString()} ${unit}`;
        }
        return `\u0549\u0561\u0583\u0561\u0566\u0561\u0576\u0581 \u0583\u0578\u0584\u0580 \u0561\u0580\u056A\u0565\u0584\u2024 \u057D\u057A\u0561\u057D\u057E\u0578\u0582\u0574 \u0567, \u0578\u0580 ${withDefiniteArticle(issue2.origin)} \u056C\u056B\u0576\u056B ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u054D\u056D\u0561\u056C \u057F\u0578\u0572\u2024 \u057A\u0565\u057F\u0584 \u0567 \u057D\u056F\u057D\u057E\u056B "${_issue.prefix}"-\u0578\u057E`;
        if (_issue.format === "ends_with")
          return `\u054D\u056D\u0561\u056C \u057F\u0578\u0572\u2024 \u057A\u0565\u057F\u0584 \u0567 \u0561\u057E\u0561\u0580\u057F\u057E\u056B "${_issue.suffix}"-\u0578\u057E`;
        if (_issue.format === "includes")
          return `\u054D\u056D\u0561\u056C \u057F\u0578\u0572\u2024 \u057A\u0565\u057F\u0584 \u0567 \u057A\u0561\u0580\u0578\u0582\u0576\u0561\u056F\u056B "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u054D\u056D\u0561\u056C \u057F\u0578\u0572\u2024 \u057A\u0565\u057F\u0584 \u0567 \u0570\u0561\u0574\u0561\u057A\u0561\u057F\u0561\u057D\u056D\u0561\u0576\u056B ${_issue.pattern} \u0571\u0587\u0561\u0579\u0561\u0583\u056B\u0576`;
        return `\u054D\u056D\u0561\u056C ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u054D\u056D\u0561\u056C \u0569\u056B\u057E\u2024 \u057A\u0565\u057F\u0584 \u0567 \u0562\u0561\u0566\u0574\u0561\u057A\u0561\u057F\u056B\u056F \u056C\u056B\u0576\u056B ${issue2.divisor}-\u056B`;
      case "unrecognized_keys":
        return `\u0549\u0573\u0561\u0576\u0561\u0579\u057E\u0561\u056E \u0562\u0561\u0576\u0561\u056C\u056B${issue2.keys.length > 1 ? "\u0576\u0565\u0580" : ""}. ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u054D\u056D\u0561\u056C \u0562\u0561\u0576\u0561\u056C\u056B ${withDefiniteArticle(issue2.origin)}-\u0578\u0582\u0574`;
      case "invalid_union":
        return "\u054D\u056D\u0561\u056C \u0574\u0578\u0582\u057F\u0584\u0561\u0563\u0580\u0578\u0582\u0574";
      case "invalid_element":
        return `\u054D\u056D\u0561\u056C \u0561\u0580\u056A\u0565\u0584 ${withDefiniteArticle(issue2.origin)}-\u0578\u0582\u0574`;
      default:
        return `\u054D\u056D\u0561\u056C \u0574\u0578\u0582\u057F\u0584\u0561\u0563\u0580\u0578\u0582\u0574`;
    }
  };
};
function hy_default() {
  return {
    localeError: error20()
  };
}

// node_modules/zod/v4/locales/id.js
var error21 = () => {
  const Sizable = {
    string: { unit: "karakter", verb: "memiliki" },
    file: { unit: "byte", verb: "memiliki" },
    array: { unit: "item", verb: "memiliki" },
    set: { unit: "item", verb: "memiliki" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "input",
    email: "alamat email",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "tanggal dan waktu format ISO",
    date: "tanggal format ISO",
    time: "jam format ISO",
    duration: "durasi format ISO",
    ipv4: "alamat IPv4",
    ipv6: "alamat IPv6",
    cidrv4: "rentang alamat IPv4",
    cidrv6: "rentang alamat IPv6",
    base64: "string dengan enkode base64",
    base64url: "string dengan enkode base64url",
    json_string: "string JSON",
    e164: "angka E.164",
    jwt: "JWT",
    template_literal: "input"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Input tidak valid: diharapkan instanceof ${issue2.expected}, diterima ${received}`;
        }
        return `Input tidak valid: diharapkan ${expected}, diterima ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Input tidak valid: diharapkan ${stringifyPrimitive(issue2.values[0])}`;
        return `Pilihan tidak valid: diharapkan salah satu dari ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Terlalu besar: diharapkan ${issue2.origin ?? "value"} memiliki ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elemen"}`;
        return `Terlalu besar: diharapkan ${issue2.origin ?? "value"} menjadi ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Terlalu kecil: diharapkan ${issue2.origin} memiliki ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Terlalu kecil: diharapkan ${issue2.origin} menjadi ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `String tidak valid: harus dimulai dengan "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `String tidak valid: harus berakhir dengan "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `String tidak valid: harus menyertakan "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `String tidak valid: harus sesuai pola ${_issue.pattern}`;
        return `${FormatDictionary[_issue.format] ?? issue2.format} tidak valid`;
      }
      case "not_multiple_of":
        return `Angka tidak valid: harus kelipatan dari ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Kunci tidak dikenali ${issue2.keys.length > 1 ? "s" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Kunci tidak valid di ${issue2.origin}`;
      case "invalid_union":
        return "Input tidak valid";
      case "invalid_element":
        return `Nilai tidak valid di ${issue2.origin}`;
      default:
        return `Input tidak valid`;
    }
  };
};
function id_default() {
  return {
    localeError: error21()
  };
}

// node_modules/zod/v4/locales/is.js
var error22 = () => {
  const Sizable = {
    string: { unit: "stafi", verb: "a\xF0 hafa" },
    file: { unit: "b\xE6ti", verb: "a\xF0 hafa" },
    array: { unit: "hluti", verb: "a\xF0 hafa" },
    set: { unit: "hluti", verb: "a\xF0 hafa" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "gildi",
    email: "netfang",
    url: "vefsl\xF3\xF0",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO dagsetning og t\xEDmi",
    date: "ISO dagsetning",
    time: "ISO t\xEDmi",
    duration: "ISO t\xEDmalengd",
    ipv4: "IPv4 address",
    ipv6: "IPv6 address",
    cidrv4: "IPv4 range",
    cidrv6: "IPv6 range",
    base64: "base64-encoded strengur",
    base64url: "base64url-encoded strengur",
    json_string: "JSON strengur",
    e164: "E.164 t\xF6lugildi",
    jwt: "JWT",
    template_literal: "gildi"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "n\xFAmer",
    array: "fylki"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Rangt gildi: \xDE\xFA sl\xF3st inn ${received} \xFEar sem \xE1 a\xF0 vera instanceof ${issue2.expected}`;
        }
        return `Rangt gildi: \xDE\xFA sl\xF3st inn ${received} \xFEar sem \xE1 a\xF0 vera ${expected}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Rangt gildi: gert r\xE1\xF0 fyrir ${stringifyPrimitive(issue2.values[0])}`;
        return `\xD3gilt val: m\xE1 vera eitt af eftirfarandi ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Of st\xF3rt: gert er r\xE1\xF0 fyrir a\xF0 ${issue2.origin ?? "gildi"} hafi ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "hluti"}`;
        return `Of st\xF3rt: gert er r\xE1\xF0 fyrir a\xF0 ${issue2.origin ?? "gildi"} s\xE9 ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Of l\xEDti\xF0: gert er r\xE1\xF0 fyrir a\xF0 ${issue2.origin} hafi ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Of l\xEDti\xF0: gert er r\xE1\xF0 fyrir a\xF0 ${issue2.origin} s\xE9 ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\xD3gildur strengur: ver\xF0ur a\xF0 byrja \xE1 "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `\xD3gildur strengur: ver\xF0ur a\xF0 enda \xE1 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\xD3gildur strengur: ver\xF0ur a\xF0 innihalda "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\xD3gildur strengur: ver\xF0ur a\xF0 fylgja mynstri ${_issue.pattern}`;
        return `Rangt ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `R\xF6ng tala: ver\xF0ur a\xF0 vera margfeldi af ${issue2.divisor}`;
      case "unrecognized_keys":
        return `\xD3\xFEekkt ${issue2.keys.length > 1 ? "ir lyklar" : "ur lykill"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Rangur lykill \xED ${issue2.origin}`;
      case "invalid_union":
        return "Rangt gildi";
      case "invalid_element":
        return `Rangt gildi \xED ${issue2.origin}`;
      default:
        return `Rangt gildi`;
    }
  };
};
function is_default() {
  return {
    localeError: error22()
  };
}

// node_modules/zod/v4/locales/it.js
var error23 = () => {
  const Sizable = {
    string: { unit: "caratteri", verb: "avere" },
    file: { unit: "byte", verb: "avere" },
    array: { unit: "elementi", verb: "avere" },
    set: { unit: "elementi", verb: "avere" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "input",
    email: "indirizzo email",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "data e ora ISO",
    date: "data ISO",
    time: "ora ISO",
    duration: "durata ISO",
    ipv4: "indirizzo IPv4",
    ipv6: "indirizzo IPv6",
    cidrv4: "intervallo IPv4",
    cidrv6: "intervallo IPv6",
    base64: "stringa codificata in base64",
    base64url: "URL codificata in base64",
    json_string: "stringa JSON",
    e164: "numero E.164",
    jwt: "JWT",
    template_literal: "input"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "numero",
    array: "vettore"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Input non valido: atteso instanceof ${issue2.expected}, ricevuto ${received}`;
        }
        return `Input non valido: atteso ${expected}, ricevuto ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Input non valido: atteso ${stringifyPrimitive(issue2.values[0])}`;
        return `Opzione non valida: atteso uno tra ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Troppo grande: ${issue2.origin ?? "valore"} deve avere ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elementi"}`;
        return `Troppo grande: ${issue2.origin ?? "valore"} deve essere ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Troppo piccolo: ${issue2.origin} deve avere ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Troppo piccolo: ${issue2.origin} deve essere ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Stringa non valida: deve iniziare con "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Stringa non valida: deve terminare con "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Stringa non valida: deve includere "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Stringa non valida: deve corrispondere al pattern ${_issue.pattern}`;
        return `Input non valido: ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Numero non valido: deve essere un multiplo di ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Chiav${issue2.keys.length > 1 ? "i" : "e"} non riconosciut${issue2.keys.length > 1 ? "e" : "a"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Chiave non valida in ${issue2.origin}`;
      case "invalid_union":
        return "Input non valido";
      case "invalid_element":
        return `Valore non valido in ${issue2.origin}`;
      default:
        return `Input non valido`;
    }
  };
};
function it_default() {
  return {
    localeError: error23()
  };
}

// node_modules/zod/v4/locales/ja.js
var error24 = () => {
  const Sizable = {
    string: { unit: "\u6587\u5B57", verb: "\u3067\u3042\u308B" },
    file: { unit: "\u30D0\u30A4\u30C8", verb: "\u3067\u3042\u308B" },
    array: { unit: "\u8981\u7D20", verb: "\u3067\u3042\u308B" },
    set: { unit: "\u8981\u7D20", verb: "\u3067\u3042\u308B" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u5165\u529B\u5024",
    email: "\u30E1\u30FC\u30EB\u30A2\u30C9\u30EC\u30B9",
    url: "URL",
    emoji: "\u7D75\u6587\u5B57",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO\u65E5\u6642",
    date: "ISO\u65E5\u4ED8",
    time: "ISO\u6642\u523B",
    duration: "ISO\u671F\u9593",
    ipv4: "IPv4\u30A2\u30C9\u30EC\u30B9",
    ipv6: "IPv6\u30A2\u30C9\u30EC\u30B9",
    cidrv4: "IPv4\u7BC4\u56F2",
    cidrv6: "IPv6\u7BC4\u56F2",
    base64: "base64\u30A8\u30F3\u30B3\u30FC\u30C9\u6587\u5B57\u5217",
    base64url: "base64url\u30A8\u30F3\u30B3\u30FC\u30C9\u6587\u5B57\u5217",
    json_string: "JSON\u6587\u5B57\u5217",
    e164: "E.164\u756A\u53F7",
    jwt: "JWT",
    template_literal: "\u5165\u529B\u5024"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u6570\u5024",
    array: "\u914D\u5217"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u7121\u52B9\u306A\u5165\u529B: instanceof ${issue2.expected}\u304C\u671F\u5F85\u3055\u308C\u307E\u3057\u305F\u304C\u3001${received}\u304C\u5165\u529B\u3055\u308C\u307E\u3057\u305F`;
        }
        return `\u7121\u52B9\u306A\u5165\u529B: ${expected}\u304C\u671F\u5F85\u3055\u308C\u307E\u3057\u305F\u304C\u3001${received}\u304C\u5165\u529B\u3055\u308C\u307E\u3057\u305F`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u7121\u52B9\u306A\u5165\u529B: ${stringifyPrimitive(issue2.values[0])}\u304C\u671F\u5F85\u3055\u308C\u307E\u3057\u305F`;
        return `\u7121\u52B9\u306A\u9078\u629E: ${joinValues(issue2.values, "\u3001")}\u306E\u3044\u305A\u308C\u304B\u3067\u3042\u308B\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
      case "too_big": {
        const adj = issue2.inclusive ? "\u4EE5\u4E0B\u3067\u3042\u308B" : "\u3088\u308A\u5C0F\u3055\u3044";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u5927\u304D\u3059\u304E\u308B\u5024: ${issue2.origin ?? "\u5024"}\u306F${issue2.maximum.toString()}${sizing.unit ?? "\u8981\u7D20"}${adj}\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
        return `\u5927\u304D\u3059\u304E\u308B\u5024: ${issue2.origin ?? "\u5024"}\u306F${issue2.maximum.toString()}${adj}\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? "\u4EE5\u4E0A\u3067\u3042\u308B" : "\u3088\u308A\u5927\u304D\u3044";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u5C0F\u3055\u3059\u304E\u308B\u5024: ${issue2.origin}\u306F${issue2.minimum.toString()}${sizing.unit}${adj}\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
        return `\u5C0F\u3055\u3059\u304E\u308B\u5024: ${issue2.origin}\u306F${issue2.minimum.toString()}${adj}\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u7121\u52B9\u306A\u6587\u5B57\u5217: "${_issue.prefix}"\u3067\u59CB\u307E\u308B\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
        if (_issue.format === "ends_with")
          return `\u7121\u52B9\u306A\u6587\u5B57\u5217: "${_issue.suffix}"\u3067\u7D42\u308F\u308B\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
        if (_issue.format === "includes")
          return `\u7121\u52B9\u306A\u6587\u5B57\u5217: "${_issue.includes}"\u3092\u542B\u3080\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
        if (_issue.format === "regex")
          return `\u7121\u52B9\u306A\u6587\u5B57\u5217: \u30D1\u30BF\u30FC\u30F3${_issue.pattern}\u306B\u4E00\u81F4\u3059\u308B\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
        return `\u7121\u52B9\u306A${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u7121\u52B9\u306A\u6570\u5024: ${issue2.divisor}\u306E\u500D\u6570\u3067\u3042\u308B\u5FC5\u8981\u304C\u3042\u308A\u307E\u3059`;
      case "unrecognized_keys":
        return `\u8A8D\u8B58\u3055\u308C\u3066\u3044\u306A\u3044\u30AD\u30FC${issue2.keys.length > 1 ? "\u7FA4" : ""}: ${joinValues(issue2.keys, "\u3001")}`;
      case "invalid_key":
        return `${issue2.origin}\u5185\u306E\u7121\u52B9\u306A\u30AD\u30FC`;
      case "invalid_union":
        return "\u7121\u52B9\u306A\u5165\u529B";
      case "invalid_element":
        return `${issue2.origin}\u5185\u306E\u7121\u52B9\u306A\u5024`;
      default:
        return `\u7121\u52B9\u306A\u5165\u529B`;
    }
  };
};
function ja_default() {
  return {
    localeError: error24()
  };
}

// node_modules/zod/v4/locales/ka.js
var error25 = () => {
  const Sizable = {
    string: { unit: "\u10E1\u10D8\u10DB\u10D1\u10DD\u10DA\u10DD", verb: "\u10E3\u10DC\u10D3\u10D0 \u10E8\u10D4\u10D8\u10EA\u10D0\u10D5\u10D3\u10D4\u10E1" },
    file: { unit: "\u10D1\u10D0\u10D8\u10E2\u10D8", verb: "\u10E3\u10DC\u10D3\u10D0 \u10E8\u10D4\u10D8\u10EA\u10D0\u10D5\u10D3\u10D4\u10E1" },
    array: { unit: "\u10D4\u10DA\u10D4\u10DB\u10D4\u10DC\u10E2\u10D8", verb: "\u10E3\u10DC\u10D3\u10D0 \u10E8\u10D4\u10D8\u10EA\u10D0\u10D5\u10D3\u10D4\u10E1" },
    set: { unit: "\u10D4\u10DA\u10D4\u10DB\u10D4\u10DC\u10E2\u10D8", verb: "\u10E3\u10DC\u10D3\u10D0 \u10E8\u10D4\u10D8\u10EA\u10D0\u10D5\u10D3\u10D4\u10E1" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u10E8\u10D4\u10E7\u10D5\u10D0\u10DC\u10D0",
    email: "\u10D4\u10DA-\u10E4\u10DD\u10E1\u10E2\u10D8\u10E1 \u10DB\u10D8\u10E1\u10D0\u10DB\u10D0\u10E0\u10D7\u10D8",
    url: "URL",
    emoji: "\u10D4\u10DB\u10DD\u10EF\u10D8",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "\u10D7\u10D0\u10E0\u10D8\u10E6\u10D8-\u10D3\u10E0\u10DD",
    date: "\u10D7\u10D0\u10E0\u10D8\u10E6\u10D8",
    time: "\u10D3\u10E0\u10DD",
    duration: "\u10EE\u10D0\u10DC\u10D2\u10E0\u10EB\u10DA\u10D8\u10D5\u10DD\u10D1\u10D0",
    ipv4: "IPv4 \u10DB\u10D8\u10E1\u10D0\u10DB\u10D0\u10E0\u10D7\u10D8",
    ipv6: "IPv6 \u10DB\u10D8\u10E1\u10D0\u10DB\u10D0\u10E0\u10D7\u10D8",
    cidrv4: "IPv4 \u10D3\u10D8\u10D0\u10DE\u10D0\u10D6\u10DD\u10DC\u10D8",
    cidrv6: "IPv6 \u10D3\u10D8\u10D0\u10DE\u10D0\u10D6\u10DD\u10DC\u10D8",
    base64: "base64-\u10D9\u10DD\u10D3\u10D8\u10E0\u10D4\u10D1\u10E3\u10DA\u10D8 \u10D5\u10D4\u10DA\u10D8",
    base64url: "base64url-\u10D9\u10DD\u10D3\u10D8\u10E0\u10D4\u10D1\u10E3\u10DA\u10D8 \u10D5\u10D4\u10DA\u10D8",
    json_string: "JSON \u10D5\u10D4\u10DA\u10D8",
    e164: "E.164 \u10DC\u10DD\u10DB\u10D4\u10E0\u10D8",
    jwt: "JWT",
    template_literal: "\u10E8\u10D4\u10E7\u10D5\u10D0\u10DC\u10D0"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u10E0\u10D8\u10EA\u10EE\u10D5\u10D8",
    string: "\u10D5\u10D4\u10DA\u10D8",
    boolean: "\u10D1\u10E3\u10DA\u10D4\u10D0\u10DC\u10D8",
    function: "\u10E4\u10E3\u10DC\u10E5\u10EA\u10D8\u10D0",
    array: "\u10DB\u10D0\u10E1\u10D8\u10D5\u10D8"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10E8\u10D4\u10E7\u10D5\u10D0\u10DC\u10D0: \u10DB\u10DD\u10E1\u10D0\u10DA\u10DD\u10D3\u10DC\u10D4\u10DA\u10D8 instanceof ${issue2.expected}, \u10DB\u10D8\u10E6\u10D4\u10D1\u10E3\u10DA\u10D8 ${received}`;
        }
        return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10E8\u10D4\u10E7\u10D5\u10D0\u10DC\u10D0: \u10DB\u10DD\u10E1\u10D0\u10DA\u10DD\u10D3\u10DC\u10D4\u10DA\u10D8 ${expected}, \u10DB\u10D8\u10E6\u10D4\u10D1\u10E3\u10DA\u10D8 ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10E8\u10D4\u10E7\u10D5\u10D0\u10DC\u10D0: \u10DB\u10DD\u10E1\u10D0\u10DA\u10DD\u10D3\u10DC\u10D4\u10DA\u10D8 ${stringifyPrimitive(issue2.values[0])}`;
        return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10D5\u10D0\u10E0\u10D8\u10D0\u10DC\u10E2\u10D8: \u10DB\u10DD\u10E1\u10D0\u10DA\u10DD\u10D3\u10DC\u10D4\u10DA\u10D8\u10D0 \u10D4\u10E0\u10D7-\u10D4\u10E0\u10D7\u10D8 ${joinValues(issue2.values, "|")}-\u10D3\u10D0\u10DC`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u10D6\u10D4\u10D3\u10DB\u10D4\u10E2\u10D0\u10D3 \u10D3\u10D8\u10D3\u10D8: \u10DB\u10DD\u10E1\u10D0\u10DA\u10DD\u10D3\u10DC\u10D4\u10DA\u10D8 ${issue2.origin ?? "\u10DB\u10DC\u10D8\u10E8\u10D5\u10DC\u10D4\u10DA\u10DD\u10D1\u10D0"} ${sizing.verb} ${adj}${issue2.maximum.toString()} ${sizing.unit}`;
        return `\u10D6\u10D4\u10D3\u10DB\u10D4\u10E2\u10D0\u10D3 \u10D3\u10D8\u10D3\u10D8: \u10DB\u10DD\u10E1\u10D0\u10DA\u10DD\u10D3\u10DC\u10D4\u10DA\u10D8 ${issue2.origin ?? "\u10DB\u10DC\u10D8\u10E8\u10D5\u10DC\u10D4\u10DA\u10DD\u10D1\u10D0"} \u10D8\u10E7\u10DD\u10E1 ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u10D6\u10D4\u10D3\u10DB\u10D4\u10E2\u10D0\u10D3 \u10DE\u10D0\u10E2\u10D0\u10E0\u10D0: \u10DB\u10DD\u10E1\u10D0\u10DA\u10DD\u10D3\u10DC\u10D4\u10DA\u10D8 ${issue2.origin} ${sizing.verb} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u10D6\u10D4\u10D3\u10DB\u10D4\u10E2\u10D0\u10D3 \u10DE\u10D0\u10E2\u10D0\u10E0\u10D0: \u10DB\u10DD\u10E1\u10D0\u10DA\u10DD\u10D3\u10DC\u10D4\u10DA\u10D8 ${issue2.origin} \u10D8\u10E7\u10DD\u10E1 ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10D5\u10D4\u10DA\u10D8: \u10E3\u10DC\u10D3\u10D0 \u10D8\u10EC\u10E7\u10D4\u10D1\u10DD\u10D3\u10D4\u10E1 "${_issue.prefix}"-\u10D8\u10D7`;
        }
        if (_issue.format === "ends_with")
          return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10D5\u10D4\u10DA\u10D8: \u10E3\u10DC\u10D3\u10D0 \u10DB\u10D7\u10D0\u10D5\u10E0\u10D3\u10D4\u10D1\u10DD\u10D3\u10D4\u10E1 "${_issue.suffix}"-\u10D8\u10D7`;
        if (_issue.format === "includes")
          return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10D5\u10D4\u10DA\u10D8: \u10E3\u10DC\u10D3\u10D0 \u10E8\u10D4\u10D8\u10EA\u10D0\u10D5\u10D3\u10D4\u10E1 "${_issue.includes}"-\u10E1`;
        if (_issue.format === "regex")
          return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10D5\u10D4\u10DA\u10D8: \u10E3\u10DC\u10D3\u10D0 \u10E8\u10D4\u10D4\u10E1\u10D0\u10D1\u10D0\u10DB\u10D4\u10D1\u10DD\u10D3\u10D4\u10E1 \u10E8\u10D0\u10D1\u10DA\u10DD\u10DC\u10E1 ${_issue.pattern}`;
        return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10E0\u10D8\u10EA\u10EE\u10D5\u10D8: \u10E3\u10DC\u10D3\u10D0 \u10D8\u10E7\u10DD\u10E1 ${issue2.divisor}-\u10D8\u10E1 \u10EF\u10D4\u10E0\u10D0\u10D3\u10D8`;
      case "unrecognized_keys":
        return `\u10E3\u10EA\u10DC\u10DD\u10D1\u10D8 \u10D2\u10D0\u10E1\u10D0\u10E6\u10D4\u10D1${issue2.keys.length > 1 ? "\u10D4\u10D1\u10D8" : "\u10D8"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10D2\u10D0\u10E1\u10D0\u10E6\u10D4\u10D1\u10D8 ${issue2.origin}-\u10E8\u10D8`;
      case "invalid_union":
        return "\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10E8\u10D4\u10E7\u10D5\u10D0\u10DC\u10D0";
      case "invalid_element":
        return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10DB\u10DC\u10D8\u10E8\u10D5\u10DC\u10D4\u10DA\u10DD\u10D1\u10D0 ${issue2.origin}-\u10E8\u10D8`;
      default:
        return `\u10D0\u10E0\u10D0\u10E1\u10EC\u10DD\u10E0\u10D8 \u10E8\u10D4\u10E7\u10D5\u10D0\u10DC\u10D0`;
    }
  };
};
function ka_default() {
  return {
    localeError: error25()
  };
}

// node_modules/zod/v4/locales/km.js
var error26 = () => {
  const Sizable = {
    string: { unit: "\u178F\u17BD\u17A2\u1780\u17D2\u179F\u179A", verb: "\u1782\u17BD\u179A\u1798\u17B6\u1793" },
    file: { unit: "\u1794\u17C3", verb: "\u1782\u17BD\u179A\u1798\u17B6\u1793" },
    array: { unit: "\u1792\u17B6\u178F\u17BB", verb: "\u1782\u17BD\u179A\u1798\u17B6\u1793" },
    set: { unit: "\u1792\u17B6\u178F\u17BB", verb: "\u1782\u17BD\u179A\u1798\u17B6\u1793" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u1791\u17B7\u1793\u17D2\u1793\u1793\u17D0\u1799\u1794\u1789\u17D2\u1785\u17BC\u179B",
    email: "\u17A2\u17B6\u179F\u1799\u178A\u17D2\u178B\u17B6\u1793\u17A2\u17CA\u17B8\u1798\u17C2\u179B",
    url: "URL",
    emoji: "\u179F\u1789\u17D2\u1789\u17B6\u17A2\u17B6\u179A\u1798\u17D2\u1798\u178E\u17CD",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "\u1780\u17B6\u179B\u1794\u179A\u17B7\u1785\u17D2\u1786\u17C1\u1791 \u1793\u17B7\u1784\u1798\u17C9\u17C4\u1784 ISO",
    date: "\u1780\u17B6\u179B\u1794\u179A\u17B7\u1785\u17D2\u1786\u17C1\u1791 ISO",
    time: "\u1798\u17C9\u17C4\u1784 ISO",
    duration: "\u179A\u1799\u17C8\u1796\u17C1\u179B ISO",
    ipv4: "\u17A2\u17B6\u179F\u1799\u178A\u17D2\u178B\u17B6\u1793 IPv4",
    ipv6: "\u17A2\u17B6\u179F\u1799\u178A\u17D2\u178B\u17B6\u1793 IPv6",
    cidrv4: "\u178A\u17C2\u1793\u17A2\u17B6\u179F\u1799\u178A\u17D2\u178B\u17B6\u1793 IPv4",
    cidrv6: "\u178A\u17C2\u1793\u17A2\u17B6\u179F\u1799\u178A\u17D2\u178B\u17B6\u1793 IPv6",
    base64: "\u1781\u17D2\u179F\u17C2\u17A2\u1780\u17D2\u179F\u179A\u17A2\u17CA\u17B7\u1780\u17BC\u178A base64",
    base64url: "\u1781\u17D2\u179F\u17C2\u17A2\u1780\u17D2\u179F\u179A\u17A2\u17CA\u17B7\u1780\u17BC\u178A base64url",
    json_string: "\u1781\u17D2\u179F\u17C2\u17A2\u1780\u17D2\u179F\u179A JSON",
    e164: "\u179B\u17C1\u1781 E.164",
    jwt: "JWT",
    template_literal: "\u1791\u17B7\u1793\u17D2\u1793\u1793\u17D0\u1799\u1794\u1789\u17D2\u1785\u17BC\u179B"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u179B\u17C1\u1781",
    array: "\u17A2\u17B6\u179A\u17C1 (Array)",
    null: "\u1782\u17D2\u1798\u17B6\u1793\u178F\u1798\u17D2\u179B\u17C3 (null)"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u1791\u17B7\u1793\u17D2\u1793\u1793\u17D0\u1799\u1794\u1789\u17D2\u1785\u17BC\u179B\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1780\u17B6\u179A instanceof ${issue2.expected} \u1794\u17C9\u17BB\u1793\u17D2\u178F\u17C2\u1791\u1791\u17BD\u179B\u1794\u17B6\u1793 ${received}`;
        }
        return `\u1791\u17B7\u1793\u17D2\u1793\u1793\u17D0\u1799\u1794\u1789\u17D2\u1785\u17BC\u179B\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1780\u17B6\u179A ${expected} \u1794\u17C9\u17BB\u1793\u17D2\u178F\u17C2\u1791\u1791\u17BD\u179B\u1794\u17B6\u1793 ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u1791\u17B7\u1793\u17D2\u1793\u1793\u17D0\u1799\u1794\u1789\u17D2\u1785\u17BC\u179B\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1780\u17B6\u179A ${stringifyPrimitive(issue2.values[0])}`;
        return `\u1787\u1798\u17D2\u179A\u17BE\u179F\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1787\u17B6\u1798\u17BD\u1799\u1780\u17D2\u1793\u17BB\u1784\u1785\u17C6\u178E\u17C4\u1798 ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u1792\u17C6\u1796\u17C1\u1780\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1780\u17B6\u179A ${issue2.origin ?? "\u178F\u1798\u17D2\u179B\u17C3"} ${adj} ${issue2.maximum.toString()} ${sizing.unit ?? "\u1792\u17B6\u178F\u17BB"}`;
        return `\u1792\u17C6\u1796\u17C1\u1780\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1780\u17B6\u179A ${issue2.origin ?? "\u178F\u1798\u17D2\u179B\u17C3"} ${adj} ${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u178F\u17BC\u1785\u1796\u17C1\u1780\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1780\u17B6\u179A ${issue2.origin} ${adj} ${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u178F\u17BC\u1785\u1796\u17C1\u1780\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1780\u17B6\u179A ${issue2.origin} ${adj} ${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u1781\u17D2\u179F\u17C2\u17A2\u1780\u17D2\u179F\u179A\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1785\u17B6\u1794\u17CB\u1795\u17D2\u178F\u17BE\u1798\u178A\u17C4\u1799 "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `\u1781\u17D2\u179F\u17C2\u17A2\u1780\u17D2\u179F\u179A\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1794\u1789\u17D2\u1785\u1794\u17CB\u178A\u17C4\u1799 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u1781\u17D2\u179F\u17C2\u17A2\u1780\u17D2\u179F\u179A\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u1798\u17B6\u1793 "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u1781\u17D2\u179F\u17C2\u17A2\u1780\u17D2\u179F\u179A\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u178F\u17C2\u1795\u17D2\u1782\u17BC\u1795\u17D2\u1782\u1784\u1793\u17B9\u1784\u1791\u1798\u17D2\u179A\u1784\u17CB\u178A\u17C2\u179B\u1794\u17B6\u1793\u1780\u17C6\u178E\u178F\u17CB ${_issue.pattern}`;
        return `\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u179B\u17C1\u1781\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u17D6 \u178F\u17D2\u179A\u17BC\u179C\u178F\u17C2\u1787\u17B6\u1796\u17A0\u17BB\u1782\u17BB\u178E\u1793\u17C3 ${issue2.divisor}`;
      case "unrecognized_keys":
        return `\u179A\u1780\u1783\u17BE\u1789\u179F\u17C4\u1798\u17B7\u1793\u179F\u17D2\u1782\u17B6\u179B\u17CB\u17D6 ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u179F\u17C4\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u1793\u17C5\u1780\u17D2\u1793\u17BB\u1784 ${issue2.origin}`;
      case "invalid_union":
        return `\u1791\u17B7\u1793\u17D2\u1793\u1793\u17D0\u1799\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C`;
      case "invalid_element":
        return `\u1791\u17B7\u1793\u17D2\u1793\u1793\u17D0\u1799\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C\u1793\u17C5\u1780\u17D2\u1793\u17BB\u1784 ${issue2.origin}`;
      default:
        return `\u1791\u17B7\u1793\u17D2\u1793\u1793\u17D0\u1799\u1798\u17B7\u1793\u178F\u17D2\u179A\u17B9\u1798\u178F\u17D2\u179A\u17BC\u179C`;
    }
  };
};
function km_default() {
  return {
    localeError: error26()
  };
}

// node_modules/zod/v4/locales/kh.js
function kh_default() {
  return km_default();
}

// node_modules/zod/v4/locales/ko.js
var error27 = () => {
  const Sizable = {
    string: { unit: "\uBB38\uC790", verb: "to have" },
    file: { unit: "\uBC14\uC774\uD2B8", verb: "to have" },
    array: { unit: "\uAC1C", verb: "to have" },
    set: { unit: "\uAC1C", verb: "to have" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\uC785\uB825",
    email: "\uC774\uBA54\uC77C \uC8FC\uC18C",
    url: "URL",
    emoji: "\uC774\uBAA8\uC9C0",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO \uB0A0\uC9DC\uC2DC\uAC04",
    date: "ISO \uB0A0\uC9DC",
    time: "ISO \uC2DC\uAC04",
    duration: "ISO \uAE30\uAC04",
    ipv4: "IPv4 \uC8FC\uC18C",
    ipv6: "IPv6 \uC8FC\uC18C",
    cidrv4: "IPv4 \uBC94\uC704",
    cidrv6: "IPv6 \uBC94\uC704",
    base64: "base64 \uC778\uCF54\uB529 \uBB38\uC790\uC5F4",
    base64url: "base64url \uC778\uCF54\uB529 \uBB38\uC790\uC5F4",
    json_string: "JSON \uBB38\uC790\uC5F4",
    e164: "E.164 \uBC88\uD638",
    jwt: "JWT",
    template_literal: "\uC785\uB825"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\uC798\uBABB\uB41C \uC785\uB825: \uC608\uC0C1 \uD0C0\uC785\uC740 instanceof ${issue2.expected}, \uBC1B\uC740 \uD0C0\uC785\uC740 ${received}\uC785\uB2C8\uB2E4`;
        }
        return `\uC798\uBABB\uB41C \uC785\uB825: \uC608\uC0C1 \uD0C0\uC785\uC740 ${expected}, \uBC1B\uC740 \uD0C0\uC785\uC740 ${received}\uC785\uB2C8\uB2E4`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\uC798\uBABB\uB41C \uC785\uB825: \uAC12\uC740 ${stringifyPrimitive(issue2.values[0])} \uC774\uC5B4\uC57C \uD569\uB2C8\uB2E4`;
        return `\uC798\uBABB\uB41C \uC635\uC158: ${joinValues(issue2.values, "\uB610\uB294 ")} \uC911 \uD558\uB098\uC5EC\uC57C \uD569\uB2C8\uB2E4`;
      case "too_big": {
        const adj = issue2.inclusive ? "\uC774\uD558" : "\uBBF8\uB9CC";
        const suffix = adj === "\uBBF8\uB9CC" ? "\uC774\uC5B4\uC57C \uD569\uB2C8\uB2E4" : "\uC5EC\uC57C \uD569\uB2C8\uB2E4";
        const sizing = getSizing(issue2.origin);
        const unit = sizing?.unit ?? "\uC694\uC18C";
        if (sizing)
          return `${issue2.origin ?? "\uAC12"}\uC774 \uB108\uBB34 \uD07D\uB2C8\uB2E4: ${issue2.maximum.toString()}${unit} ${adj}${suffix}`;
        return `${issue2.origin ?? "\uAC12"}\uC774 \uB108\uBB34 \uD07D\uB2C8\uB2E4: ${issue2.maximum.toString()} ${adj}${suffix}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? "\uC774\uC0C1" : "\uCD08\uACFC";
        const suffix = adj === "\uC774\uC0C1" ? "\uC774\uC5B4\uC57C \uD569\uB2C8\uB2E4" : "\uC5EC\uC57C \uD569\uB2C8\uB2E4";
        const sizing = getSizing(issue2.origin);
        const unit = sizing?.unit ?? "\uC694\uC18C";
        if (sizing) {
          return `${issue2.origin ?? "\uAC12"}\uC774 \uB108\uBB34 \uC791\uC2B5\uB2C8\uB2E4: ${issue2.minimum.toString()}${unit} ${adj}${suffix}`;
        }
        return `${issue2.origin ?? "\uAC12"}\uC774 \uB108\uBB34 \uC791\uC2B5\uB2C8\uB2E4: ${issue2.minimum.toString()} ${adj}${suffix}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\uC798\uBABB\uB41C \uBB38\uC790\uC5F4: "${_issue.prefix}"(\uC73C)\uB85C \uC2DC\uC791\uD574\uC57C \uD569\uB2C8\uB2E4`;
        }
        if (_issue.format === "ends_with")
          return `\uC798\uBABB\uB41C \uBB38\uC790\uC5F4: "${_issue.suffix}"(\uC73C)\uB85C \uB05D\uB098\uC57C \uD569\uB2C8\uB2E4`;
        if (_issue.format === "includes")
          return `\uC798\uBABB\uB41C \uBB38\uC790\uC5F4: "${_issue.includes}"\uC744(\uB97C) \uD3EC\uD568\uD574\uC57C \uD569\uB2C8\uB2E4`;
        if (_issue.format === "regex")
          return `\uC798\uBABB\uB41C \uBB38\uC790\uC5F4: \uC815\uADDC\uC2DD ${_issue.pattern} \uD328\uD134\uACFC \uC77C\uCE58\uD574\uC57C \uD569\uB2C8\uB2E4`;
        return `\uC798\uBABB\uB41C ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\uC798\uBABB\uB41C \uC22B\uC790: ${issue2.divisor}\uC758 \uBC30\uC218\uC5EC\uC57C \uD569\uB2C8\uB2E4`;
      case "unrecognized_keys":
        return `\uC778\uC2DD\uD560 \uC218 \uC5C6\uB294 \uD0A4: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\uC798\uBABB\uB41C \uD0A4: ${issue2.origin}`;
      case "invalid_union":
        return `\uC798\uBABB\uB41C \uC785\uB825`;
      case "invalid_element":
        return `\uC798\uBABB\uB41C \uAC12: ${issue2.origin}`;
      default:
        return `\uC798\uBABB\uB41C \uC785\uB825`;
    }
  };
};
function ko_default() {
  return {
    localeError: error27()
  };
}

// node_modules/zod/v4/locales/lt.js
var capitalizeFirstCharacter = (text) => {
  return text.charAt(0).toUpperCase() + text.slice(1);
};
function getUnitTypeFromNumber(number4) {
  const abs = Math.abs(number4);
  const last = abs % 10;
  const last2 = abs % 100;
  if (last2 >= 11 && last2 <= 19 || last === 0)
    return "many";
  if (last === 1)
    return "one";
  return "few";
}
var error28 = () => {
  const Sizable = {
    string: {
      unit: {
        one: "simbolis",
        few: "simboliai",
        many: "simboli\u0173"
      },
      verb: {
        smaller: {
          inclusive: "turi b\u016Bti ne ilgesn\u0117 kaip",
          notInclusive: "turi b\u016Bti trumpesn\u0117 kaip"
        },
        bigger: {
          inclusive: "turi b\u016Bti ne trumpesn\u0117 kaip",
          notInclusive: "turi b\u016Bti ilgesn\u0117 kaip"
        }
      }
    },
    file: {
      unit: {
        one: "baitas",
        few: "baitai",
        many: "bait\u0173"
      },
      verb: {
        smaller: {
          inclusive: "turi b\u016Bti ne didesnis kaip",
          notInclusive: "turi b\u016Bti ma\u017Eesnis kaip"
        },
        bigger: {
          inclusive: "turi b\u016Bti ne ma\u017Eesnis kaip",
          notInclusive: "turi b\u016Bti didesnis kaip"
        }
      }
    },
    array: {
      unit: {
        one: "element\u0105",
        few: "elementus",
        many: "element\u0173"
      },
      verb: {
        smaller: {
          inclusive: "turi tur\u0117ti ne daugiau kaip",
          notInclusive: "turi tur\u0117ti ma\u017Eiau kaip"
        },
        bigger: {
          inclusive: "turi tur\u0117ti ne ma\u017Eiau kaip",
          notInclusive: "turi tur\u0117ti daugiau kaip"
        }
      }
    },
    set: {
      unit: {
        one: "element\u0105",
        few: "elementus",
        many: "element\u0173"
      },
      verb: {
        smaller: {
          inclusive: "turi tur\u0117ti ne daugiau kaip",
          notInclusive: "turi tur\u0117ti ma\u017Eiau kaip"
        },
        bigger: {
          inclusive: "turi tur\u0117ti ne ma\u017Eiau kaip",
          notInclusive: "turi tur\u0117ti daugiau kaip"
        }
      }
    }
  };
  function getSizing(origin, unitType, inclusive, targetShouldBe) {
    const result = Sizable[origin] ?? null;
    if (result === null)
      return result;
    return {
      unit: result.unit[unitType],
      verb: result.verb[targetShouldBe][inclusive ? "inclusive" : "notInclusive"]
    };
  }
  const FormatDictionary = {
    regex: "\u012Fvestis",
    email: "el. pa\u0161to adresas",
    url: "URL",
    emoji: "jaustukas",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO data ir laikas",
    date: "ISO data",
    time: "ISO laikas",
    duration: "ISO trukm\u0117",
    ipv4: "IPv4 adresas",
    ipv6: "IPv6 adresas",
    cidrv4: "IPv4 tinklo prefiksas (CIDR)",
    cidrv6: "IPv6 tinklo prefiksas (CIDR)",
    base64: "base64 u\u017Ekoduota eilut\u0117",
    base64url: "base64url u\u017Ekoduota eilut\u0117",
    json_string: "JSON eilut\u0117",
    e164: "E.164 numeris",
    jwt: "JWT",
    template_literal: "\u012Fvestis"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "skai\u010Dius",
    bigint: "sveikasis skai\u010Dius",
    string: "eilut\u0117",
    boolean: "login\u0117 reik\u0161m\u0117",
    undefined: "neapibr\u0117\u017Eta reik\u0161m\u0117",
    function: "funkcija",
    symbol: "simbolis",
    array: "masyvas",
    object: "objektas",
    null: "nulin\u0117 reik\u0161m\u0117"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Gautas tipas ${received}, o tik\u0117tasi - instanceof ${issue2.expected}`;
        }
        return `Gautas tipas ${received}, o tik\u0117tasi - ${expected}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Privalo b\u016Bti ${stringifyPrimitive(issue2.values[0])}`;
        return `Privalo b\u016Bti vienas i\u0161 ${joinValues(issue2.values, "|")} pasirinkim\u0173`;
      case "too_big": {
        const origin = TypeDictionary[issue2.origin] ?? issue2.origin;
        const sizing = getSizing(issue2.origin, getUnitTypeFromNumber(Number(issue2.maximum)), issue2.inclusive ?? false, "smaller");
        if (sizing?.verb)
          return `${capitalizeFirstCharacter(origin ?? issue2.origin ?? "reik\u0161m\u0117")} ${sizing.verb} ${issue2.maximum.toString()} ${sizing.unit ?? "element\u0173"}`;
        const adj = issue2.inclusive ? "ne didesnis kaip" : "ma\u017Eesnis kaip";
        return `${capitalizeFirstCharacter(origin ?? issue2.origin ?? "reik\u0161m\u0117")} turi b\u016Bti ${adj} ${issue2.maximum.toString()} ${sizing?.unit}`;
      }
      case "too_small": {
        const origin = TypeDictionary[issue2.origin] ?? issue2.origin;
        const sizing = getSizing(issue2.origin, getUnitTypeFromNumber(Number(issue2.minimum)), issue2.inclusive ?? false, "bigger");
        if (sizing?.verb)
          return `${capitalizeFirstCharacter(origin ?? issue2.origin ?? "reik\u0161m\u0117")} ${sizing.verb} ${issue2.minimum.toString()} ${sizing.unit ?? "element\u0173"}`;
        const adj = issue2.inclusive ? "ne ma\u017Eesnis kaip" : "didesnis kaip";
        return `${capitalizeFirstCharacter(origin ?? issue2.origin ?? "reik\u0161m\u0117")} turi b\u016Bti ${adj} ${issue2.minimum.toString()} ${sizing?.unit}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `Eilut\u0117 privalo prasid\u0117ti "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `Eilut\u0117 privalo pasibaigti "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Eilut\u0117 privalo \u012Ftraukti "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Eilut\u0117 privalo atitikti ${_issue.pattern}`;
        return `Neteisingas ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Skai\u010Dius privalo b\u016Bti ${issue2.divisor} kartotinis.`;
      case "unrecognized_keys":
        return `Neatpa\u017Eint${issue2.keys.length > 1 ? "i" : "as"} rakt${issue2.keys.length > 1 ? "ai" : "as"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return "Rastas klaidingas raktas";
      case "invalid_union":
        return "Klaidinga \u012Fvestis";
      case "invalid_element": {
        const origin = TypeDictionary[issue2.origin] ?? issue2.origin;
        return `${capitalizeFirstCharacter(origin ?? issue2.origin ?? "reik\u0161m\u0117")} turi klaiding\u0105 \u012Fvest\u012F`;
      }
      default:
        return "Klaidinga \u012Fvestis";
    }
  };
};
function lt_default() {
  return {
    localeError: error28()
  };
}

// node_modules/zod/v4/locales/mk.js
var error29 = () => {
  const Sizable = {
    string: { unit: "\u0437\u043D\u0430\u0446\u0438", verb: "\u0434\u0430 \u0438\u043C\u0430\u0430\u0442" },
    file: { unit: "\u0431\u0430\u0458\u0442\u0438", verb: "\u0434\u0430 \u0438\u043C\u0430\u0430\u0442" },
    array: { unit: "\u0441\u0442\u0430\u0432\u043A\u0438", verb: "\u0434\u0430 \u0438\u043C\u0430\u0430\u0442" },
    set: { unit: "\u0441\u0442\u0430\u0432\u043A\u0438", verb: "\u0434\u0430 \u0438\u043C\u0430\u0430\u0442" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0432\u043D\u0435\u0441",
    email: "\u0430\u0434\u0440\u0435\u0441\u0430 \u043D\u0430 \u0435-\u043F\u043E\u0448\u0442\u0430",
    url: "URL",
    emoji: "\u0435\u043C\u043E\u045F\u0438",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO \u0434\u0430\u0442\u0443\u043C \u0438 \u0432\u0440\u0435\u043C\u0435",
    date: "ISO \u0434\u0430\u0442\u0443\u043C",
    time: "ISO \u0432\u0440\u0435\u043C\u0435",
    duration: "ISO \u0432\u0440\u0435\u043C\u0435\u0442\u0440\u0430\u0435\u045A\u0435",
    ipv4: "IPv4 \u0430\u0434\u0440\u0435\u0441\u0430",
    ipv6: "IPv6 \u0430\u0434\u0440\u0435\u0441\u0430",
    cidrv4: "IPv4 \u043E\u043F\u0441\u0435\u0433",
    cidrv6: "IPv6 \u043E\u043F\u0441\u0435\u0433",
    base64: "base64-\u0435\u043D\u043A\u043E\u0434\u0438\u0440\u0430\u043D\u0430 \u043D\u0438\u0437\u0430",
    base64url: "base64url-\u0435\u043D\u043A\u043E\u0434\u0438\u0440\u0430\u043D\u0430 \u043D\u0438\u0437\u0430",
    json_string: "JSON \u043D\u0438\u0437\u0430",
    e164: "E.164 \u0431\u0440\u043E\u0458",
    jwt: "JWT",
    template_literal: "\u0432\u043D\u0435\u0441"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0431\u0440\u043E\u0458",
    array: "\u043D\u0438\u0437\u0430"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u0413\u0440\u0435\u0448\u0435\u043D \u0432\u043D\u0435\u0441: \u0441\u0435 \u043E\u0447\u0435\u043A\u0443\u0432\u0430 instanceof ${issue2.expected}, \u043F\u0440\u0438\u043C\u0435\u043D\u043E ${received}`;
        }
        return `\u0413\u0440\u0435\u0448\u0435\u043D \u0432\u043D\u0435\u0441: \u0441\u0435 \u043E\u0447\u0435\u043A\u0443\u0432\u0430 ${expected}, \u043F\u0440\u0438\u043C\u0435\u043D\u043E ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Invalid input: expected ${stringifyPrimitive(issue2.values[0])}`;
        return `\u0413\u0440\u0435\u0448\u0430\u043D\u0430 \u043E\u043F\u0446\u0438\u0458\u0430: \u0441\u0435 \u043E\u0447\u0435\u043A\u0443\u0432\u0430 \u0435\u0434\u043D\u0430 ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u041F\u0440\u0435\u043C\u043D\u043E\u0433\u0443 \u0433\u043E\u043B\u0435\u043C: \u0441\u0435 \u043E\u0447\u0435\u043A\u0443\u0432\u0430 ${issue2.origin ?? "\u0432\u0440\u0435\u0434\u043D\u043E\u0441\u0442\u0430"} \u0434\u0430 \u0438\u043C\u0430 ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u0435\u043B\u0435\u043C\u0435\u043D\u0442\u0438"}`;
        return `\u041F\u0440\u0435\u043C\u043D\u043E\u0433\u0443 \u0433\u043E\u043B\u0435\u043C: \u0441\u0435 \u043E\u0447\u0435\u043A\u0443\u0432\u0430 ${issue2.origin ?? "\u0432\u0440\u0435\u0434\u043D\u043E\u0441\u0442\u0430"} \u0434\u0430 \u0431\u0438\u0434\u0435 ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u041F\u0440\u0435\u043C\u043D\u043E\u0433\u0443 \u043C\u0430\u043B: \u0441\u0435 \u043E\u0447\u0435\u043A\u0443\u0432\u0430 ${issue2.origin} \u0434\u0430 \u0438\u043C\u0430 ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u041F\u0440\u0435\u043C\u043D\u043E\u0433\u0443 \u043C\u0430\u043B: \u0441\u0435 \u043E\u0447\u0435\u043A\u0443\u0432\u0430 ${issue2.origin} \u0434\u0430 \u0431\u0438\u0434\u0435 ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u041D\u0435\u0432\u0430\u0436\u0435\u0447\u043A\u0430 \u043D\u0438\u0437\u0430: \u043C\u043E\u0440\u0430 \u0434\u0430 \u0437\u0430\u043F\u043E\u0447\u043D\u0443\u0432\u0430 \u0441\u043E "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `\u041D\u0435\u0432\u0430\u0436\u0435\u0447\u043A\u0430 \u043D\u0438\u0437\u0430: \u043C\u043E\u0440\u0430 \u0434\u0430 \u0437\u0430\u0432\u0440\u0448\u0443\u0432\u0430 \u0441\u043E "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u041D\u0435\u0432\u0430\u0436\u0435\u0447\u043A\u0430 \u043D\u0438\u0437\u0430: \u043C\u043E\u0440\u0430 \u0434\u0430 \u0432\u043A\u043B\u0443\u0447\u0443\u0432\u0430 "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u041D\u0435\u0432\u0430\u0436\u0435\u0447\u043A\u0430 \u043D\u0438\u0437\u0430: \u043C\u043E\u0440\u0430 \u0434\u0430 \u043E\u0434\u0433\u043E\u0430\u0440\u0430 \u043D\u0430 \u043F\u0430\u0442\u0435\u0440\u043D\u043E\u0442 ${_issue.pattern}`;
        return `Invalid ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u0413\u0440\u0435\u0448\u0435\u043D \u0431\u0440\u043E\u0458: \u043C\u043E\u0440\u0430 \u0434\u0430 \u0431\u0438\u0434\u0435 \u0434\u0435\u043B\u0438\u0432 \u0441\u043E ${issue2.divisor}`;
      case "unrecognized_keys":
        return `${issue2.keys.length > 1 ? "\u041D\u0435\u043F\u0440\u0435\u043F\u043E\u0437\u043D\u0430\u0435\u043D\u0438 \u043A\u043B\u0443\u0447\u0435\u0432\u0438" : "\u041D\u0435\u043F\u0440\u0435\u043F\u043E\u0437\u043D\u0430\u0435\u043D \u043A\u043B\u0443\u0447"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u0413\u0440\u0435\u0448\u0435\u043D \u043A\u043B\u0443\u0447 \u0432\u043E ${issue2.origin}`;
      case "invalid_union":
        return "\u0413\u0440\u0435\u0448\u0435\u043D \u0432\u043D\u0435\u0441";
      case "invalid_element":
        return `\u0413\u0440\u0435\u0448\u043D\u0430 \u0432\u0440\u0435\u0434\u043D\u043E\u0441\u0442 \u0432\u043E ${issue2.origin}`;
      default:
        return `\u0413\u0440\u0435\u0448\u0435\u043D \u0432\u043D\u0435\u0441`;
    }
  };
};
function mk_default() {
  return {
    localeError: error29()
  };
}

// node_modules/zod/v4/locales/ms.js
var error30 = () => {
  const Sizable = {
    string: { unit: "aksara", verb: "mempunyai" },
    file: { unit: "bait", verb: "mempunyai" },
    array: { unit: "elemen", verb: "mempunyai" },
    set: { unit: "elemen", verb: "mempunyai" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "input",
    email: "alamat e-mel",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "tarikh masa ISO",
    date: "tarikh ISO",
    time: "masa ISO",
    duration: "tempoh ISO",
    ipv4: "alamat IPv4",
    ipv6: "alamat IPv6",
    cidrv4: "julat IPv4",
    cidrv6: "julat IPv6",
    base64: "string dikodkan base64",
    base64url: "string dikodkan base64url",
    json_string: "string JSON",
    e164: "nombor E.164",
    jwt: "JWT",
    template_literal: "input"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "nombor"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Input tidak sah: dijangka instanceof ${issue2.expected}, diterima ${received}`;
        }
        return `Input tidak sah: dijangka ${expected}, diterima ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Input tidak sah: dijangka ${stringifyPrimitive(issue2.values[0])}`;
        return `Pilihan tidak sah: dijangka salah satu daripada ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Terlalu besar: dijangka ${issue2.origin ?? "nilai"} ${sizing.verb} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elemen"}`;
        return `Terlalu besar: dijangka ${issue2.origin ?? "nilai"} adalah ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Terlalu kecil: dijangka ${issue2.origin} ${sizing.verb} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Terlalu kecil: dijangka ${issue2.origin} adalah ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `String tidak sah: mesti bermula dengan "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `String tidak sah: mesti berakhir dengan "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `String tidak sah: mesti mengandungi "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `String tidak sah: mesti sepadan dengan corak ${_issue.pattern}`;
        return `${FormatDictionary[_issue.format] ?? issue2.format} tidak sah`;
      }
      case "not_multiple_of":
        return `Nombor tidak sah: perlu gandaan ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Kunci tidak dikenali: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Kunci tidak sah dalam ${issue2.origin}`;
      case "invalid_union":
        return "Input tidak sah";
      case "invalid_element":
        return `Nilai tidak sah dalam ${issue2.origin}`;
      default:
        return `Input tidak sah`;
    }
  };
};
function ms_default() {
  return {
    localeError: error30()
  };
}

// node_modules/zod/v4/locales/nl.js
var error31 = () => {
  const Sizable = {
    string: { unit: "tekens", verb: "heeft" },
    file: { unit: "bytes", verb: "heeft" },
    array: { unit: "elementen", verb: "heeft" },
    set: { unit: "elementen", verb: "heeft" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "invoer",
    email: "emailadres",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO datum en tijd",
    date: "ISO datum",
    time: "ISO tijd",
    duration: "ISO duur",
    ipv4: "IPv4-adres",
    ipv6: "IPv6-adres",
    cidrv4: "IPv4-bereik",
    cidrv6: "IPv6-bereik",
    base64: "base64-gecodeerde tekst",
    base64url: "base64 URL-gecodeerde tekst",
    json_string: "JSON string",
    e164: "E.164-nummer",
    jwt: "JWT",
    template_literal: "invoer"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "getal"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Ongeldige invoer: verwacht instanceof ${issue2.expected}, ontving ${received}`;
        }
        return `Ongeldige invoer: verwacht ${expected}, ontving ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Ongeldige invoer: verwacht ${stringifyPrimitive(issue2.values[0])}`;
        return `Ongeldige optie: verwacht \xE9\xE9n van ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        const longName = issue2.origin === "date" ? "laat" : issue2.origin === "string" ? "lang" : "groot";
        if (sizing)
          return `Te ${longName}: verwacht dat ${issue2.origin ?? "waarde"} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elementen"} ${sizing.verb}`;
        return `Te ${longName}: verwacht dat ${issue2.origin ?? "waarde"} ${adj}${issue2.maximum.toString()} is`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        const shortName = issue2.origin === "date" ? "vroeg" : issue2.origin === "string" ? "kort" : "klein";
        if (sizing) {
          return `Te ${shortName}: verwacht dat ${issue2.origin} ${adj}${issue2.minimum.toString()} ${sizing.unit} ${sizing.verb}`;
        }
        return `Te ${shortName}: verwacht dat ${issue2.origin} ${adj}${issue2.minimum.toString()} is`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `Ongeldige tekst: moet met "${_issue.prefix}" beginnen`;
        }
        if (_issue.format === "ends_with")
          return `Ongeldige tekst: moet op "${_issue.suffix}" eindigen`;
        if (_issue.format === "includes")
          return `Ongeldige tekst: moet "${_issue.includes}" bevatten`;
        if (_issue.format === "regex")
          return `Ongeldige tekst: moet overeenkomen met patroon ${_issue.pattern}`;
        return `Ongeldig: ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Ongeldig getal: moet een veelvoud van ${issue2.divisor} zijn`;
      case "unrecognized_keys":
        return `Onbekende key${issue2.keys.length > 1 ? "s" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Ongeldige key in ${issue2.origin}`;
      case "invalid_union":
        return "Ongeldige invoer";
      case "invalid_element":
        return `Ongeldige waarde in ${issue2.origin}`;
      default:
        return `Ongeldige invoer`;
    }
  };
};
function nl_default() {
  return {
    localeError: error31()
  };
}

// node_modules/zod/v4/locales/no.js
var error32 = () => {
  const Sizable = {
    string: { unit: "tegn", verb: "\xE5 ha" },
    file: { unit: "bytes", verb: "\xE5 ha" },
    array: { unit: "elementer", verb: "\xE5 inneholde" },
    set: { unit: "elementer", verb: "\xE5 inneholde" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "input",
    email: "e-postadresse",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO dato- og klokkeslett",
    date: "ISO-dato",
    time: "ISO-klokkeslett",
    duration: "ISO-varighet",
    ipv4: "IPv4-omr\xE5de",
    ipv6: "IPv6-omr\xE5de",
    cidrv4: "IPv4-spekter",
    cidrv6: "IPv6-spekter",
    base64: "base64-enkodet streng",
    base64url: "base64url-enkodet streng",
    json_string: "JSON-streng",
    e164: "E.164-nummer",
    jwt: "JWT",
    template_literal: "input"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "tall",
    array: "liste"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Ugyldig input: forventet instanceof ${issue2.expected}, fikk ${received}`;
        }
        return `Ugyldig input: forventet ${expected}, fikk ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Ugyldig verdi: forventet ${stringifyPrimitive(issue2.values[0])}`;
        return `Ugyldig valg: forventet en av ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `For stor(t): forventet ${issue2.origin ?? "value"} til \xE5 ha ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elementer"}`;
        return `For stor(t): forventet ${issue2.origin ?? "value"} til \xE5 ha ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `For lite(n): forventet ${issue2.origin} til \xE5 ha ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `For lite(n): forventet ${issue2.origin} til \xE5 ha ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Ugyldig streng: m\xE5 starte med "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Ugyldig streng: m\xE5 ende med "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Ugyldig streng: m\xE5 inneholde "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Ugyldig streng: m\xE5 matche m\xF8nsteret ${_issue.pattern}`;
        return `Ugyldig ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Ugyldig tall: m\xE5 v\xE6re et multiplum av ${issue2.divisor}`;
      case "unrecognized_keys":
        return `${issue2.keys.length > 1 ? "Ukjente n\xF8kler" : "Ukjent n\xF8kkel"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Ugyldig n\xF8kkel i ${issue2.origin}`;
      case "invalid_union":
        return "Ugyldig input";
      case "invalid_element":
        return `Ugyldig verdi i ${issue2.origin}`;
      default:
        return `Ugyldig input`;
    }
  };
};
function no_default() {
  return {
    localeError: error32()
  };
}

// node_modules/zod/v4/locales/ota.js
var error33 = () => {
  const Sizable = {
    string: { unit: "harf", verb: "olmal\u0131d\u0131r" },
    file: { unit: "bayt", verb: "olmal\u0131d\u0131r" },
    array: { unit: "unsur", verb: "olmal\u0131d\u0131r" },
    set: { unit: "unsur", verb: "olmal\u0131d\u0131r" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "giren",
    email: "epostag\xE2h",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO heng\xE2m\u0131",
    date: "ISO tarihi",
    time: "ISO zaman\u0131",
    duration: "ISO m\xFCddeti",
    ipv4: "IPv4 ni\u015F\xE2n\u0131",
    ipv6: "IPv6 ni\u015F\xE2n\u0131",
    cidrv4: "IPv4 menzili",
    cidrv6: "IPv6 menzili",
    base64: "base64-\u015Fifreli metin",
    base64url: "base64url-\u015Fifreli metin",
    json_string: "JSON metin",
    e164: "E.164 say\u0131s\u0131",
    jwt: "JWT",
    template_literal: "giren"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "numara",
    array: "saf",
    null: "gayb"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `F\xE2sit giren: umulan instanceof ${issue2.expected}, al\u0131nan ${received}`;
        }
        return `F\xE2sit giren: umulan ${expected}, al\u0131nan ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `F\xE2sit giren: umulan ${stringifyPrimitive(issue2.values[0])}`;
        return `F\xE2sit tercih: m\xFBteberler ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Fazla b\xFCy\xFCk: ${issue2.origin ?? "value"}, ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elements"} sahip olmal\u0131yd\u0131.`;
        return `Fazla b\xFCy\xFCk: ${issue2.origin ?? "value"}, ${adj}${issue2.maximum.toString()} olmal\u0131yd\u0131.`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Fazla k\xFC\xE7\xFCk: ${issue2.origin}, ${adj}${issue2.minimum.toString()} ${sizing.unit} sahip olmal\u0131yd\u0131.`;
        }
        return `Fazla k\xFC\xE7\xFCk: ${issue2.origin}, ${adj}${issue2.minimum.toString()} olmal\u0131yd\u0131.`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `F\xE2sit metin: "${_issue.prefix}" ile ba\u015Flamal\u0131.`;
        if (_issue.format === "ends_with")
          return `F\xE2sit metin: "${_issue.suffix}" ile bitmeli.`;
        if (_issue.format === "includes")
          return `F\xE2sit metin: "${_issue.includes}" ihtiv\xE2 etmeli.`;
        if (_issue.format === "regex")
          return `F\xE2sit metin: ${_issue.pattern} nak\u015F\u0131na uymal\u0131.`;
        return `F\xE2sit ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `F\xE2sit say\u0131: ${issue2.divisor} kat\u0131 olmal\u0131yd\u0131.`;
      case "unrecognized_keys":
        return `Tan\u0131nmayan anahtar ${issue2.keys.length > 1 ? "s" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `${issue2.origin} i\xE7in tan\u0131nmayan anahtar var.`;
      case "invalid_union":
        return "Giren tan\u0131namad\u0131.";
      case "invalid_element":
        return `${issue2.origin} i\xE7in tan\u0131nmayan k\u0131ymet var.`;
      default:
        return `K\u0131ymet tan\u0131namad\u0131.`;
    }
  };
};
function ota_default() {
  return {
    localeError: error33()
  };
}

// node_modules/zod/v4/locales/ps.js
var error34 = () => {
  const Sizable = {
    string: { unit: "\u062A\u0648\u06A9\u064A", verb: "\u0648\u0644\u0631\u064A" },
    file: { unit: "\u0628\u0627\u06CC\u067C\u0633", verb: "\u0648\u0644\u0631\u064A" },
    array: { unit: "\u062A\u0648\u06A9\u064A", verb: "\u0648\u0644\u0631\u064A" },
    set: { unit: "\u062A\u0648\u06A9\u064A", verb: "\u0648\u0644\u0631\u064A" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0648\u0631\u0648\u062F\u064A",
    email: "\u0628\u0631\u06CC\u069A\u0646\u0627\u0644\u06CC\u06A9",
    url: "\u06CC\u0648 \u0622\u0631 \u0627\u0644",
    emoji: "\u0627\u06CC\u0645\u0648\u062C\u064A",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "\u0646\u06CC\u067C\u0647 \u0627\u0648 \u0648\u062E\u062A",
    date: "\u0646\u06D0\u067C\u0647",
    time: "\u0648\u062E\u062A",
    duration: "\u0645\u0648\u062F\u0647",
    ipv4: "\u062F IPv4 \u067E\u062A\u0647",
    ipv6: "\u062F IPv6 \u067E\u062A\u0647",
    cidrv4: "\u062F IPv4 \u0633\u0627\u062D\u0647",
    cidrv6: "\u062F IPv6 \u0633\u0627\u062D\u0647",
    base64: "base64-encoded \u0645\u062A\u0646",
    base64url: "base64url-encoded \u0645\u062A\u0646",
    json_string: "JSON \u0645\u062A\u0646",
    e164: "\u062F E.164 \u0634\u0645\u06D0\u0631\u0647",
    jwt: "JWT",
    template_literal: "\u0648\u0631\u0648\u062F\u064A"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0639\u062F\u062F",
    array: "\u0627\u0631\u06D0"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u0646\u0627\u0633\u0645 \u0648\u0631\u0648\u062F\u064A: \u0628\u0627\u06CC\u062F instanceof ${issue2.expected} \u0648\u0627\u06CC, \u0645\u06AB\u0631 ${received} \u062A\u0631\u0644\u0627\u0633\u0647 \u0634\u0648`;
        }
        return `\u0646\u0627\u0633\u0645 \u0648\u0631\u0648\u062F\u064A: \u0628\u0627\u06CC\u062F ${expected} \u0648\u0627\u06CC, \u0645\u06AB\u0631 ${received} \u062A\u0631\u0644\u0627\u0633\u0647 \u0634\u0648`;
      }
      case "invalid_value":
        if (issue2.values.length === 1) {
          return `\u0646\u0627\u0633\u0645 \u0648\u0631\u0648\u062F\u064A: \u0628\u0627\u06CC\u062F ${stringifyPrimitive(issue2.values[0])} \u0648\u0627\u06CC`;
        }
        return `\u0646\u0627\u0633\u0645 \u0627\u0646\u062A\u062E\u0627\u0628: \u0628\u0627\u06CC\u062F \u06CC\u0648 \u0644\u0647 ${joinValues(issue2.values, "|")} \u0685\u062E\u0647 \u0648\u0627\u06CC`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u0689\u06CC\u0631 \u0644\u0648\u06CC: ${issue2.origin ?? "\u0627\u0631\u0632\u069A\u062A"} \u0628\u0627\u06CC\u062F ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u0639\u0646\u0635\u0631\u0648\u0646\u0647"} \u0648\u0644\u0631\u064A`;
        }
        return `\u0689\u06CC\u0631 \u0644\u0648\u06CC: ${issue2.origin ?? "\u0627\u0631\u0632\u069A\u062A"} \u0628\u0627\u06CC\u062F ${adj}${issue2.maximum.toString()} \u0648\u064A`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u0689\u06CC\u0631 \u06A9\u0648\u0686\u0646\u06CC: ${issue2.origin} \u0628\u0627\u06CC\u062F ${adj}${issue2.minimum.toString()} ${sizing.unit} \u0648\u0644\u0631\u064A`;
        }
        return `\u0689\u06CC\u0631 \u06A9\u0648\u0686\u0646\u06CC: ${issue2.origin} \u0628\u0627\u06CC\u062F ${adj}${issue2.minimum.toString()} \u0648\u064A`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u0646\u0627\u0633\u0645 \u0645\u062A\u0646: \u0628\u0627\u06CC\u062F \u062F "${_issue.prefix}" \u0633\u0631\u0647 \u067E\u06CC\u0644 \u0634\u064A`;
        }
        if (_issue.format === "ends_with") {
          return `\u0646\u0627\u0633\u0645 \u0645\u062A\u0646: \u0628\u0627\u06CC\u062F \u062F "${_issue.suffix}" \u0633\u0631\u0647 \u067E\u0627\u06CC \u062A\u0647 \u0648\u0631\u0633\u064A\u0696\u064A`;
        }
        if (_issue.format === "includes") {
          return `\u0646\u0627\u0633\u0645 \u0645\u062A\u0646: \u0628\u0627\u06CC\u062F "${_issue.includes}" \u0648\u0644\u0631\u064A`;
        }
        if (_issue.format === "regex") {
          return `\u0646\u0627\u0633\u0645 \u0645\u062A\u0646: \u0628\u0627\u06CC\u062F \u062F ${_issue.pattern} \u0633\u0631\u0647 \u0645\u0637\u0627\u0628\u0642\u062A \u0648\u0644\u0631\u064A`;
        }
        return `${FormatDictionary[_issue.format] ?? issue2.format} \u0646\u0627\u0633\u0645 \u062F\u06CC`;
      }
      case "not_multiple_of":
        return `\u0646\u0627\u0633\u0645 \u0639\u062F\u062F: \u0628\u0627\u06CC\u062F \u062F ${issue2.divisor} \u0645\u0636\u0631\u0628 \u0648\u064A`;
      case "unrecognized_keys":
        return `\u0646\u0627\u0633\u0645 ${issue2.keys.length > 1 ? "\u06A9\u0644\u06CC\u0689\u0648\u0646\u0647" : "\u06A9\u0644\u06CC\u0689"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u0646\u0627\u0633\u0645 \u06A9\u0644\u06CC\u0689 \u067E\u0647 ${issue2.origin} \u06A9\u06D0`;
      case "invalid_union":
        return `\u0646\u0627\u0633\u0645\u0647 \u0648\u0631\u0648\u062F\u064A`;
      case "invalid_element":
        return `\u0646\u0627\u0633\u0645 \u0639\u0646\u0635\u0631 \u067E\u0647 ${issue2.origin} \u06A9\u06D0`;
      default:
        return `\u0646\u0627\u0633\u0645\u0647 \u0648\u0631\u0648\u062F\u064A`;
    }
  };
};
function ps_default() {
  return {
    localeError: error34()
  };
}

// node_modules/zod/v4/locales/pl.js
var error35 = () => {
  const Sizable = {
    string: { unit: "znak\xF3w", verb: "mie\u0107" },
    file: { unit: "bajt\xF3w", verb: "mie\u0107" },
    array: { unit: "element\xF3w", verb: "mie\u0107" },
    set: { unit: "element\xF3w", verb: "mie\u0107" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "wyra\u017Cenie",
    email: "adres email",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "data i godzina w formacie ISO",
    date: "data w formacie ISO",
    time: "godzina w formacie ISO",
    duration: "czas trwania ISO",
    ipv4: "adres IPv4",
    ipv6: "adres IPv6",
    cidrv4: "zakres IPv4",
    cidrv6: "zakres IPv6",
    base64: "ci\u0105g znak\xF3w zakodowany w formacie base64",
    base64url: "ci\u0105g znak\xF3w zakodowany w formacie base64url",
    json_string: "ci\u0105g znak\xF3w w formacie JSON",
    e164: "liczba E.164",
    jwt: "JWT",
    template_literal: "wej\u015Bcie"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "liczba",
    array: "tablica"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Nieprawid\u0142owe dane wej\u015Bciowe: oczekiwano instanceof ${issue2.expected}, otrzymano ${received}`;
        }
        return `Nieprawid\u0142owe dane wej\u015Bciowe: oczekiwano ${expected}, otrzymano ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Nieprawid\u0142owe dane wej\u015Bciowe: oczekiwano ${stringifyPrimitive(issue2.values[0])}`;
        return `Nieprawid\u0142owa opcja: oczekiwano jednej z warto\u015Bci ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Za du\u017Ca warto\u015B\u0107: oczekiwano, \u017Ce ${issue2.origin ?? "warto\u015B\u0107"} b\u0119dzie mie\u0107 ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "element\xF3w"}`;
        }
        return `Zbyt du\u017C(y/a/e): oczekiwano, \u017Ce ${issue2.origin ?? "warto\u015B\u0107"} b\u0119dzie wynosi\u0107 ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Za ma\u0142a warto\u015B\u0107: oczekiwano, \u017Ce ${issue2.origin ?? "warto\u015B\u0107"} b\u0119dzie mie\u0107 ${adj}${issue2.minimum.toString()} ${sizing.unit ?? "element\xF3w"}`;
        }
        return `Zbyt ma\u0142(y/a/e): oczekiwano, \u017Ce ${issue2.origin ?? "warto\u015B\u0107"} b\u0119dzie wynosi\u0107 ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Nieprawid\u0142owy ci\u0105g znak\xF3w: musi zaczyna\u0107 si\u0119 od "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Nieprawid\u0142owy ci\u0105g znak\xF3w: musi ko\u0144czy\u0107 si\u0119 na "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Nieprawid\u0142owy ci\u0105g znak\xF3w: musi zawiera\u0107 "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Nieprawid\u0142owy ci\u0105g znak\xF3w: musi odpowiada\u0107 wzorcowi ${_issue.pattern}`;
        return `Nieprawid\u0142ow(y/a/e) ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Nieprawid\u0142owa liczba: musi by\u0107 wielokrotno\u015Bci\u0105 ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Nierozpoznane klucze${issue2.keys.length > 1 ? "s" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Nieprawid\u0142owy klucz w ${issue2.origin}`;
      case "invalid_union":
        return "Nieprawid\u0142owe dane wej\u015Bciowe";
      case "invalid_element":
        return `Nieprawid\u0142owa warto\u015B\u0107 w ${issue2.origin}`;
      default:
        return `Nieprawid\u0142owe dane wej\u015Bciowe`;
    }
  };
};
function pl_default() {
  return {
    localeError: error35()
  };
}

// node_modules/zod/v4/locales/pt.js
var error36 = () => {
  const Sizable = {
    string: { unit: "caracteres", verb: "ter" },
    file: { unit: "bytes", verb: "ter" },
    array: { unit: "itens", verb: "ter" },
    set: { unit: "itens", verb: "ter" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "padr\xE3o",
    email: "endere\xE7o de e-mail",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "data e hora ISO",
    date: "data ISO",
    time: "hora ISO",
    duration: "dura\xE7\xE3o ISO",
    ipv4: "endere\xE7o IPv4",
    ipv6: "endere\xE7o IPv6",
    cidrv4: "faixa de IPv4",
    cidrv6: "faixa de IPv6",
    base64: "texto codificado em base64",
    base64url: "URL codificada em base64",
    json_string: "texto JSON",
    e164: "n\xFAmero E.164",
    jwt: "JWT",
    template_literal: "entrada"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "n\xFAmero",
    null: "nulo"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Tipo inv\xE1lido: esperado instanceof ${issue2.expected}, recebido ${received}`;
        }
        return `Tipo inv\xE1lido: esperado ${expected}, recebido ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Entrada inv\xE1lida: esperado ${stringifyPrimitive(issue2.values[0])}`;
        return `Op\xE7\xE3o inv\xE1lida: esperada uma das ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Muito grande: esperado que ${issue2.origin ?? "valor"} tivesse ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elementos"}`;
        return `Muito grande: esperado que ${issue2.origin ?? "valor"} fosse ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Muito pequeno: esperado que ${issue2.origin} tivesse ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Muito pequeno: esperado que ${issue2.origin} fosse ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Texto inv\xE1lido: deve come\xE7ar com "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Texto inv\xE1lido: deve terminar com "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Texto inv\xE1lido: deve incluir "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Texto inv\xE1lido: deve corresponder ao padr\xE3o ${_issue.pattern}`;
        return `${FormatDictionary[_issue.format] ?? issue2.format} inv\xE1lido`;
      }
      case "not_multiple_of":
        return `N\xFAmero inv\xE1lido: deve ser m\xFAltiplo de ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Chave${issue2.keys.length > 1 ? "s" : ""} desconhecida${issue2.keys.length > 1 ? "s" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Chave inv\xE1lida em ${issue2.origin}`;
      case "invalid_union":
        return "Entrada inv\xE1lida";
      case "invalid_element":
        return `Valor inv\xE1lido em ${issue2.origin}`;
      default:
        return `Campo inv\xE1lido`;
    }
  };
};
function pt_default() {
  return {
    localeError: error36()
  };
}

// node_modules/zod/v4/locales/ro.js
var error37 = () => {
  const Sizable = {
    string: { unit: "caractere", verb: "s\u0103 aib\u0103" },
    file: { unit: "octe\u021Bi", verb: "s\u0103 aib\u0103" },
    array: { unit: "elemente", verb: "s\u0103 aib\u0103" },
    set: { unit: "elemente", verb: "s\u0103 aib\u0103" },
    map: { unit: "intr\u0103ri", verb: "s\u0103 aib\u0103" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "intrare",
    email: "adres\u0103 de email",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "dat\u0103 \u0219i or\u0103 ISO",
    date: "dat\u0103 ISO",
    time: "or\u0103 ISO",
    duration: "durat\u0103 ISO",
    ipv4: "adres\u0103 IPv4",
    ipv6: "adres\u0103 IPv6",
    mac: "adres\u0103 MAC",
    cidrv4: "interval IPv4",
    cidrv6: "interval IPv6",
    base64: "\u0219ir codat base64",
    base64url: "\u0219ir codat base64url",
    json_string: "\u0219ir JSON",
    e164: "num\u0103r E.164",
    jwt: "JWT",
    template_literal: "intrare"
  };
  const TypeDictionary = {
    nan: "NaN",
    string: "\u0219ir",
    number: "num\u0103r",
    boolean: "boolean",
    function: "func\u021Bie",
    array: "matrice",
    object: "obiect",
    undefined: "nedefinit",
    symbol: "simbol",
    bigint: "num\u0103r mare",
    void: "void",
    never: "never",
    map: "hart\u0103",
    set: "set"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        return `Intrare invalid\u0103: a\u0219teptat ${expected}, primit ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Intrare invalid\u0103: a\u0219teptat ${stringifyPrimitive(issue2.values[0])}`;
        return `Op\u021Biune invalid\u0103: a\u0219teptat una dintre ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Prea mare: a\u0219teptat ca ${issue2.origin ?? "valoarea"} ${sizing.verb} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elemente"}`;
        return `Prea mare: a\u0219teptat ca ${issue2.origin ?? "valoarea"} s\u0103 fie ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Prea mic: a\u0219teptat ca ${issue2.origin} ${sizing.verb} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Prea mic: a\u0219teptat ca ${issue2.origin} s\u0103 fie ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u0218ir invalid: trebuie s\u0103 \xEEnceap\u0103 cu "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `\u0218ir invalid: trebuie s\u0103 se termine cu "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u0218ir invalid: trebuie s\u0103 includ\u0103 "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u0218ir invalid: trebuie s\u0103 se potriveasc\u0103 cu modelul ${_issue.pattern}`;
        return `Format invalid: ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Num\u0103r invalid: trebuie s\u0103 fie multiplu de ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Chei nerecunoscute: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Cheie invalid\u0103 \xEEn ${issue2.origin}`;
      case "invalid_union":
        return "Intrare invalid\u0103";
      case "invalid_element":
        return `Valoare invalid\u0103 \xEEn ${issue2.origin}`;
      default:
        return `Intrare invalid\u0103`;
    }
  };
};
function ro_default() {
  return {
    localeError: error37()
  };
}

// node_modules/zod/v4/locales/ru.js
function getRussianPlural(count, one, few, many) {
  const absCount = Math.abs(count);
  const lastDigit = absCount % 10;
  const lastTwoDigits = absCount % 100;
  if (lastTwoDigits >= 11 && lastTwoDigits <= 19) {
    return many;
  }
  if (lastDigit === 1) {
    return one;
  }
  if (lastDigit >= 2 && lastDigit <= 4) {
    return few;
  }
  return many;
}
var error38 = () => {
  const Sizable = {
    string: {
      unit: {
        one: "\u0441\u0438\u043C\u0432\u043E\u043B",
        few: "\u0441\u0438\u043C\u0432\u043E\u043B\u0430",
        many: "\u0441\u0438\u043C\u0432\u043E\u043B\u043E\u0432"
      },
      verb: "\u0438\u043C\u0435\u0442\u044C"
    },
    file: {
      unit: {
        one: "\u0431\u0430\u0439\u0442",
        few: "\u0431\u0430\u0439\u0442\u0430",
        many: "\u0431\u0430\u0439\u0442"
      },
      verb: "\u0438\u043C\u0435\u0442\u044C"
    },
    array: {
      unit: {
        one: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442",
        few: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442\u0430",
        many: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442\u043E\u0432"
      },
      verb: "\u0438\u043C\u0435\u0442\u044C"
    },
    set: {
      unit: {
        one: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442",
        few: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442\u0430",
        many: "\u044D\u043B\u0435\u043C\u0435\u043D\u0442\u043E\u0432"
      },
      verb: "\u0438\u043C\u0435\u0442\u044C"
    }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0432\u0432\u043E\u0434",
    email: "email \u0430\u0434\u0440\u0435\u0441",
    url: "URL",
    emoji: "\u044D\u043C\u043E\u0434\u0437\u0438",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO \u0434\u0430\u0442\u0430 \u0438 \u0432\u0440\u0435\u043C\u044F",
    date: "ISO \u0434\u0430\u0442\u0430",
    time: "ISO \u0432\u0440\u0435\u043C\u044F",
    duration: "ISO \u0434\u043B\u0438\u0442\u0435\u043B\u044C\u043D\u043E\u0441\u0442\u044C",
    ipv4: "IPv4 \u0430\u0434\u0440\u0435\u0441",
    ipv6: "IPv6 \u0430\u0434\u0440\u0435\u0441",
    cidrv4: "IPv4 \u0434\u0438\u0430\u043F\u0430\u0437\u043E\u043D",
    cidrv6: "IPv6 \u0434\u0438\u0430\u043F\u0430\u0437\u043E\u043D",
    base64: "\u0441\u0442\u0440\u043E\u043A\u0430 \u0432 \u0444\u043E\u0440\u043C\u0430\u0442\u0435 base64",
    base64url: "\u0441\u0442\u0440\u043E\u043A\u0430 \u0432 \u0444\u043E\u0440\u043C\u0430\u0442\u0435 base64url",
    json_string: "JSON \u0441\u0442\u0440\u043E\u043A\u0430",
    e164: "\u043D\u043E\u043C\u0435\u0440 E.164",
    jwt: "JWT",
    template_literal: "\u0432\u0432\u043E\u0434"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0447\u0438\u0441\u043B\u043E",
    array: "\u043C\u0430\u0441\u0441\u0438\u0432"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u041D\u0435\u0432\u0435\u0440\u043D\u044B\u0439 \u0432\u0432\u043E\u0434: \u043E\u0436\u0438\u0434\u0430\u043B\u043E\u0441\u044C instanceof ${issue2.expected}, \u043F\u043E\u043B\u0443\u0447\u0435\u043D\u043E ${received}`;
        }
        return `\u041D\u0435\u0432\u0435\u0440\u043D\u044B\u0439 \u0432\u0432\u043E\u0434: \u043E\u0436\u0438\u0434\u0430\u043B\u043E\u0441\u044C ${expected}, \u043F\u043E\u043B\u0443\u0447\u0435\u043D\u043E ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u041D\u0435\u0432\u0435\u0440\u043D\u044B\u0439 \u0432\u0432\u043E\u0434: \u043E\u0436\u0438\u0434\u0430\u043B\u043E\u0441\u044C ${stringifyPrimitive(issue2.values[0])}`;
        return `\u041D\u0435\u0432\u0435\u0440\u043D\u044B\u0439 \u0432\u0430\u0440\u0438\u0430\u043D\u0442: \u043E\u0436\u0438\u0434\u0430\u043B\u043E\u0441\u044C \u043E\u0434\u043D\u043E \u0438\u0437 ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          const maxValue = Number(issue2.maximum);
          const unit = getRussianPlural(maxValue, sizing.unit.one, sizing.unit.few, sizing.unit.many);
          return `\u0421\u043B\u0438\u0448\u043A\u043E\u043C \u0431\u043E\u043B\u044C\u0448\u043E\u0435 \u0437\u043D\u0430\u0447\u0435\u043D\u0438\u0435: \u043E\u0436\u0438\u0434\u0430\u043B\u043E\u0441\u044C, \u0447\u0442\u043E ${issue2.origin ?? "\u0437\u043D\u0430\u0447\u0435\u043D\u0438\u0435"} \u0431\u0443\u0434\u0435\u0442 \u0438\u043C\u0435\u0442\u044C ${adj}${issue2.maximum.toString()} ${unit}`;
        }
        return `\u0421\u043B\u0438\u0448\u043A\u043E\u043C \u0431\u043E\u043B\u044C\u0448\u043E\u0435 \u0437\u043D\u0430\u0447\u0435\u043D\u0438\u0435: \u043E\u0436\u0438\u0434\u0430\u043B\u043E\u0441\u044C, \u0447\u0442\u043E ${issue2.origin ?? "\u0437\u043D\u0430\u0447\u0435\u043D\u0438\u0435"} \u0431\u0443\u0434\u0435\u0442 ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          const minValue = Number(issue2.minimum);
          const unit = getRussianPlural(minValue, sizing.unit.one, sizing.unit.few, sizing.unit.many);
          return `\u0421\u043B\u0438\u0448\u043A\u043E\u043C \u043C\u0430\u043B\u0435\u043D\u044C\u043A\u043E\u0435 \u0437\u043D\u0430\u0447\u0435\u043D\u0438\u0435: \u043E\u0436\u0438\u0434\u0430\u043B\u043E\u0441\u044C, \u0447\u0442\u043E ${issue2.origin} \u0431\u0443\u0434\u0435\u0442 \u0438\u043C\u0435\u0442\u044C ${adj}${issue2.minimum.toString()} ${unit}`;
        }
        return `\u0421\u043B\u0438\u0448\u043A\u043E\u043C \u043C\u0430\u043B\u0435\u043D\u044C\u043A\u043E\u0435 \u0437\u043D\u0430\u0447\u0435\u043D\u0438\u0435: \u043E\u0436\u0438\u0434\u0430\u043B\u043E\u0441\u044C, \u0447\u0442\u043E ${issue2.origin} \u0431\u0443\u0434\u0435\u0442 ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u041D\u0435\u0432\u0435\u0440\u043D\u0430\u044F \u0441\u0442\u0440\u043E\u043A\u0430: \u0434\u043E\u043B\u0436\u043D\u0430 \u043D\u0430\u0447\u0438\u043D\u0430\u0442\u044C\u0441\u044F \u0441 "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `\u041D\u0435\u0432\u0435\u0440\u043D\u0430\u044F \u0441\u0442\u0440\u043E\u043A\u0430: \u0434\u043E\u043B\u0436\u043D\u0430 \u0437\u0430\u043A\u0430\u043D\u0447\u0438\u0432\u0430\u0442\u044C\u0441\u044F \u043D\u0430 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u041D\u0435\u0432\u0435\u0440\u043D\u0430\u044F \u0441\u0442\u0440\u043E\u043A\u0430: \u0434\u043E\u043B\u0436\u043D\u0430 \u0441\u043E\u0434\u0435\u0440\u0436\u0430\u0442\u044C "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u041D\u0435\u0432\u0435\u0440\u043D\u0430\u044F \u0441\u0442\u0440\u043E\u043A\u0430: \u0434\u043E\u043B\u0436\u043D\u0430 \u0441\u043E\u043E\u0442\u0432\u0435\u0442\u0441\u0442\u0432\u043E\u0432\u0430\u0442\u044C \u0448\u0430\u0431\u043B\u043E\u043D\u0443 ${_issue.pattern}`;
        return `\u041D\u0435\u0432\u0435\u0440\u043D\u044B\u0439 ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u041D\u0435\u0432\u0435\u0440\u043D\u043E\u0435 \u0447\u0438\u0441\u043B\u043E: \u0434\u043E\u043B\u0436\u043D\u043E \u0431\u044B\u0442\u044C \u043A\u0440\u0430\u0442\u043D\u044B\u043C ${issue2.divisor}`;
      case "unrecognized_keys":
        return `\u041D\u0435\u0440\u0430\u0441\u043F\u043E\u0437\u043D\u0430\u043D\u043D${issue2.keys.length > 1 ? "\u044B\u0435" : "\u044B\u0439"} \u043A\u043B\u044E\u0447${issue2.keys.length > 1 ? "\u0438" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u041D\u0435\u0432\u0435\u0440\u043D\u044B\u0439 \u043A\u043B\u044E\u0447 \u0432 ${issue2.origin}`;
      case "invalid_union":
        return "\u041D\u0435\u0432\u0435\u0440\u043D\u044B\u0435 \u0432\u0445\u043E\u0434\u043D\u044B\u0435 \u0434\u0430\u043D\u043D\u044B\u0435";
      case "invalid_element":
        return `\u041D\u0435\u0432\u0435\u0440\u043D\u043E\u0435 \u0437\u043D\u0430\u0447\u0435\u043D\u0438\u0435 \u0432 ${issue2.origin}`;
      default:
        return `\u041D\u0435\u0432\u0435\u0440\u043D\u044B\u0435 \u0432\u0445\u043E\u0434\u043D\u044B\u0435 \u0434\u0430\u043D\u043D\u044B\u0435`;
    }
  };
};
function ru_default() {
  return {
    localeError: error38()
  };
}

// node_modules/zod/v4/locales/sl.js
var error39 = () => {
  const Sizable = {
    string: { unit: "znakov", verb: "imeti" },
    file: { unit: "bajtov", verb: "imeti" },
    array: { unit: "elementov", verb: "imeti" },
    set: { unit: "elementov", verb: "imeti" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "vnos",
    email: "e-po\u0161tni naslov",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO datum in \u010Das",
    date: "ISO datum",
    time: "ISO \u010Das",
    duration: "ISO trajanje",
    ipv4: "IPv4 naslov",
    ipv6: "IPv6 naslov",
    cidrv4: "obseg IPv4",
    cidrv6: "obseg IPv6",
    base64: "base64 kodiran niz",
    base64url: "base64url kodiran niz",
    json_string: "JSON niz",
    e164: "E.164 \u0161tevilka",
    jwt: "JWT",
    template_literal: "vnos"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0161tevilo",
    array: "tabela"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Neveljaven vnos: pri\u010Dakovano instanceof ${issue2.expected}, prejeto ${received}`;
        }
        return `Neveljaven vnos: pri\u010Dakovano ${expected}, prejeto ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Neveljaven vnos: pri\u010Dakovano ${stringifyPrimitive(issue2.values[0])}`;
        return `Neveljavna mo\u017Enost: pri\u010Dakovano eno izmed ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Preveliko: pri\u010Dakovano, da bo ${issue2.origin ?? "vrednost"} imelo ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "elementov"}`;
        return `Preveliko: pri\u010Dakovano, da bo ${issue2.origin ?? "vrednost"} ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Premajhno: pri\u010Dakovano, da bo ${issue2.origin} imelo ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Premajhno: pri\u010Dakovano, da bo ${issue2.origin} ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `Neveljaven niz: mora se za\u010Deti z "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `Neveljaven niz: mora se kon\u010Dati z "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Neveljaven niz: mora vsebovati "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Neveljaven niz: mora ustrezati vzorcu ${_issue.pattern}`;
        return `Neveljaven ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Neveljavno \u0161tevilo: mora biti ve\u010Dkratnik ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Neprepoznan${issue2.keys.length > 1 ? "i klju\u010Di" : " klju\u010D"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Neveljaven klju\u010D v ${issue2.origin}`;
      case "invalid_union":
        return "Neveljaven vnos";
      case "invalid_element":
        return `Neveljavna vrednost v ${issue2.origin}`;
      default:
        return "Neveljaven vnos";
    }
  };
};
function sl_default() {
  return {
    localeError: error39()
  };
}

// node_modules/zod/v4/locales/sv.js
var error40 = () => {
  const Sizable = {
    string: { unit: "tecken", verb: "att ha" },
    file: { unit: "bytes", verb: "att ha" },
    array: { unit: "objekt", verb: "att inneh\xE5lla" },
    set: { unit: "objekt", verb: "att inneh\xE5lla" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "regulj\xE4rt uttryck",
    email: "e-postadress",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO-datum och tid",
    date: "ISO-datum",
    time: "ISO-tid",
    duration: "ISO-varaktighet",
    ipv4: "IPv4-intervall",
    ipv6: "IPv6-intervall",
    cidrv4: "IPv4-spektrum",
    cidrv6: "IPv6-spektrum",
    base64: "base64-kodad str\xE4ng",
    base64url: "base64url-kodad str\xE4ng",
    json_string: "JSON-str\xE4ng",
    e164: "E.164-nummer",
    jwt: "JWT",
    template_literal: "mall-literal"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "antal",
    array: "lista"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Ogiltig inmatning: f\xF6rv\xE4ntat instanceof ${issue2.expected}, fick ${received}`;
        }
        return `Ogiltig inmatning: f\xF6rv\xE4ntat ${expected}, fick ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Ogiltig inmatning: f\xF6rv\xE4ntat ${stringifyPrimitive(issue2.values[0])}`;
        return `Ogiltigt val: f\xF6rv\xE4ntade en av ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `F\xF6r stor(t): f\xF6rv\xE4ntade ${issue2.origin ?? "v\xE4rdet"} att ha ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "element"}`;
        }
        return `F\xF6r stor(t): f\xF6rv\xE4ntat ${issue2.origin ?? "v\xE4rdet"} att ha ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `F\xF6r lite(t): f\xF6rv\xE4ntade ${issue2.origin ?? "v\xE4rdet"} att ha ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `F\xF6r lite(t): f\xF6rv\xE4ntade ${issue2.origin ?? "v\xE4rdet"} att ha ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `Ogiltig str\xE4ng: m\xE5ste b\xF6rja med "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `Ogiltig str\xE4ng: m\xE5ste sluta med "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Ogiltig str\xE4ng: m\xE5ste inneh\xE5lla "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Ogiltig str\xE4ng: m\xE5ste matcha m\xF6nstret "${_issue.pattern}"`;
        return `Ogiltig(t) ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Ogiltigt tal: m\xE5ste vara en multipel av ${issue2.divisor}`;
      case "unrecognized_keys":
        return `${issue2.keys.length > 1 ? "Ok\xE4nda nycklar" : "Ok\xE4nd nyckel"}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Ogiltig nyckel i ${issue2.origin ?? "v\xE4rdet"}`;
      case "invalid_union":
        return "Ogiltig input";
      case "invalid_element":
        return `Ogiltigt v\xE4rde i ${issue2.origin ?? "v\xE4rdet"}`;
      default:
        return `Ogiltig input`;
    }
  };
};
function sv_default() {
  return {
    localeError: error40()
  };
}

// node_modules/zod/v4/locales/ta.js
var error41 = () => {
  const Sizable = {
    string: { unit: "\u0B8E\u0BB4\u0BC1\u0BA4\u0BCD\u0BA4\u0BC1\u0B95\u0BCD\u0B95\u0BB3\u0BCD", verb: "\u0B95\u0BCA\u0BA3\u0BCD\u0B9F\u0BBF\u0BB0\u0BC1\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD" },
    file: { unit: "\u0BAA\u0BC8\u0B9F\u0BCD\u0B9F\u0BC1\u0B95\u0BB3\u0BCD", verb: "\u0B95\u0BCA\u0BA3\u0BCD\u0B9F\u0BBF\u0BB0\u0BC1\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD" },
    array: { unit: "\u0B89\u0BB1\u0BC1\u0BAA\u0BCD\u0BAA\u0BC1\u0B95\u0BB3\u0BCD", verb: "\u0B95\u0BCA\u0BA3\u0BCD\u0B9F\u0BBF\u0BB0\u0BC1\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD" },
    set: { unit: "\u0B89\u0BB1\u0BC1\u0BAA\u0BCD\u0BAA\u0BC1\u0B95\u0BB3\u0BCD", verb: "\u0B95\u0BCA\u0BA3\u0BCD\u0B9F\u0BBF\u0BB0\u0BC1\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0B89\u0BB3\u0BCD\u0BB3\u0BC0\u0B9F\u0BC1",
    email: "\u0BAE\u0BBF\u0BA9\u0BCD\u0BA9\u0B9E\u0BCD\u0B9A\u0BB2\u0BCD \u0BAE\u0BC1\u0B95\u0BB5\u0BB0\u0BBF",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO \u0BA4\u0BC7\u0BA4\u0BBF \u0BA8\u0BC7\u0BB0\u0BAE\u0BCD",
    date: "ISO \u0BA4\u0BC7\u0BA4\u0BBF",
    time: "ISO \u0BA8\u0BC7\u0BB0\u0BAE\u0BCD",
    duration: "ISO \u0B95\u0BBE\u0BB2 \u0B85\u0BB3\u0BB5\u0BC1",
    ipv4: "IPv4 \u0BAE\u0BC1\u0B95\u0BB5\u0BB0\u0BBF",
    ipv6: "IPv6 \u0BAE\u0BC1\u0B95\u0BB5\u0BB0\u0BBF",
    cidrv4: "IPv4 \u0BB5\u0BB0\u0BAE\u0BCD\u0BAA\u0BC1",
    cidrv6: "IPv6 \u0BB5\u0BB0\u0BAE\u0BCD\u0BAA\u0BC1",
    base64: "base64-encoded \u0B9A\u0BB0\u0BAE\u0BCD",
    base64url: "base64url-encoded \u0B9A\u0BB0\u0BAE\u0BCD",
    json_string: "JSON \u0B9A\u0BB0\u0BAE\u0BCD",
    e164: "E.164 \u0B8E\u0BA3\u0BCD",
    jwt: "JWT",
    template_literal: "input"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0B8E\u0BA3\u0BCD",
    array: "\u0B85\u0BA3\u0BBF",
    null: "\u0BB5\u0BC6\u0BB1\u0BC1\u0BAE\u0BC8"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B89\u0BB3\u0BCD\u0BB3\u0BC0\u0B9F\u0BC1: \u0B8E\u0BA4\u0BBF\u0BB0\u0BCD\u0BAA\u0BBE\u0BB0\u0BCD\u0B95\u0BCD\u0B95\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 instanceof ${issue2.expected}, \u0BAA\u0BC6\u0BB1\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 ${received}`;
        }
        return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B89\u0BB3\u0BCD\u0BB3\u0BC0\u0B9F\u0BC1: \u0B8E\u0BA4\u0BBF\u0BB0\u0BCD\u0BAA\u0BBE\u0BB0\u0BCD\u0B95\u0BCD\u0B95\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 ${expected}, \u0BAA\u0BC6\u0BB1\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B89\u0BB3\u0BCD\u0BB3\u0BC0\u0B9F\u0BC1: \u0B8E\u0BA4\u0BBF\u0BB0\u0BCD\u0BAA\u0BBE\u0BB0\u0BCD\u0B95\u0BCD\u0B95\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 ${stringifyPrimitive(issue2.values[0])}`;
        return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0BB5\u0BBF\u0BB0\u0BC1\u0BAA\u0BCD\u0BAA\u0BAE\u0BCD: \u0B8E\u0BA4\u0BBF\u0BB0\u0BCD\u0BAA\u0BBE\u0BB0\u0BCD\u0B95\u0BCD\u0B95\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 ${joinValues(issue2.values, "|")} \u0B87\u0BB2\u0BCD \u0B92\u0BA9\u0BCD\u0BB1\u0BC1`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u0BAE\u0BBF\u0B95 \u0BAA\u0BC6\u0BB0\u0BBF\u0BAF\u0BA4\u0BC1: \u0B8E\u0BA4\u0BBF\u0BB0\u0BCD\u0BAA\u0BBE\u0BB0\u0BCD\u0B95\u0BCD\u0B95\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 ${issue2.origin ?? "\u0BAE\u0BA4\u0BBF\u0BAA\u0BCD\u0BAA\u0BC1"} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u0B89\u0BB1\u0BC1\u0BAA\u0BCD\u0BAA\u0BC1\u0B95\u0BB3\u0BCD"} \u0B86\u0B95 \u0B87\u0BB0\u0BC1\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD`;
        }
        return `\u0BAE\u0BBF\u0B95 \u0BAA\u0BC6\u0BB0\u0BBF\u0BAF\u0BA4\u0BC1: \u0B8E\u0BA4\u0BBF\u0BB0\u0BCD\u0BAA\u0BBE\u0BB0\u0BCD\u0B95\u0BCD\u0B95\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 ${issue2.origin ?? "\u0BAE\u0BA4\u0BBF\u0BAA\u0BCD\u0BAA\u0BC1"} ${adj}${issue2.maximum.toString()} \u0B86\u0B95 \u0B87\u0BB0\u0BC1\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u0BAE\u0BBF\u0B95\u0B9A\u0BCD \u0B9A\u0BBF\u0BB1\u0BBF\u0BAF\u0BA4\u0BC1: \u0B8E\u0BA4\u0BBF\u0BB0\u0BCD\u0BAA\u0BBE\u0BB0\u0BCD\u0B95\u0BCD\u0B95\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 ${issue2.origin} ${adj}${issue2.minimum.toString()} ${sizing.unit} \u0B86\u0B95 \u0B87\u0BB0\u0BC1\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD`;
        }
        return `\u0BAE\u0BBF\u0B95\u0B9A\u0BCD \u0B9A\u0BBF\u0BB1\u0BBF\u0BAF\u0BA4\u0BC1: \u0B8E\u0BA4\u0BBF\u0BB0\u0BCD\u0BAA\u0BBE\u0BB0\u0BCD\u0B95\u0BCD\u0B95\u0BAA\u0BCD\u0BAA\u0B9F\u0BCD\u0B9F\u0BA4\u0BC1 ${issue2.origin} ${adj}${issue2.minimum.toString()} \u0B86\u0B95 \u0B87\u0BB0\u0BC1\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B9A\u0BB0\u0BAE\u0BCD: "${_issue.prefix}" \u0B87\u0BB2\u0BCD \u0BA4\u0BCA\u0B9F\u0B99\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD`;
        if (_issue.format === "ends_with")
          return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B9A\u0BB0\u0BAE\u0BCD: "${_issue.suffix}" \u0B87\u0BB2\u0BCD \u0BAE\u0BC1\u0B9F\u0BBF\u0BB5\u0B9F\u0BC8\u0BAF \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD`;
        if (_issue.format === "includes")
          return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B9A\u0BB0\u0BAE\u0BCD: "${_issue.includes}" \u0B90 \u0B89\u0BB3\u0BCD\u0BB3\u0B9F\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD`;
        if (_issue.format === "regex")
          return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B9A\u0BB0\u0BAE\u0BCD: ${_issue.pattern} \u0BAE\u0BC1\u0BB1\u0BC8\u0BAA\u0BBE\u0B9F\u0BCD\u0B9F\u0BC1\u0B9F\u0BA9\u0BCD \u0BAA\u0BCA\u0BB0\u0BC1\u0BA8\u0BCD\u0BA4 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD`;
        return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B8E\u0BA3\u0BCD: ${issue2.divisor} \u0B87\u0BA9\u0BCD \u0BAA\u0BB2\u0BAE\u0BBE\u0B95 \u0B87\u0BB0\u0BC1\u0B95\u0BCD\u0B95 \u0BB5\u0BC7\u0BA3\u0BCD\u0B9F\u0BC1\u0BAE\u0BCD`;
      case "unrecognized_keys":
        return `\u0B85\u0B9F\u0BC8\u0BAF\u0BBE\u0BB3\u0BAE\u0BCD \u0BA4\u0BC6\u0BB0\u0BBF\u0BAF\u0BBE\u0BA4 \u0BB5\u0BBF\u0B9A\u0BC8${issue2.keys.length > 1 ? "\u0B95\u0BB3\u0BCD" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `${issue2.origin} \u0B87\u0BB2\u0BCD \u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0BB5\u0BBF\u0B9A\u0BC8`;
      case "invalid_union":
        return "\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B89\u0BB3\u0BCD\u0BB3\u0BC0\u0B9F\u0BC1";
      case "invalid_element":
        return `${issue2.origin} \u0B87\u0BB2\u0BCD \u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0BAE\u0BA4\u0BBF\u0BAA\u0BCD\u0BAA\u0BC1`;
      default:
        return `\u0BA4\u0BB5\u0BB1\u0BBE\u0BA9 \u0B89\u0BB3\u0BCD\u0BB3\u0BC0\u0B9F\u0BC1`;
    }
  };
};
function ta_default() {
  return {
    localeError: error41()
  };
}

// node_modules/zod/v4/locales/th.js
var error42 = () => {
  const Sizable = {
    string: { unit: "\u0E15\u0E31\u0E27\u0E2D\u0E31\u0E01\u0E29\u0E23", verb: "\u0E04\u0E27\u0E23\u0E21\u0E35" },
    file: { unit: "\u0E44\u0E1A\u0E15\u0E4C", verb: "\u0E04\u0E27\u0E23\u0E21\u0E35" },
    array: { unit: "\u0E23\u0E32\u0E22\u0E01\u0E32\u0E23", verb: "\u0E04\u0E27\u0E23\u0E21\u0E35" },
    set: { unit: "\u0E23\u0E32\u0E22\u0E01\u0E32\u0E23", verb: "\u0E04\u0E27\u0E23\u0E21\u0E35" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0E02\u0E49\u0E2D\u0E21\u0E39\u0E25\u0E17\u0E35\u0E48\u0E1B\u0E49\u0E2D\u0E19",
    email: "\u0E17\u0E35\u0E48\u0E2D\u0E22\u0E39\u0E48\u0E2D\u0E35\u0E40\u0E21\u0E25",
    url: "URL",
    emoji: "\u0E2D\u0E34\u0E42\u0E21\u0E08\u0E34",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "\u0E27\u0E31\u0E19\u0E17\u0E35\u0E48\u0E40\u0E27\u0E25\u0E32\u0E41\u0E1A\u0E1A ISO",
    date: "\u0E27\u0E31\u0E19\u0E17\u0E35\u0E48\u0E41\u0E1A\u0E1A ISO",
    time: "\u0E40\u0E27\u0E25\u0E32\u0E41\u0E1A\u0E1A ISO",
    duration: "\u0E0A\u0E48\u0E27\u0E07\u0E40\u0E27\u0E25\u0E32\u0E41\u0E1A\u0E1A ISO",
    ipv4: "\u0E17\u0E35\u0E48\u0E2D\u0E22\u0E39\u0E48 IPv4",
    ipv6: "\u0E17\u0E35\u0E48\u0E2D\u0E22\u0E39\u0E48 IPv6",
    cidrv4: "\u0E0A\u0E48\u0E27\u0E07 IP \u0E41\u0E1A\u0E1A IPv4",
    cidrv6: "\u0E0A\u0E48\u0E27\u0E07 IP \u0E41\u0E1A\u0E1A IPv6",
    base64: "\u0E02\u0E49\u0E2D\u0E04\u0E27\u0E32\u0E21\u0E41\u0E1A\u0E1A Base64",
    base64url: "\u0E02\u0E49\u0E2D\u0E04\u0E27\u0E32\u0E21\u0E41\u0E1A\u0E1A Base64 \u0E2A\u0E33\u0E2B\u0E23\u0E31\u0E1A URL",
    json_string: "\u0E02\u0E49\u0E2D\u0E04\u0E27\u0E32\u0E21\u0E41\u0E1A\u0E1A JSON",
    e164: "\u0E40\u0E1A\u0E2D\u0E23\u0E4C\u0E42\u0E17\u0E23\u0E28\u0E31\u0E1E\u0E17\u0E4C\u0E23\u0E30\u0E2B\u0E27\u0E48\u0E32\u0E07\u0E1B\u0E23\u0E30\u0E40\u0E17\u0E28 (E.164)",
    jwt: "\u0E42\u0E17\u0E40\u0E04\u0E19 JWT",
    template_literal: "\u0E02\u0E49\u0E2D\u0E21\u0E39\u0E25\u0E17\u0E35\u0E48\u0E1B\u0E49\u0E2D\u0E19"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0E15\u0E31\u0E27\u0E40\u0E25\u0E02",
    array: "\u0E2D\u0E32\u0E23\u0E4C\u0E40\u0E23\u0E22\u0E4C (Array)",
    null: "\u0E44\u0E21\u0E48\u0E21\u0E35\u0E04\u0E48\u0E32 (null)"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u0E1B\u0E23\u0E30\u0E40\u0E20\u0E17\u0E02\u0E49\u0E2D\u0E21\u0E39\u0E25\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E04\u0E27\u0E23\u0E40\u0E1B\u0E47\u0E19 instanceof ${issue2.expected} \u0E41\u0E15\u0E48\u0E44\u0E14\u0E49\u0E23\u0E31\u0E1A ${received}`;
        }
        return `\u0E1B\u0E23\u0E30\u0E40\u0E20\u0E17\u0E02\u0E49\u0E2D\u0E21\u0E39\u0E25\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E04\u0E27\u0E23\u0E40\u0E1B\u0E47\u0E19 ${expected} \u0E41\u0E15\u0E48\u0E44\u0E14\u0E49\u0E23\u0E31\u0E1A ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u0E04\u0E48\u0E32\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E04\u0E27\u0E23\u0E40\u0E1B\u0E47\u0E19 ${stringifyPrimitive(issue2.values[0])}`;
        return `\u0E15\u0E31\u0E27\u0E40\u0E25\u0E37\u0E2D\u0E01\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E04\u0E27\u0E23\u0E40\u0E1B\u0E47\u0E19\u0E2B\u0E19\u0E36\u0E48\u0E07\u0E43\u0E19 ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "\u0E44\u0E21\u0E48\u0E40\u0E01\u0E34\u0E19" : "\u0E19\u0E49\u0E2D\u0E22\u0E01\u0E27\u0E48\u0E32";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u0E40\u0E01\u0E34\u0E19\u0E01\u0E33\u0E2B\u0E19\u0E14: ${issue2.origin ?? "\u0E04\u0E48\u0E32"} \u0E04\u0E27\u0E23\u0E21\u0E35${adj} ${issue2.maximum.toString()} ${sizing.unit ?? "\u0E23\u0E32\u0E22\u0E01\u0E32\u0E23"}`;
        return `\u0E40\u0E01\u0E34\u0E19\u0E01\u0E33\u0E2B\u0E19\u0E14: ${issue2.origin ?? "\u0E04\u0E48\u0E32"} \u0E04\u0E27\u0E23\u0E21\u0E35${adj} ${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? "\u0E2D\u0E22\u0E48\u0E32\u0E07\u0E19\u0E49\u0E2D\u0E22" : "\u0E21\u0E32\u0E01\u0E01\u0E27\u0E48\u0E32";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u0E19\u0E49\u0E2D\u0E22\u0E01\u0E27\u0E48\u0E32\u0E01\u0E33\u0E2B\u0E19\u0E14: ${issue2.origin} \u0E04\u0E27\u0E23\u0E21\u0E35${adj} ${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u0E19\u0E49\u0E2D\u0E22\u0E01\u0E27\u0E48\u0E32\u0E01\u0E33\u0E2B\u0E19\u0E14: ${issue2.origin} \u0E04\u0E27\u0E23\u0E21\u0E35${adj} ${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u0E23\u0E39\u0E1B\u0E41\u0E1A\u0E1A\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E02\u0E49\u0E2D\u0E04\u0E27\u0E32\u0E21\u0E15\u0E49\u0E2D\u0E07\u0E02\u0E36\u0E49\u0E19\u0E15\u0E49\u0E19\u0E14\u0E49\u0E27\u0E22 "${_issue.prefix}"`;
        }
        if (_issue.format === "ends_with")
          return `\u0E23\u0E39\u0E1B\u0E41\u0E1A\u0E1A\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E02\u0E49\u0E2D\u0E04\u0E27\u0E32\u0E21\u0E15\u0E49\u0E2D\u0E07\u0E25\u0E07\u0E17\u0E49\u0E32\u0E22\u0E14\u0E49\u0E27\u0E22 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u0E23\u0E39\u0E1B\u0E41\u0E1A\u0E1A\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E02\u0E49\u0E2D\u0E04\u0E27\u0E32\u0E21\u0E15\u0E49\u0E2D\u0E07\u0E21\u0E35 "${_issue.includes}" \u0E2D\u0E22\u0E39\u0E48\u0E43\u0E19\u0E02\u0E49\u0E2D\u0E04\u0E27\u0E32\u0E21`;
        if (_issue.format === "regex")
          return `\u0E23\u0E39\u0E1B\u0E41\u0E1A\u0E1A\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E15\u0E49\u0E2D\u0E07\u0E15\u0E23\u0E07\u0E01\u0E31\u0E1A\u0E23\u0E39\u0E1B\u0E41\u0E1A\u0E1A\u0E17\u0E35\u0E48\u0E01\u0E33\u0E2B\u0E19\u0E14 ${_issue.pattern}`;
        return `\u0E23\u0E39\u0E1B\u0E41\u0E1A\u0E1A\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u0E15\u0E31\u0E27\u0E40\u0E25\u0E02\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E15\u0E49\u0E2D\u0E07\u0E40\u0E1B\u0E47\u0E19\u0E08\u0E33\u0E19\u0E27\u0E19\u0E17\u0E35\u0E48\u0E2B\u0E32\u0E23\u0E14\u0E49\u0E27\u0E22 ${issue2.divisor} \u0E44\u0E14\u0E49\u0E25\u0E07\u0E15\u0E31\u0E27`;
      case "unrecognized_keys":
        return `\u0E1E\u0E1A\u0E04\u0E35\u0E22\u0E4C\u0E17\u0E35\u0E48\u0E44\u0E21\u0E48\u0E23\u0E39\u0E49\u0E08\u0E31\u0E01: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u0E04\u0E35\u0E22\u0E4C\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07\u0E43\u0E19 ${issue2.origin}`;
      case "invalid_union":
        return "\u0E02\u0E49\u0E2D\u0E21\u0E39\u0E25\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07: \u0E44\u0E21\u0E48\u0E15\u0E23\u0E07\u0E01\u0E31\u0E1A\u0E23\u0E39\u0E1B\u0E41\u0E1A\u0E1A\u0E22\u0E39\u0E40\u0E19\u0E35\u0E22\u0E19\u0E17\u0E35\u0E48\u0E01\u0E33\u0E2B\u0E19\u0E14\u0E44\u0E27\u0E49";
      case "invalid_element":
        return `\u0E02\u0E49\u0E2D\u0E21\u0E39\u0E25\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07\u0E43\u0E19 ${issue2.origin}`;
      default:
        return `\u0E02\u0E49\u0E2D\u0E21\u0E39\u0E25\u0E44\u0E21\u0E48\u0E16\u0E39\u0E01\u0E15\u0E49\u0E2D\u0E07`;
    }
  };
};
function th_default() {
  return {
    localeError: error42()
  };
}

// node_modules/zod/v4/locales/tr.js
var error43 = () => {
  const Sizable = {
    string: { unit: "karakter", verb: "olmal\u0131" },
    file: { unit: "bayt", verb: "olmal\u0131" },
    array: { unit: "\xF6\u011Fe", verb: "olmal\u0131" },
    set: { unit: "\xF6\u011Fe", verb: "olmal\u0131" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "girdi",
    email: "e-posta adresi",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO tarih ve saat",
    date: "ISO tarih",
    time: "ISO saat",
    duration: "ISO s\xFCre",
    ipv4: "IPv4 adresi",
    ipv6: "IPv6 adresi",
    cidrv4: "IPv4 aral\u0131\u011F\u0131",
    cidrv6: "IPv6 aral\u0131\u011F\u0131",
    base64: "base64 ile \u015Fifrelenmi\u015F metin",
    base64url: "base64url ile \u015Fifrelenmi\u015F metin",
    json_string: "JSON dizesi",
    e164: "E.164 say\u0131s\u0131",
    jwt: "JWT",
    template_literal: "\u015Eablon dizesi"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Ge\xE7ersiz de\u011Fer: beklenen instanceof ${issue2.expected}, al\u0131nan ${received}`;
        }
        return `Ge\xE7ersiz de\u011Fer: beklenen ${expected}, al\u0131nan ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Ge\xE7ersiz de\u011Fer: beklenen ${stringifyPrimitive(issue2.values[0])}`;
        return `Ge\xE7ersiz se\xE7enek: a\u015Fa\u011F\u0131dakilerden biri olmal\u0131: ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\xC7ok b\xFCy\xFCk: beklenen ${issue2.origin ?? "de\u011Fer"} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\xF6\u011Fe"}`;
        return `\xC7ok b\xFCy\xFCk: beklenen ${issue2.origin ?? "de\u011Fer"} ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\xC7ok k\xFC\xE7\xFCk: beklenen ${issue2.origin} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        return `\xC7ok k\xFC\xE7\xFCk: beklenen ${issue2.origin} ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Ge\xE7ersiz metin: "${_issue.prefix}" ile ba\u015Flamal\u0131`;
        if (_issue.format === "ends_with")
          return `Ge\xE7ersiz metin: "${_issue.suffix}" ile bitmeli`;
        if (_issue.format === "includes")
          return `Ge\xE7ersiz metin: "${_issue.includes}" i\xE7ermeli`;
        if (_issue.format === "regex")
          return `Ge\xE7ersiz metin: ${_issue.pattern} desenine uymal\u0131`;
        return `Ge\xE7ersiz ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Ge\xE7ersiz say\u0131: ${issue2.divisor} ile tam b\xF6l\xFCnebilmeli`;
      case "unrecognized_keys":
        return `Tan\u0131nmayan anahtar${issue2.keys.length > 1 ? "lar" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `${issue2.origin} i\xE7inde ge\xE7ersiz anahtar`;
      case "invalid_union":
        return "Ge\xE7ersiz de\u011Fer";
      case "invalid_element":
        return `${issue2.origin} i\xE7inde ge\xE7ersiz de\u011Fer`;
      default:
        return `Ge\xE7ersiz de\u011Fer`;
    }
  };
};
function tr_default() {
  return {
    localeError: error43()
  };
}

// node_modules/zod/v4/locales/uk.js
var error44 = () => {
  const Sizable = {
    string: { unit: "\u0441\u0438\u043C\u0432\u043E\u043B\u0456\u0432", verb: "\u043C\u0430\u0442\u0438\u043C\u0435" },
    file: { unit: "\u0431\u0430\u0439\u0442\u0456\u0432", verb: "\u043C\u0430\u0442\u0438\u043C\u0435" },
    array: { unit: "\u0435\u043B\u0435\u043C\u0435\u043D\u0442\u0456\u0432", verb: "\u043C\u0430\u0442\u0438\u043C\u0435" },
    set: { unit: "\u0435\u043B\u0435\u043C\u0435\u043D\u0442\u0456\u0432", verb: "\u043C\u0430\u0442\u0438\u043C\u0435" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0432\u0445\u0456\u0434\u043D\u0456 \u0434\u0430\u043D\u0456",
    email: "\u0430\u0434\u0440\u0435\u0441\u0430 \u0435\u043B\u0435\u043A\u0442\u0440\u043E\u043D\u043D\u043E\u0457 \u043F\u043E\u0448\u0442\u0438",
    url: "URL",
    emoji: "\u0435\u043C\u043E\u0434\u0437\u0456",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "\u0434\u0430\u0442\u0430 \u0442\u0430 \u0447\u0430\u0441 ISO",
    date: "\u0434\u0430\u0442\u0430 ISO",
    time: "\u0447\u0430\u0441 ISO",
    duration: "\u0442\u0440\u0438\u0432\u0430\u043B\u0456\u0441\u0442\u044C ISO",
    ipv4: "\u0430\u0434\u0440\u0435\u0441\u0430 IPv4",
    ipv6: "\u0430\u0434\u0440\u0435\u0441\u0430 IPv6",
    cidrv4: "\u0434\u0456\u0430\u043F\u0430\u0437\u043E\u043D IPv4",
    cidrv6: "\u0434\u0456\u0430\u043F\u0430\u0437\u043E\u043D IPv6",
    base64: "\u0440\u044F\u0434\u043E\u043A \u0443 \u043A\u043E\u0434\u0443\u0432\u0430\u043D\u043D\u0456 base64",
    base64url: "\u0440\u044F\u0434\u043E\u043A \u0443 \u043A\u043E\u0434\u0443\u0432\u0430\u043D\u043D\u0456 base64url",
    json_string: "\u0440\u044F\u0434\u043E\u043A JSON",
    e164: "\u043D\u043E\u043C\u0435\u0440 E.164",
    jwt: "JWT",
    template_literal: "\u0432\u0445\u0456\u0434\u043D\u0456 \u0434\u0430\u043D\u0456"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0447\u0438\u0441\u043B\u043E",
    array: "\u043C\u0430\u0441\u0438\u0432"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0456 \u0432\u0445\u0456\u0434\u043D\u0456 \u0434\u0430\u043D\u0456: \u043E\u0447\u0456\u043A\u0443\u0454\u0442\u044C\u0441\u044F instanceof ${issue2.expected}, \u043E\u0442\u0440\u0438\u043C\u0430\u043D\u043E ${received}`;
        }
        return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0456 \u0432\u0445\u0456\u0434\u043D\u0456 \u0434\u0430\u043D\u0456: \u043E\u0447\u0456\u043A\u0443\u0454\u0442\u044C\u0441\u044F ${expected}, \u043E\u0442\u0440\u0438\u043C\u0430\u043D\u043E ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0456 \u0432\u0445\u0456\u0434\u043D\u0456 \u0434\u0430\u043D\u0456: \u043E\u0447\u0456\u043A\u0443\u0454\u0442\u044C\u0441\u044F ${stringifyPrimitive(issue2.values[0])}`;
        return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0430 \u043E\u043F\u0446\u0456\u044F: \u043E\u0447\u0456\u043A\u0443\u0454\u0442\u044C\u0441\u044F \u043E\u0434\u043D\u0435 \u0437 ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u0417\u0430\u043D\u0430\u0434\u0442\u043E \u0432\u0435\u043B\u0438\u043A\u0435: \u043E\u0447\u0456\u043A\u0443\u0454\u0442\u044C\u0441\u044F, \u0449\u043E ${issue2.origin ?? "\u0437\u043D\u0430\u0447\u0435\u043D\u043D\u044F"} ${sizing.verb} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u0435\u043B\u0435\u043C\u0435\u043D\u0442\u0456\u0432"}`;
        return `\u0417\u0430\u043D\u0430\u0434\u0442\u043E \u0432\u0435\u043B\u0438\u043A\u0435: \u043E\u0447\u0456\u043A\u0443\u0454\u0442\u044C\u0441\u044F, \u0449\u043E ${issue2.origin ?? "\u0437\u043D\u0430\u0447\u0435\u043D\u043D\u044F"} \u0431\u0443\u0434\u0435 ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u0417\u0430\u043D\u0430\u0434\u0442\u043E \u043C\u0430\u043B\u0435: \u043E\u0447\u0456\u043A\u0443\u0454\u0442\u044C\u0441\u044F, \u0449\u043E ${issue2.origin} ${sizing.verb} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u0417\u0430\u043D\u0430\u0434\u0442\u043E \u043C\u0430\u043B\u0435: \u043E\u0447\u0456\u043A\u0443\u0454\u0442\u044C\u0441\u044F, \u0449\u043E ${issue2.origin} \u0431\u0443\u0434\u0435 ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0438\u0439 \u0440\u044F\u0434\u043E\u043A: \u043F\u043E\u0432\u0438\u043D\u0435\u043D \u043F\u043E\u0447\u0438\u043D\u0430\u0442\u0438\u0441\u044F \u0437 "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0438\u0439 \u0440\u044F\u0434\u043E\u043A: \u043F\u043E\u0432\u0438\u043D\u0435\u043D \u0437\u0430\u043A\u0456\u043D\u0447\u0443\u0432\u0430\u0442\u0438\u0441\u044F \u043D\u0430 "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0438\u0439 \u0440\u044F\u0434\u043E\u043A: \u043F\u043E\u0432\u0438\u043D\u0435\u043D \u043C\u0456\u0441\u0442\u0438\u0442\u0438 "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0438\u0439 \u0440\u044F\u0434\u043E\u043A: \u043F\u043E\u0432\u0438\u043D\u0435\u043D \u0432\u0456\u0434\u043F\u043E\u0432\u0456\u0434\u0430\u0442\u0438 \u0448\u0430\u0431\u043B\u043E\u043D\u0443 ${_issue.pattern}`;
        return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0438\u0439 ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0435 \u0447\u0438\u0441\u043B\u043E: \u043F\u043E\u0432\u0438\u043D\u043D\u043E \u0431\u0443\u0442\u0438 \u043A\u0440\u0430\u0442\u043D\u0438\u043C ${issue2.divisor}`;
      case "unrecognized_keys":
        return `\u041D\u0435\u0440\u043E\u0437\u043F\u0456\u0437\u043D\u0430\u043D\u0438\u0439 \u043A\u043B\u044E\u0447${issue2.keys.length > 1 ? "\u0456" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0438\u0439 \u043A\u043B\u044E\u0447 \u0443 ${issue2.origin}`;
      case "invalid_union":
        return "\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0456 \u0432\u0445\u0456\u0434\u043D\u0456 \u0434\u0430\u043D\u0456";
      case "invalid_element":
        return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0435 \u0437\u043D\u0430\u0447\u0435\u043D\u043D\u044F \u0443 ${issue2.origin}`;
      default:
        return `\u041D\u0435\u043F\u0440\u0430\u0432\u0438\u043B\u044C\u043D\u0456 \u0432\u0445\u0456\u0434\u043D\u0456 \u0434\u0430\u043D\u0456`;
    }
  };
};
function uk_default() {
  return {
    localeError: error44()
  };
}

// node_modules/zod/v4/locales/ua.js
function ua_default() {
  return uk_default();
}

// node_modules/zod/v4/locales/ur.js
var error45 = () => {
  const Sizable = {
    string: { unit: "\u062D\u0631\u0648\u0641", verb: "\u06C1\u0648\u0646\u0627" },
    file: { unit: "\u0628\u0627\u0626\u0679\u0633", verb: "\u06C1\u0648\u0646\u0627" },
    array: { unit: "\u0622\u0626\u0679\u0645\u0632", verb: "\u06C1\u0648\u0646\u0627" },
    set: { unit: "\u0622\u0626\u0679\u0645\u0632", verb: "\u06C1\u0648\u0646\u0627" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0627\u0646 \u067E\u0679",
    email: "\u0627\u06CC \u0645\u06CC\u0644 \u0627\u06CC\u0688\u0631\u06CC\u0633",
    url: "\u06CC\u0648 \u0622\u0631 \u0627\u06CC\u0644",
    emoji: "\u0627\u06CC\u0645\u0648\u062C\u06CC",
    uuid: "\u06CC\u0648 \u06CC\u0648 \u0622\u0626\u06CC \u0688\u06CC",
    uuidv4: "\u06CC\u0648 \u06CC\u0648 \u0622\u0626\u06CC \u0688\u06CC \u0648\u06CC 4",
    uuidv6: "\u06CC\u0648 \u06CC\u0648 \u0622\u0626\u06CC \u0688\u06CC \u0648\u06CC 6",
    nanoid: "\u0646\u06CC\u0646\u0648 \u0622\u0626\u06CC \u0688\u06CC",
    guid: "\u062C\u06CC \u06CC\u0648 \u0622\u0626\u06CC \u0688\u06CC",
    cuid: "\u0633\u06CC \u06CC\u0648 \u0622\u0626\u06CC \u0688\u06CC",
    cuid2: "\u0633\u06CC \u06CC\u0648 \u0622\u0626\u06CC \u0688\u06CC 2",
    ulid: "\u06CC\u0648 \u0627\u06CC\u0644 \u0622\u0626\u06CC \u0688\u06CC",
    xid: "\u0627\u06CC\u06A9\u0633 \u0622\u0626\u06CC \u0688\u06CC",
    ksuid: "\u06A9\u06D2 \u0627\u06CC\u0633 \u06CC\u0648 \u0622\u0626\u06CC \u0688\u06CC",
    datetime: "\u0622\u0626\u06CC \u0627\u06CC\u0633 \u0627\u0648 \u0688\u06CC\u0679 \u0679\u0627\u0626\u0645",
    date: "\u0622\u0626\u06CC \u0627\u06CC\u0633 \u0627\u0648 \u062A\u0627\u0631\u06CC\u062E",
    time: "\u0622\u0626\u06CC \u0627\u06CC\u0633 \u0627\u0648 \u0648\u0642\u062A",
    duration: "\u0622\u0626\u06CC \u0627\u06CC\u0633 \u0627\u0648 \u0645\u062F\u062A",
    ipv4: "\u0622\u0626\u06CC \u067E\u06CC \u0648\u06CC 4 \u0627\u06CC\u0688\u0631\u06CC\u0633",
    ipv6: "\u0622\u0626\u06CC \u067E\u06CC \u0648\u06CC 6 \u0627\u06CC\u0688\u0631\u06CC\u0633",
    cidrv4: "\u0622\u0626\u06CC \u067E\u06CC \u0648\u06CC 4 \u0631\u06CC\u0646\u062C",
    cidrv6: "\u0622\u0626\u06CC \u067E\u06CC \u0648\u06CC 6 \u0631\u06CC\u0646\u062C",
    base64: "\u0628\u06CC\u0633 64 \u0627\u0646 \u06A9\u0648\u0688\u0688 \u0633\u0679\u0631\u0646\u06AF",
    base64url: "\u0628\u06CC\u0633 64 \u06CC\u0648 \u0622\u0631 \u0627\u06CC\u0644 \u0627\u0646 \u06A9\u0648\u0688\u0688 \u0633\u0679\u0631\u0646\u06AF",
    json_string: "\u062C\u06D2 \u0627\u06CC\u0633 \u0627\u0648 \u0627\u06CC\u0646 \u0633\u0679\u0631\u0646\u06AF",
    e164: "\u0627\u06CC 164 \u0646\u0645\u0628\u0631",
    jwt: "\u062C\u06D2 \u0688\u0628\u0644\u06CC\u0648 \u0679\u06CC",
    template_literal: "\u0627\u0646 \u067E\u0679"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u0646\u0645\u0628\u0631",
    array: "\u0622\u0631\u06D2",
    null: "\u0646\u0644"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u063A\u0644\u0637 \u0627\u0646 \u067E\u0679: instanceof ${issue2.expected} \u0645\u062A\u0648\u0642\u0639 \u062A\u06BE\u0627\u060C ${received} \u0645\u0648\u0635\u0648\u0644 \u06C1\u0648\u0627`;
        }
        return `\u063A\u0644\u0637 \u0627\u0646 \u067E\u0679: ${expected} \u0645\u062A\u0648\u0642\u0639 \u062A\u06BE\u0627\u060C ${received} \u0645\u0648\u0635\u0648\u0644 \u06C1\u0648\u0627`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u063A\u0644\u0637 \u0627\u0646 \u067E\u0679: ${stringifyPrimitive(issue2.values[0])} \u0645\u062A\u0648\u0642\u0639 \u062A\u06BE\u0627`;
        return `\u063A\u0644\u0637 \u0622\u067E\u0634\u0646: ${joinValues(issue2.values, "|")} \u0645\u06CC\u06BA \u0633\u06D2 \u0627\u06CC\u06A9 \u0645\u062A\u0648\u0642\u0639 \u062A\u06BE\u0627`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u0628\u06C1\u062A \u0628\u0691\u0627: ${issue2.origin ?? "\u0648\u06CC\u0644\u06CC\u0648"} \u06A9\u06D2 ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u0639\u0646\u0627\u0635\u0631"} \u06C1\u0648\u0646\u06D2 \u0645\u062A\u0648\u0642\u0639 \u062A\u06BE\u06D2`;
        return `\u0628\u06C1\u062A \u0628\u0691\u0627: ${issue2.origin ?? "\u0648\u06CC\u0644\u06CC\u0648"} \u06A9\u0627 ${adj}${issue2.maximum.toString()} \u06C1\u0648\u0646\u0627 \u0645\u062A\u0648\u0642\u0639 \u062A\u06BE\u0627`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u0628\u06C1\u062A \u0686\u06BE\u0648\u0679\u0627: ${issue2.origin} \u06A9\u06D2 ${adj}${issue2.minimum.toString()} ${sizing.unit} \u06C1\u0648\u0646\u06D2 \u0645\u062A\u0648\u0642\u0639 \u062A\u06BE\u06D2`;
        }
        return `\u0628\u06C1\u062A \u0686\u06BE\u0648\u0679\u0627: ${issue2.origin} \u06A9\u0627 ${adj}${issue2.minimum.toString()} \u06C1\u0648\u0646\u0627 \u0645\u062A\u0648\u0642\u0639 \u062A\u06BE\u0627`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u063A\u0644\u0637 \u0633\u0679\u0631\u0646\u06AF: "${_issue.prefix}" \u0633\u06D2 \u0634\u0631\u0648\u0639 \u06C1\u0648\u0646\u0627 \u0686\u0627\u06C1\u06CC\u06D2`;
        }
        if (_issue.format === "ends_with")
          return `\u063A\u0644\u0637 \u0633\u0679\u0631\u0646\u06AF: "${_issue.suffix}" \u067E\u0631 \u062E\u062A\u0645 \u06C1\u0648\u0646\u0627 \u0686\u0627\u06C1\u06CC\u06D2`;
        if (_issue.format === "includes")
          return `\u063A\u0644\u0637 \u0633\u0679\u0631\u0646\u06AF: "${_issue.includes}" \u0634\u0627\u0645\u0644 \u06C1\u0648\u0646\u0627 \u0686\u0627\u06C1\u06CC\u06D2`;
        if (_issue.format === "regex")
          return `\u063A\u0644\u0637 \u0633\u0679\u0631\u0646\u06AF: \u067E\u06CC\u0679\u0631\u0646 ${_issue.pattern} \u0633\u06D2 \u0645\u06CC\u0686 \u06C1\u0648\u0646\u0627 \u0686\u0627\u06C1\u06CC\u06D2`;
        return `\u063A\u0644\u0637 ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u063A\u0644\u0637 \u0646\u0645\u0628\u0631: ${issue2.divisor} \u06A9\u0627 \u0645\u0636\u0627\u0639\u0641 \u06C1\u0648\u0646\u0627 \u0686\u0627\u06C1\u06CC\u06D2`;
      case "unrecognized_keys":
        return `\u063A\u06CC\u0631 \u062A\u0633\u0644\u06CC\u0645 \u0634\u062F\u06C1 \u06A9\u06CC${issue2.keys.length > 1 ? "\u0632" : ""}: ${joinValues(issue2.keys, "\u060C ")}`;
      case "invalid_key":
        return `${issue2.origin} \u0645\u06CC\u06BA \u063A\u0644\u0637 \u06A9\u06CC`;
      case "invalid_union":
        return "\u063A\u0644\u0637 \u0627\u0646 \u067E\u0679";
      case "invalid_element":
        return `${issue2.origin} \u0645\u06CC\u06BA \u063A\u0644\u0637 \u0648\u06CC\u0644\u06CC\u0648`;
      default:
        return `\u063A\u0644\u0637 \u0627\u0646 \u067E\u0679`;
    }
  };
};
function ur_default() {
  return {
    localeError: error45()
  };
}

// node_modules/zod/v4/locales/uz.js
var error46 = () => {
  const Sizable = {
    string: { unit: "belgi", verb: "bo\u2018lishi kerak" },
    file: { unit: "bayt", verb: "bo\u2018lishi kerak" },
    array: { unit: "element", verb: "bo\u2018lishi kerak" },
    set: { unit: "element", verb: "bo\u2018lishi kerak" },
    map: { unit: "yozuv", verb: "bo\u2018lishi kerak" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "kirish",
    email: "elektron pochta manzili",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO sana va vaqti",
    date: "ISO sana",
    time: "ISO vaqt",
    duration: "ISO davomiylik",
    ipv4: "IPv4 manzil",
    ipv6: "IPv6 manzil",
    mac: "MAC manzil",
    cidrv4: "IPv4 diapazon",
    cidrv6: "IPv6 diapazon",
    base64: "base64 kodlangan satr",
    base64url: "base64url kodlangan satr",
    json_string: "JSON satr",
    e164: "E.164 raqam",
    jwt: "JWT",
    template_literal: "kirish"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "raqam",
    array: "massiv"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `Noto\u2018g\u2018ri kirish: kutilgan instanceof ${issue2.expected}, qabul qilingan ${received}`;
        }
        return `Noto\u2018g\u2018ri kirish: kutilgan ${expected}, qabul qilingan ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `Noto\u2018g\u2018ri kirish: kutilgan ${stringifyPrimitive(issue2.values[0])}`;
        return `Noto\u2018g\u2018ri variant: quyidagilardan biri kutilgan ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Juda katta: kutilgan ${issue2.origin ?? "qiymat"} ${adj}${issue2.maximum.toString()} ${sizing.unit} ${sizing.verb}`;
        return `Juda katta: kutilgan ${issue2.origin ?? "qiymat"} ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Juda kichik: kutilgan ${issue2.origin} ${adj}${issue2.minimum.toString()} ${sizing.unit} ${sizing.verb}`;
        }
        return `Juda kichik: kutilgan ${issue2.origin} ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Noto\u2018g\u2018ri satr: "${_issue.prefix}" bilan boshlanishi kerak`;
        if (_issue.format === "ends_with")
          return `Noto\u2018g\u2018ri satr: "${_issue.suffix}" bilan tugashi kerak`;
        if (_issue.format === "includes")
          return `Noto\u2018g\u2018ri satr: "${_issue.includes}" ni o\u2018z ichiga olishi kerak`;
        if (_issue.format === "regex")
          return `Noto\u2018g\u2018ri satr: ${_issue.pattern} shabloniga mos kelishi kerak`;
        return `Noto\u2018g\u2018ri ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `Noto\u2018g\u2018ri raqam: ${issue2.divisor} ning karralisi bo\u2018lishi kerak`;
      case "unrecognized_keys":
        return `Noma\u2019lum kalit${issue2.keys.length > 1 ? "lar" : ""}: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `${issue2.origin} dagi kalit noto\u2018g\u2018ri`;
      case "invalid_union":
        return "Noto\u2018g\u2018ri kirish";
      case "invalid_element":
        return `${issue2.origin} da noto\u2018g\u2018ri qiymat`;
      default:
        return `Noto\u2018g\u2018ri kirish`;
    }
  };
};
function uz_default() {
  return {
    localeError: error46()
  };
}

// node_modules/zod/v4/locales/vi.js
var error47 = () => {
  const Sizable = {
    string: { unit: "k\xFD t\u1EF1", verb: "c\xF3" },
    file: { unit: "byte", verb: "c\xF3" },
    array: { unit: "ph\u1EA7n t\u1EED", verb: "c\xF3" },
    set: { unit: "ph\u1EA7n t\u1EED", verb: "c\xF3" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u0111\u1EA7u v\xE0o",
    email: "\u0111\u1ECBa ch\u1EC9 email",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ng\xE0y gi\u1EDD ISO",
    date: "ng\xE0y ISO",
    time: "gi\u1EDD ISO",
    duration: "kho\u1EA3ng th\u1EDDi gian ISO",
    ipv4: "\u0111\u1ECBa ch\u1EC9 IPv4",
    ipv6: "\u0111\u1ECBa ch\u1EC9 IPv6",
    cidrv4: "d\u1EA3i IPv4",
    cidrv6: "d\u1EA3i IPv6",
    base64: "chu\u1ED7i m\xE3 h\xF3a base64",
    base64url: "chu\u1ED7i m\xE3 h\xF3a base64url",
    json_string: "chu\u1ED7i JSON",
    e164: "s\u1ED1 E.164",
    jwt: "JWT",
    template_literal: "\u0111\u1EA7u v\xE0o"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "s\u1ED1",
    array: "m\u1EA3ng"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u0110\u1EA7u v\xE0o kh\xF4ng h\u1EE3p l\u1EC7: mong \u0111\u1EE3i instanceof ${issue2.expected}, nh\u1EADn \u0111\u01B0\u1EE3c ${received}`;
        }
        return `\u0110\u1EA7u v\xE0o kh\xF4ng h\u1EE3p l\u1EC7: mong \u0111\u1EE3i ${expected}, nh\u1EADn \u0111\u01B0\u1EE3c ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u0110\u1EA7u v\xE0o kh\xF4ng h\u1EE3p l\u1EC7: mong \u0111\u1EE3i ${stringifyPrimitive(issue2.values[0])}`;
        return `T\xF9y ch\u1ECDn kh\xF4ng h\u1EE3p l\u1EC7: mong \u0111\u1EE3i m\u1ED9t trong c\xE1c gi\xE1 tr\u1ECB ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `Qu\xE1 l\u1EDBn: mong \u0111\u1EE3i ${issue2.origin ?? "gi\xE1 tr\u1ECB"} ${sizing.verb} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "ph\u1EA7n t\u1EED"}`;
        return `Qu\xE1 l\u1EDBn: mong \u0111\u1EE3i ${issue2.origin ?? "gi\xE1 tr\u1ECB"} ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `Qu\xE1 nh\u1ECF: mong \u0111\u1EE3i ${issue2.origin} ${sizing.verb} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `Qu\xE1 nh\u1ECF: mong \u0111\u1EE3i ${issue2.origin} ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `Chu\u1ED7i kh\xF4ng h\u1EE3p l\u1EC7: ph\u1EA3i b\u1EAFt \u0111\u1EA7u b\u1EB1ng "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `Chu\u1ED7i kh\xF4ng h\u1EE3p l\u1EC7: ph\u1EA3i k\u1EBFt th\xFAc b\u1EB1ng "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `Chu\u1ED7i kh\xF4ng h\u1EE3p l\u1EC7: ph\u1EA3i bao g\u1ED3m "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `Chu\u1ED7i kh\xF4ng h\u1EE3p l\u1EC7: ph\u1EA3i kh\u1EDBp v\u1EDBi m\u1EABu ${_issue.pattern}`;
        return `${FormatDictionary[_issue.format] ?? issue2.format} kh\xF4ng h\u1EE3p l\u1EC7`;
      }
      case "not_multiple_of":
        return `S\u1ED1 kh\xF4ng h\u1EE3p l\u1EC7: ph\u1EA3i l\xE0 b\u1ED9i s\u1ED1 c\u1EE7a ${issue2.divisor}`;
      case "unrecognized_keys":
        return `Kh\xF3a kh\xF4ng \u0111\u01B0\u1EE3c nh\u1EADn d\u1EA1ng: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `Kh\xF3a kh\xF4ng h\u1EE3p l\u1EC7 trong ${issue2.origin}`;
      case "invalid_union":
        return "\u0110\u1EA7u v\xE0o kh\xF4ng h\u1EE3p l\u1EC7";
      case "invalid_element":
        return `Gi\xE1 tr\u1ECB kh\xF4ng h\u1EE3p l\u1EC7 trong ${issue2.origin}`;
      default:
        return `\u0110\u1EA7u v\xE0o kh\xF4ng h\u1EE3p l\u1EC7`;
    }
  };
};
function vi_default() {
  return {
    localeError: error47()
  };
}

// node_modules/zod/v4/locales/zh-CN.js
var error48 = () => {
  const Sizable = {
    string: { unit: "\u5B57\u7B26", verb: "\u5305\u542B" },
    file: { unit: "\u5B57\u8282", verb: "\u5305\u542B" },
    array: { unit: "\u9879", verb: "\u5305\u542B" },
    set: { unit: "\u9879", verb: "\u5305\u542B" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u8F93\u5165",
    email: "\u7535\u5B50\u90AE\u4EF6",
    url: "URL",
    emoji: "\u8868\u60C5\u7B26\u53F7",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO\u65E5\u671F\u65F6\u95F4",
    date: "ISO\u65E5\u671F",
    time: "ISO\u65F6\u95F4",
    duration: "ISO\u65F6\u957F",
    ipv4: "IPv4\u5730\u5740",
    ipv6: "IPv6\u5730\u5740",
    cidrv4: "IPv4\u7F51\u6BB5",
    cidrv6: "IPv6\u7F51\u6BB5",
    base64: "base64\u7F16\u7801\u5B57\u7B26\u4E32",
    base64url: "base64url\u7F16\u7801\u5B57\u7B26\u4E32",
    json_string: "JSON\u5B57\u7B26\u4E32",
    e164: "E.164\u53F7\u7801",
    jwt: "JWT",
    template_literal: "\u8F93\u5165"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "\u6570\u5B57",
    array: "\u6570\u7EC4",
    null: "\u7A7A\u503C(null)"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u65E0\u6548\u8F93\u5165\uFF1A\u671F\u671B instanceof ${issue2.expected}\uFF0C\u5B9E\u9645\u63A5\u6536 ${received}`;
        }
        return `\u65E0\u6548\u8F93\u5165\uFF1A\u671F\u671B ${expected}\uFF0C\u5B9E\u9645\u63A5\u6536 ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u65E0\u6548\u8F93\u5165\uFF1A\u671F\u671B ${stringifyPrimitive(issue2.values[0])}`;
        return `\u65E0\u6548\u9009\u9879\uFF1A\u671F\u671B\u4EE5\u4E0B\u4E4B\u4E00 ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u6570\u503C\u8FC7\u5927\uFF1A\u671F\u671B ${issue2.origin ?? "\u503C"} ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u4E2A\u5143\u7D20"}`;
        return `\u6570\u503C\u8FC7\u5927\uFF1A\u671F\u671B ${issue2.origin ?? "\u503C"} ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u6570\u503C\u8FC7\u5C0F\uFF1A\u671F\u671B ${issue2.origin} ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u6570\u503C\u8FC7\u5C0F\uFF1A\u671F\u671B ${issue2.origin} ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u65E0\u6548\u5B57\u7B26\u4E32\uFF1A\u5FC5\u987B\u4EE5 "${_issue.prefix}" \u5F00\u5934`;
        if (_issue.format === "ends_with")
          return `\u65E0\u6548\u5B57\u7B26\u4E32\uFF1A\u5FC5\u987B\u4EE5 "${_issue.suffix}" \u7ED3\u5C3E`;
        if (_issue.format === "includes")
          return `\u65E0\u6548\u5B57\u7B26\u4E32\uFF1A\u5FC5\u987B\u5305\u542B "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u65E0\u6548\u5B57\u7B26\u4E32\uFF1A\u5FC5\u987B\u6EE1\u8DB3\u6B63\u5219\u8868\u8FBE\u5F0F ${_issue.pattern}`;
        return `\u65E0\u6548${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u65E0\u6548\u6570\u5B57\uFF1A\u5FC5\u987B\u662F ${issue2.divisor} \u7684\u500D\u6570`;
      case "unrecognized_keys":
        return `\u51FA\u73B0\u672A\u77E5\u7684\u952E(key): ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `${issue2.origin} \u4E2D\u7684\u952E(key)\u65E0\u6548`;
      case "invalid_union":
        return "\u65E0\u6548\u8F93\u5165";
      case "invalid_element":
        return `${issue2.origin} \u4E2D\u5305\u542B\u65E0\u6548\u503C(value)`;
      default:
        return `\u65E0\u6548\u8F93\u5165`;
    }
  };
};
function zh_CN_default() {
  return {
    localeError: error48()
  };
}

// node_modules/zod/v4/locales/zh-TW.js
var error49 = () => {
  const Sizable = {
    string: { unit: "\u5B57\u5143", verb: "\u64C1\u6709" },
    file: { unit: "\u4F4D\u5143\u7D44", verb: "\u64C1\u6709" },
    array: { unit: "\u9805\u76EE", verb: "\u64C1\u6709" },
    set: { unit: "\u9805\u76EE", verb: "\u64C1\u6709" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u8F38\u5165",
    email: "\u90F5\u4EF6\u5730\u5740",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "ISO \u65E5\u671F\u6642\u9593",
    date: "ISO \u65E5\u671F",
    time: "ISO \u6642\u9593",
    duration: "ISO \u671F\u9593",
    ipv4: "IPv4 \u4F4D\u5740",
    ipv6: "IPv6 \u4F4D\u5740",
    cidrv4: "IPv4 \u7BC4\u570D",
    cidrv6: "IPv6 \u7BC4\u570D",
    base64: "base64 \u7DE8\u78BC\u5B57\u4E32",
    base64url: "base64url \u7DE8\u78BC\u5B57\u4E32",
    json_string: "JSON \u5B57\u4E32",
    e164: "E.164 \u6578\u503C",
    jwt: "JWT",
    template_literal: "\u8F38\u5165"
  };
  const TypeDictionary = {
    nan: "NaN"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\u7121\u6548\u7684\u8F38\u5165\u503C\uFF1A\u9810\u671F\u70BA instanceof ${issue2.expected}\uFF0C\u4F46\u6536\u5230 ${received}`;
        }
        return `\u7121\u6548\u7684\u8F38\u5165\u503C\uFF1A\u9810\u671F\u70BA ${expected}\uFF0C\u4F46\u6536\u5230 ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\u7121\u6548\u7684\u8F38\u5165\u503C\uFF1A\u9810\u671F\u70BA ${stringifyPrimitive(issue2.values[0])}`;
        return `\u7121\u6548\u7684\u9078\u9805\uFF1A\u9810\u671F\u70BA\u4EE5\u4E0B\u5176\u4E2D\u4E4B\u4E00 ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `\u6578\u503C\u904E\u5927\uFF1A\u9810\u671F ${issue2.origin ?? "\u503C"} \u61C9\u70BA ${adj}${issue2.maximum.toString()} ${sizing.unit ?? "\u500B\u5143\u7D20"}`;
        return `\u6578\u503C\u904E\u5927\uFF1A\u9810\u671F ${issue2.origin ?? "\u503C"} \u61C9\u70BA ${adj}${issue2.maximum.toString()}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing) {
          return `\u6578\u503C\u904E\u5C0F\uFF1A\u9810\u671F ${issue2.origin} \u61C9\u70BA ${adj}${issue2.minimum.toString()} ${sizing.unit}`;
        }
        return `\u6578\u503C\u904E\u5C0F\uFF1A\u9810\u671F ${issue2.origin} \u61C9\u70BA ${adj}${issue2.minimum.toString()}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with") {
          return `\u7121\u6548\u7684\u5B57\u4E32\uFF1A\u5FC5\u9808\u4EE5 "${_issue.prefix}" \u958B\u982D`;
        }
        if (_issue.format === "ends_with")
          return `\u7121\u6548\u7684\u5B57\u4E32\uFF1A\u5FC5\u9808\u4EE5 "${_issue.suffix}" \u7D50\u5C3E`;
        if (_issue.format === "includes")
          return `\u7121\u6548\u7684\u5B57\u4E32\uFF1A\u5FC5\u9808\u5305\u542B "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u7121\u6548\u7684\u5B57\u4E32\uFF1A\u5FC5\u9808\u7B26\u5408\u683C\u5F0F ${_issue.pattern}`;
        return `\u7121\u6548\u7684 ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `\u7121\u6548\u7684\u6578\u5B57\uFF1A\u5FC5\u9808\u70BA ${issue2.divisor} \u7684\u500D\u6578`;
      case "unrecognized_keys":
        return `\u7121\u6CD5\u8B58\u5225\u7684\u9375\u503C${issue2.keys.length > 1 ? "\u5011" : ""}\uFF1A${joinValues(issue2.keys, "\u3001")}`;
      case "invalid_key":
        return `${issue2.origin} \u4E2D\u6709\u7121\u6548\u7684\u9375\u503C`;
      case "invalid_union":
        return "\u7121\u6548\u7684\u8F38\u5165\u503C";
      case "invalid_element":
        return `${issue2.origin} \u4E2D\u6709\u7121\u6548\u7684\u503C`;
      default:
        return `\u7121\u6548\u7684\u8F38\u5165\u503C`;
    }
  };
};
function zh_TW_default() {
  return {
    localeError: error49()
  };
}

// node_modules/zod/v4/locales/yo.js
var error50 = () => {
  const Sizable = {
    string: { unit: "\xE0mi", verb: "n\xED" },
    file: { unit: "bytes", verb: "n\xED" },
    array: { unit: "nkan", verb: "n\xED" },
    set: { unit: "nkan", verb: "n\xED" }
  };
  function getSizing(origin) {
    return Sizable[origin] ?? null;
  }
  const FormatDictionary = {
    regex: "\u1EB9\u0300r\u1ECD \xECb\xE1w\u1ECDl\xE9",
    email: "\xE0d\xEDr\u1EB9\u0301s\xEC \xECm\u1EB9\u0301l\xEC",
    url: "URL",
    emoji: "emoji",
    uuid: "UUID",
    uuidv4: "UUIDv4",
    uuidv6: "UUIDv6",
    nanoid: "nanoid",
    guid: "GUID",
    cuid: "cuid",
    cuid2: "cuid2",
    ulid: "ULID",
    xid: "XID",
    ksuid: "KSUID",
    datetime: "\xE0k\xF3k\xF2 ISO",
    date: "\u1ECDj\u1ECD\u0301 ISO",
    time: "\xE0k\xF3k\xF2 ISO",
    duration: "\xE0k\xF3k\xF2 t\xF3 p\xE9 ISO",
    ipv4: "\xE0d\xEDr\u1EB9\u0301s\xEC IPv4",
    ipv6: "\xE0d\xEDr\u1EB9\u0301s\xEC IPv6",
    cidrv4: "\xE0gb\xE8gb\xE8 IPv4",
    cidrv6: "\xE0gb\xE8gb\xE8 IPv6",
    base64: "\u1ECD\u0300r\u1ECD\u0300 t\xED a k\u1ECD\u0301 n\xED base64",
    base64url: "\u1ECD\u0300r\u1ECD\u0300 base64url",
    json_string: "\u1ECD\u0300r\u1ECD\u0300 JSON",
    e164: "n\u1ECD\u0301mb\xE0 E.164",
    jwt: "JWT",
    template_literal: "\u1EB9\u0300r\u1ECD \xECb\xE1w\u1ECDl\xE9"
  };
  const TypeDictionary = {
    nan: "NaN",
    number: "n\u1ECD\u0301mb\xE0",
    array: "akop\u1ECD"
  };
  return (issue2) => {
    switch (issue2.code) {
      case "invalid_type": {
        const expected = TypeDictionary[issue2.expected] ?? issue2.expected;
        const receivedType = parsedType(issue2.input);
        const received = TypeDictionary[receivedType] ?? receivedType;
        if (/^[A-Z]/.test(issue2.expected)) {
          return `\xCCb\xE1w\u1ECDl\xE9 a\u1E63\xEC\u1E63e: a n\xED l\xE1ti fi instanceof ${issue2.expected}, \xE0m\u1ECD\u0300 a r\xED ${received}`;
        }
        return `\xCCb\xE1w\u1ECDl\xE9 a\u1E63\xEC\u1E63e: a n\xED l\xE1ti fi ${expected}, \xE0m\u1ECD\u0300 a r\xED ${received}`;
      }
      case "invalid_value":
        if (issue2.values.length === 1)
          return `\xCCb\xE1w\u1ECDl\xE9 a\u1E63\xEC\u1E63e: a n\xED l\xE1ti fi ${stringifyPrimitive(issue2.values[0])}`;
        return `\xC0\u1E63\xE0y\xE0n a\u1E63\xEC\u1E63e: yan \u1ECD\u0300kan l\xE1ra ${joinValues(issue2.values, "|")}`;
      case "too_big": {
        const adj = issue2.inclusive ? "<=" : "<";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `T\xF3 p\u1ECD\u0300 j\xF9: a n\xED l\xE1ti j\u1EB9\u0301 p\xE9 ${issue2.origin ?? "iye"} ${sizing.verb} ${adj}${issue2.maximum} ${sizing.unit}`;
        return `T\xF3 p\u1ECD\u0300 j\xF9: a n\xED l\xE1ti j\u1EB9\u0301 ${adj}${issue2.maximum}`;
      }
      case "too_small": {
        const adj = issue2.inclusive ? ">=" : ">";
        const sizing = getSizing(issue2.origin);
        if (sizing)
          return `K\xE9r\xE9 ju: a n\xED l\xE1ti j\u1EB9\u0301 p\xE9 ${issue2.origin} ${sizing.verb} ${adj}${issue2.minimum} ${sizing.unit}`;
        return `K\xE9r\xE9 ju: a n\xED l\xE1ti j\u1EB9\u0301 ${adj}${issue2.minimum}`;
      }
      case "invalid_format": {
        const _issue = issue2;
        if (_issue.format === "starts_with")
          return `\u1ECC\u0300r\u1ECD\u0300 a\u1E63\xEC\u1E63e: gb\u1ECD\u0301d\u1ECD\u0300 b\u1EB9\u0300r\u1EB9\u0300 p\u1EB9\u0300l\xFA "${_issue.prefix}"`;
        if (_issue.format === "ends_with")
          return `\u1ECC\u0300r\u1ECD\u0300 a\u1E63\xEC\u1E63e: gb\u1ECD\u0301d\u1ECD\u0300 par\xED p\u1EB9\u0300l\xFA "${_issue.suffix}"`;
        if (_issue.format === "includes")
          return `\u1ECC\u0300r\u1ECD\u0300 a\u1E63\xEC\u1E63e: gb\u1ECD\u0301d\u1ECD\u0300 n\xED "${_issue.includes}"`;
        if (_issue.format === "regex")
          return `\u1ECC\u0300r\u1ECD\u0300 a\u1E63\xEC\u1E63e: gb\u1ECD\u0301d\u1ECD\u0300 b\xE1 \xE0p\u1EB9\u1EB9r\u1EB9 mu ${_issue.pattern}`;
        return `A\u1E63\xEC\u1E63e: ${FormatDictionary[_issue.format] ?? issue2.format}`;
      }
      case "not_multiple_of":
        return `N\u1ECD\u0301mb\xE0 a\u1E63\xEC\u1E63e: gb\u1ECD\u0301d\u1ECD\u0300 j\u1EB9\u0301 \xE8y\xE0 p\xEDp\xEDn ti ${issue2.divisor}`;
      case "unrecognized_keys":
        return `B\u1ECDt\xECn\xEC \xE0\xECm\u1ECD\u0300: ${joinValues(issue2.keys, ", ")}`;
      case "invalid_key":
        return `B\u1ECDt\xECn\xEC a\u1E63\xEC\u1E63e n\xEDn\xFA ${issue2.origin}`;
      case "invalid_union":
        return "\xCCb\xE1w\u1ECDl\xE9 a\u1E63\xEC\u1E63e";
      case "invalid_element":
        return `Iye a\u1E63\xEC\u1E63e n\xEDn\xFA ${issue2.origin}`;
      default:
        return "\xCCb\xE1w\u1ECDl\xE9 a\u1E63\xEC\u1E63e";
    }
  };
};
function yo_default() {
  return {
    localeError: error50()
  };
}

// node_modules/zod/v4/core/registries.js
var _a2;
var $output = Symbol("ZodOutput");
var $input = Symbol("ZodInput");
var $ZodRegistry = class {
  constructor() {
    this._map = /* @__PURE__ */ new WeakMap();
    this._idmap = /* @__PURE__ */ new Map();
  }
  add(schema, ..._meta) {
    const meta3 = _meta[0];
    this._map.set(schema, meta3);
    if (meta3 && typeof meta3 === "object" && "id" in meta3) {
      this._idmap.set(meta3.id, schema);
    }
    return this;
  }
  clear() {
    this._map = /* @__PURE__ */ new WeakMap();
    this._idmap = /* @__PURE__ */ new Map();
    return this;
  }
  remove(schema) {
    const meta3 = this._map.get(schema);
    if (meta3 && typeof meta3 === "object" && "id" in meta3) {
      this._idmap.delete(meta3.id);
    }
    this._map.delete(schema);
    return this;
  }
  get(schema) {
    const p = schema._zod.parent;
    if (p) {
      const pm = { ...this.get(p) ?? {} };
      delete pm.id;
      const f = { ...pm, ...this._map.get(schema) };
      return Object.keys(f).length ? f : void 0;
    }
    return this._map.get(schema);
  }
  has(schema) {
    return this._map.has(schema);
  }
};
function registry() {
  return new $ZodRegistry();
}
(_a2 = globalThis).__zod_globalRegistry ?? (_a2.__zod_globalRegistry = registry());
var globalRegistry = globalThis.__zod_globalRegistry;

// node_modules/zod/v4/core/api.js
// @__NO_SIDE_EFFECTS__
function _string(Class2, params) {
  return new Class2({
    type: "string",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _coercedString(Class2, params) {
  return new Class2({
    type: "string",
    coerce: true,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _email(Class2, params) {
  return new Class2({
    type: "string",
    format: "email",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _guid(Class2, params) {
  return new Class2({
    type: "string",
    format: "guid",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _uuid(Class2, params) {
  return new Class2({
    type: "string",
    format: "uuid",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _uuidv4(Class2, params) {
  return new Class2({
    type: "string",
    format: "uuid",
    check: "string_format",
    abort: false,
    version: "v4",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _uuidv6(Class2, params) {
  return new Class2({
    type: "string",
    format: "uuid",
    check: "string_format",
    abort: false,
    version: "v6",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _uuidv7(Class2, params) {
  return new Class2({
    type: "string",
    format: "uuid",
    check: "string_format",
    abort: false,
    version: "v7",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _url(Class2, params) {
  return new Class2({
    type: "string",
    format: "url",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _emoji2(Class2, params) {
  return new Class2({
    type: "string",
    format: "emoji",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _nanoid(Class2, params) {
  return new Class2({
    type: "string",
    format: "nanoid",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _cuid(Class2, params) {
  return new Class2({
    type: "string",
    format: "cuid",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _cuid2(Class2, params) {
  return new Class2({
    type: "string",
    format: "cuid2",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _ulid(Class2, params) {
  return new Class2({
    type: "string",
    format: "ulid",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _xid(Class2, params) {
  return new Class2({
    type: "string",
    format: "xid",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _ksuid(Class2, params) {
  return new Class2({
    type: "string",
    format: "ksuid",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _ipv4(Class2, params) {
  return new Class2({
    type: "string",
    format: "ipv4",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _ipv6(Class2, params) {
  return new Class2({
    type: "string",
    format: "ipv6",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _mac(Class2, params) {
  return new Class2({
    type: "string",
    format: "mac",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _cidrv4(Class2, params) {
  return new Class2({
    type: "string",
    format: "cidrv4",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _cidrv6(Class2, params) {
  return new Class2({
    type: "string",
    format: "cidrv6",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _base64(Class2, params) {
  return new Class2({
    type: "string",
    format: "base64",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _base64url(Class2, params) {
  return new Class2({
    type: "string",
    format: "base64url",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _e164(Class2, params) {
  return new Class2({
    type: "string",
    format: "e164",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _jwt(Class2, params) {
  return new Class2({
    type: "string",
    format: "jwt",
    check: "string_format",
    abort: false,
    ...normalizeParams(params)
  });
}
var TimePrecision = {
  Any: null,
  Minute: -1,
  Second: 0,
  Millisecond: 3,
  Microsecond: 6
};
// @__NO_SIDE_EFFECTS__
function _isoDateTime(Class2, params) {
  return new Class2({
    type: "string",
    format: "datetime",
    check: "string_format",
    offset: false,
    local: false,
    precision: null,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _isoDate(Class2, params) {
  return new Class2({
    type: "string",
    format: "date",
    check: "string_format",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _isoTime(Class2, params) {
  return new Class2({
    type: "string",
    format: "time",
    check: "string_format",
    precision: null,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _isoDuration(Class2, params) {
  return new Class2({
    type: "string",
    format: "duration",
    check: "string_format",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _number(Class2, params) {
  return new Class2({
    type: "number",
    checks: [],
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _coercedNumber(Class2, params) {
  return new Class2({
    type: "number",
    coerce: true,
    checks: [],
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _int(Class2, params) {
  return new Class2({
    type: "number",
    check: "number_format",
    abort: false,
    format: "safeint",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _float32(Class2, params) {
  return new Class2({
    type: "number",
    check: "number_format",
    abort: false,
    format: "float32",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _float64(Class2, params) {
  return new Class2({
    type: "number",
    check: "number_format",
    abort: false,
    format: "float64",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _int32(Class2, params) {
  return new Class2({
    type: "number",
    check: "number_format",
    abort: false,
    format: "int32",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _uint32(Class2, params) {
  return new Class2({
    type: "number",
    check: "number_format",
    abort: false,
    format: "uint32",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _boolean(Class2, params) {
  return new Class2({
    type: "boolean",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _coercedBoolean(Class2, params) {
  return new Class2({
    type: "boolean",
    coerce: true,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _bigint(Class2, params) {
  return new Class2({
    type: "bigint",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _coercedBigint(Class2, params) {
  return new Class2({
    type: "bigint",
    coerce: true,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _int64(Class2, params) {
  return new Class2({
    type: "bigint",
    check: "bigint_format",
    abort: false,
    format: "int64",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _uint64(Class2, params) {
  return new Class2({
    type: "bigint",
    check: "bigint_format",
    abort: false,
    format: "uint64",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _symbol(Class2, params) {
  return new Class2({
    type: "symbol",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _undefined2(Class2, params) {
  return new Class2({
    type: "undefined",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _null2(Class2, params) {
  return new Class2({
    type: "null",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _any(Class2) {
  return new Class2({
    type: "any"
  });
}
// @__NO_SIDE_EFFECTS__
function _unknown(Class2) {
  return new Class2({
    type: "unknown"
  });
}
// @__NO_SIDE_EFFECTS__
function _never(Class2, params) {
  return new Class2({
    type: "never",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _void(Class2, params) {
  return new Class2({
    type: "void",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _date(Class2, params) {
  return new Class2({
    type: "date",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _coercedDate(Class2, params) {
  return new Class2({
    type: "date",
    coerce: true,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _nan(Class2, params) {
  return new Class2({
    type: "nan",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _lt(value, params) {
  return new $ZodCheckLessThan({
    check: "less_than",
    ...normalizeParams(params),
    value,
    inclusive: false
  });
}
// @__NO_SIDE_EFFECTS__
function _lte(value, params) {
  return new $ZodCheckLessThan({
    check: "less_than",
    ...normalizeParams(params),
    value,
    inclusive: true
  });
}
// @__NO_SIDE_EFFECTS__
function _gt(value, params) {
  return new $ZodCheckGreaterThan({
    check: "greater_than",
    ...normalizeParams(params),
    value,
    inclusive: false
  });
}
// @__NO_SIDE_EFFECTS__
function _gte(value, params) {
  return new $ZodCheckGreaterThan({
    check: "greater_than",
    ...normalizeParams(params),
    value,
    inclusive: true
  });
}
// @__NO_SIDE_EFFECTS__
function _positive(params) {
  return /* @__PURE__ */ _gt(0, params);
}
// @__NO_SIDE_EFFECTS__
function _negative(params) {
  return /* @__PURE__ */ _lt(0, params);
}
// @__NO_SIDE_EFFECTS__
function _nonpositive(params) {
  return /* @__PURE__ */ _lte(0, params);
}
// @__NO_SIDE_EFFECTS__
function _nonnegative(params) {
  return /* @__PURE__ */ _gte(0, params);
}
// @__NO_SIDE_EFFECTS__
function _multipleOf(value, params) {
  return new $ZodCheckMultipleOf({
    check: "multiple_of",
    ...normalizeParams(params),
    value
  });
}
// @__NO_SIDE_EFFECTS__
function _maxSize(maximum, params) {
  return new $ZodCheckMaxSize({
    check: "max_size",
    ...normalizeParams(params),
    maximum
  });
}
// @__NO_SIDE_EFFECTS__
function _minSize(minimum, params) {
  return new $ZodCheckMinSize({
    check: "min_size",
    ...normalizeParams(params),
    minimum
  });
}
// @__NO_SIDE_EFFECTS__
function _size(size, params) {
  return new $ZodCheckSizeEquals({
    check: "size_equals",
    ...normalizeParams(params),
    size
  });
}
// @__NO_SIDE_EFFECTS__
function _maxLength(maximum, params) {
  const ch = new $ZodCheckMaxLength({
    check: "max_length",
    ...normalizeParams(params),
    maximum
  });
  return ch;
}
// @__NO_SIDE_EFFECTS__
function _minLength(minimum, params) {
  return new $ZodCheckMinLength({
    check: "min_length",
    ...normalizeParams(params),
    minimum
  });
}
// @__NO_SIDE_EFFECTS__
function _length(length, params) {
  return new $ZodCheckLengthEquals({
    check: "length_equals",
    ...normalizeParams(params),
    length
  });
}
// @__NO_SIDE_EFFECTS__
function _regex(pattern, params) {
  return new $ZodCheckRegex({
    check: "string_format",
    format: "regex",
    ...normalizeParams(params),
    pattern
  });
}
// @__NO_SIDE_EFFECTS__
function _lowercase(params) {
  return new $ZodCheckLowerCase({
    check: "string_format",
    format: "lowercase",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _uppercase(params) {
  return new $ZodCheckUpperCase({
    check: "string_format",
    format: "uppercase",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _includes(includes, params) {
  return new $ZodCheckIncludes({
    check: "string_format",
    format: "includes",
    ...normalizeParams(params),
    includes
  });
}
// @__NO_SIDE_EFFECTS__
function _startsWith(prefix, params) {
  return new $ZodCheckStartsWith({
    check: "string_format",
    format: "starts_with",
    ...normalizeParams(params),
    prefix
  });
}
// @__NO_SIDE_EFFECTS__
function _endsWith(suffix, params) {
  return new $ZodCheckEndsWith({
    check: "string_format",
    format: "ends_with",
    ...normalizeParams(params),
    suffix
  });
}
// @__NO_SIDE_EFFECTS__
function _property(property, schema, params) {
  return new $ZodCheckProperty({
    check: "property",
    property,
    schema,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _mime(types, params) {
  return new $ZodCheckMimeType({
    check: "mime_type",
    mime: types,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _overwrite(tx) {
  return new $ZodCheckOverwrite({
    check: "overwrite",
    tx
  });
}
// @__NO_SIDE_EFFECTS__
function _normalize(form) {
  return /* @__PURE__ */ _overwrite((input) => input.normalize(form));
}
// @__NO_SIDE_EFFECTS__
function _trim() {
  return /* @__PURE__ */ _overwrite((input) => input.trim());
}
// @__NO_SIDE_EFFECTS__
function _toLowerCase() {
  return /* @__PURE__ */ _overwrite((input) => input.toLowerCase());
}
// @__NO_SIDE_EFFECTS__
function _toUpperCase() {
  return /* @__PURE__ */ _overwrite((input) => input.toUpperCase());
}
// @__NO_SIDE_EFFECTS__
function _slugify() {
  return /* @__PURE__ */ _overwrite((input) => slugify(input));
}
// @__NO_SIDE_EFFECTS__
function _array(Class2, element, params) {
  return new Class2({
    type: "array",
    element,
    // get element() {
    //   return element;
    // },
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _union(Class2, options, params) {
  return new Class2({
    type: "union",
    options,
    ...normalizeParams(params)
  });
}
function _xor(Class2, options, params) {
  return new Class2({
    type: "union",
    options,
    inclusive: false,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _discriminatedUnion(Class2, discriminator, options, params) {
  return new Class2({
    type: "union",
    options,
    discriminator,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _intersection(Class2, left, right) {
  return new Class2({
    type: "intersection",
    left,
    right
  });
}
// @__NO_SIDE_EFFECTS__
function _tuple(Class2, items, _paramsOrRest, _params) {
  const hasRest = _paramsOrRest instanceof $ZodType;
  const params = hasRest ? _params : _paramsOrRest;
  const rest = hasRest ? _paramsOrRest : null;
  return new Class2({
    type: "tuple",
    items,
    rest,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _record(Class2, keyType, valueType, params) {
  return new Class2({
    type: "record",
    keyType,
    valueType,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _map(Class2, keyType, valueType, params) {
  return new Class2({
    type: "map",
    keyType,
    valueType,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _set(Class2, valueType, params) {
  return new Class2({
    type: "set",
    valueType,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _enum(Class2, values, params) {
  const entries = Array.isArray(values) ? Object.fromEntries(values.map((v) => [v, v])) : values;
  return new Class2({
    type: "enum",
    entries,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _nativeEnum(Class2, entries, params) {
  return new Class2({
    type: "enum",
    entries,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _literal(Class2, value, params) {
  return new Class2({
    type: "literal",
    values: Array.isArray(value) ? value : [value],
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _file(Class2, params) {
  return new Class2({
    type: "file",
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _transform(Class2, fn) {
  return new Class2({
    type: "transform",
    transform: fn
  });
}
// @__NO_SIDE_EFFECTS__
function _optional(Class2, innerType) {
  return new Class2({
    type: "optional",
    innerType
  });
}
// @__NO_SIDE_EFFECTS__
function _nullable(Class2, innerType) {
  return new Class2({
    type: "nullable",
    innerType
  });
}
// @__NO_SIDE_EFFECTS__
function _default(Class2, innerType, defaultValue) {
  return new Class2({
    type: "default",
    innerType,
    get defaultValue() {
      return typeof defaultValue === "function" ? defaultValue() : shallowClone(defaultValue);
    }
  });
}
// @__NO_SIDE_EFFECTS__
function _nonoptional(Class2, innerType, params) {
  return new Class2({
    type: "nonoptional",
    innerType,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _success(Class2, innerType) {
  return new Class2({
    type: "success",
    innerType
  });
}
// @__NO_SIDE_EFFECTS__
function _catch(Class2, innerType, catchValue) {
  return new Class2({
    type: "catch",
    innerType,
    catchValue: typeof catchValue === "function" ? catchValue : () => catchValue
  });
}
// @__NO_SIDE_EFFECTS__
function _pipe(Class2, in_, out) {
  return new Class2({
    type: "pipe",
    in: in_,
    out
  });
}
// @__NO_SIDE_EFFECTS__
function _readonly(Class2, innerType) {
  return new Class2({
    type: "readonly",
    innerType
  });
}
// @__NO_SIDE_EFFECTS__
function _templateLiteral(Class2, parts, params) {
  return new Class2({
    type: "template_literal",
    parts,
    ...normalizeParams(params)
  });
}
// @__NO_SIDE_EFFECTS__
function _lazy(Class2, getter) {
  return new Class2({
    type: "lazy",
    getter
  });
}
// @__NO_SIDE_EFFECTS__
function _promise(Class2, innerType) {
  return new Class2({
    type: "promise",
    innerType
  });
}
// @__NO_SIDE_EFFECTS__
function _custom(Class2, fn, _params) {
  const norm = normalizeParams(_params);
  norm.abort ?? (norm.abort = true);
  const schema = new Class2({
    type: "custom",
    check: "custom",
    fn,
    ...norm
  });
  return schema;
}
// @__NO_SIDE_EFFECTS__
function _refine(Class2, fn, _params) {
  const schema = new Class2({
    type: "custom",
    check: "custom",
    fn,
    ...normalizeParams(_params)
  });
  return schema;
}
// @__NO_SIDE_EFFECTS__
function _superRefine(fn, params) {
  const ch = /* @__PURE__ */ _check((payload) => {
    payload.addIssue = (issue2) => {
      if (typeof issue2 === "string") {
        payload.issues.push(issue(issue2, payload.value, ch._zod.def));
      } else {
        const _issue = issue2;
        if (_issue.fatal)
          _issue.continue = false;
        _issue.code ?? (_issue.code = "custom");
        _issue.input ?? (_issue.input = payload.value);
        _issue.inst ?? (_issue.inst = ch);
        _issue.continue ?? (_issue.continue = !ch._zod.def.abort);
        payload.issues.push(issue(_issue));
      }
    };
    return fn(payload.value, payload);
  }, params);
  return ch;
}
// @__NO_SIDE_EFFECTS__
function _check(fn, params) {
  const ch = new $ZodCheck({
    check: "custom",
    ...normalizeParams(params)
  });
  ch._zod.check = fn;
  return ch;
}
// @__NO_SIDE_EFFECTS__
function describe(description) {
  const ch = new $ZodCheck({ check: "describe" });
  ch._zod.onattach = [
    (inst) => {
      const existing = globalRegistry.get(inst) ?? {};
      globalRegistry.add(inst, { ...existing, description });
    }
  ];
  ch._zod.check = () => {
  };
  return ch;
}
// @__NO_SIDE_EFFECTS__
function meta(metadata) {
  const ch = new $ZodCheck({ check: "meta" });
  ch._zod.onattach = [
    (inst) => {
      const existing = globalRegistry.get(inst) ?? {};
      globalRegistry.add(inst, { ...existing, ...metadata });
    }
  ];
  ch._zod.check = () => {
  };
  return ch;
}
// @__NO_SIDE_EFFECTS__
function _stringbool(Classes, _params) {
  const params = normalizeParams(_params);
  let truthyArray = params.truthy ?? ["true", "1", "yes", "on", "y", "enabled"];
  let falsyArray = params.falsy ?? ["false", "0", "no", "off", "n", "disabled"];
  if (params.case !== "sensitive") {
    truthyArray = truthyArray.map((v) => typeof v === "string" ? v.toLowerCase() : v);
    falsyArray = falsyArray.map((v) => typeof v === "string" ? v.toLowerCase() : v);
  }
  const truthySet = new Set(truthyArray);
  const falsySet = new Set(falsyArray);
  const _Codec = Classes.Codec ?? $ZodCodec;
  const _Boolean = Classes.Boolean ?? $ZodBoolean;
  const _String = Classes.String ?? $ZodString;
  const stringSchema = new _String({ type: "string", error: params.error });
  const booleanSchema = new _Boolean({ type: "boolean", error: params.error });
  const codec2 = new _Codec({
    type: "pipe",
    in: stringSchema,
    out: booleanSchema,
    transform: ((input, payload) => {
      let data = input;
      if (params.case !== "sensitive")
        data = data.toLowerCase();
      if (truthySet.has(data)) {
        return true;
      } else if (falsySet.has(data)) {
        return false;
      } else {
        payload.issues.push({
          code: "invalid_value",
          expected: "stringbool",
          values: [...truthySet, ...falsySet],
          input: payload.value,
          inst: codec2,
          continue: false
        });
        return {};
      }
    }),
    reverseTransform: ((input, _payload) => {
      if (input === true) {
        return truthyArray[0] || "true";
      } else {
        return falsyArray[0] || "false";
      }
    }),
    error: params.error
  });
  return codec2;
}
// @__NO_SIDE_EFFECTS__
function _stringFormat(Class2, format, fnOrRegex, _params = {}) {
  const params = normalizeParams(_params);
  const def = {
    ...normalizeParams(_params),
    check: "string_format",
    type: "string",
    format,
    fn: typeof fnOrRegex === "function" ? fnOrRegex : (val) => fnOrRegex.test(val),
    ...params
  };
  if (fnOrRegex instanceof RegExp) {
    def.pattern = fnOrRegex;
  }
  const inst = new Class2(def);
  return inst;
}

// node_modules/zod/v4/core/to-json-schema.js
function initializeContext(params) {
  let target = params?.target ?? "draft-2020-12";
  if (target === "draft-4")
    target = "draft-04";
  if (target === "draft-7")
    target = "draft-07";
  return {
    processors: params.processors ?? {},
    metadataRegistry: params?.metadata ?? globalRegistry,
    target,
    unrepresentable: params?.unrepresentable ?? "throw",
    override: params?.override ?? (() => {
    }),
    io: params?.io ?? "output",
    counter: 0,
    seen: /* @__PURE__ */ new Map(),
    cycles: params?.cycles ?? "ref",
    reused: params?.reused ?? "inline",
    external: params?.external ?? void 0
  };
}
function process2(schema, ctx, _params = { path: [], schemaPath: [] }) {
  var _a3;
  const def = schema._zod.def;
  const seen = ctx.seen.get(schema);
  if (seen) {
    seen.count++;
    const isCycle = _params.schemaPath.includes(schema);
    if (isCycle) {
      seen.cycle = _params.path;
    }
    return seen.schema;
  }
  const result = { schema: {}, count: 1, cycle: void 0, path: _params.path };
  ctx.seen.set(schema, result);
  const overrideSchema = schema._zod.toJSONSchema?.();
  if (overrideSchema) {
    result.schema = overrideSchema;
  } else {
    const params = {
      ..._params,
      schemaPath: [..._params.schemaPath, schema],
      path: _params.path
    };
    if (schema._zod.processJSONSchema) {
      schema._zod.processJSONSchema(ctx, result.schema, params);
    } else {
      const _json = result.schema;
      const processor = ctx.processors[def.type];
      if (!processor) {
        throw new Error(`[toJSONSchema]: Non-representable type encountered: ${def.type}`);
      }
      processor(schema, ctx, _json, params);
    }
    const parent = schema._zod.parent;
    if (parent) {
      if (!result.ref)
        result.ref = parent;
      process2(parent, ctx, params);
      ctx.seen.get(parent).isParent = true;
    }
  }
  const meta3 = ctx.metadataRegistry.get(schema);
  if (meta3)
    Object.assign(result.schema, meta3);
  if (ctx.io === "input" && isTransforming(schema)) {
    delete result.schema.examples;
    delete result.schema.default;
  }
  if (ctx.io === "input" && "_prefault" in result.schema)
    (_a3 = result.schema).default ?? (_a3.default = result.schema._prefault);
  delete result.schema._prefault;
  const _result = ctx.seen.get(schema);
  return _result.schema;
}
function extractDefs(ctx, schema) {
  const root2 = ctx.seen.get(schema);
  if (!root2)
    throw new Error("Unprocessed schema. This is a bug in Zod.");
  const idToSchema = /* @__PURE__ */ new Map();
  for (const entry of ctx.seen.entries()) {
    const id = ctx.metadataRegistry.get(entry[0])?.id;
    if (id) {
      const existing = idToSchema.get(id);
      if (existing && existing !== entry[0]) {
        throw new Error(`Duplicate schema id "${id}" detected during JSON Schema conversion. Two different schemas cannot share the same id when converted together.`);
      }
      idToSchema.set(id, entry[0]);
    }
  }
  const makeURI = (entry) => {
    const defsSegment = ctx.target === "draft-2020-12" ? "$defs" : "definitions";
    if (ctx.external) {
      const externalId = ctx.external.registry.get(entry[0])?.id;
      const uriGenerator = ctx.external.uri ?? ((id2) => id2);
      if (externalId) {
        return { ref: uriGenerator(externalId) };
      }
      const id = entry[1].defId ?? entry[1].schema.id ?? `schema${ctx.counter++}`;
      entry[1].defId = id;
      return { defId: id, ref: `${uriGenerator("__shared")}#/${defsSegment}/${id}` };
    }
    if (entry[1] === root2) {
      return { ref: "#" };
    }
    const uriPrefix = `#`;
    const defUriPrefix = `${uriPrefix}/${defsSegment}/`;
    const defId = entry[1].schema.id ?? `__schema${ctx.counter++}`;
    return { defId, ref: defUriPrefix + defId };
  };
  const extractToDef = (entry) => {
    if (entry[1].schema.$ref) {
      return;
    }
    const seen = entry[1];
    const { ref, defId } = makeURI(entry);
    seen.def = { ...seen.schema };
    if (defId)
      seen.defId = defId;
    const schema2 = seen.schema;
    for (const key in schema2) {
      delete schema2[key];
    }
    schema2.$ref = ref;
  };
  if (ctx.cycles === "throw") {
    for (const entry of ctx.seen.entries()) {
      const seen = entry[1];
      if (seen.cycle) {
        throw new Error(`Cycle detected: #/${seen.cycle?.join("/")}/<root>

Set the \`cycles\` parameter to \`"ref"\` to resolve cyclical schemas with defs.`);
      }
    }
  }
  for (const entry of ctx.seen.entries()) {
    const seen = entry[1];
    if (schema === entry[0]) {
      extractToDef(entry);
      continue;
    }
    if (ctx.external) {
      const ext = ctx.external.registry.get(entry[0])?.id;
      if (schema !== entry[0] && ext) {
        extractToDef(entry);
        continue;
      }
    }
    const id = ctx.metadataRegistry.get(entry[0])?.id;
    if (id) {
      extractToDef(entry);
      continue;
    }
    if (seen.cycle) {
      extractToDef(entry);
      continue;
    }
    if (seen.count > 1) {
      if (ctx.reused === "ref") {
        extractToDef(entry);
        continue;
      }
    }
  }
}
function finalize(ctx, schema) {
  const root2 = ctx.seen.get(schema);
  if (!root2)
    throw new Error("Unprocessed schema. This is a bug in Zod.");
  const flattenRef = (zodSchema) => {
    const seen = ctx.seen.get(zodSchema);
    if (seen.ref === null)
      return;
    const schema2 = seen.def ?? seen.schema;
    const _cached = { ...schema2 };
    const ref = seen.ref;
    seen.ref = null;
    if (ref) {
      flattenRef(ref);
      const refSeen = ctx.seen.get(ref);
      const refSchema = refSeen.schema;
      if (refSchema.$ref && (ctx.target === "draft-07" || ctx.target === "draft-04" || ctx.target === "openapi-3.0")) {
        schema2.allOf = schema2.allOf ?? [];
        schema2.allOf.push(refSchema);
      } else {
        Object.assign(schema2, refSchema);
      }
      Object.assign(schema2, _cached);
      const isParentRef = zodSchema._zod.parent === ref;
      if (isParentRef) {
        for (const key in schema2) {
          if (key === "$ref" || key === "allOf")
            continue;
          if (!(key in _cached)) {
            delete schema2[key];
          }
        }
      }
      if (refSchema.$ref && refSeen.def) {
        for (const key in schema2) {
          if (key === "$ref" || key === "allOf")
            continue;
          if (key in refSeen.def && JSON.stringify(schema2[key]) === JSON.stringify(refSeen.def[key])) {
            delete schema2[key];
          }
        }
      }
    }
    const parent = zodSchema._zod.parent;
    if (parent && parent !== ref) {
      flattenRef(parent);
      const parentSeen = ctx.seen.get(parent);
      if (parentSeen?.schema.$ref) {
        schema2.$ref = parentSeen.schema.$ref;
        if (parentSeen.def) {
          for (const key in schema2) {
            if (key === "$ref" || key === "allOf")
              continue;
            if (key in parentSeen.def && JSON.stringify(schema2[key]) === JSON.stringify(parentSeen.def[key])) {
              delete schema2[key];
            }
          }
        }
      }
    }
    ctx.override({
      zodSchema,
      jsonSchema: schema2,
      path: seen.path ?? []
    });
  };
  for (const entry of [...ctx.seen.entries()].reverse()) {
    flattenRef(entry[0]);
  }
  const result = {};
  if (ctx.target === "draft-2020-12") {
    result.$schema = "https://json-schema.org/draft/2020-12/schema";
  } else if (ctx.target === "draft-07") {
    result.$schema = "http://json-schema.org/draft-07/schema#";
  } else if (ctx.target === "draft-04") {
    result.$schema = "http://json-schema.org/draft-04/schema#";
  } else if (ctx.target === "openapi-3.0") {
  } else {
  }
  if (ctx.external?.uri) {
    const id = ctx.external.registry.get(schema)?.id;
    if (!id)
      throw new Error("Schema is missing an `id` property");
    result.$id = ctx.external.uri(id);
  }
  Object.assign(result, root2.def ?? root2.schema);
  const rootMetaId = ctx.metadataRegistry.get(schema)?.id;
  if (rootMetaId !== void 0 && result.id === rootMetaId)
    delete result.id;
  const defs = ctx.external?.defs ?? {};
  for (const entry of ctx.seen.entries()) {
    const seen = entry[1];
    if (seen.def && seen.defId) {
      if (seen.def.id === seen.defId)
        delete seen.def.id;
      defs[seen.defId] = seen.def;
    }
  }
  if (ctx.external) {
  } else {
    if (Object.keys(defs).length > 0) {
      if (ctx.target === "draft-2020-12") {
        result.$defs = defs;
      } else {
        result.definitions = defs;
      }
    }
  }
  try {
    const finalized = JSON.parse(JSON.stringify(result));
    Object.defineProperty(finalized, "~standard", {
      value: {
        ...schema["~standard"],
        jsonSchema: {
          input: createStandardJSONSchemaMethod(schema, "input", ctx.processors),
          output: createStandardJSONSchemaMethod(schema, "output", ctx.processors)
        }
      },
      enumerable: false,
      writable: false
    });
    return finalized;
  } catch (_err) {
    throw new Error("Error converting schema to JSON.");
  }
}
function isTransforming(_schema, _ctx) {
  const ctx = _ctx ?? { seen: /* @__PURE__ */ new Set() };
  if (ctx.seen.has(_schema))
    return false;
  ctx.seen.add(_schema);
  const def = _schema._zod.def;
  if (def.type === "transform")
    return true;
  if (def.type === "array")
    return isTransforming(def.element, ctx);
  if (def.type === "set")
    return isTransforming(def.valueType, ctx);
  if (def.type === "lazy")
    return isTransforming(def.getter(), ctx);
  if (def.type === "promise" || def.type === "optional" || def.type === "nonoptional" || def.type === "nullable" || def.type === "readonly" || def.type === "default" || def.type === "prefault") {
    return isTransforming(def.innerType, ctx);
  }
  if (def.type === "intersection") {
    return isTransforming(def.left, ctx) || isTransforming(def.right, ctx);
  }
  if (def.type === "record" || def.type === "map") {
    return isTransforming(def.keyType, ctx) || isTransforming(def.valueType, ctx);
  }
  if (def.type === "pipe") {
    if (_schema._zod.traits.has("$ZodCodec"))
      return true;
    return isTransforming(def.in, ctx) || isTransforming(def.out, ctx);
  }
  if (def.type === "object") {
    for (const key in def.shape) {
      if (isTransforming(def.shape[key], ctx))
        return true;
    }
    return false;
  }
  if (def.type === "union") {
    for (const option of def.options) {
      if (isTransforming(option, ctx))
        return true;
    }
    return false;
  }
  if (def.type === "tuple") {
    for (const item of def.items) {
      if (isTransforming(item, ctx))
        return true;
    }
    if (def.rest && isTransforming(def.rest, ctx))
      return true;
    return false;
  }
  return false;
}
var createToJSONSchemaMethod = (schema, processors = {}) => (params) => {
  const ctx = initializeContext({ ...params, processors });
  process2(schema, ctx);
  extractDefs(ctx, schema);
  return finalize(ctx, schema);
};
var createStandardJSONSchemaMethod = (schema, io, processors = {}) => (params) => {
  const { libraryOptions, target } = params ?? {};
  const ctx = initializeContext({ ...libraryOptions ?? {}, target, io, processors });
  process2(schema, ctx);
  extractDefs(ctx, schema);
  return finalize(ctx, schema);
};

// node_modules/zod/v4/core/json-schema-processors.js
var formatMap = {
  guid: "uuid",
  url: "uri",
  datetime: "date-time",
  json_string: "json-string",
  regex: ""
  // do not set
};
var stringProcessor = (schema, ctx, _json, _params) => {
  const json2 = _json;
  json2.type = "string";
  const { minimum, maximum, format, patterns, contentEncoding } = schema._zod.bag;
  if (typeof minimum === "number")
    json2.minLength = minimum;
  if (typeof maximum === "number")
    json2.maxLength = maximum;
  if (format) {
    json2.format = formatMap[format] ?? format;
    if (json2.format === "")
      delete json2.format;
    if (format === "time") {
      delete json2.format;
    }
  }
  if (contentEncoding)
    json2.contentEncoding = contentEncoding;
  if (patterns && patterns.size > 0) {
    const regexes = [...patterns];
    if (regexes.length === 1)
      json2.pattern = regexes[0].source;
    else if (regexes.length > 1) {
      json2.allOf = [
        ...regexes.map((regex) => ({
          ...ctx.target === "draft-07" || ctx.target === "draft-04" || ctx.target === "openapi-3.0" ? { type: "string" } : {},
          pattern: regex.source
        }))
      ];
    }
  }
};
var numberProcessor = (schema, ctx, _json, _params) => {
  const json2 = _json;
  const { minimum, maximum, format, multipleOf, exclusiveMaximum, exclusiveMinimum } = schema._zod.bag;
  if (typeof format === "string" && format.includes("int"))
    json2.type = "integer";
  else
    json2.type = "number";
  const exMin = typeof exclusiveMinimum === "number" && exclusiveMinimum >= (minimum ?? Number.NEGATIVE_INFINITY);
  const exMax = typeof exclusiveMaximum === "number" && exclusiveMaximum <= (maximum ?? Number.POSITIVE_INFINITY);
  const legacy = ctx.target === "draft-04" || ctx.target === "openapi-3.0";
  if (exMin) {
    if (legacy) {
      json2.minimum = exclusiveMinimum;
      json2.exclusiveMinimum = true;
    } else {
      json2.exclusiveMinimum = exclusiveMinimum;
    }
  } else if (typeof minimum === "number") {
    json2.minimum = minimum;
  }
  if (exMax) {
    if (legacy) {
      json2.maximum = exclusiveMaximum;
      json2.exclusiveMaximum = true;
    } else {
      json2.exclusiveMaximum = exclusiveMaximum;
    }
  } else if (typeof maximum === "number") {
    json2.maximum = maximum;
  }
  if (typeof multipleOf === "number")
    json2.multipleOf = multipleOf;
};
var booleanProcessor = (_schema, _ctx, json2, _params) => {
  json2.type = "boolean";
};
var bigintProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("BigInt cannot be represented in JSON Schema");
  }
};
var symbolProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("Symbols cannot be represented in JSON Schema");
  }
};
var nullProcessor = (_schema, ctx, json2, _params) => {
  if (ctx.target === "openapi-3.0") {
    json2.type = "string";
    json2.nullable = true;
    json2.enum = [null];
  } else {
    json2.type = "null";
  }
};
var undefinedProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("Undefined cannot be represented in JSON Schema");
  }
};
var voidProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("Void cannot be represented in JSON Schema");
  }
};
var neverProcessor = (_schema, _ctx, json2, _params) => {
  json2.not = {};
};
var anyProcessor = (_schema, _ctx, _json, _params) => {
};
var unknownProcessor = (_schema, _ctx, _json, _params) => {
};
var dateProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("Date cannot be represented in JSON Schema");
  }
};
var enumProcessor = (schema, _ctx, json2, _params) => {
  const def = schema._zod.def;
  const values = getEnumValues(def.entries);
  if (values.every((v) => typeof v === "number"))
    json2.type = "number";
  if (values.every((v) => typeof v === "string"))
    json2.type = "string";
  json2.enum = values;
};
var literalProcessor = (schema, ctx, json2, _params) => {
  const def = schema._zod.def;
  const vals = [];
  for (const val of def.values) {
    if (val === void 0) {
      if (ctx.unrepresentable === "throw") {
        throw new Error("Literal `undefined` cannot be represented in JSON Schema");
      } else {
      }
    } else if (typeof val === "bigint") {
      if (ctx.unrepresentable === "throw") {
        throw new Error("BigInt literals cannot be represented in JSON Schema");
      } else {
        vals.push(Number(val));
      }
    } else {
      vals.push(val);
    }
  }
  if (vals.length === 0) {
  } else if (vals.length === 1) {
    const val = vals[0];
    json2.type = val === null ? "null" : typeof val;
    if (ctx.target === "draft-04" || ctx.target === "openapi-3.0") {
      json2.enum = [val];
    } else {
      json2.const = val;
    }
  } else {
    if (vals.every((v) => typeof v === "number"))
      json2.type = "number";
    if (vals.every((v) => typeof v === "string"))
      json2.type = "string";
    if (vals.every((v) => typeof v === "boolean"))
      json2.type = "boolean";
    if (vals.every((v) => v === null))
      json2.type = "null";
    json2.enum = vals;
  }
};
var nanProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("NaN cannot be represented in JSON Schema");
  }
};
var templateLiteralProcessor = (schema, _ctx, json2, _params) => {
  const _json = json2;
  const pattern = schema._zod.pattern;
  if (!pattern)
    throw new Error("Pattern not found in template literal");
  _json.type = "string";
  _json.pattern = pattern.source;
};
var fileProcessor = (schema, _ctx, json2, _params) => {
  const _json = json2;
  const file2 = {
    type: "string",
    format: "binary",
    contentEncoding: "binary"
  };
  const { minimum, maximum, mime } = schema._zod.bag;
  if (minimum !== void 0)
    file2.minLength = minimum;
  if (maximum !== void 0)
    file2.maxLength = maximum;
  if (mime) {
    if (mime.length === 1) {
      file2.contentMediaType = mime[0];
      Object.assign(_json, file2);
    } else {
      Object.assign(_json, file2);
      _json.anyOf = mime.map((m) => ({ contentMediaType: m }));
    }
  } else {
    Object.assign(_json, file2);
  }
};
var successProcessor = (_schema, _ctx, json2, _params) => {
  json2.type = "boolean";
};
var customProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("Custom types cannot be represented in JSON Schema");
  }
};
var functionProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("Function types cannot be represented in JSON Schema");
  }
};
var transformProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("Transforms cannot be represented in JSON Schema");
  }
};
var mapProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("Map cannot be represented in JSON Schema");
  }
};
var setProcessor = (_schema, ctx, _json, _params) => {
  if (ctx.unrepresentable === "throw") {
    throw new Error("Set cannot be represented in JSON Schema");
  }
};
var arrayProcessor = (schema, ctx, _json, params) => {
  const json2 = _json;
  const def = schema._zod.def;
  const { minimum, maximum } = schema._zod.bag;
  if (typeof minimum === "number")
    json2.minItems = minimum;
  if (typeof maximum === "number")
    json2.maxItems = maximum;
  json2.type = "array";
  json2.items = process2(def.element, ctx, {
    ...params,
    path: [...params.path, "items"]
  });
};
var objectProcessor = (schema, ctx, _json, params) => {
  const json2 = _json;
  const def = schema._zod.def;
  json2.type = "object";
  json2.properties = {};
  const shape = def.shape;
  for (const key in shape) {
    json2.properties[key] = process2(shape[key], ctx, {
      ...params,
      path: [...params.path, "properties", key]
    });
  }
  const allKeys = new Set(Object.keys(shape));
  const requiredKeys = new Set([...allKeys].filter((key) => {
    const v = def.shape[key]._zod;
    if (ctx.io === "input") {
      return v.optin === void 0;
    } else {
      return v.optout === void 0;
    }
  }));
  if (requiredKeys.size > 0) {
    json2.required = Array.from(requiredKeys);
  }
  if (def.catchall?._zod.def.type === "never") {
    json2.additionalProperties = false;
  } else if (!def.catchall) {
    if (ctx.io === "output")
      json2.additionalProperties = false;
  } else if (def.catchall) {
    json2.additionalProperties = process2(def.catchall, ctx, {
      ...params,
      path: [...params.path, "additionalProperties"]
    });
  }
};
var unionProcessor = (schema, ctx, json2, params) => {
  const def = schema._zod.def;
  const isExclusive = def.inclusive === false;
  const options = def.options.map((x, i) => process2(x, ctx, {
    ...params,
    path: [...params.path, isExclusive ? "oneOf" : "anyOf", i]
  }));
  if (isExclusive) {
    json2.oneOf = options;
  } else {
    json2.anyOf = options;
  }
};
var intersectionProcessor = (schema, ctx, json2, params) => {
  const def = schema._zod.def;
  const a = process2(def.left, ctx, {
    ...params,
    path: [...params.path, "allOf", 0]
  });
  const b = process2(def.right, ctx, {
    ...params,
    path: [...params.path, "allOf", 1]
  });
  const isSimpleIntersection = (val) => "allOf" in val && Object.keys(val).length === 1;
  const allOf = [
    ...isSimpleIntersection(a) ? a.allOf : [a],
    ...isSimpleIntersection(b) ? b.allOf : [b]
  ];
  json2.allOf = allOf;
};
var tupleProcessor = (schema, ctx, _json, params) => {
  const json2 = _json;
  const def = schema._zod.def;
  json2.type = "array";
  const prefixPath = ctx.target === "draft-2020-12" ? "prefixItems" : "items";
  const restPath = ctx.target === "draft-2020-12" ? "items" : ctx.target === "openapi-3.0" ? "items" : "additionalItems";
  const prefixItems = def.items.map((x, i) => process2(x, ctx, {
    ...params,
    path: [...params.path, prefixPath, i]
  }));
  const rest = def.rest ? process2(def.rest, ctx, {
    ...params,
    path: [...params.path, restPath, ...ctx.target === "openapi-3.0" ? [def.items.length] : []]
  }) : null;
  if (ctx.target === "draft-2020-12") {
    json2.prefixItems = prefixItems;
    if (rest) {
      json2.items = rest;
    }
  } else if (ctx.target === "openapi-3.0") {
    json2.items = {
      anyOf: prefixItems
    };
    if (rest) {
      json2.items.anyOf.push(rest);
    }
    json2.minItems = prefixItems.length;
    if (!rest) {
      json2.maxItems = prefixItems.length;
    }
  } else {
    json2.items = prefixItems;
    if (rest) {
      json2.additionalItems = rest;
    }
  }
  const { minimum, maximum } = schema._zod.bag;
  if (typeof minimum === "number")
    json2.minItems = minimum;
  if (typeof maximum === "number")
    json2.maxItems = maximum;
};
var recordProcessor = (schema, ctx, _json, params) => {
  const json2 = _json;
  const def = schema._zod.def;
  json2.type = "object";
  const keyType = def.keyType;
  const keyBag = keyType._zod.bag;
  const patterns = keyBag?.patterns;
  if (def.mode === "loose" && patterns && patterns.size > 0) {
    const valueSchema = process2(def.valueType, ctx, {
      ...params,
      path: [...params.path, "patternProperties", "*"]
    });
    json2.patternProperties = {};
    for (const pattern of patterns) {
      json2.patternProperties[pattern.source] = valueSchema;
    }
  } else {
    if (ctx.target === "draft-07" || ctx.target === "draft-2020-12") {
      json2.propertyNames = process2(def.keyType, ctx, {
        ...params,
        path: [...params.path, "propertyNames"]
      });
    }
    json2.additionalProperties = process2(def.valueType, ctx, {
      ...params,
      path: [...params.path, "additionalProperties"]
    });
  }
  const keyValues = keyType._zod.values;
  if (keyValues) {
    const validKeyValues = [...keyValues].filter((v) => typeof v === "string" || typeof v === "number");
    if (validKeyValues.length > 0) {
      json2.required = validKeyValues;
    }
  }
};
var nullableProcessor = (schema, ctx, json2, params) => {
  const def = schema._zod.def;
  const inner = process2(def.innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  if (ctx.target === "openapi-3.0") {
    seen.ref = def.innerType;
    json2.nullable = true;
  } else {
    json2.anyOf = [inner, { type: "null" }];
  }
};
var nonoptionalProcessor = (schema, ctx, _json, params) => {
  const def = schema._zod.def;
  process2(def.innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  seen.ref = def.innerType;
};
var defaultProcessor = (schema, ctx, json2, params) => {
  const def = schema._zod.def;
  process2(def.innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  seen.ref = def.innerType;
  json2.default = JSON.parse(JSON.stringify(def.defaultValue));
};
var prefaultProcessor = (schema, ctx, json2, params) => {
  const def = schema._zod.def;
  process2(def.innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  seen.ref = def.innerType;
  if (ctx.io === "input")
    json2._prefault = JSON.parse(JSON.stringify(def.defaultValue));
};
var catchProcessor = (schema, ctx, json2, params) => {
  const def = schema._zod.def;
  process2(def.innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  seen.ref = def.innerType;
  let catchValue;
  try {
    catchValue = def.catchValue(void 0);
  } catch {
    throw new Error("Dynamic catch values are not supported in JSON Schema");
  }
  json2.default = catchValue;
};
var pipeProcessor = (schema, ctx, _json, params) => {
  const def = schema._zod.def;
  const inIsTransform = def.in._zod.traits.has("$ZodTransform");
  const innerType = ctx.io === "input" ? inIsTransform ? def.out : def.in : def.out;
  process2(innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  seen.ref = innerType;
};
var readonlyProcessor = (schema, ctx, json2, params) => {
  const def = schema._zod.def;
  process2(def.innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  seen.ref = def.innerType;
  json2.readOnly = true;
};
var promiseProcessor = (schema, ctx, _json, params) => {
  const def = schema._zod.def;
  process2(def.innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  seen.ref = def.innerType;
};
var optionalProcessor = (schema, ctx, _json, params) => {
  const def = schema._zod.def;
  process2(def.innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  seen.ref = def.innerType;
};
var lazyProcessor = (schema, ctx, _json, params) => {
  const innerType = schema._zod.innerType;
  process2(innerType, ctx, params);
  const seen = ctx.seen.get(schema);
  seen.ref = innerType;
};
var allProcessors = {
  string: stringProcessor,
  number: numberProcessor,
  boolean: booleanProcessor,
  bigint: bigintProcessor,
  symbol: symbolProcessor,
  null: nullProcessor,
  undefined: undefinedProcessor,
  void: voidProcessor,
  never: neverProcessor,
  any: anyProcessor,
  unknown: unknownProcessor,
  date: dateProcessor,
  enum: enumProcessor,
  literal: literalProcessor,
  nan: nanProcessor,
  template_literal: templateLiteralProcessor,
  file: fileProcessor,
  success: successProcessor,
  custom: customProcessor,
  function: functionProcessor,
  transform: transformProcessor,
  map: mapProcessor,
  set: setProcessor,
  array: arrayProcessor,
  object: objectProcessor,
  union: unionProcessor,
  intersection: intersectionProcessor,
  tuple: tupleProcessor,
  record: recordProcessor,
  nullable: nullableProcessor,
  nonoptional: nonoptionalProcessor,
  default: defaultProcessor,
  prefault: prefaultProcessor,
  catch: catchProcessor,
  pipe: pipeProcessor,
  readonly: readonlyProcessor,
  promise: promiseProcessor,
  optional: optionalProcessor,
  lazy: lazyProcessor
};
function toJSONSchema(input, params) {
  if ("_idmap" in input) {
    const registry2 = input;
    const ctx2 = initializeContext({ ...params, processors: allProcessors });
    const defs = {};
    for (const entry of registry2._idmap.entries()) {
      const [_, schema] = entry;
      process2(schema, ctx2);
    }
    const schemas = {};
    const external = {
      registry: registry2,
      uri: params?.uri,
      defs
    };
    ctx2.external = external;
    for (const entry of registry2._idmap.entries()) {
      const [key, schema] = entry;
      extractDefs(ctx2, schema);
      schemas[key] = finalize(ctx2, schema);
    }
    if (Object.keys(defs).length > 0) {
      const defsSegment = ctx2.target === "draft-2020-12" ? "$defs" : "definitions";
      schemas.__shared = {
        [defsSegment]: defs
      };
    }
    return { schemas };
  }
  const ctx = initializeContext({ ...params, processors: allProcessors });
  process2(input, ctx);
  extractDefs(ctx, input);
  return finalize(ctx, input);
}

// node_modules/zod/v4/core/json-schema-generator.js
var JSONSchemaGenerator = class {
  /** @deprecated Access via ctx instead */
  get metadataRegistry() {
    return this.ctx.metadataRegistry;
  }
  /** @deprecated Access via ctx instead */
  get target() {
    return this.ctx.target;
  }
  /** @deprecated Access via ctx instead */
  get unrepresentable() {
    return this.ctx.unrepresentable;
  }
  /** @deprecated Access via ctx instead */
  get override() {
    return this.ctx.override;
  }
  /** @deprecated Access via ctx instead */
  get io() {
    return this.ctx.io;
  }
  /** @deprecated Access via ctx instead */
  get counter() {
    return this.ctx.counter;
  }
  set counter(value) {
    this.ctx.counter = value;
  }
  /** @deprecated Access via ctx instead */
  get seen() {
    return this.ctx.seen;
  }
  constructor(params) {
    let normalizedTarget = params?.target ?? "draft-2020-12";
    if (normalizedTarget === "draft-4")
      normalizedTarget = "draft-04";
    if (normalizedTarget === "draft-7")
      normalizedTarget = "draft-07";
    this.ctx = initializeContext({
      processors: allProcessors,
      target: normalizedTarget,
      ...params?.metadata && { metadata: params.metadata },
      ...params?.unrepresentable && { unrepresentable: params.unrepresentable },
      ...params?.override && { override: params.override },
      ...params?.io && { io: params.io }
    });
  }
  /**
   * Process a schema to prepare it for JSON Schema generation.
   * This must be called before emit().
   */
  process(schema, _params = { path: [], schemaPath: [] }) {
    return process2(schema, this.ctx, _params);
  }
  /**
   * Emit the final JSON Schema after processing.
   * Must call process() first.
   */
  emit(schema, _params) {
    if (_params) {
      if (_params.cycles)
        this.ctx.cycles = _params.cycles;
      if (_params.reused)
        this.ctx.reused = _params.reused;
      if (_params.external)
        this.ctx.external = _params.external;
    }
    extractDefs(this.ctx, schema);
    const result = finalize(this.ctx, schema);
    const { "~standard": _, ...plainResult } = result;
    return plainResult;
  }
};

// node_modules/zod/v4/core/json-schema.js
var json_schema_exports = {};

// node_modules/zod/v4/classic/schemas.js
var schemas_exports2 = {};
__export(schemas_exports2, {
  ZodAny: () => ZodAny,
  ZodArray: () => ZodArray,
  ZodBase64: () => ZodBase64,
  ZodBase64URL: () => ZodBase64URL,
  ZodBigInt: () => ZodBigInt,
  ZodBigIntFormat: () => ZodBigIntFormat,
  ZodBoolean: () => ZodBoolean,
  ZodCIDRv4: () => ZodCIDRv4,
  ZodCIDRv6: () => ZodCIDRv6,
  ZodCUID: () => ZodCUID,
  ZodCUID2: () => ZodCUID2,
  ZodCatch: () => ZodCatch,
  ZodCodec: () => ZodCodec,
  ZodCustom: () => ZodCustom,
  ZodCustomStringFormat: () => ZodCustomStringFormat,
  ZodDate: () => ZodDate,
  ZodDefault: () => ZodDefault,
  ZodDiscriminatedUnion: () => ZodDiscriminatedUnion,
  ZodE164: () => ZodE164,
  ZodEmail: () => ZodEmail,
  ZodEmoji: () => ZodEmoji,
  ZodEnum: () => ZodEnum,
  ZodExactOptional: () => ZodExactOptional,
  ZodFile: () => ZodFile,
  ZodFunction: () => ZodFunction,
  ZodGUID: () => ZodGUID,
  ZodIPv4: () => ZodIPv4,
  ZodIPv6: () => ZodIPv6,
  ZodIntersection: () => ZodIntersection,
  ZodJWT: () => ZodJWT,
  ZodKSUID: () => ZodKSUID,
  ZodLazy: () => ZodLazy,
  ZodLiteral: () => ZodLiteral,
  ZodMAC: () => ZodMAC,
  ZodMap: () => ZodMap,
  ZodNaN: () => ZodNaN,
  ZodNanoID: () => ZodNanoID,
  ZodNever: () => ZodNever,
  ZodNonOptional: () => ZodNonOptional,
  ZodNull: () => ZodNull,
  ZodNullable: () => ZodNullable,
  ZodNumber: () => ZodNumber,
  ZodNumberFormat: () => ZodNumberFormat,
  ZodObject: () => ZodObject,
  ZodOptional: () => ZodOptional,
  ZodPipe: () => ZodPipe,
  ZodPrefault: () => ZodPrefault,
  ZodPreprocess: () => ZodPreprocess,
  ZodPromise: () => ZodPromise,
  ZodReadonly: () => ZodReadonly,
  ZodRecord: () => ZodRecord,
  ZodSet: () => ZodSet,
  ZodString: () => ZodString,
  ZodStringFormat: () => ZodStringFormat,
  ZodSuccess: () => ZodSuccess,
  ZodSymbol: () => ZodSymbol,
  ZodTemplateLiteral: () => ZodTemplateLiteral,
  ZodTransform: () => ZodTransform,
  ZodTuple: () => ZodTuple,
  ZodType: () => ZodType,
  ZodULID: () => ZodULID,
  ZodURL: () => ZodURL,
  ZodUUID: () => ZodUUID,
  ZodUndefined: () => ZodUndefined,
  ZodUnion: () => ZodUnion,
  ZodUnknown: () => ZodUnknown,
  ZodVoid: () => ZodVoid,
  ZodXID: () => ZodXID,
  ZodXor: () => ZodXor,
  _ZodString: () => _ZodString,
  _default: () => _default2,
  _function: () => _function,
  any: () => any,
  array: () => array,
  base64: () => base642,
  base64url: () => base64url2,
  bigint: () => bigint2,
  boolean: () => boolean2,
  catch: () => _catch2,
  check: () => check,
  cidrv4: () => cidrv42,
  cidrv6: () => cidrv62,
  codec: () => codec,
  cuid: () => cuid3,
  cuid2: () => cuid22,
  custom: () => custom,
  date: () => date3,
  describe: () => describe2,
  discriminatedUnion: () => discriminatedUnion,
  e164: () => e1642,
  email: () => email2,
  emoji: () => emoji2,
  enum: () => _enum2,
  exactOptional: () => exactOptional,
  file: () => file,
  float32: () => float32,
  float64: () => float64,
  function: () => _function,
  guid: () => guid2,
  hash: () => hash,
  hex: () => hex2,
  hostname: () => hostname2,
  httpUrl: () => httpUrl,
  instanceof: () => _instanceof,
  int: () => int,
  int32: () => int32,
  int64: () => int64,
  intersection: () => intersection,
  invertCodec: () => invertCodec,
  ipv4: () => ipv42,
  ipv6: () => ipv62,
  json: () => json,
  jwt: () => jwt,
  keyof: () => keyof,
  ksuid: () => ksuid2,
  lazy: () => lazy,
  literal: () => literal,
  looseObject: () => looseObject,
  looseRecord: () => looseRecord,
  mac: () => mac2,
  map: () => map,
  meta: () => meta2,
  nan: () => nan,
  nanoid: () => nanoid2,
  nativeEnum: () => nativeEnum,
  never: () => never,
  nonoptional: () => nonoptional,
  null: () => _null3,
  nullable: () => nullable,
  nullish: () => nullish2,
  number: () => number2,
  object: () => object,
  optional: () => optional,
  partialRecord: () => partialRecord,
  pipe: () => pipe,
  prefault: () => prefault,
  preprocess: () => preprocess,
  promise: () => promise,
  readonly: () => readonly,
  record: () => record,
  refine: () => refine,
  set: () => set,
  strictObject: () => strictObject,
  string: () => string2,
  stringFormat: () => stringFormat,
  stringbool: () => stringbool,
  success: () => success,
  superRefine: () => superRefine,
  symbol: () => symbol,
  templateLiteral: () => templateLiteral,
  transform: () => transform,
  tuple: () => tuple,
  uint32: () => uint32,
  uint64: () => uint64,
  ulid: () => ulid2,
  undefined: () => _undefined3,
  union: () => union,
  unknown: () => unknown,
  url: () => url,
  uuid: () => uuid2,
  uuidv4: () => uuidv4,
  uuidv6: () => uuidv6,
  uuidv7: () => uuidv7,
  void: () => _void2,
  xid: () => xid2,
  xor: () => xor
});

// node_modules/zod/v4/classic/checks.js
var checks_exports2 = {};
__export(checks_exports2, {
  endsWith: () => _endsWith,
  gt: () => _gt,
  gte: () => _gte,
  includes: () => _includes,
  length: () => _length,
  lowercase: () => _lowercase,
  lt: () => _lt,
  lte: () => _lte,
  maxLength: () => _maxLength,
  maxSize: () => _maxSize,
  mime: () => _mime,
  minLength: () => _minLength,
  minSize: () => _minSize,
  multipleOf: () => _multipleOf,
  negative: () => _negative,
  nonnegative: () => _nonnegative,
  nonpositive: () => _nonpositive,
  normalize: () => _normalize,
  overwrite: () => _overwrite,
  positive: () => _positive,
  property: () => _property,
  regex: () => _regex,
  size: () => _size,
  slugify: () => _slugify,
  startsWith: () => _startsWith,
  toLowerCase: () => _toLowerCase,
  toUpperCase: () => _toUpperCase,
  trim: () => _trim,
  uppercase: () => _uppercase
});

// node_modules/zod/v4/classic/iso.js
var iso_exports = {};
__export(iso_exports, {
  ZodISODate: () => ZodISODate,
  ZodISODateTime: () => ZodISODateTime,
  ZodISODuration: () => ZodISODuration,
  ZodISOTime: () => ZodISOTime,
  date: () => date2,
  datetime: () => datetime2,
  duration: () => duration2,
  time: () => time2
});
var ZodISODateTime = /* @__PURE__ */ $constructor("ZodISODateTime", (inst, def) => {
  $ZodISODateTime.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function datetime2(params) {
  return _isoDateTime(ZodISODateTime, params);
}
var ZodISODate = /* @__PURE__ */ $constructor("ZodISODate", (inst, def) => {
  $ZodISODate.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function date2(params) {
  return _isoDate(ZodISODate, params);
}
var ZodISOTime = /* @__PURE__ */ $constructor("ZodISOTime", (inst, def) => {
  $ZodISOTime.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function time2(params) {
  return _isoTime(ZodISOTime, params);
}
var ZodISODuration = /* @__PURE__ */ $constructor("ZodISODuration", (inst, def) => {
  $ZodISODuration.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function duration2(params) {
  return _isoDuration(ZodISODuration, params);
}

// node_modules/zod/v4/classic/errors.js
var initializer2 = (inst, issues) => {
  $ZodError.init(inst, issues);
  inst.name = "ZodError";
  Object.defineProperties(inst, {
    format: {
      value: (mapper) => formatError(inst, mapper)
      // enumerable: false,
    },
    flatten: {
      value: (mapper) => flattenError(inst, mapper)
      // enumerable: false,
    },
    addIssue: {
      value: (issue2) => {
        inst.issues.push(issue2);
        inst.message = JSON.stringify(inst.issues, jsonStringifyReplacer, 2);
      }
      // enumerable: false,
    },
    addIssues: {
      value: (issues2) => {
        inst.issues.push(...issues2);
        inst.message = JSON.stringify(inst.issues, jsonStringifyReplacer, 2);
      }
      // enumerable: false,
    },
    isEmpty: {
      get() {
        return inst.issues.length === 0;
      }
      // enumerable: false,
    }
  });
};
var ZodError = /* @__PURE__ */ $constructor("ZodError", initializer2);
var ZodRealError = /* @__PURE__ */ $constructor("ZodError", initializer2, {
  Parent: Error
});

// node_modules/zod/v4/classic/parse.js
var parse2 = /* @__PURE__ */ _parse(ZodRealError);
var parseAsync2 = /* @__PURE__ */ _parseAsync(ZodRealError);
var safeParse2 = /* @__PURE__ */ _safeParse(ZodRealError);
var safeParseAsync2 = /* @__PURE__ */ _safeParseAsync(ZodRealError);
var encode2 = /* @__PURE__ */ _encode(ZodRealError);
var decode2 = /* @__PURE__ */ _decode(ZodRealError);
var encodeAsync2 = /* @__PURE__ */ _encodeAsync(ZodRealError);
var decodeAsync2 = /* @__PURE__ */ _decodeAsync(ZodRealError);
var safeEncode2 = /* @__PURE__ */ _safeEncode(ZodRealError);
var safeDecode2 = /* @__PURE__ */ _safeDecode(ZodRealError);
var safeEncodeAsync2 = /* @__PURE__ */ _safeEncodeAsync(ZodRealError);
var safeDecodeAsync2 = /* @__PURE__ */ _safeDecodeAsync(ZodRealError);

// node_modules/zod/v4/classic/schemas.js
var _installedGroups = /* @__PURE__ */ new WeakMap();
function _installLazyMethods(inst, group, methods) {
  const proto = Object.getPrototypeOf(inst);
  let installed = _installedGroups.get(proto);
  if (!installed) {
    installed = /* @__PURE__ */ new Set();
    _installedGroups.set(proto, installed);
  }
  if (installed.has(group))
    return;
  installed.add(group);
  for (const key in methods) {
    const fn = methods[key];
    Object.defineProperty(proto, key, {
      configurable: true,
      enumerable: false,
      get() {
        const bound = fn.bind(this);
        Object.defineProperty(this, key, {
          configurable: true,
          writable: true,
          enumerable: true,
          value: bound
        });
        return bound;
      },
      set(v) {
        Object.defineProperty(this, key, {
          configurable: true,
          writable: true,
          enumerable: true,
          value: v
        });
      }
    });
  }
}
var ZodType = /* @__PURE__ */ $constructor("ZodType", (inst, def) => {
  $ZodType.init(inst, def);
  Object.assign(inst["~standard"], {
    jsonSchema: {
      input: createStandardJSONSchemaMethod(inst, "input"),
      output: createStandardJSONSchemaMethod(inst, "output")
    }
  });
  inst.toJSONSchema = createToJSONSchemaMethod(inst, {});
  inst.def = def;
  inst.type = def.type;
  Object.defineProperty(inst, "_def", { value: def });
  inst.parse = (data, params) => parse2(inst, data, params, { callee: inst.parse });
  inst.safeParse = (data, params) => safeParse2(inst, data, params);
  inst.parseAsync = async (data, params) => parseAsync2(inst, data, params, { callee: inst.parseAsync });
  inst.safeParseAsync = async (data, params) => safeParseAsync2(inst, data, params);
  inst.spa = inst.safeParseAsync;
  inst.encode = (data, params) => encode2(inst, data, params);
  inst.decode = (data, params) => decode2(inst, data, params);
  inst.encodeAsync = async (data, params) => encodeAsync2(inst, data, params);
  inst.decodeAsync = async (data, params) => decodeAsync2(inst, data, params);
  inst.safeEncode = (data, params) => safeEncode2(inst, data, params);
  inst.safeDecode = (data, params) => safeDecode2(inst, data, params);
  inst.safeEncodeAsync = async (data, params) => safeEncodeAsync2(inst, data, params);
  inst.safeDecodeAsync = async (data, params) => safeDecodeAsync2(inst, data, params);
  _installLazyMethods(inst, "ZodType", {
    check(...chks) {
      const def2 = this.def;
      return this.clone(util_exports.mergeDefs(def2, {
        checks: [
          ...def2.checks ?? [],
          ...chks.map((ch) => typeof ch === "function" ? { _zod: { check: ch, def: { check: "custom" }, onattach: [] } } : ch)
        ]
      }), { parent: true });
    },
    with(...chks) {
      return this.check(...chks);
    },
    clone(def2, params) {
      return clone(this, def2, params);
    },
    brand() {
      return this;
    },
    register(reg, meta3) {
      reg.add(this, meta3);
      return this;
    },
    refine(check2, params) {
      return this.check(refine(check2, params));
    },
    superRefine(refinement, params) {
      return this.check(superRefine(refinement, params));
    },
    overwrite(fn) {
      return this.check(_overwrite(fn));
    },
    optional() {
      return optional(this);
    },
    exactOptional() {
      return exactOptional(this);
    },
    nullable() {
      return nullable(this);
    },
    nullish() {
      return optional(nullable(this));
    },
    nonoptional(params) {
      return nonoptional(this, params);
    },
    array() {
      return array(this);
    },
    or(arg) {
      return union([this, arg]);
    },
    and(arg) {
      return intersection(this, arg);
    },
    transform(tx) {
      return pipe(this, transform(tx));
    },
    default(d) {
      return _default2(this, d);
    },
    prefault(d) {
      return prefault(this, d);
    },
    catch(params) {
      return _catch2(this, params);
    },
    pipe(target) {
      return pipe(this, target);
    },
    readonly() {
      return readonly(this);
    },
    describe(description) {
      const cl = this.clone();
      globalRegistry.add(cl, { description });
      return cl;
    },
    meta(...args) {
      if (args.length === 0)
        return globalRegistry.get(this);
      const cl = this.clone();
      globalRegistry.add(cl, args[0]);
      return cl;
    },
    isOptional() {
      return this.safeParse(void 0).success;
    },
    isNullable() {
      return this.safeParse(null).success;
    },
    apply(fn) {
      return fn(this);
    }
  });
  Object.defineProperty(inst, "description", {
    get() {
      return globalRegistry.get(inst)?.description;
    },
    configurable: true
  });
  return inst;
});
var _ZodString = /* @__PURE__ */ $constructor("_ZodString", (inst, def) => {
  $ZodString.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => stringProcessor(inst, ctx, json2, params);
  const bag = inst._zod.bag;
  inst.format = bag.format ?? null;
  inst.minLength = bag.minimum ?? null;
  inst.maxLength = bag.maximum ?? null;
  _installLazyMethods(inst, "_ZodString", {
    regex(...args) {
      return this.check(_regex(...args));
    },
    includes(...args) {
      return this.check(_includes(...args));
    },
    startsWith(...args) {
      return this.check(_startsWith(...args));
    },
    endsWith(...args) {
      return this.check(_endsWith(...args));
    },
    min(...args) {
      return this.check(_minLength(...args));
    },
    max(...args) {
      return this.check(_maxLength(...args));
    },
    length(...args) {
      return this.check(_length(...args));
    },
    nonempty(...args) {
      return this.check(_minLength(1, ...args));
    },
    lowercase(params) {
      return this.check(_lowercase(params));
    },
    uppercase(params) {
      return this.check(_uppercase(params));
    },
    trim() {
      return this.check(_trim());
    },
    normalize(...args) {
      return this.check(_normalize(...args));
    },
    toLowerCase() {
      return this.check(_toLowerCase());
    },
    toUpperCase() {
      return this.check(_toUpperCase());
    },
    slugify() {
      return this.check(_slugify());
    }
  });
});
var ZodString = /* @__PURE__ */ $constructor("ZodString", (inst, def) => {
  $ZodString.init(inst, def);
  _ZodString.init(inst, def);
  inst.email = (params) => inst.check(_email(ZodEmail, params));
  inst.url = (params) => inst.check(_url(ZodURL, params));
  inst.jwt = (params) => inst.check(_jwt(ZodJWT, params));
  inst.emoji = (params) => inst.check(_emoji2(ZodEmoji, params));
  inst.guid = (params) => inst.check(_guid(ZodGUID, params));
  inst.uuid = (params) => inst.check(_uuid(ZodUUID, params));
  inst.uuidv4 = (params) => inst.check(_uuidv4(ZodUUID, params));
  inst.uuidv6 = (params) => inst.check(_uuidv6(ZodUUID, params));
  inst.uuidv7 = (params) => inst.check(_uuidv7(ZodUUID, params));
  inst.nanoid = (params) => inst.check(_nanoid(ZodNanoID, params));
  inst.guid = (params) => inst.check(_guid(ZodGUID, params));
  inst.cuid = (params) => inst.check(_cuid(ZodCUID, params));
  inst.cuid2 = (params) => inst.check(_cuid2(ZodCUID2, params));
  inst.ulid = (params) => inst.check(_ulid(ZodULID, params));
  inst.base64 = (params) => inst.check(_base64(ZodBase64, params));
  inst.base64url = (params) => inst.check(_base64url(ZodBase64URL, params));
  inst.xid = (params) => inst.check(_xid(ZodXID, params));
  inst.ksuid = (params) => inst.check(_ksuid(ZodKSUID, params));
  inst.ipv4 = (params) => inst.check(_ipv4(ZodIPv4, params));
  inst.ipv6 = (params) => inst.check(_ipv6(ZodIPv6, params));
  inst.cidrv4 = (params) => inst.check(_cidrv4(ZodCIDRv4, params));
  inst.cidrv6 = (params) => inst.check(_cidrv6(ZodCIDRv6, params));
  inst.e164 = (params) => inst.check(_e164(ZodE164, params));
  inst.datetime = (params) => inst.check(datetime2(params));
  inst.date = (params) => inst.check(date2(params));
  inst.time = (params) => inst.check(time2(params));
  inst.duration = (params) => inst.check(duration2(params));
});
function string2(params) {
  return _string(ZodString, params);
}
var ZodStringFormat = /* @__PURE__ */ $constructor("ZodStringFormat", (inst, def) => {
  $ZodStringFormat.init(inst, def);
  _ZodString.init(inst, def);
});
var ZodEmail = /* @__PURE__ */ $constructor("ZodEmail", (inst, def) => {
  $ZodEmail.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function email2(params) {
  return _email(ZodEmail, params);
}
var ZodGUID = /* @__PURE__ */ $constructor("ZodGUID", (inst, def) => {
  $ZodGUID.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function guid2(params) {
  return _guid(ZodGUID, params);
}
var ZodUUID = /* @__PURE__ */ $constructor("ZodUUID", (inst, def) => {
  $ZodUUID.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function uuid2(params) {
  return _uuid(ZodUUID, params);
}
function uuidv4(params) {
  return _uuidv4(ZodUUID, params);
}
function uuidv6(params) {
  return _uuidv6(ZodUUID, params);
}
function uuidv7(params) {
  return _uuidv7(ZodUUID, params);
}
var ZodURL = /* @__PURE__ */ $constructor("ZodURL", (inst, def) => {
  $ZodURL.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function url(params) {
  return _url(ZodURL, params);
}
function httpUrl(params) {
  return _url(ZodURL, {
    protocol: regexes_exports.httpProtocol,
    hostname: regexes_exports.domain,
    ...util_exports.normalizeParams(params)
  });
}
var ZodEmoji = /* @__PURE__ */ $constructor("ZodEmoji", (inst, def) => {
  $ZodEmoji.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function emoji2(params) {
  return _emoji2(ZodEmoji, params);
}
var ZodNanoID = /* @__PURE__ */ $constructor("ZodNanoID", (inst, def) => {
  $ZodNanoID.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function nanoid2(params) {
  return _nanoid(ZodNanoID, params);
}
var ZodCUID = /* @__PURE__ */ $constructor("ZodCUID", (inst, def) => {
  $ZodCUID.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function cuid3(params) {
  return _cuid(ZodCUID, params);
}
var ZodCUID2 = /* @__PURE__ */ $constructor("ZodCUID2", (inst, def) => {
  $ZodCUID2.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function cuid22(params) {
  return _cuid2(ZodCUID2, params);
}
var ZodULID = /* @__PURE__ */ $constructor("ZodULID", (inst, def) => {
  $ZodULID.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function ulid2(params) {
  return _ulid(ZodULID, params);
}
var ZodXID = /* @__PURE__ */ $constructor("ZodXID", (inst, def) => {
  $ZodXID.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function xid2(params) {
  return _xid(ZodXID, params);
}
var ZodKSUID = /* @__PURE__ */ $constructor("ZodKSUID", (inst, def) => {
  $ZodKSUID.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function ksuid2(params) {
  return _ksuid(ZodKSUID, params);
}
var ZodIPv4 = /* @__PURE__ */ $constructor("ZodIPv4", (inst, def) => {
  $ZodIPv4.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function ipv42(params) {
  return _ipv4(ZodIPv4, params);
}
var ZodMAC = /* @__PURE__ */ $constructor("ZodMAC", (inst, def) => {
  $ZodMAC.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function mac2(params) {
  return _mac(ZodMAC, params);
}
var ZodIPv6 = /* @__PURE__ */ $constructor("ZodIPv6", (inst, def) => {
  $ZodIPv6.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function ipv62(params) {
  return _ipv6(ZodIPv6, params);
}
var ZodCIDRv4 = /* @__PURE__ */ $constructor("ZodCIDRv4", (inst, def) => {
  $ZodCIDRv4.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function cidrv42(params) {
  return _cidrv4(ZodCIDRv4, params);
}
var ZodCIDRv6 = /* @__PURE__ */ $constructor("ZodCIDRv6", (inst, def) => {
  $ZodCIDRv6.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function cidrv62(params) {
  return _cidrv6(ZodCIDRv6, params);
}
var ZodBase64 = /* @__PURE__ */ $constructor("ZodBase64", (inst, def) => {
  $ZodBase64.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function base642(params) {
  return _base64(ZodBase64, params);
}
var ZodBase64URL = /* @__PURE__ */ $constructor("ZodBase64URL", (inst, def) => {
  $ZodBase64URL.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function base64url2(params) {
  return _base64url(ZodBase64URL, params);
}
var ZodE164 = /* @__PURE__ */ $constructor("ZodE164", (inst, def) => {
  $ZodE164.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function e1642(params) {
  return _e164(ZodE164, params);
}
var ZodJWT = /* @__PURE__ */ $constructor("ZodJWT", (inst, def) => {
  $ZodJWT.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function jwt(params) {
  return _jwt(ZodJWT, params);
}
var ZodCustomStringFormat = /* @__PURE__ */ $constructor("ZodCustomStringFormat", (inst, def) => {
  $ZodCustomStringFormat.init(inst, def);
  ZodStringFormat.init(inst, def);
});
function stringFormat(format, fnOrRegex, _params = {}) {
  return _stringFormat(ZodCustomStringFormat, format, fnOrRegex, _params);
}
function hostname2(_params) {
  return _stringFormat(ZodCustomStringFormat, "hostname", regexes_exports.hostname, _params);
}
function hex2(_params) {
  return _stringFormat(ZodCustomStringFormat, "hex", regexes_exports.hex, _params);
}
function hash(alg, params) {
  const enc = params?.enc ?? "hex";
  const format = `${alg}_${enc}`;
  const regex = regexes_exports[format];
  if (!regex)
    throw new Error(`Unrecognized hash format: ${format}`);
  return _stringFormat(ZodCustomStringFormat, format, regex, params);
}
var ZodNumber = /* @__PURE__ */ $constructor("ZodNumber", (inst, def) => {
  $ZodNumber.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => numberProcessor(inst, ctx, json2, params);
  _installLazyMethods(inst, "ZodNumber", {
    gt(value, params) {
      return this.check(_gt(value, params));
    },
    gte(value, params) {
      return this.check(_gte(value, params));
    },
    min(value, params) {
      return this.check(_gte(value, params));
    },
    lt(value, params) {
      return this.check(_lt(value, params));
    },
    lte(value, params) {
      return this.check(_lte(value, params));
    },
    max(value, params) {
      return this.check(_lte(value, params));
    },
    int(params) {
      return this.check(int(params));
    },
    safe(params) {
      return this.check(int(params));
    },
    positive(params) {
      return this.check(_gt(0, params));
    },
    nonnegative(params) {
      return this.check(_gte(0, params));
    },
    negative(params) {
      return this.check(_lt(0, params));
    },
    nonpositive(params) {
      return this.check(_lte(0, params));
    },
    multipleOf(value, params) {
      return this.check(_multipleOf(value, params));
    },
    step(value, params) {
      return this.check(_multipleOf(value, params));
    },
    finite() {
      return this;
    }
  });
  const bag = inst._zod.bag;
  inst.minValue = Math.max(bag.minimum ?? Number.NEGATIVE_INFINITY, bag.exclusiveMinimum ?? Number.NEGATIVE_INFINITY) ?? null;
  inst.maxValue = Math.min(bag.maximum ?? Number.POSITIVE_INFINITY, bag.exclusiveMaximum ?? Number.POSITIVE_INFINITY) ?? null;
  inst.isInt = (bag.format ?? "").includes("int") || Number.isSafeInteger(bag.multipleOf ?? 0.5);
  inst.isFinite = true;
  inst.format = bag.format ?? null;
});
function number2(params) {
  return _number(ZodNumber, params);
}
var ZodNumberFormat = /* @__PURE__ */ $constructor("ZodNumberFormat", (inst, def) => {
  $ZodNumberFormat.init(inst, def);
  ZodNumber.init(inst, def);
});
function int(params) {
  return _int(ZodNumberFormat, params);
}
function float32(params) {
  return _float32(ZodNumberFormat, params);
}
function float64(params) {
  return _float64(ZodNumberFormat, params);
}
function int32(params) {
  return _int32(ZodNumberFormat, params);
}
function uint32(params) {
  return _uint32(ZodNumberFormat, params);
}
var ZodBoolean = /* @__PURE__ */ $constructor("ZodBoolean", (inst, def) => {
  $ZodBoolean.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => booleanProcessor(inst, ctx, json2, params);
});
function boolean2(params) {
  return _boolean(ZodBoolean, params);
}
var ZodBigInt = /* @__PURE__ */ $constructor("ZodBigInt", (inst, def) => {
  $ZodBigInt.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => bigintProcessor(inst, ctx, json2, params);
  inst.gte = (value, params) => inst.check(_gte(value, params));
  inst.min = (value, params) => inst.check(_gte(value, params));
  inst.gt = (value, params) => inst.check(_gt(value, params));
  inst.gte = (value, params) => inst.check(_gte(value, params));
  inst.min = (value, params) => inst.check(_gte(value, params));
  inst.lt = (value, params) => inst.check(_lt(value, params));
  inst.lte = (value, params) => inst.check(_lte(value, params));
  inst.max = (value, params) => inst.check(_lte(value, params));
  inst.positive = (params) => inst.check(_gt(BigInt(0), params));
  inst.negative = (params) => inst.check(_lt(BigInt(0), params));
  inst.nonpositive = (params) => inst.check(_lte(BigInt(0), params));
  inst.nonnegative = (params) => inst.check(_gte(BigInt(0), params));
  inst.multipleOf = (value, params) => inst.check(_multipleOf(value, params));
  const bag = inst._zod.bag;
  inst.minValue = bag.minimum ?? null;
  inst.maxValue = bag.maximum ?? null;
  inst.format = bag.format ?? null;
});
function bigint2(params) {
  return _bigint(ZodBigInt, params);
}
var ZodBigIntFormat = /* @__PURE__ */ $constructor("ZodBigIntFormat", (inst, def) => {
  $ZodBigIntFormat.init(inst, def);
  ZodBigInt.init(inst, def);
});
function int64(params) {
  return _int64(ZodBigIntFormat, params);
}
function uint64(params) {
  return _uint64(ZodBigIntFormat, params);
}
var ZodSymbol = /* @__PURE__ */ $constructor("ZodSymbol", (inst, def) => {
  $ZodSymbol.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => symbolProcessor(inst, ctx, json2, params);
});
function symbol(params) {
  return _symbol(ZodSymbol, params);
}
var ZodUndefined = /* @__PURE__ */ $constructor("ZodUndefined", (inst, def) => {
  $ZodUndefined.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => undefinedProcessor(inst, ctx, json2, params);
});
function _undefined3(params) {
  return _undefined2(ZodUndefined, params);
}
var ZodNull = /* @__PURE__ */ $constructor("ZodNull", (inst, def) => {
  $ZodNull.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => nullProcessor(inst, ctx, json2, params);
});
function _null3(params) {
  return _null2(ZodNull, params);
}
var ZodAny = /* @__PURE__ */ $constructor("ZodAny", (inst, def) => {
  $ZodAny.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => anyProcessor(inst, ctx, json2, params);
});
function any() {
  return _any(ZodAny);
}
var ZodUnknown = /* @__PURE__ */ $constructor("ZodUnknown", (inst, def) => {
  $ZodUnknown.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => unknownProcessor(inst, ctx, json2, params);
});
function unknown() {
  return _unknown(ZodUnknown);
}
var ZodNever = /* @__PURE__ */ $constructor("ZodNever", (inst, def) => {
  $ZodNever.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => neverProcessor(inst, ctx, json2, params);
});
function never(params) {
  return _never(ZodNever, params);
}
var ZodVoid = /* @__PURE__ */ $constructor("ZodVoid", (inst, def) => {
  $ZodVoid.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => voidProcessor(inst, ctx, json2, params);
});
function _void2(params) {
  return _void(ZodVoid, params);
}
var ZodDate = /* @__PURE__ */ $constructor("ZodDate", (inst, def) => {
  $ZodDate.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => dateProcessor(inst, ctx, json2, params);
  inst.min = (value, params) => inst.check(_gte(value, params));
  inst.max = (value, params) => inst.check(_lte(value, params));
  const c = inst._zod.bag;
  inst.minDate = c.minimum ? new Date(c.minimum) : null;
  inst.maxDate = c.maximum ? new Date(c.maximum) : null;
});
function date3(params) {
  return _date(ZodDate, params);
}
var ZodArray = /* @__PURE__ */ $constructor("ZodArray", (inst, def) => {
  $ZodArray.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => arrayProcessor(inst, ctx, json2, params);
  inst.element = def.element;
  _installLazyMethods(inst, "ZodArray", {
    min(n, params) {
      return this.check(_minLength(n, params));
    },
    nonempty(params) {
      return this.check(_minLength(1, params));
    },
    max(n, params) {
      return this.check(_maxLength(n, params));
    },
    length(n, params) {
      return this.check(_length(n, params));
    },
    unwrap() {
      return this.element;
    }
  });
});
function array(element, params) {
  return _array(ZodArray, element, params);
}
function keyof(schema) {
  const shape = schema._zod.def.shape;
  return _enum2(Object.keys(shape));
}
var ZodObject = /* @__PURE__ */ $constructor("ZodObject", (inst, def) => {
  $ZodObjectJIT.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => objectProcessor(inst, ctx, json2, params);
  util_exports.defineLazy(inst, "shape", () => {
    return def.shape;
  });
  _installLazyMethods(inst, "ZodObject", {
    keyof() {
      return _enum2(Object.keys(this._zod.def.shape));
    },
    catchall(catchall) {
      return this.clone({ ...this._zod.def, catchall });
    },
    passthrough() {
      return this.clone({ ...this._zod.def, catchall: unknown() });
    },
    loose() {
      return this.clone({ ...this._zod.def, catchall: unknown() });
    },
    strict() {
      return this.clone({ ...this._zod.def, catchall: never() });
    },
    strip() {
      return this.clone({ ...this._zod.def, catchall: void 0 });
    },
    extend(incoming) {
      return util_exports.extend(this, incoming);
    },
    safeExtend(incoming) {
      return util_exports.safeExtend(this, incoming);
    },
    merge(other) {
      return util_exports.merge(this, other);
    },
    pick(mask) {
      return util_exports.pick(this, mask);
    },
    omit(mask) {
      return util_exports.omit(this, mask);
    },
    partial(...args) {
      return util_exports.partial(ZodOptional, this, args[0]);
    },
    required(...args) {
      return util_exports.required(ZodNonOptional, this, args[0]);
    }
  });
});
function object(shape, params) {
  const def = {
    type: "object",
    shape: shape ?? {},
    ...util_exports.normalizeParams(params)
  };
  return new ZodObject(def);
}
function strictObject(shape, params) {
  return new ZodObject({
    type: "object",
    shape,
    catchall: never(),
    ...util_exports.normalizeParams(params)
  });
}
function looseObject(shape, params) {
  return new ZodObject({
    type: "object",
    shape,
    catchall: unknown(),
    ...util_exports.normalizeParams(params)
  });
}
var ZodUnion = /* @__PURE__ */ $constructor("ZodUnion", (inst, def) => {
  $ZodUnion.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => unionProcessor(inst, ctx, json2, params);
  inst.options = def.options;
});
function union(options, params) {
  return new ZodUnion({
    type: "union",
    options,
    ...util_exports.normalizeParams(params)
  });
}
var ZodXor = /* @__PURE__ */ $constructor("ZodXor", (inst, def) => {
  ZodUnion.init(inst, def);
  $ZodXor.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => unionProcessor(inst, ctx, json2, params);
  inst.options = def.options;
});
function xor(options, params) {
  return new ZodXor({
    type: "union",
    options,
    inclusive: false,
    ...util_exports.normalizeParams(params)
  });
}
var ZodDiscriminatedUnion = /* @__PURE__ */ $constructor("ZodDiscriminatedUnion", (inst, def) => {
  ZodUnion.init(inst, def);
  $ZodDiscriminatedUnion.init(inst, def);
});
function discriminatedUnion(discriminator, options, params) {
  return new ZodDiscriminatedUnion({
    type: "union",
    options,
    discriminator,
    ...util_exports.normalizeParams(params)
  });
}
var ZodIntersection = /* @__PURE__ */ $constructor("ZodIntersection", (inst, def) => {
  $ZodIntersection.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => intersectionProcessor(inst, ctx, json2, params);
});
function intersection(left, right) {
  return new ZodIntersection({
    type: "intersection",
    left,
    right
  });
}
var ZodTuple = /* @__PURE__ */ $constructor("ZodTuple", (inst, def) => {
  $ZodTuple.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => tupleProcessor(inst, ctx, json2, params);
  inst.rest = (rest) => inst.clone({
    ...inst._zod.def,
    rest
  });
});
function tuple(items, _paramsOrRest, _params) {
  const hasRest = _paramsOrRest instanceof $ZodType;
  const params = hasRest ? _params : _paramsOrRest;
  const rest = hasRest ? _paramsOrRest : null;
  return new ZodTuple({
    type: "tuple",
    items,
    rest,
    ...util_exports.normalizeParams(params)
  });
}
var ZodRecord = /* @__PURE__ */ $constructor("ZodRecord", (inst, def) => {
  $ZodRecord.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => recordProcessor(inst, ctx, json2, params);
  inst.keyType = def.keyType;
  inst.valueType = def.valueType;
});
function record(keyType, valueType, params) {
  if (!valueType || !valueType._zod) {
    return new ZodRecord({
      type: "record",
      keyType: string2(),
      valueType: keyType,
      ...util_exports.normalizeParams(valueType)
    });
  }
  return new ZodRecord({
    type: "record",
    keyType,
    valueType,
    ...util_exports.normalizeParams(params)
  });
}
function partialRecord(keyType, valueType, params) {
  const k = clone(keyType);
  k._zod.values = void 0;
  return new ZodRecord({
    type: "record",
    keyType: k,
    valueType,
    ...util_exports.normalizeParams(params)
  });
}
function looseRecord(keyType, valueType, params) {
  return new ZodRecord({
    type: "record",
    keyType,
    valueType,
    mode: "loose",
    ...util_exports.normalizeParams(params)
  });
}
var ZodMap = /* @__PURE__ */ $constructor("ZodMap", (inst, def) => {
  $ZodMap.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => mapProcessor(inst, ctx, json2, params);
  inst.keyType = def.keyType;
  inst.valueType = def.valueType;
  inst.min = (...args) => inst.check(_minSize(...args));
  inst.nonempty = (params) => inst.check(_minSize(1, params));
  inst.max = (...args) => inst.check(_maxSize(...args));
  inst.size = (...args) => inst.check(_size(...args));
});
function map(keyType, valueType, params) {
  return new ZodMap({
    type: "map",
    keyType,
    valueType,
    ...util_exports.normalizeParams(params)
  });
}
var ZodSet = /* @__PURE__ */ $constructor("ZodSet", (inst, def) => {
  $ZodSet.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => setProcessor(inst, ctx, json2, params);
  inst.min = (...args) => inst.check(_minSize(...args));
  inst.nonempty = (params) => inst.check(_minSize(1, params));
  inst.max = (...args) => inst.check(_maxSize(...args));
  inst.size = (...args) => inst.check(_size(...args));
});
function set(valueType, params) {
  return new ZodSet({
    type: "set",
    valueType,
    ...util_exports.normalizeParams(params)
  });
}
var ZodEnum = /* @__PURE__ */ $constructor("ZodEnum", (inst, def) => {
  $ZodEnum.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => enumProcessor(inst, ctx, json2, params);
  inst.enum = def.entries;
  inst.options = Object.values(def.entries);
  const keys = new Set(Object.keys(def.entries));
  inst.extract = (values, params) => {
    const newEntries = {};
    for (const value of values) {
      if (keys.has(value)) {
        newEntries[value] = def.entries[value];
      } else
        throw new Error(`Key ${value} not found in enum`);
    }
    return new ZodEnum({
      ...def,
      checks: [],
      ...util_exports.normalizeParams(params),
      entries: newEntries
    });
  };
  inst.exclude = (values, params) => {
    const newEntries = { ...def.entries };
    for (const value of values) {
      if (keys.has(value)) {
        delete newEntries[value];
      } else
        throw new Error(`Key ${value} not found in enum`);
    }
    return new ZodEnum({
      ...def,
      checks: [],
      ...util_exports.normalizeParams(params),
      entries: newEntries
    });
  };
});
function _enum2(values, params) {
  const entries = Array.isArray(values) ? Object.fromEntries(values.map((v) => [v, v])) : values;
  return new ZodEnum({
    type: "enum",
    entries,
    ...util_exports.normalizeParams(params)
  });
}
function nativeEnum(entries, params) {
  return new ZodEnum({
    type: "enum",
    entries,
    ...util_exports.normalizeParams(params)
  });
}
var ZodLiteral = /* @__PURE__ */ $constructor("ZodLiteral", (inst, def) => {
  $ZodLiteral.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => literalProcessor(inst, ctx, json2, params);
  inst.values = new Set(def.values);
  Object.defineProperty(inst, "value", {
    get() {
      if (def.values.length > 1) {
        throw new Error("This schema contains multiple valid literal values. Use `.values` instead.");
      }
      return def.values[0];
    }
  });
});
function literal(value, params) {
  return new ZodLiteral({
    type: "literal",
    values: Array.isArray(value) ? value : [value],
    ...util_exports.normalizeParams(params)
  });
}
var ZodFile = /* @__PURE__ */ $constructor("ZodFile", (inst, def) => {
  $ZodFile.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => fileProcessor(inst, ctx, json2, params);
  inst.min = (size, params) => inst.check(_minSize(size, params));
  inst.max = (size, params) => inst.check(_maxSize(size, params));
  inst.mime = (types, params) => inst.check(_mime(Array.isArray(types) ? types : [types], params));
});
function file(params) {
  return _file(ZodFile, params);
}
var ZodTransform = /* @__PURE__ */ $constructor("ZodTransform", (inst, def) => {
  $ZodTransform.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => transformProcessor(inst, ctx, json2, params);
  inst._zod.parse = (payload, _ctx) => {
    if (_ctx.direction === "backward") {
      throw new $ZodEncodeError(inst.constructor.name);
    }
    payload.addIssue = (issue2) => {
      if (typeof issue2 === "string") {
        payload.issues.push(util_exports.issue(issue2, payload.value, def));
      } else {
        const _issue = issue2;
        if (_issue.fatal)
          _issue.continue = false;
        _issue.code ?? (_issue.code = "custom");
        _issue.input ?? (_issue.input = payload.value);
        _issue.inst ?? (_issue.inst = inst);
        payload.issues.push(util_exports.issue(_issue));
      }
    };
    const output = def.transform(payload.value, payload);
    if (output instanceof Promise) {
      return output.then((output2) => {
        payload.value = output2;
        payload.fallback = true;
        return payload;
      });
    }
    payload.value = output;
    payload.fallback = true;
    return payload;
  };
});
function transform(fn) {
  return new ZodTransform({
    type: "transform",
    transform: fn
  });
}
var ZodOptional = /* @__PURE__ */ $constructor("ZodOptional", (inst, def) => {
  $ZodOptional.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => optionalProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
});
function optional(innerType) {
  return new ZodOptional({
    type: "optional",
    innerType
  });
}
var ZodExactOptional = /* @__PURE__ */ $constructor("ZodExactOptional", (inst, def) => {
  $ZodExactOptional.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => optionalProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
});
function exactOptional(innerType) {
  return new ZodExactOptional({
    type: "optional",
    innerType
  });
}
var ZodNullable = /* @__PURE__ */ $constructor("ZodNullable", (inst, def) => {
  $ZodNullable.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => nullableProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
});
function nullable(innerType) {
  return new ZodNullable({
    type: "nullable",
    innerType
  });
}
function nullish2(innerType) {
  return optional(nullable(innerType));
}
var ZodDefault = /* @__PURE__ */ $constructor("ZodDefault", (inst, def) => {
  $ZodDefault.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => defaultProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
  inst.removeDefault = inst.unwrap;
});
function _default2(innerType, defaultValue) {
  return new ZodDefault({
    type: "default",
    innerType,
    get defaultValue() {
      return typeof defaultValue === "function" ? defaultValue() : util_exports.shallowClone(defaultValue);
    }
  });
}
var ZodPrefault = /* @__PURE__ */ $constructor("ZodPrefault", (inst, def) => {
  $ZodPrefault.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => prefaultProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
});
function prefault(innerType, defaultValue) {
  return new ZodPrefault({
    type: "prefault",
    innerType,
    get defaultValue() {
      return typeof defaultValue === "function" ? defaultValue() : util_exports.shallowClone(defaultValue);
    }
  });
}
var ZodNonOptional = /* @__PURE__ */ $constructor("ZodNonOptional", (inst, def) => {
  $ZodNonOptional.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => nonoptionalProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
});
function nonoptional(innerType, params) {
  return new ZodNonOptional({
    type: "nonoptional",
    innerType,
    ...util_exports.normalizeParams(params)
  });
}
var ZodSuccess = /* @__PURE__ */ $constructor("ZodSuccess", (inst, def) => {
  $ZodSuccess.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => successProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
});
function success(innerType) {
  return new ZodSuccess({
    type: "success",
    innerType
  });
}
var ZodCatch = /* @__PURE__ */ $constructor("ZodCatch", (inst, def) => {
  $ZodCatch.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => catchProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
  inst.removeCatch = inst.unwrap;
});
function _catch2(innerType, catchValue) {
  return new ZodCatch({
    type: "catch",
    innerType,
    catchValue: typeof catchValue === "function" ? catchValue : () => catchValue
  });
}
var ZodNaN = /* @__PURE__ */ $constructor("ZodNaN", (inst, def) => {
  $ZodNaN.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => nanProcessor(inst, ctx, json2, params);
});
function nan(params) {
  return _nan(ZodNaN, params);
}
var ZodPipe = /* @__PURE__ */ $constructor("ZodPipe", (inst, def) => {
  $ZodPipe.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => pipeProcessor(inst, ctx, json2, params);
  inst.in = def.in;
  inst.out = def.out;
});
function pipe(in_, out) {
  return new ZodPipe({
    type: "pipe",
    in: in_,
    out
    // ...util.normalizeParams(params),
  });
}
var ZodCodec = /* @__PURE__ */ $constructor("ZodCodec", (inst, def) => {
  ZodPipe.init(inst, def);
  $ZodCodec.init(inst, def);
});
function codec(in_, out, params) {
  return new ZodCodec({
    type: "pipe",
    in: in_,
    out,
    transform: params.decode,
    reverseTransform: params.encode
  });
}
function invertCodec(codec2) {
  const def = codec2._zod.def;
  return new ZodCodec({
    type: "pipe",
    in: def.out,
    out: def.in,
    transform: def.reverseTransform,
    reverseTransform: def.transform
  });
}
var ZodPreprocess = /* @__PURE__ */ $constructor("ZodPreprocess", (inst, def) => {
  ZodPipe.init(inst, def);
  $ZodPreprocess.init(inst, def);
});
var ZodReadonly = /* @__PURE__ */ $constructor("ZodReadonly", (inst, def) => {
  $ZodReadonly.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => readonlyProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
});
function readonly(innerType) {
  return new ZodReadonly({
    type: "readonly",
    innerType
  });
}
var ZodTemplateLiteral = /* @__PURE__ */ $constructor("ZodTemplateLiteral", (inst, def) => {
  $ZodTemplateLiteral.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => templateLiteralProcessor(inst, ctx, json2, params);
});
function templateLiteral(parts, params) {
  return new ZodTemplateLiteral({
    type: "template_literal",
    parts,
    ...util_exports.normalizeParams(params)
  });
}
var ZodLazy = /* @__PURE__ */ $constructor("ZodLazy", (inst, def) => {
  $ZodLazy.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => lazyProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.getter();
});
function lazy(getter) {
  return new ZodLazy({
    type: "lazy",
    getter
  });
}
var ZodPromise = /* @__PURE__ */ $constructor("ZodPromise", (inst, def) => {
  $ZodPromise.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => promiseProcessor(inst, ctx, json2, params);
  inst.unwrap = () => inst._zod.def.innerType;
});
function promise(innerType) {
  return new ZodPromise({
    type: "promise",
    innerType
  });
}
var ZodFunction = /* @__PURE__ */ $constructor("ZodFunction", (inst, def) => {
  $ZodFunction.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => functionProcessor(inst, ctx, json2, params);
});
function _function(params) {
  return new ZodFunction({
    type: "function",
    input: Array.isArray(params?.input) ? tuple(params?.input) : params?.input ?? array(unknown()),
    output: params?.output ?? unknown()
  });
}
var ZodCustom = /* @__PURE__ */ $constructor("ZodCustom", (inst, def) => {
  $ZodCustom.init(inst, def);
  ZodType.init(inst, def);
  inst._zod.processJSONSchema = (ctx, json2, params) => customProcessor(inst, ctx, json2, params);
});
function check(fn) {
  const ch = new $ZodCheck({
    check: "custom"
    // ...util.normalizeParams(params),
  });
  ch._zod.check = fn;
  return ch;
}
function custom(fn, _params) {
  return _custom(ZodCustom, fn ?? (() => true), _params);
}
function refine(fn, _params = {}) {
  return _refine(ZodCustom, fn, _params);
}
function superRefine(fn, params) {
  return _superRefine(fn, params);
}
var describe2 = describe;
var meta2 = meta;
function _instanceof(cls, params = {}) {
  const inst = new ZodCustom({
    type: "custom",
    check: "custom",
    fn: (data) => data instanceof cls,
    abort: true,
    ...util_exports.normalizeParams(params)
  });
  inst._zod.bag.Class = cls;
  inst._zod.check = (payload) => {
    if (!(payload.value instanceof cls)) {
      payload.issues.push({
        code: "invalid_type",
        expected: cls.name,
        input: payload.value,
        inst,
        path: [...inst._zod.def.path ?? []]
      });
    }
  };
  return inst;
}
var stringbool = (...args) => _stringbool({
  Codec: ZodCodec,
  Boolean: ZodBoolean,
  String: ZodString
}, ...args);
function json(params) {
  const jsonSchema = lazy(() => {
    return union([string2(params), number2(), boolean2(), _null3(), array(jsonSchema), record(string2(), jsonSchema)]);
  });
  return jsonSchema;
}
function preprocess(fn, schema) {
  return new ZodPreprocess({
    type: "pipe",
    in: transform(fn),
    out: schema
  });
}

// node_modules/zod/v4/classic/compat.js
var ZodIssueCode = {
  invalid_type: "invalid_type",
  too_big: "too_big",
  too_small: "too_small",
  invalid_format: "invalid_format",
  not_multiple_of: "not_multiple_of",
  unrecognized_keys: "unrecognized_keys",
  invalid_union: "invalid_union",
  invalid_key: "invalid_key",
  invalid_element: "invalid_element",
  invalid_value: "invalid_value",
  custom: "custom"
};
function setErrorMap(map2) {
  config({
    customError: map2
  });
}
function getErrorMap() {
  return config().customError;
}
var ZodFirstPartyTypeKind;
/* @__PURE__ */ (function(ZodFirstPartyTypeKind2) {
})(ZodFirstPartyTypeKind || (ZodFirstPartyTypeKind = {}));

// node_modules/zod/v4/classic/from-json-schema.js
var z = {
  ...schemas_exports2,
  ...checks_exports2,
  iso: iso_exports
};
var RECOGNIZED_KEYS = /* @__PURE__ */ new Set([
  // Schema identification
  "$schema",
  "$ref",
  "$defs",
  "definitions",
  // Core schema keywords
  "$id",
  "id",
  "$comment",
  "$anchor",
  "$vocabulary",
  "$dynamicRef",
  "$dynamicAnchor",
  // Type
  "type",
  "enum",
  "const",
  // Composition
  "anyOf",
  "oneOf",
  "allOf",
  "not",
  // Object
  "properties",
  "required",
  "additionalProperties",
  "patternProperties",
  "propertyNames",
  "minProperties",
  "maxProperties",
  // Array
  "items",
  "prefixItems",
  "additionalItems",
  "minItems",
  "maxItems",
  "uniqueItems",
  "contains",
  "minContains",
  "maxContains",
  // String
  "minLength",
  "maxLength",
  "pattern",
  "format",
  // Number
  "minimum",
  "maximum",
  "exclusiveMinimum",
  "exclusiveMaximum",
  "multipleOf",
  // Already handled metadata
  "description",
  "default",
  // Content
  "contentEncoding",
  "contentMediaType",
  "contentSchema",
  // Unsupported (error-throwing)
  "unevaluatedItems",
  "unevaluatedProperties",
  "if",
  "then",
  "else",
  "dependentSchemas",
  "dependentRequired",
  // OpenAPI
  "nullable",
  "readOnly"
]);
function detectVersion(schema, defaultTarget) {
  const $schema = schema.$schema;
  if ($schema === "https://json-schema.org/draft/2020-12/schema") {
    return "draft-2020-12";
  }
  if ($schema === "http://json-schema.org/draft-07/schema#") {
    return "draft-7";
  }
  if ($schema === "http://json-schema.org/draft-04/schema#") {
    return "draft-4";
  }
  return defaultTarget ?? "draft-2020-12";
}
function resolveRef(ref, ctx) {
  if (!ref.startsWith("#")) {
    throw new Error("External $ref is not supported, only local refs (#/...) are allowed");
  }
  const path = ref.slice(1).split("/").filter(Boolean);
  if (path.length === 0) {
    return ctx.rootSchema;
  }
  const defsKey = ctx.version === "draft-2020-12" ? "$defs" : "definitions";
  if (path[0] === defsKey) {
    const key = path[1];
    if (!key || !ctx.defs[key]) {
      throw new Error(`Reference not found: ${ref}`);
    }
    return ctx.defs[key];
  }
  throw new Error(`Reference not found: ${ref}`);
}
function convertBaseSchema(schema, ctx) {
  if (schema.not !== void 0) {
    if (typeof schema.not === "object" && Object.keys(schema.not).length === 0) {
      return z.never();
    }
    throw new Error("not is not supported in Zod (except { not: {} } for never)");
  }
  if (schema.unevaluatedItems !== void 0) {
    throw new Error("unevaluatedItems is not supported");
  }
  if (schema.unevaluatedProperties !== void 0) {
    throw new Error("unevaluatedProperties is not supported");
  }
  if (schema.if !== void 0 || schema.then !== void 0 || schema.else !== void 0) {
    throw new Error("Conditional schemas (if/then/else) are not supported");
  }
  if (schema.dependentSchemas !== void 0 || schema.dependentRequired !== void 0) {
    throw new Error("dependentSchemas and dependentRequired are not supported");
  }
  if (schema.$ref) {
    const refPath = schema.$ref;
    if (ctx.refs.has(refPath)) {
      return ctx.refs.get(refPath);
    }
    if (ctx.processing.has(refPath)) {
      return z.lazy(() => {
        if (!ctx.refs.has(refPath)) {
          throw new Error(`Circular reference not resolved: ${refPath}`);
        }
        return ctx.refs.get(refPath);
      });
    }
    ctx.processing.add(refPath);
    const resolved = resolveRef(refPath, ctx);
    const zodSchema2 = convertSchema(resolved, ctx);
    ctx.refs.set(refPath, zodSchema2);
    ctx.processing.delete(refPath);
    return zodSchema2;
  }
  if (schema.enum !== void 0) {
    const enumValues = schema.enum;
    if (ctx.version === "openapi-3.0" && schema.nullable === true && enumValues.length === 1 && enumValues[0] === null) {
      return z.null();
    }
    if (enumValues.length === 0) {
      return z.never();
    }
    if (enumValues.length === 1) {
      return z.literal(enumValues[0]);
    }
    if (enumValues.every((v) => typeof v === "string")) {
      return z.enum(enumValues);
    }
    const literalSchemas = enumValues.map((v) => z.literal(v));
    if (literalSchemas.length < 2) {
      return literalSchemas[0];
    }
    return z.union([literalSchemas[0], literalSchemas[1], ...literalSchemas.slice(2)]);
  }
  if (schema.const !== void 0) {
    return z.literal(schema.const);
  }
  const type = schema.type;
  if (Array.isArray(type)) {
    const typeSchemas = type.map((t) => {
      const typeSchema = { ...schema, type: t };
      return convertBaseSchema(typeSchema, ctx);
    });
    if (typeSchemas.length === 0) {
      return z.never();
    }
    if (typeSchemas.length === 1) {
      return typeSchemas[0];
    }
    return z.union(typeSchemas);
  }
  if (!type) {
    return z.any();
  }
  let zodSchema;
  switch (type) {
    case "string": {
      let stringSchema = z.string();
      if (schema.format) {
        const format = schema.format;
        if (format === "email") {
          stringSchema = stringSchema.check(z.email());
        } else if (format === "uri" || format === "uri-reference") {
          stringSchema = stringSchema.check(z.url());
        } else if (format === "uuid" || format === "guid") {
          stringSchema = stringSchema.check(z.uuid());
        } else if (format === "date-time") {
          stringSchema = stringSchema.check(z.iso.datetime());
        } else if (format === "date") {
          stringSchema = stringSchema.check(z.iso.date());
        } else if (format === "time") {
          stringSchema = stringSchema.check(z.iso.time());
        } else if (format === "duration") {
          stringSchema = stringSchema.check(z.iso.duration());
        } else if (format === "ipv4") {
          stringSchema = stringSchema.check(z.ipv4());
        } else if (format === "ipv6") {
          stringSchema = stringSchema.check(z.ipv6());
        } else if (format === "mac") {
          stringSchema = stringSchema.check(z.mac());
        } else if (format === "cidr") {
          stringSchema = stringSchema.check(z.cidrv4());
        } else if (format === "cidr-v6") {
          stringSchema = stringSchema.check(z.cidrv6());
        } else if (format === "base64") {
          stringSchema = stringSchema.check(z.base64());
        } else if (format === "base64url") {
          stringSchema = stringSchema.check(z.base64url());
        } else if (format === "e164") {
          stringSchema = stringSchema.check(z.e164());
        } else if (format === "jwt") {
          stringSchema = stringSchema.check(z.jwt());
        } else if (format === "emoji") {
          stringSchema = stringSchema.check(z.emoji());
        } else if (format === "nanoid") {
          stringSchema = stringSchema.check(z.nanoid());
        } else if (format === "cuid") {
          stringSchema = stringSchema.check(z.cuid());
        } else if (format === "cuid2") {
          stringSchema = stringSchema.check(z.cuid2());
        } else if (format === "ulid") {
          stringSchema = stringSchema.check(z.ulid());
        } else if (format === "xid") {
          stringSchema = stringSchema.check(z.xid());
        } else if (format === "ksuid") {
          stringSchema = stringSchema.check(z.ksuid());
        }
      }
      if (typeof schema.minLength === "number") {
        stringSchema = stringSchema.min(schema.minLength);
      }
      if (typeof schema.maxLength === "number") {
        stringSchema = stringSchema.max(schema.maxLength);
      }
      if (schema.pattern) {
        stringSchema = stringSchema.regex(new RegExp(schema.pattern));
      }
      zodSchema = stringSchema;
      break;
    }
    case "number":
    case "integer": {
      let numberSchema = type === "integer" ? z.number().int() : z.number();
      if (typeof schema.minimum === "number") {
        numberSchema = numberSchema.min(schema.minimum);
      }
      if (typeof schema.maximum === "number") {
        numberSchema = numberSchema.max(schema.maximum);
      }
      if (typeof schema.exclusiveMinimum === "number") {
        numberSchema = numberSchema.gt(schema.exclusiveMinimum);
      } else if (schema.exclusiveMinimum === true && typeof schema.minimum === "number") {
        numberSchema = numberSchema.gt(schema.minimum);
      }
      if (typeof schema.exclusiveMaximum === "number") {
        numberSchema = numberSchema.lt(schema.exclusiveMaximum);
      } else if (schema.exclusiveMaximum === true && typeof schema.maximum === "number") {
        numberSchema = numberSchema.lt(schema.maximum);
      }
      if (typeof schema.multipleOf === "number") {
        numberSchema = numberSchema.multipleOf(schema.multipleOf);
      }
      zodSchema = numberSchema;
      break;
    }
    case "boolean": {
      zodSchema = z.boolean();
      break;
    }
    case "null": {
      zodSchema = z.null();
      break;
    }
    case "object": {
      const shape = {};
      const properties = schema.properties || {};
      const requiredSet = new Set(schema.required || []);
      for (const [key, propSchema] of Object.entries(properties)) {
        const propZodSchema = convertSchema(propSchema, ctx);
        shape[key] = requiredSet.has(key) ? propZodSchema : propZodSchema.optional();
      }
      if (schema.propertyNames) {
        const keySchema = convertSchema(schema.propertyNames, ctx);
        const valueSchema = schema.additionalProperties && typeof schema.additionalProperties === "object" ? convertSchema(schema.additionalProperties, ctx) : z.any();
        if (Object.keys(shape).length === 0) {
          zodSchema = z.record(keySchema, valueSchema);
          break;
        }
        const objectSchema2 = z.object(shape).passthrough();
        const recordSchema = z.looseRecord(keySchema, valueSchema);
        zodSchema = z.intersection(objectSchema2, recordSchema);
        break;
      }
      if (schema.patternProperties) {
        const patternProps = schema.patternProperties;
        const patternKeys = Object.keys(patternProps);
        const looseRecords = [];
        for (const pattern of patternKeys) {
          const patternValue = convertSchema(patternProps[pattern], ctx);
          const keySchema = z.string().regex(new RegExp(pattern));
          looseRecords.push(z.looseRecord(keySchema, patternValue));
        }
        const schemasToIntersect = [];
        if (Object.keys(shape).length > 0) {
          schemasToIntersect.push(z.object(shape).passthrough());
        }
        schemasToIntersect.push(...looseRecords);
        if (schemasToIntersect.length === 0) {
          zodSchema = z.object({}).passthrough();
        } else if (schemasToIntersect.length === 1) {
          zodSchema = schemasToIntersect[0];
        } else {
          let result = z.intersection(schemasToIntersect[0], schemasToIntersect[1]);
          for (let i = 2; i < schemasToIntersect.length; i++) {
            result = z.intersection(result, schemasToIntersect[i]);
          }
          zodSchema = result;
        }
        break;
      }
      const objectSchema = z.object(shape);
      if (schema.additionalProperties === false) {
        zodSchema = objectSchema.strict();
      } else if (typeof schema.additionalProperties === "object") {
        zodSchema = objectSchema.catchall(convertSchema(schema.additionalProperties, ctx));
      } else {
        zodSchema = objectSchema.passthrough();
      }
      break;
    }
    case "array": {
      const prefixItems = schema.prefixItems;
      const items = schema.items;
      if (prefixItems && Array.isArray(prefixItems)) {
        const tupleItems = prefixItems.map((item) => convertSchema(item, ctx));
        const rest = items && typeof items === "object" && !Array.isArray(items) ? convertSchema(items, ctx) : void 0;
        if (rest) {
          zodSchema = z.tuple(tupleItems).rest(rest);
        } else {
          zodSchema = z.tuple(tupleItems);
        }
        if (typeof schema.minItems === "number") {
          zodSchema = zodSchema.check(z.minLength(schema.minItems));
        }
        if (typeof schema.maxItems === "number") {
          zodSchema = zodSchema.check(z.maxLength(schema.maxItems));
        }
      } else if (Array.isArray(items)) {
        const tupleItems = items.map((item) => convertSchema(item, ctx));
        const rest = schema.additionalItems && typeof schema.additionalItems === "object" ? convertSchema(schema.additionalItems, ctx) : void 0;
        if (rest) {
          zodSchema = z.tuple(tupleItems).rest(rest);
        } else {
          zodSchema = z.tuple(tupleItems);
        }
        if (typeof schema.minItems === "number") {
          zodSchema = zodSchema.check(z.minLength(schema.minItems));
        }
        if (typeof schema.maxItems === "number") {
          zodSchema = zodSchema.check(z.maxLength(schema.maxItems));
        }
      } else if (items !== void 0) {
        const element = convertSchema(items, ctx);
        let arraySchema = z.array(element);
        if (typeof schema.minItems === "number") {
          arraySchema = arraySchema.min(schema.minItems);
        }
        if (typeof schema.maxItems === "number") {
          arraySchema = arraySchema.max(schema.maxItems);
        }
        zodSchema = arraySchema;
      } else {
        zodSchema = z.array(z.any());
      }
      break;
    }
    default:
      throw new Error(`Unsupported type: ${type}`);
  }
  return zodSchema;
}
function convertSchema(schema, ctx) {
  if (typeof schema === "boolean") {
    return schema ? z.any() : z.never();
  }
  let baseSchema = convertBaseSchema(schema, ctx);
  const hasExplicitType = schema.type || schema.enum !== void 0 || schema.const !== void 0;
  if (schema.anyOf && Array.isArray(schema.anyOf)) {
    const options = schema.anyOf.map((s) => convertSchema(s, ctx));
    const anyOfUnion = z.union(options);
    baseSchema = hasExplicitType ? z.intersection(baseSchema, anyOfUnion) : anyOfUnion;
  }
  if (schema.oneOf && Array.isArray(schema.oneOf)) {
    const options = schema.oneOf.map((s) => convertSchema(s, ctx));
    const oneOfUnion = z.xor(options);
    baseSchema = hasExplicitType ? z.intersection(baseSchema, oneOfUnion) : oneOfUnion;
  }
  if (schema.allOf && Array.isArray(schema.allOf)) {
    if (schema.allOf.length === 0) {
      baseSchema = hasExplicitType ? baseSchema : z.any();
    } else {
      let result = hasExplicitType ? baseSchema : convertSchema(schema.allOf[0], ctx);
      const startIdx = hasExplicitType ? 0 : 1;
      for (let i = startIdx; i < schema.allOf.length; i++) {
        result = z.intersection(result, convertSchema(schema.allOf[i], ctx));
      }
      baseSchema = result;
    }
  }
  if (schema.nullable === true && ctx.version === "openapi-3.0") {
    baseSchema = z.nullable(baseSchema);
  }
  if (schema.readOnly === true) {
    baseSchema = z.readonly(baseSchema);
  }
  if (schema.default !== void 0) {
    baseSchema = baseSchema.default(schema.default);
  }
  const extraMeta = {};
  const coreMetadataKeys = ["$id", "id", "$comment", "$anchor", "$vocabulary", "$dynamicRef", "$dynamicAnchor"];
  for (const key of coreMetadataKeys) {
    if (key in schema) {
      extraMeta[key] = schema[key];
    }
  }
  const contentMetadataKeys = ["contentEncoding", "contentMediaType", "contentSchema"];
  for (const key of contentMetadataKeys) {
    if (key in schema) {
      extraMeta[key] = schema[key];
    }
  }
  for (const key of Object.keys(schema)) {
    if (!RECOGNIZED_KEYS.has(key)) {
      extraMeta[key] = schema[key];
    }
  }
  if (Object.keys(extraMeta).length > 0) {
    ctx.registry.add(baseSchema, extraMeta);
  }
  if (schema.description) {
    baseSchema = baseSchema.describe(schema.description);
  }
  return baseSchema;
}
function fromJSONSchema(schema, params) {
  if (typeof schema === "boolean") {
    return schema ? z.any() : z.never();
  }
  let normalized;
  try {
    normalized = JSON.parse(JSON.stringify(schema));
  } catch {
    throw new Error("fromJSONSchema input is not valid JSON (possibly cyclic); use $defs/$ref for recursive schemas");
  }
  const version2 = detectVersion(normalized, params?.defaultTarget);
  const defs = normalized.$defs || normalized.definitions || {};
  const ctx = {
    version: version2,
    defs,
    refs: /* @__PURE__ */ new Map(),
    processing: /* @__PURE__ */ new Set(),
    rootSchema: normalized,
    registry: params?.registry ?? globalRegistry
  };
  return convertSchema(normalized, ctx);
}

// node_modules/zod/v4/classic/coerce.js
var coerce_exports = {};
__export(coerce_exports, {
  bigint: () => bigint3,
  boolean: () => boolean3,
  date: () => date4,
  number: () => number3,
  string: () => string3
});
function string3(params) {
  return _coercedString(ZodString, params);
}
function number3(params) {
  return _coercedNumber(ZodNumber, params);
}
function boolean3(params) {
  return _coercedBoolean(ZodBoolean, params);
}
function bigint3(params) {
  return _coercedBigint(ZodBigInt, params);
}
function date4(params) {
  return _coercedDate(ZodDate, params);
}

// node_modules/zod/v4/classic/external.js
config(en_default());

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
function isPromiseLike(value) {
  return value !== null && typeof value === "object" && typeof value.then === "function";
}
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
function noexcept(callback, message) {
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
async function noexceptFetch(input, init) {
  try {
    return await globalThis.fetch(input, init);
  } catch (err) {
    if (typeof DOMException !== "undefined" && err instanceof DOMException && err.name === "AbortError") {
      return abortedError(err.message, [err], err);
    }
    if (err instanceof TypeError && err.message.includes("Failed to fetch")) {
      return internalError(
        `${err.message}. This is likely due to a network issue or CORS policy. Check console for more details.`,
        [],
        err
      );
    }
    return statusFromUnknown(err, void 0, 14 /* UNAVAILABLE */);
  }
}
function statusToJson(status) {
  return {
    code: status.code,
    message: status.message,
    details: status.details ? [...status.details] : []
  };
}
function statusFromJson(value) {
  try {
    if (typeof value !== "object" || value === null) {
      return invalidArgumentError("JSON does not contain a valid Status.");
    }
    const candidate = value;
    if (!Number.isInteger(candidate.code) || candidate.code < 0 /* OK */ || candidate.code > 16 /* UNAUTHENTICATED */ || typeof candidate.message !== "string") {
      return invalidArgumentError("JSON does not contain a valid Status.");
    }
    const details = candidate.details ?? [];
    if (!Array.isArray(details) || details.some(
      (detail) => typeof detail !== "object" || detail === null
    )) {
      return invalidArgumentError("Status details must be an array of objects.");
    }
    return {
      code: candidate.code,
      message: candidate.message,
      details
    };
  } catch (error51) {
    return invalidArgumentError(
      "JSON does not contain a readable Status.",
      [],
      error51
    );
  }
}
function statusCodeFromHttp(httpCode) {
  if (httpCode >= 200 && httpCode < 300) return 0 /* OK */;
  switch (httpCode) {
    case 400:
      return 3 /* INVALID_ARGUMENT */;
    case 401:
      return 16 /* UNAUTHENTICATED */;
    case 403:
      return 7 /* PERMISSION_DENIED */;
    case 404:
      return 5 /* NOT_FOUND */;
    case 409:
      return 10 /* ABORTED */;
    case 429:
      return 8 /* RESOURCE_EXHAUSTED */;
    case 501:
      return 12 /* UNIMPLEMENTED */;
    case 503:
      return 14 /* UNAVAILABLE */;
    default:
      if (httpCode >= 400 && httpCode < 500) {
        return 9 /* FAILED_PRECONDITION */;
      }
      if (httpCode >= 500 && httpCode < 600) return 13 /* INTERNAL */;
      return 2 /* UNKNOWN */;
  }
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
async function statusFromResponse(response, operation = "HTTP request") {
  let httpStatus;
  try {
    if (typeof response !== "object" || response === null) {
      return invalidArgumentError(`${operation} did not return a Response.`);
    }
    if (response.ok) return okStatus();
    httpStatus = response.status;
    if (!Number.isInteger(httpStatus)) {
      return dataLossError(`${operation} returned an invalid HTTP status.`);
    }
  } catch (error51) {
    return statusFromUnknown(
      error51,
      `${operation} returned an unreadable HTTP response.`,
      14 /* UNAVAILABLE */
    );
  }
  let body = "";
  try {
    body = await response.text();
  } catch (error51) {
    return statusFromUnknown(
      error51,
      `${operation} returned HTTP ${httpStatus}, and its response body could not be read.`,
      statusCodeFromHttp(httpStatus)
    );
  }
  if (body) {
    try {
      const decoded = JSON.parse(body);
      const parsed = statusFromJson(decoded);
      if (isStatus(parsed) && parsed.code !== 0 /* OK */) return parsed;
    } catch {
    }
  }
  return {
    code: statusCodeFromHttp(httpStatus),
    message: body || `${operation} returned HTTP ${httpStatus}.`
  };
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
  } catch (error51) {
    return statusFromUnknown(error51, "Could not copy byte data.");
  }
}
async function toBytesAsync(value) {
  if (typeof Blob !== "undefined" && value instanceof Blob) {
    try {
      return new Uint8Array(await value.arrayBuffer());
    } catch (error51) {
      return statusFromUnknown(error51, "Could not read Blob data.");
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
  } catch (error51) {
    return invalidArgumentError("Byte data is not valid UTF-8.", [], error51);
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
  } catch (error51) {
    return statusFromUnknown(error51, "Could not normalize byte map.");
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
function encodeDateToTimeSpec(date5) {
  const msec = date5.getTime();
  const sec = Math.floor(msec / 1e3);
  const nsec = (msec - sec * 1e3) * 1e6;
  const nsecInSec = Math.floor(nsec / 1e9);
  return {
    sec: sec + nsecInSec,
    nsec: nsec - nsecInSec * 1e9
  };
}
function encodeTimestampExtension(object2) {
  if (object2 instanceof Date) {
    const timeSpec = encodeDateToTimeSpec(object2);
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
  register({ type, encode: encode4, decode: decode4 }) {
    if (type >= 0) {
      this.encoders[type] = encode4;
      this.decoders[type] = decode4;
    } else {
      const index = -1 - type;
      this.builtInEncoders[index] = encode4;
      this.builtInDecoders[index] = decode4;
    }
  }
  tryToEncode(object2, context) {
    for (let i = 0; i < this.builtInEncoders.length; i++) {
      const encodeExt = this.builtInEncoders[i];
      if (encodeExt != null) {
        const data = encodeExt(object2, context);
        if (data != null) {
          const type = -1 - i;
          return new ExtData(type, data);
        }
      }
    }
    for (let i = 0; i < this.encoders.length; i++) {
      const encodeExt = this.encoders[i];
      if (encodeExt != null) {
        const data = encodeExt(object2, context);
        if (data != null) {
          const type = i;
          return new ExtData(type, data);
        }
      }
    }
    if (object2 instanceof ExtData) {
      return object2;
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
  encodeSharedRef(object2) {
    if (this.entered) {
      const instance = this.clone();
      return instance.encodeSharedRef(object2);
    }
    try {
      this.entered = true;
      this.reinitializeState();
      this.doEncode(object2, 1);
      return this.bytes.subarray(0, this.pos);
    } finally {
      this.entered = false;
    }
  }
  /**
   * @returns Encodes the object and returns a copy of the encoder's internal buffer.
   */
  encode(object2) {
    if (this.entered) {
      const instance = this.clone();
      return instance.encode(object2);
    }
    try {
      this.entered = true;
      this.reinitializeState();
      this.doEncode(object2, 1);
      return this.bytes.slice(0, this.pos);
    } finally {
      this.entered = false;
    }
  }
  doEncode(object2, depth) {
    if (depth > this.maxDepth) {
      throw new Error(`Too deep objects in depth ${depth}`);
    }
    if (object2 == null) {
      this.encodeNil();
    } else if (typeof object2 === "boolean") {
      this.encodeBoolean(object2);
    } else if (typeof object2 === "number") {
      if (!this.forceIntegerToFloat) {
        this.encodeNumber(object2);
      } else {
        this.encodeNumberAsFloat(object2);
      }
    } else if (typeof object2 === "string") {
      this.encodeString(object2);
    } else if (this.useBigInt64 && typeof object2 === "bigint") {
      this.encodeBigInt64(object2);
    } else {
      this.encodeObject(object2, depth);
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
  encodeBoolean(object2) {
    if (object2 === false) {
      this.writeU8(194);
    } else {
      this.writeU8(195);
    }
  }
  encodeNumber(object2) {
    if (!this.forceIntegerToFloat && Number.isSafeInteger(object2)) {
      if (object2 >= 0) {
        if (object2 < 128) {
          this.writeU8(object2);
        } else if (object2 < 256) {
          this.writeU8(204);
          this.writeU8(object2);
        } else if (object2 < 65536) {
          this.writeU8(205);
          this.writeU16(object2);
        } else if (object2 < 4294967296) {
          this.writeU8(206);
          this.writeU32(object2);
        } else if (!this.useBigInt64) {
          this.writeU8(207);
          this.writeU64(object2);
        } else {
          this.encodeNumberAsFloat(object2);
        }
      } else {
        if (object2 >= -32) {
          this.writeU8(224 | object2 + 32);
        } else if (object2 >= -128) {
          this.writeU8(208);
          this.writeI8(object2);
        } else if (object2 >= -32768) {
          this.writeU8(209);
          this.writeI16(object2);
        } else if (object2 >= -2147483648) {
          this.writeU8(210);
          this.writeI32(object2);
        } else if (!this.useBigInt64) {
          this.writeU8(211);
          this.writeI64(object2);
        } else {
          this.encodeNumberAsFloat(object2);
        }
      }
    } else {
      this.encodeNumberAsFloat(object2);
    }
  }
  encodeNumberAsFloat(object2) {
    if (this.forceFloat32) {
      this.writeU8(202);
      this.writeF32(object2);
    } else {
      this.writeU8(203);
      this.writeF64(object2);
    }
  }
  encodeBigInt64(object2) {
    if (object2 >= BigInt(0)) {
      this.writeU8(207);
      this.writeBigUint64(object2);
    } else {
      this.writeU8(211);
      this.writeBigInt64(object2);
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
  encodeString(object2) {
    const maxHeaderSize = 1 + 4;
    const byteLength = utf8Count(object2);
    this.ensureBufferSizeToWrite(maxHeaderSize + byteLength);
    this.writeStringHeader(byteLength);
    utf8Encode2(object2, this.bytes, this.pos);
    this.pos += byteLength;
  }
  encodeObject(object2, depth) {
    const ext = this.extensionCodec.tryToEncode(object2, this.context);
    if (ext != null) {
      this.encodeExtension(ext);
    } else if (Array.isArray(object2)) {
      this.encodeArray(object2, depth);
    } else if (ArrayBuffer.isView(object2)) {
      this.encodeBinary(object2);
    } else if (typeof object2 === "object") {
      this.encodeMap(object2, depth);
    } else {
      throw new Error(`Unrecognized object: ${Object.prototype.toString.apply(object2)}`);
    }
  }
  encodeBinary(object2) {
    const size = object2.byteLength;
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
    const bytes = ensureUint8Array(object2);
    this.writeU8a(bytes);
  }
  encodeArray(object2, depth) {
    const size = object2.length;
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
    for (const item of object2) {
      this.doEncode(item, depth + 1);
    }
  }
  countWithoutUndefined(object2, keys) {
    let count = 0;
    for (const key of keys) {
      if (object2[key] !== void 0) {
        count++;
      }
    }
    return count;
  }
  encodeMap(object2, depth) {
    const keys = Object.keys(object2);
    if (this.sortKeys) {
      keys.sort();
    }
    const size = this.ignoreUndefined ? this.countWithoutUndefined(object2, keys) : keys.length;
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
      const value = object2[key];
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
function encode3(value, options) {
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
    FIND_CHUNK: for (const record2 of records) {
      const recordBytes = record2.bytes;
      for (let j = 0; j < byteLength; j++) {
        if (recordBytes[j] !== bytes[inputOffset + j]) {
          continue FIND_CHUNK;
        }
      }
      return record2.str;
    }
    return null;
  }
  store(bytes, value) {
    const records = this.caches[bytes.length - 1];
    const record2 = { bytes, str: value };
    if (records.length >= this.maxLengthPerKey) {
      records[Math.random() * records.length | 0] = record2;
    } else {
      records.push(record2);
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
      const object2 = this.doDecodeSync();
      if (this.hasRemaining(1)) {
        throw this.createExtraByteError(this.pos);
      }
      return object2;
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
      let object2;
      for await (const buffer of stream) {
        if (decoded) {
          this.entered = false;
          throw this.createExtraByteError(this.totalPos);
        }
        this.appendBuffer(buffer);
        try {
          object2 = this.doDecodeSync();
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
        return object2;
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
      let object2;
      if (headByte >= 224) {
        object2 = headByte - 256;
      } else if (headByte < 192) {
        if (headByte < 128) {
          object2 = headByte;
        } else if (headByte < 144) {
          const size = headByte - 128;
          if (size !== 0) {
            this.pushMapState(size);
            this.complete();
            continue DECODE;
          } else {
            object2 = {};
          }
        } else if (headByte < 160) {
          const size = headByte - 144;
          if (size !== 0) {
            this.pushArrayState(size);
            this.complete();
            continue DECODE;
          } else {
            object2 = [];
          }
        } else {
          const byteLength = headByte - 160;
          object2 = this.decodeString(byteLength, 0);
        }
      } else if (headByte === 192) {
        object2 = null;
      } else if (headByte === 194) {
        object2 = false;
      } else if (headByte === 195) {
        object2 = true;
      } else if (headByte === 202) {
        object2 = this.readF32();
      } else if (headByte === 203) {
        object2 = this.readF64();
      } else if (headByte === 204) {
        object2 = this.readU8();
      } else if (headByte === 205) {
        object2 = this.readU16();
      } else if (headByte === 206) {
        object2 = this.readU32();
      } else if (headByte === 207) {
        if (this.useBigInt64) {
          object2 = this.readU64AsBigInt();
        } else {
          object2 = this.readU64();
        }
      } else if (headByte === 208) {
        object2 = this.readI8();
      } else if (headByte === 209) {
        object2 = this.readI16();
      } else if (headByte === 210) {
        object2 = this.readI32();
      } else if (headByte === 211) {
        if (this.useBigInt64) {
          object2 = this.readI64AsBigInt();
        } else {
          object2 = this.readI64();
        }
      } else if (headByte === 217) {
        const byteLength = this.lookU8();
        object2 = this.decodeString(byteLength, 1);
      } else if (headByte === 218) {
        const byteLength = this.lookU16();
        object2 = this.decodeString(byteLength, 2);
      } else if (headByte === 219) {
        const byteLength = this.lookU32();
        object2 = this.decodeString(byteLength, 4);
      } else if (headByte === 220) {
        const size = this.readU16();
        if (size !== 0) {
          this.pushArrayState(size);
          this.complete();
          continue DECODE;
        } else {
          object2 = [];
        }
      } else if (headByte === 221) {
        const size = this.readU32();
        if (size !== 0) {
          this.pushArrayState(size);
          this.complete();
          continue DECODE;
        } else {
          object2 = [];
        }
      } else if (headByte === 222) {
        const size = this.readU16();
        if (size !== 0) {
          this.pushMapState(size);
          this.complete();
          continue DECODE;
        } else {
          object2 = {};
        }
      } else if (headByte === 223) {
        const size = this.readU32();
        if (size !== 0) {
          this.pushMapState(size);
          this.complete();
          continue DECODE;
        } else {
          object2 = {};
        }
      } else if (headByte === 196) {
        const size = this.lookU8();
        object2 = this.decodeBinary(size, 1);
      } else if (headByte === 197) {
        const size = this.lookU16();
        object2 = this.decodeBinary(size, 2);
      } else if (headByte === 198) {
        const size = this.lookU32();
        object2 = this.decodeBinary(size, 4);
      } else if (headByte === 212) {
        object2 = this.decodeExtension(1, 0);
      } else if (headByte === 213) {
        object2 = this.decodeExtension(2, 0);
      } else if (headByte === 214) {
        object2 = this.decodeExtension(4, 0);
      } else if (headByte === 215) {
        object2 = this.decodeExtension(8, 0);
      } else if (headByte === 216) {
        object2 = this.decodeExtension(16, 0);
      } else if (headByte === 199) {
        const size = this.lookU8();
        object2 = this.decodeExtension(size, 1);
      } else if (headByte === 200) {
        const size = this.lookU16();
        object2 = this.decodeExtension(size, 2);
      } else if (headByte === 201) {
        const size = this.lookU32();
        object2 = this.decodeExtension(size, 4);
      } else {
        throw new DecodeError(`Unrecognized type byte: ${prettyByte(headByte)}`);
      }
      this.complete();
      const stack = this.stack;
      while (stack.length > 0) {
        const state = stack.top();
        if (state.type === STATE_ARRAY) {
          state.array[state.position] = object2;
          state.position++;
          if (state.position === state.size) {
            object2 = state.array;
            stack.release(state);
          } else {
            continue DECODE;
          }
        } else if (state.type === STATE_MAP_KEY) {
          if (object2 === "__proto__") {
            throw new DecodeError("The key __proto__ is not allowed");
          }
          state.key = this.mapKeyConverter(object2);
          state.type = STATE_MAP_VALUE;
          continue DECODE;
        } else {
          state.map[state.key] = object2;
          state.readCount++;
          if (state.readCount === state.size) {
            object2 = state.map;
            stack.release(state);
          } else {
            state.key = null;
            state.type = STATE_MAP_KEY;
            continue DECODE;
          }
        }
      }
      return object2;
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
    let object2;
    if (this.stateIsMapKey() && this.keyDecoder?.canBeCached(byteLength)) {
      object2 = this.keyDecoder.decode(this.bytes, offset, byteLength);
    } else {
      object2 = utf8Decode2(this.bytes, offset, byteLength);
    }
    this.pos += headerOffset + byteLength;
    return object2;
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
    const object2 = this.bytes.subarray(offset, offset + byteLength);
    this.pos += headOffset + byteLength;
    return object2;
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
function decode3(buffer, options) {
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
    return concatBytes(values.map((value) => encode3(value)));
  } catch (error51) {
    return statusFromUnknown(
      error51,
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
  } catch (error51) {
    return invalidArgumentError(
      `Failed to decode ${context} MessagePack.`,
      [],
      error51
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
  const object2 = asRecord(value, field);
  if (!isOk(object2)) return object2;
  const result = /* @__PURE__ */ new Map();
  for (const [key, raw] of Object.entries(object2)) {
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
  const object2 = asRecord(value, field);
  if (!isOk(object2)) return object2;
  const result = /* @__PURE__ */ new Map();
  for (const [key, raw] of Object.entries(object2)) {
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
    } catch (error51) {
      return invalidArgumentError("Invalid ChunkMetadata options.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid ChunkMetadata value.", [], error51);
    }
  }
  getAttribute(key) {
    try {
      const valid = validateName(key);
      if (!isOk(valid)) return valid;
      const value = this.attributes.get(key);
      return value === void 0 ? notFoundError(`Attribute not found: ${key}`) : new Uint8Array(value);
    } catch (error51) {
      return statusFromUnknown(error51, "Could not read chunk attribute.");
    }
  }
  setAttribute(key, value) {
    const status = validateName(key);
    if (!isOk(status)) return status;
    try {
      this.attributes.set(key, new Uint8Array(value));
      return okStatus();
    } catch (error51) {
      return statusFromUnknown(error51, "Could not set chunk attribute.");
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
    } catch (error51) {
      return invalidArgumentError("Invalid Chunk options.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid Chunk value.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid NodeRef options.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid NodeRef value.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid NodeFragment options.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid NodeFragment value.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid Port value.", [], error51);
    }
  }
  get approxBytes() {
    return this.name.length + this.id.length + 1;
  }
  validate() {
    try {
      const nameStatus = validateOptionalName(this.name);
      return isOk(nameStatus) ? validateOptionalName(this.id) : nameStatus;
    } catch (error51) {
      return invalidArgumentError("Invalid Port value.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid ActionMessage options.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid ActionMessage value.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid WireMessage options.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid WireMessage value.", [], error51);
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
    const version2 = asUnsigned(fields[0], "WireMessage.version", UINT32_MAX2);
    if (!isOk(version2)) return version2;
    if (version2 !== _WireMessage.VERSION) {
      return invalidArgumentError(`Invalid serialized WireMessage version: ${version2}`);
    }
    const fragments = [];
    const actions = [];
    for (const [index, target, decode4, field] of [
      [1, fragments, NodeFragment.fromMsgpack, "node_fragments"],
      [2, actions, ActionMessage.fromMsgpack, "actions"]
    ]) {
      const raw = fields[index];
      if (!Array.isArray(raw)) return invalidArgumentError(`WireMessage.${field} must be a list.`);
      for (const item of raw) {
        const encoded = asBinary(item, `WireMessage.${field}`);
        if (!isOk(encoded)) return encoded;
        const value = decode4(encoded);
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
    } catch (error51) {
      return statusFromUnknown(error51, "Failed to serialize WireMessage JSON.");
    }
  }
  static fromJson(value) {
    if (typeof value !== "string") {
      return invalidArgumentError("WireMessage JSON must be a string.");
    }
    try {
      return wireMessageFromJsonValue(JSON.parse(value));
    } catch (error51) {
      return invalidArgumentError("Failed to parse WireMessage JSON.", [], error51);
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
  } catch (error51) {
    return statusFromUnknown(error51, "Serializing WireMessage JSON value raised.");
  }
}
function metadataFromJson(value) {
  const object2 = asRecord(value, "ChunkMetadata");
  if (!isOk(object2)) return object2;
  const mimetype = object2.mimetype === void 0 ? "" : asString(object2.mimetype, "ChunkMetadata.mimetype");
  if (!isOk(mimetype)) return mimetype;
  let timestamp = null;
  if (object2.timestamp !== void 0 && object2.timestamp !== null) {
    if (typeof object2.timestamp !== "string") return invalidArgumentError("ChunkMetadata.timestamp must be an RFC 3339 string or null.");
    timestamp = new Date(object2.timestamp);
    if (!isValidDate(timestamp)) return invalidArgumentError("Invalid ChunkMetadata.timestamp.");
  }
  const attributes = byteMapFromJson(object2.attributes, "ChunkMetadata.attributes");
  if (!isOk(attributes)) return attributes;
  return ChunkMetadata.create({ mimetype, timestamp, attributes });
}
function chunkFromJson(value) {
  const object2 = asRecord(value, "Chunk");
  if (!isOk(object2)) return object2;
  let metadata = null;
  if (object2.metadata !== void 0 && object2.metadata !== null) {
    const decoded = metadataFromJson(object2.metadata);
    if (!isOk(decoded)) return decoded;
    metadata = decoded;
  }
  const ref = object2.ref === void 0 ? "" : asString(object2.ref, "Chunk.ref");
  if (!isOk(ref)) return ref;
  let data = new Uint8Array();
  if (object2.data !== void 0) {
    if (typeof object2.data !== "string") return invalidArgumentError("Chunk.data must be a base64 string.");
    const decoded = base64Decode(object2.data);
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
  const object2 = asRecord(value, "NodeFragment");
  if (!isOk(object2)) return object2;
  const id = object2.id === void 0 ? "" : asString(object2.id, "NodeFragment.id");
  if (!isOk(id)) return id;
  const dataObject = asRecord(object2.data, "NodeFragment.data");
  if (!isOk(dataObject)) return dataObject;
  const isNodeRef = dataObject.id !== void 0 && dataObject.data === void 0 && dataObject.ref === void 0 && dataObject.metadata === void 0;
  const data = isNodeRef ? nodeRefFromJson(dataObject) : chunkFromJson(dataObject);
  if (!isOk(data)) return data;
  let seq = null;
  if (object2.seq !== void 0 && object2.seq !== null) {
    const decoded = asUnsigned(object2.seq, "NodeFragment.seq", UINT32_MAX2);
    if (!isOk(decoded)) return decoded;
    seq = decoded;
  }
  if (object2.continued !== void 0 && typeof object2.continued !== "boolean") {
    return invalidArgumentError("NodeFragment.continued must be a boolean.");
  }
  return NodeFragment.create({ id, data, seq, continued: object2.continued });
}
function portFromJson(value) {
  const object2 = asRecord(value, "Port");
  if (!isOk(object2)) return object2;
  const name = object2.name === void 0 ? "" : asString(object2.name, "Port.name");
  if (!isOk(name)) return name;
  const id = object2.id === void 0 ? "" : asString(object2.id, "Port.id");
  return isOk(id) ? Port.create(name, id) : id;
}
function actionFromJson(value) {
  const object2 = asRecord(value, "ActionMessage");
  if (!isOk(object2)) return object2;
  const id = object2.id === void 0 ? "" : asString(object2.id, "ActionMessage.id");
  if (!isOk(id)) return id;
  const name = object2.name === void 0 ? "" : asString(object2.name, "ActionMessage.name");
  if (!isOk(name)) return name;
  const ports = [[], []];
  for (const [index, field] of [[0, "inputs"], [1, "outputs"]]) {
    const raw = object2[field];
    if (raw === void 0) continue;
    if (!Array.isArray(raw)) return invalidArgumentError(`ActionMessage.${field} must be an array.`);
    for (const item of raw) {
      const port = portFromJson(item);
      if (!isOk(port)) return port;
      ports[index].push(port);
    }
  }
  const headers = byteMapFromJson(object2.headers, "ActionMessage.headers");
  if (!isOk(headers)) return headers;
  return ActionMessage.create({ id, name, inputs: ports[0], outputs: ports[1], headers });
}
function wireMessageFromJsonValue(value) {
  try {
    return wireMessageFromJsonValueUnchecked(value);
  } catch (error51) {
    return invalidArgumentError("Invalid WireMessage JSON value.", [], error51);
  }
}
function wireMessageFromJsonValueUnchecked(value) {
  const object2 = asRecord(value, "WireMessage");
  if (!isOk(object2)) return object2;
  const fragments = [];
  const actions = [];
  if (object2.node_fragments !== void 0) {
    if (!Array.isArray(object2.node_fragments)) return invalidArgumentError("WireMessage.node_fragments must be an array.");
    for (const item of object2.node_fragments) {
      const fragment = fragmentFromJson(item);
      if (!isOk(fragment)) return fragment;
      fragments.push(fragment);
    }
  }
  if (object2.actions !== void 0) {
    if (!Array.isArray(object2.actions)) return invalidArgumentError("WireMessage.actions must be an array.");
    for (const item of object2.actions) {
      const action = actionFromJson(item);
      if (!isOk(action)) return action;
      actions.push(action);
    }
  }
  const headers = byteMapFromJson(object2.headers, "WireMessage.headers");
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
var INTERACTION_TAG = "a11.sdk.Interaction";
var PEER_TAG = "a11.sdk.Peer";
var ACTION_CONFIG_TAG = "a11.sdk.ActionConfig";
var USAGE_METADATA_TAG = "a11.sdk.UsageMetadata";

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
function registerWireValueCodec(codec2) {
  if (typeof codec2?.tag !== "string" || codec2.tag === "") {
    return invalidArgumentError("A wire value codec tag must be non-empty.");
  }
  if (typeof codec2.test !== "function" || typeof codec2.dump !== "function" || typeof codec2.load !== "function") {
    return invalidArgumentError(
      `The wire value codec for ${codec2.tag} must provide test, dump and load.`
    );
  }
  const existing = byTag.get(codec2.tag);
  if (existing !== void 0) {
    if (existing === codec2) return okStatus();
    return alreadyExistsError(`A wire value codec for ${codec2.tag} is already registered.`);
  }
  byTag.set(codec2.tag, codec2);
  codecs.push(codec2);
  return okStatus();
}
function wireValueCodecFor(value) {
  for (const codec2 of codecs) if (codec2.test(value)) return codec2;
  return null;
}
function wireValueCodecs() {
  return codecs;
}
function wireValueCodecCount() {
  return codecs.length;
}
function wireValueCodecByTag(tag) {
  return byTag.get(tag) ?? null;
}
function put(fields, key, value, omit2) {
  if (!omit2) fields[key] = value;
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
function byteMapFields(map2) {
  const result = {};
  for (const [key, value] of map2) result[key] = value;
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
  for (const codec2 of entries) registerWireValueCodec(codec2);
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
  } catch (error51) {
    return invalidArgumentError("Invalid base64 byte data.", [], error51);
  }
}
function jsonSerialize(value) {
  const wire = toWire(value, false);
  if (!isOk(wire)) return wire;
  try {
    return utf8Encode(JSON.stringify(wire, (_key, item) => typeof item === "bigint" ? Number(item) : item));
  } catch (error51) {
    return invalidArgumentError("Failed to serialize JSON.", [], error51);
  }
}
function jsonDeserialize(data) {
  const text = utf8Decode(data);
  if (!isOk(text)) return text;
  try {
    return JSON.parse(text);
  } catch (error51) {
    return invalidArgumentError("Invalid JSON data.", [], error51);
  }
}
function msgpackSerialize(value) {
  const wire = toWire(value, true);
  if (!isOk(wire)) return wire;
  try {
    return encode3(wire);
  } catch (error51) {
    return invalidArgumentError("Failed to serialize MessagePack.", [], error51);
  }
}
function msgpackDeserialize(data) {
  try {
    return decode3(data, { useBigInt64: true });
  } catch (error51) {
    return invalidArgumentError("Invalid MessagePack data.", [], error51);
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
    const date5 = new Date(decoded);
    return Number.isFinite(date5.getTime()) ? date5 : invalidArgumentError("Serialized datetime is invalid.");
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
  register(codec2) {
    try {
      if (typeof codec2.tag !== "string" || codec2.tag === "") return invalidArgumentError("Serialization codec tag must be non-empty.");
      if (typeof codec2.test !== "function" || typeof codec2.serialize !== "function" || typeof codec2.deserialize !== "function") {
        return invalidArgumentError("Serialization codec callbacks must be functions.");
      }
      const parsed = parseMimetype(codec2.mimetype);
      if (!isOk(parsed)) return parsed;
      if (this.codecs.some((registered) => registered.tag === codec2.tag && registered.parsed.mediaType === parsed.mediaType)) {
        return alreadyExistsError(`A codec for ${codec2.tag} and ${parsed.mediaType} is already registered.`);
      }
      this.codecs.push({ ...codec2, parsed, order: this.nextOrder++ });
      return { code: 0, message: "OK" };
    } catch (error51) {
      return statusFromUnknown(error51, "Could not register serialization codec.");
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
        const json2 = mimetype === JSON_MIMETYPE;
        derived.push({
          tag: wireValue.tag,
          mimetype,
          parsed,
          order: -1,
          test: (value) => wireValue.test(value),
          serialize: (value) => json2 ? jsonSerialize(value) : msgpackSerialize(value),
          deserialize: (data) => {
            const decoded = json2 ? jsonDeserialize(data) : msgpackDeserialize(data);
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
        (codec3) => codec3.test(value) && (selection === null || registrationMatches(codec3.parsed, selection))
      ).sort((left, right) => left.order - right.order);
      if (candidates.length === 0) {
        return notFoundError(`No serializer is registered for the value${mimetype ? ` and ${mimetype}` : ""}.`);
      }
      const codec2 = candidates[0];
      let serializedResult;
      try {
        serializedResult = await codec2.serialize(value);
      } catch (error51) {
        return statusFromUnknown(error51, `Serializer for ${codec2.tag} failed.`);
      }
      if (!isOk(serializedResult)) return serializedResult;
      const serialized = serializedResult;
      const exactMimetype = formatMimetype(codec2.parsed, codec2.tag);
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
      const outputMimetype = blobType !== null && isOk(blobType) ? formatMimetype(blobType, codec2.tag) : exactMimetype;
      return Chunk.create({ metadata: new ChunkMetadata({ mimetype: outputMimetype }), data });
    } catch (error51) {
      return statusFromUnknown(error51, "Could not serialize value.");
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
      } catch (error51) {
        return invalidArgumentError("The chunk contains an invalid encoded type tag.", [], error51);
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
        (codec3) => (
          // A Blob carries its concrete browser media type (for example,
          // image/png) on the chunk. Its stable `blob` tag selects the binary
          // decoder independently of that concrete media type.
          wanted === "blob" && codec3.tag === "blob" || registrationMatches(codec3.parsed, actual)
        )
      );
      const generic = byMediaType.filter((codec3) => GENERIC_TAGS.has(codec3.tag));
      let candidates = wanted === void 0 ? generic : byMediaType.filter((codec3) => codec3.tag === wanted);
      if (candidates.length === 0 && expectedTag === void 0) {
        candidates = generic.length > 0 ? generic : byMediaType;
      }
      if (candidates.length === 0) {
        return notFoundError(`No deserializer is registered for ${chunk.mimetype}.`);
      }
      const codec2 = candidates.sort((left, right) => left.order - right.order)[0];
      try {
        const value = await codec2.deserialize(new Uint8Array(chunk.data), chunk);
        return value;
      } catch (error51) {
        return statusFromUnknown(error51, `Deserializer for ${codec2.tag} failed.`);
      }
    } catch (error51) {
      return statusFromUnknown(error51, "Could not deserialize chunk.");
    }
  }
};
function mediaTypeOf(mimetype) {
  const parsed = parseMimetype(mimetype);
  return isOk(parsed) ? parsed.mediaType : OCTET_STREAM_MIMETYPE;
}
var globalRegistry2 = new SerializationRegistry({ registerDefaults: true });
function getGlobalSerializationRegistry() {
  return globalRegistry2;
}
async function toChunk(value, mimetype = "") {
  return globalRegistry2.toChunk(value, mimetype);
}

// src/concurrency.ts
async function sleep(ms) {
  if (!Number.isFinite(ms) || ms < 0) {
    return invalidArgumentError("Sleep duration must be non-negative and finite.");
  }
  try {
    await new Promise((resolve) => setTimeout(resolve, ms));
    return okStatus();
  } catch (error51) {
    return statusFromUnknown(error51, "Sleep raised an exception.");
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
      } catch (error51) {
        failure = statusFromUnknown(error51);
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
  } catch (error51) {
    return invalidArgumentError("Deadline could not be read.", [], error51);
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
    } catch (error51) {
      return statusFromUnknown(error51, "Chunk store wait failed.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Chunk store get failed.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Chunk store arrival-order get failed.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Chunk store next failed.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Chunk store put failed.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Chunk store clear failed.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Chunk store close failed.");
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
  } catch (error51) {
    return invalidArgumentError("Reader options could not be read.", [], error51);
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
    } catch (error51) {
      return statusFromUnknown(error51, "ChunkStoreReader could not be created.");
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
    } catch (error51) {
      this.fetchDone(generation, statusFromUnknown(error51, "ChunkStore reader fetch raised an exception"));
      return okStatus();
    }
    Promise.resolve(pending).then((result) => this.fetchDone(generation, result)).catch((error51) => this.fetchDone(generation, statusFromUnknown(error51, "ChunkStore reader fetch rejected")));
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
      } catch (error51) {
        this.clearDone(clearGeneration, statusFromUnknown(error51, "ChunkStore clear raised an exception"));
        return;
      }
      Promise.resolve(pending).then((cleared) => this.clearDone(clearGeneration, cleared)).catch((error51) => this.clearDone(clearGeneration, statusFromUnknown(error51, "ChunkStore clear rejected")));
    } catch (error51) {
      this.operation = "none";
      this.fail(statusFromUnknown(error51, "Processing ChunkStore fetch result raised an exception"));
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
    } catch (error51) {
      this.operation = "none";
      this.fail(statusFromUnknown(error51, "Processing ChunkStore clear result raised an exception"));
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
    } catch (error51) {
      return invalidArgumentError("Invalid ActionPortSchema options.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid ActionPortSchema value.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid ActionHeaderSchema options.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Invalid ActionHeaderSchema value.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Could not create ActionSchema.", [], error51);
    }
  }
  validate() {
    try {
      return this.validateUnchecked();
    } catch (error51) {
      return invalidArgumentError("Invalid ActionSchema value.", [], error51);
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
    } catch (error51) {
      return invalidArgumentError("Mapping Action output raised.", [], error51);
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
  } catch (error51) {
    return invalidArgumentError("Writer options could not be read.", [], error51);
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
    } catch (error51) {
      return statusFromUnknown(error51, "ChunkStoreWriter could not be created.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "WireStream getId raised an exception");
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
    } catch (error51) {
      this.writeDone(generation, statusFromUnknown(error51, "ChunkStore getId raised an exception"));
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
    } catch (error51) {
      this.writeDone(generation, statusFromUnknown(error51, "ChunkStore putMany raised an exception"));
      return okStatus();
    }
    Promise.resolve(pending).then((result) => this.writeDone(generation, result)).catch((error51) => this.writeDone(generation, statusFromUnknown(error51, "ChunkStore putMany rejected")));
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
        } catch (error51) {
          id = statusFromUnknown(error51, "ChunkStore getId raised an exception");
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
            } catch (error51) {
              teeStatus = statusFromUnknown(error51, "WireStream send raised an exception");
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
    } catch (error51) {
      this.operation = "none";
      this.fail(statusFromUnknown(error51, "Processing ChunkStore putMany result raised an exception"));
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
    } catch (error51) {
      id = statusFromUnknown(error51, "ChunkStore getId raised an exception");
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
      } catch (error51) {
        return statusFromUnknown(error51, "WireStream send raised an exception");
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
    } catch (error51) {
      this.closeDone(generation, requested, teeStatus, statusFromUnknown(error51, "ChunkStore close raised an exception"));
      return okStatus();
    }
    Promise.resolve(pending).then((result) => this.closeDone(generation, requested, teeStatus, result)).catch((error51) => this.closeDone(generation, requested, teeStatus, statusFromUnknown(error51, "ChunkStore close rejected")));
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
    } catch (error51) {
      this.operation = "none";
      this.fail(statusFromUnknown(error51, "Processing ChunkStore close result raised an exception"));
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
    } catch (error51) {
      return statusFromUnknown(error51, "AsyncNode could not be created.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Chunk-store factory raised an exception.");
    }
  }
  /** Return the stable id used by fragments, actions, and sessions. */
  getId() {
    try {
      const id = this.chunkStore.getId();
      if (isStatus(id) && !isOk(id)) return id;
      return typeof id === "string" ? id : internalError("ChunkStore.getId() returned an invalid value.");
    } catch (error51) {
      return statusFromUnknown(error51, "ChunkStore.getId() raised an exception.");
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
  setSerializationRegistry(registry2) {
    if (!(registry2 instanceof SerializationRegistry)) return invalidArgumentError("registry must be a SerializationRegistry.");
    this.registry = registry2;
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
    } catch (error51) {
      return invalidArgumentError("Expected type options could not be read.", [], error51);
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
    } catch (error51) {
      return statusFromUnknown(error51, "Resetting AsyncNode reader raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Changing AsyncNode writer options raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Writing AsyncNode value raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Reading AsyncNode value raised an exception.");
    }
  }
  /**
   * Consume exactly one whole value's fragment and validate its terminator.
   * Use this for unary action ports; streaming ports should call `next`.
   */
  async consumeFragment(options = {}) {
    try {
      return await this.consumeFragmentInternal(options);
    } catch (error51) {
      return statusFromUnknown(error51, "Consuming AsyncNode fragment raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Consuming AsyncNode value raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Chunk-store factory raised an exception.");
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
  } catch (error51) {
    return invalidArgumentError(
      "deadline must be a readable Date, number, or null.",
      [],
      error51
    );
  }
}
function normalizeWireStreamOptions(options = {}) {
  try {
    return normalizeWireStreamOptionsUnchecked(options);
  } catch (error51) {
    return invalidArgumentError(
      "WireStream options could not be read.",
      [],
      error51
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
  } catch (error51) {
    return statusFromUnknown(error51, "Normalizing WireStream headers raised.");
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
  } catch (error51) {
    return statusFromUnknown(error51, "WireStream callback raised an exception.");
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
  } catch (error51) {
    return invalidArgumentError("Action settings could not be read.", [], error51);
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
    } catch (error51) {
      return statusFromUnknown(error51, "Creating Action raised an exception.");
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
    } catch (error51) {
      return invalidArgumentError("Action settings could not be copied.", [], error51);
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
  bindRegistry(registry2) {
    if (registry2 !== null && !hasRegistryShape(registry2)) {
      return invalidArgumentError("registry must implement ActionRegistryLike or be null.");
    }
    this.registry = registry2;
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
      } catch (error51) {
        return statusFromUnknown(error51, "Tracking Action in Session raised an exception.");
      }
      if (!isStatus(tracked)) {
        return internalError("Session.trackAction() returned a non-Status value.");
      }
      if (!isOk(tracked)) return tracked;
    }
    try {
      previous?.untrackAction(this);
    } catch (error51) {
      if (session !== null) {
        try {
          session.untrackAction(this);
        } catch {
        }
      }
      return statusFromUnknown(error51, "Removing Action from Session raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Creating nested Action raised an exception.");
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
    } catch (error51) {
      this.untrackFromSession();
      if (this.mode === "call" && this.completionStatus === null) this.mode = "none";
      return statusFromUnknown(error51, "Calling Action raised an exception.");
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
    } catch (error51) {
      sent = statusFromUnknown(error51, "Sending Action call raised an exception.");
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
      } catch (error51) {
        first = firstError(first, statusFromUnknown(error51, "Action cancel callback raised."));
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
    } catch (error51) {
      return statusFromUnknown(error51, "Applying Action input autofills raised an exception.");
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
            } catch (error51) {
              status = statusFromUnknown(
                error51,
                "Action handler raised an exception."
              );
            }
          }
        }
      }
    } catch (error51) {
      status = statusFromUnknown(error51, "Running Action handler raised an exception.");
    } finally {
      if (acquired) {
        try {
          this.session?.releaseActionSlot?.(nested);
        } catch (error51) {
          status = firstError(
            status,
            statusFromUnknown(error51, "Releasing Session Action slot raised.")
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
    } catch (error51) {
      const failure = firstError(
        initialStatus,
        statusFromUnknown(error51, "Finishing Action run raised an exception.")
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
        } catch (error51) {
          first = firstError(
            first,
            statusFromUnknown(error51, "Sending Action output status raised.")
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
    } catch (error51) {
      return statusFromUnknown(error51, "Tracking Action in Session raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Sending remote Action cancellation raised.");
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
  async waitForStatus(promise2, timeoutMs, message) {
    if (timeoutMs !== void 0 && (!Number.isFinite(timeoutMs) || timeoutMs < 0)) {
      return invalidArgumentError("timeoutMs must be a non-negative finite number.");
    }
    if (timeoutMs === void 0) return promise2;
    const timeout = new Deferred();
    const timer = setTimeout(() => timeout.resolve(deadlineExceededError(message)), timeoutMs);
    try {
      return await Promise.race([promise2, timeout.promise]);
    } catch (error51) {
      return statusFromUnknown(error51, "Waiting for Action raised an exception.");
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
  } catch (error51) {
    return statusFromUnknown(error51, "Copying ActionSchema raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Registering Action raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Unregistering Action raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Looking up Action schema raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Looking up Action handler raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Creating registered Action raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Copying ActionRegistry raised an exception.");
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
  } catch (error51) {
    return statusFromUnknown(error51, "Validating Session options raised an exception.");
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
  } catch (error51) {
    return statusFromUnknown(error51, "Creating Session configuration raised an exception.");
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
  } catch (error51) {
    return statusFromUnknown(error51, "Session message callback raised an exception.");
  }
}
async function invokeSessionDoneCallback(callback, stream, session) {
  try {
    const result = await callback(stream, session);
    if (result === void 0) return okStatus();
    return isStatus(result) ? result : internalError("Session done callback returned a non-Status value.");
  } catch (error51) {
    return statusFromUnknown(error51, "Session done callback raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Constructing Session raised an exception.");
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
  setActionRegistry(registry2) {
    if (registry2 !== null && !(registry2 instanceof ActionRegistry)) {
      return invalidArgumentError(
        "registry must be an ActionRegistry or null."
      );
    }
    this.actionRegistry = registry2;
    let first = okStatus();
    for (const action of this.activeActions.values()) {
      first = firstError2(first, action.bindRegistry(registry2));
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
    } catch (error51) {
      return statusFromUnknown(error51, "Listing Session streams raised an exception.");
    }
  }
  getStream(streamId) {
    try {
      const state = this.streamsById.get(streamId);
      return state?.stream ?? notFoundError(
        `Stream '${streamId}' is not attached to the Session.`
      );
    } catch (error51) {
      return statusFromUnknown(error51, "Looking up Session stream raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Looking up Session Action raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Waiting for Session Actions raised an exception.");
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
    } catch (error51) {
      return Promise.resolve(
        statusFromUnknown(error51, "Acquiring a Session Action slot raised.")
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
    } catch (error51) {
      return statusFromUnknown(error51, "Dispatching NodeFragment raised an exception.");
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
        } catch (error51) {
          sent = statusFromUnknown(error51, "Sending Action dispatch status raised.");
        }
        return isStatus(sent) ? sent : internalError("WireStream.send() returned an invalid Status.");
      }
      return dispatchStatus;
    } catch (error51) {
      return statusFromUnknown(error51, "Dispatching ActionMessage raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Dispatching Action raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Dispatching WireMessage raised an exception.");
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
      } catch (error51) {
        return statusFromUnknown(error51, "WireStream.getId() raised an exception.");
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
      } catch (error51) {
        startup = statusFromUnknown(error51, "WireStream startup raised an exception.");
      }
      if (!isStatus(startup)) {
        const invalid = internalError("WireStream startup returned an invalid Status.");
        this.removeStream(state);
        return invalid;
      }
      if (!isOk(startup)) this.removeStream(state);
      return startup;
    } catch (error51) {
      if (state !== null) this.removeStream(state);
      return statusFromUnknown(error51, "Attaching Session stream raised an exception.");
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
      } catch (error51) {
        return statusFromUnknown(error51, "WireStream.send() raised an exception.");
      }
    } catch (error51) {
      return statusFromUnknown(error51, "Sending Session message raised an exception.");
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
        } catch (error51) {
          first = firstError2(
            first,
            statusFromUnknown(error51, "WireStream.halfClose() raised.")
          );
        }
      }
      this.finishIfPossible();
      return first;
    } catch (error51) {
      return statusFromUnknown(error51, "Half-closing Session raised an exception.");
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
        } catch (error51) {
          sent = statusFromUnknown(error51, "Sending Session abort raised.");
        }
        if (!isStatus(sent)) {
          sent = internalError("WireStream.send() returned an invalid Status.");
        }
        if (!isOk(sent)) {
          first = firstError2(first, sent);
          try {
            const aborted2 = state.stream.abort(streamAbort);
            first = firstError2(first, aborted2);
          } catch (error51) {
            first = firstError2(
              first,
              statusFromUnknown(error51, "WireStream.abort() raised.")
            );
          }
        }
      }
      this.finishIfPossible();
      return first;
    } catch (error51) {
      return statusFromUnknown(error51, "Aborting Session raised an exception.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Handling Session stream message raised.");
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
    } catch (error51) {
      state.acceptingMessages = false;
      this.clearPendingMessages(state);
      state.messagePumpRunning = false;
      const status = statusFromUnknown(error51, "Session message pump raised.");
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
    } catch (error51) {
      this.removeStream(state);
      return statusFromUnknown(error51, "Handling Session stream completion raised.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "WireStream.getStatus() raised.");
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
  } catch (error51) {
    return invalidArgumentError("Byte chunking options could not be read.", [], error51);
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
  } catch (error51) {
    return statusFromUnknown(error51, "Failed to split byte message.");
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
  } catch (error51) {
    return statusFromUnknown(error51, "Failed to parse byte packet.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Creating ByteReassembler raised an exception.");
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
  } catch (error51) {
    return invalidArgumentError(
      "Channel framing options could not be read.",
      [],
      error51
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
    } catch (error51) {
      return statusFromUnknown(
        error51,
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
    } catch (error51) {
      const status = statusFromUnknown(
        error51,
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
    } catch (error51) {
      return statusFromUnknown(
        error51,
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
    } catch (error51) {
      this.forceAbort(
        statusFromUnknown(error51, "Receiving channel data raised an exception."),
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
          } catch (error51) {
            sent = statusFromUnknown(
              error51,
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
    } catch (error51) {
      this.finish(
        statusFromUnknown(error51, "WireStream sender raised an exception.")
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
      } catch (error51) {
        return statusFromUnknown(
          error51,
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
      } catch (error51) {
        waited = statusFromUnknown(
          error51,
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
    } catch (error51) {
      this.forceAbort(
        statusFromUnknown(error51, "WireStream receiver raised an exception."),
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
    } catch (error51) {
      cleanupStatus = statusFromUnknown(
        error51,
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
    } catch (error51) {
      if (isOk(cleanupStatus)) {
        cleanupStatus = statusFromUnknown(
          error51,
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
  } catch (error51) {
    return invalidArgumentError("Could not normalize WebSocket options.", [], error51);
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
function validateWebSocketUrl(url2) {
  if (typeof url2 !== "string") {
    return invalidArgumentError("WebSocket URL must be a string.");
  }
  try {
    const parsed = new URL(url2);
    if (parsed.protocol !== "ws:" && parsed.protocol !== "wss:") {
      return invalidArgumentError("WebSocket URL must start with ws:// or wss://.");
    }
    if (!parsed.hostname) {
      return invalidArgumentError("WebSocket URL host must not be empty.");
    }
    return okStatus();
  } catch (error51) {
    return invalidArgumentError("WebSocket URL is invalid.", [], error51);
  }
}
function hasBrowserWebSocket() {
  return typeof globalThis.WebSocket === "function";
}
var WebSocketBinaryChannel = class {
  constructor(url2, options) {
    this.url = url2;
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
        } catch (error51) {
          this.signalCallbackError(error51, "Reading WebSocket message event failed.");
        }
      });
      socket.addEventListener("error", () => this.handleError());
      socket.addEventListener("close", (event) => {
        try {
          this.handleClose(event.code ?? 1006, event.reason ?? "");
        } catch (error51) {
          this.signalCallbackError(error51, "Reading WebSocket close event failed.");
        }
      });
      if (socket.readyState === 1) this.handleOpen();
    } catch (error51) {
      const status = statusFromUnknown(
        error51,
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
    } catch (error51) {
      return statusFromUnknown(error51, "WebSocket send raised an exception.");
    }
  }
  bufferedAmount() {
    const socket = this.socket;
    if (socket === null) return failedPreconditionError("WebSocket is not created.");
    try {
      const amount = socket.bufferedAmount;
      return Number.isFinite(amount) && amount >= 0 ? amount : unavailableError("WebSocket reported an invalid bufferedAmount.");
    } catch (error51) {
      return statusFromUnknown(
        error51,
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
    } catch (error51) {
      return statusFromUnknown(error51, "WebSocket close raised an exception.");
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
      } catch (error51) {
        return statusFromUnknown(
          error51,
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
      } catch (error51) {
        return statusFromUnknown(error51, "Browser WebSocket construction failed.");
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
    } catch (error51) {
      return statusFromUnknown(error51, "Node.js WebSocket construction failed.");
    }
  }
  handleOpen() {
    if (this.closed) return;
    this.opening?.resolve(okStatus());
    try {
      this.callbacks?.onOpen();
    } catch (error51) {
      this.signalCallbackError(error51, "WebSocket open callback raised an exception.");
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
      } catch (error51) {
        this.signalCallbackError(
          error51,
          "WebSocket message callback raised an exception."
        );
      }
    }).catch((error51) => {
      this.signalCallbackError(error51, "WebSocket message processing failed.");
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
  signalCallbackError(error51, message) {
    const status = statusFromUnknown(error51, message);
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
  static createClient(url2, options = {}, websocketOptions = {}) {
    try {
      const validUrl = validateWebSocketUrl(url2);
      if (!isOk(validUrl)) return validUrl;
      const normalized = normalizeWebSocketOptions(websocketOptions);
      if (!isOk(normalized)) return normalized;
      const channel = new WebSocketBinaryChannel(url2, normalized);
      const stream = ChannelWireStream.create(
        channel,
        randomId("ws-"),
        "client" /* CLIENT */,
        options,
        normalized.framing
      );
      return isOk(stream) ? new _WebSocketWireStream(stream) : stream;
    } catch (error51) {
      return statusFromUnknown(error51, "Creating WebSocketWireStream raised an exception.");
    }
  }
  /** Alias for {@link createClient}; connection still begins in {@link start}. */
  static connect(url2, options = {}, websocketOptions = {}) {
    return _WebSocketWireStream.createClient(url2, options, websocketOptions);
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

// src/sdk/llm.ts
var DEFAULT_ROLE = "user" /* USER */;
var roleSchema = external_exports.enum(["system", "model", "user"]);
function zodParse(schema, value, context) {
  const result = schema.safeParse(value);
  if (result.success) return result.data;
  return invalidArgumentError(
    `${context}: ${result.error.issues.map((issue2) => `${issue2.path.join(".") || "<root>"} ${issue2.message}`).join("; ")}`,
    [...result.error.issues]
  );
}
var usageMetadataSchema = external_exports.object({
  input_tokens: external_exports.number().int().nullish(),
  output_tokens: external_exports.number().int().nullish(),
  total_tokens: external_exports.number().int().nullish(),
  cached_input_tokens: external_exports.number().int().nullish(),
  cache_write_tokens: external_exports.number().int().nullish(),
  reasoning_tokens: external_exports.number().int().nullish()
}).loose();
function taggedOr(tag, schema) {
  return external_exports.union([
    external_exports.custom((value) => valueTag(value) === tag),
    schema.transform((value) => tagValue({ ...value }, tag))
  ]);
}
function wireValueField(tag) {
  return external_exports.unknown().transform((value, ctx) => {
    const codec2 = wireValueCodecByTag(tag);
    if (codec2 === null) {
      ctx.addIssue({ code: "custom", message: `No wire value codec for ${tag}.` });
      return external_exports.NEVER;
    }
    if (codec2.test(value)) return value;
    if (typeof value !== "object" || value === null || Array.isArray(value)) {
      ctx.addIssue({ code: "custom", message: `Expected the fields of a ${tag}.` });
      return external_exports.NEVER;
    }
    const loaded = codec2.load(value);
    if (!isOk(loaded)) {
      ctx.addIssue({ code: "custom", message: loaded.message });
      return external_exports.NEVER;
    }
    return loaded;
  });
}
var byteRecordSchema = external_exports.record(
  external_exports.string(),
  // `z.instanceof` narrows to `Uint8Array<ArrayBuffer>`, which rejects the
  // `Uint8Array<ArrayBufferLike>` that `utf8Encode` and friends return.
  external_exports.union([
    external_exports.custom((value) => value instanceof Uint8Array),
    external_exports.string()
  ]).transform((value, ctx) => {
    if (value instanceof Uint8Array) return value;
    const decoded = base64Decode(value);
    if (!isOk(decoded)) {
      ctx.addIssue({
        code: "custom",
        message: "A byte field takes bytes or the base64 the wire spells them as."
      });
      return external_exports.NEVER;
    }
    return decoded;
  })
).default({});
var a11PeerSchema = external_exports.object({
  protocol: external_exports.enum(["a11", "mcp"]).default("a11"),
  scheme: external_exports.enum(["session", "ws", "wss", "http", "https", "rtc"]).default("session"),
  identity: external_exports.string().default(""),
  endpoint: external_exports.string().default("")
});
var a11ActionConfigSchema = external_exports.object({
  peer: external_exports.union([external_exports.string(), taggedOr(PEER_TAG, a11PeerSchema)]).default("a11://$sender"),
  header_autofills: byteRecordSchema
});
var interactionSchema = external_exports.object({
  id: external_exports.string().default(() => randomUuid()),
  role: roleSchema.default(DEFAULT_ROLE),
  created_at_millis: external_exports.number().int().nullish(),
  previous_interaction_id: external_exports.string().default(""),
  model: external_exports.string().default(""),
  // These types are the only thing that says what the wire carries here, so
  // they mirror the Python model's annotations exactly. Loosening one back to
  // `z.unknown()` would leave its values as anonymous field maps.
  status: wireValueField(STATUS_TAG).optional(),
  system_instructions: external_exports.array(wireValueField(CHUNK_TAG)).default([]),
  action_configs: external_exports.record(external_exports.string(), taggedOr(ACTION_CONFIG_TAG, a11ActionConfigSchema)).default({}),
  content: external_exports.array(wireValueField(CHUNK_TAG)).default([]),
  action_calls: external_exports.array(wireValueField(ACTION_MESSAGE_TAG)).default([]),
  action_inputs: external_exports.record(external_exports.string(), external_exports.array(wireValueField(NODE_FRAGMENT_TAG))).default({}),
  action_outputs: external_exports.record(external_exports.string(), external_exports.array(wireValueField(NODE_FRAGMENT_TAG))).default({}),
  backend_specific_metadata: byteRecordSchema,
  usage_metadata: taggedOr(USAGE_METADATA_TAG, usageMetadataSchema).nullish()
}).loose();
function parseInteraction(value) {
  const parsed = zodParse(interactionSchema, value, "Interaction");
  if (!isOk(parsed)) return parsed;
  return tagValue(parsed, INTERACTION_TAG);
}
function makeInteraction(partial2 = {}) {
  return parseInteraction(partial2);
}
async function makeTextMessageInteraction(text, systemPrompt = "", role = "user" /* USER */) {
  if (role === "system" /* SYSTEM */) {
    return invalidArgumentError(
      "A text message interaction cannot use the system role as content."
    );
  }
  const roleStr = role === "model" /* ASSISTANT */ ? "model" : "user";
  const content = await toChunk({ role: roleStr, content: [{ type: "text", text }] });
  if (!isOk(content)) return content;
  const instructions = [];
  if (systemPrompt) {
    const instruction = await toChunk(systemPrompt);
    if (!isOk(instruction)) return instruction;
    instructions.push(instruction);
  }
  return makeInteraction({
    role,
    content: [content],
    system_instructions: instructions
  });
}
function randomUuid() {
  const generator = globalThis.crypto?.randomUUID;
  if (typeof generator === "function") return generator.call(globalThis.crypto);
  return `${Date.now().toString(16)}-${Math.floor(
    Math.random() * 4294967295
  ).toString(16)}`;
}
function registerModelCodec(tag, schema, context) {
  registerWireValueCodec({
    tag,
    test: testTagged(tag),
    dump: (value) => ({ ...value }),
    load: (fields) => {
      const parsed = zodParse(schema, fields, context);
      if (!isOk(parsed)) return parsed;
      return tagValue(parsed, tag);
    }
  });
}
registerModelCodec(INTERACTION_TAG, interactionSchema, "Interaction");
registerModelCodec(PEER_TAG, a11PeerSchema, "A11Peer");
registerModelCodec(ACTION_CONFIG_TAG, a11ActionConfigSchema, "A11ActionConfig");
registerModelCodec(USAGE_METADATA_TAG, usageMetadataSchema, "UsageMetadata");
var BACKEND_METADATA_KEY = "backend";
var interactionNormalizers = /* @__PURE__ */ new Map();
function registerInteractionNormalizer(backend, normalizer) {
  interactionNormalizers.set(String(backend), normalizer);
}

// src/sdk/jsonschema.ts
function zodToJsonSchema(schema) {
  return noexcept(
    () => external_exports.toJSONSchema(schema, { io: "input" }),
    "Could not convert the zod schema to JSON Schema."
  );
}
function isPlainObject2(value) {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
function isDedupableJsonschema(resolved) {
  if ("enum" in resolved) return true;
  return resolved.type === "object" && "properties" in resolved;
}
function organiseAndDeduplicateJsonschema(schema) {
  const rootDefs = {};
  const keyToName = /* @__PURE__ */ new Map();
  const inProgress = /* @__PURE__ */ new Map();
  const dedupeKey = (value) => stableStringify(value);
  const uniqueName = (preferred) => {
    let name = preferred;
    let suffix = 2;
    while (name in rootDefs) {
      name = `${preferred}__${suffix}`;
      suffix += 1;
    }
    return name;
  };
  const register = (preferredName, resolved) => {
    const key = dedupeKey(resolved);
    const existing = keyToName.get(key);
    if (existing !== void 0) return existing;
    const name = uniqueName(preferredName);
    rootDefs[name] = resolved;
    keyToName.set(key, name);
    return name;
  };
  const resolveNamed = (name, target, scopeStack) => {
    if (isPlainObject2(target) || Array.isArray(target)) {
      const existingProgress = inProgress.get(target);
      if (existingProgress !== void 0) return existingProgress;
    }
    const reservedName = uniqueName(name);
    rootDefs[reservedName] = null;
    if (isPlainObject2(target) || Array.isArray(target)) {
      inProgress.set(target, reservedName);
    }
    let resolved;
    try {
      resolved = walk(target, scopeStack, false);
    } finally {
      if (isPlainObject2(target) || Array.isArray(target)) {
        inProgress.delete(target);
      }
    }
    const key = dedupeKey(resolved);
    const existing = keyToName.get(key);
    if (existing !== void 0 && existing !== reservedName) {
      delete rootDefs[reservedName];
      return existing;
    }
    rootDefs[reservedName] = resolved;
    keyToName.set(key, reservedName);
    return reservedName;
  };
  const walk = (node, scopeStack, wrap = true) => {
    if (Array.isArray(node)) return node.map((item) => walk(item, scopeStack));
    if (!isPlainObject2(node)) return node;
    let scopes = scopeStack;
    const localDefs = node.$defs;
    if (isPlainObject2(localDefs)) scopes = [...scopeStack, localDefs];
    const ref = node.$ref;
    if (typeof ref === "string" && ref.startsWith("#/$defs/")) {
      const name = ref.slice("#/$defs/".length);
      let target;
      for (let index = scopes.length - 1; index >= 0; index -= 1) {
        const scope = scopes[index];
        if (name in scope) {
          target = scope[name];
          break;
        }
      }
      const overrides = {};
      for (const [key, value] of Object.entries(node)) {
        if (key !== "$ref" && key !== "$defs") overrides[key] = walk(value, scopes);
      }
      if (target === void 0) return { $ref: ref, ...overrides };
      const finalName = resolveNamed(name, target, scopes);
      return { $ref: `#/$defs/${finalName}`, ...overrides };
    }
    const resolved = {};
    for (const [key, value] of Object.entries(node)) {
      if (key !== "$defs") resolved[key] = walk(value, scopes);
    }
    if (wrap && typeof resolved.title === "string" && isDedupableJsonschema(resolved)) {
      const name = register(resolved.title, resolved);
      return { $ref: `#/$defs/${name}` };
    }
    return resolved;
  };
  const organised = walk(schema, [], false);
  if (Object.keys(rootDefs).length > 0) {
    return { $defs: rootDefs, ...organised };
  }
  return organised;
}
function stableStringify(value) {
  return JSON.stringify(value, (_key, val) => {
    if (isPlainObject2(val)) {
      const sorted = {};
      for (const key of Object.keys(val).sort()) sorted[key] = val[key];
      return sorted;
    }
    return val;
  });
}

// src/sdk/tool_adapter.ts
function mimeToJsonSchema(mimetype) {
  const mediaType = mimetype.split(";")[0]?.trim().toLowerCase() ?? "";
  if (mediaType.startsWith("text/")) return { type: "string" };
  return { type: "object" };
}
var ToolAdapter = class {
  constructor(schema, portSchemas = {}) {
    this.schema = schema;
    this.portSchemas = portSchemas;
  }
  /** JSON Schema for the tool's callable inputs (autofilled inputs excluded). */
  getInputSchema() {
    const properties = {};
    const requiredNodes = [];
    for (const port of this.schema.inputs.values()) {
      if (port.autofills.length > 0) continue;
      const nodeSchema = this.nodeSchema(port);
      if (!isOk(nodeSchema)) return nodeSchema;
      if (port.required) requiredNodes.push(port.name);
      properties[port.name] = this.wrapCollection(nodeSchema, port, port.required);
    }
    return organiseAndDeduplicateJsonschema({
      type: "object",
      properties,
      required: requiredNodes
    });
  }
  /** JSON Schema for the tool's result, honoring `output_to_json_field`. */
  getOutputSchema() {
    const properties = {};
    const requiredNodes = [];
    for (const port of this.schema.outputs.values()) {
      const nodeSchema = this.nodeSchema(port);
      if (!isOk(nodeSchema)) return nodeSchema;
      if (port.required) requiredNodes.push(port.name);
      properties[port.name] = this.wrapCollection(nodeSchema, port, true);
    }
    const substitutions = this.schema.outputToJsonField;
    let schema;
    if (substitutions.size === 0) {
      schema = { type: "object", properties, required: requiredNodes };
    } else if (substitutions.size === 1 && [...substitutions.values()][0] === WHOLE_JSON_OUTPUT) {
      const only = [...substitutions.keys()][0];
      schema = properties[only];
    } else {
      for (const [name, substitution] of substitutions) {
        if (name in properties) {
          properties[substitution] = properties[name];
          delete properties[name];
        }
      }
      schema = { type: "object", properties, required: requiredNodes };
    }
    return organiseAndDeduplicateJsonschema(schema);
  }
  nodeSchema(port) {
    const provided = this.portSchemas[port.name];
    if (provided !== void 0) return zodToJsonSchema(provided);
    return mimeToJsonSchema(port.type);
  }
  wrapCollection(nodeSchema, port, minOne) {
    if (port.unary) return nodeSchema;
    const wrapped = { type: "array", items: nodeSchema };
    if (minOne) wrapped.minItems = 1;
    return wrapped;
  }
};

// src/sdk/tool_runner.ts
function getToolDefinitions(registry2, allowedActions = [], portSchemas = {}) {
  if (registry2 === null) return [];
  const definitions = [];
  for (const name of allowedActions) {
    const schema = registry2.getSchema(name);
    if (!isOk(schema)) {
      if (schema.code === notFoundError().code) continue;
      return schema;
    }
    const adapter = new ToolAdapter(schema, portSchemas[schema.name] ?? {});
    const inputSchema = adapter.getInputSchema();
    if (!isOk(inputSchema)) return inputSchema;
    definitions.push({
      name: schema.name,
      description: schema.description,
      input_schema: inputSchema
    });
  }
  return definitions;
}

// src/sdk/gemma/interact_with_gemma.ts
var DEFAULT_MODEL = "gemma-4-E2B-it";
var DEFAULT_MODEL_ASSET_PATH = "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it-web.litertlm?download=true";
var DEFAULT_STOP_SEQUENCES = ["<end_of_turn>", "<start_of_turn>", "<eos>"];
var gemmaConfigSchema = external_exports.object({
  model_asset_path: external_exports.string().default(DEFAULT_MODEL_ASSET_PATH).describe(
    "URL of the Gemma model asset (`.task`/`.litertlm`) to download and run on WebGPU. HTTP redirects are followed when fetching it."
  ),
  max_tokens: external_exports.number().int().default(1024).describe("Maximum number of tokens to generate for a turn."),
  temperature: external_exports.number().nullish().describe("Sampling temperature; higher is more random."),
  top_k: external_exports.number().int().nullish().describe("Top-k sampling cutoff."),
  random_seed: external_exports.number().int().nullish().describe("Sampling seed for reproducible output."),
  stop_sequences: external_exports.array(external_exports.string()).default(DEFAULT_STOP_SEQUENCES).describe(
    "Text sequences that end generation. Output is truncated at the first occurrence so turn markers do not leak and the model does not continue past its own turn."
  ),
  runtime_url: external_exports.string().default("https://cdn.jsdelivr.net/npm/@mediapipe/tasks-genai").describe("ES module URL of the MediaPipe GenAI tasks runtime."),
  wasm_base: external_exports.string().default("https://cdn.jsdelivr.net/npm/@mediapipe/tasks-genai/wasm").describe("Base URL of the MediaPipe GenAI WebAssembly fileset.")
});
var CONFIG_READ_TIMEOUT_MS = 2e4;
var MODEL_CACHE_NAME = "a11-gemma-models";
async function openModelCache() {
  try {
    if (typeof caches === "undefined") return null;
    return await caches.open(MODEL_CACHE_NAME);
  } catch {
    return null;
  }
}
async function fetchModelAssetBuffer(url2) {
  const cache = await openModelCache();
  if (cache !== null) {
    try {
      const hit = await cache.match(url2);
      if (hit !== void 0) return new Uint8Array(await hit.arrayBuffer());
    } catch {
    }
  }
  const response = await noexceptFetch(url2, { redirect: "follow" });
  if (!isOk(response)) return response;
  if (!response.ok) {
    const status = await statusFromResponse(response, "Downloading the Gemma model");
    return isOk(status) ? unavailableError("Downloading the Gemma model failed.") : status;
  }
  let buffer;
  try {
    buffer = await response.arrayBuffer();
  } catch (error51) {
    return statusFromUnknown(error51, "Reading the downloaded Gemma model failed.");
  }
  if (cache !== null) {
    try {
      await cache.put(
        url2,
        new Response(buffer, { headers: { "content-type": "application/octet-stream" } })
      );
    } catch {
    }
  }
  return new Uint8Array(buffer);
}
async function defaultGemmaEngineFactory(config2) {
  if (!config2.model_asset_path) {
    return invalidArgumentError(
      "GemmaConfig.model_asset_path is required to load a browser model."
    );
  }
  if (typeof globalThis.navigator === "undefined" || !("gpu" in globalThis.navigator)) {
    return unavailableError(
      "WebGPU is not available in this environment; a Gemma model cannot run."
    );
  }
  let llm;
  try {
    const runtimeUrl = config2.runtime_url;
    const mediapipe = await import(
      /* @vite-ignore */
      runtimeUrl
    );
    const fileset = await mediapipe.FilesetResolver.forGenAiTasks(config2.wasm_base);
    const modelBuffer = await fetchModelAssetBuffer(config2.model_asset_path);
    if (!isOk(modelBuffer)) return modelBuffer;
    const options = {
      baseOptions: { modelAssetBuffer: modelBuffer },
      maxTokens: config2.max_tokens
    };
    if (config2.top_k !== void 0 && config2.top_k !== null) options.topK = config2.top_k;
    if (config2.temperature !== void 0 && config2.temperature !== null) {
      options.temperature = config2.temperature;
    }
    if (config2.random_seed !== void 0 && config2.random_seed !== null) {
      options.randomSeed = config2.random_seed;
    }
    llm = await mediapipe.LlmInference.createFromOptions(fileset, options);
  } catch (error51) {
    return statusFromUnknown(
      error51,
      "Loading the MediaPipe Gemma runtime failed.",
      unavailableError().code
    );
  }
  const engine = {
    generate(prompt, onToken) {
      return new Promise((resolve) => {
        try {
          let full = "";
          const generateResponse = llm.generateResponse;
          generateResponse(prompt, (partial2, done) => {
            if (partial2) {
              full += partial2;
              try {
                onToken(partial2);
              } catch {
              }
            }
            if (done) resolve(full);
          });
        } catch (error51) {
          resolve(statusFromUnknown(error51, "Gemma generation failed."));
        }
      });
    },
    close() {
      try {
        llm.close?.();
      } catch {
      }
    }
  };
  return engine;
}
var gemmaEngineFactory = defaultGemmaEngineFactory;
var INTERACT_WITH_GEMMA_SCHEMA = new ActionSchema({
  name: "interact_with_gemma",
  description: "Run a Gemma-family model in the browser over WebGPU, streaming its output.",
  inputs: {
    interactions: new ActionPortSchema({
      name: "interactions",
      type: "application/json",
      required: true
    }),
    tools: new ActionPortSchema({
      name: "tools",
      type: "application/json",
      required: false
    }),
    config: new ActionPortSchema({
      name: "config",
      type: "application/json",
      unary: true,
      required: false
    })
  },
  outputs: {
    event_stream: new ActionPortSchema({
      name: "event_stream",
      type: "application/json",
      required: false
    }),
    thoughts: new ActionPortSchema({ name: "thoughts", type: "text/plain", required: false }),
    text_output: new ActionPortSchema({
      name: "text_output",
      type: "text/plain",
      required: false
    }),
    new_interactions: new ActionPortSchema({
      name: "new_interactions",
      type: "application/json",
      required: true
    })
  },
  headers: {
    ["x-a11-llm-model" /* MODEL */]: new ActionHeaderSchema({
      name: "x-a11-llm-model" /* MODEL */,
      description: "Label recorded for the local model."
    }),
    ["x-a11-allowed-llm-actions" /* ALLOWED_LLM_ACTIONS */]: new ActionHeaderSchema({
      name: "x-a11-allowed-llm-actions" /* ALLOWED_LLM_ACTIONS */,
      description: "The allowed LLM action (tool) name patterns, comma-separated."
    })
  }
});
var CONTROL_TOKEN_PATTERN = /<\|?(?:start_of_turn|end_of_turn|eos|bos|pad)\|?>/g;
function sanitizeGemmaText(text) {
  return text.replace(CONTROL_TOKEN_PATTERN, "").trim();
}
var GemmaStopFilter = class {
  constructor(stops) {
    this.stops = stops;
    this.maxStopLength = stops.reduce((max, stop) => Math.max(max, stop.length), 1);
  }
  raw = "";
  emitted = 0;
  stopped = false;
  maxStopLength;
  /** Feed one streamed piece; return the text that is now safe to show. */
  push(delta) {
    if (this.stopped || !delta) return "";
    this.raw += delta;
    let cut = -1;
    for (const stop of this.stops) {
      const index = this.raw.indexOf(stop);
      if (index >= 0 && (cut === -1 || index < cut)) cut = index;
    }
    if (cut >= 0) {
      this.stopped = true;
      const out2 = this.raw.slice(this.emitted, cut);
      this.emitted = cut;
      return out2;
    }
    const safe = Math.max(this.emitted, this.raw.length - (this.maxStopLength - 1));
    const out = this.raw.slice(this.emitted, safe);
    this.emitted = safe;
    return out;
  }
  /** Flush the held-back tail when generation ends without a stop sequence. */
  finish() {
    if (this.stopped) return "";
    const out = this.raw.slice(this.emitted);
    this.emitted = this.raw.length;
    return out;
  }
};
function entryValue(item) {
  if (!(item instanceof Chunk)) return item;
  const text = utf8Decode(item.data);
  if (!isOk(text)) return null;
  const mediaType = item.mimetype.split(";")[0]?.trim() ?? "";
  if (mediaType.startsWith("text/")) return text;
  if (mediaType !== "" && mediaType !== JSON_MIMETYPE) return null;
  try {
    return JSON.parse(text);
  } catch {
    return text;
  }
}
function extractInteractionText(interaction) {
  const pieces = [];
  for (const entry of interaction.content ?? []) {
    const item = entryValue(entry);
    if (typeof item === "string") {
      pieces.push(item);
      continue;
    }
    if (typeof item !== "object" || item === null) continue;
    const envelope = item;
    const inner = envelope.content;
    if (typeof inner === "string") {
      pieces.push(inner);
    } else if (Array.isArray(inner)) {
      for (const part of inner) {
        if (typeof part === "object" && part !== null) {
          const partObject = part;
          if (partObject.type === "text" && typeof partObject.text === "string") {
            pieces.push(partObject.text);
          }
        } else if (typeof part === "string") {
          pieces.push(part);
        }
      }
    } else if (typeof envelope.text === "string") {
      pieces.push(envelope.text);
    }
  }
  return pieces.join("");
}
function systemInstructionsText(interactions) {
  const lines = [];
  for (const interaction of interactions) {
    for (const entry of interaction.system_instructions ?? []) {
      const instruction = entryValue(entry);
      if (typeof instruction === "string") lines.push(instruction);
      else if (typeof instruction === "object" && instruction !== null && typeof instruction.text === "string") {
        lines.push(instruction.text);
      }
    }
  }
  return lines.join("\n\n");
}
function buildGemmaPrompt(interactions) {
  const system = systemInstructionsText(interactions);
  const turns = [];
  let first = true;
  for (const interaction of interactions) {
    const text = extractInteractionText(interaction);
    if (!text) continue;
    const role = interaction.role === "model" /* ASSISTANT */ ? "model" : "user";
    let body = text;
    if (first && role === "user" && system) {
      body = `${system}

${text}`;
      first = false;
    }
    turns.push(`<start_of_turn>${role}
${body}<end_of_turn>
`);
  }
  return `${turns.join("")}<start_of_turn>model
`;
}
function gemmaToNormalized(interaction) {
  const text = extractInteractionText(interaction);
  const parts = [];
  if (text) parts.push({ type: "text" /* TEXT */, text });
  const role = interaction.role === "model" /* ASSISTANT */ ? "model" /* ASSISTANT */ : "user" /* USER */;
  return { role, parts };
}
registerInteractionNormalizer("gemma" /* GEMMA */, gemmaToNormalized);
async function closeStream(node) {
  await node.putNullFinal();
  await node.drainAndClose();
}
async function interactWithGemma(action) {
  const eventNodeResult = await action.getOutput("event_stream", false);
  const thoughtsNodeResult = await action.getOutput("thoughts", false);
  const textNodeResult = await action.getOutput("text_output", false);
  const newInteractionsResult = await action.getOutput("new_interactions", false);
  for (const result of [
    eventNodeResult,
    thoughtsNodeResult,
    textNodeResult,
    newInteractionsResult
  ]) {
    if (!isOk(result)) return result;
  }
  const eventNode = eventNodeResult;
  const thoughtsNode = thoughtsNodeResult;
  const textNode = textNodeResult;
  const newInteractionsNode = newInteractionsResult;
  try {
    const modelHeader = action.getHeader("x-a11-llm-model" /* MODEL */);
    if (!isOk(modelHeader)) return modelHeader;
    let model = DEFAULT_MODEL;
    if (modelHeader !== null) {
      const decoded = utf8Decode(modelHeader);
      if (!isOk(decoded)) return decoded;
      if (decoded) model = decoded;
    }
    const configNode = await action.getInput("config");
    if (!isOk(configNode)) return configNode;
    const rawConfig = await configNode.consume({
      allowNone: true,
      timeoutMs: CONFIG_READ_TIMEOUT_MS
    });
    let configValue = null;
    if (isOk(rawConfig)) configValue = rawConfig;
    const config2 = zodParse(gemmaConfigSchema, configValue ?? {}, "GemmaConfig");
    if (!isOk(config2)) return config2;
    const interactionsNode = await action.getInput("interactions");
    if (!isOk(interactionsNode)) return interactionsNode;
    const interactions = [];
    let previousInteractionId = "";
    while (true) {
      const value = await interactionsNode.next();
      if (!isOk(value)) return value;
      if (value === null) break;
      const parsed = parseInteraction(value);
      if (!isOk(parsed)) return parsed;
      interactions.push(parsed);
      previousInteractionId = parsed.id;
    }
    if (interactions.length === 0) {
      return invalidArgumentError("At least one interaction is required.");
    }
    const engine = await gemmaEngineFactory(config2, action.signal);
    if (!isOk(engine)) return engine;
    const prompt = buildGemmaPrompt(interactions);
    let writeChain = Promise.resolve(0);
    let writeError = null;
    const chain = (work) => {
      writeChain = writeChain.then(async (previous) => {
        if (writeError !== null) return previous;
        const result = await work();
        if (!isOk(result)) writeError = result;
        return result;
      });
    };
    const stopFilter = new GemmaStopFilter(config2.stop_sequences);
    let visible = "";
    const show = (piece) => {
      if (!piece) return;
      visible += piece;
      chain(() => textNode.put(piece));
      chain(() => eventNode.put({ type: "token", text: piece }));
    };
    const emit = (delta) => show(stopFilter.push(delta));
    const generated = await engine.generate(prompt, emit, action.signal);
    if (isOk(generated)) show(stopFilter.finish());
    await writeChain;
    try {
      engine.close?.();
    } catch {
    }
    if (!isOk(generated)) return generated;
    if (writeError !== null) return writeError;
    const replyText = sanitizeGemmaText(visible);
    const replyChunk = await toChunk({
      role: "model",
      content: [{ type: "text", text: replyText }]
    });
    if (!isOk(replyChunk)) return replyChunk;
    const assistant = makeInteraction({
      role: "model" /* ASSISTANT */,
      model,
      created_at_millis: Date.now(),
      previous_interaction_id: previousInteractionId,
      content: [replyChunk],
      backend_specific_metadata: { [BACKEND_METADATA_KEY]: utf8Encode("gemma" /* GEMMA */) }
    });
    if (!isOk(assistant)) return assistant;
    const put2 = await newInteractionsNode.put(assistant, { final: true });
    if (!isOk(put2)) return put2;
    await eventNode.put({ type: "done" });
    await eventNode.drainAndClose();
    await closeStream(thoughtsNode);
    await textNode.drainAndClose();
    await newInteractionsNode.drainAndClose();
    return okStatus();
  } catch (error51) {
    return statusFromUnknown(error51, "interact_with_gemma raised an exception.");
  }
}

// src/sdk/interact_with_llm.ts
var PROVIDERS = {
  gemma: { handler: interactWithGemma, note: "" },
  claude: {
    handler: null,
    note: "The claude backend is only available in the Python package (a11-kit[claude])."
  },
  gemini: {
    handler: null,
    note: "The gemini backend is only available in the Python package (a11-kit[gemini])."
  },
  ollama: {
    handler: null,
    note: "The ollama backend is only available in the Python package (a11-kit[ollama])."
  }
};
var INTERACT_WITH_LLM_SCHEMA = new ActionSchema({
  name: "interact_with_llm",
  description: `Route an LLM interaction to a concrete backend chosen by the ${"x-a11-llm-provider" /* PROVIDER */} header.`,
  inputs: {
    interactions: new ActionPortSchema({
      name: "interactions",
      type: "application/json",
      required: true
    }),
    tools: new ActionPortSchema({ name: "tools", type: "application/json", required: false }),
    config: new ActionPortSchema({
      name: "config",
      type: "application/json",
      unary: true,
      required: false
    })
  },
  outputs: {
    event_stream: new ActionPortSchema({
      name: "event_stream",
      type: "application/json",
      required: false
    }),
    thoughts: new ActionPortSchema({ name: "thoughts", type: "text/plain", required: false }),
    text_output: new ActionPortSchema({
      name: "text_output",
      type: "text/plain",
      required: false
    }),
    new_interactions: new ActionPortSchema({
      name: "new_interactions",
      type: "application/json",
      required: true
    })
  },
  headers: {
    ["x-a11-llm-api-key" /* API_KEY */]: new ActionHeaderSchema({
      name: "x-a11-llm-api-key" /* API_KEY */,
      description: "The backend API key."
    }),
    ["x-a11-llm-provider" /* PROVIDER */]: new ActionHeaderSchema({
      name: "x-a11-llm-provider" /* PROVIDER */,
      description: `Which backend to route to, one of: ${Object.keys(PROVIDERS).join(", ")}.`
    }),
    ["x-a11-llm-model" /* MODEL */]: new ActionHeaderSchema({
      name: "x-a11-llm-model" /* MODEL */,
      description: "The downstream model."
    }),
    ["x-a11-llm-base-url" /* BASE_URL */]: new ActionHeaderSchema({
      name: "x-a11-llm-base-url" /* BASE_URL */,
      description: "The downstream base URL, where applicable."
    }),
    ["x-a11-allowed-llm-actions" /* ALLOWED_LLM_ACTIONS */]: new ActionHeaderSchema({
      name: "x-a11-allowed-llm-actions" /* ALLOWED_LLM_ACTIONS */,
      description: "The allowed action (tool) name patterns, comma-separated."
    })
  }
});

// demo/demo_support.ts
var need = (value) => {
  if (!isOk(value)) throw new Error(`${StatusCode[value.code]}: ${value.message}`);
  return value;
};
var READ_TIMEOUT_MS = 3e5;
var DEFAULT_SERVER_URL = "wss://a11.services:9443/a11-demos";
var BACKEND_DEFAULTS = {
  ollama: { model: "glm-4.7-flash", baseUrl: "http://127.0.0.1:11434" },
  claude: { model: "claude-sonnet-4-6", baseUrl: "" },
  gemini: { model: "gemini-3.5-flash", baseUrl: "" }
};
var BackendControls = class {
  provider;
  model;
  apiKey;
  baseUrl;
  server;
  constructor(prefix, onChange) {
    this.provider = document.querySelector(`#${prefix}-provider`);
    this.model = document.querySelector(`#${prefix}-model`);
    this.apiKey = document.querySelector(`#${prefix}-api-key`);
    this.baseUrl = document.querySelector(`#${prefix}-base-url`);
    this.server = document.querySelector(`#${prefix}-server`);
    this.provider.onchange = () => {
      const defaults = BACKEND_DEFAULTS[this.provider.value];
      if (defaults) {
        this.model.value = defaults.model;
        this.baseUrl.value = defaults.baseUrl;
      }
      onChange?.();
    };
  }
  get value() {
    return {
      provider: this.provider.value,
      model: this.model.value.trim(),
      apiKey: this.apiKey.value.trim(),
      baseUrl: this.baseUrl.value.trim()
    };
  }
};
function LlmHeadersFor(backend) {
  const headers = [
    ["x-a11-llm-provider" /* PROVIDER */, backend.provider],
    ["x-a11-llm-model" /* MODEL */, backend.model]
  ];
  if (backend.apiKey) headers.push(["x-a11-llm-api-key" /* API_KEY */, backend.apiKey]);
  if (backend.baseUrl) headers.push(["x-a11-llm-base-url" /* BASE_URL */, backend.baseUrl]);
  return headers;
}
function webSocketUrl(url2) {
  return url2.trim().replace(/^http(s?):\/\//i, "ws$1://");
}
async function connect(url2, registry2 = new ActionRegistry()) {
  const session = need(Session.create({ actionRegistry: registry2, noStreamTimeoutMs: null }));
  const stream = need(WebSocketWireStream.connect(webSocketUrl(url2)));
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
async function drainPort(action, port) {
  try {
    await readPort(action, port, () => {
    });
  } catch {
  }
}
var REGISTER_TOOLS_SCHEMA = new ActionSchema({
  name: "__register_tools__",
  description: "Announce the caller's tool schemas for reverse dispatch.",
  inputs: { tools: new ActionPortSchema({ name: "tools", type: "application/json", required: true }) },
  outputs: { ok: new ActionPortSchema({ name: "ok", type: "application/json", required: true }) }
});
var USER_FACING_LOG_PORT = "user_facing_log";
function describePort(port, userFacing = false) {
  const described = {
    name: port.name,
    type: port.type,
    description: port.description,
    required: port.required,
    unary: port.unary
  };
  if (userFacing) described.user_facing = true;
  return described;
}
function describeTool(schema) {
  return {
    name: schema.name,
    description: schema.description,
    inputs: [...schema.inputs.values()].map((port) => describePort(port)),
    // Flagging the log port is what moves it onto the canonical name on the far
    // side, where the tool runner drains it, keeps it away from the model, and
    // files it under the call — so a replayed conversation still shows what a
    // tool did rather than only that it ran.
    outputs: [...schema.outputs.values()].map(
      (port) => describePort(port, port.name === USER_FACING_LOG_PORT)
    )
  };
}
async function announceTools(connection, schemas) {
  const announce = makeCall(connection, REGISTER_TOOLS_SCHEMA);
  need(await announce.call());
  const tools = need(await announce.getInput("tools"));
  for (const schema of schemas) need(await tools.put(describeTool(schema)));
  need(await tools.putNullFinal());
  need(await tools.drainAndClose());
  const ok = need(await announce.getOutput("ok", false));
  const acknowledged = need(await ok.next({ timeoutMs: 3e4 }));
  need(await announce.wait(3e4));
  return acknowledged?.registered ?? [];
}
async function runTurn(request) {
  const { connection, backend, prompt, history } = request;
  const registry2 = connection.session.getActionRegistry();
  const call = makeCall(connection, INTERACT_WITH_LLM_SCHEMA);
  for (const [header, value] of LlmHeadersFor(backend)) need(call.setHeader(header, value));
  const toolNames = (request.tools ?? []).map((schema) => schema.name);
  if (toolNames.length > 0) {
    need(call.setHeader("x-a11-allowed-llm-actions" /* ALLOWED_LLM_ACTIONS */, toolNames.join(",")));
  }
  need(await call.call());
  const question = need(
    await makeTextMessageInteraction(prompt, history.length === 0 ? request.systemPrompt ?? "" : "")
  );
  const interactions = need(await call.getInput("interactions"));
  for (const interaction of history) need(await interactions.put(interaction));
  need(await interactions.putFinal(question));
  need(await interactions.drainAndClose());
  const config2 = need(await call.getInput("config"));
  need(await config2.putNullFinal());
  need(await config2.drainAndClose());
  const definitions = need(getToolDefinitions(registry2, toolNames, request.portSchemas ?? {}));
  const tools = need(await call.getInput("tools"));
  for (const definition of definitions) need(await tools.put(definition));
  need(await tools.putNullFinal());
  need(await tools.drainAndClose());
  const produced = [];
  const thoughts = request.onThought ? readPort(call, "thoughts", (value) => request.onThought?.(String(value))).catch(() => {
  }) : Promise.resolve();
  const interactionsOut = readPort(call, "new_interactions", (value) => {
    produced.push(value);
  });
  const events = drainPort(call, "event_stream");
  await readPort(call, "text_output", (value) => request.onToken?.(String(value)));
  await thoughts;
  await interactionsOut;
  await events;
  need(await call.wait(READ_TIMEOUT_MS));
  return [question, ...produced];
}
function addBubble(container, text, kind) {
  const bubble = document.createElement("div");
  bubble.className = `a11-bubble ${kind}`;
  bubble.textContent = text;
  container.append(bubble);
  container.scrollTop = container.scrollHeight;
  return bubble;
}
function addLine(container, text, kind = "") {
  const line = document.createElement("div");
  line.className = `a11-log-line ${kind}`.trim();
  line.textContent = text;
  container.append(line);
  container.scrollTop = container.scrollHeight;
}
function streamInto(bubble, container) {
  return (text) => {
    bubble.textContent = `${bubble.textContent ?? ""}${text}`;
    container.scrollTop = container.scrollHeight;
  };
}
function showError(region, error51) {
  region.textContent = error51 instanceof Error ? error51.message : String(error51);
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

// demo/browser_tools.ts
var PALETTE = ["#4f6df5", "#f5a34f", "#4fb0f5", "#a34ff5", "#4ff5a3"];
var Scene = class {
  canvas = document.querySelector("#tools-canvas");
  blobs = [];
  constructor() {
    for (let index = 0; index < 5; index += 1) {
      this.blobs.push({
        id: index,
        x: 90 + index * 110,
        y: 150,
        radius: 34,
        color: PALETTE[index]
      });
    }
    this.draw();
  }
  find(id) {
    return this.blobs.find((blob) => blob.id === id);
  }
  draw() {
    const context = this.canvas.getContext("2d");
    if (!context) return;
    context.clearRect(0, 0, this.canvas.width, this.canvas.height);
    for (const blob of this.blobs) {
      context.beginPath();
      context.arc(blob.x, blob.y, blob.radius, 0, Math.PI * 2);
      context.fillStyle = blob.color;
      context.fill();
      context.fillStyle = "#00000099";
      context.font = "13px system-ui, sans-serif";
      context.textAlign = "center";
      context.fillText(String(blob.id), blob.x, blob.y + 4);
    }
  }
  /** Move a blob over half a second, so the model's work is visible. */
  async glide(blob, x, y) {
    const from = { x: blob.x, y: blob.y };
    const frames = 30;
    for (let frame = 1; frame <= frames; frame += 1) {
      blob.x = from.x + (x - from.x) * frame / frames;
      blob.y = from.y + (y - from.y) * frame / frames;
      this.draw();
      await new Promise((resolve) => setTimeout(resolve, 500 / frames));
    }
  }
};
var DESCRIBE_SCENE_SCHEMA = new ActionSchema({
  name: "describe_scene",
  description: "List the blobs on the page: their ids, colours and positions. Call this before changing anything, to find out what is there.",
  outputs: {
    blobs: new ActionPortSchema({
      name: "blobs",
      type: "application/json",
      required: true,
      description: "One `{id, x, y, radius, color}` per blob."
    })
  }
});
var SET_COLOR_SCHEMA = new ActionSchema({
  name: "set_color",
  description: "Recolour blobs: the i-th id is given the i-th colour.",
  inputs: {
    ids: new ActionPortSchema({
      name: "ids",
      type: "application/json",
      required: true,
      description: "Which blobs to recolour."
    }),
    colors: new ActionPortSchema({
      name: "colors",
      type: "text/plain",
      required: true,
      description: "One `#rrggbb` per id, in the same order."
    })
  },
  outputs: {
    recoloured: new ActionPortSchema({
      name: "recoloured",
      type: "application/json",
      unary: true,
      required: true,
      description: "How many blobs changed colour."
    }),
    [USER_FACING_LOG_PORT]: new ActionPortSchema({
      name: USER_FACING_LOG_PORT,
      type: "text/plain",
      description: "What the page did, for the person watching."
    })
  }
});
var SHIFT_POSITION_SCHEMA = new ActionSchema({
  name: "shift_position",
  description: "Move blobs by an offset in pixels. +x is right and +y is down.",
  inputs: {
    ids: new ActionPortSchema({
      name: "ids",
      type: "application/json",
      required: true,
      description: "Which blobs to move."
    }),
    dx: new ActionPortSchema({
      name: "dx",
      type: "application/json",
      unary: true,
      required: true,
      description: "How far to move them horizontally, in pixels."
    }),
    dy: new ActionPortSchema({
      name: "dy",
      type: "application/json",
      unary: true,
      required: true,
      description: "How far to move them vertically, in pixels."
    })
  },
  outputs: {
    moved: new ActionPortSchema({
      name: "moved",
      type: "application/json",
      unary: true,
      required: true,
      description: "How many blobs moved."
    }),
    [USER_FACING_LOG_PORT]: new ActionPortSchema({
      name: USER_FACING_LOG_PORT,
      type: "text/plain",
      description: "What the page did, for the person watching."
    })
  }
});
var PAGE_TOOLS = [DESCRIBE_SCENE_SCHEMA, SET_COLOR_SCHEMA, SHIFT_POSITION_SCHEMA];
var PORT_SCHEMAS = {
  set_color: { ids: external_exports.number().int(), colors: external_exports.string() },
  shift_position: { ids: external_exports.number().int(), dx: external_exports.number(), dy: external_exports.number() }
};
var READ_TIMEOUT_MS2 = 1e4;
async function readAll(action, port) {
  const node = need(await action.getInput(port));
  const values = [];
  for (; ; ) {
    const next = await node.next({ timeoutMs: READ_TIMEOUT_MS2 });
    if (!isOk(next) || next === null) break;
    values.push(next);
  }
  return values;
}
async function narrate(action, text, onLog) {
  onLog(text);
  const node = need(await action.getOutput(USER_FACING_LOG_PORT));
  need(await node.putFinal(text));
  need(await node.drainAndClose());
}
function pageRegistry(scene, onLog) {
  const registry2 = new ActionRegistry();
  need(
    registry2.register(DESCRIBE_SCENE_SCHEMA.name, DESCRIBE_SCENE_SCHEMA, async (action) => {
      try {
        onLog(`describe_scene \u2192 ${scene.blobs.length} blobs`);
        const node = need(await action.getOutput("blobs"));
        for (const [index, blob] of scene.blobs.entries()) {
          need(await node.put({ ...blob }, { final: index === scene.blobs.length - 1 }));
        }
        return await node.drainAndClose();
      } catch (error51) {
        return statusFromUnknown(error51, "describe_scene failed.");
      }
    })
  );
  need(
    registry2.register(SET_COLOR_SCHEMA.name, SET_COLOR_SCHEMA, async (action) => {
      try {
        const [ids, colors] = await Promise.all([readAll(action, "ids"), readAll(action, "colors")]);
        let recoloured = 0;
        for (const [index, id] of ids.entries()) {
          const blob = scene.find(Number(id));
          const color = colors[Math.min(index, colors.length - 1)];
          if (!blob || typeof color !== "string") continue;
          blob.color = color;
          recoloured += 1;
        }
        scene.draw();
        const result = need(await action.getOutput("recoloured"));
        need(await result.putFinal(recoloured));
        need(await result.drainAndClose());
        await narrate(action, `Recoloured ${recoloured} blob(s): ${ids.join(", ")}.`, onLog);
        return okStatus();
      } catch (error51) {
        return statusFromUnknown(error51, "set_color failed.");
      }
    })
  );
  need(
    registry2.register(SHIFT_POSITION_SCHEMA.name, SHIFT_POSITION_SCHEMA, async (action) => {
      try {
        const ids = await readAll(action, "ids");
        const dxNode = need(await action.getInput("dx"));
        const dyNode = need(await action.getInput("dy"));
        const dx = Number(need(await dxNode.consume({ timeoutMs: READ_TIMEOUT_MS2, allowNone: true })) ?? 0);
        const dy = Number(need(await dyNode.consume({ timeoutMs: READ_TIMEOUT_MS2, allowNone: true })) ?? 0);
        const moving = ids.map((id) => scene.find(Number(id))).filter((blob) => blob !== void 0);
        void Promise.all(moving.map((blob) => scene.glide(blob, blob.x + dx, blob.y + dy)));
        const result = need(await action.getOutput("moved"));
        need(await result.putFinal(moving.length));
        need(await result.drainAndClose());
        await narrate(action, `Moved ${moving.length} blob(s) by (${dx}, ${dy}).`, onLog);
        return okStatus();
      } catch (error51) {
        return statusFromUnknown(error51, "shift_position failed.");
      }
    })
  );
  return registry2;
}
var SYSTEM_PROMPT = "You are looking after a small canvas of coloured blobs in a web page. Use the tools to inspect and change it: call describe_scene when you need to know what is there, then set_color or shift_position to act. Do not describe what you would do \u2014 do it, then say in one sentence what you did. Pick colours yourself when none are given; the canvas is 620 by 300 pixels.";
var BrowserToolsDemo = class {
  backend = new BackendControls("tools");
  errors = document.querySelector("#tools-errors");
  messages = document.querySelector("#tools-messages");
  log = document.querySelector("#tools-log");
  scene = new Scene();
  connection = null;
  /**
   * The session, with the page's own registry bound to it *before* the stream is
   * attached, and the tools announced once it is up.
   */
  async connected() {
    if (this.connection !== null) return this.connection;
    const registry2 = pageRegistry(this.scene, (text) => addLine(this.log, text));
    const connection = await connect(this.backend.server.value.trim() || DEFAULT_SERVER_URL, registry2);
    const registered = await announceTools(connection, PAGE_TOOLS);
    addLine(this.log, `announced: ${registered.join(", ")}`, "done");
    this.connection = connection;
    return connection;
  }
  async send(prompt) {
    this.errors.textContent = "";
    addBubble(this.messages, prompt, "question");
    const answer = addBubble(this.messages, "", "answer");
    try {
      const connection = await this.connected();
      await runTurn({
        connection,
        backend: this.backend.value,
        prompt,
        history: [],
        systemPrompt: SYSTEM_PROMPT,
        tools: PAGE_TOOLS,
        portSchemas: PORT_SCHEMAS,
        onToken: streamInto(answer, this.messages)
      });
    } catch (error51) {
      answer.remove();
      this.connection = null;
      showError(this.errors, error51);
    }
  }
};
var root = document.querySelector("#tools-demo");
if (root) {
  const demo = new BrowserToolsDemo();
  const form = document.querySelector("#tools-form");
  const input = document.querySelector("#tools-input");
  form.onsubmit = (event) => {
    event.preventDefault();
    const prompt = input.value.trim();
    if (!prompt) return;
    input.value = "";
    void whileBusy(form, () => demo.send(prompt));
  };
}

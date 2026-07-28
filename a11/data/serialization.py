"""Serialization of Python objects to and from :class:`~a11.data.types.Chunk`.

The registry deliberately separates a media type (the representation) from a
Python type.  A serialized chunk combines the two by adding a stable ``type``
parameter to its MIME type, for example ``application/json;type=dict``.
"""

import asyncio
import base64
import binascii
import datetime
import enum
import fnmatch
import functools
import inspect
import json
import math
import re
import urllib.parse
import uuid
from dataclasses import dataclass
from typing import Any, Callable, Sequence, TypeVar, cast

import msgpack
import pydantic

from a11 import _native, timing
from a11.data import types
from a11.status import Status, StatusCode, StatusException

SerializedData = str | bytes | bytearray | memoryview | types.Chunk
SerializerFn = Callable[[Any], SerializedData]
DeserializerFn = Callable[..., Any]

JSON_MIMETYPE = _native.JSON_MIMETYPE
MSGPACK_MIMETYPE = _native.MSGPACK_MIMETYPE

_TYPE_PARAMETER = "type"
_MIME_TOKEN_RE = re.compile(r"^[!#$%&'*+.^_`|~0-9A-Za-z-]+$")
_MIME_PART_RE = re.compile(r"^[!#$%&'*+.^_`|~0-9A-Za-z?*\[\]-]+$")
_F = TypeVar("_F", bound=Callable[..., Any])


def _status_boundary(fn: _F) -> _F:
    """Ensure ordinary exceptions do not escape a registry method."""

    @functools.wraps(fn)
    def wrapped(*args: Any, **kwargs: Any) -> Any:
        try:
            return fn(*args, **kwargs)
        except (
            KeyboardInterrupt,
            asyncio.CancelledError,
            SystemExit,
            GeneratorExit,
        ):
            raise
        except StatusException:
            raise
        except BaseException as exc:
            try:
                status = Status.from_exception(exc)
                if not isinstance(status, Status):
                    raise TypeError(
                        "Status.from_exception() did not return a Status."
                    )
                if status.is_ok():
                    status = Status(
                        code=StatusCode.INTERNAL,
                        message=(
                            "An exception was unexpectedly converted to an OK"
                            " status."
                        ),
                    )
            except StatusException:
                raise
            except (
                KeyboardInterrupt,
                asyncio.CancelledError,
                SystemExit,
                GeneratorExit,
            ):
                raise
            except BaseException:
                try:
                    message = str(exc)
                except BaseException:
                    message = exc.__class__.__name__
                status = Status(
                    code=StatusCode.UNKNOWN,
                    message=f"Unexpected serialization error: {message}",
                )
            raise status.to_exception() from exc

    return cast(_F, wrapped)


def _raise_status(code: StatusCode, message: str) -> None:
    raise Status(code=code, message=message).to_exception()


@dataclass(frozen=True, slots=True)
class _Mimetype:
    media_type: str
    parameters: tuple[tuple[str, str], ...] = ()

    def get_parameter(self, name: str) -> str | None:
        name = name.lower()
        for key, value in self.parameters:
            if key == name:
                return value
        return None

    def without_parameter(self, name: str) -> "_Mimetype":
        name = name.lower()
        return _Mimetype(
            self.media_type,
            tuple(
                (key, value) for key, value in self.parameters if key != name
            ),
        )


def _parse_parameter_value(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        value = value[1:-1].replace(r"\"", '"').replace(r"\\", "\\")
    return value


def _parse_mimetype(value: str, *, allow_patterns: bool) -> _Mimetype:
    if not isinstance(value, str) or not value.strip():
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "Mimetype must be a non-empty string.",
        )

    parts = value.split(";")
    media_type = parts[0].strip().lower()
    if media_type.count("/") != 1:
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            f"Invalid mimetype: {value!r}.",
        )

    major, minor = media_type.split("/", 1)
    part_re = _MIME_PART_RE if allow_patterns else _MIME_TOKEN_RE
    if not major or not minor or not part_re.fullmatch(major):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            f"Invalid mimetype: {value!r}.",
        )
    if not part_re.fullmatch(minor):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            f"Invalid mimetype: {value!r}.",
        )
    if not allow_patterns and any(char in media_type for char in "*?["):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "Registered mimetypes must identify an exact media type.",
        )

    parameters: list[tuple[str, str]] = []
    seen: set[str] = set()
    for raw_parameter in parts[1:]:
        name, separator, raw_value = raw_parameter.partition("=")
        name = name.strip().lower()
        parameter_value = _parse_parameter_value(raw_value)
        if (
            not separator
            or not _MIME_TOKEN_RE.fullmatch(name)
            or not parameter_value
            or name in seen
        ):
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                f"Invalid mimetype parameter in {value!r}.",
            )
        if not allow_patterns and any(
            char in parameter_value for char in "*?["
        ):
            if name != _TYPE_PARAMETER or parameter_value != "*":
                _raise_status(
                    StatusCode.INVALID_ARGUMENT,
                    "Registered mimetype parameters cannot be patterns.",
                )
        if name == _TYPE_PARAMETER:
            parameter_value = urllib.parse.unquote(parameter_value)
        parameters.append((name, parameter_value))
        seen.add(name)

    return _Mimetype(media_type, tuple(parameters))


def _format_parameter(value: str) -> str:
    if _MIME_TOKEN_RE.fullmatch(value):
        return value
    escaped = value.replace("\\", r"\\").replace('"', r"\"")
    return f'"{escaped}"'


def _format_exact_mimetype(mimetype: _Mimetype, obj_type: type) -> str:
    parameters = list(mimetype.without_parameter(_TYPE_PARAMETER).parameters)
    type_identifier = urllib.parse.quote(
        obj_type.__name__, safe="!#$%&'*+.^_`|~-"
    )
    parameters.append((_TYPE_PARAMETER, type_identifier))
    suffix = "".join(
        f";{name}={_format_parameter(value)}" for name, value in parameters
    )
    return f"{mimetype.media_type}{suffix}"


def _mimetype_patterns(actual: _Mimetype, pattern: _Mimetype) -> bool:
    if not fnmatch.fnmatchcase(actual.media_type, pattern.media_type):
        return False
    actual_parameters = dict(actual.parameters)
    for name, expected in pattern.parameters:
        actual_value = actual_parameters.get(name)
        if actual_value is None or not fnmatch.fnmatchcase(
            actual_value, expected
        ):
            return False
    return True


def _registration_matches(registered: _Mimetype, selection: _Mimetype) -> bool:
    if not fnmatch.fnmatchcase(registered.media_type, selection.media_type):
        return False

    selected_parameters = dict(selection.parameters)
    for name, value in registered.parameters:
        if name == _TYPE_PARAMETER:
            continue
        selected_value = selected_parameters.get(name)
        if selected_value is not None and not fnmatch.fnmatchcase(
            value, selected_value
        ):
            return False
    return True


def _inheritance_distance(child: type, parent: type) -> int:
    try:
        return child.__mro__.index(parent)
    except ValueError:
        return len(child.__mro__) + 1000


@dataclass(frozen=True, slots=True)
class _SerializerRegistration:
    obj_type: type
    mimetype: _Mimetype
    serializer: SerializerFn
    order: int


@dataclass(frozen=True, slots=True)
class _DeserializerRegistration:
    obj_type: type
    mimetype: _Mimetype
    deserializer: DeserializerFn
    call_mode: str
    receives_chunk: bool
    order: int


def _callable_signature(fn: Callable[..., Any]) -> inspect.Signature | None:
    try:
        return inspect.signature(fn)
    except (TypeError, ValueError):
        return None


def _validate_serializer(serializer: SerializerFn) -> None:
    if not callable(serializer):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "Serializer must be callable.",
        )
    signature = _callable_signature(serializer)
    if signature is None:
        return
    try:
        signature.bind(object())
    except TypeError:
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "Serializer must accept one object argument.",
        )


def _deserializer_call_info(
    deserializer: DeserializerFn,
    receives_chunk: bool | None,
) -> tuple[str, bool]:
    if not callable(deserializer):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "Deserializer must be callable.",
        )

    signature = _callable_signature(deserializer)
    if signature is None:
        return "data", bool(receives_chunk)

    marker = object()
    call_mode = ""
    try:
        signature.bind(marker, marker)
        call_mode = "data_and_type"
    except TypeError:
        try:
            signature.bind(marker, obj_type=marker)
            call_mode = "data_and_keyword_type"
        except TypeError:
            try:
                signature.bind(marker)
                call_mode = "data"
            except TypeError:
                _raise_status(
                    StatusCode.INVALID_ARGUMENT,
                    "Deserializer must accept data and, optionally, an object"
                    " type.",
                )

    if receives_chunk is None:
        positional = [
            parameter
            for parameter in signature.parameters.values()
            if parameter.kind
            in (
                inspect.Parameter.POSITIONAL_ONLY,
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
            )
        ]
        first = positional[0] if positional else None
        annotation = first.annotation if first is not None else None
        receives_chunk = bool(
            first is not None
            and (
                first.name in {"chunk", "serialized_chunk"}
                or annotation is types.Chunk
                or annotation in {"Chunk", "types.Chunk"}
            )
        )

    return call_mode, bool(receives_chunk)


class SerializationRegistry:
    """A registry of serializers and deserializers indexed by type and MIME.

    New registries are empty.  Pass ``register_defaults=True`` or call
    :meth:`register_defaults` to install the built-in JSON and MessagePack
    codecs.  The process-wide registry returned by
    :func:`get_global_serialization_registry` already contains them.
    """

    @_status_boundary
    def __init__(self, *, register_defaults: bool = False):
        self._serializers: list[_SerializerRegistration] = []
        self._deserializers: list[_DeserializerRegistration] = []
        self._known_types: dict[str, list[type]] = {}
        self._next_order = 0
        if register_defaults:
            self.register_defaults()

    @_status_boundary
    def register_serializer(
        self,
        obj_type: type,
        mimetype: str,
        serializer: SerializerFn,
    ) -> None:
        """Register ``serializer(obj)`` for a type and exact media type."""

        parsed = self._registration_key(obj_type, mimetype)
        _validate_serializer(serializer)
        self._ensure_not_registered(self._serializers, obj_type, parsed)
        self._serializers.append(
            _SerializerRegistration(
                obj_type=obj_type,
                mimetype=parsed,
                serializer=serializer,
                order=self._take_order(),
            )
        )
        self._remember_type(obj_type)

    @_status_boundary
    def register_deserializer(
        self,
        obj_type: type,
        mimetype: str,
        deserializer: DeserializerFn,
        *,
        receives_chunk: bool | None = None,
    ) -> None:
        """Register a data deserializer for a type and exact media type.

        A deserializer may accept either ``data`` or ``data, obj_type``.  A
        callback whose first argument is named ``chunk`` (or is annotated as a
        :class:`Chunk`) receives the complete chunk instead of ``chunk.data``.
        ``receives_chunk`` can be used to select that behavior explicitly.
        """

        parsed = self._registration_key(obj_type, mimetype)
        call_mode, receives_chunk = _deserializer_call_info(
            deserializer, receives_chunk
        )
        self._ensure_not_registered(self._deserializers, obj_type, parsed)
        self._deserializers.append(
            _DeserializerRegistration(
                obj_type=obj_type,
                mimetype=parsed,
                deserializer=deserializer,
                call_mode=call_mode,
                receives_chunk=receives_chunk,
                order=self._take_order(),
            )
        )
        self._remember_type(obj_type)

    @_status_boundary
    def register(
        self,
        obj_type: type,
        mimetype: str,
        serializer: SerializerFn,
        deserializer: DeserializerFn,
        *,
        receives_chunk: bool | None = None,
    ) -> None:
        """Atomically register a serializer/deserializer pair."""

        parsed = self._registration_key(obj_type, mimetype)
        _validate_serializer(serializer)
        call_mode, receives_chunk = _deserializer_call_info(
            deserializer, receives_chunk
        )
        self._ensure_not_registered(self._serializers, obj_type, parsed)
        self._ensure_not_registered(self._deserializers, obj_type, parsed)

        serializer_order = self._take_order()
        deserializer_order = self._take_order()
        self._serializers.append(
            _SerializerRegistration(
                obj_type, parsed, serializer, serializer_order
            )
        )
        self._deserializers.append(
            _DeserializerRegistration(
                obj_type,
                parsed,
                deserializer,
                call_mode,
                receives_chunk,
                deserializer_order,
            )
        )
        self._remember_type(obj_type)

    @_status_boundary
    def register_defaults(self) -> None:
        """Install the standard JSON and MessagePack registrations."""

        _register_default_serializers(self)

    @_status_boundary
    def to_chunk(self, obj: Any, mimetype: str = "") -> types.Chunk:
        """Serialize ``obj`` into a chunk.

        If ``mimetype`` is empty, the closest registered Python type wins and
        registration order chooses its preferred representation.  An explicit
        MIME value can be exact or contain ``*`` wildcards.  Returned chunks
        always have an exact MIME type and a stable Python type identifier.
        """

        selection = None
        if mimetype:
            selection = _parse_mimetype(mimetype, allow_patterns=True)
        elif not isinstance(mimetype, str):
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                "Mimetype must be a string.",
            )

        actual_type = type(obj)
        actual_identifier = actual_type.__name__
        candidates = [
            registration
            for registration in self._serializers
            if isinstance(obj, registration.obj_type)
            and (
                selection is None
                or _registration_matches(registration.mimetype, selection)
            )
            and (
                selection is None
                or selection.get_parameter(_TYPE_PARAMETER) is None
                or fnmatch.fnmatchcase(
                    actual_identifier,
                    cast(str, selection.get_parameter(_TYPE_PARAMETER)),
                )
            )
        ]
        candidates.sort(
            key=lambda registration: (
                _inheritance_distance(actual_type, registration.obj_type),
                registration.order,
            )
        )
        if not candidates:
            requested = mimetype or "the object's type"
            _raise_status(
                StatusCode.NOT_FOUND,
                f"No serializer is registered for {actual_type.__name__} and"
                f" {requested!r}.",
            )

        registration = candidates[0]
        serialized = registration.serializer(obj)
        exact_mimetype = _format_exact_mimetype(
            registration.mimetype, actual_type
        )
        self._remember_type(actual_type)

        if isinstance(serialized, types.Chunk):
            chunk = serialized.model_copy(deep=True)
            metadata = chunk.metadata or types.ChunkMetadata(
                mimetype=exact_mimetype
            )
            metadata = metadata.model_copy(update={"mimetype": exact_mimetype})
            return chunk.model_copy(update={"metadata": metadata})

        if isinstance(serialized, str):
            data = serialized.encode("utf-8")
        elif isinstance(serialized, bytes):
            data = serialized
        elif isinstance(serialized, (bytearray, memoryview)):
            data = bytes(serialized)
        else:
            _raise_status(
                StatusCode.INTERNAL,
                "Serializer returned an unsupported value of type"
                f" {type(serialized).__name__}.",
            )

        return types.Chunk(
            metadata=types.ChunkMetadata(mimetype=exact_mimetype),
            data=data,
        )

    @_status_boundary
    def from_chunk(
        self,
        chunk: types.Chunk,
        mimetype_patterns: str | Sequence[str] = "",
        obj_type: type | None = None,
    ) -> Any:
        """Deserialize ``chunk`` using the first matching MIME selector.

        Selectors are matched in order against the chunk's MIME type and may
        contain wildcards.  If the chunk has no MIME metadata, a supplied exact
        selector acts as the representation.  A requested ``obj_type`` uses an
        exact registration first and then registrations for its superclasses.
        """

        if not isinstance(chunk, types.Chunk):
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                "chunk must be an instance of Chunk.",
            )
        if obj_type is not None and not isinstance(obj_type, type):
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                "obj_type must be a type or None.",
            )
        if chunk.ref:
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                "A referenced chunk must be resolved before deserialization.",
            )

        actual = None
        actual_mimetype = chunk.get_mimetype()
        if actual_mimetype:
            actual = _parse_mimetype(actual_mimetype, allow_patterns=False)

        selectors = self._prepare_selectors(mimetype_patterns, actual)
        had_matching_format = False
        had_unresolved_type = False

        for selection in selectors:
            format_registrations = [
                registration
                for registration in self._deserializers
                if _registration_matches(registration.mimetype, selection)
            ]
            if not format_registrations:
                continue
            had_matching_format = True

            encoded_name = selection.get_parameter(_TYPE_PARAMETER)
            encoded_type = (
                self._resolve_type(encoded_name) if encoded_name else None
            )
            target_type = self._choose_target_type(
                obj_type, encoded_name, encoded_type
            )

            if target_type is None:
                distinct_types = {
                    registration.obj_type
                    for registration in format_registrations
                }
                if len(distinct_types) == 1:
                    target_type = next(iter(distinct_types))
                else:
                    had_unresolved_type = True
                    continue

            candidates = [
                registration
                for registration in format_registrations
                if issubclass(target_type, registration.obj_type)
            ]
            candidates.sort(
                key=lambda registration: (
                    _inheritance_distance(target_type, registration.obj_type),
                    registration.order,
                )
            )
            if not candidates:
                continue

            registration = candidates[0]
            result = self._invoke_deserializer(registration, chunk, target_type)
            expected_type = obj_type or target_type
            if expected_type is not None and not isinstance(
                result, expected_type
            ):
                _raise_status(
                    StatusCode.INVALID_ARGUMENT,
                    "Deserializer returned"
                    f" {type(result).__name__}; expected"
                    f" {expected_type.__name__}.",
                )
            return result

        if obj_type is not None and had_matching_format:
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                f"The chunk cannot be deserialized as {obj_type.__name__}.",
            )
        if had_unresolved_type:
            _raise_status(
                StatusCode.NOT_FOUND,
                "The chunk's MIME type does not identify a registered Python"
                " type.",
            )
        requested = (
            mimetype_patterns
            if mimetype_patterns
            else actual_mimetype or "<missing>"
        )
        _raise_status(
            StatusCode.NOT_FOUND,
            f"No deserializer matched {requested!r}.",
        )

    @_status_boundary
    def _registration_key(self, obj_type: type, mimetype: str) -> _Mimetype:
        if not isinstance(obj_type, type):
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                "obj_type must be a type.",
            )
        parsed = _parse_mimetype(mimetype, allow_patterns=False)
        encoded_type = parsed.get_parameter(_TYPE_PARAMETER)
        if encoded_type not in {None, "*", obj_type.__name__}:
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                "A registered MIME type's type parameter must be '*' or the"
                f" class name {obj_type.__name__!r}.",
            )
        return parsed.without_parameter(_TYPE_PARAMETER)

    @_status_boundary
    def _ensure_not_registered(
        self,
        registrations: Sequence[
            _SerializerRegistration | _DeserializerRegistration
        ],
        obj_type: type,
        mimetype: _Mimetype,
    ) -> None:
        if any(
            registration.obj_type is obj_type
            and registration.mimetype == mimetype
            for registration in registrations
        ):
            _raise_status(
                StatusCode.ALREADY_EXISTS,
                f"A handler for {obj_type.__name__} and"
                f" {mimetype.media_type!r} is already registered.",
            )

    @_status_boundary
    def _take_order(self) -> int:
        order = self._next_order
        self._next_order += 1
        return order

    @_status_boundary
    def _remember_type(self, obj_type: type) -> None:
        known = self._known_types.setdefault(obj_type.__name__, [])
        if obj_type not in known:
            known.append(obj_type)

    @_status_boundary
    def _resolve_type(self, name: str) -> type | None:
        known = self._known_types.get(name)
        if known:
            return known[0]

        visited: set[type] = set()
        pending = [
            candidate
            for candidates in self._known_types.values()
            for candidate in candidates
        ]
        while pending:
            candidate = pending.pop(0)
            if candidate in visited:
                continue
            visited.add(candidate)
            if candidate.__name__ == name:
                self._remember_type(candidate)
                return candidate
            try:
                pending.extend(candidate.__subclasses__())
            except TypeError:
                pass
        return None

    @_status_boundary
    def _prepare_selectors(
        self,
        mimetype_patterns: str | Sequence[str],
        actual: _Mimetype | None,
    ) -> list[_Mimetype]:
        if isinstance(mimetype_patterns, str):
            requested = [mimetype_patterns] if mimetype_patterns else []
        elif isinstance(mimetype_patterns, Sequence):
            requested = list(mimetype_patterns)
        else:
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                "mimetype_patterns must be a string or sequence of strings.",
            )

        if not requested:
            if actual is None:
                _raise_status(
                    StatusCode.INVALID_ARGUMENT,
                    "The chunk has no mimetype and no mimetype match was"
                    " supplied.",
                )
            return [cast(_Mimetype, actual)]

        selectors: list[_Mimetype] = []
        for requested_mimetype in requested:
            if not isinstance(requested_mimetype, str):
                _raise_status(
                    StatusCode.INVALID_ARGUMENT,
                    "Every mimetype match must be a string.",
                )
            if not requested_mimetype:
                if actual is not None:
                    selectors.append(actual)
                continue
            pattern = _parse_mimetype(requested_mimetype, allow_patterns=True)
            if actual is not None and _mimetype_patterns(actual, pattern):
                selectors.append(actual)
                continue

            # An explicit selector is authoritative. Preserve a useful type
            # identifier from metadata when only the representation is being
            # overridden (for example, to repair stale metadata).
            if (
                actual is not None
                and pattern.get_parameter(_TYPE_PARAMETER) is None
                and actual.get_parameter(_TYPE_PARAMETER) is not None
            ):
                pattern = _Mimetype(
                    pattern.media_type,
                    pattern.parameters
                    + (
                        (
                            _TYPE_PARAMETER,
                            cast(
                                str,
                                actual.get_parameter(_TYPE_PARAMETER),
                            ),
                        ),
                    ),
                )
            selectors.append(pattern)

        if not selectors:
            if actual is None:
                _raise_status(
                    StatusCode.INVALID_ARGUMENT,
                    "The chunk has no mimetype and no usable mimetype match was"
                    " supplied.",
                )
            selectors.append(actual)
        return selectors

    @_status_boundary
    def _choose_target_type(
        self,
        requested: type | None,
        encoded_name: str | None,
        encoded: type | None,
    ) -> type | None:
        if requested is None:
            return encoded
        if encoded_name is None:
            return requested
        if encoded is None:
            if encoded_name != requested.__name__:
                _raise_status(
                    StatusCode.INVALID_ARGUMENT,
                    f"The chunk contains {encoded_name}, not"
                    f" {requested.__name__}.",
                )
            return requested
        if issubclass(encoded, requested):
            return encoded
        if issubclass(requested, encoded):
            return requested
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            f"The chunk contains {encoded.__name__}, not {requested.__name__}.",
        )

    @_status_boundary
    def _invoke_deserializer(
        self,
        registration: _DeserializerRegistration,
        chunk: types.Chunk,
        target_type: type,
    ) -> Any:
        first_argument: Any = (
            chunk if registration.receives_chunk else chunk.data
        )
        if registration.call_mode == "data_and_type":
            return registration.deserializer(first_argument, target_type)
        if registration.call_mode == "data_and_keyword_type":
            return registration.deserializer(
                first_argument, obj_type=target_type
            )
        return registration.deserializer(first_argument)


_WIRE_TAG = "__a11_serialized_type__"
_WIRE_VALUE = "value"
_MSGPACK_MIN_INT = -(2**63)
_MSGPACK_MAX_INT = 2**64 - 1


def _wire_tag(name: str, value: Any, **metadata: Any) -> dict[str, Any]:
    return {_WIRE_TAG: name, _WIRE_VALUE: value, **metadata}


def _timing_value(value: timing.Time | timing.Duration) -> int | str:
    if isinstance(value, timing.Time):
        if value == timing.infinite_future():
            return "+inf"
        if value == timing.infinite_past():
            return "-inf"
        return value.nanoseconds_since_epoch
    if value == timing.infinite_duration():
        return "+inf"
    if value == -timing.infinite_duration():
        return "-inf"
    return value.nanoseconds_value


def _pydantic_values(model: pydantic.BaseModel) -> dict[str, Any]:
    result = {
        name: getattr(model, name)
        for name in type(model).model_fields
        if hasattr(model, name)
    }
    extra = getattr(model, "__pydantic_extra__", None)
    if extra:
        result.update(extra)
    return result


def _to_wire(value: Any, *, binary: bool, top_level: bool = False) -> Any:
    if value is None or isinstance(value, (bool, str)):
        return value
    if isinstance(value, int):
        if binary and not (_MSGPACK_MIN_INT <= value <= _MSGPACK_MAX_INT):
            return _wire_tag("int", str(value))
        return value
    if isinstance(value, float):
        if math.isfinite(value):
            return value
        marker = (
            "nan" if math.isnan(value) else ("+inf" if value > 0 else "-inf")
        )
        return _wire_tag("float", marker)
    if isinstance(value, bytes):
        encoded: str | bytes = (
            value if binary else base64.b64encode(value).decode("ascii")
        )
        return encoded if top_level else _wire_tag("bytes", encoded)
    if isinstance(value, bytearray):
        encoded = (
            bytes(value) if binary else base64.b64encode(value).decode("ascii")
        )
        return encoded if top_level else _wire_tag("bytearray", encoded)
    if isinstance(value, timing.Time):
        encoded = _timing_value(value)
        if isinstance(encoded, int):
            encoded = _to_wire(encoded, binary=binary)
        return encoded if top_level else _wire_tag("a11.Time", encoded)
    if isinstance(value, timing.Duration):
        encoded = _timing_value(value)
        if isinstance(encoded, int):
            encoded = _to_wire(encoded, binary=binary)
        return encoded if top_level else _wire_tag("a11.Duration", encoded)
    if isinstance(value, datetime.datetime):
        encoded = value.isoformat()
        return encoded if top_level else _wire_tag("datetime", encoded)
    if isinstance(value, datetime.date):
        encoded = value.isoformat()
        return encoded if top_level else _wire_tag("date", encoded)
    if isinstance(value, datetime.time):
        encoded = value.isoformat()
        return encoded if top_level else _wire_tag("time", encoded)
    if isinstance(value, datetime.timedelta):
        encoded = (
            value.days * 86_400_000_000
            + value.seconds * 1_000_000
            + value.microseconds
        )
        encoded = _to_wire(encoded, binary=binary)
        return encoded if top_level else _wire_tag("timedelta", encoded)
    if isinstance(value, uuid.UUID):
        encoded = str(value)
        return encoded if top_level else _wire_tag("uuid", encoded)
    if isinstance(
        value,
        (
            types.ChunkMetadata,
            types.Chunk,
            types.NodeRef,
            types.NodeFragment,
            types.Port,
            types.ActionMessage,
            types.WireMessage,
        ),
    ):
        encoded = _to_wire(value.model_dump(), binary=binary, top_level=True)
        if top_level:
            return encoded
        return _wire_tag("a11.value", encoded, class_name=type(value).__name__)
    if isinstance(value, pydantic.BaseModel):
        encoded = {
            key: _to_wire(item, binary=binary)
            for key, item in _pydantic_values(value).items()
        }
        if top_level and _WIRE_TAG not in encoded:
            return encoded
        return _wire_tag(
            "pydantic",
            encoded,
            class_name=type(value).__name__,
        )
    if isinstance(value, enum.Enum):
        return _to_wire(value.value, binary=binary)
    if isinstance(value, dict):
        if (
            all(isinstance(key, str) for key in value)
            and _WIRE_TAG not in value
        ):
            return {
                key: _to_wire(item, binary=binary)
                for key, item in value.items()
            }
        pairs = [
            [
                _to_wire(key, binary=binary),
                _to_wire(item, binary=binary),
            ]
            for key, item in value.items()
        ]
        return _wire_tag("dict", pairs)
    if isinstance(value, list):
        return [_to_wire(item, binary=binary) for item in value]
    if isinstance(value, tuple):
        values = [_to_wire(item, binary=binary) for item in value]
        return values if top_level else _wire_tag("tuple", values)
    if isinstance(value, set):
        values = [_to_wire(item, binary=binary) for item in value]
        return values if top_level else _wire_tag("set", values)
    if isinstance(value, frozenset):
        values = [_to_wire(item, binary=binary) for item in value]
        return values if top_level else _wire_tag("frozenset", values)

    _raise_status(
        StatusCode.INVALID_ARGUMENT,
        f"Objects of type {type(value).__name__} cannot be serialized by the"
        " default codecs.",
    )


def _decode_base64(value: Any) -> bytes:
    if isinstance(value, bytes):
        return value
    if not isinstance(value, str):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "Serialized bytes must be bytes or a base64 string.",
        )
    try:
        return base64.b64decode(value, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Serialized bytes contain invalid base64 data.",
        ).to_exception() from exc


def _parse_datetime(value: Any, cls: type) -> Any:
    if not isinstance(value, str):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            f"Serialized {cls.__name__} must be a string.",
        )
    try:
        return cls.fromisoformat(value)
    except ValueError as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid serialized {cls.__name__}: {value!r}.",
        ).to_exception() from exc


def _make_time(value: Any) -> timing.Time:
    if value == "+inf":
        return timing.infinite_future()
    if value == "-inf":
        return timing.infinite_past()
    if type(value) is not int:
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "Serialized Time must be nanoseconds or an infinity marker.",
        )
    return timing.Time.from_nanoseconds_since_epoch(value)


def _make_duration(value: Any) -> timing.Duration:
    if value == "+inf":
        return timing.infinite_duration()
    if value == "-inf":
        return -timing.infinite_duration()
    if type(value) is not int:
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "Serialized Duration must be nanoseconds or an infinity marker.",
        )
    return timing.Duration(value)


def _from_wire(value: Any, resolver: Callable[[str], type | None]) -> Any:
    if isinstance(value, list):
        return [_from_wire(item, resolver) for item in value]
    if not isinstance(value, dict):
        return value

    tag = value.get(_WIRE_TAG)
    if not isinstance(tag, str):
        return {key: _from_wire(item, resolver) for key, item in value.items()}
    encoded = value.get(_WIRE_VALUE)
    if tag == "int":
        try:
            return int(encoded)
        except (TypeError, ValueError) as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Invalid tagged integer.",
            ).to_exception() from exc
    if tag == "float":
        values = {"nan": math.nan, "+inf": math.inf, "-inf": -math.inf}
        if encoded not in values:
            _raise_status(StatusCode.INVALID_ARGUMENT, "Invalid tagged float.")
        return values[encoded]
    if tag == "bytes":
        return _decode_base64(encoded)
    if tag == "bytearray":
        return bytearray(_decode_base64(encoded))
    if tag == "datetime":
        return _parse_datetime(encoded, datetime.datetime)
    if tag == "date":
        return _parse_datetime(encoded, datetime.date)
    if tag == "time":
        return _parse_datetime(encoded, datetime.time)
    if tag == "timedelta":
        encoded = _from_wire(encoded, resolver)
        if type(encoded) is not int:
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                "Invalid tagged timedelta.",
            )
        return datetime.timedelta(microseconds=encoded)
    if tag == "uuid":
        try:
            return uuid.UUID(encoded)
        except (AttributeError, TypeError, ValueError) as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Invalid tagged UUID.",
            ).to_exception() from exc
    if tag == "a11.Time":
        return _make_time(_from_wire(encoded, resolver))
    if tag == "a11.Duration":
        return _make_duration(_from_wire(encoded, resolver))
    if tag in {"tuple", "set", "frozenset"}:
        if not isinstance(encoded, list):
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                f"Invalid tagged {tag}.",
            )
        items = [_from_wire(item, resolver) for item in encoded]
        try:
            return {"tuple": tuple, "set": set, "frozenset": frozenset}[tag](
                items
            )
        except TypeError as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid tagged {tag}: {exc}.",
            ).to_exception() from exc
    if tag == "dict":
        if not isinstance(encoded, list):
            _raise_status(StatusCode.INVALID_ARGUMENT, "Invalid tagged dict.")
        result = {}
        try:
            for pair in encoded:
                if not isinstance(pair, list) or len(pair) != 2:
                    _raise_status(
                        StatusCode.INVALID_ARGUMENT,
                        "Invalid key/value pair in tagged dict.",
                    )
                key, item = pair
                result[_from_wire(key, resolver)] = _from_wire(item, resolver)
        except TypeError as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid tagged dict key: {exc}.",
            ).to_exception() from exc
        return result
    if tag == "pydantic":
        class_name = value.get("class_name")
        model_type = (
            resolver(class_name) if isinstance(class_name, str) else None
        )
        if model_type is None or not issubclass(model_type, pydantic.BaseModel):
            _raise_status(
                StatusCode.NOT_FOUND,
                f"Pydantic model {class_name!r} is not registered or loaded.",
            )
        decoded = _from_wire(encoded, resolver)
        return _validate_pydantic_model(model_type, decoded)
    if tag == "a11.value":
        class_name = value.get("class_name")
        model_type = (
            resolver(class_name) if isinstance(class_name, str) else None
        )
        if model_type not in _NATIVE_DATA_TYPES:
            _raise_status(
                StatusCode.NOT_FOUND,
                f"A11 value type {class_name!r} is not registered or loaded.",
            )
        return model_type.model_validate(_from_wire(encoded, resolver))

    # A normal user mapping can contain the reserved key.  Unknown tags are
    # therefore treated as ordinary data rather than rejected.
    return {key: _from_wire(item, resolver) for key, item in value.items()}


def _validate_pydantic_model(
    model_type: type[pydantic.BaseModel], value: Any
) -> pydantic.BaseModel:
    validation_value = value
    if (
        getattr(model_type, "__pydantic_root_model__", False)
        and isinstance(value, dict)
        and set(value) == {"root"}
    ):
        validation_value = value["root"]
    result = model_type.model_validate(
        validation_value, by_alias=True, by_name=True
    )
    if not isinstance(result, model_type):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            f"Validation did not produce a {model_type.__name__} instance.",
        )
    return result


def _coerce_target(
    value: Any,
    target: type,
    resolver: Callable[[str], type | None],
) -> Any:
    value = _from_wire(value, resolver)

    if target in _NATIVE_DATA_TYPES:
        if isinstance(value, target):
            return value
        return target.model_validate(value)
    if issubclass(target, pydantic.BaseModel):
        if isinstance(value, target):
            return value
        return _validate_pydantic_model(target, value)
    if target is dict:
        if not isinstance(value, dict):
            _raise_status(StatusCode.INVALID_ARGUMENT, "Expected a dict.")
        return value
    if target is list:
        if not isinstance(value, list):
            _raise_status(StatusCode.INVALID_ARGUMENT, "Expected a list.")
        return value
    if target in {tuple, set, frozenset}:
        if not isinstance(value, (list, tuple, set, frozenset)):
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                f"Expected data for {target.__name__}.",
            )
        try:
            return target(value)
        except TypeError as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid {target.__name__}: {exc}.",
            ).to_exception() from exc
    if target is bool:
        if type(value) is not bool:
            _raise_status(StatusCode.INVALID_ARGUMENT, "Expected a bool.")
        return value
    if target is int:
        if type(value) is not int:
            _raise_status(StatusCode.INVALID_ARGUMENT, "Expected an int.")
        return value
    if target is float:
        if type(value) not in {int, float}:
            _raise_status(StatusCode.INVALID_ARGUMENT, "Expected a float.")
        return float(value)
    if target is str:
        if not isinstance(value, str):
            _raise_status(StatusCode.INVALID_ARGUMENT, "Expected a str.")
        return value
    if target is bytes:
        return _decode_base64(value)
    if target is bytearray:
        return bytearray(_decode_base64(value))
    if target is type(None):
        if value is not None:
            _raise_status(StatusCode.INVALID_ARGUMENT, "Expected null.")
        return None
    if target is datetime.datetime:
        return (
            value
            if isinstance(value, datetime.datetime)
            else _parse_datetime(value, datetime.datetime)
        )
    if target is datetime.date:
        return (
            value
            if type(value) is datetime.date
            else _parse_datetime(value, datetime.date)
        )
    if target is datetime.time:
        return (
            value
            if isinstance(value, datetime.time)
            else _parse_datetime(value, datetime.time)
        )
    if target is datetime.timedelta:
        if isinstance(value, datetime.timedelta):
            return value
        if type(value) is not int:
            _raise_status(
                StatusCode.INVALID_ARGUMENT,
                "Serialized timedelta must contain integer microseconds.",
            )
        return datetime.timedelta(microseconds=value)
    if target is uuid.UUID:
        if isinstance(value, uuid.UUID):
            return value
        try:
            return uuid.UUID(value)
        except (AttributeError, TypeError, ValueError) as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Invalid serialized UUID.",
            ).to_exception() from exc
    if target is timing.Time:
        return value if isinstance(value, timing.Time) else _make_time(value)
    if target is timing.Duration:
        return (
            value
            if isinstance(value, timing.Duration)
            else _make_duration(value)
        )
    return value


def _serialize_json(obj: Any) -> bytes:
    try:
        return json.dumps(
            _to_wire(obj, binary=False, top_level=True),
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
        ).encode("utf-8")
    except StatusException:
        raise
    except (TypeError, ValueError, OverflowError) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Failed to serialize JSON: {exc}",
        ).to_exception() from exc


def _deserialize_json(
    data: str | bytes,
    obj_type: type,
    resolver: Callable[[str], type | None],
) -> Any:
    try:
        decoded = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid JSON data: {exc}",
        ).to_exception() from exc
    return _coerce_target(decoded, obj_type, resolver)


def _serialize_msgpack(obj: Any) -> bytes:
    try:
        return msgpack.packb(
            _to_wire(obj, binary=True, top_level=True),
            use_bin_type=True,
        )
    except StatusException:
        raise
    except (TypeError, ValueError, OverflowError, msgpack.PackException) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Failed to serialize MessagePack: {exc}",
        ).to_exception() from exc


def _deserialize_msgpack(
    data: bytes,
    obj_type: type,
    resolver: Callable[[str], type | None],
) -> Any:
    try:
        decoded = msgpack.unpackb(data, raw=False, strict_map_key=False)
    except (TypeError, ValueError, msgpack.UnpackException) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid MessagePack data: {exc}",
        ).to_exception() from exc
    return _coerce_target(decoded, obj_type, resolver)


_NATIVE_DATA_TYPES: tuple[type, ...] = (
    types.ChunkMetadata,
    types.Chunk,
    types.NodeRef,
    types.NodeFragment,
    types.Port,
    types.ActionMessage,
    types.WireMessage,
)


DEFAULT_SERIALIZABLE_TYPES: tuple[type, ...] = (
    dict,
    list,
    tuple,
    set,
    frozenset,
    int,
    float,
    str,
    bytes,
    bytearray,
    bool,
    type(None),
    datetime.datetime,
    datetime.date,
    datetime.time,
    datetime.timedelta,
    uuid.UUID,
    pydantic.BaseModel,
    timing.Time,
    timing.Duration,
    *_NATIVE_DATA_TYPES,
)


def _register_default_serializers(registry: SerializationRegistry) -> None:
    def deserialize_json(data: str | bytes, obj_type: type) -> Any:
        return _deserialize_json(data, obj_type, registry._resolve_type)

    def deserialize_msgpack(data: bytes, obj_type: type) -> Any:
        return _deserialize_msgpack(data, obj_type, registry._resolve_type)

    for obj_type in DEFAULT_SERIALIZABLE_TYPES:
        registry.register(
            obj_type,
            JSON_MIMETYPE,
            _serialize_json,
            deserialize_json,
        )
        registry.register(
            obj_type,
            MSGPACK_MIMETYPE,
            _serialize_msgpack,
            deserialize_msgpack,
        )


def register_default_serializers(registry: SerializationRegistry) -> None:
    """Register all built-in codecs with an otherwise empty registry."""

    if not isinstance(registry, SerializationRegistry):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "registry must be a SerializationRegistry.",
        )
    registry.register_defaults()


_global_serialization_registry = SerializationRegistry(register_defaults=True)


def get_global_serialization_registry() -> SerializationRegistry:
    return _global_serialization_registry


def set_global_serialization_registry(registry: SerializationRegistry) -> None:
    if not isinstance(registry, SerializationRegistry):
        _raise_status(
            StatusCode.INVALID_ARGUMENT,
            "registry must be a SerializationRegistry.",
        )
    global _global_serialization_registry
    _global_serialization_registry = registry

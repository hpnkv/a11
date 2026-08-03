"""Serialization of Python objects to and from [Chunk][a11.data.types.Chunk].

The registry deliberately separates a media type (the representation) from a
value type. A serialized chunk combines the two by adding a stable ``type``
parameter to its MIME type, for example ``application/json;type=object``.
JSON-native types use language-neutral names; application-specific types use
stable Python-qualified names unless explicitly configured otherwise.
"""

import asyncio
import base64
import binascii
import datetime
import enum
import fnmatch
import functools
import importlib
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
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Mimetype must be a non-empty string.",
        ).to_exception()

    parts = value.split(";")
    media_type = parts[0].strip().lower()
    if media_type.count("/") != 1:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid mimetype: {value!r}.",
        ).to_exception()

    major, minor = media_type.split("/", 1)
    part_re = _MIME_PART_RE if allow_patterns else _MIME_TOKEN_RE
    if not major or not minor or not part_re.fullmatch(major):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid mimetype: {value!r}.",
        ).to_exception()
    if not part_re.fullmatch(minor):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid mimetype: {value!r}.",
        ).to_exception()
    if not allow_patterns and any(char in media_type for char in "*?["):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Registered mimetypes must identify an exact media type.",
        ).to_exception()

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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid mimetype parameter in {value!r}.",
            ).to_exception()
        if not allow_patterns and any(
            char in parameter_value for char in "*?["
        ):
            if name != _TYPE_PARAMETER or parameter_value != "*":
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "Registered mimetype parameters cannot be patterns."
                    ),
                ).to_exception()
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


def _qualified_name(obj_type: type) -> str:
    """A stable, collision-resistant identifier for a type.

    Uses the fully qualified ``module.qualname`` so identically named classes
    from different modules (for example, two SDKs that each define a
    ``TextDelta``) do not clash. Builtins, ``__main__`` and locally defined
    classes — whose qualified names cannot be resolved by import anyway — fall
    back to the bare class name.
    """
    qualname = getattr(obj_type, "__qualname__", None) or obj_type.__name__
    module = getattr(obj_type, "__module__", "") or ""
    if "<locals>" in qualname or module in ("", "builtins", "__main__"):
        return obj_type.__name__
    return f"{module}.{qualname}"


def _import_qualified(name: str) -> type | None:
    """Best-effort resolution of a ``module.qualname`` identifier by import."""
    if "." not in name:
        return None
    module_path, _, attribute = name.rpartition(".")
    while module_path:
        try:
            module = importlib.import_module(module_path)
        except Exception:
            module_path, _, head = module_path.rpartition(".")
            attribute = f"{head}.{attribute}" if head else attribute
            continue
        obj: Any = module
        try:
            for part in attribute.split("."):
                obj = getattr(obj, part)
        except AttributeError:
            return None
        return obj if isinstance(obj, type) else None
    return None


def _format_exact_mimetype(mimetype: _Mimetype, type_identifier: str) -> str:
    parameters = list(mimetype.without_parameter(_TYPE_PARAMETER).parameters)
    encoded_identifier = urllib.parse.quote(
        type_identifier, safe="!#$%&'*+.^_`|~-"
    )
    parameters.append((_TYPE_PARAMETER, encoded_identifier))
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
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Serializer must be callable.",
        ).to_exception()
    signature = _callable_signature(serializer)
    if signature is None:
        return
    try:
        signature.bind(object())
    except TypeError:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Serializer must accept one object argument.",
        ).to_exception()


def _deserializer_call_info(
    deserializer: DeserializerFn,
    receives_chunk: bool | None,
) -> tuple[str, bool]:
    if not callable(deserializer):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Deserializer must be callable.",
        ).to_exception()

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
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "Deserializer must accept data and, optionally, an"
                        " object type."
                    ),
                ).to_exception()

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
    `register_defaults` to install the built-in JSON and MessagePack
    codecs.  The process-wide registry returned by
    `get_global_serialization_registry` already contains them.
    """

    @_status_boundary
    def __init__(self, *, register_defaults: bool = False):
        self._serializers: list[_SerializerRegistration] = []
        self._deserializers: list[_DeserializerRegistration] = []
        self._known_types: dict[str, list[type]] = {}
        # Explicit, user-set type tags and their reverse index, plus a
        # qualified-name index populated as types are seen.
        self._type_tags: dict[type, str] = {}
        self._tag_to_type: dict[str, type] = {}
        self._known_by_tag: dict[str, type] = {}
        self._next_order = 0
        if register_defaults:
            self.register_defaults()

    @_status_boundary
    def set_type_tag(self, obj_type: type, tag: str) -> None:
        """Pin the wire tag used to identify ``obj_type`` in serialized data.

        Overrides the default fully-qualified name. Use this to keep a short,
        stable tag for a type (for example to preserve an existing wire format)
        or to give two like-named types deterministic, distinct identifiers.
        """
        if not isinstance(obj_type, type):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="obj_type must be a type.",
            ).to_exception()
        if not isinstance(tag, str) or not tag:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="tag must be a non-empty string.",
            ).to_exception()
        self._set_type_tag(obj_type, tag, allow_shared=False)

    @_status_boundary
    def _set_type_tag(
        self, obj_type: type, tag: str, *, allow_shared: bool
    ) -> None:
        """Set a tag, optionally sharing a language-neutral JSON tag."""
        if not isinstance(obj_type, type):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="obj_type must be a type.",
            ).to_exception()
        if not isinstance(tag, str) or not tag:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="tag must be a non-empty string.",
            ).to_exception()
        existing = self._tag_to_type.get(tag)
        if existing is not None and existing is not obj_type:
            if not allow_shared:
                raise Status(
                    code=StatusCode.ALREADY_EXISTS,
                    message=(
                        f"Tag {tag!r} is already assigned to"
                        f" {existing.__name__}."
                    ),
                ).to_exception()
        previous = self._type_tags.get(obj_type)
        if previous is not None and previous != tag:
            if self._tag_to_type.get(previous) is obj_type:
                self._tag_to_type.pop(previous, None)
        self._type_tags[obj_type] = tag
        self._tag_to_type.setdefault(tag, obj_type)
        self._remember_type(obj_type)

    @_status_boundary
    def _type_tag(self, obj_type: type) -> str:
        """The wire tag for ``obj_type``: explicit if set, else qualified."""
        explicit = self._type_tags.get(obj_type)
        if explicit is not None:
            return explicit
        return _qualified_name(obj_type)

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
        `Chunk`) receives the complete chunk instead of ``chunk.data``.
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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Mimetype must be a string.",
            ).to_exception()

        actual_type = type(obj)
        actual_identifiers = {actual_type.__name__, self._type_tag(actual_type)}
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
                or any(
                    fnmatch.fnmatchcase(
                        identifier,
                        cast(str, selection.get_parameter(_TYPE_PARAMETER)),
                    )
                    for identifier in actual_identifiers
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
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=(
                    f"No serializer is registered for {actual_type.__name__}"
                    f" and {requested!r}."
                ),
            ).to_exception()

        registration = candidates[0]
        serialized = registration.serializer(obj)
        exact_mimetype = _format_exact_mimetype(
            registration.mimetype, self._type_tag(actual_type)
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
            raise Status(
                code=StatusCode.INTERNAL,
                message=(
                    "Serializer returned an unsupported value of type"
                    f" {type(serialized).__name__}."
                ),
            ).to_exception()

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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="chunk must be an instance of Chunk.",
            ).to_exception()
        if obj_type is not None and not isinstance(obj_type, type):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="obj_type must be a type or None.",
            ).to_exception()
        if chunk.ref:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "A referenced chunk must be resolved before"
                    " deserialization."
                ),
            ).to_exception()

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
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "Deserializer returned"
                        f" {type(result).__name__}; expected"
                        f" {expected_type.__name__}."
                    ),
                ).to_exception()
            return result

        if obj_type is not None and had_matching_format:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    f"The chunk cannot be deserialized as {obj_type.__name__}."
                ),
            ).to_exception()
        if had_unresolved_type:
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=(
                    "The chunk's MIME type does not identify a registered"
                    " Python type."
                ),
            ).to_exception()
        requested = (
            mimetype_patterns
            if mimetype_patterns
            else actual_mimetype or "<missing>"
        )
        raise Status(
            code=StatusCode.NOT_FOUND,
            message=f"No deserializer matched {requested!r}.",
        ).to_exception()

    @_status_boundary
    def _registration_key(self, obj_type: type, mimetype: str) -> _Mimetype:
        if not isinstance(obj_type, type):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="obj_type must be a type.",
            ).to_exception()
        parsed = _parse_mimetype(mimetype, allow_patterns=False)
        encoded_type = parsed.get_parameter(_TYPE_PARAMETER)
        if encoded_type not in {None, "*", obj_type.__name__}:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "A registered MIME type's type parameter must be '*' or the"
                    f" class name {obj_type.__name__!r}."
                ),
            ).to_exception()
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
            raise Status(
                code=StatusCode.ALREADY_EXISTS,
                message=(
                    f"A handler for {obj_type.__name__} and"
                    f" {mimetype.media_type!r} is already registered."
                ),
            ).to_exception()

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
        self._known_by_tag.setdefault(_qualified_name(obj_type), obj_type)

    @_status_boundary
    def _resolve_type(self, name: str) -> type | None:
        # 1. An explicitly-pinned tag always wins.
        explicit = self._tag_to_type.get(name)
        if explicit is not None:
            return explicit

        # 2. A previously-seen fully-qualified name.
        by_tag = self._known_by_tag.get(name)
        if by_tag is not None:
            return by_tag

        # 3. Legacy bare class name (ambiguous; first-seen wins, as before).
        known = self._known_types.get(name)
        if known:
            return known[0]

        # 4. Scan loaded subclasses, matching the qualified name first (exact,
        #    collision-free) and the bare name second (back-compatible).
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
            if _qualified_name(candidate) == name or candidate.__name__ == name:
                self._remember_type(candidate)
                return candidate
            try:
                pending.extend(candidate.__subclasses__())
            except TypeError:
                pass

        # 5. Last resort: import a dotted qualified name directly.
        resolved = _import_qualified(name)
        if resolved is not None:
            self._remember_type(resolved)
        return resolved

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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "mimetype_patterns must be a string or sequence of strings."
                ),
            ).to_exception()

        if not requested:
            if actual is None:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "The chunk has no mimetype and no mimetype match was"
                        " supplied."
                    ),
                ).to_exception()
            return [cast(_Mimetype, actual)]

        selectors: list[_Mimetype] = []
        for requested_mimetype in requested:
            if not isinstance(requested_mimetype, str):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Every mimetype match must be a string.",
                ).to_exception()
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
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "The chunk has no mimetype and no usable mimetype match"
                        " was supplied."
                    ),
                ).to_exception()
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
        # Language-neutral tags can intentionally be shared.  For example,
        # both list and tuple use the JSON wire type "array"; an explicit
        # requested type disambiguates them without leaking Python names.
        if encoded_name == self._type_tag(requested):
            return requested
        if encoded is None:
            accepted = {
                requested.__name__,
                self._type_tag(requested),
                _qualified_name(requested),
            }
            if encoded_name not in accepted:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        f"The chunk contains {encoded_name}, not"
                        f" {requested.__name__}."
                    ),
                ).to_exception()
            return requested
        if issubclass(encoded, requested):
            return encoded
        if issubclass(requested, encoded):
            return requested
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"The chunk contains {encoded.__name__}, not"
                f" {requested.__name__}."
            ),
        ).to_exception()

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


def _to_wire(
    value: Any,
    *,
    binary: bool,
    top_level: bool = False,
    tag_for: Callable[[type], str] | None = None,
) -> Any:
    resolve_tag = tag_for or _qualified_name

    def rec(item: Any, *, top: bool = False) -> Any:
        return _to_wire(item, binary=binary, top_level=top, tag_for=resolve_tag)

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
            encoded = rec(encoded)
        return encoded if top_level else _wire_tag("a11.Time", encoded)
    if isinstance(value, timing.Duration):
        encoded = _timing_value(value)
        if isinstance(encoded, int):
            encoded = rec(encoded)
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
        encoded = rec(encoded)
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
            Status,
        ),
    ):
        encoded = rec(value.model_dump(), top=True)
        if top_level:
            return encoded
        return _wire_tag(
            "a11.value", encoded, class_name=resolve_tag(type(value))
        )
    if isinstance(value, pydantic.BaseModel):
        encoded = {
            key: rec(item) for key, item in _pydantic_values(value).items()
        }
        if top_level and _WIRE_TAG not in encoded:
            return encoded
        return _wire_tag(
            "pydantic",
            encoded,
            class_name=resolve_tag(type(value)),
        )
    if isinstance(value, enum.Enum):
        return rec(value.value)
    if isinstance(value, dict):
        if (
            all(isinstance(key, str) for key in value)
            and _WIRE_TAG not in value
        ):
            return {key: rec(item) for key, item in value.items()}
        pairs = [[rec(key), rec(item)] for key, item in value.items()]
        return _wire_tag("dict", pairs)
    if isinstance(value, list):
        return [rec(item) for item in value]
    if isinstance(value, tuple):
        values = [rec(item) for item in value]
        return values if top_level else _wire_tag("tuple", values)
    if isinstance(value, set):
        values = [rec(item) for item in value]
        return values if top_level else _wire_tag("set", values)
    if isinstance(value, frozenset):
        values = [rec(item) for item in value]
        return values if top_level else _wire_tag("frozenset", values)

    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=(
            f"Objects of type {type(value).__name__} cannot be serialized by"
            " the default codecs."
        ),
    ).to_exception()


def _decode_base64(value: Any) -> bytes:
    if isinstance(value, bytes):
        return value
    if not isinstance(value, str):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Serialized bytes must be bytes or a base64 string.",
        ).to_exception()
    try:
        return base64.b64decode(value, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Serialized bytes contain invalid base64 data.",
        ).to_exception() from exc


def _parse_datetime(value: Any, cls: type) -> Any:
    if not isinstance(value, str):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Serialized {cls.__name__} must be a string.",
        ).to_exception()
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
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "Serialized Time must be nanoseconds or an infinity marker."
            ),
        ).to_exception()
    return timing.Time.from_nanoseconds_since_epoch(value)


def _make_duration(value: Any) -> timing.Duration:
    if value == "+inf":
        return timing.infinite_duration()
    if value == "-inf":
        return -timing.infinite_duration()
    if type(value) is not int:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "Serialized Duration must be nanoseconds or an infinity marker."
            ),
        ).to_exception()
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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Invalid tagged float.",
            ).to_exception()
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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Invalid tagged timedelta.",
            ).to_exception()
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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid tagged {tag}.",
            ).to_exception()
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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT, message="Invalid tagged dict."
            ).to_exception()
        result = {}
        try:
            for pair in encoded:
                if not isinstance(pair, list) or len(pair) != 2:
                    raise Status(
                        code=StatusCode.INVALID_ARGUMENT,
                        message="Invalid key/value pair in tagged dict.",
                    ).to_exception()
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
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=(
                    f"Pydantic model {class_name!r} is not registered or"
                    " loaded."
                ),
            ).to_exception()
        decoded = _from_wire(encoded, resolver)
        return _validate_pydantic_model(model_type, decoded)
    if tag == "a11.value":
        class_name = value.get("class_name")
        model_type = (
            resolver(class_name) if isinstance(class_name, str) else None
        )
        if model_type not in _NATIVE_DATA_TYPES:
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=(
                    f"A11 value type {class_name!r} is not registered or"
                    " loaded."
                ),
            ).to_exception()
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
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"Validation did not produce a {model_type.__name__} instance."
            ),
        ).to_exception()
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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT, message="Expected a dict."
            ).to_exception()
        return value
    if target is list:
        if not isinstance(value, list):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT, message="Expected a list."
            ).to_exception()
        return value
    if target in {tuple, set, frozenset}:
        if not isinstance(value, (list, tuple, set, frozenset)):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Expected data for {target.__name__}.",
            ).to_exception()
        try:
            return target(value)
        except TypeError as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid {target.__name__}: {exc}.",
            ).to_exception() from exc
    if target is bool:
        if type(value) is not bool:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT, message="Expected a bool."
            ).to_exception()
        return value
    if target is int:
        if type(value) is not int:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT, message="Expected an int."
            ).to_exception()
        return value
    if target is float:
        if type(value) not in {int, float}:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT, message="Expected a float."
            ).to_exception()
        return float(value)
    if target is str:
        if not isinstance(value, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT, message="Expected a str."
            ).to_exception()
        return value
    if target is bytes:
        return _decode_base64(value)
    if target is bytearray:
        return bytearray(_decode_base64(value))
    if target is type(None):
        if value is not None:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT, message="Expected null."
            ).to_exception()
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
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Serialized timedelta must contain integer microseconds."
                ),
            ).to_exception()
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


def _serialize_json(
    obj: Any, tag_for: Callable[[type], str] | None = None
) -> bytes:
    try:
        return json.dumps(
            _to_wire(obj, binary=False, top_level=True, tag_for=tag_for),
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


def _serialize_msgpack(
    obj: Any, tag_for: Callable[[type], str] | None = None
) -> bytes:
    try:
        return msgpack.packb(
            _to_wire(obj, binary=True, top_level=True, tag_for=tag_for),
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
    Status,
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
    def serialize_json(obj: Any) -> bytes:
        return _serialize_json(obj, registry._type_tag)

    def serialize_msgpack(obj: Any) -> bytes:
        return _serialize_msgpack(obj, registry._type_tag)

    def deserialize_json(data: str | bytes, obj_type: type) -> Any:
        return _deserialize_json(data, obj_type, registry._resolve_type)

    def deserialize_msgpack(data: bytes, obj_type: type) -> Any:
        return _deserialize_msgpack(data, obj_type, registry._resolve_type)

    for obj_type in DEFAULT_SERIALIZABLE_TYPES:
        registry.register(
            obj_type,
            JSON_MIMETYPE,
            serialize_json,
            deserialize_json,
        )
        registry.register(
            obj_type,
            MSGPACK_MIMETYPE,
            serialize_msgpack,
            deserialize_msgpack,
        )

    # JSON-native values use language-neutral tags.  Legacy bare Python tags
    # remain accepted through _resolve_type's class-name compatibility path.
    canonical_json_tags = {
        dict: "object",
        list: "array",
        tuple: "array",
        int: "integer",
        float: "number",
        str: "string",
        bool: "boolean",
        type(None): "null",
    }

    # The framework's other well-known types retain their historical bare-name
    # tags; user-defined types use qualified, collision-free tags by default.
    for obj_type in DEFAULT_SERIALIZABLE_TYPES:
        if obj_type is pydantic.BaseModel:
            continue
        registry._set_type_tag(
            obj_type,
            canonical_json_tags.get(obj_type, obj_type.__name__),
            allow_shared=obj_type in (list, tuple),
        )


def register_default_serializers(registry: SerializationRegistry) -> None:
    """Register all built-in codecs with an otherwise empty registry."""

    if not isinstance(registry, SerializationRegistry):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="registry must be a SerializationRegistry.",
        ).to_exception()
    registry.register_defaults()


_global_serialization_registry = SerializationRegistry(register_defaults=True)


def get_global_serialization_registry() -> SerializationRegistry:
    return _global_serialization_registry


def set_global_type_tag(obj_type: type, tag: str) -> None:
    """Pin the wire tag for ``obj_type`` on the process-wide registry."""
    _global_serialization_registry.set_type_tag(obj_type, tag)


def set_global_serialization_registry(registry: SerializationRegistry) -> None:
    if not isinstance(registry, SerializationRegistry):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="registry must be a SerializationRegistry.",
        ).to_exception()
    global _global_serialization_registry
    _global_serialization_registry = registry

"""Serialization of Python objects to and from [Chunk][a11.data.types.Chunk].

A chunk's metadata is the only thing that says how to read its bytes. The media
type gives the representation (``application/json``, ``application/x-msgpack``)
and, when the value is not one JSON already describes, a ``type`` parameter
names it: ``application/json;type=a11.sdk.Interaction``. Nothing inside the
payload repeats that: a serialized value is ordinary JSON or MessagePack.

So a bare ``application/json`` is a complete description: it decodes to a
`dict`, `list` or scalar. Ask for a particular ``obj_type`` and the registry
makes a best effort to produce it, reporting a real failure when the data will
not fit.
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
from typing import Any, Callable, Sequence, TypeVar, cast, overload

import msgpack
import pydantic

from a11 import _native, timing
from a11.data import serial_tags, types
from a11.status import Status, StatusCode, StatusException

SerializedData = str | bytes | bytearray | memoryview | types.Chunk
SerializerFn = Callable[[Any], SerializedData]
DeserializerFn = Callable[..., Any]

JSON_MIMETYPE = _native.JSON_MIMETYPE
MSGPACK_MIMETYPE = _native.MSGPACK_MIMETYPE

_TYPE_PARAMETER = "type"

#: Tags a JSON or MessagePack payload already spells out for itself. A chunk
#: holding one of these carries no ``type`` parameter at all: ``;type=object``
#: on an object says nothing a parser did not already know, and a peer matching
#: on bare ``application/json`` would fail to recognise it.
_GENERIC_TAGS = frozenset(
    {"object", "array", "string", "integer", "number", "boolean", "null"}
)

_MIME_TOKEN_RE = re.compile(r"^[!#$%&'*+.^_`|~0-9A-Za-z-]+$")
_MIME_PART_RE = re.compile(r"^[!#$%&'*+.^_`|~0-9A-Za-z?*\[\]-]+$")
_F = TypeVar("_F", bound=Callable[..., Any])
_T = TypeVar("_T")


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


def _declared_serial_tag(obj_type: type) -> str | None:
    """The cross-language tag a class declares for itself, if any.

    A class opts in with an ``A11_SERIAL_TAG`` ClassVar naming its entry in
    `a11.data.serial_tags` — the Python counterpart of the C++ ``A11SerialTag``
    ADL customization point. Read from the class's own ``__dict__`` rather than
    with ``getattr``: a subclass must not inherit its base's identity and
    silently serialize as the wrong type.
    """
    tag = obj_type.__dict__.get(serial_tags.SERIAL_TAG_ATTRIBUTE)
    return tag if isinstance(tag, str) and tag else None


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
    """The mimetype a serialized chunk carries.

    The ``type`` parameter is added only when the tag says something the format
    does not: an object, array, string or number is left bare.
    """
    parameters = list(mimetype.without_parameter(_TYPE_PARAMETER).parameters)
    if type_identifier not in _GENERIC_TAGS:
        parameters.append(
            (
                _TYPE_PARAMETER,
                urllib.parse.quote(type_identifier, safe="!#$%&'*+.^_`|~-"),
            )
        )
    suffix = "".join(
        f";{name}={_format_parameter(value)}" for name, value in parameters
    )
    return f"{mimetype.media_type}{suffix}"


def _mimetype_matches(actual: _Mimetype, pattern: _Mimetype) -> bool:
    """Whether a chunk's own mimetype is one the caller asked for.

    The ``type`` parameter is not part of the comparison: a selector chooses a
    *representation*, and which type comes back is settled separately by the
    tag and the caller's ``obj_type``.
    """
    if not fnmatch.fnmatchcase(actual.media_type, pattern.media_type):
        return False
    actual_parameters = dict(actual.parameters)
    for name, expected in pattern.parameters:
        if name == _TYPE_PARAMETER:
            continue
        actual_value = actual_parameters.get(name)
        if actual_value is None or not fnmatch.fnmatchcase(
            actual_value, expected
        ):
            return False
    return True


def _registration_matches(registered: _Mimetype, selection: _Mimetype) -> bool:
    """Whether a registration offers the representation a selector asks for.

    A registration never carries a ``type`` parameter -- `_registration_key`
    strips it -- so only the media type and any other parameters are compared.
    """
    if not fnmatch.fnmatchcase(registered.media_type, selection.media_type):
        return False

    selected_parameters = dict(selection.parameters)
    for name, value in registered.parameters:
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
    #: Whether this is one of the registry's own codecs. A built-in is known to
    #: be quick and is run on the event loop; anything a caller registered may
    #: block for as long as it likes and is handed to a worker thread.
    builtin: bool = False


@dataclass(frozen=True, slots=True)
class _DeserializerRegistration:
    obj_type: type
    mimetype: _Mimetype
    deserializer: DeserializerFn
    call_mode: str
    receives_chunk: bool
    order: int
    builtin: bool = False


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
        # Resolution is pure over (type, mimetype) between mutations, and it
        # dominates the cost of `to_chunk`: an `isinstance` against every
        # registration, a sort, then parsing and reformatting the mimetype --
        # none of which depends on the payload, and all of it far dearer than
        # the MessagePack encode it wraps. Anything that changes what
        # resolution would answer clears this.
        self._resolution_cache: dict[
            tuple[type, str], tuple[_SerializerRegistration, str]
        ] = {}
        # The same idea for reading. `from_chunk` asked
        # `_registration_matches` -- an `fnmatch` per call -- once for every
        # registration, which profiled at 56 regex matches and 60% of the
        # function for one small MessagePack decode. Which registrations a
        # selector matches depends only on the selector.
        self._format_cache: dict[
            _Mimetype, list[_DeserializerRegistration]
        ] = {}
        #: Set only while register_defaults() runs, so the registrations it
        #: makes can be told apart from a caller's.
        self._registering_defaults = False
        if register_defaults:
            self.register_defaults()

    def _invalidate_resolution_cache(self) -> None:
        """Forget memoized lookups.

        Call from anything that changes what a lookup would answer.
        """
        self._resolution_cache.clear()
        self._format_cache.clear()

    @_status_boundary
    def set_type_tag(self, obj_type: type, tag: str) -> None:
        """Pin the wire tag used to identify ``obj_type`` in serialized data.

        Overrides the default fully-qualified name, giving a type a short,
        stable identifier or disambiguating two like-named types.
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
        self._invalidate_resolution_cache()

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
        """The wire tag for ``obj_type``.

        A tag pinned on this registry wins, then one the class declares for
        itself with an ``A11_SERIAL_TAG`` ClassVar, and failing both the
        qualified name.
        """
        explicit = self._type_tags.get(obj_type)
        if explicit is not None:
            return explicit
        declared = _declared_serial_tag(obj_type)
        if declared is not None:
            return declared
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
                builtin=self._registering_defaults,
            )
        )
        self._remember_type(obj_type)
        self._invalidate_resolution_cache()

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
                obj_type,
                parsed,
                serializer,
                serializer_order,
                builtin=self._registering_defaults,
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
                builtin=self._registering_defaults,
            )
        )
        self._remember_type(obj_type)
        self._invalidate_resolution_cache()

    @_status_boundary
    def register_defaults(self) -> None:
        """Install the standard JSON and MessagePack registrations."""

        self._registering_defaults = True
        try:
            _register_default_serializers(self)
        finally:
            self._registering_defaults = False
        self._invalidate_resolution_cache()

    def _resolve_serializer(
        self, obj: Any, mimetype: str
    ) -> tuple[_SerializerRegistration, str]:
        """Pick the codec and the exact mimetype, without running either.

        Kept separate from `to_chunk` so that "would this block the loop?" can
        be answered without serializing the value -- asking by serializing runs
        a caller's codec an extra time, and on the wrong thread.
        """
        actual_type = type(obj)
        cache_key = (actual_type, mimetype)
        cached = self._resolution_cache.get(cache_key)
        if cached is not None:
            return cached

        selection = (
            _parse_mimetype(mimetype, allow_patterns=True) if mimetype else None
        )
        candidates = [
            candidate
            for candidate in self._serializers
            if isinstance(obj, candidate.obj_type)
            and (
                selection is None
                or _registration_matches(candidate.mimetype, selection)
            )
        ]
        candidates.sort(
            key=lambda candidate: (
                _inheritance_distance(actual_type, candidate.obj_type),
                candidate.order,
            )
        )
        if not candidates:
            requested = mimetype or "the object's type"
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=(
                    f"No serializer is registered for"
                    f" {actual_type.__name__} and {requested!r}."
                ),
            ).to_exception()

        registration = candidates[0]
        exact_mimetype = _format_exact_mimetype(
            registration.mimetype, self._type_tag(actual_type)
        )
        # Remembering the type is part of resolving it, and like the rest of it
        # only has to happen once per type.
        self._remember_type(actual_type)
        resolved = (registration, exact_mimetype)
        self._resolution_cache[cache_key] = resolved
        return resolved

    async def to_chunk_async(self, obj: Any, mimetype: str = "") -> types.Chunk:
        """`to_chunk`, off the event loop only when it might block it.

        A caller's codec may take as long as it likes, so it goes to a worker
        thread -- that is a contract, not an optimisation, and
        `test_object_serialization_and_deserialization_run_in_worker_threads`
        holds it. The registry's own codecs are a different matter: they are a
        few microseconds, and `asyncio.to_thread` costs an order of magnitude
        more than they do, so paying for a thread to run one is pure loss.

        Deciding on the *codec* rather than on the payload's size is what makes
        this safe. A small value can still have a slow codec -- that is exactly
        what the test registers -- so size says nothing about whether the loop
        is about to be blocked.
        """
        if self._resolves_to_builtin_serializer(obj, mimetype):
            return self.to_chunk(obj, mimetype)
        return await asyncio.to_thread(self.to_chunk, obj, mimetype)

    async def from_chunk_async(
        self,
        chunk: types.Chunk,
        mimetype_patterns: str | Sequence[str] = "",
        obj_type: type | None = None,
    ) -> Any:
        """`from_chunk`, off the event loop only when it might block it.

        The mirror of `to_chunk_async`; see it for why the decision is made on
        the codec and not the payload.
        """
        if self._resolves_to_builtin_deserializer(chunk, mimetype_patterns):
            return self.from_chunk(chunk, mimetype_patterns, obj_type)
        return await asyncio.to_thread(
            self.from_chunk, chunk, mimetype_patterns, obj_type
        )

    def _resolves_to_builtin_serializer(self, obj: Any, mimetype: str) -> bool:
        """Whether `to_chunk` would pick one of the registry's own codecs."""
        if not isinstance(mimetype, str):
            return False
        try:
            registration, _ = self._resolve_serializer(obj, mimetype)
        except StatusException:
            # Let the real call report the real error, on its own thread.
            return False
        return registration.builtin

    def _resolves_to_builtin_deserializer(
        self, chunk: types.Chunk, mimetype_patterns: str | Sequence[str]
    ) -> bool:
        """Whether every codec `from_chunk` might pick is one of ours.

        Conservative on purpose: if a caller has registered anything that could
        match this representation, the whole decode goes to a thread.
        """
        if mimetype_patterns:
            return False
        actual_mimetype = chunk.get_mimetype()
        if not actual_mimetype:
            return False
        try:
            actual = _parse_mimetype(actual_mimetype, allow_patterns=False)
        except StatusException:
            return False
        matches = self._format_cache.get(actual)
        if matches is None:
            matches = [
                registration
                for registration in self._deserializers
                if _registration_matches(registration.mimetype, actual)
            ]
            self._format_cache[actual] = matches
        return bool(matches) and all(
            registration.builtin for registration in matches
        )

    @_status_boundary
    def to_chunk(self, obj: Any, mimetype: str = "") -> types.Chunk:
        """Serialize ``obj`` into a chunk.

        If ``mimetype`` is empty, the closest registered Python type wins and
        registration order chooses its preferred representation.  An explicit
        MIME value selects a representation and can be exact or contain ``*``
        wildcards; the object's own type decides the tag, so a ``type``
        parameter in it is ignored.
        """

        if not isinstance(mimetype, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Mimetype must be a string.",
            ).to_exception()

        actual_type = type(obj)
        # Resolution depends on the type and the requested mimetype and on
        # nothing else, so it is done once per pair. `isinstance` against every
        # registration, the sort, and re-formatting the exact mimetype are all
        # on this side of the line; only the encode itself is below it.
        registration, exact_mimetype = self._resolve_serializer(obj, mimetype)
        serialized = registration.serializer(obj)

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

    @overload
    def from_chunk(
        self,
        chunk: types.Chunk,
        mimetype_patterns: str | Sequence[str],
        obj_type: type[_T],
    ) -> _T: ...

    @overload
    def from_chunk(
        self,
        chunk: types.Chunk,
        mimetype_patterns: str | Sequence[str] = "",
        *,
        obj_type: type[_T],
    ) -> _T: ...

    @overload
    def from_chunk(
        self,
        chunk: types.Chunk,
        mimetype_patterns: str | Sequence[str] = "",
        obj_type: None = None,
    ) -> Any: ...

    @_status_boundary
    def from_chunk(
        self,
        chunk: types.Chunk,
        mimetype_patterns: str | Sequence[str] = "",
        obj_type: type | None = None,
    ) -> Any:
        """Deserialize ``chunk``.

        Selectors choose the *representation*: they are matched in order
        against the chunk's media type and may contain wildcards.  If the chunk
        has no MIME metadata, a supplied exact selector acts as the
        representation.

        What comes back is chosen separately.  An explicit ``obj_type`` always
        wins and the registry makes a best effort to produce it, reporting the
        deserializer's own error when the data will not fit.  Otherwise the
        chunk's ``type`` parameter names the type, and when it names nothing --
        because it is absent, or because the module that would define it was
        never imported -- the payload decodes to whatever its format describes:
        a `dict`, a `list`, a scalar.
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

        # The tag comes from the chunk itself, never from a selector: a caller
        # overriding a stale media type is correcting the representation, not
        # renaming the value.
        encoded_name = (
            actual.get_parameter(_TYPE_PARAMETER)
            if actual is not None
            else None
        )
        target_type = obj_type
        if target_type is None and encoded_name:
            target_type = self._resolve_type(encoded_name)

        selectors = self._prepare_selectors(mimetype_patterns, actual)
        had_matching_format = False

        for selection in selectors:
            format_registrations = self._format_cache.get(selection)
            if format_registrations is None:
                format_registrations = [
                    registration
                    for registration in self._deserializers
                    if _registration_matches(registration.mimetype, selection)
                ]
                self._format_cache[selection] = format_registrations
            if not format_registrations:
                continue
            had_matching_format = True

            selected = target_type
            if selected is None:
                # A codec registered for exactly one type needs no help
                # deciding what to build.
                distinct_types = {
                    registration.obj_type
                    for registration in format_registrations
                }
                if len(distinct_types) == 1:
                    selected = next(iter(distinct_types))

            if selected is None:
                # Nothing named a type, so the representation is the whole
                # answer.  Registration order picks the codec; the built-in
                # ones all decode the same bytes the same way.
                registration = min(
                    format_registrations,
                    key=lambda registration: registration.order,
                )
                return self._invoke_deserializer(registration, chunk, None)

            candidates = [
                registration
                for registration in format_registrations
                if issubclass(selected, registration.obj_type)
            ]
            candidates.sort(
                key=lambda registration: (
                    _inheritance_distance(
                        cast(type, selected), registration.obj_type
                    ),
                    registration.order,
                )
            )
            if not candidates:
                continue

            registration = candidates[0]
            result = self._invoke_deserializer(registration, chunk, selected)
            if not isinstance(result, selected):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "Deserializer returned"
                        f" {type(result).__name__}; expected"
                        f" {selected.__name__}."
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
        # A registration names a representation.  Which type it produces is
        # the obj_type argument, so any type parameter here is redundant.
        parsed = _parse_mimetype(mimetype, allow_patterns=False)
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
        declared = _declared_serial_tag(obj_type)
        if declared is not None:
            self._known_by_tag.setdefault(declared, obj_type)

    @_status_boundary
    def resolve_type(self, name: str) -> type | None:
        """The Python type a wire tag names, or ``None`` if none is known.

        The reverse of [_type_tag][a11.data.serialization.
        SerializationRegistry._type_tag]: given ``a11.sdk.AudioBuffer``, the
        class it identifies. What a tag resolves to depends on what has been
        registered and imported, so a caller that gets ``None`` has learned
        that this process does not know the type -- not that it does not exist.
        """
        return self._resolve_type(name)

    @_status_boundary
    def _resolve_type(self, name: str) -> type | None:
        # 1. An explicitly-pinned tag always wins.
        explicit = self._tag_to_type.get(name)
        if explicit is not None:
            return explicit

        # 2. A previously-seen declared tag or fully-qualified name.
        by_tag = self._known_by_tag.get(name)
        if by_tag is not None:
            return by_tag

        # 3. Scan loaded subclasses for one that declares or qualifies as this
        #    tag.  A bare class name is never enough: two SDKs may each define
        #    a TextDelta, and picking whichever loaded first is a coin toss.
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
            if (
                _declared_serial_tag(candidate) == name
                or _qualified_name(candidate) == name
            ):
                self._remember_type(candidate)
                return candidate
            try:
                pending.extend(candidate.__subclasses__())
            except TypeError:
                pass

        # 4. Last resort: import a dotted qualified name directly.
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
            # An explicit selector is authoritative, so it stands in for the
            # chunk's own media type when the two disagree -- which is how
            # stale metadata gets repaired.
            selectors.append(
                actual
                if actual is not None and _mimetype_matches(actual, pattern)
                else pattern
            )

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
    def _invoke_deserializer(
        self,
        registration: _DeserializerRegistration,
        chunk: types.Chunk,
        target_type: type | None,
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


_MSGPACK_MIN_INT = -(2**63)
_MSGPACK_MAX_INT = 2**64 - 1


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


def _nested_models(
    annotation: Any, seen: set[type]
) -> list[type[pydantic.BaseModel]]:
    """Every model reachable through ``annotation``, innermost first."""
    found: list[type[pydantic.BaseModel]] = []
    for argument in getattr(annotation, "__args__", ()):
        found.extend(_nested_models(argument, seen))
    if (
        isinstance(annotation, type)
        and issubclass(annotation, pydantic.BaseModel)
        and annotation not in seen
    ):
        seen.add(annotation)
        for field in annotation.model_fields.values():
            found.extend(_nested_models(field.annotation, seen))
        found.append(annotation)
    return found


def _normalize_bytes_validation(model_type: type[pydantic.BaseModel]) -> None:
    """Teach a model tree to read the base64 its byte fields are written as.

    Bytes go out as base64, and pydantic's default is to read a JSON string
    into `bytes` as UTF-8 -- which quietly mangles any payload that is not
    text. Its base64 reader accepts the standard alphabet A11 writes as
    well as the URL-safe one, so this only has to be turned on. Only *reading*
    is delegated: pydantic writes URL-safe base64, so A11 keeps writing its
    own.

    Every nested model needs the same treatment, and needs it first: an outer
    model compiles its fields' schemas into its own, so rebuilding it before
    its members would bake in the unfixed ones.
    """
    if model_type in _NORMALIZED_MODELS:
        return
    for model in _nested_models(model_type, set()):
        _NORMALIZED_MODELS.add(model)
        if model.model_config.get("val_json_bytes") != "base64":
            model.model_config = {
                **model.model_config,
                "val_json_bytes": "base64",
            }
        model.model_rebuild(force=True)


_NORMALIZED_MODELS: set[type] = set()


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


def _to_wire(value: Any, *, binary: bool) -> Any:
    """Encode ``value`` as a JSON- or MessagePack-ready tree.

    Nothing here is tagged.  A `bytes` becomes base64 (or, in MessagePack, real
    bytes), a `datetime` becomes an ISO string, a `set` becomes an array --
    exactly what the format can say, and no more.  Recovering the Python type
    is the reader's job, and it does that from the chunk's ``;type=`` or the
    caller's ``obj_type``.
    """

    def rec(item: Any) -> Any:
        return _to_wire(item, binary=binary)

    if value is None or isinstance(value, (bool, str)):
        return value
    if isinstance(value, int):
        if binary and not (_MSGPACK_MIN_INT <= value <= _MSGPACK_MAX_INT):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "MessagePack cannot represent integers outside the 64-bit"
                    " range; use JSON for arbitrary-precision integers."
                ),
            ).to_exception()
        return value
    if isinstance(value, float):
        # JSON has no NaN or infinity; _serialize_json rejects them. MessagePack
        # carries them natively.
        return value
    if isinstance(value, (bytes, bytearray)):
        if binary:
            return bytes(value)
        return base64.b64encode(value).decode("ascii")
    if isinstance(value, (timing.Time, timing.Duration)):
        return _timing_value(value)
    if isinstance(value, (datetime.datetime, datetime.date, datetime.time)):
        return value.isoformat()
    if isinstance(value, datetime.timedelta):
        return (
            value.days * 86_400_000_000
            + value.seconds * 1_000_000
            + value.microseconds
        )
    if isinstance(value, uuid.UUID):
        return str(value)
    if isinstance(value, _NATIVE_DATA_TYPES):
        # The native dumpers are exact.  In JSON mode they have already spelled
        # bytes as base64 and timestamps as RFC 3339; in binary mode the walk
        # below does it.
        return rec(value.model_dump(mode="python" if binary else "json"))
    if isinstance(value, pydantic.BaseModel):
        return {key: rec(item) for key, item in _pydantic_values(value).items()}
    if isinstance(value, enum.Enum):
        return rec(value.value)
    if isinstance(value, dict):
        # Keys go out as they are: JSON stringifies the scalars it can and
        # rejects the rest, and MessagePack keeps them. Neither invents a
        # representation A11 would then have to teach every peer to read.
        return {key: rec(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set, frozenset)):
        return [rec(item) for item in value]

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


def _validate_pydantic_model(
    model_type: type[pydantic.BaseModel],
    value: Any,
    *,
    binary: bool = True,
) -> pydantic.BaseModel:
    """Rebuild a model from a wire tree.

    A JSON tree spells the model's byte fields as base64, which only pydantic's
    JSON validator reads as bytes -- so the tree goes back through JSON rather
    than being validated as Python objects. A MessagePack tree already carries
    real bytes and validates directly.
    """
    validation_value = value
    if (
        getattr(model_type, "__pydantic_root_model__", False)
        and isinstance(value, dict)
        and set(value) == {"root"}
    ):
        validation_value = value["root"]
    _normalize_bytes_validation(model_type)
    if binary:
        result = model_type.model_validate(
            validation_value, by_alias=True, by_name=True
        )
    else:
        result = _validate_model_json(model_type, json.dumps(validation_value))
    if not isinstance(result, model_type):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"Validation did not produce a {model_type.__name__} instance."
            ),
        ).to_exception()
    return result


def _validate_model_json(
    model_type: type[pydantic.BaseModel], data: str | bytes
) -> pydantic.BaseModel:
    _normalize_bytes_validation(model_type)
    return model_type.model_validate_json(data, by_alias=True, by_name=True)


def _coerce_target(
    value: Any,
    target: type | None,
    *,
    binary: bool = True,
) -> Any:
    """Best-effort conversion of a decoded tree into ``target``.

    ``None`` asks for nothing in particular, so the tree is the answer.  A
    target the data cannot fill raises rather than returning something
    plausible: that is the "real deserialization error" the caller asked to
    hear about.
    """
    if target is None:
        return value

    if target in _NATIVE_DATA_TYPES:
        if isinstance(value, target):
            return value
        return types.validate_wire(target, value)
    if issubclass(target, pydantic.BaseModel):
        if isinstance(value, target):
            return value
        return _validate_pydantic_model(target, value, binary=binary)
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


def _serialize_json(obj: Any) -> bytes:
    try:
        return json.dumps(
            _to_wire(obj, binary=False),
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


def _deserialize_json(data: str | bytes, obj_type: type | None) -> Any:
    # A declared model reads the payload itself: its annotations are what turn
    # the tree back into Chunks, bytes and timestamps, and going straight to
    # pydantic's JSON validator keeps the bytes from being read as UTF-8.
    if (
        isinstance(obj_type, type)
        and obj_type not in _NATIVE_DATA_TYPES
        and issubclass(obj_type, pydantic.BaseModel)
    ):
        try:
            return _validate_model_json(obj_type, data)
        except pydantic.ValidationError as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid {obj_type.__name__} JSON: {exc}",
            ).to_exception() from exc
    try:
        decoded = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid JSON data: {exc}",
        ).to_exception() from exc
    return _coerce_target(decoded, obj_type, binary=False)


def _serialize_msgpack(obj: Any) -> bytes:
    try:
        return msgpack.packb(_to_wire(obj, binary=True), use_bin_type=True)
    except StatusException:
        raise
    except (TypeError, ValueError, OverflowError, msgpack.PackException) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Failed to serialize MessagePack: {exc}",
        ).to_exception() from exc


def _deserialize_msgpack(data: bytes, obj_type: type | None) -> Any:
    try:
        decoded = msgpack.unpackb(data, raw=False, strict_map_key=False)
    except (TypeError, ValueError, msgpack.UnpackException) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid MessagePack data: {exc}",
        ).to_exception() from exc
    return _coerce_target(decoded, obj_type)


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


#: Canonical cross-language tags for the runtime's own types. They are native
#: (pybind11) classes and so cannot declare an ``A11_SERIAL_TAG`` ClassVar the
#: way an SDK model does; a registry pins their tags from here instead.
CORE_TYPE_TAGS: dict[type, str] = {
    types.ChunkMetadata: serial_tags.CHUNK_METADATA,
    types.Chunk: serial_tags.CHUNK,
    types.NodeRef: serial_tags.NODE_REF,
    types.NodeFragment: serial_tags.NODE_FRAGMENT,
    types.Port: serial_tags.PORT,
    types.ActionMessage: serial_tags.ACTION_MESSAGE,
    types.WireMessage: serial_tags.WIRE_MESSAGE,
    Status: serial_tags.STATUS,
    timing.Time: serial_tags.TIME,
    timing.Duration: serial_tags.DURATION,
}


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
    for obj_type in DEFAULT_SERIALIZABLE_TYPES:
        registry.register(
            obj_type,
            JSON_MIMETYPE,
            _serialize_json,
            _deserialize_json,
        )
        registry.register(
            obj_type,
            MSGPACK_MIMETYPE,
            _serialize_msgpack,
            _deserialize_msgpack,
        )

    # JSON-native values use language-neutral tags rather than Python's names.
    # These are exactly the tags _format_exact_mimetype leaves off the wire:
    # the format already says this much.
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

    # Anything else well-known keeps its bare-name tag; user-defined types use
    # qualified, collision-free tags by default.
    for obj_type in DEFAULT_SERIALIZABLE_TYPES:
        if obj_type is pydantic.BaseModel:
            continue
        default = CORE_TYPE_TAGS.get(obj_type, obj_type.__name__)
        registry._set_type_tag(
            obj_type,
            canonical_json_tags.get(obj_type, default),
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

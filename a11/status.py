import asyncio
import contextlib
import copy
import json
from enum import IntEnum
from typing import Any, Callable

import fastapi
import httpx
import msgpack
import pydantic
from pydantic_core import core_schema
from pydantic import BaseModel, Field, ValidationError

from a11 import _native


class StatusCode(IntEnum):
    """Canonical gRPC / Abseil status codes, mapped to HTTP status codes."""

    OK = 0
    CANCELLED = 1
    UNKNOWN = 2
    INVALID_ARGUMENT = 3
    DEADLINE_EXCEEDED = 4
    NOT_FOUND = 5
    ALREADY_EXISTS = 6
    PERMISSION_DENIED = 7
    RESOURCE_EXHAUSTED = 8
    FAILED_PRECONDITION = 9
    ABORTED = 10
    OUT_OF_RANGE = 11
    UNIMPLEMENTED = 12
    INTERNAL = 13
    UNAVAILABLE = 14
    DATA_LOSS = 15
    UNAUTHENTICATED = 16

    @classmethod
    def __get_pydantic_json_schema__(cls, core_schema, handler):
        schema = handler(core_schema)
        schema["description"] = "\n\n".join(
            f"{member.value} = {member.name}" for member in cls
        )
        return schema

    @staticmethod
    def from_http_code(http_code: int) -> "StatusCode":
        from a11 import _native

        return StatusCode(_native.status_code_from_http(http_code))

    @staticmethod
    def from_ws_code(code: int) -> "StatusCode":
        from a11 import _native

        return StatusCode(_native.status_code_from_websocket(code))

    def to_ws_code(self) -> int:
        from a11 import _native

        return _native.status_code_to_websocket(self.value)

    def to_http_code(self) -> int:
        from a11 import _native

        return _native.status_code_to_http(self.value)

    def __str__(self) -> str:
        return self.name


class StatusException(Exception):
    def __init__(self, status: "Status"):
        if status.is_ok():
            raise ValueError(
                "Cannot create a StatusException for an OK status."
            )

        self.status = status
        super().__init__(status.message)

    def __str__(self) -> str:
        return f"{self.status.code}: {self.status.message}"

    def __repr__(self) -> str:
        return f"code: {self.status.code.name}, message: {self.status.message}"


class StatusParseResult(BaseModel):
    # status of validation
    validation_status: "Status"

    # actual parsed status; in case of failed validation is set to UNKNOWN
    parsed: "Status"

    def is_ok(self):
        return self.validation_status.is_ok()


class _StatusConvenience(BaseModel):
    code: StatusCode = Field(
        default=StatusCode.OK,
        description=(
            "The status code representing the outcome. Allowed values are: "
            + ", ".join(
                f"{member.value} = {member.name}" for member in StatusCode
            )
            + "."
        ),
    )
    message: str = Field(
        default="OK",
        description=(
            "A human-readable message providing more details about the status."
        ),
    )
    details: list[dict] = Field(
        default_factory=list,
        description=(
            "A list of additional details about the status, represented as"
            " arbitrary objects."
        ),
        exclude_if=lambda x: not x,
    )

    @staticmethod
    def from_exception(
        exc: BaseException, casters: "StatusExceptionCasters | None" = None
    ) -> "Status":
        casters = casters or StatusExceptionCasters.global_instance()
        return casters.cast(exc)

    @staticmethod
    def from_http_exception(
        http_exception: fastapi.HTTPException | httpx.HTTPStatusError,
    ) -> "Status":
        if isinstance(http_exception, fastapi.HTTPException):
            return _make_status_from_fastapi_exception(http_exception)
        if isinstance(http_exception, httpx.HTTPStatusError):
            return _make_status_from_httpx_exception(http_exception)

        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Unsupported HTTP exception type: {type(http_exception)}",
        ).to_exception()

    @staticmethod
    def get_fastapi_response_dict_for_codes(
        *codes: StatusCode,
    ) -> dict[int, dict]:
        responses = {}
        for code in codes:
            responses[code.to_http_code()] = {
                "model": Status,
                "content": {
                    "application/json": {
                        "example": _STATUS_EXAMPLES[code],
                    }
                },
            }

        validation_error_example = _STATUS_EXAMPLES[
            StatusCode.INVALID_ARGUMENT
        ].model_copy()
        validation_error_example.details = [
            {
                "type": "missing",
                "loc": ["body", "name"],
                "msg": "Field required",
                "input": {},
            }
        ]
        responses[422] = {
            "model": Status,
            "content": {
                "application/json": {
                    "example": validation_error_example,
                }
            },
        }
        return responses

    @staticmethod
    def get_fastapi_response_dict_for_http_codes(
        *codes: int,
    ) -> dict[int, dict]:
        responses = {}
        for http_code in codes:
            responses[http_code] = {"model": Status}
        return responses

    @staticmethod
    def ok(message: str | None = None) -> "Status":
        return Status(
            code=StatusCode.OK,
            message="OK" if message is None else message,
            details=[],
        )

    @staticmethod
    def parse_from_json(data: str | bytes) -> StatusParseResult:
        if isinstance(data, bytes):
            data = data.decode("utf-8")

        try:
            parsed = Status.model_validate(data)
            return StatusParseResult(
                validation_status=Status.ok(), parsed=parsed
            )
        except ValidationError as exc:
            return StatusParseResult(
                validation_status=Status(
                    code=StatusCode.OUT_OF_RANGE,
                    message=(
                        "Input data did not contain a valid "
                        'Status JSON. Check .details[0]["input"] for the '
                        "raw input."
                    ),
                    details=[{"input": data, "error": str(exc)}],
                ),
                parsed=Status(
                    code=StatusCode.UNKNOWN,
                    message=(
                        "This status is inconclusive, because the input data "
                        "did not contain a valid Status JSON. Check "
                        ".validation_status on the returned StatusParseResult."
                    ),
                ),
            )

    def to_msgpack(self, packer: msgpack.Packer) -> None:
        try:
            packer.pack([self.code, self.message])
        except Exception as exc:
            raise Status(
                code=StatusCode.INTERNAL,
                message="Failed to pack status into MessagePack data.",
                details=[{"error": str(exc)}],
            ).to_exception()

    def __str__(self) -> str:
        return f"{self.code}: {self.message}"

    def is_ok(self) -> bool:
        return self.code == StatusCode.OK

    def raise_if_not_ok(self) -> None:
        if not self.is_ok():
            raise self.to_exception()

    def to_exception(self) -> StatusException:
        if self.is_ok():
            raise Status(
                code=StatusCode.INTERNAL,
                message="Cannot convert an OK status to an exception.",
                details=[],
            ).to_exception()
        exc = StatusException(self)
        return exc


Status = _native.Status
Status.__module__ = __name__


def _status_model_validate(cls, value: Any, **_: Any) -> "Status":
    if isinstance(value, cls):
        return value
    if isinstance(value, (str, bytes, bytearray)):
        value = json.loads(value)
    validated = _StatusConvenience.model_validate(value)
    return cls(
        code=validated.code,
        message=validated.message,
        details=validated.details,
    )


def _status_model_dump(
    self: "Status", *, mode: str = "python", **_: Any
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "code": int(self.code) if mode == "json" else self.code,
        "message": self.message,
    }
    if self.details:
        result["details"] = copy.deepcopy(self.details)
    return result


def _status_model_dump_json(self: "Status", **kwargs: Any) -> str:
    return json.dumps(self.model_dump(mode="json"), **kwargs)


def _status_model_copy(
    self: "Status",
    *,
    update: dict[str, Any] | None = None,
    deep: bool = False,
) -> "Status":
    values = self.model_dump()
    if deep:
        values = copy.deepcopy(values)
    if update:
        values.update(update)
    return Status(**values)


def _status_model_json_schema(cls, **kwargs: Any) -> dict[str, Any]:
    schema = _StatusConvenience.model_json_schema(**kwargs)
    schema["title"] = "Status"
    return schema


def _status_core_schema(cls, _source_type, _handler):
    return core_schema.no_info_plain_validator_function(
        cls.model_validate,
        serialization=core_schema.plain_serializer_function_ser_schema(
            lambda value: value.model_dump(mode="json"),
            when_used="always",
        ),
    )


def _status_json_schema(cls, _schema, _handler):
    return cls.model_json_schema()


Status.model_validate = classmethod(_status_model_validate)
Status.model_dump = _status_model_dump
Status.model_dump_json = _status_model_dump_json
Status.model_copy = _status_model_copy
Status.__copy__ = lambda self: self.model_copy()
Status.__deepcopy__ = lambda self, _memo: self.model_copy(deep=True)
Status.model_json_schema = classmethod(_status_model_json_schema)
Status.__get_pydantic_core_schema__ = classmethod(_status_core_schema)
Status.__get_pydantic_json_schema__ = classmethod(_status_json_schema)

for _name in (
    "from_exception",
    "from_http_exception",
    "get_fastapi_response_dict_for_codes",
    "get_fastapi_response_dict_for_http_codes",
    "ok",
    "parse_from_json",
):
    setattr(Status, _name, staticmethod(getattr(_StatusConvenience, _name)))

for _name in ("raise_if_not_ok", "to_exception", "to_msgpack"):
    setattr(Status, _name, getattr(_StatusConvenience, _name))

StatusParseResult.model_rebuild()


class StatusExceptionCasters:
    def __init__(self):
        self._casters: dict[
            type[BaseException], Callable[[BaseException], Status]
        ] = {}

    @staticmethod
    def global_instance() -> "StatusExceptionCasters":
        if not hasattr(StatusExceptionCasters, "_global_instance"):
            StatusExceptionCasters._global_instance = StatusExceptionCasters()

            instance = StatusExceptionCasters._global_instance
            instance.register(
                ValidationError, make_status_from_pydantic_validation_error
            )
            instance.register(httpx.HTTPError, _cast_httpx_exceptions)
            instance.register(httpx.InvalidURL, _cast_httpx_exceptions)
            instance.register(httpx.CookieConflict, _cast_httpx_exceptions)
            instance.register(httpx.StreamError, _cast_httpx_exceptions)

        return StatusExceptionCasters._global_instance

    def cast(self, exc: BaseException) -> Status:
        if isinstance(exc, StatusException):
            return exc.status

        if isinstance(exc, asyncio.CancelledError):
            return Status(code=StatusCode.CANCELLED, message="Cancelled.")

        for cls in exc.__class__.__mro__:
            if cls in self._casters:
                return self._casters[cls](exc)

        return Status(
            code=StatusCode.UNKNOWN,
            message=str(exc),
        )

    def register(
        self,
        exception_type: type[BaseException],
        caster: Callable[[BaseException], Status],
    ):
        if exception_type is StatusException:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Setting a custom caster for StatusException is not"
                    " allowed."
                ),
            ).to_exception()

        if exception_type in self._casters:
            raise Status(
                code=StatusCode.ALREADY_EXISTS,
                message=(
                    f"Custom caster for {exception_type} is already registered."
                ),
            ).to_exception()

        self._casters[exception_type] = caster


@contextlib.contextmanager
def reraise_exceptions_as_status(casters: StatusExceptionCasters | None = None):
    casters = casters or StatusExceptionCasters.global_instance()
    try:
        yield
    except Exception as exc:
        status = casters.cast(exc)
        raise status.to_exception() from exc


def _cast_httpx_exceptions(exc: Exception) -> Status:
    if isinstance(exc, httpx.InvalidURL):
        return Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"httpx.InvalidURL: {str(exc)}",
        )

    if isinstance(exc, httpx.CookieConflict):
        return Status(
            code=StatusCode.ALREADY_EXISTS,
            message=f"httpx.CookieConflict: {str(exc)}",
        )

    if isinstance(exc, httpx.TimeoutException):
        return Status(
            code=StatusCode.DEADLINE_EXCEEDED,
            message=str(exc) or f"Timed out ({exc.__class__.__name__})",
        )

    if isinstance(exc, httpx.ProxyError):
        return Status(
            code=StatusCode.UNAVAILABLE,
            message=str(exc),
        )

    if isinstance(exc, httpx.NetworkError):
        what = str(exc)
        return Status(
            code=StatusCode.UNAVAILABLE,
            message=(
                f"httpx.NetworkError: {what}"
                if what
                else f"Network error ({exc.__class__.__name__})"
            ),
        )

    if isinstance(exc, httpx.ProtocolError):
        return Status(
            code=StatusCode.UNAVAILABLE,
            message=str(exc),
        )

    if isinstance(exc, httpx.DecodingError):
        return Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=str(exc),
        )

    if isinstance(exc, httpx.TooManyRedirects):
        return Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message=f"httpx.TooManyRedirects: {str(exc)}",
        )

    if isinstance(exc, httpx.StreamError):
        return Status(
            code=StatusCode.UNAVAILABLE,
            message=str(exc),
        )

    if isinstance(exc, httpx.HTTPStatusError):
        return Status(
            code=StatusCode.from_http_code(exc.response.status_code),
            message=exc.response.reason_phrase,
            details=[{"response": exc.response.content}],
        )

    if isinstance(exc, httpx.HTTPError):
        return Status(
            code=StatusCode.UNKNOWN,
            message=f"Unexpected HTTP error: {str(exc)}",
            details=[{"exc": str(exc)}],
        )

    return Status(
        code=StatusCode.UNKNOWN,
        message=f"Unexpected exception: {str(exc)}",
    )


def _make_status_from_fastapi_exception(exc: fastapi.HTTPException) -> Status:
    code = StatusCode.from_http_code(exc.status_code)
    detail = exc.detail
    message = "An error occurred."
    details = []

    if isinstance(detail, dict):
        message = detail.get("message", message)
        details = detail.get("details", details)
    elif isinstance(detail, str):
        message = detail

    return Status(code=code, message=message, details=details)


def _make_status_from_httpx_exception(exc: httpx.HTTPStatusError) -> Status:
    return Status(
        code=StatusCode.from_http_code(exc.response.status_code),
        message=exc.response.reason_phrase,
        details=[{"response": exc.response.content}],
    )


def make_status_from_pydantic_validation_error(exc: pydantic.ValidationError):
    errors = []
    for e in exc.errors():
        error = {}
        for field in {"loc", "msg", "type", "input", "url", "ctx"}:
            if field in e:
                error[field] = e[field]  # type: ignore
        errors.append(error)

    return Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=f"{str(exc)}",
        details=errors,
    )


_STATUS_EXAMPLES = {
    StatusCode.OK: Status.ok(),
    StatusCode.CANCELLED: Status(
        code=StatusCode.CANCELLED,
        message="The request was cancelled, typically by the caller",
    ),
    StatusCode.UNKNOWN: Status(
        code=StatusCode.UNKNOWN,
        message="There is no way to determine a more specific error code",
    ),
    StatusCode.INVALID_ARGUMENT: Status(
        code=StatusCode.INVALID_ARGUMENT,
        message="The request parameters would never work (validation error)",
    ),
    StatusCode.DEADLINE_EXCEEDED: Status(
        code=StatusCode.DEADLINE_EXCEEDED,
        message="The operation did not complete within the specified deadline",
    ),
    StatusCode.NOT_FOUND: Status(
        code=StatusCode.NOT_FOUND,
        message=(
            "The requested entity does not exist (or sometimes, the requester"
            " does not have access to it)"
        ),
    ),
    StatusCode.ALREADY_EXISTS: Status(
        code=StatusCode.ALREADY_EXISTS,
        message="The entity being created already exists",
    ),
    StatusCode.PERMISSION_DENIED: Status(
        code=StatusCode.PERMISSION_DENIED,
        message="The caller does not have permission to execute the operation",
    ),
    StatusCode.UNAUTHENTICATED: Status(
        code=StatusCode.UNAUTHENTICATED,
        message="The caller’s identity cannot be verified",
    ),
    StatusCode.RESOURCE_EXHAUSTED: Status(
        code=StatusCode.RESOURCE_EXHAUSTED,
        message=(
            "Some infrastructure resource is exhausted (quota, server "
            "capacity, etc); does not always imply caller's fault in \"Too "
            'Many Requests"'
        ),
    ),
    StatusCode.FAILED_PRECONDITION: Status(
        code=StatusCode.FAILED_PRECONDITION,
        message="The system is not in the required state for the operation",
    ),
    StatusCode.ABORTED: Status(
        code=StatusCode.ABORTED,
        message=(
            "The operation was aborted, typically due to a concurrency issue"
            " like sequencer check failures, transaction aborts, etc."
        ),
    ),
    StatusCode.OUT_OF_RANGE: Status(
        code=StatusCode.OUT_OF_RANGE,
        message="The client has iterated too far, and should stop",
    ),
    StatusCode.UNIMPLEMENTED: Status(
        code=StatusCode.UNIMPLEMENTED,
        message="There is no implementation for the requested operation",
    ),
    StatusCode.INTERNAL: Status(
        code=StatusCode.INTERNAL,
        message=(
            "A serious internal invariant is broken (i.e. worthy of a bug or"
            " outage report)"
        ),
    ),
    StatusCode.UNAVAILABLE: Status(
        code=StatusCode.UNAVAILABLE, message="Unavailable"
    ),
    StatusCode.DATA_LOSS: Status(
        code=StatusCode.DATA_LOSS,
        message="Unrecoverable data loss or corruption",
    ),
}

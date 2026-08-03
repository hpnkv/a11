"""Small Python conveniences shared by native C++ option structs."""

from __future__ import annotations

import copy
from collections.abc import Mapping
from typing import Any, get_args

from pydantic_core import core_schema

from a11.status import Status, StatusCode, StatusException


def install_native_options(
    cls: type,
    fields: Mapping[str, tuple[type | Any, Any]],
) -> type:
    """Add mapping validation/copy/schema conveniences to a bound struct."""

    if getattr(cls, "_a11_options_installed", False):
        return cls

    cls.__annotations__ = {
        name: annotation for name, (annotation, _default) in fields.items()
    }

    def model_validate(option_cls, value: Any, **_: Any):
        if isinstance(value, option_cls):
            return value
        if not isinstance(value, Mapping):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    f"{option_cls.__name__} must be validated from a mapping."
                ),
            ).to_exception()
        try:
            return option_cls(**dict(value))
        except StatusException:
            raise
        except Exception as error:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid {option_cls.__name__}: {error}",
            ).to_exception() from None

    def model_dump(self, **_: Any) -> dict[str, Any]:
        return {
            name: getattr(self, name)
            for name in fields
            if getattr(self, name) is not None
        }

    def model_copy(
        self,
        *,
        update: Mapping[str, Any] | None = None,
        deep: bool = False,
    ):
        values = self.model_dump()
        if deep:
            values = copy.deepcopy(values)
        if update:
            values.update(update)
        return cls(**values)

    def model_json_schema(option_cls, **_: Any) -> dict[str, Any]:
        properties: dict[str, Any] = {}
        for name, (annotation, default) in fields.items():
            candidates = get_args(annotation) or (annotation,)
            json_types = []
            unknown_type = False
            for candidate in candidates:
                value_type = {
                    bool: "boolean",
                    int: "integer",
                    float: "number",
                    str: "string",
                    type(None): "null",
                }.get(candidate)
                if value_type is None:
                    unknown_type = True
                    break
                if value_type not in json_types:
                    json_types.append(value_type)
            properties[name] = {}
            if not unknown_type and json_types:
                properties[name]["type"] = (
                    json_types[0] if len(json_types) == 1 else json_types
                )
            if default is None or isinstance(default, (bool, int, float, str)):
                properties[name]["default"] = default
        return {
            "title": option_cls.__name__,
            "type": "object",
            "properties": properties,
        }

    def get_core_schema(option_cls, _source_type, _handler):
        return core_schema.no_info_plain_validator_function(
            option_cls.model_validate,
            serialization=core_schema.plain_serializer_function_ser_schema(
                lambda value: value.model_dump(), when_used="always"
            ),
        )

    def get_json_schema(option_cls, _schema, _handler):
        return option_cls.model_json_schema()

    def repr_options(self) -> str:
        values = ", ".join(f"{name}={getattr(self, name)!r}" for name in fields)
        return f"{cls.__name__}({values})"

    def equal_options(self, other: object) -> bool:
        return isinstance(other, cls) and all(
            getattr(self, name) == getattr(other, name) for name in fields
        )

    cls.model_validate = classmethod(model_validate)
    cls.model_dump = model_dump
    cls.model_copy = model_copy
    cls.model_json_schema = classmethod(model_json_schema)
    cls.__get_pydantic_core_schema__ = classmethod(get_core_schema)
    cls.__get_pydantic_json_schema__ = classmethod(get_json_schema)
    cls.__copy__ = lambda self: self.model_copy()
    cls.__deepcopy__ = lambda self, _memo: self.model_copy(deep=True)
    cls.__repr__ = repr_options
    cls.__eq__ = equal_options
    cls._a11_options_installed = True
    return cls


__all__ = ["install_native_options"]

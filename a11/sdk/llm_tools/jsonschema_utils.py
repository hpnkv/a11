# Copyright 2026 The A11 Authors.

import enum
import json
import types
from typing import Annotated, Any, Literal, Union, get_args, get_origin

from pydantic import BaseModel
from pydantic.fields import FieldInfo

# Maps pydantic/annotated_types constraint attribute names (found on the
# objects in a FieldInfo's `.metadata`) to their JSON Schema keywords.
_FIELD_CONSTRAINT_TO_JSON_SCHEMA_KEY = (
    ("ge", "minimum"),
    ("gt", "exclusiveMinimum"),
    ("le", "maximum"),
    ("lt", "exclusiveMaximum"),
    ("min_length", "minLength"),
    ("max_length", "maxLength"),
    ("multiple_of", "multipleOf"),
    ("pattern", "pattern"),
)


def _field_info_json_schema(field_info: FieldInfo) -> dict[str, Any]:
    schema: dict[str, Any] = {}

    if field_info.title is not None:
        schema["title"] = field_info.title

    if field_info.description is not None:
        schema["description"] = field_info.description

    if field_info.examples is not None:
        schema["examples"] = list(field_info.examples)

    for constraint in field_info.metadata:
        for attr, json_schema_key in _FIELD_CONSTRAINT_TO_JSON_SCHEMA_KEY:
            value = getattr(constraint, attr, None)
            if value is not None:
                schema[json_schema_key] = value

    return schema


def get_json_schema_type(obj_type: Any) -> dict[str, Any]:
    if obj_type is None or obj_type is type(None):
        return {"type": "null"}

    origin = get_origin(obj_type)
    args = get_args(obj_type)

    if origin is Annotated:
        base_type, *metadata = args
        schema = get_json_schema_type(base_type)
        for item in metadata:
            if isinstance(item, FieldInfo):
                schema = {**schema, **_field_info_json_schema(item)}
        return schema

    if origin is Literal:
        return {"enum": list(args)}

    if origin is Union or origin is types.UnionType:
        return {"anyOf": [get_json_schema_type(arg) for arg in args]}

    if isinstance(obj_type, type) and issubclass(obj_type, enum.Enum):
        return {"enum": [member.value for member in obj_type]}

    if isinstance(obj_type, type) and issubclass(obj_type, BaseModel):
        return obj_type.model_json_schema()

    if obj_type in (int, float):
        return {"type": "number"}

    if obj_type == bool:
        return {"type": "boolean"}

    if obj_type == str:
        return {"type": "string"}

    if origin in (list, tuple) or obj_type in (list, tuple):
        schema = {"type": "array"}
        item_args = [arg for arg in args if arg is not Ellipsis]
        if len(item_args) == 1:
            schema["items"] = get_json_schema_type(item_args[0])
        elif len(item_args) > 1:
            schema["items"] = {
                "anyOf": [get_json_schema_type(arg) for arg in item_args]
            }
        return schema

    if origin is dict or obj_type is dict:
        schema = {"type": "object"}
        if len(args) == 2:
            schema["additionalProperties"] = get_json_schema_type(args[1])
        return schema

    return {"type": "object"}


def _is_dedupable_jsonschema(resolved: dict[str, Any]) -> bool:
    """Whether a resolved subschema is worth hoisting into `$defs`.

    Restricted to "named" schemas (models and enums, which is what
    `get_json_schema_type` actually produces `title`s for) so that e.g. a
    plain string field with a stray `Field(title=...)` isn't pointlessly
    turned into a `$ref`.
    """
    if "enum" in resolved:
        return True
    return resolved.get("type") == "object" and "properties" in resolved


def organise_and_deduplicate_jsonschema(schema: dict) -> dict:
    """Hoists every `$defs` entry to the schema root and deduplicates.

    `get_json_schema_type` composes subschemas from independent sources
    (each Pydantic model's own `model_json_schema()` call), so the same
    schema can end up duplicated verbatim in multiple places, and `$defs`
    can end up nested wherever a given subschema happened to be spliced in.
    Since `$ref`s resolve against the document root (see the `Status`/
    `StatusCode` bug this fixes), any `$defs` not living at the root are
    effectively broken once embedded in a larger document.

    This walks the whole tree once, resolves every `$ref` against the
    nearest enclosing (now-stale) `$defs` scope, and re-homes the result
    under a single root-level `$defs` — deduplicating by content so that a
    schema which appears more than once (whether it originally arrived via
    `$ref` or was simply duplicated inline) is fully expanded exactly once,
    with every other occurrence replaced by a `$ref` to that one copy.
    """
    root_defs: dict[str, Any] = {}
    key_to_name: dict[str, str] = {}
    in_progress: dict[int, str] = {}

    def dedupe_key(value: Any) -> str:
        return json.dumps(value, sort_keys=True)

    def unique_name(preferred: str) -> str:
        name = preferred
        suffix = 2
        while name in root_defs:
            name = f"{preferred}__{suffix}"
            suffix += 1
        return name

    def register(preferred_name: str, resolved: dict[str, Any]) -> str:
        key = dedupe_key(resolved)
        existing = key_to_name.get(key)
        if existing is not None:
            return existing

        name = unique_name(preferred_name)
        root_defs[name] = resolved
        key_to_name[key] = name
        return name

    def resolve_named(
        name: str, target: Any, scope_stack: list[dict[str, Any]]
    ) -> str:
        target_id = id(target)
        if target_id in in_progress:
            return in_progress[target_id]

        reserved_name = unique_name(name)
        root_defs[reserved_name] = None  # reserve, guards against cycles
        in_progress[target_id] = reserved_name
        try:
            resolved = walk(target, scope_stack, wrap=False)
        finally:
            del in_progress[target_id]

        key = dedupe_key(resolved)
        existing = key_to_name.get(key)
        if existing is not None and existing != reserved_name:
            del root_defs[reserved_name]
            return existing

        root_defs[reserved_name] = resolved
        key_to_name[key] = reserved_name
        return reserved_name

    def walk(
        node: Any, scope_stack: list[dict[str, Any]], wrap: bool = True
    ) -> Any:
        if isinstance(node, list):
            return [walk(item, scope_stack) for item in node]

        if not isinstance(node, dict):
            return node

        local_defs = node.get("$defs")
        if isinstance(local_defs, dict):
            scope_stack = [*scope_stack, local_defs]

        ref = node.get("$ref")
        if isinstance(ref, str) and ref.startswith("#/$defs/"):
            name = ref.removeprefix("#/$defs/")
            target = next(
                (
                    scope[name]
                    for scope in reversed(scope_stack)
                    if name in scope
                ),
                None,
            )
            overrides = {
                key: walk(value, scope_stack)
                for key, value in node.items()
                if key not in ("$ref", "$defs")
            }
            if target is None:
                return {"$ref": ref, **overrides}

            final_name = resolve_named(name, target, scope_stack)
            return {"$ref": f"#/$defs/{final_name}", **overrides}

        resolved = {
            key: walk(value, scope_stack)
            for key, value in node.items()
            if key != "$defs"
        }

        if wrap and isinstance(resolved.get("title"), str):
            if _is_dedupable_jsonschema(resolved):
                name = register(resolved["title"], resolved)
                return {"$ref": f"#/$defs/{name}"}

        return resolved

    organised = walk(schema, [], wrap=False)
    if root_defs:
        organised = {"$defs": root_defs, **organised}
    return organised

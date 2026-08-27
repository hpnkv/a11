#!/usr/bin/env python3
# Copyright 2026 The A11 Authors.

"""Generate the catalogue the Flow tools know the world by.

The language tools know the *language* exhaustively and the world it runs in not
at all, because `a11::flow_lang` links nothing but Abseil and nlohmann on
purpose. So what the world contains has to reach them as data, and this is where
that data comes from: the live action registry and the live serialization
registry, read once and written down.

Two outputs, both checked in:

* ``testdata/flow/catalogue.json`` -- the snapshot itself, readable by anything.
* ``cpp/a11/flow/catalogue_data.h`` -- the same bytes as a C string, so the
  standalone ``a11-flow`` is useful with nothing configured.

``--check`` reports whether the checked-in files are what this would write and
exits non-zero if they are not, which is what a CI job runs. That is the same
contract the generated editor definitions have: the table lives in one place and
the copy is proved to match it.

A frontend with a *live* registry does not need any of this -- it passes its own
catalogue with the request. The snapshot is the default, not the truth.
"""

from __future__ import annotations

import argparse
import dataclasses
import inspect
import json
import pathlib
import sys
import types
import typing

ROOT = pathlib.Path(__file__).resolve().parents[1]
CATALOGUE_JSON = ROOT / "testdata" / "flow" / "catalogue.json"
CATALOGUE_HEADER = ROOT / "cpp" / "a11" / "flow" / "catalogue_data.h"

#: The Flow field type each Python annotation gives a described type's field.
_FIELD_TYPES: dict[object, str] = {
    str: "string",
    int: "integer",
    float: "number",
    bool: "bool",
    bytes: "bytes",
    dict: "object",
    list: "list",
}


def _flow_type(annotation: object) -> tuple[str, str]:
    """The Flow type an annotation is, and its element type where it has one.

    This shallow representation supports completions, hovers, and list fields.
    The catalogue does not reproduce Python's full generic type system.
    """
    origin = typing.get_origin(annotation)
    if origin in (typing.Union, types.UnionType):
        # `X | None` is Flow's "not required", which the field's own flag says.
        parts = [
            part
            for part in typing.get_args(annotation)
            if part is not type(None)
        ]
        if len(parts) == 1:
            return _flow_type(parts[0])
        return "json", ""
    if origin in (list, tuple, set, frozenset):
        args = typing.get_args(annotation)
        element = _flow_type(args[0])[0] if args else ""
        return "list", element
    if origin in (dict,):
        return "object", ""
    if annotation in _FIELD_TYPES:
        return _FIELD_TYPES[annotation], ""
    tag = getattr(annotation, "A11_SERIAL_TAG", None)
    if isinstance(tag, str):
        return tag, ""
    if isinstance(annotation, type) and issubclass(annotation, bytes):
        return "bytes", ""
    return "json", ""


def _port(name: str, schema: object) -> dict[str, object]:
    port: dict[str, object] = {
        "name": name,
        "type": getattr(schema, "type", ""),
    }
    description = getattr(schema, "description", "")
    if description:
        port["description"] = description
    if getattr(schema, "required", False):
        port["required"] = True
    if not getattr(schema, "unary", True):
        port["unary"] = False
    return port


def _action(schema: object) -> dict[str, object]:
    entry: dict[str, object] = {"name": schema.name}
    if schema.description:
        entry["description"] = schema.description
    for key, ports in (("inputs", schema.inputs), ("outputs", schema.outputs)):
        described = [_port(name, port) for name, port in sorted(ports.items())]
        if described:
            entry[key] = described
    headers = [
        {
            "name": name,
            "type": "string",
            **(
                {"description": header.description}
                if getattr(header, "description", "")
                else {}
            ),
        }
        for name, header in sorted(getattr(schema, "headers", {}).items())
    ]
    if headers:
        entry["headers"] = headers
    return entry


def _fields_of(model: type) -> list[dict[str, object]]:
    """What a described type holds, as a tool needs to list it."""
    described: list[dict[str, object]] = []
    model_fields = getattr(model, "model_fields", None)
    if model_fields:
        for name, field in model_fields.items():
            flow_type, element = _flow_type(field.annotation)
            entry: dict[str, object] = {"name": name, "type": flow_type}
            if element:
                entry["element"] = element
            if field.description:
                entry["description"] = field.description
            if field.is_required():
                entry["required"] = True
            described.append(entry)
        return described
    if dataclasses.is_dataclass(model):
        for field in dataclasses.fields(model):
            flow_type, element = _flow_type(field.type)
            entry = {"name": field.name, "type": flow_type}
            if element:
                entry["element"] = element
            described.append(entry)
        return described
    # A native (pybind11) class: its readable properties are the closest thing
    # it has to fields, and a docstring per property is what a hover wants.
    for name, member in sorted(vars(model).items()):
        if name.startswith("_") or not isinstance(member, property):
            continue
        entry = {"name": name, "type": "json"}
        if member.__doc__:
            entry["description"] = inspect.cleandoc(member.__doc__).split("\n")[
                0
            ]
        described.append(entry)
    return described


#: Modules that register their own actions, asked in that order.
#:
#: Programmatic wherever it can be: a module that knows how to put its actions
#: on a registry is the one place that knows what they are, and a list of names
#: here would be a second one to keep in step.
_REGISTERING_MODULES = (
    "a11.sdk.http.actions",
    "a11.sdk.audio.actions",
    "a11.sdk.bash",
    "a11.sdk.llm_tools",
    "a11.sdk.flow_tools",
)

#: Schemas that have no ``register`` to call, named here on purpose.
#:
#: ``interact_with_llm`` is the foundational one: it is how a flow reaches a
#: model at all, and it is a dispatcher rather than a module with a registry
#: function -- which provider answers it is decided per call by the headers. A
#: tool that could complete `make_http_request`'s ports and not this one would
#: be missing the action people write most.
_NAMED_SCHEMAS = (
    ("a11.sdk.interact_with_llm", "INTERACT_WITH_LLM_SCHEMA"),
    ("a11.sdk.interact_with_llm_schema", "INTERACT_WITH_LLM_SCHEMA"),
)


def _registered_actions() -> list[dict[str, object]]:
    """Every action the SDK can register, by asking each module for its own."""
    from a11.actions import ActionRegistry

    registry = ActionRegistry()
    for module_name in _REGISTERING_MODULES:
        try:
            module = __import__(module_name, fromlist=["register"])
        except Exception as error:  # pragma: no cover - platform-dependent
            print(f"  skipped {module_name}: {error}", file=sys.stderr)
            continue
        entry = getattr(module, "register", None)
        if entry is None:
            continue
        try:
            entry(registry)
        except Exception as error:  # pragma: no cover - platform-dependent
            print(f"  skipped {module_name}: {error}", file=sys.stderr)

    described = [
        _action(registry.get_schema(name))
        for name in registry.list_registered_actions()
    ]
    named = {one["name"] for one in described}
    for module_name, constant in _NAMED_SCHEMAS:
        try:
            schema = getattr(
                __import__(module_name, fromlist=[constant]), constant
            )
        except Exception as error:  # pragma: no cover - optional dependency
            print(
                f"  skipped {module_name}.{constant}: {error}", file=sys.stderr
            )
            continue
        if schema.name in named:
            continue
        named.add(schema.name)
        described.append(_action(schema))
    return sorted(described, key=lambda one: one["name"])


def _registered_types() -> list[dict[str, object]]:
    """Every type a serialization registry knows, by the tag a flow writes."""
    from a11.data.serialization import get_global_serialization_registry

    # Importing the SDK is what puts its models in the registry, so a catalogue
    # generated without it would be a catalogue of the runtime's own types only.
    for module_name in ("a11.sdk", "a11.sdk.llm", "a11.sdk.audio"):
        try:
            __import__(module_name)
        except Exception as error:  # pragma: no cover - platform-dependent
            print(f"  skipped {module_name}: {error}", file=sys.stderr)

    # The canonical table, not the registry's own bookkeeping: `serial_tags` is
    # where the cross-language tags are declared, and a type that is not in it
    # is not one a flow can name.
    from a11.data import serial_tags

    registry = get_global_serialization_registry()
    tags = sorted({
        value
        for name, value in vars(serial_tags).items()
        if not name.startswith("_")
        and isinstance(value, str)
        and value.startswith("a11.")
    })
    described: list[dict[str, object]] = []
    for tag in tags:
        model = registry.resolve_type(tag)
        if model is None:
            continue
        entry: dict[str, object] = {"tag": tag}
        summary = inspect.getdoc(model)
        if summary:
            entry["description"] = summary.split("\n")[0]
        fields = _fields_of(model)
        if fields:
            entry["fields"] = fields
        described.append(entry)
    return described


def build() -> dict[str, object]:
    return {
        "format": "flow.catalogue/v1",
        "actions": _registered_actions(),
        "types": _registered_types(),
    }


def header_for(text: str) -> str:
    """The snapshot as a C++ header holding one raw string literal."""
    # A raw string with a delimiter the payload cannot contain, so no escaping
    # is needed and the file stays readable as the JSON it is.
    return (
        "// Copyright 2026 The A11 Authors.\n"
        "//\n"
        "// GENERATED by scripts/generate_flow_catalogue.py. Do not edit.\n"
        "//\n"
        "// The snapshot of the world the standalone tools know with nothing\n"
        "// configured: every action the SDK registers and every type its\n"
        "// serialization registry knows. A frontend with a live registry\n"
        "// passes its own catalogue, which is merged over this.\n"
        "\n"
        "#ifndef A11_FLOW_CATALOGUE_DATA_H_\n"
        "#define A11_FLOW_CATALOGUE_DATA_H_\n"
        "\n"
        "#include <string_view>\n"
        "\n"
        "namespace a11::flow::catalogue {\n"
        "\n"
        'inline constexpr std::string_view kCatalogueSnapshot = R"catalogue(\n'
        f"{text}"
        ')catalogue";\n'
        "\n"
        "}  // namespace a11::flow::catalogue\n"
        "\n"
        "#endif  // A11_FLOW_CATALOGUE_DATA_H_\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="report whether the checked-in files are what this would write",
    )
    arguments = parser.parse_args()

    catalogue = json.dumps(build(), indent=2, sort_keys=True) + "\n"
    header = header_for(catalogue)

    wanted = {CATALOGUE_JSON: catalogue, CATALOGUE_HEADER: header}
    stale = [
        path
        for path, text in wanted.items()
        if not path.exists() or path.read_text(encoding="utf-8") != text
    ]
    if arguments.check:
        for path in stale:
            print(
                f"{path.relative_to(ROOT)}: out of date -- run "
                "scripts/generate_flow_catalogue.py",
                file=sys.stderr,
            )
        return 1 if stale else 0

    for path, text in wanted.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        state = "generated" if path in stale else "unchanged"
        print(f"{path.relative_to(ROOT)}: {state}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

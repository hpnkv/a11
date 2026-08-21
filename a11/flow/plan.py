# Copyright 2026 The A11 Authors.

"""The compiled program a Flow file becomes, as Python holds it.

The language is implemented once, in C++ (`cpp/a11/flow/`), and this is the
Python frontend onto it: [Program][a11.flow.plan.Program] and
[FlowPlan][a11.flow.plan.FlowPlan] *are* the classes the native extension
exports, with the conveniences a Python caller expects attached onto them --
mapping access, registration, and running one. There is no shadow model here and
no second resolver: a flow's ports, steps and diagnostics all come from the one
implementation, so what `a11 flow check` says and what `flow.loads` raises
cannot disagree.

Two rules of the compiled graph are worth knowing, because they are what makes
it predictable:

* **Steps run concurrently.** Order comes from the data, not from the order the
  statements were written in. A call is dispatched at once and its inputs stream
  in while it works. Where an order is genuinely needed, `after`, `wait` and
  `drain` say so.
* **A stream read inside a loop or branch is materialised.** The runtime
  buffers it once, in the scope that owns it, and replays the buffer to each
  reader. That is what lets every pass of a loop see the same outer value, and
  it is the one place the language trades streaming for repeatability.

A flow's *shape* is readable as plain data with
[FlowPlan.describe][a11.flow.plan.FlowPlan.describe], which is the same
``flow.plan/v1`` payload `a11 flow describe` prints.
"""

from __future__ import annotations

import json
from collections.abc import Iterator, Mapping
from typing import TYPE_CHECKING, Any

from a11._native import flow as _flow
from a11._native_protocol import attach_protocol
from a11.flow.diagnostics import FlowSyntaxError

from a11._native.flow import FlowPlan, Program

if TYPE_CHECKING:
    # What the protocols below say they hand back. The names are the native
    # ones, which is also how they read in the generated stub; importing them
    # for real here would be a cycle -- `a11.actions.action` is what builds the
    # schema a flow presents -- so the methods that need a class at runtime
    # still import it where they use it.
    from a11._native import (
        ActionHandler,
        ActionHeaderSchema,
        ActionPortSchema,
        ActionRegistry,
        ActionSchema,
        NativeActionHandler,
    )
    from a11.actions.action import Action

#: Friendly type names, and the Python type each gives a port. The type drives
#: the JSON schema A11 shows an LLM, so a flow's ports describe themselves as
#: well as a hand-written action's do.
#:
#: The *host's* half of the table, which is why it is here: the language knows
#: the names (`cpp/a11/flow/vocabulary.cc`) and what each one is called in an
#: action schema (`FlowSchema` in `cpp/a11/flow/runtime.cc`); only Python knows
#: which Python type that is.
TYPE_NAMES: dict[str, type | str] = {
    "string": str,
    "text": str,
    "number": float,
    "integer": int,
    "int": int,
    "bool": bool,
    "boolean": bool,
    "object": dict,
    "json": dict,
    "list": list,
    "array": list,
    "bytes": bytes,
    # A11's own instant and length, which the language has always had values of
    # and now has a port type for. Named by their serialisation tags rather than
    # by the classes: the tag is what a registry knows them by, and it is what
    # `FlowSchema` puts in an action schema for them.
    "time": "a11.Time",
    "duration": "a11.Duration",
    "any": "application/json",
}

_native_describe = FlowPlan.describe

#: The Python type each Flow field type gives a generated model's field.
#:
#: Only the scalars: a list says what it holds and a shape is another model, and
#: both are worked out from the plan rather than looked up.
_FIELD_TYPES: dict[str, Any] = {
    "string": str,
    "text": str,
    "number": float,
    "integer": int,
    "int": int,
    "bool": bool,
    "boolean": bool,
    "bytes": bytes,
    "object": dict,
    "json": Any,
    "any": Any,
    "list": list,
    "array": list,
}

#: Models built from shapes, by the plan they were built from.
#:
#: A stream of ten thousand records is one class, not ten thousand. Keyed on the
#: plan's own JSON because that is exactly what the model depends on: two flows
#: declaring the same shape share a model, and a shape that changed is a
#: different key rather than a stale hit.
_MODELS: dict[str, Any] = {}


def _field_type(described: Mapping[str, Any], shapes: Mapping[str, Any]) -> Any:
    """The annotation one field of a shape gets."""
    named = described.get("struct")
    if named:
        return _model_from_plans(named, shapes)
    resolved = described.get("resolved", "json")
    if resolved in ("list", "array"):
        element_struct = described.get("element_struct")
        if element_struct:
            return list[_model_from_plans(element_struct, shapes)]  # type: ignore[misc]
        element = described.get("element")
        if element:
            return list[_FIELD_TYPES.get(element, Any)]  # type: ignore[misc]
        return list
    if resolved == "time":
        from a11 import Time

        return Time
    if resolved == "duration":
        from a11 import Duration

        return Duration
    return _FIELD_TYPES.get(resolved, Any)


def _model_from_plans(name: str, shapes: Mapping[str, Any]) -> Any:
    """The model for the shape `name`, building the ones it names first.

    A shape may name itself, so the model goes into the cache *before* its
    fields are built and the annotations are resolved afterwards -- which is the
    same dance a hand-written recursive model does with a forward reference.
    """
    import pydantic

    described = shapes[name]
    key = json.dumps(described, sort_keys=True)
    cached = _MODELS.get(key)
    if cached is not None:
        return cached

    model = pydantic.create_model(name, __doc__=described.get("description") or None)
    _MODELS[key] = model

    fields = described.get("fields", {})
    for field_name in described.get("order", list(fields)):
        field = fields[field_name]
        annotation = _field_type(field, shapes)
        # The model reflects the native plan; optional fields accept None.
        if not field.get("required", False):
            annotation = annotation | None if annotation is not Any else Any
            default = field.get("default", None)
        else:
            default = field.get("default", ...)
        model.model_fields[field_name] = pydantic.fields.FieldInfo(
            annotation=annotation,
            default=default,
            description=field.get("description") or None,
        )
    model.model_rebuild(force=True)
    return model


def _model_for_dto(described_json: str) -> Any:
    """The pydantic model one shape describes, or ``None`` without pydantic.

    Called from the native bridge's ``Adopt``: a value coerced to a `struct` comes
    out of a flow as an instance of a real model, so Python code reading a
    `struct`-typed port gets attribute access, ``model_dump()`` and everything else
    a model gives -- rather than a mapping that merely has the right keys.

    ``None`` where pydantic is not installed, which leaves the record as the
    plain mapping the language built. A flow should run either way.
    """
    try:
        import pydantic  # noqa: F401
    except ImportError:
        return None
    described = json.loads(described_json)
    # The shape itself, and every shape it names, are all in the one plan the
    # bridge handed over -- so a nested model is built from the same data rather
    # than from a second trip across the boundary.
    shapes = {described["struct"]: described}
    for nested in described.get("nested", []):
        shapes[nested["struct"]] = nested
    return _model_from_plans(described["struct"], shapes)


def _port_type(declared: str) -> type | str:
    """What a declared type gives a port, on this side of the boundary.

    A built-in name gives the Python type behind it, whatever parameters it was
    written with -- ``list[string]`` is a list. A mimetype and a serialisation
    tag are carried through as they were written: the tag is the name the
    registries know the type by, and the module defining it may not even be
    imported at the time the flow is compiled.
    """
    name = declared.split("[", 1)[0].strip().strip('"')
    return TYPE_NAMES.get(name.lower() if name.isupper() else name, name)


def _port_spec(name: str, described: Mapping[str, Any]) -> dict[str, Any]:
    spec: dict[str, Any] = {
        "name": name,
        "type": _port_type(described["type"]),
        "unary": described["unary"],
        "required": described["required"],
    }
    if described.get("description"):
        spec["description"] = described["description"]
    return spec


class _FlowPlanProtocol:
    """What the native ``FlowPlan`` looks like from Python."""

    @property
    def schema(self) -> ActionSchema:
        """The [ActionSchema][a11.actions.action.ActionSchema] a flow presents.

        A flow is an action: it has ports, headers and a name, so anything that
        can dispatch an action can dispatch a composition without being told it
        is one.

        Built through the Python validator rather than taken from the native
        schema, because a port's ``typeinfo`` -- the Python type its JSON schema
        comes from, and so what a model is shown -- is something only this side
        can supply.
        """
        from a11.actions.action import ActionSchema

        described = _native_describe(self)
        return ActionSchema.model_validate({
            "name": described["flow"],
            "description": described["description"],
            "inputs": {
                name: _port_spec(name, port)
                for name, port in described["inputs"].items()
            },
            "outputs": {
                name: _port_spec(name, port)
                for name, port in described["outputs"].items()
            },
            "headers": {name: {"name": name} for name in described["headers"]},
        })

    @property
    def inputs(self) -> Mapping[str, ActionPortSchema]:
        """The declared input ports, by name, as the action schema has them."""
        return self.schema.inputs

    @property
    def outputs(self) -> Mapping[str, ActionPortSchema]:
        """The declared output ports, by name, as the action schema has them."""
        return self.schema.outputs

    @property
    def headers(self) -> Mapping[str, ActionHeaderSchema]:
        """The declared headers, by name."""
        return self.schema.headers

    @property
    def handler(self) -> ActionHandler | NativeActionHandler | None:
        """The action handler that runs this flow."""
        return self.make_handler()

    def register(
        self: FlowPlan, registry: ActionRegistry, name: str | None = None
    ) -> FlowPlan:
        """Register this flow as an action in ``registry``.

        After this the composition is an action like any other: a session
        dispatches it, another flow calls it, and a model can be offered it as a
        tool, without any of them knowing it is a composition.
        """
        registry.register(name or self.name, self.schema, self.handler)
        return self

    def action(self, **kwargs: Any) -> Action:
        """Build a standalone [Action][a11.actions.action.Action] for it."""
        from a11.actions.action import Action

        return Action(self.schema, handler=self.handler, **kwargs)

    async def invoke(
        self, inputs: Mapping[str, Any] | None = None, **kwargs: Any
    ) -> dict[str, Any]:
        """Run the flow once, here, and collect its outputs.

        See [a11.flow.runtime.invoke][] for what the keywords mean.
        """
        from a11.flow.runtime import invoke

        return await invoke(self, inputs, **kwargs)


class _ProgramProtocol:
    """What the native ``Program`` is from Python: a mapping of flows."""

    def __getitem__(self, name: str) -> FlowPlan:
        found = self.get(name)
        if found is None:
            known = ", ".join(sorted(self.names)) or "none"
            raise KeyError(
                f"No flow named {name!r} in "
                f"{self.source_name or 'this program'} (declared: {known})."
            )
        return found

    def __contains__(self, name: object) -> bool:
        return isinstance(name, str) and self.get(name) is not None

    def __iter__(self) -> Iterator[FlowPlan]:
        return iter(self[name] for name in self.names)

    def __len__(self) -> int:
        return len(self.names)

    @property
    def flows(self) -> dict[str, FlowPlan]:
        """Every flow, by name, in declaration order."""
        return {name: self[name] for name in self.names}

    @property
    def main(self) -> FlowPlan:
        """The first flow declared, which is the one a file is usually about."""
        return self[self.names[0]]

    def register_all(self: Program, registry: ActionRegistry) -> Program:
        """Register every flow in ``registry``."""
        for name in self.names:
            self[name].register(registry)
        return self


attach_protocol(FlowPlan, _FlowPlanProtocol)
attach_protocol(Program, _ProgramProtocol)
FlowPlan.__module__ = __name__
Program.__module__ = __name__


def compile_source(source: str, source_name: str = "") -> Program:
    """Compile Flow source into a [Program][a11.flow.plan.Program].

    Raises:
        FlowSyntaxError: On any lexical, grammatical or naming problem, with the
            line and column it was found at.
    """
    try:
        return _flow.compile(source, source_name)
    except Exception as error:  # noqa: BLE001 - re-raised with its position
        raise _syntax_error(source, source_name, error) from None


def _syntax_error(
    source: str, source_name: str, raised: BaseException
) -> FlowSyntaxError:
    """The first error in ``source``, as the exception a compiler raises.

    Uses the diagnostic matching the compiler error when several problems share
    a source position.
    """
    errors = [
        found
        for found in _flow.check(source, source_name)["diagnostics"]
        if found["severity"] == "error"
    ]
    if not errors:
        return FlowSyntaxError(str(raised), 1, 1, source_name)
    text = str(raised)
    chosen = next(
        (found for found in errors if found["message"] in text), errors[0]
    )
    start = chosen["range"]["start"]
    return FlowSyntaxError(
        chosen["message"], start["line"], start["column"], source_name
    )


__all__ = [
    "TYPE_NAMES",
    "FlowPlan",
    "Program",
    "compile_source",
]

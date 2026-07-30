# Copyright 2026 The A11 Authors.

from typing import Any

import a11

from a11.sdk.llm_tools import jsonschema_utils


class ToolAdapter:
    def __init__(self, schema: a11.ActionSchema):
        self._schema = schema

    @property
    def input_schema(self):
        return self._get_input_json_schema()

    def _get_input_json_schema(self):
        properties = dict()
        required_nodes = []

        for input_node in self._schema.inputs.values():
            # Autofilled inputs are supplied automatically before the handler
            # runs, so the LLM must never see them in the tool definition.
            if input_node.autofills:
                continue

            name = input_node.name
            unary = input_node.unary
            required = input_node.required

            node_schema = {"type": "object"}
            if input_node.typeinfo is not None:
                node_schema = jsonschema_utils.get_json_schema_type(
                    input_node.typeinfo
                )

            if required:
                required_nodes.append(name)

            if not unary:
                node_schema: dict[str, Any] = {
                    "type": "array",
                    "items": node_schema,
                }
                if required:
                    node_schema["minItems"] = 1

            properties[name] = node_schema

        return jsonschema_utils.organise_and_deduplicate_jsonschema(
            {
                "type": "object",
                "properties": properties,
                "required": required_nodes,
            }
        )

    @property
    def output_schema(self):
        return self._get_output_json_schema()

    def _get_output_json_schema(self):
        properties = dict()
        required_nodes = []

        for output_node in self._schema.outputs.values():
            name = output_node.name
            unary = output_node.unary
            required = output_node.required

            node_schema = {"type": "object"}
            if output_node.typeinfo is not None:
                node_schema = jsonschema_utils.get_json_schema_type(
                    output_node.typeinfo
                )

            if required:
                required_nodes.append(name)

            if not unary:
                node_schema: dict[str, Any] = {
                    "type": "array",
                    "items": node_schema,
                }
                node_schema["minItems"] = 1

            properties[name] = node_schema

        substitutions = self._schema.output_to_json_field
        if not substitutions:
            schema = {
                "type": "object",
                "properties": properties,
                "required": required_nodes,
            }
        elif len(substitutions) == 1 and list(substitutions.values())[0] == "$":
            schema = properties[list(substitutions.keys())[0]]
        else:
            for name, substitution in substitutions.items():
                properties[substitution] = properties.pop(name)
            schema = {
                "type": "object",
                "properties": properties,
                "required": required_nodes,
            }

        return jsonschema_utils.organise_and_deduplicate_jsonschema(schema)

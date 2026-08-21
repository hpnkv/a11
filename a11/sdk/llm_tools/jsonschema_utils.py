# Copyright 2026 The A11 Authors.

"""Re-export of [a11.actions.jsonschema][a11.actions.jsonschema].

The module moved down into the actions layer, because deriving a port's
`json_schema` is part of describing an action and the describer cannot import an
SDK. This name stays for the tool adapters that were its first callers.
"""

from __future__ import annotations

from a11.actions.jsonschema import (
    get_json_schema_type,
    organise_and_deduplicate_jsonschema,
)

__all__ = ["get_json_schema_type", "organise_and_deduplicate_jsonschema"]

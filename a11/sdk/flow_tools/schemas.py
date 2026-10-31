# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Schemas for the three Flow Actions, kept separate from the handlers.

Execution is the default; discovery and compile-only checks are optional:

* ``flow_actions`` says what may be composed, and -- the part a tool definition
  cannot carry -- what each action's *output* ports are called;
* ``flow_check`` compiles a composition without running it;
* ``flow_run`` runs one and hands back its outputs.

Each returns its whole result as one JSON value
([WHOLE_JSON_OUTPUT][a11.sdk.llm_tools.adapter.WHOLE_JSON_OUTPUT]), because
what a model wants back is the object, not an envelope around it.

``flow_run`` serves a second kind of caller as well, and the difference is only
in how a port is filled. A model sends an object of values and reads the
collected ``result``; a client with a session of its own names its ports on
``input_streams`` and writes them as nodes while the flow runs, reading the
outputs the same way (see
[flow_input_node_id][a11.sdk.flow_tools.handlers.flow_input_node_id]). Neither
mechanism cares how many values a port carries.
"""

from __future__ import annotations

import json

import a11
from a11.sdk.llm_tools.adapter import WHOLE_JSON_OUTPUT

FLOW_ACTIONS_SCHEMA = a11.ActionSchema(
    name="flow_actions",
    description=(
        "Optionally discover missing contracts for actions a flow may call."
        " Pass names to request only the actions needed for the next step;"
        " omit names to list all permitted composable actions. Returns input"
        " and output ports plus runnable flags. Reuse known contracts in"
        " flow_run without this lookup."
    ),
    inputs={
        "names": a11.ActionPortSchema(
            "names",
            "application/json",
            description="Optional list of up to 12 unique action names.",
            typeinfo=list,
            unary=True,
            json_schema=json.dumps(
                {
                    "type": "array",
                    "items": {"type": "string"},
                    "maxItems": 12,
                    "uniqueItems": True,
                }
            ),
        ),
    },
    outputs={
        "actions": a11.ActionPortSchema(
            "actions",
            "application/json",
            description=(
                "One entry per composable action: its name, description, and"
                " its input and output ports."
            ),
            typeinfo=list,
            unary=True,
            required=True,
        ),
    },
    output_to_json_field={"actions": WHOLE_JSON_OUTPUT},
)


FLOW_CHECK_SCHEMA = a11.ActionSchema(
    name="flow_check",
    description=(
        "Optionally compile a Flow without dispatching actions. Returns the"
        " composition or a diagnostic with its location. Use this when"
        " compiler feedback would help; flow_run also compiles before running."
    ),
    inputs={
        "source": a11.ActionPortSchema(
            "source",
            "text/plain",
            description="The text of one or more flow declarations.",
            typeinfo=str,
            unary=True,
            required=True,
        ),
    },
    outputs={
        "plan": a11.ActionPortSchema(
            "plan",
            "application/json",
            description="The compiled composition, as data.",
            typeinfo=dict,
            unary=True,
            required=True,
        ),
    },
    output_to_json_field={"plan": WHOLE_JSON_OUTPUT},
)


FLOW_RUN_SCHEMA = a11.ActionSchema(
    name="flow_run",
    description=(
        "Default execution tool for almost every tool operation, including a"
        " single read, search, edit, command, or request. Invoke underlying"
        " tools inside the Flow. Prefer composing specialized non-shell tools"
        " and Flow stages whenever they can solve the task. Combine"
        " independent work and known chains;"
        " use another Flow after a new decision. Compiles, checks permissions,"
        " and executes permitted actions."
        " Proactively limit sources, filter and project results, and combine"
        " item caps with text truncation on returned evidence. Keep full"
        " intermediates needed by downstream actions inside the Flow."
        " Returns declared outputs as an object keyed by port name. Discovery"
        " and a separate flow_check call are optional."
    ),
    inputs={
        "source": a11.ActionPortSchema(
            "source",
            "text/plain",
            description=(
                "Raw, unfenced Flow source. A one-action Flow is enough; use"
                " exact action and port names from known contracts and return"
                " only bounded evidence needed for the next decision."
            ),
            typeinfo=str,
            unary=True,
            required=True,
        ),
        "inputs": a11.ActionPortSchema(
            "inputs",
            "application/json",
            description=(
                "Object keyed by input port name. Supply one value for a unary"
                " port and a list of items for a stream port. Omitted ports"
                " are closed without values."
            ),
            typeinfo=dict,
            unary=True,
            required=False,
        ),
        "input_streams": a11.ActionPortSchema(
            "input_streams",
            "application/json",
            description=(
                "Input ports you will fill yourself, as nodes, while the flow"
                " runs -- named here because otherwise a port you do not send a"
                " value for is closed empty. Write each one at"
                " `<this call's id>-flow#<port name>` in the session's node map"
                " (`flow_input_node_id` computes it) and close it when you are"
                " done: nothing else will, and a flow reading a port nobody"
                " closes waits. This is how a value reaches a running flow"
                " rather than being decided before it starts, and how a port"
                " that carries a real type is fed at all, since a value in"
                " `inputs` arrives as plain JSON. A caller with no node map of"
                " its own -- a model calling this as a tool -- wants `inputs`."
            ),
            typeinfo=list,
            unary=True,
            required=False,
        ),
        "flow": a11.ActionPortSchema(
            "flow",
            "text/plain",
            description=(
                "Which flow in the source to run. Defaults to the first one"
                " declared, which is the one a file is usually about."
            ),
            typeinfo=str,
            unary=True,
            required=False,
        ),
    },
    outputs={
        "result": a11.ActionPortSchema(
            "result",
            "application/json",
            description=(
                "The flow's outputs, keyed by port name: one value for an"
                " ordinary port, a list for a `stream` one."
            ),
            typeinfo=dict,
            unary=True,
            required=True,
        ),
    },
    output_to_json_field={"result": WHOLE_JSON_OUTPUT},
)


#: The names of the three tools, which a flow may never call itself.
FLOW_TOOL_NAMES = frozenset(
    {
        FLOW_ACTIONS_SCHEMA.name,
        FLOW_CHECK_SCHEMA.name,
        FLOW_RUN_SCHEMA.name,
    }
)


__all__ = [
    "FLOW_ACTIONS_SCHEMA",
    "FLOW_CHECK_SCHEMA",
    "FLOW_RUN_SCHEMA",
    "FLOW_TOOL_NAMES",
]

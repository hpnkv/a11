# Copyright 2026 The A11 Authors.

"""Schemas for the three Flow Actions, kept separate from the handlers.

The three form a small protocol an LLM drives as tools, in the order it needs
them:

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

import a11
from a11.sdk.llm import USER_FACING_LOG_PORT
from a11.sdk.llm_tools.adapter import WHOLE_JSON_OUTPUT


def _user_facing_log_port() -> a11.ActionPortSchema:
    """The port each Action narrates its run on, for the user's eyes.

    The same contract the shell tools use: a UI showing the call reads this,
    and the LLM tool runner keeps it out of the result the model is given. See
    [USER_FACING_LOG_PORT][a11.sdk.llm.USER_FACING_LOG_PORT].
    """
    return a11.ActionPortSchema(
        USER_FACING_LOG_PORT,
        "text/plain",
        description=(
            "Narration of this call for the person watching: first line a"
            " one-sentence summary, the rest markdown detail. Not part of the"
            " tool result."
        ),
        required=False,
    )


FLOW_ACTIONS_SCHEMA = a11.ActionSchema(
    name="flow_actions",
    description=(
        "List the actions a flow may call, with the name, arity and"
        " description of every input and output port each one has. Call this"
        " before writing a flow: piping one action into another needs the"
        " names of the ports on both sides, and an action's output ports are"
        " not part of the tool definition you were given. The list is the"
        " actions you are allowed to call, minus the flow tools themselves."
    ),
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
        USER_FACING_LOG_PORT: _user_facing_log_port(),
    },
    output_to_json_field={"actions": WHOLE_JSON_OUTPUT},
)


FLOW_CHECK_SCHEMA = a11.ActionSchema(
    name="flow_check",
    description=(
        "Compile a flow without running it, and return the composition it"
        " resolves to: its ports, and every step in the order they are"
        " written. Nothing is dispatched and no action is called, so this is"
        " the safe way to find out whether a flow says what you meant before"
        " its calls have real effects. A flow that will not compile comes back"
        " as an error naming the line and column of the problem."
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
        USER_FACING_LOG_PORT: _user_facing_log_port(),
    },
    output_to_json_field={"plan": WHOLE_JSON_OUTPUT},
)


FLOW_RUN_SCHEMA = a11.ActionSchema(
    name="flow_run",
    description=(
        "Run a flow: a composition of the actions you may call, written in the"
        " A11 Flow language, dispatched here as one step. The actions it names"
        " really run, and their intermediate values move between them without"
        " passing through you -- which is the point, and the reason to reach"
        " for a flow when the data between two tools is large or when the same"
        " shape of work repeats. What comes back is the flow's declared"
        " outputs, one object keyed by port name, so declare outputs that are"
        " small enough to read. A flow may only call actions you are allowed"
        " to call; one that names another is refused before anything runs."
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
        "inputs": a11.ActionPortSchema(
            "inputs",
            "application/json",
            description=(
                "Values for the flow's input ports, keyed by port name: the list"
                " of values to write to that port, or a bare value as shorthand"
                " for a list of one. How many a port takes is the flow's"
                " business, not this port's -- a port declared `stream` reads"
                " every value, an ordinary one reads the first, and a port left"
                " out here carries none."
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
        USER_FACING_LOG_PORT: _user_facing_log_port(),
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

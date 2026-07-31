import a11

from a11.sdk.llm import Interaction, LlmHeaders

INTERACT_WITH_LLM_SCHEMA = a11.ActionSchema(
    name="interact_with_llm",
    inputs={
        "interactions": a11.ActionPortSchema(
            "interactions",
            "application/json",
            typeinfo=Interaction,
            required=True,
        ),
        "tools": a11.ActionPortSchema(
            "tools",
            "application/json",
            typeinfo=dict,
            required=False,
        ),
        "config": a11.ActionPortSchema(
            "config",
            "application/json",
            unary=True,
            required=True,
        ),
    },
    outputs={
        "event_stream": a11.ActionPortSchema(
            "event_stream",
            "application/json",
            typeinfo=dict,
            required=False,
        ),
        "thoughts": a11.ActionPortSchema(
            "thoughts",
            "text/plain",
            required=False,
        ),
        "text_output": a11.ActionPortSchema(
            "text_output",
            "text/plain",
            required=False,
        ),
        "new_interactions": a11.ActionPortSchema(
            "new_interactions",
            "application/json",
            typeinfo=Interaction,
            required=True,
        ),
    },
    headers=a11.DEFAULT_ACTION_HEADERS
    | {
        LlmHeaders.API_KEY: a11.ActionHeaderSchema(
            LlmHeaders.API_KEY, "API key for the downstream provider."
        ),
        LlmHeaders.PROVIDER: a11.ActionHeaderSchema(
            LlmHeaders.PROVIDER, "The downstream provider."
        ),
        LlmHeaders.MODEL: a11.ActionHeaderSchema(
            LlmHeaders.MODEL, "The downstream model."
        ),
        LlmHeaders.ALLOWED_LLM_ACTIONS: a11.ActionHeaderSchema(
            LlmHeaders.ALLOWED_LLM_ACTIONS,
            "The allowed downstream LLM action (tool) name patterns,"
            " comma-separated.",
        ),
    },
)

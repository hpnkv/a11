import a11
from a11.actions import ActionPortSchema, ActionSchema
from a11.data import types
from a11.sdk.llm import action_name_matches_allowed
from a11.sdk.llm_tools.adapter import ToolAdapter


def _autofilled_schema() -> ActionSchema:
    return ActionSchema(
        name="tool-with-autofill",
        inputs={
            "visible": ActionPortSchema(
                name="visible", type="application/json", typeinfo=str
            ),
            "hidden": ActionPortSchema(
                name="hidden",
                type="text/plain",
                autofills=[
                    types.NodeFragment(
                        data=a11.to_chunk("secret"), continued=False
                    )
                ],
            ),
        },
    )


def test_tool_adapter_hides_autofilled_inputs_from_the_llm():
    adapter = ToolAdapter(_autofilled_schema())
    properties = adapter.input_schema["properties"]

    assert "visible" in properties
    assert "hidden" not in properties


def test_allowed_llm_actions_match_regex_patterns():
    patterns = ["get_.*", "list_users"]

    assert action_name_matches_allowed("get_weather", patterns)
    assert action_name_matches_allowed("list_users", patterns)
    assert not action_name_matches_allowed("delete_everything", patterns)
    # A plain name is still matched exactly, preserving the old behaviour.
    assert action_name_matches_allowed("list_users", ["list_users"])
    assert not action_name_matches_allowed("list_users_v2", ["list_users"])

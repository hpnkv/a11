"""An action this workspace declares, for the discovery half of the test."""

import a11

FIXTURE = a11.ActionSchema(
    name="fixture_action",
    description="An action declared in the test workspace.",
    inputs={"text": a11.ActionPortSchema(name="text", type="text/plain", required=True)},
    outputs={"out": a11.ActionPortSchema(name="out", type="text/plain")},
)

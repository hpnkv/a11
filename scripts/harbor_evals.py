from absl import app
from absl import logging
import a11

import asyncio
from typing import Sequence

from a11 import logging as a11_logging
from a11.client import open_gateway
from a11.sdk import bash as shell_actions
from a11.sdk import interact_with_llm
from a11.sdk import interact_with_llm_schema
from a11.sdk import llm
from harbor.agents.base import BaseAgent, BaseEnvironment, AgentContext


async def _drain(node: a11.AsyncNode) -> None:
    async for _ in node:
        pass


class A11Agent(BaseAgent):
    _action_registry: a11.ActionRegistry
    _allowed_action_patterns: list[str]

    @staticmethod
    def name() -> str:
        return "A11"

    def version(self) -> str | None:
        return "0.5.0"

    async def setup(self, environment: BaseEnvironment) -> None:
        action_registry = a11.ActionRegistry()
        shell_actions.register(action_registry)
        self._action_registry = action_registry

        self._allowed_action_patterns = ["shell_.*"]

    async def run(
        self,
        instruction: str,
        environment: BaseEnvironment,
        context: AgentContext,
    ) -> None:
        """
        Runs the agent in the environment. Be sure to populate the context
        with the results of the agent execution. Ideally, populate the context
        as the agent executes in case of a timeout or other error.
        Args:
            instruction: The task instruction.
            environment: The environment in which to complete the task.
            context: The context to populate with the results of the agent
                execution.
        """

        interact = (
            a11
            .Action(interact_with_llm_schema.INTERACT_WITH_LLM_SCHEMA)
            .bind_registry(self._action_registry)
            .bind_handler(interact_with_llm.interact_with_llm)
            .set_header(llm.LlmHeaders.PROVIDER.value, "ollama")
            .set_header(llm.LlmHeaders.MODEL.value, "glm-4.7-flash")
            .set_header(
                llm.LlmHeaders.BASE_URL.value, "http://192.168.1.209:11434"
            )
            .set_header(
                llm.LlmHeaders.ALLOWED_LLM_ACTIONS.value,
                ",".join(self._allowed_action_patterns),
            )
        ).run()

        await asyncio.gather(
            _drain(interact["thoughts"]),
            _drain(interact["text_output"]),
            _drain(interact["new_interactions"]),
            interact["config"].finalize(),
            interact["tools"].finalize(),
            interact["interactions"].finalize(
                llm.Interaction(
                    role=llm.Role.USER,
                    system_instructions=[
                        a11.to_chunk("You are a helpful assistant.")
                    ],
                    content=[a11.to_chunk(instruction)],
                )
            ),
        )
        await interact.wait()


async def play(
    registry: a11.ActionRegistry,
    allowed_action_patterns: list[str] | None = None,
):
    registry = registry or a11.ActionRegistry()
    allowed_action_patterns = allowed_action_patterns or []

    async with (
        open_gateway(
            "ws://127.0.0.1:8011/a11", registry=registry
        ) as connection,
        asyncio.TaskGroup() as tg,
    ):
        interact = (
            a11
            .Action(interact_with_llm_schema.INTERACT_WITH_LLM_SCHEMA)
            .bind_node_map(connection.session.node_map)
            .bind_session(connection.session)
            .bind_stream(connection.stream)
            .bind_registry(connection.session.action_registry)
            .set_header(llm.LlmHeaders.PROVIDER.value, "ollama")
            .set_header(llm.LlmHeaders.MODEL.value, "glm-4.7-flash")
            .set_header(
                llm.LlmHeaders.BASE_URL.value, "http://192.168.1.209:11434"
            )
            .set_header(
                llm.LlmHeaders.ALLOWED_LLM_ACTIONS.value,
                ",".join(allowed_action_patterns),
            )
        )

        await interact.call()
        await asyncio.gather(
            interact["config"].finalize(),
            interact["tools"].finalize(),
            interact["interactions"].finalize(
                llm.Interaction(
                    role=llm.Role.USER,
                    system_instructions=[
                        a11.to_chunk("You are a helpful assistant.")
                    ],
                    content=[
                        a11.to_chunk("Hello! What is my operating system?")
                    ],
                ),
                wait=True,
            ),
        )

        tg.create_task(_drain(interact["new_interactions"]))

        async for thought_piece in interact["thoughts"]:
            print(thought_piece, end="")
        print()

        async for output_piece in interact["text_output"]:
            print(output_piece, end="")
        print()

        await interact.wait()


async def main(_: Sequence[str]):
    action_registry = a11.ActionRegistry()
    shell_actions.register(action_registry)

    allowed_action_patterns = ["shell_.*"]

    await play(action_registry, allowed_action_patterns)


def sync_main(argv: Sequence[str]):
    # a11_logging.enable(logging.DEBUG)
    asyncio.run(main(argv))


if __name__ == "__main__":
    app.run(sync_main)

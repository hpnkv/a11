import asyncio
from typing import Sequence

from absl import app

import a11
from a11 import flow


async def run_echo_python(action: a11.Action):
    async for text_chunk in action["input"]:
        await action["output"].put(f"echo: {text_chunk}")

    await action["output"].finalize()


ECHO_SCHEMA = a11.ActionSchema(
    name="echo",
    inputs={"input": a11.ActionPortSchema("input", "text/plain", typeinfo=str)},
    outputs={
        "output": a11.ActionPortSchema("output", "text/plain", typeinfo=str)
    },
)


async def demo_echo_python():
    echo = a11.Action(ECHO_SCHEMA).bind_handler(run_echo_python).run()
    await echo["input"].put("Hello, ")
    await echo["input"].finalize("world!")

    async for text_chunk in echo["output"]:
        print(text_chunk, end="")


async def demo_echo_flow():
    program = flow.loads(
        """
    flow echo {
      in input: string stream required "Input text"
      out output: string stream "Output text"
      
      l1 = run log1(input: input)
      l2 = run log1(input: input)
      
      let n = wait first of l2, l1
      
      input | map strformat("%d: %s", n, it) -> output
    }
    
    flow log1 {
      in input: string required "Input"
      
      nodes scratch
      
      i = node() in scratch
      s = node() in scratch
      input -> i
      
      i | logf info "%s" it -> s
      skip s
    }
    """,
        "echo.flow",
    )
    outputs = await program["echo"].invoke({"input": ["Hello, ", "world!"]})
    print(outputs)


async def main(_: Sequence[str]):
    await demo_echo_python()
    print()
    await demo_echo_flow()


def sync_main(args: Sequence[str]):
    asyncio.run(main(args))


if __name__ == "__main__":
    app.run(sync_main)

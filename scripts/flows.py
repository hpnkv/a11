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

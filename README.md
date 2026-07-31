# A11

**A concurrent action and streaming runtime for building AI agents.**

A11 lets you write agents as ordinary `async def` code: values *stream* between
producers and consumers, work is packaged as composable *actions*, and the same
code runs in one process or across a network with a transport swap. The API is
Python; the runtime underneath is a native C++20 implementation, so the
streaming and concurrency stay fast and off the event loop's critical path.

📖 **[Documentation →](https://hpnkv.github.io/a11)**

## Install

```sh
pip install "a11-kit[llm]"
```

The `[llm]` extra pulls in the Anthropic and Google model SDKs. Drop it for the
core runtime only.

## See it in 30 seconds

Chat with a model right from the terminal (streaming its reply, and its
thoughts with `-v`):

```sh
export GEMINI_API_KEY=...        # or ANTHROPIC_API_KEY
a11 chat -v
```

## The ideas

A11 is small at its core — a few ideas compose into everything from a one-file
helper to a fleet of networked agents. (The
[Principles](https://hpnkv.github.io/a11/principles.html) page goes deeper.)

- **Everything is asynchronous.** Every operation that can wait is a coroutine
  you `await`; the runtime schedules thousands cooperatively. Completion is an
  event (`await action.done.wait()`) and lifecycles are context managers that
  finalise — or abort with the right status — for you.
- **Everything is a stream.** The unit of state is a **node**: a single ordered
  sequence of chunks with a writer end and a reader end. An agent rarely has its
  whole answer at once — it has the *next* token, frame, or tool call — so nodes
  make incremental production and consumption the natural shape, with
  backpressure built in.
- **Actions are wired streams.** An action's typed input/output **ports** are
  nodes, so calling one is wiring streams together. A handler can emit output
  before it has finished reading input — exactly what streaming an LLM response
  through a pipeline looks like.
- **Two extension points: storage and transport.** A `ChunkStore` is the log
  behind a node (swap the in-memory default for disk, a database, or fault
  injection); a `WireStream` moves bytes between peers (in-process, WebSocket,
  HTTP SSE, WebRTC). Everything above them is unchanged, so making an agent
  distributed is a transport swap, not a rewrite.
- **Sessions tie it together.** A `Session` multiplexes wire streams, dispatches
  incoming action calls against a registry, and drains and closes the connection
  cleanly.

## A taste

Produce into a node and read it back — backpressure and finalisation included:

```python
import asyncio
import a11


async def main() -> None:
    async with a11.AsyncNode.create("tokens") as node:   # seals on exit
        for word in ["A11", "streams", "everything"]:
            await node.put(word)                          # await = backpressure

    async for token in node:
        print(token)


asyncio.run(main())
```

Stream a model's reply through that same node abstraction — `interact_with_llm`
is just an action whose `text_output` port carries tokens as they arrive:

```python
import a11
from a11.sdk.interact_with_llm import INTERACT_WITH_LLM_SCHEMA, interact_with_llm
from a11.sdk.llm import LlmHeaders

interact = (
    a11.Action(INTERACT_WITH_LLM_SCHEMA)
    .bind_handler(interact_with_llm)
    .set_header(LlmHeaders.PROVIDER.value, "gemini")
    .set_header(LlmHeaders.MODEL.value, "gemini-3.5-flash")
    .run()
)

# ...then, inside your async code, feed the conversation in and stream the reply:
async for chunk in interact["text_output"]:   # tokens as the model emits them
    print(chunk, end="", flush=True)
```

The [guides](https://hpnkv.github.io/a11/guides/streaming.html) build these up
step by step — from a node, to a WebSocket echo session, to calling an action on
a remote server, to a tool-using agent.

## Learn more

- **[Documentation](https://hpnkv.github.io/a11)** — principles, guides, and the
  full Python API reference.
- **[Guides](https://hpnkv.github.io/a11/guides/streaming.html)** — hands-on
  walkthroughs from a single stream to a networked, tool-using agent.
- **Examples** — runnable programs under [`examples/`](examples/).

## Building the C++ runtime

A11's runtime is a standalone C++20 library you can build and link **without
Python**. The steps below are self-contained; for the editable Python build,
wheel matrix, testing workflow, and architecture, see
[BUILDING.md](BUILDING.md).

**1. Install the build tools and libraries.** On macOS with Homebrew:

```sh
brew install boost cmake googletest libnghttp2 ninja \
  nlohmann-json openssl@3 pkg-config uvw
```

You need a C++20 compiler, CMake ≥ 3.28, and Ninja. CMake fetches the pinned
Abseil (and libdatachannel, for WebRTC) automatically. On Linux the package
names vary; `scripts/bootstrap_wheel_deps.sh` builds the static dependencies
reproducibly (see [BUILDING.md](BUILDING.md)).

**2. Configure, build, and install to a prefix:**

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DA11_BUILD_PYTHON=OFF \
  -DA11_FETCH_MISSING_DEPS=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"

cmake --build build -j
cmake --install build
```

**3. Use it from your own CMake project.** The install exports a CMake package
named `a11` with per-component targets (`a11::service` links the whole runtime):

```cmake
find_package(a11 CONFIG REQUIRED)

add_executable(my_agent main.cc)
target_link_libraries(my_agent PRIVATE a11::service)
target_compile_features(my_agent PRIVATE cxx_std_20)
```

```cpp
#include "a11/nodes/node_map.h"

int main() {
  auto node_map = a11::nodes::NodeMap::Create();
  return node_map.ok() ? 0 : 1;
}
```

Configure your project with `-DCMAKE_PREFIX_PATH=/path/to/install` so
`find_package` locates it. The generated C++ API reference is published
[alongside the docs](https://hpnkv.github.io/a11/cpp/).

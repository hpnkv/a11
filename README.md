<p align="center">
  <img src="https://docs.a11.to/assets/a11-logo.svg"
       width="260" alt="A11">
</p>

# A11

**A streaming action runtime for AI agents, model serving, and multimodal
APIs.**

A11 gives ordinary asynchronous code a stable boundary for tools, model calls,
pipelines, and services. Its Python API runs on a native C++20 runtime, with
TypeScript, Kotlin, C++, and Flow interfaces for the boundaries that need them.

- **Named streams instead of mixed provider events.** Read model text,
  reasoning, tool activity, progress, and durable interaction state from
  separate ports.
- **The same action contract locally and remotely.** Move a handler to a GPU
  host, browser, or service without changing its inputs and outputs.
- **Deterministic runtime composition with Flow.** Check and load compositions
  at runtime with defined concurrency, draining, deadlines, cancellation, and
  error propagation.

**[Install in five minutes](#five-minute-quickstart)** ·
**[Try the live research agent][live-demo]** ·
**[Read the documentation](https://docs.a11.to/)** ·
**[Star A11](https://github.com/hpnkv/a11)**

[live-demo]: https://docs.a11.to/guides/deep-research.html#try-the-deployed-agent

## Try A11 live

The hosted [parallel research
agent](https://docs.a11.to/guides/deep-research.html#try-the-deployed-agent)
plans a topic, runs several investigations concurrently, and streams one report
to the browser. Its default Ollama backend needs no account or API key.

## Five-minute quickstart

Install the core runtime on Python 3.11 or later:

```sh
python -m venv .venv
source .venv/bin/activate
pip install a11-kit
```

Save this as `quickstart.py`. The function signature becomes an action schema,
and its asynchronous iterator becomes a named streaming output:

```python
import asyncio
from collections.abc import AsyncIterator

import a11


REGISTRY = a11.ActionRegistry()


@REGISTRY.action(name="split-words", output="words")
async def split_words(text: str) -> AsyncIterator[str]:
    """Emit the words in a line as they become available."""
    for word in text.split():
        yield word


async def main() -> None:
    action = REGISTRY.make_action("split-words")
    await action["text"].finalize("named streams arrive early")
    action.run()

    async for word in action["words"]:
        print(word)
    await action.wait()


asyncio.run(main())
```

Run it:

```sh
python quickstart.py
```

The output port streams four values and then closes successfully. The same
`REGISTRY` can be served over WebSocket, HTTP SSE, or WebRTC; callers still use
the `text` and `words` ports.

Add model providers when the application needs them. With Ollama already
running locally:

```sh
pip install "a11-kit[llm]"
a11 chat --provider ollama --no-voice --no-shell-tools
```

## Continue from a working example

- [Build a parallel research agent in
  Python](https://docs.a11.to/guides/deep-research.html).
- [Stream one model interface across Claude, Gemini, and
  Ollama](https://docs.a11.to/guides/llm.html).
- [Turn an application action into an LLM
  tool](https://docs.a11.to/guides/agent-tool.html).
- [Move a local action behind a remote
  service](https://docs.a11.to/guides/local-to-remote.html).
- [Compose allowed actions safely at
  runtime](https://docs.a11.to/guides/flow.html).

Questions and rough edges are useful project signals:
**[start a discussion](https://github.com/hpnkv/a11/discussions)**,
**[report friction](https://github.com/hpnkv/a11/issues/new?template=question.yml)**,
or **[tell us about an
integration](https://github.com/hpnkv/a11/issues/new?template=adoption.yml)**.

## Building the C++ runtime

A11's runtime is a standalone C++20 library you can build and link **without
Python**. The steps below are self-contained; for the editable Python build,
wheel matrix, testing workflow, and architecture, see
[BUILDING.md](BUILDING.md).

**1. Install the tools, then build the C++ libraries.** A11 links a pinned set
of statically-built libraries (Boost, OpenSSL, libcurl, nghttp2, hiredis,
nlohmann-json, uvw) rather than system copies; `scripts/bootstrap_wheel_deps.sh` builds them
into a per-architecture prefix. From Homebrew you install only the tools (a C++20
compiler, CMake ≥ 3.28, Ninja; Linux tool package names vary):

```sh
brew install cmake googletest ninja pkg-config

export A11_DEPS_PREFIX="$HOME/.cache/a11-deps/$(uname -m)"
export CMAKE_PREFIX_PATH="$A11_DEPS_PREFIX"
export OPENSSL_ROOT_DIR="$A11_DEPS_PREFIX"
export PKG_CONFIG_PATH="$A11_DEPS_PREFIX/lib/pkgconfig"
export MACOSX_DEPLOYMENT_TARGET=14.4   # macOS only
scripts/bootstrap_wheel_deps.sh
```

CMake still fetches the pinned Abseil (and libdatachannel, for WebRTC)
automatically. See [BUILDING.md](BUILDING.md#prerequisites) for the full rundown.

**2. Configure, build, and install to a prefix** (the exports above point CMake
at the dependency prefix):

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DA11_BUILD_PYTHON=OFF \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4 \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"

cmake --build build -j
cmake --install build
```

On macOS, pass `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.4` as shown — it must match the
value the prefix was bootstrapped with. Setting it as a cache variable here (not
only via the `MACOSX_DEPLOYMENT_TARGET` environment export, which CMake may not
pick up) is what enables the Boost.Fiber futex spinlock; a lower target compiles
Boost.Fiber without futex support and fails with
`"futex not supported on this platform"`. The flag is ignored on Linux.

**3. Use it from your own CMake project.** The install exports a CMake package
named `a11` with per-component targets (`a11::service` links the whole runtime).
Point your consumer's `CMAKE_PREFIX_PATH` at both the install prefix and the
dependency prefix from step 1, so the transitive static Boost/OpenSSL/... resolve:

```cmake
find_package(a11 CONFIG REQUIRED)

add_executable(my_agent main.cc)
target_link_libraries(my_agent PRIVATE a11::service)
target_compile_features(my_agent PRIVATE cxx_std_20)
```

```cpp
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/strings/str_split.h>

#include "a11/actions/registry.h"
#include "a11/nodes/async_node.h"

namespace {

using a11::actions::Action;
using a11::actions::ActionPortSchema;
using a11::actions::ActionRegistry;
using a11::actions::ActionSchema;
using a11::nodes::AsyncNode;

ActionSchema SplitWordsSchema() {
  return ActionSchema{
      .name = "split-words",
      .inputs = {{"text", ActionPortSchema{
                             .name = "text", .type = "text/plain"}}},
      .outputs = {{"words", ActionPortSchema{
                               .name = "words", .type = "text/plain"}}},
  };
}

absl::Status SplitWords(std::shared_ptr<Action> action) {
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> input,
                        action->GetInput("text"));
  ABSL_ASSIGN_OR_RETURN(
      std::optional<std::string> text,
      input->NextObject<std::string>().Await());
  if (!text.has_value()) {
    return absl::FailedPreconditionError("text input ended before a value");
  }

  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> output,
                        action->GetOutput("words"));
  std::vector<std::string> words =
      absl::StrSplit(*text, absl::ByAnyChar(" \t\n"), absl::SkipWhitespace());
  if (words.empty()) {
    return output->Finalize({.wait = true}).Await().status();
  }
  for (size_t index = 0; index + 1 < words.size(); ++index) {
    ABSL_RETURN_IF_ERROR(output->Put(words[index]).Await().status());
  }
  return output->Finalize(words.back(), {.wait = true}).Await().status();
}

absl::Status RunExample() {
  auto registry = std::make_shared<ActionRegistry>();
  ABSL_RETURN_IF_ERROR(registry->RegisterSync(
      "split-words", SplitWordsSchema(), SplitWords));
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<Action> action,
                        registry->MakeAction("split-words"));

  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> input,
                        action->GetInput("text"));
  ABSL_RETURN_IF_ERROR(
      input->Finalize(std::string("named streams arrive early"),
                      {.wait = true})
          .Await()
          .status());
  ABSL_RETURN_IF_ERROR(action->Run().status());

  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> output,
                        action->GetOutput("words"));
  while (true) {
    ABSL_ASSIGN_OR_RETURN(std::optional<std::string> word,
                          output->NextObject<std::string>().Await());
    if (!word.has_value()) break;
    std::cout << *word << '\n';
  }
  return action->Wait().Await().status();
}

}  // namespace

int main() {
  const absl::Status status = RunExample();
  if (!status.ok()) {
    LOG(ERROR) << status;
    return 1;
  }
  return 0;
}
```

This is the same `split-words` action as the Python quickstart: the registry
owns its schema and handler, the input carries one complete value, and the
output emits words as a stream. Reading the output before `Wait()` drains it
while the handler is running and preserves backpressure.

Configure your project with `-DCMAKE_PREFIX_PATH=/path/to/install` so
`find_package` locates it. The generated C++ API reference is published
[alongside the docs](https://docs.a11.to/cpp/).

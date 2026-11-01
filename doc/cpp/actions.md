# Build and run an action {#cpp_actions}

An `a11::actions::Action` is one schema-described operation. Its input and
output ports are `a11::nodes::AsyncNode` streams, so the same handler can return
one result, emit values as they become available, or combine several named
outputs.

This example ports the Python `shout` action. It reads one text value and
finalises one result. The schema remains useful without the handler: clients
use it to construct the same ports when execution moves to another process.

## Define the interface

An action schema names every port and its representation. `unary` describes a
port that carries one value; `required` tells callers and tooling that omitting
the port is an error.

```cpp
a11::actions::ActionSchema ShoutSchema() {
  using a11::actions::ActionPortSchema;
  return {
      .name = "shout",
      .description = "Convert one text value to uppercase.",
      .inputs = {{"text",
                  ActionPortSchema{.name = "text",
                                   .type = "text/plain",
                                   .description = "Text to convert.",
                                   .required = true,
                                   .unary = true}}},
      .outputs = {{"result",
                   ActionPortSchema{.name = "result",
                                    .type = "text/plain",
                                    .description = "Uppercase text.",
                                    .required = true,
                                    .unary = true}}},
  };
}
```

The media type describes the bytes on the node. The C++ serialization registry
already maps `std::string` to `text/plain`, so typed reads and writes use the
same representation as a remote peer.

## Implement the handler

A synchronous-looking handler can await node operations because A11 runs it on
its cooperative runtime. Return each failure as `absl::Status`; A11 completes
the action with that status and aborts unfinished outputs.

```cpp
absl::Status Shout(std::shared_ptr<a11::actions::Action> action) {
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<a11::nodes::AsyncNode> input,
                        action->GetInput("text"));
  ABSL_ASSIGN_OR_RETURN(
      std::optional<std::string> text,
      input->NextObject<std::string>().Await());
  if (!text.has_value()) {
    return absl::FailedPreconditionError("text input ended before a value");
  }

  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<a11::nodes::AsyncNode> output,
                        action->GetOutput("result"));
  return output
      ->Finalize(absl::AsciiStrToUpper(*text), {.wait = true})
      .Await()
      .status();
}
```

`Finalize(value)` writes the last value, records the logical end of the data,
and closes the writer. `wait = true` keeps the handler alive until the backing
store confirms both operations.

## Register and invoke it

The registry owns the schema and handler. `RegisterSync` adapts the
status-returning function to an asynchronous action handler.

```cpp
absl::Status RunShout() {
  auto registry = std::make_shared<a11::actions::ActionRegistry>();
  ABSL_RETURN_IF_ERROR(
      registry->RegisterSync("shout", ShoutSchema(), Shout));
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<a11::actions::Action> action,
                        registry->MakeAction("shout"));

  ABSL_RETURN_IF_ERROR(action->Run().status());
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<a11::nodes::AsyncNode> input,
                        action->GetInput("text"));
  ABSL_RETURN_IF_ERROR(
      input->Finalize(std::string("hello"), {.wait = true}).Await().status());

  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<a11::nodes::AsyncNode> output,
                        action->GetOutput("result"));
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> result,
                        output->NextObject<std::string>().Await());
  if (!result.has_value()) {
    return absl::DataLossError("shout returned no result");
  }
  std::cout << *result << '\n';
  return action->Wait().Await().status();
}
```

`Run()` is the dispatch barrier: it confirms that local execution started.
`Wait()` is the completion barrier. Read output ports before waiting when a
handler can stream enough data to fill its bounded writer; draining outputs
lets that handler continue.

The complete buildable `split-words` variant is in the repository
[README](https://github.com/hpnkv/a11#building-the-c-runtime). Continue with
[streaming nodes](@ref cpp_streaming), or move the same schema and handler to a
[remote session](@ref cpp_remote_actions).

## Publish auxiliary stream references

`Action::RegisterStream` creates an owned stream and returns a typed
`StreamReference`. The descriptor contains producer identity, parentage, a
`data::NodeRef`, a purpose, and opaque source metadata. `GetStreamSourcesNode`
discovers references produced within an action subtree. `ExposeStreamSources`
can also publish references of one purpose on a declared output port.

The runtime closes producer streams with their owning actions and waits for
active descendants before ending a requested scope. Session adapters forward
references and mirror payloads when the collector lacks direct storage access.
JSON conversion occurs through `StreamReferenceToChunk` and
`StreamReferenceFromChunk`; registration and forwarding use the native struct.

`NodeMap::Resolve` resolves a `NodeRef`. `ImportReference` shares its backing
node across local scopes. A qualified reference contains `StorageIdentity`;
`RegisterStorage` binds its logical id to a host-accessible backend and
`SetStorageResolver` installs host connection policy. Optional service access
information contains roles, endpoints, and scheme-labelled credential fields.
The host decides how to interpret those fields. Namespace resolution remains
independent of the usage payload protocol.

A collector advertises directly accessible namespace ids through framework
headers. Intermediate actions forward qualified references without relaying
usage payloads for those namespaces. Unqualified references retain session-local
resolution and transport mirroring. Reference windows use fragment sequences
and counts.

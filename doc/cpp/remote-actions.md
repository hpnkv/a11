# Move an action to another peer {#cpp_remote_actions}

`Action::Run()` invokes the bound handler in the current process.
`Action::Call()` sends the action envelope to a peer whose session resolves the
name against its registry. The schema and port I/O remain the same.

This supports a client that sends work to a process with a GPU, network egress,
or service credentials. The example uses an in-process stream pair so the
session wiring is visible without socket setup. A WebSocket, HTTP SSE, or
WebRTC listener supplies the same `a11::net::WireStream` interface.

## Separate the client and server registries

The server registers the schema with its handler. The client needs the schema
to build ports, but does not execute the handler.

```cpp
auto server_registry = std::make_shared<a11::actions::ActionRegistry>();
ABSL_RETURN_IF_ERROR(server_registry->RegisterSync(
    "shout", ShoutSchema(), Shout));

auto client_registry = std::make_shared<a11::actions::ActionRegistry>();
ABSL_RETURN_IF_ERROR(client_registry->Register("shout", ShoutSchema()));
```

`ShoutSchema()` and `Shout()` are the definitions from
[Build and run an action](@ref cpp_actions).

## Connect two sessions

A service owns the server registry and creates one session for each accepted
stream. The client owns its session and starts the other endpoint.

```cpp
ABSL_ASSIGN_OR_RETURN(std::shared_ptr<a11::service::Service> service,
                      a11::service::Service::Create(server_registry));
ABSL_ASSIGN_OR_RETURN(std::shared_ptr<a11::service::Session> client,
                      a11::service::Session::Create("desktop-client"));
ABSL_ASSIGN_OR_RETURN(a11::net::InProcessWireStream::Pair pair,
                      a11::net::InProcessWireStream::CreatePair());

ABSL_ASSIGN_OR_RETURN(
    a11::Task started,
    client->AddStream(pair.first, a11::service::StreamMode::kStart));
ABSL_ASSIGN_OR_RETURN(
    std::shared_ptr<a11::service::Session> server,
    service->StartStreamHandler(pair.second,
                                a11::service::StreamMode::kAccept));
ABSL_RETURN_IF_ERROR(started.Await().status());
```

For a socket server, pass each accepted stream to `Service::Serve`. Several
listeners can share one service, and therefore one action registry and
shutdown lifecycle.

## Dispatch and use the ports

Build the client action against the session's node map and the stream used for
dispatch. `Call()` resolves after the peer accepts the envelope; `Wait()`
resolves when the remote handler completes.

```cpp
ABSL_ASSIGN_OR_RETURN(
    std::shared_ptr<a11::actions::Action> call,
    client_registry->MakeAction("shout", /*action_id=*/"",
                                client->GetNodeMap(), pair.first, client));
ABSL_RETURN_IF_ERROR(call->Call().Await().status());

ABSL_ASSIGN_OR_RETURN(std::shared_ptr<a11::nodes::AsyncNode> input,
                      call->GetInput("text"));
ABSL_RETURN_IF_ERROR(
    input->Finalize(std::string("hello"), {.wait = true}).Await().status());

ABSL_ASSIGN_OR_RETURN(std::shared_ptr<a11::nodes::AsyncNode> output,
                      call->GetOutput("result"));
ABSL_ASSIGN_OR_RETURN(std::optional<std::string> result,
                      output->NextObject<std::string>().Await());
if (!result.has_value()) {
  return absl::DataLossError("remote shout returned no result");
}
std::cout << "remote: " << *result << '\n';
ABSL_RETURN_IF_ERROR(call->Wait().Await().status());
```

Input finality and output closure cross the connection with the fragments.
Draining every output is part of calling an action: independent output ports
have independent bounded writers, and an unread port can block the handler.

## Shut down cleanly

Half-close both session endpoints after calls finish, then stop admission and
drain the service:

```cpp
ABSL_RETURN_IF_ERROR(client->HalfClose());
ABSL_RETURN_IF_ERROR(server->HalfClose());
ABSL_RETURN_IF_ERROR(client->Done().Await().status());
ABSL_RETURN_IF_ERROR(service->StopAccepting());
ABSL_RETURN_IF_ERROR(service->Drain(absl::Seconds(10)).Await().status());
```

`StopAccepting()` leaves live sessions alone. `Drain()` waits for them. Use
`Service::Abort()` with a non-OK status when the process cannot complete
in-flight work.

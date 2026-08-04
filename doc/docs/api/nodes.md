# Nodes

Nodes are A11's ordered, asynchronous streams — the way data moves between
action ports and across the network. See
[Principles](../principles.md#everything-is-a-stream) for the model.

## AsyncNode

Create a stream with [`create`][a11.nodes.async_node.AsyncNode.create], produce
it with [`put`][a11.nodes.async_node.AsyncNode.put] and
[`put_final`][a11.nodes.async_node.AsyncNode.put_final], then finish the writer
with [`drain_and_close`][a11.nodes.async_node.AsyncNode.drain_and_close]. A
consumer can follow the stream with
[`next`][a11.nodes.async_node.AsyncNode.next] or async iteration, while
[`consume`][a11.nodes.async_node.AsyncNode.consume] validates the common case
of one complete logical value. Configure repeated reads once with
[`set_expected_types`][a11.nodes.async_node.AsyncNode.set_expected_types].

::: a11.nodes.async_node.AsyncNode

## NodeMap

`NodeMap` lets several actions resolve the same named streams. Create one for a
related group of actions, then attach it with
[`Action.bind_node_map`][a11.actions.action.Action.bind_node_map].

::: a11.nodes.async_node.NodeMap

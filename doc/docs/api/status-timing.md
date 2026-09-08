# Status & timing

A11's error and time types. Native failures cross the boundary as
[`StatusException`][a11.status.StatusException] with structured details
preserved.

## Error boundaries

An A11 status contains a canonical code, message, and structured details.
Actions, nodes, sessions, stores, and transports retain that status through
asynchronous cleanup and remote propagation. Python wait methods and
`consume()` raise `StatusException` for a non-OK result, preserving the original
status for code-based handling.

A unary read reaches success through the node's final marker and OK closure.
Streaming iteration can end at a clean close after yielding independent
values. An abort exposes the non-OK status in either case. An action's
completion status separately reports the operation and its output cleanup.

A status can also travel as application data in a status chunk with media type
`application/x-a11-status`. This representation is used for dispatch,
completion, node abort, and closure records. It remains separate from ordinary
serialization because a non-OK `StatusOr` represents an error rather than a
value.

## Status

::: a11.status.Status

::: a11.status.StatusCode

::: a11.status.StatusException

## Timing

::: a11.timing.Duration

::: a11.timing.Time

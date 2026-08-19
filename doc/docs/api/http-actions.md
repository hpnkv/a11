# HTTP Actions

Two Actions over one engine in C++, for making HTTP requests from anywhere an
Action can be run — Python, a Flow, or a model's tool call.

| Action | For |
| --- | --- |
| `make_http_request` | HTTP with nothing hidden: a port per protocol concern |
| `web-fetch` | the `fetch()`-shaped adapter, for a caller that wants a document |

## Why an Action rather than a function

An ordinary HTTP client hands back one `Response` object because its language
gives it nothing better to hand back: headers, body and trailers are one value
that is only complete at the end. An A11 port is a stream, and there is no
reason for these to share one — so they do not.

That is not a stylistic preference; it is what lets a caller

* branch on the status **while the body is still arriving**, and never read one
  it did not want;
* give the body to a parser and the trailer section to a checksum verifier,
  concurrently;
* read responses the server *pushed* off a port, which a `fetch()`-shaped API
  has no way to express at all.

## `make_http_request`

### Inputs

| Port | Type | | Meaning |
| --- | --- | --- | --- |
| `url` | `string` | one, required | absolute `http`/`https` URL |
| `method` | `string` | one | `GET` when omitted |
| `request_body` | bytes | stream | the request body, in order |
| `options` | JSON | one | see [Options](#options) |

`request_body` rather than `body` because a port name identifies one node
whichever direction it faces, and the response body is a port too. Feeding it is
optional; closing it empty means a bodyless request.

### Outputs

One per concern, in the order they become readable.

| Port | Type | | Meaning |
| --- | --- | --- | --- |
| `status_code` | `integer` | one | the final status, written **before** the body |
| `headers` | JSON | one | lower-cased name → value; repeats joined `", "` (`set-cookie` with `\n`) |
| `fields` | JSON | stream | every field as `[name, value]`, wire order, repeats intact |
| `body` | bytes | stream | body chunks as they arrive |
| `trailers` | JSON | one | the trailer section, `{}` when there was none; after `body` ends |
| `redirects` | JSON | stream | one `{url, status, location}` per hop followed |
| `pushes` | JSON | stream | one record per pushed response — see below |
| `connection` | JSON | one | `{url, http_version, secure, reused}` |

`headers` and `fields` are the same data twice, on purpose. A joined map is what
`resp.headers["content-type"]` needs; a joined map also destroys repeated fields,
which is precisely the detail this action exists to preserve.

!!! note "`status_code`, not `status`"
    Flow reads `x.status` as the *outcome of the step called `x`*, whatever ports
    it declares — so a port named `status` would be unreachable from a flow. The
    same constraint applies to any action meant to be composed.

### A 4xx is a response

`make_http_request` fails only when there is **no** response. A 404 was answered
by a server that was reached, so it arrives on `status_code` with its body
intact. A caller that wants a failure can make one from the status; the reverse
is not recoverable, and failing would discard the error document — usually the
interesting part.

### Pushed responses

A push carries a head *and* a body, and one port cannot interleave several
bodies without inventing a framing for them. A11 already has the answer: each
pushed body gets a node of its own, and the record on `pushes` carries that
node's id.

```
{"method": "GET", "url": "...", "path": "/style.css", "status": 200,
 "headers": {...}, "request_headers": {...}, "body": "<node id>"}
```

Read it with `node(rec.body)` in Flow, or `action.get_node_map().get(rec["body"])`
in Python. Needs `options.accept_pushes`; without it nothing is ever pushed,
because the connection advertises `SETTINGS_ENABLE_PUSH: 0` and a peer cannot
spend your streams on responses you did not ask for.

### Options

All optional.

| Key | Default | Meaning |
| --- | --- | --- |
| `max_redirects` | `5` | `0` returns the 3xx as the response |
| `timeout` | `300` | seconds; the tighter of this and `x-a11-deadline` wins |
| `request_body` | `"buffer"` | `"buffer"` reads the port to its end and sends a `content-length`; `"stream"` sends each chunk as it arrives |
| `http_version` | `"auto"` | `"auto"`, `"2"` or `"1.1"` |
| `accept_pushes` | `false` | accept HTTP/2 server pushes |
| `reuse_connection` | `true` | share a connection with other requests to the peer |
| `max_body_bytes` | 32 MiB | bound on the request and response bodies |
| `user_agent` | `a11-http/1` | sent when no `user-agent` header is given |
| `headers` | — | an object merged **over** the action's own headers |
| `tls` | — | `{verify_peer, ca_file, certificate_file, key_file}` |
| `omit` | — | output port names to close immediately rather than write |

`"stream"` is the reason `request_body` is a stream port: an upload whose length
nobody knows yet has no `content-length` to declare. It cannot be replayed to a
redirect, so a URL that may redirect wants `"buffer"`.

## `web-fetch`

The same engine with the protocol turned down. Inputs are the same;
outputs are `status_code`, `ok` (below 400), `headers`, `text`, `json`, `body`,
and `items`.

`json` closes with nothing when the body does not parse — "it is not JSON" is an
answer, not a failure.

`items` decodes the body by content type, so a caller does not have to:

| Content type | One item per |
| --- | --- |
| `text/event-stream` | SSE event, `{event, data, id, json?}`, **as it arrives** |
| `application/x-ndjson`, `application/jsonl` | line, **as it arrives** |
| `application/json` holding an array | element |

## Headers

An action header that does not begin with `x-a11-` is sent as an HTTP request
header, verbatim. So Flow's `with "accept": "application/json"` and
`forward headers "authorization"` are already HTTP header syntax, and A11's own
framework headers — a deadline, a trace — stay out of the request.
`options.headers` wins over an inherited value of the same name, and is how a
literal `x-a11-...` header reaches a peer that wants one.

## Connections

A request to a peer another request is already talking to joins that connection
rather than opening a socket — HTTP/2 multiplexes, so both travel at once. When
the last request on a connection finishes, it is **closed**: nothing is kept open
with no work on it, so there is no idle keep-alive to reason about and no
"the server closed our pooled socket" failure mode.

HTTP/1.1 is not shared, because A11's HTTP/1.1 connection carries one request by
design. `connection.http_version` says which you got.

## Registering them

```python
from a11.actions import ActionRegistry
from a11.sdk import http

registry = ActionRegistry()
http.register(registry)                     # both
http.register(registry, low_level=False)    # only web-fetch
```

The two are separately selectable because they answer to different amounts of
trust: a gateway happy to let a caller fetch a document may not want to hand out
streamed uploads, arbitrary methods and server pushes.

## From Python

```python
from a11.sdk import http

async with await http.fetch("https://example.com/rows.ndjson") as response:
    if not await response.ok():
        raise RuntimeError(await response.text())
    async for row in response.aiter_items():
        ...
```

Eight ports is right for the protocol and wrong for a caller who wants three of
them, so `request` and `fetch` do the wiring and drain what nobody asked for.

::: a11.sdk.http.client

::: a11.sdk.http.actions

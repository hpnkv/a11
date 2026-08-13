// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_TOOL_LSP_H_
#define A11_FLOW_TOOL_LSP_H_

#include <iosfwd>

namespace a11::flow::tool {

/// Speak the Language Server Protocol over two streams until the client stops.
///
/// The thin half of the tool: it owns the open documents, converts between the
/// protocol's line-and-UTF-16-character positions and the byte offsets the
/// language works in, and turns each request into one of the service's methods.
/// Nothing about the language is decided here -- what is a problem, what may be
/// written at a caret and what a token means are answered once, in
/// `a11_flow_lang`, and this is the shape an editor wants them in.
///
/// Returns the process's exit code: 0 for a clean `shutdown`/`exit`, 1 when the
/// stream ended without one.
int RunLsp(std::istream& in, std::ostream& out);

}  // namespace a11::flow::tool

#endif  // A11_FLOW_TOOL_LSP_H_

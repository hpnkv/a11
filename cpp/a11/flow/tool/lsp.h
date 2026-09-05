/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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

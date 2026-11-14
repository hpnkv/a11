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
#include <string>
#include <vector>

#include "a11/flow/limits.h"

namespace a11::flow::tool {

/// Serves Language Server Protocol requests over `in` and `out`.
/// Converts UTF-16 positions to source byte offsets and tracks open documents.
/// Returns 0 after `shutdown`/`exit`, or 1 if the input ends first.
int RunLsp(std::istream& in, std::ostream& out,
           std::vector<std::string> source_roots = {},
           CompilationLimits limits = {});

}  // namespace a11::flow::tool

#endif  // A11_FLOW_TOOL_LSP_H_

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

#ifndef A11_FLOW_INSPECT_H_
#define A11_FLOW_INSPECT_H_

#include <string_view>
#include <vector>

#include "a11/flow/diagnostic.h"
#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"

namespace a11::flow {

/// Reports nonfatal Flow diagnostics from parsed and resolved source.
/// Findings include unused values, unwritten outputs, and ineffective stages.
std::vector<Diagnostic> Inspect(std::string_view source,
                                const ParseResult& parsed,
                                const ResolveResult& resolved);

}  // namespace a11::flow

#endif  // A11_FLOW_INSPECT_H_

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

#ifndef A11_FLOW_SERVICE_H_
#define A11_FLOW_SERVICE_H_

#include <string_view>

#include <absl/types/span.h>
#include <nlohmann/json_fwd.hpp>

namespace a11::flow {

/// One question about one document, answered.
///
/// The whole of what a frontend needs from the language, behind one function: a
/// method name, a document, and whatever that method takes. Every surface is an
/// adapter over this -- `a11-flow serve --protocol json` passes requests
/// through
/// almost unchanged, the LSP adapter translates positions and wraps the answers
/// in what an editor expects, and `a11.flow.request` hands the same dict across
/// the Python boundary -- so a capability added here is available in all of
/// them
/// without any adapter knowing what it is.
///
/// A request is
///
/// ```json
/// {"id": 1, "method": "check", "source": "flow t { }", "path": "t.flow"}
/// ```
///
/// and the answer is `{"id": 1, "ok": true, "result": {...}}` with the
/// envelope the method produces, or `{"id": 1, "ok": false, "error":
/// {"message": "..."}}` when the request itself made no sense. A *flow* that
/// makes no sense is not an error; syntax problems are returned as
/// diagnostics.
///
/// `id` is echoed if it is there and omitted if it is not, so a client that
/// does
/// not correlate need not invent one.
nlohmann::json Handle(const nlohmann::json& request);

/// Every method [Handle] knows, in the order `--help` lists them.
absl::Span<const std::string_view> Methods();

/// What a method takes and gives back, one line, for `--help`.
std::string_view MethodSummary(std::string_view method);

}  // namespace a11::flow

#endif  // A11_FLOW_SERVICE_H_

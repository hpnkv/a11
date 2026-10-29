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

#include <pybind11/stl.h>

#include "python/bindings.h"
#include "python/interop.h"
#include "sdk/llm_context/transform.h"

namespace a11::python {
void BindLlmContext(pybind11::module_& module) {
  namespace py = pybind11;
  namespace ctx = a11::sdk::llm_context;
  module.def(
      "llm_context_compact_json",
      [](const std::string& text, size_t limit, size_t depth) {
        return ValueWithoutGil(
            [&] { return ctx::CompactJson(text, limit, depth); });
      },
      py::arg("text"), py::arg("max_bytes") = 8 * 1024 * 1024,
      py::arg("max_depth") = 128,
      "Compact JSON without changing its string or number tokens.");
  module.def(
      "llm_context_encode",
      [](const std::string& text, const std::string& format, size_t limit) {
        return ValueWithoutGil(
            [&] { return ctx::Encode(text, format, limit); });
      },
      py::arg("text"), py::arg("format"),
      py::arg("max_bytes") = 8 * 1024 * 1024,
      "Encode content in a reversible model-facing envelope.");
  module.def(
      "llm_context_expand",
      [](const std::string& text, size_t limit) {
        return ValueWithoutGil([&] { return ctx::Expand(text, limit); });
      },
      py::arg("text"), py::arg("max_bytes") = 8 * 1024 * 1024,
      "Expand a context envelope with a hard output byte limit.");
  module.def("llm_context_estimate_tokens", &ctx::EstimateTokens,
             py::arg("text"),
             "Estimate tokens from bytes; not a provider-authoritative count.");
}
}  // namespace a11::python

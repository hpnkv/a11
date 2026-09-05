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

/**
 * @file
 * @brief Compiling a caller-written name pattern, which std::regex throws on.
 *
 * The same problem, and the same answer, as a11/flow/internal/pattern.h: a
 * pattern in a `__list_actions__` request or an `x-a11-allowed-llm-actions`
 * header is written by whoever is asking, so a malformed one is bad *input* and
 * has to become an InvalidArgument rather than an abort. `std::regex`'s
 * constructor reports that by throwing and offers no way to ask beforehand.
 *
 * Duplicated rather than shared because a11::actions cannot depend on
 * a11::flow -- the dependency runs the other way.
 *
 * Implemented in a11/actions/boundary.cc, which is compiled with exceptions for
 * this purpose. See a11/exception_guard.h.
 */

#ifndef A11_ACTIONS_INTERNAL_PATTERN_H_
#define A11_ACTIONS_INTERNAL_PATTERN_H_

#include <regex>
#include <string>

#include <absl/status/statusor.h>

namespace a11::actions::internal {
/**
 * @brief Compiles an ECMAScript pattern, or explains why it is not one.
 *
 * Matching is always full-match at the call sites, which is the rule
 * `x-a11-allowed-llm-actions` already established with Python's `re.fullmatch`.
 */
absl::StatusOr<std::regex> CompilePattern(const std::string& pattern);
}  // namespace a11::actions::internal

#endif  // A11_ACTIONS_INTERNAL_PATTERN_H_

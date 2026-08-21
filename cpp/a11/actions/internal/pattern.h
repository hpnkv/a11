// Copyright 2026 The A11 Authors.

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

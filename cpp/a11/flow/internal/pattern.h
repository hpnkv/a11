// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Compiling a user-written regular expression, which std::regex throws
 *        on.
 *
 * `std::regex`'s constructor reports a malformed pattern by throwing
 * `std::regex_error` and offers no way to ask beforehand -- it is the one
 * standard-library facility A11 uses whose failure has no non-throwing form. A
 * `pattern` in a `.flow` field constraint is written by whoever wrote the flow,
 * so a bad one is bad *input* and has to become a diagnostic rather than an
 * abort.
 *
 * Implemented in a11/flow/boundary.cc, which is compiled with exceptions for
 * this purpose. See a11/exception_guard.h.
 */

#ifndef A11_FLOW_INTERNAL_PATTERN_H_
#define A11_FLOW_INTERNAL_PATTERN_H_

#include <regex>
#include <string>

#include <absl/status/statusor.h>

namespace a11::flow::internal {

/// Compiles an ECMAScript pattern, or explains why it is not one.
absl::StatusOr<std::regex> CompilePattern(const std::string& pattern);

}  // namespace a11::flow::internal

#endif  // A11_FLOW_INTERNAL_PATTERN_H_

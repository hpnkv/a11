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

#ifndef A11_INTERNAL_REGEX_H_
#define A11_INTERNAL_REGEX_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/statusor.h>

namespace re2 {
class RE2;
}  // namespace re2

namespace a11::internal {

struct RegexCapture {
  std::optional<std::string_view> text;
  std::optional<size_t> offset;
  size_t extent = 0;
};

struct RegexMatch {
  size_t start = 0;
  size_t end = 0;
  /// Group zero is the complete match; absent groups have no value.
  std::vector<RegexCapture> groups;
};

/// A compiled regular expression. Link a11::regex to use it.
class Regex {
 public:
  Regex(Regex&&) noexcept;
  Regex& operator=(Regex&&) noexcept;
  ~Regex();

  /// Whether the pattern matches the entire text.
  [[nodiscard]] bool FullMatch(std::string_view text) const;
  /// Whether the pattern matches anywhere in the text.
  [[nodiscard]] bool PartialMatch(std::string_view text) const;
  [[nodiscard]] size_t CaptureCount() const;
  [[nodiscard]] std::string_view CaptureName(size_t group) const;
  [[nodiscard]] absl::StatusOr<std::optional<RegexMatch>> Find(
      std::string_view text, size_t start = 0) const;

 private:
  friend absl::StatusOr<Regex> CompileRegex(std::string_view pattern);
  explicit Regex(std::unique_ptr<re2::RE2> expression);

  std::unique_ptr<re2::RE2> expression_;
  std::vector<std::string> names_;
};

/// Compiles an RE2 pattern, reporting invalid syntax without logging it.
absl::StatusOr<Regex> CompileRegex(std::string_view pattern);

}  // namespace a11::internal

#endif  // A11_INTERNAL_REGEX_H_

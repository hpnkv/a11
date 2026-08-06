// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A scoped temporary directory for tests that touch the filesystem.
 */

#ifndef A11_TESTS_TEMP_DIR_H_
#define A11_TESTS_TEMP_DIR_H_

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <absl/strings/str_cat.h>
#include <unistd.h>

namespace a11::testing {

/**
 * A uniquely named directory that removes itself and its contents on scope
 * exit.
 *
 * Names combine the process id with a monotonic counter, so parallel test
 * binaries and repeated cases within one binary never collide.
 */
class TempDir {
 public:
  /**
   * @brief Create a fresh directory under the system temporary location.
   *
   * @param label
   *   Short prefix that makes a leaked directory identifiable.
   */
  explicit TempDir(std::string_view label = "a11-test") {
    static std::atomic<unsigned long long> counter{0};
    const auto unique = counter.fetch_add(1, std::memory_order_relaxed);
    path_ =
        std::filesystem::temp_directory_path() /
        absl::StrCat(label, "-", static_cast<unsigned long long>(::getpid()),
                     "-", unique);
    std::filesystem::create_directories(path_);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /** The directory path. */
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  /** The directory path as a string, for APIs that take one. */
  [[nodiscard]] std::string string() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace a11::testing

#endif  // A11_TESTS_TEMP_DIR_H_

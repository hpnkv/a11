// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   Helpers shared by the ChunkStore backends.
 *
 * These are implementation details of `a11::stores`, not public API. They exist
 * because every backend needs the same four things -- an inline completion
 * wrapper, a deadline-honoring wait, the Put()-in-terms-of-PutMany() adapter,
 * and environment parsing for its options struct -- and three near-identical
 * copies is two too many.
 */

#ifndef A11_STORES_INTERNAL_CHUNK_STORE_COMMON_H_
#define A11_STORES_INTERNAL_CHUNK_STORE_COMMON_H_

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "thread/select.h"
#include "thread/selectables.h"

namespace a11::stores::internal {

/**
 * @brief
 *   Run @p operation on the calling thread and wrap its result in a ready
 *   Future, converting any escaping exception into an `UnknownError`.
 *
 * Use this for backend operations that never block. Operations that can block
 * belong on `a11::Submit` instead, so they do not stall the caller.
 *
 * @param operation
 *   A callable returning `absl::StatusOr<T>` (or something convertible to it).
 * @return
 *   An already-completed Future carrying the operation's result.
 */
template <typename T, typename F>
a11::Future<T> CompleteInline(F&& operation) {
  try {
    return a11::CompletedFuture<T>(std::forward<F>(operation)());
  } catch (const std::exception& error) {
    return a11::FailedFuture<T>(absl::UnknownError(error.what()));
  } catch (...) {
    return a11::FailedFuture<T>(
        absl::UnknownError("Chunk store operation raised an exception"));
  }
}

/**
 * @brief
 *   Block the calling fiber until @p changed fires, the fiber is cancelled, or
 *   @p deadline elapses.
 *
 * The event must be snapshotted *before* the state check that decided a wait
 * was necessary; because PermanentEvent latches and is replaced per generation,
 * that ordering is what closes the lost-wakeup window.
 *
 * @param changed
 *   The generation event snapshotted before the state check.
 * @param deadline
 *   The absolute time after which the wait gives up.
 * @param message
 *   The message for the DeadlineExceededError, describing what was awaited.
 * @return
 *   OK when the event fired, `CancelledError` when the fiber was cancelled, or
 *   `DeadlineExceededError` carrying @p message.
 */
inline absl::Status WaitForChange(
    const std::shared_ptr<thread::PermanentEvent>& changed, absl::Time deadline,
    std::string message) {
  const int selected =
      thread::SelectUntil(deadline, {thread::OnCancel(), changed->OnEvent()});
  if (selected == 0) {
    return absl::CancelledError("Chunk store operation was cancelled");
  }
  if (selected < 0) {
    return absl::DeadlineExceededError(std::move(message));
  }
  return absl::OkStatus();
}

/**
 * @brief
 *   Implement `ChunkStore::Put()` by delegating to a backend's `PutMany()`.
 *
 * @param put_many
 *   The backend's batch writer, invoked with a single-element batch.
 * @param fragment
 *   The fragment to append.
 * @param backend_name
 *   Backend name used in the diagnostic when the batch misbehaves.
 * @return
 *   An awaitable resolving with the sequence number assigned to @p fragment.
 */
template <typename PutManyFn>
a11::Future<std::uint32_t> PutOneViaPutMany(PutManyFn&& put_many,
                                            data::NodeFragment fragment,
                                            std::string_view backend_name) {
  std::vector<data::NodeFragment> fragments;
  fragments.push_back(std::move(fragment));
  a11::Future<std::vector<std::uint32_t>> batch =
      std::forward<PutManyFn>(put_many)(std::move(fragments));
  a11::Promise<std::uint32_t> promise;
  a11::Future<std::uint32_t> result = promise.future();
  batch.OnReady(
      [promise = std::move(promise), name = std::string(backend_name)](
          const absl::StatusOr<std::vector<std::uint32_t>>& seqs) mutable {
        if (!seqs.ok()) {
          promise.SetStatus(seqs.status()).IgnoreError();
        } else if (seqs->size() != 1) {
          promise
              .SetStatus(absl::DataLossError(absl::StrCat(
                  name, " PutMany did not return exactly one sequence")))
              .IgnoreError();
        } else {
          promise.SetValue(seqs->front()).IgnoreError();
        }
      });
  return result;
}

/**
 * @brief Read an environment variable, or nullopt when it is unset.
 *
 * @param name
 *   The environment variable name.
 * @return
 *   The value, or nullopt when the variable is not present.
 */
inline std::optional<std::string> EnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

/**
 * @brief Parse a whole environment value as a non-negative size.
 *
 * The entire value must be consumed, so trailing text is an error rather than
 * being silently ignored.
 *
 * @param value
 *   The raw environment value.
 * @param name
 *   The variable name, used in the error message.
 * @return
 *   The parsed size, or `InvalidArgumentError`.
 */
inline absl::StatusOr<size_t> ParseEnvironmentSize(std::string_view value,
                                                   std::string_view name) {
  if (value.empty()) {
    return absl::InvalidArgumentError(absl::StrCat(name, " is empty"));
  }
  size_t parsed = 0;
  const char* first = value.data();
  const char* last = first + value.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) {
    return absl::InvalidArgumentError(
        absl::StrCat(name, " is not a non-negative integer"));
  }
  return parsed;
}

/**
 * @brief Parse a whole environment value as a non-negative duration in millis.
 *
 * @param value
 *   The raw environment value.
 * @param name
 *   The variable name, used in the error message.
 * @return
 *   The parsed duration, or `InvalidArgumentError`.
 */
inline absl::StatusOr<absl::Duration> ParseEnvironmentMilliseconds(
    std::string_view value, std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(const size_t parsed, ParseEnvironmentSize(value, name));
  return absl::Milliseconds(static_cast<std::int64_t>(parsed));
}

}  // namespace a11::stores::internal

#endif  // A11_STORES_INTERNAL_CHUNK_STORE_COMMON_H_

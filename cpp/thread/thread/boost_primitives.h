// Copyright 2026 The Action Engine Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef THREAD_BOOST_PRIMITIVES_H_
#define THREAD_BOOST_PRIMITIVES_H_

#include <cstddef>

#include <absl/base/nullability.h>
#include <absl/base/thread_annotations.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

namespace thread {

// Fiber-aware synchronization primitives. Their Boost implementation is kept
// out of this public header in fixed, stack-resident storage; size/alignment
// checks in the implementation fail the build if the implementation ever
// outgrows that storage.
class ABSL_LOCKABLE ABSL_ATTRIBUTE_WARN_UNUSED Mutex {
 public:
  Mutex();
  ~Mutex();

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  void Lock() noexcept ABSL_EXCLUSIVE_LOCK_FUNCTION();
  void Unlock() noexcept ABSL_UNLOCK_FUNCTION();

  void lock() noexcept ABSL_EXCLUSIVE_LOCK_FUNCTION() { Lock(); }

  void unlock() noexcept ABSL_UNLOCK_FUNCTION() { Unlock(); }

  friend class CondVar;

 private:
  struct Impl;
  static constexpr size_t kImplSize = 64;
  static constexpr size_t kImplAlignment = alignof(std::max_align_t);

  Impl* absl_nonnull GetImpl();
  const Impl* absl_nonnull GetImpl() const;

  alignas(kImplAlignment) std::byte impl_[kImplSize];
};

class ABSL_SCOPED_LOCKABLE MutexLock {
 public:
  explicit MutexLock(Mutex* absl_nonnull mu) ABSL_EXCLUSIVE_LOCK_FUNCTION(mu)
      : mu_(mu) {
    mu_->Lock();
  }

  MutexLock(const MutexLock&) = delete;  // NOLINT(runtime/mutex)
  MutexLock(MutexLock&&) = delete;       // NOLINT(runtime/mutex)
  MutexLock& operator=(const MutexLock&) = delete;
  MutexLock& operator=(MutexLock&&) = delete;

  ~MutexLock() ABSL_UNLOCK_FUNCTION() { mu_->Unlock(); }

 private:
  Mutex* absl_nonnull const mu_;
};

class CondVar {
 public:
  CondVar();
  ~CondVar();

  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;

  void Wait(Mutex* absl_nonnull mu) noexcept ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu);

  bool WaitWithTimeout(Mutex* absl_nonnull mu, absl::Duration timeout) noexcept
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    return WaitWithDeadline(mu, absl::Now() + timeout);
  }

  bool WaitWithDeadline(Mutex* absl_nonnull mu,
                        const absl::Time& deadline) noexcept
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu);

  void Signal() noexcept;
  void SignalAll() noexcept;

 private:
  struct Impl;
  static constexpr size_t kImplSize = 64;
  static constexpr size_t kImplAlignment = alignof(std::max_align_t);

  Impl* absl_nonnull GetImpl();
  const Impl* absl_nonnull GetImpl() const;

  alignas(kImplAlignment) std::byte impl_[kImplSize];
};

// Suspends only the current fiber. A nonpositive duration is a cooperative
// yield, which makes polling loops fair without parking their worker thread.
void SleepFor(absl::Duration duration);

}  // namespace thread

#endif  // THREAD_BOOST_PRIMITIVES_H_

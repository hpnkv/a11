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

#include "thread/boost_primitives.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <new>

#include <absl/base/optimization.h>
#include <absl/log/log.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <boost/fiber/condition_variable.hpp>
#include <boost/fiber/context.hpp>
#include <boost/fiber/exceptions.hpp>
#include <boost/fiber/mutex.hpp>
#include <boost/fiber/operations.hpp>

#include "thread/fiber_diagnostics.h"

namespace thread {
namespace {

// Null when this thread has no fiber scheduler yet.
const void* absl_nullable ActiveContext() {
  return boost::fibers::context::active();
}

}  // namespace

struct Mutex::Impl {
  boost::fibers::mutex mutex;
};

Mutex::Mutex() {
  static_assert(sizeof(Impl) <= kImplSize);
  static_assert(alignof(Impl) <= kImplAlignment);
  std::construct_at(reinterpret_cast<Impl*>(impl_));
}

Mutex::~Mutex() {
  std::destroy_at(GetImpl());
}

Mutex::Impl* Mutex::GetImpl() {
  return std::launder(reinterpret_cast<Impl*>(impl_));
}

const Mutex::Impl* Mutex::GetImpl() const {
  return std::launder(reinterpret_cast<const Impl*>(impl_));
}

void Mutex::Lock() noexcept {
  if (!internal::OwnerTrackingEnabled()) {
    try {
      GetImpl()->mutex.lock();
    } catch (const boost::fibers::lock_error& error) {
      LOG(FATAL) << "Fiber mutex lock failed: " << error.what();
    }
    return;
  }

  const void* self = ActiveContext();
  const void* holder = holder_context_.load(std::memory_order_relaxed);
  try {
    if (holder == nullptr) {
      GetImpl()->mutex.lock();
    } else {
      // A holder seen here may have released by the time lock() runs, which
      // costs a wait record nobody reads.
      THREAD_WAIT_SCOPE(scope, WaitKind::kMutex, this);
      if (FiberDiagnostics* record = scope.record(); record != nullptr) {
        internal::SetWaitOwnerContext(record, holder);
      }
      GetImpl()->mutex.lock();
    }
  } catch (const boost::fibers::lock_error& error) {
    LOG(FATAL) << "Fiber mutex lock failed: " << error.what();
  }
  holder_context_.store(self, std::memory_order_relaxed);
}

void Mutex::Unlock() noexcept {
  // Cleared before the unlock, so the next holder's store is not overwritten.
  holder_context_.store(nullptr, std::memory_order_relaxed);
  try {
    GetImpl()->mutex.unlock();
  } catch (const boost::fibers::lock_error& error) {
    LOG(FATAL) << "Fiber mutex unlock failed: " << error.what();
  }
}

struct CondVar::Impl {
  boost::fibers::condition_variable_any condition;
};

CondVar::CondVar() {
  static_assert(sizeof(Impl) <= kImplSize);
  static_assert(alignof(Impl) <= kImplAlignment);
  std::construct_at(reinterpret_cast<Impl*>(impl_));
}

CondVar::~CondVar() {
  std::destroy_at(GetImpl());
}

CondVar::Impl* CondVar::GetImpl() {
  return std::launder(reinterpret_cast<Impl*>(impl_));
}

const CondVar::Impl* CondVar::GetImpl() const {
  return std::launder(reinterpret_cast<const Impl*>(impl_));
}

void CondVar::Wait(Mutex* mu) noexcept {
  // The scope's frame pointer is this frame's, so an unwind from it starts at
  // the caller of Wait.
  THREAD_WAIT_SCOPE(scope, WaitKind::kCondVar, this);
  try {
    GetImpl()->condition.wait(mu->GetImpl()->mutex);
  } catch (const boost::fibers::lock_error& error) {
    LOG(FATAL) << "Fiber condition wait failed: " << error.what();
  }
}

bool CondVar::WaitWithDeadline(Mutex* mu, const absl::Time& deadline) noexcept {
  if (deadline == absl::InfiniteFuture()) {
    Wait(mu);
    return false;
  }

  const absl::Duration remaining = deadline - absl::Now();
  if (remaining <= absl::ZeroDuration()) {
    return true;
  }
  THREAD_WAIT_SCOPE(scope, WaitKind::kCondVar, this, DeadlineNanos(deadline));
  try {
    return GetImpl()->condition.wait_for(
               mu->GetImpl()->mutex, absl::ToChronoNanoseconds(remaining)) ==
           boost::fibers::cv_status::timeout;
  } catch (const boost::fibers::lock_error& error) {
    LOG(FATAL) << "Fiber condition timed wait failed: " << error.what();
  }
  return false;
}

void CondVar::Signal() noexcept {
  try {
    GetImpl()->condition.notify_one();
  } catch (const boost::fibers::lock_error& error) {
    LOG(FATAL) << "Fiber condition signal failed: " << error.what();
  }
}

void CondVar::SignalAll() noexcept {
  try {
    GetImpl()->condition.notify_all();
  } catch (const boost::fibers::lock_error& error) {
    LOG(FATAL) << "Fiber condition broadcast failed: " << error.what();
  }
}

void SleepFor(absl::Duration duration) {
  if (duration <= absl::ZeroDuration()) {
    boost::this_fiber::yield();
    return;
  }
  if (duration == absl::InfiniteDuration()) {
    THREAD_WAIT_SCOPE(scope, WaitKind::kSleep, nullptr);
    boost::fibers::context::active()->wait_until(
        std::chrono::steady_clock::time_point::max());
    return;
  }
  THREAD_WAIT_SCOPE(scope, WaitKind::kSleep, nullptr,
                    DeadlineNanos(absl::Now() + duration));
  boost::this_fiber::sleep_for(absl::ToChronoNanoseconds(duration));
}

}  // namespace thread

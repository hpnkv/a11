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

#include <chrono>
#include <cstdio>
#include <memory>
#include <new>
#include <string>

#include <absl/base/optimization.h>
#include <absl/debugging/stacktrace.h>
#include <absl/debugging/symbolize.h>
#include <absl/log/log.h>
#include <absl/time/clock.h>
#include <boost/fiber/condition_variable.hpp>
#include <boost/fiber/exceptions.hpp>
#include <boost/fiber/mutex.hpp>
#include <boost/fiber/operations.hpp>

namespace thread {
namespace {

// Set to a finite duration while diagnosing a stalled wait. Production waits
// do no stack-trace work.
constexpr absl::Duration kDiagnosticTimeout = absl::InfiniteDuration();

[[maybe_unused]] std::string GetCurrentStackTrace() {
  void* trace[20];
  const int trace_size = absl::GetStackTrace(trace, 20, 1);

  std::string result =
      "[] Execution path: " + std::to_string(trace_size) + " frames\n";
  for (int index = 0; index < trace_size; ++index) {
    char buffer[1024];
    result += "[] ";
    if (absl::Symbolize(trace[index], buffer, sizeof(buffer))) {
      result += buffer;
    } else {
      char address[32];
      std::snprintf(address, sizeof(address), "%p", trace[index]);
      result += address;
    }
    result += '\n';
  }
  return result;
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
  try {
    GetImpl()->mutex.lock();
  } catch (const boost::fibers::lock_error& error) {
    LOG(FATAL) << "Fiber mutex lock failed: " << error.what();
  }
}

void Mutex::Unlock() noexcept {
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
  if constexpr (kDiagnosticTimeout < absl::InfiniteDuration()) {
    const std::string trace = GetCurrentStackTrace();
    try {
      if (GetImpl()->condition.wait_for(
              mu->GetImpl()->mutex,
              absl::ToChronoNanoseconds(kDiagnosticTimeout)) !=
          boost::fibers::cv_status::timeout) {
        return;
      }
      LOG(ERROR) << "Fiber condition wait exceeded diagnostic timeout:\n"
                 << trace;
    } catch (const boost::fibers::lock_error& error) {
      LOG(FATAL) << "Fiber condition wait failed: " << error.what();
    }
  }

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

  if constexpr (kDiagnosticTimeout < absl::InfiniteDuration()) {
    const absl::Time diagnostic_deadline = absl::Now() + kDiagnosticTimeout;
    if (diagnostic_deadline < deadline) {
      const std::string trace = GetCurrentStackTrace();
      try {
        if (GetImpl()->condition.wait_for(
                mu->GetImpl()->mutex,
                absl::ToChronoNanoseconds(diagnostic_deadline - absl::Now())) !=
            boost::fibers::cv_status::timeout) {
          return false;
        }
        LOG(ERROR) << "Fiber condition wait exceeded diagnostic timeout:\n"
                   << trace;
      } catch (const boost::fibers::lock_error& error) {
        LOG(FATAL) << "Fiber condition wait failed: " << error.what();
      }
    }
  }

  const absl::Duration remaining = deadline - absl::Now();
  if (remaining <= absl::ZeroDuration()) {
    return true;
  }
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
    boost::fibers::context::active()->wait_until(
        std::chrono::steady_clock::time_point::max());
    return;
  }
  boost::this_fiber::sleep_for(absl::ToChronoNanoseconds(duration));
}

}  // namespace thread

// Copyright 2026 The A11 Authors
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

#include "jvm/bridge.h"

#include <deque>
#include <limits>

#include <absl/base/no_destructor.h>
#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_set.h>
#include <absl/log/check.h>
#include <absl/time/clock.h>

#include "a11/json_codec.h"
#include "thread/boost_primitives.h"

namespace a11::jvm {
namespace {
struct CompletionEvent {
  std::int64_t token;
  std::function<nlohmann::json()> encode;
  std::int64_t retire_callback = 0;
  std::int64_t retire_invocation = 0;
};

struct State {
  thread::Mutex mu;
  thread::CondVar cv;
  absl::flat_hash_map<std::int64_t, std::any> handles ABSL_GUARDED_BY(mu);
  std::int64_t next_handle ABSL_GUARDED_BY(mu) = 1;
  std::deque<CompletionEvent> events ABSL_GUARDED_BY(mu);
  bool stopping ABSL_GUARDED_BY(mu) = false;
  absl::flat_hash_set<std::int64_t> subscribers ABSL_GUARDED_BY(mu);
  absl::flat_hash_set<std::int64_t> callbacks ABSL_GUARDED_BY(mu);
  absl::flat_hash_set<std::int64_t> invocations ABSL_GUARDED_BY(mu);
  absl::flat_hash_set<std::int64_t> cancelled_invocations ABSL_GUARDED_BY(mu);
};

State& Shared() {
  static absl::NoDestructor<State> state;
  return *state;
}

const absl::flat_hash_map<std::string, Operation>& Registered() {
  static const absl::NoDestructor<absl::flat_hash_map<std::string, Operation>>
      operations([] {
        absl::flat_hash_map<std::string, Operation> result;
        BindCore(&result);
        BindSchemas(&result);
        BindStores(&result);
        BindFlow(&result);
        BindNet(&result);
        return result;
      }());
  return *operations;
}
}  // namespace

std::int64_t Retain(std::any value) {
  State& state = Shared();
  thread::MutexLock lock(&state.mu);
  const std::int64_t id = state.next_handle++;
  state.handles.emplace(id, std::move(value));
  return id;
}

absl::StatusOr<std::any> Lookup(std::int64_t id) {
  State& state = Shared();
  thread::MutexLock lock(&state.mu);
  const auto found = state.handles.find(id);
  if (found == state.handles.end()) {
    return absl::FailedPreconditionError("Native object has been released");
  }
  return found->second;
}

void Release(std::int64_t id) {
  std::any removed;
  State& state = Shared();
  {
    thread::MutexLock lock(&state.mu);
    const auto found = state.handles.find(id);
    if (found == state.handles.end()) {
      return;
    }
    removed = std::move(found->second);
    state.handles.erase(found);
  }
}

size_t HandleCount() {
  State& state = Shared();
  thread::MutexLock lock(&state.mu);
  return state.handles.size();
}

size_t HandleCount(const std::type_info& type) {
  State& state = Shared();
  thread::MutexLock lock(&state.mu);
  size_t count = 0;
  for (const auto& [unused, value] : state.handles) {
    if (value.type() == type) {
      ++count;
    }
  }
  return count;
}

nlohmann::json Envelope(const absl::StatusOr<nlohmann::json>& result) {
  return nlohmann::json{{"status", StatusToJsonOrEmptyDetails(result.status())},
                        {"value", result.ok() ? *result : Null()}};
}

absl::Status Subscribe(std::int64_t token) {
  State& state = Shared();
  thread::MutexLock lock(&state.mu);
  if (state.stopping) {
    return absl::CancelledError("JVM bridge is stopping");
  }
  if (state.subscribers.size() >= 1024) {
    return absl::ResourceExhaustedError(
        "Too many pending JVM future observers");
  }
  if (!state.subscribers.insert(token).second) {
    return absl::AlreadyExistsError("JVM future observer already registered");
  }
  return absl::OkStatus();
}

void Unsubscribe(std::int64_t token) {
  std::vector<CompletionEvent> removed;
  State& state = Shared();
  {
    thread::MutexLock lock(&state.mu);
    state.subscribers.erase(token);
    for (auto& event : state.events) {
      if (event.token == token) {
        removed.push_back(std::move(event));
      }
    }
    std::erase_if(state.events, [token](const CompletionEvent& event) {
      return event.token == token;
    });
  }
}

void EnqueueCompletion(std::int64_t token,
                       std::function<nlohmann::json()> encode) {
  State& state = Shared();
  thread::MutexLock lock(&state.mu);
  if (!state.stopping &&
      state.subscribers.find(token) != state.subscribers.end()) {
    state.events.push_back(CompletionEvent{token, std::move(encode)});
  }
  state.cv.SignalAll();
}

Callback::~Callback() {
  State& state = Shared();
  thread::MutexLock lock(&state.mu);
  if (!state.stopping) {
    state.events.push_back(CompletionEvent{
        0,
        [token = token_] {
          return nlohmann::json{{"kind", "releaseCallback"}, {"token", token}};
        },
        token_});
  }
  state.cv.SignalAll();
}

Task Callback::Invoke(nlohmann::json arguments,
                      std::vector<std::int64_t> owned) {
  auto promise = std::make_shared<Promise<Unit>>();
  Task future = promise->future();
  const auto id = Retain(promise);
  State& state = Shared();
  bool admitted = false;
  {
    thread::MutexLock lock(&state.mu);
    admitted = !state.stopping && state.invocations.size() < 1024;
    if (admitted) {
      state.invocations.insert(id);
    }
  }
  if (!admitted) {
    for (auto handle : owned) {
      Release(handle);
    }
    Release(id);
    promise
        ->SetStatus(
            absl::ResourceExhaustedError("Too many pending JVM callbacks"))
        .IgnoreError();
    return future;
  }
  std::weak_ptr<Promise<Unit>> weak_promise = promise;
  promise
      ->SetCancellationCallback([id, weak_promise] {
        auto retained = weak_promise.lock();
        if (retained == nullptr) {
          return;
        }
        State& state = Shared();
        {
          thread::MutexLock lock(&state.mu);
          if (!state.stopping &&
              state.invocations.find(id) != state.invocations.end() &&
              state.cancelled_invocations.insert(id).second) {
            state.events.push_back(CompletionEvent{
                0,
                [id] {
                  return nlohmann::json{{"kind", "cancelCallback"},
                                        {"promise", id}};
                },
                0, id});
          }
          state.cv.SignalAll();
        }
        retained->SetStatus(absl::CancelledError("JVM callback was cancelled"))
            .IgnoreError();
        Release(id);
      })
      .IgnoreError();
  future.OnReady([keep_alive = shared_from_this()](const auto&) {});
  {
    thread::MutexLock lock(&state.mu);
    state.events.push_back(CompletionEvent{
        0, [token = token_, id, arguments = std::move(arguments),
            owned = std::move(owned)]() mutable {
          return nlohmann::json{{"kind", "callback"},
                                {"token", token},
                                {"promise", id},
                                {"args", std::move(arguments)},
                                {"owned", std::move(owned)}};
        }});
    state.cv.SignalAll();
  }
  return future;
}

absl::Status CompleteCallback(std::int64_t id, absl::Status status) {
  ABSL_ASSIGN_OR_RETURN(auto promise, Object<Promise<Unit>>(id));
  auto completed = status.ok() ? promise->SetValue(Unit{})
                               : promise->SetStatus(std::move(status));
  Release(id);
  State& state = Shared();
  {
    thread::MutexLock lock(&state.mu);
    if (state.cancelled_invocations.find(id) ==
        state.cancelled_invocations.end()) {
      state.invocations.erase(id);
    }
  }
  return completed;
}

nlohmann::json TakeEvents() {
  std::vector<CompletionEvent> ready;
  std::vector<CompletionEvent> discarded;
  State& state = Shared();
  {
    thread::MutexLock lock(&state.mu);
    while (state.events.empty() && !state.stopping) {
      state.cv.Wait(&state.mu);
    }
    while (!state.events.empty() && ready.size() < 128) {
      CompletionEvent event = std::move(state.events.front());
      state.events.pop_front();
      if (event.retire_callback != 0) {
        state.callbacks.erase(event.retire_callback);
      }
      if (event.retire_invocation != 0) {
        state.invocations.erase(event.retire_invocation);
        state.cancelled_invocations.erase(event.retire_invocation);
      }
      if (event.token == 0 || state.subscribers.erase(event.token) != 0) {
        ready.push_back(std::move(event));
      } else {
        discarded.push_back(std::move(event));
      }
    }
    state.cv.SignalAll();
  }
  nlohmann::json events = nlohmann::json::array();
  for (auto& event : ready) {
    events.push_back(event.encode());
  }
  return events;
}

void Shutdown() {
  State& state = Shared();
  thread::MutexLock lock(&state.mu);
  state.stopping = true;
  state.cv.SignalAll();
}

absl::StatusOr<absl::Duration> Timeout(const nlohmann::json& args) {
  const auto typed = args.find("timeout");
  if (typed != args.end()) {
    return ReadDuration(*typed);
  }
  if (args.find("timeoutMs") == args.end() || args.at("timeoutMs").is_null()) {
    return absl::InfiniteDuration();
  }
  ABSL_ASSIGN_OR_RETURN(auto milliseconds,
                        Arg<std::int64_t>(args, "timeoutMs"));
  if (milliseconds < 0) {
    return absl::InvalidArgumentError("Timeout must be non-negative");
  }
  return absl::Milliseconds(milliseconds);
}

absl::StatusOr<absl::Time> Deadline(const nlohmann::json& args) {
  const auto absolute = args.find("deadline");
  if (absolute != args.end() && !absolute->is_null()) {
    return ReadTime(*absolute);
  }
  ABSL_ASSIGN_OR_RETURN(absl::Duration timeout, Timeout(args));
  return timeout == absl::InfiniteDuration() ? absl::InfiniteFuture()
                                             : absl::Now() + timeout;
}

absl::StatusOr<std::optional<std::uint32_t>> Sequence(
    const nlohmann::json& args) {
  if (args.find("seq") == args.end() || args.at("seq").is_null()) {
    return std::nullopt;
  }
  ABSL_ASSIGN_OR_RETURN(auto seq, Arg<std::uint32_t>(args, "seq"));
  return seq;
}

absl::StatusOr<absl::Status> StatusArg(const nlohmann::json& args,
                                       std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(nlohmann::json value, Arg<nlohmann::json>(args, name));
  return StatusFromJson(value);
}

absl::StatusOr<nlohmann::json> Dispatch(const nlohmann::json& request) {
  ABSL_ASSIGN_OR_RETURN(std::string operation, Arg<std::string>(request, "op"));
  ABSL_ASSIGN_OR_RETURN(auto self,
                        DefaultArg<std::int64_t>(request, "self", 0));
  ABSL_ASSIGN_OR_RETURN(
      nlohmann::json args,
      DefaultArg<nlohmann::json>(request, "args", nlohmann::json::object()));
  if (!args.is_object()) {
    return absl::InvalidArgumentError("Native arguments must be an object");
  }
  if (operation == "bridge.operations") {
    std::vector<std::string> names;
    for (const auto& [name, unused] : Registered()) {
      names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return nlohmann::json(names);
  }
  if (operation == "bridge.version") {
    return nlohmann::json(A11_VERSION);
  }
  if (operation == "bridge.handles") {
    return nlohmann::json(HandleCount());
  }
  if (operation == "callback.create") {
    ABSL_ASSIGN_OR_RETURN(auto token, Arg<std::int64_t>(args, "token"));
    State& state = Shared();
    {
      thread::MutexLock lock(&state.mu);
      if (state.stopping) {
        return absl::CancelledError("JVM bridge is stopping");
      }
      if (state.callbacks.size() >= 1024) {
        return absl::ResourceExhaustedError(
            "Too many registered JVM callbacks");
      }
      if (!state.callbacks.insert(token).second) {
        return absl::AlreadyExistsError("JVM callback already registered");
      }
    }
    return Save(std::make_shared<Callback>(token));
  }
  if (operation == "future.subscribe") {
    ABSL_ASSIGN_OR_RETURN(auto future, Object<PendingOperation>(self));
    ABSL_ASSIGN_OR_RETURN(auto token, Arg<std::int64_t>(args, "token"));
    ABSL_RETURN_IF_ERROR(Subscribe(token));
    future->SubscribeResult(token);
    return Null();
  }
  if (operation == "future.unsubscribe") {
    ABSL_ASSIGN_OR_RETURN(auto token, Arg<std::int64_t>(args, "token"));
    Unsubscribe(token);
    return Null();
  }

  const auto found = Registered().find(operation);
  if (found == Registered().end()) {
    return absl::UnimplementedError("Unknown native operation: " + operation);
  }
  return found->second(self, args);
}
}  // namespace a11::jvm

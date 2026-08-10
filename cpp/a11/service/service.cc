// Copyright 2026 The A11 Authors.

#include "a11/service/service.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/actions/registry.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/service/session.h"
#include "thread/boost_primitives.h"

namespace a11::service {

struct Service::State {
  mutable thread::Mutex mu;
  mutable thread::CondVar idle;  // signalled whenever sessions_ shrinks

  bool accepting ABSL_GUARDED_BY(mu) = true;
  std::shared_ptr<actions::ActionRegistry> registry ABSL_GUARDED_BY(mu);
  OnServiceConnection on_connection ABSL_GUARDED_BY(mu);
  ServiceOptions options;  // immutable after Create

  absl::flat_hash_map<std::string, std::shared_ptr<Session>> sessions
      ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::string, std::string> stream_to_session
      ABSL_GUARDED_BY(mu);

  void Unregister(const std::string& session_id) {
    thread::MutexLock lock(&mu);
    sessions.erase(session_id);
    for (auto it = stream_to_session.begin(); it != stream_to_session.end();) {
      if (it->second == session_id) {
        stream_to_session.erase(it++);
      } else {
        ++it;
      }
    }
    idle.SignalAll();
  }
};

namespace {

/**
 * Builds and registers a session for @p stream, runs the connection hook, and
 * attaches. Shared by Serve and StartStreamHandler, which differ only in what
 * they hand back.
 */
absl::StatusOr<std::shared_ptr<Session>> Admit(
    const std::shared_ptr<Service::State>& state,
    const std::shared_ptr<net::WireStream>& stream, StreamMode mode) {
  if (stream == nullptr) {
    return absl::InvalidArgumentError("Service cannot serve a null stream");
  }

  std::shared_ptr<actions::ActionRegistry> registry;
  OnServiceConnection on_connection;
  {
    thread::MutexLock lock(&state->mu);
    if (!state->accepting) {
      return absl::FailedPreconditionError(
          "The service is no longer accepting connections");
    }
    registry = state->registry;
    on_connection = state->on_connection;
  }
  if (state->options.copy_registry_per_connection && registry != nullptr) {
    registry = registry->Copy();
  }

  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<Session> session,
      Session::Create(/*session_id=*/{}, state->options.on_stream_message,
                      state->options.on_stream_done,
                      state->options.session_headers,
                      state->options.session_options, /*node_map=*/nullptr,
                      registry));

  const std::string session_id = session->GetId();
  {
    thread::MutexLock lock(&state->mu);
    // Re-checked under the lock: StopAccepting may have run while the session
    // was being built, and admitting after it would make a drain never finish.
    if (!state->accepting) {
      return absl::FailedPreconditionError(
          "The service is no longer accepting connections");
    }
    state->sessions.emplace(session_id, session);
    state->stream_to_session.emplace(stream->GetId(), session_id);
  }

  // The hook runs before AddStream, so it can still swap the registry or bind a
  // bridge without racing the first inbound message.
  if (on_connection) {
    a11::Task prepared = on_connection(session, stream);
    absl::Status status = prepared.valid()
                              ? prepared.Await().status()
                              : absl::FailedPreconditionError(
                                    "The connection hook returned an invalid "
                                    "Task");
    if (!status.ok()) {
      state->Unregister(session_id);
      (void)stream->Abort(status);
      return status;
    }
  }

  absl::StatusOr<a11::Task> attached = session->AddStream(stream, mode);
  if (!attached.ok()) {
    state->Unregister(session_id);
    return attached.status();
  }
  if (absl::Status status = attached->Await().status(); !status.ok()) {
    state->Unregister(session_id);
    return status;
  }
  return session;
}

}  // namespace

absl::Status ServiceOptions::Validate() const {
  if (drain_timeout < absl::ZeroDuration()) {
    return absl::InvalidArgumentError(
        "ServiceOptions.drain_timeout must not be negative");
  }
  return session_options.Validate();
}

absl::StatusOr<std::shared_ptr<Service>> Service::Create(
    std::shared_ptr<actions::ActionRegistry> action_registry,
    OnServiceConnection on_connection, ServiceOptions options) {
  ABSL_RETURN_IF_ERROR(options.Validate());
  auto state = std::make_shared<State>();
  {
    // Nothing else can see `state` yet, but the guarded members are declared
    // guarded unconditionally and the analyser is right to insist.
    thread::MutexLock lock(&state->mu);
    state->registry = action_registry != nullptr
                          ? std::move(action_registry)
                          : std::make_shared<actions::ActionRegistry>();
    state->on_connection = std::move(on_connection);
  }
  state->options = std::move(options);
  return std::shared_ptr<Service>(new Service(std::move(state)));
}

Service::~Service() {
  // Deliberately non-blocking. Draining here would submit a fiber and Await it
  // from a destructor -- which, when the destructor runs during interpreter or
  // process shutdown, has no scheduler left to make progress on and never
  // returns; that is a process that ignores SIGTERM and has to be killed.
  //
  // Draining is something an application asks for, explicitly, while it still
  // has a runtime: Drain(), or the Python facade's aclose(). What a destructor
  // owes is only that nothing is left accepting and nothing is left running.
  // Both of these are mutex-only, and every in-flight connection task holds its
  // own reference to the state, so abandoning them here is safe.
  (void)StopAccepting();
  (void)Abort(absl::CancelledError("The service was destroyed"));
}

std::shared_ptr<actions::ActionRegistry> Service::GetActionRegistry() const {
  thread::MutexLock lock(&state_->mu);
  return state_->registry;
}

absl::Status Service::SetActionRegistry(
    std::shared_ptr<actions::ActionRegistry> action_registry) {
  if (action_registry == nullptr) {
    return absl::InvalidArgumentError("The action registry must not be null");
  }
  std::vector<std::shared_ptr<Session>> live;
  {
    thread::MutexLock lock(&state_->mu);
    state_->registry = action_registry;
    if (!state_->options.copy_registry_per_connection) {
      live.reserve(state_->sessions.size());
      for (const auto& [id, session] : state_->sessions) {
        live.push_back(session);
      }
    }
  }
  // Outside the lock: a session's own mutex is not ours to take under ours.
  for (const std::shared_ptr<Session>& session : live) {
    ABSL_RETURN_IF_ERROR(session->SetActionRegistry(action_registry));
  }
  return absl::OkStatus();
}

absl::Status Service::SetOnConnection(OnServiceConnection on_connection) {
  thread::MutexLock lock(&state_->mu);
  state_->on_connection = std::move(on_connection);
  return absl::OkStatus();
}

a11::Task Service::Serve(std::shared_ptr<net::WireStream> stream,
                         StreamMode mode) {
  std::shared_ptr<State> state = state_;
  return a11::SubmitTask(
      [state = std::move(state), stream = std::move(stream),
       mode]() -> absl::Status {
        ABSL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                              Admit(state, stream, mode));
        const std::string session_id = session->GetId();
        const absl::Status status = session->Done().Await().status();
        state->Unregister(session_id);
        if (!status.ok()) {
          LOG(INFO) << "session " << session_id << " finished: " << status;
        }
        return status;
      });
}

absl::StatusOr<std::shared_ptr<Session>> Service::StartStreamHandler(
    std::shared_ptr<net::WireStream> stream, StreamMode mode) {
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                        Admit(state_, stream, mode));
  // The service owns completion from here; the caller just wanted the handle.
  std::shared_ptr<State> state = state_;
  const std::string session_id = session->GetId();
  a11::Task watcher = a11::SubmitTask(
      [state, session, session_id]() -> absl::Status {
        const absl::Status status = session->Done().Await().status();
        state->Unregister(session_id);
        return status;
      });
  (void)watcher;
  return session;
}

absl::Status Service::AddStreamToSession(
    std::string_view session_id, std::shared_ptr<net::WireStream> stream,
    StreamMode mode) {
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                        GetSession(session_id));
  ABSL_ASSIGN_OR_RETURN(a11::Task attached, session->AddStream(stream, mode));
  {
    thread::MutexLock lock(&state_->mu);
    state_->stream_to_session.emplace(stream->GetId(), std::string(session_id));
  }
  return attached.Await().status();
}

std::vector<std::string> Service::SessionIds() const {
  thread::MutexLock lock(&state_->mu);
  std::vector<std::string> ids;
  ids.reserve(state_->sessions.size());
  for (const auto& [id, session] : state_->sessions) {
    ids.push_back(id);
  }
  return ids;
}

absl::StatusOr<std::shared_ptr<Session>> Service::GetSession(
    std::string_view session_id) const {
  thread::MutexLock lock(&state_->mu);
  const auto it = state_->sessions.find(session_id);
  if (it == state_->sessions.end()) {
    return absl::NotFoundError(
        absl::StrCat("No session ", session_id, " on this service"));
  }
  return it->second;
}

absl::StatusOr<std::shared_ptr<Session>> Service::GetSessionForStream(
    std::string_view stream_id) const {
  thread::MutexLock lock(&state_->mu);
  const auto mapped = state_->stream_to_session.find(stream_id);
  if (mapped == state_->stream_to_session.end()) {
    return absl::NotFoundError(
        absl::StrCat("No session for stream ", stream_id));
  }
  const auto it = state_->sessions.find(mapped->second);
  if (it == state_->sessions.end()) {
    return absl::NotFoundError(
        absl::StrCat("No session for stream ", stream_id));
  }
  return it->second;
}

size_t Service::SessionCount() const {
  thread::MutexLock lock(&state_->mu);
  return state_->sessions.size();
}

bool Service::accepting() const {
  thread::MutexLock lock(&state_->mu);
  return state_->accepting;
}

absl::Status Service::StopAccepting() {
  thread::MutexLock lock(&state_->mu);
  state_->accepting = false;
  state_->idle.SignalAll();
  return absl::OkStatus();
}

a11::Task Service::Drain(absl::Duration timeout) {
  std::shared_ptr<State> state = state_;
  return a11::SubmitTask([state = std::move(state), timeout]() -> absl::Status {
    const absl::Time deadline = timeout == absl::InfiniteDuration()
                                    ? absl::InfiniteFuture()
                                    : absl::Now() + timeout;
    thread::MutexLock lock(&state->mu);
    while (!state->sessions.empty()) {
      if (absl::Now() >= deadline) {
        return absl::DeadlineExceededError(absl::StrCat(
            "The service still has ", state->sessions.size(),
            " session(s) after draining"));
      }
      state->idle.WaitWithDeadline(&state->mu, deadline);
    }
    return absl::OkStatus();
  });
}

absl::Status Service::Abort(absl::Status status) {
  std::vector<std::shared_ptr<Session>> live;
  {
    thread::MutexLock lock(&state_->mu);
    state_->accepting = false;
    live.reserve(state_->sessions.size());
    for (const auto& [id, session] : state_->sessions) {
      live.push_back(session);
    }
  }
  for (const std::shared_ptr<Session>& session : live) {
    (void)session->Abort(status);
  }
  return absl::OkStatus();
}

a11::Task Service::Done() const {
  std::shared_ptr<State> state = state_;
  return a11::SubmitTask([state = std::move(state)]() -> absl::Status {
    thread::MutexLock lock(&state->mu);
    while (state->accepting || !state->sessions.empty()) {
      state->idle.Wait(&state->mu);
    }
    return absl::OkStatus();
  });
}

}  // namespace a11::service

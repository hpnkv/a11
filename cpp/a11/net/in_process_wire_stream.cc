// Copyright 2026 The A11 Authors.

#include "a11/net/in_process_wire_stream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <execinfo.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/msgpack.h"
#include "a11/data/types.h"
#include "a11/net/internal/exception_guarded_callbacks.h"
#include "a11/net/wire_stream.h"
#include "a11/obs/span.h"
#include "a11/obs/tracer.h"
#include "a11/uuid.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

enum class End { kNone, kHalfClose, kAbort };

absl::Status InvokeMessageCallback(const OnMessage& callback,
                                   std::optional<data::WireMessage> message) {
  // Unguarded on purpose: StartEndpoint adopts both callbacks through
  // net/internal/exception_guarded_callbacks.h, so by the time either is
  // invoked a raised exception has already become the failed Task this awaits.
  a11::Task task = callback(std::move(message));
  return task.Await().status();
}

absl::Status InvokeDoneCallback(const OnDone& callback) {
  a11::Task task = callback();
  return task.Await().status();
}

}  // namespace

struct InProcessWireStream::State {
  struct Outbound {
    data::WireMessage message;
    End end = End::kNone;
  };

  State(WireStreamOptions stream_options, std::string stream_id)
      : options(stream_options),
        id(std::move(stream_id)),
        deadline(options.deadline),
        terminal_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        terminal_future(terminal_promise->future()),
        done_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        done_future(done_promise->future()) {}

  const WireStreamOptions options;
  const std::string id;
  // Wired once by CreatePair before either endpoint is published.
  std::weak_ptr<State> peer;
  mutable thread::Mutex mu;
  // One condition variable per thing that is waited for, rather than one per
  // endpoint. Split, each change reaches only the fiber whose predicate it can
  // make true.
  thread::CondVar outbound_cv;  // Sender, for something to send.
  thread::CondVar incoming_cv;  // Receiver, for something to deliver.
  thread::CondVar room_cv;      // The peer's Sender, for buffer room.
  thread::CondVar timing_cv;    // WatchTiming, for a deadline that moved in.
  std::deque<Outbound> outbound ABSL_GUARDED_BY(mu);
  std::deque<std::string> incoming ABSL_GUARDED_BY(mu);
  size_t incoming_bytes ABSL_GUARDED_BY(mu) = 0;
  // Set while somebody is delivering a message that is no longer in `outbound`:
  // either Sender, or the thread that called Send and took the fast path.
  bool sending ABSL_GUARDED_BY(mu) = false;

  bool started ABSL_GUARDED_BY(mu) = false;
  bool transport_finished ABSL_GUARDED_BY(mu) = false;
  End local_end ABSL_GUARDED_BY(mu) = End::kNone;
  End local_end_sent ABSL_GUARDED_BY(mu) = End::kNone;
  bool half_close_requested ABSL_GUARDED_BY(mu) = false;
  bool remote_half_closed ABSL_GUARDED_BY(mu) = false;
  bool remote_aborted ABSL_GUARDED_BY(mu) = false;
  bool implementation_aborted ABSL_GUARDED_BY(mu) = false;
  absl::Status status ABSL_GUARDED_BY(mu);
  std::optional<data::ByteMap> trailers ABSL_GUARDED_BY(mu);
  OnMessage on_message ABSL_GUARDED_BY(mu);
  OnDone on_done ABSL_GUARDED_BY(mu);
  bool done_called ABSL_GUARDED_BY(mu) = false;

  absl::Time deadline ABSL_GUARDED_BY(mu);
  absl::Time last_activity ABSL_GUARDED_BY(mu) = absl::InfinitePast();
  const std::shared_ptr<a11::Promise<a11::Unit>> terminal_promise;
  const a11::Task terminal_future;
  const std::shared_ptr<a11::Promise<a11::Unit>> done_promise;
  const a11::Task done_future;
  // Span covering this endpoint. Both endpoints share the stream id, so its
  // trace groups the two sides of the stream.
  obs::Span span ABSL_GUARDED_BY(mu);
};

absl::StatusOr<InProcessWireStream::Pair> InProcessWireStream::CreatePair(
    std::optional<WireStreamOptions> options,
    std::optional<WireStreamOptions> first_options,
    std::optional<WireStreamOptions> second_options,
    std::string preassigned_id) {
  if (options.has_value() &&
      (first_options.has_value() || second_options.has_value())) {
    return absl::InvalidArgumentError(
        "Supply shared options or endpoint-specific options, not both");
  }
  if (options.has_value()) {
    first_options = options;
    second_options = options;
  }
  first_options = first_options.value_or(WireStreamOptions{});
  second_options = second_options.value_or(WireStreamOptions{});
  ABSL_RETURN_IF_ERROR(first_options->Validate());
  ABSL_RETURN_IF_ERROR(second_options->Validate());
  const std::string id =
      preassigned_id.empty() ? NewStreamId() : std::move(preassigned_id);
  auto first_state = std::make_shared<State>(*first_options, id);
  auto second_state = std::make_shared<State>(*second_options, id);
  first_state->peer = second_state;
  second_state->peer = first_state;
  auto first =
      std::make_shared<InProcessWireStream>(ConstructorToken{}, first_state);
  auto second =
      std::make_shared<InProcessWireStream>(ConstructorToken{}, second_state);
  if (first_options->deadline <= absl::Now()) {
    ForceAbort(first_state,
               absl::DeadlineExceededError("WireStream deadline exceeded"));
  }
  if (second_options->deadline <= absl::Now()) {
    ForceAbort(second_state,
               absl::DeadlineExceededError("WireStream deadline exceeded"));
  }
  return Pair{std::move(first), std::move(second)};
}

absl::Status InProcessWireStream::Send(data::WireMessage message) {
  ABSL_RETURN_IF_ERROR(message.Validate());
  bool claimed = false;
  if (const std::shared_ptr<State> peer = state_->peer.lock()) {
    thread::MutexLock peer_lock(&peer->mu);
    if (peer->local_end == End::kAbort) {
      return absl::FailedPreconditionError(
          "The opposite side has aborted the stream");
    }
  }

  End end = End::kNone;
  if (data::IsHalfCloseMessage(message)) {
    ABSL_ASSIGN_OR_RETURN(data::ByteMap headers,
                          NormalizeWireHeaders(std::move(message.headers)));
    message.headers = std::move(headers);
    end = message.headers.find(kAbortStatusHeader) != message.headers.end()
              ? End::kAbort
              : End::kHalfClose;
  }
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->remote_aborted) {
      return absl::FailedPreconditionError(
          "The opposite side has aborted the stream");
    }
    if (state_->local_end != End::kNone) {
      return absl::FailedPreconditionError(
          "This endpoint has already half-closed or aborted");
    }
    if (state_->transport_finished) {
      return absl::FailedPreconditionError("The transport has finished");
    }
    if (state_->deadline <= absl::Now()) {
      // Abort outside the lock below.
    } else {
      state_->local_end = end;
      if (end == End::kHalfClose) {
        state_->half_close_requested = true;
      }
      if (end == End::kAbort) {
        state_->status =
            absl::AbortedError("The stream was aborted by this endpoint");
      }
      if (state_->span.IsRecording()) {
        state_->span.AddEvent(
            "a11.wire.send",
            {{"a11.wire.action_messages", absl::StrCat(message.actions.size())},
             {"a11.wire.node_fragments",
              absl::StrCat(message.node_fragments.size())},
             {"a11.wire.bytes", absl::StrCat(message.ApproxBytes())}});
      }
      // Deliver on this thread when there is nothing to get in the way. The
      // Sender fiber exists for the two things a caller of Send must not do:
      if (end == End::kNone && state_->outbound.empty() && !state_->sending) {
        state_->sending = true;
        claimed = true;
      } else {
        state_->outbound.push_back(
            State::Outbound{.message = std::move(message), .end = end});
        state_->outbound_cv.SignalAll();
        return absl::OkStatus();
      }
    }
  }
  if (claimed) {
    DeliverClaimed(state_, std::move(message));
    return absl::OkStatus();
  }
  ForceAbort(state_,
             absl::DeadlineExceededError("WireStream deadline exceeded"));
  return absl::FailedPreconditionError("WireStream deadline exceeded");
}

void InProcessWireStream::DeliverClaimed(const std::shared_ptr<State>& state,
                                         data::WireMessage message) {
  // Everything that can fail or has to wait puts the message back and lets
  // Sender do it: it is the only place that may block, and the only place that
  // turns a delivery failure into an abort.
  bool delivered = false;
  std::shared_ptr<State> peer;
  absl::StatusOr<std::string> payload = message.ToMsgpack();
  if (payload.ok()) {
    peer = state->peer.lock();
  }
  if (peer != nullptr) {
    thread::MutexLock peer_lock(&peer->mu);
    if (!peer->transport_finished &&
        peer->incoming.size() < peer->options.max_buffered_incoming_messages &&
        (peer->incoming_bytes == 0 ||
         peer->incoming_bytes + payload->size() <=
             peer->options.max_buffered_incoming_bytes)) {
      peer->incoming_bytes += payload->size();
      peer->incoming.push_back(std::move(*payload));
      peer->incoming_cv.SignalAll();
      delivered = true;
    }
  }
  {
    thread::MutexLock lock(&state->mu);
    state->sending = false;
    if (!delivered) {
      // The front, not the back: this message was ahead of anything queued
      // while the claim was held.
      state->outbound.push_front(
          State::Outbound{.message = std::move(message), .end = End::kNone});
    }
    if (!state->outbound.empty()) {
      state->outbound_cv.SignalAll();
    }
  }
  if (delivered) {
    MarkActivity(state, peer);
  }
}

a11::Task InProcessWireStream::Start(OnMessage on_message, OnDone on_done) {
  return StartEndpoint(std::move(on_message), std::move(on_done));
}

a11::Task InProcessWireStream::Accept(OnMessage on_message, OnDone on_done) {
  return StartEndpoint(std::move(on_message), std::move(on_done));
}

a11::Task InProcessWireStream::StartEndpoint(OnMessage on_message,
                                             OnDone on_done) {
  if (!on_message || !on_done) {
    return a11::FailedTask(
        absl::InvalidArgumentError("on_message and on_done must be callable"));
  }
  bool expired = false;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->started) {
      return a11::FailedTask(
          absl::FailedPreconditionError("The stream has already been started"));
    }
    state_->started = true;
    // Guarded on the way in, so the Receiver fiber can invoke them without a
    // try of its own -- see net/internal/exception_guarded_callbacks.h.
    state_->on_message = internal::GuardOnMessage(std::move(on_message));
    state_->on_done = internal::GuardOnDone(std::move(on_done));
    state_->last_activity = absl::Now();
    expired = state_->deadline <= absl::Now();
    state_->span = obs::Tracer::StartRootSpan(
        "a11.wire_stream", obs::SpanKind::kInternal, state_->id);
    if (state_->span.IsRecording()) {
      state_->span.SetAttribute("a11.stream.id", state_->id);
    }
  }
  a11::Schedule([state = state_]() { Sender(state); });
  a11::Schedule([state = state_]() { Receiver(state); });
  a11::Schedule([state = state_]() { WatchTiming(state); });
  if (expired) {
    ForceAbort(state_,
               absl::DeadlineExceededError("WireStream deadline exceeded"));
  }
  return a11::ReadyTask();
}

absl::Status InProcessWireStream::HalfClose(data::ByteMap trailers) {
  ABSL_ASSIGN_OR_RETURN(data::ByteMap normalized,
                        NormalizeWireHeaders(std::move(trailers)));
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->deadline <= absl::Now()) {
      // Force the deadline abort after releasing the lock.
    } else if (state_->local_end != End::kNone || state_->remote_aborted ||
               state_->transport_finished) {
      return absl::OkStatus();
    } else {
      state_->local_end = End::kHalfClose;
      state_->half_close_requested = true;
      state_->outbound.push_back(State::Outbound{
          .message = data::MakeHalfCloseMessage(std::move(normalized)),
          .end = End::kHalfClose,
      });
      state_->outbound_cv.SignalAll();
      return absl::OkStatus();
    }
  }
  ForceAbort(state_,
             absl::DeadlineExceededError("WireStream deadline exceeded"));
  return absl::OkStatus();
}

a11::Task InProcessWireStream::DrainOutgoingMessages() {
  thread::MutexLock lock(&state_->mu);
  if (!state_->half_close_requested) {
    return a11::FailedTask(absl::FailedPreconditionError(
        "DrainOutgoingMessages requires HalfClose first"));
  }
  if (state_->local_end_sent == End::kHalfClose) {
    return a11::ReadyTask();
  }
  if (!state_->started) {
    return a11::FailedTask(absl::FailedPreconditionError(
        "The stream must be started before drain"));
  }
  return state_->terminal_future;
}

absl::Status InProcessWireStream::Abort(absl::Status status) {
  if (status.ok()) {
    return absl::InvalidArgumentError("Abort status must be non-OK");
  }
  ABSL_ASSIGN_OR_RETURN(std::string encoded, data::PackStatus(status));
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->deadline <= absl::Now()) {
      // Force deadline status below.
    } else if (state_->local_end != End::kNone || state_->remote_aborted ||
               state_->transport_finished) {
      return absl::OkStatus();
    } else {
      state_->status = absl::AbortedError(std::string(status.message()));
      state_->local_end = End::kAbort;
      data::WireMessage message;
      message.headers.emplace(std::string(kAbortStatusHeader),
                              std::move(encoded));
      state_->outbound.push_back(
          State::Outbound{.message = std::move(message), .end = End::kAbort});
      state_->outbound_cv.SignalAll();
      return absl::OkStatus();
    }
  }
  ForceAbort(state_,
             absl::DeadlineExceededError("WireStream deadline exceeded"));
  return absl::OkStatus();
}

absl::Status InProcessWireStream::SetDeadline(absl::Time deadline) {
  bool expired = false;
  {
    thread::MutexLock lock(&state_->mu);
    state_->deadline = deadline;
    expired = deadline <= absl::Now();
    state_->timing_cv.SignalAll();
  }
  if (expired) {
    ForceAbort(state_,
               absl::DeadlineExceededError("WireStream deadline exceeded"));
  }
  return absl::OkStatus();
}

a11::Task InProcessWireStream::Done() const {
  return state_->done_future;
}

absl::Time InProcessWireStream::deadline() const {
  thread::MutexLock lock(&state_->mu);
  return state_->deadline;
}

absl::Status InProcessWireStream::GetStatus() const {
  bool expired = false;
  {
    thread::MutexLock lock(&state_->mu);
    expired = state_->deadline <= absl::Now() &&
              state_->local_end == End::kNone && !state_->transport_finished;
  }
  if (expired) {
    ForceAbort(state_,
               absl::DeadlineExceededError("WireStream deadline exceeded"));
  }
  thread::MutexLock lock(&state_->mu);
  return state_->status;
}

std::optional<data::ByteMap> InProcessWireStream::GetTrailers() const {
  thread::MutexLock lock(&state_->mu);
  return state_->trailers;
}

std::string InProcessWireStream::GetId() const {
  return state_->id;
}

void* absl_nullable InProcessWireStream::GetImpl() const {
  return nullptr;
}

// Merged frames stay small on purpose.
constexpr size_t kMergeCeilingBytes = 64 * 1024;

void InProcessWireStream::Sender(const std::shared_ptr<State>& state) {
  // Holds `State::sending` while this Sender is delivering one message, and
  // wakes Sender again on the way out if more arrived meanwhile. Local, because
  // State is this class's own business.
  class Claim {
   public:
    explicit Claim(const std::shared_ptr<State>& state) : state_(state) {}

    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;

    ~Claim() {
      thread::MutexLock lock(&state_->mu);
      state_->sending = false;
      if (!state_->outbound.empty()) {
        state_->outbound_cv.SignalAll();
      }
    }

   private:
    const std::shared_ptr<State>& state_;
  };

  while (true) {
    State::Outbound outbound;
    {
      thread::MutexLock lock(&state->mu);
      // `sending` means a Send is delivering the message that was at the front,
      // and popping the next one now would put it on the wire first.
      while ((state->outbound.empty() || state->sending) &&
             !state->transport_finished) {
        state->outbound_cv.Wait(&state->mu);
      }
      if (state->transport_finished) {
        return;
      }
      outbound = std::move(state->outbound.front());
      state->outbound.pop_front();
      if (state->implementation_aborted && outbound.end != End::kAbort) {
        continue;
      }

      // Fold in whatever is already waiting behind it. An action costs `3 + 2 x
      // ports` wire messages and most carry three bytes of status -- a dispatch
      // status, a completion status, one `a11-close` marker per output port.
      if (outbound.end == End::kNone &&
          outbound.message.ApproxBytes() < kMergeCeilingBytes) {
        size_t approximate = outbound.message.ApproxBytes();
        while (!state->outbound.empty()) {
          const State::Outbound& next = state->outbound.front();
          if (next.end != End::kNone ||
              next.message.headers != outbound.message.headers) {
            break;
          }
          const size_t addition = next.message.ApproxBytes();
          if (approximate + addition > kMergeCeilingBytes) {
            break;
          }
          approximate += addition;
          State::Outbound merged = std::move(state->outbound.front());
          state->outbound.pop_front();
          auto& fragments = outbound.message.node_fragments;
          fragments.insert(
              fragments.end(),
              std::make_move_iterator(merged.message.node_fragments.begin()),
              std::make_move_iterator(merged.message.node_fragments.end()));
          auto& actions = outbound.message.actions;
          actions.insert(
              actions.end(),
              std::make_move_iterator(merged.message.actions.begin()),
              std::make_move_iterator(merged.message.actions.end()));
        }
      }
      // Claimed as the last thing under the lock: from here until the release
      // below, no Send may deliver ahead of this message. See Send.
      state->sending = true;
    }
    // Releases the claim however this iteration ends -- delivered, abandoned on
    // an encode failure, or returning for good.
    const Claim claim(state);

    absl::StatusOr<std::string> payload = outbound.message.ToMsgpack();
    if (!payload.ok()) {
      ForceAbort(state, payload.status());
      continue;
    }
    std::shared_ptr<State> peer = state->peer.lock();
    if (peer == nullptr) {
      ForceAbort(state, absl::InternalError("In-process stream has no peer"));
      return;
    }
    absl::Status send_status;
    {
      thread::MutexLock peer_lock(&peer->mu);
      while (!peer->transport_finished &&
             (peer->incoming.size() >=
                  peer->options.max_buffered_incoming_messages ||
              (peer->incoming_bytes != 0 &&
               peer->incoming_bytes + payload->size() >
                   peer->options.max_buffered_incoming_bytes))) {
        peer->room_cv.Wait(&peer->mu);
      }
      if (peer->transport_finished) {
        send_status = absl::UnavailableError("In-process peer has closed");
      } else {
        peer->incoming_bytes += payload->size();
        peer->incoming.push_back(std::move(*payload));
        peer->incoming_cv.SignalAll();
      }
    }
    if (!send_status.ok()) {
      ForceAbort(state, send_status);
      return;
    }
    MarkActivity(state, peer);

    if (outbound.end != End::kNone) {
      std::shared_ptr<a11::Promise<a11::Unit>> terminal;
      {
        thread::MutexLock lock(&state->mu);
        state->local_end_sent = outbound.end;
        terminal = state->terminal_promise;
      }
      terminal->SetValue(a11::Unit{}).IgnoreError();
      MaybeFinish(state);
      return;
    }
  }
}

void InProcessWireStream::Receiver(const std::shared_ptr<State>& state) {
  while (true) {
    std::string payload;
    {
      thread::MutexLock lock(&state->mu);
      while (state->incoming.empty() && !state->transport_finished) {
        state->incoming_cv.Wait(&state->mu);
      }
      if (state->transport_finished) {
        return;
      }
      payload = std::move(state->incoming.front());
      state->incoming.pop_front();
      state->incoming_bytes -= payload.size();
      state->room_cv.SignalAll();
    }
    if (payload.size() > state->options.max_single_message_size) {
      ForceAbort(state, absl::OutOfRangeError(
                            "Incoming WireMessage exceeds the size limit"));
      return;
    }
    absl::StatusOr<data::WireMessage> message =
        data::WireMessage::FromMsgpack(payload);
    if (!message.ok()) {
      ForceAbort(state, message.status());
      return;
    }
    MarkActivity(state, state->peer.lock());
    if (!data::IsHalfCloseMessage(*message)) {
      OnMessage callback;
      bool data_after_terminal = false;
      {
        thread::MutexLock lock(&state->mu);
        if (state->remote_half_closed || state->remote_aborted) {
          data_after_terminal = true;
        } else {
          callback = state->on_message;
        }
      }
      if (data_after_terminal) {
        ForceAbort(state, absl::FailedPreconditionError(
                              "Peer sent data after a terminal message"));
        return;
      }
      const absl::Status callback_status =
          InvokeMessageCallback(callback, std::move(*message));
      if (!callback_status.ok()) {
        ForceAbort(state, callback_status);
        return;
      }
      continue;
    }

    absl::StatusOr<data::ByteMap> headers =
        NormalizeWireHeaders(std::move(message->headers));
    if (!headers.ok()) {
      ForceAbort(state, headers.status());
      return;
    }
    const auto abort = headers->find(kAbortStatusHeader);
    if (abort != headers->end()) {
      absl::StatusOr<absl::Status> remote_status =
          data::UnpackStatus(abort->second);
      absl::Status recorded =
          remote_status.ok() ? *remote_status : remote_status.status();
      if (recorded.ok()) {
        recorded = absl::AbortedError(std::string(recorded.message()));
      }
      {
        thread::MutexLock lock(&state->mu);
        state->remote_aborted = true;
        state->trailers.reset();
        if (state->status.ok()) {
          state->status = recorded;
        }
      }
      Finish(state);
      return;
    }

    OnMessage callback;
    {
      thread::MutexLock lock(&state->mu);
      state->remote_half_closed = true;
      state->trailers = std::move(*headers);
      callback = state->on_message;
    }
    const absl::Status callback_status =
        InvokeMessageCallback(callback, std::nullopt);
    if (!callback_status.ok()) {
      ForceAbort(state, callback_status);
      return;
    }
    MaybeFinish(state);
    return;
  }
}

void InProcessWireStream::WatchTiming(const std::shared_ptr<State>& state) {
  while (true) {
    absl::Time wake;
    bool deadline_is_first = false;
    {
      thread::MutexLock lock(&state->mu);
      if (state->transport_finished) {
        return;
      }
      const absl::Time inactivity =
          state->options.message_timeout == absl::InfiniteDuration()
              ? absl::InfiniteFuture()
              : state->last_activity + state->options.message_timeout;
      wake = std::min(state->deadline, inactivity);
      deadline_is_first = state->deadline <= inactivity;
      if (wake > absl::Now()) {
        state->timing_cv.WaitWithDeadline(&state->mu, wake);
        continue;
      }
    }
    const absl::Status status =
        deadline_is_first
            ? absl::DeadlineExceededError("WireStream deadline exceeded")
            : absl::DeadlineExceededError(
                  "Timed out waiting for WireStream activity");
    ForceAbort(state, status);
    return;
  }
}

void InProcessWireStream::MarkActivity(const std::shared_ptr<State>& first,
                                       const std::shared_ptr<State>& second) {
  // Only an endpoint with a message timeout has anybody to tell.
  const bool wanted =
      (first && first->options.message_timeout != absl::InfiniteDuration()) ||
      (second && second->options.message_timeout != absl::InfiniteDuration());
  if (!wanted) {
    return;
  }
  const absl::Time now = absl::Now();
  if (first && first->options.message_timeout != absl::InfiniteDuration()) {
    thread::MutexLock lock(&first->mu);
    first->last_activity = now;
  }
  if (second && second != first &&
      second->options.message_timeout != absl::InfiniteDuration()) {
    thread::MutexLock lock(&second->mu);
    second->last_activity = now;
  }
}

bool InProcessWireStream::ForceAbort(const std::shared_ptr<State>& state,
                                     absl::Status status) {
  if (status.ok()) {
    status = absl::InternalError("Invalid OK transport abort");
  }
  absl::StatusOr<std::string> encoded = data::PackStatus(status);
  if (!encoded.ok()) {
    status = encoded.status();
    encoded = data::PackStatus(status);
    if (!encoded.ok()) {
      return false;
    }
  }
  {
    thread::MutexLock lock(&state->mu);
    if (state->transport_finished || state->remote_aborted ||
        state->local_end == End::kAbort) {
      return false;
    }
    state->status = status;
    state->trailers.reset();
    state->local_end = End::kAbort;
    state->implementation_aborted = true;
    if (state->local_end_sent == End::kNone) {
      state->outbound.clear();
      data::WireMessage message;
      message.headers.emplace(std::string(kAbortStatusHeader),
                              std::move(*encoded));
      state->outbound.push_back(
          State::Outbound{.message = std::move(message), .end = End::kAbort});
    }
    state->outbound_cv.SignalAll();
  }
  return true;
}

void InProcessWireStream::MaybeFinish(const std::shared_ptr<State>& state) {
  bool finish = false;
  {
    thread::MutexLock lock(&state->mu);
    finish =
        state->remote_aborted || state->local_end_sent == End::kAbort ||
        (state->local_end_sent == End::kHalfClose && state->remote_half_closed);
  }
  if (finish) {
    Finish(state);
  }
}

void InProcessWireStream::Finish(const std::shared_ptr<State>& state) {
  OnDone callback;
  obs::Span span;
  absl::Status span_status;
  {
    thread::MutexLock lock(&state->mu);
    if (state->transport_finished) {
      return;
    }
    state->transport_finished = true;
    state->outbound.clear();
    state->outbound_cv.SignalAll();
    state->incoming_cv.SignalAll();
    state->room_cv.SignalAll();
    state->timing_cv.SignalAll();
    if (!state->done_called && state->on_done) {
      state->done_called = true;
      callback = state->on_done;
    }
    span = std::move(state->span);
    span_status = state->status;
  }
  if (span.IsRecording()) {
    span.SetStatus(span_status);
    span.End();
  }
  const absl::Status callback_status =
      callback ? InvokeDoneCallback(callback) : absl::OkStatus();
  if (!callback_status.ok()) {
    thread::MutexLock lock(&state->mu);
    if (state->status.ok()) {
      state->status = callback_status;
    }
  }
  const absl::Status completed =
      callback_status.ok() ? state->done_promise->SetValue(a11::Unit{})
                           : state->done_promise->SetStatus(callback_status);
  (void)completed;
}

}  // namespace a11::net

// Copyright 2026 The A11 Authors.

#include "a11/net/wire_stream_with_recv.h"

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/internal/exception_guarded_callbacks.h"
#include "a11/net/wire_stream.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"
#include "thread/select.h"
#include "thread/selectables.h"

namespace a11::net {
namespace {

struct ReceiveCancellation {
  // Accessed only while the owning WireStreamWithRecv::State mutex is held.
  bool requested = false;
};

}  // namespace

struct WireStreamWithRecv::State {
  mutable thread::Mutex mu;
  std::deque<std::optional<data::WireMessage>> queue ABSL_GUARDED_BY(mu);
  std::optional<absl::Status> error ABSL_GUARDED_BY(mu);
  bool done ABSL_GUARDED_BY(mu) = false;
  bool remote_half_closed ABSL_GUARDED_BY(mu) = false;
  bool eof_delivered ABSL_GUARDED_BY(mu) = false;
  std::shared_ptr<thread::PermanentEvent> changed ABSL_GUARDED_BY(mu) =
      std::make_shared<thread::PermanentEvent>();
};

absl::StatusOr<std::shared_ptr<WireStreamWithRecv>> WireStreamWithRecv::Create(
    std::shared_ptr<WireStream> stream) {
  if (stream == nullptr) {
    return absl::InvalidArgumentError("stream must not be null");
  }
  std::string id;
  id = stream->GetId();
  return std::make_shared<WireStreamWithRecv>(ConstructorToken{},
                                              std::move(stream), std::move(id),
                                              std::make_shared<State>());
}

absl::Status WireStreamWithRecv::Send(data::WireMessage message) {
  return stream_->Send(std::move(message));
}

a11::Task WireStreamWithRecv::Start(OnMessage on_message, OnDone on_done) {
  return StartImpl(false, std::move(on_message), std::move(on_done));
}

a11::Task WireStreamWithRecv::Accept(OnMessage on_message, OnDone on_done) {
  return StartImpl(true, std::move(on_message), std::move(on_done));
}

a11::Task WireStreamWithRecv::Start() {
  return StartImpl(false, {}, {});
}

a11::Task WireStreamWithRecv::Accept() {
  return StartImpl(true, {}, {});
}

a11::Task WireStreamWithRecv::StartImpl(bool accept, OnMessage on_message,
                                        OnDone on_done) {
  std::shared_ptr<WireStreamWithRecv> self = shared_from_this();
  // Guarded here rather than at the invocations in HandleMessage/HandleDone,
  // which run on fibers of A11's own. See
  // net/internal/exception_guarded_callbacks.h.
  OnMessage internal_message =
      [self, observer = internal::GuardOnMessage(std::move(on_message))](
          std::optional<data::WireMessage> message) mutable {
        return self->HandleMessage(observer, std::move(message));
      };
  OnDone internal_done =
      [self, observer = internal::GuardOnDone(std::move(on_done))]() mutable {
        return self->HandleDone(observer);
      };
  return accept ? stream_->Accept(std::move(internal_message),
                                  std::move(internal_done))
                : stream_->Start(std::move(internal_message),
                                 std::move(internal_done));
}

absl::Status WireStreamWithRecv::HalfClose(data::ByteMap trailers) {
  return stream_->HalfClose(std::move(trailers));
}

a11::Task WireStreamWithRecv::DrainOutgoingMessages() {
  return stream_->DrainOutgoingMessages();
}

absl::Status WireStreamWithRecv::Abort(absl::Status status) {
  absl::Status result;
  result = stream_->Abort(std::move(status));
  RecordCurrentStatus();
  return result;
}

absl::Status WireStreamWithRecv::SetDeadline(absl::Time deadline) {
  absl::Status result;
  result = stream_->SetDeadline(deadline);
  RecordCurrentStatus();
  return result;
}

absl::Time WireStreamWithRecv::deadline() const {
  return stream_->deadline();
}

absl::Status WireStreamWithRecv::GetStatus() const {
  return stream_->GetStatus();
}

std::optional<data::ByteMap> WireStreamWithRecv::GetTrailers() const {
  return stream_->GetTrailers();
}

std::string WireStreamWithRecv::GetId() const {
  return id_;
}

void* absl_nullable WireStreamWithRecv::GetImpl() const {
  return stream_->GetImpl();
}

a11::Future<std::optional<data::WireMessage>> WireStreamWithRecv::Receive(
    absl::Duration timeout) {
  if (timeout < absl::ZeroDuration()) {
    return a11::FailedFuture<std::optional<data::WireMessage>>(
        absl::InvalidArgumentError("timeout must not be negative"));
  }
  std::shared_ptr<WireStreamWithRecv> self = shared_from_this();
  auto cancellation = std::make_shared<ReceiveCancellation>();
  std::shared_ptr<State> state = self->state_;
  return a11::SubmitWithCancellationHook<std::optional<data::WireMessage>>(
      [self = std::move(self), timeout,
       cancellation]() -> absl::StatusOr<std::optional<data::WireMessage>> {
        const absl::Time deadline = timeout == absl::InfiniteDuration()
                                        ? absl::InfiniteFuture()
                                        : absl::Now() + timeout;
        while (true) {
          self->RecordCurrentStatus();
          std::shared_ptr<thread::PermanentEvent> changed;
          std::shared_ptr<thread::PermanentEvent> notify;
          std::optional<data::WireMessage> result;
          bool has_result = false;
          {
            thread::MutexLock lock(&self->state_->mu);
            if (cancellation->requested) {
              return absl::CancelledError("WireStream receive was cancelled");
            }
            if (self->state_->error.has_value()) {
              return *self->state_->error;
            }
            if (self->state_->eof_delivered) {
              return absl::FailedPreconditionError(
                  "The remote WireStream half-close was already received");
            }
            if (!self->state_->queue.empty()) {
              result = std::move(self->state_->queue.front());
              self->state_->queue.pop_front();
              has_result = true;
              if (!result.has_value()) {
                self->state_->eof_delivered = true;
              }
              notify =
                  std::exchange(self->state_->changed,
                                std::make_shared<thread::PermanentEvent>());
            } else if (self->state_->done) {
              return absl::InternalError(
                  "WireStream finished without a remote half-close");
            } else {
              changed = self->state_->changed;
            }
          }
          if (notify) {
            notify->Notify();
          }
          if (has_result) {
            return result;
          }

          const int selected = thread::SelectUntil(
              deadline, {thread::OnCancel(), changed->OnEvent()});
          if (selected == 0) {
            return absl::CancelledError("WireStream receive was cancelled");
          }
          if (selected < 0) {
            return absl::DeadlineExceededError(
                "WireStream receive timed out before a message was available");
          }
        }
      },
      [state = std::move(state), cancellation]() {
        thread::MutexLock lock(&state->mu);
        cancellation->requested = true;
      },
      {.stack_size = 256});
}

a11::Task WireStreamWithRecv::HandleMessage(
    OnMessage observer, std::optional<data::WireMessage> message) {
  std::shared_ptr<WireStreamWithRecv> self = shared_from_this();
  return a11::SubmitTask(
      [self = std::move(self), observer = std::move(observer),
       message = std::move(message)]() mutable -> absl::Status {
        while (true) {
          std::shared_ptr<thread::PermanentEvent> changed;
          std::shared_ptr<thread::PermanentEvent> notify;
          {
            thread::MutexLock lock(&self->state_->mu);
            if (self->state_->error.has_value()) {
              return absl::OkStatus();
            }

            if (self->state_->remote_half_closed) {
              return absl::InternalError(
                  "WireStream delivered data after remote half-close");
            }

            if (self->state_->queue.empty()) {
              if (!message.has_value()) {
                self->state_->remote_half_closed = true;
              }
              self->state_->queue.push_back(message);
              notify =
                  std::exchange(self->state_->changed,
                                std::make_shared<thread::PermanentEvent>());
            } else {
              changed = self->state_->changed;
            }
          }

          if (notify) {
            notify->Notify();
            break;
          }

          const int selected =
              thread::Select({thread::OnCancel(), changed->OnEvent()});
          if (selected == 0) {
            return absl::CancelledError(
                "WireStream receive callback was cancelled");
          }
        }

        if (!observer) {
          return absl::OkStatus();
        }
        return observer(message).Await().status();
      },
      {.stack_size = 256});
}

a11::Task WireStreamWithRecv::HandleDone(OnDone observer) {
  std::shared_ptr<WireStreamWithRecv> self = shared_from_this();
  return a11::SubmitTask(
      [self = std::move(self),
       observer = std::move(observer)]() mutable -> absl::Status {
        const absl::Status stream_status = self->GetStatus();
        std::shared_ptr<thread::PermanentEvent> notify;
        {
          thread::MutexLock lock(&self->state_->mu);
          self->state_->done = true;
          if (!stream_status.ok() && !self->state_->error.has_value()) {
            self->state_->error = stream_status;
            self->state_->queue.clear();
          }
          notify = std::exchange(self->state_->changed,
                                 std::make_shared<thread::PermanentEvent>());
        }
        notify->Notify();

        if (!observer) {
          return absl::OkStatus();
        }
        return observer().Await().status();
      },
      {.stack_size = 256});
}

void WireStreamWithRecv::RecordCurrentStatus() const {
  if (absl::Status status = GetStatus(); !status.ok()) {
    SignalError(std::move(status));
  }
}

void WireStreamWithRecv::SignalError(absl::Status status) const {
  if (status.ok()) {
    return;
  }
  std::shared_ptr<thread::PermanentEvent> notify;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->error.has_value()) {
      return;
    }
    state_->error = std::move(status);
    state_->queue.clear();
    notify = std::exchange(state_->changed,
                           std::make_shared<thread::PermanentEvent>());
  }
  notify->Notify();
}

}  // namespace a11::net

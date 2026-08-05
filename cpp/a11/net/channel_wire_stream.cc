// Copyright 2026 The A11 Authors.

#include "a11/net/channel_wire_stream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <absl/base/thread_annotations.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/msgpack.h"
#include "a11/data/types.h"
#include "a11/net/byte_chunking.h"
#include "a11/net/internal/binary_channel.h"
#include "a11/net/wire_stream.h"
#include "a11/obs/span.h"
#include "a11/obs/tracer.h"
#include "a11/status.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"
#include "thread/select.h"
#include "thread/selectables.h"

namespace a11::net {
namespace {

enum class End { kNone, kHalfClose, kAbort };

absl::Status ExternalError(const std::exception& error) {
  return absl::UnknownError(error.what());
}

}  // namespace

struct ChannelWireStream::State {
  struct Outbound {
    std::string bytes;
    End end = End::kNone;
    std::uint64_t message_id = 0;
  };

  State(std::shared_ptr<internal::BinaryChannel> value_channel,
        std::string value_id, ChannelEndpointRole value_role,
        OpenOperation operation, WireStreamOptions value_options,
        ChannelFramingOptions value_framing)
      : channel(std::move(value_channel)),
        id(std::move(value_id)),
        role(value_role),
        open_operation(std::move(operation)),
        options(value_options),
        framing(value_framing),
        reassembler(ByteChunkingOptions{
            .packet_size = value_framing.split_size,
            .max_message_size = value_options.max_single_message_size,
            .max_pending_messages = value_framing.max_pending_messages,
            .max_pending_bytes = value_framing.max_pending_bytes}),
        deadline(value_options.deadline),
        startup_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        startup_future(startup_promise->future()),
        drain_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        drain_future(drain_promise->future()),
        changed(std::make_shared<thread::PermanentEvent>()) {}

  ~State() {
    std::shared_ptr<thread::PermanentEvent> event;
    {
      thread::MutexLock lock(&mu);
      event = changed;
    }
    event->Notify();
  }

  mutable thread::Mutex mu;
  const std::shared_ptr<internal::BinaryChannel> channel;
  const std::string id;
  const ChannelEndpointRole role;
  const OpenOperation open_operation;
  const WireStreamOptions options;
  const ChannelFramingOptions framing;
  ByteReassembler reassembler;
  absl::Time deadline ABSL_GUARDED_BY(mu);
  absl::Time last_activity ABSL_GUARDED_BY(mu) = absl::Now();
  OnMessage on_message ABSL_GUARDED_BY(mu);
  OnDone on_done ABSL_GUARDED_BY(mu);
  bool started ABSL_GUARDED_BY(mu) = false;
  bool open ABSL_GUARDED_BY(mu) = false;
  bool channel_closed ABSL_GUARDED_BY(mu) = false;
  bool finished ABSL_GUARDED_BY(mu) = false;
  bool done_called ABSL_GUARDED_BY(mu) = false;
  End local_end ABSL_GUARDED_BY(mu) = End::kNone;
  End local_end_sent ABSL_GUARDED_BY(mu) = End::kNone;
  bool remote_half_closed ABSL_GUARDED_BY(mu) = false;
  bool remote_aborted ABSL_GUARDED_BY(mu) = false;
  absl::Status status ABSL_GUARDED_BY(mu);
  std::optional<data::ByteMap> trailers ABSL_GUARDED_BY(mu);
  std::deque<Outbound> outgoing ABSL_GUARDED_BY(mu);
  std::deque<std::string> incoming ABSL_GUARDED_BY(mu);
  size_t incoming_bytes ABSL_GUARDED_BY(mu) = 0;
  std::uint64_t next_outgoing_message_id ABSL_GUARDED_BY(mu) = 1;
  const std::shared_ptr<a11::Promise<a11::Unit>> startup_promise;
  const a11::Task startup_future;
  const std::shared_ptr<a11::Promise<a11::Unit>> drain_promise;
  const a11::Task drain_future;
  std::shared_ptr<thread::PermanentEvent> changed ABSL_GUARDED_BY(mu);
  // Span covering this endpoint; its trace is pinned to the stream id.
  obs::Span span ABSL_GUARDED_BY(mu);
};

absl::Status ChannelFramingOptions::Validate() const {
  if (split_size < 18 || split_size > 1024 * 1024) {
    return absl::InvalidArgumentError(
        "channel split_size must be in [18, 1048576]");
  }
  if (max_pending_messages == 0 || max_pending_bytes == 0) {
    return absl::InvalidArgumentError(
        "channel pending reassembly limits must be positive");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<ChannelWireStream::State>>
ChannelWireStream::MakeState(std::shared_ptr<internal::BinaryChannel> channel,
                             std::string id, ChannelEndpointRole role,
                             OpenOperation open_operation,
                             WireStreamOptions options,
                             ChannelFramingOptions framing) {
  if (channel == nullptr) {
    return absl::InvalidArgumentError("channel must not be null");
  }
  if (id.empty()) {
    return absl::InvalidArgumentError("id must not be empty");
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  ABSL_RETURN_IF_ERROR(framing.Validate());
  ABSL_RETURN_IF_ERROR(
      (ByteChunkingOptions{.packet_size = framing.split_size,
                           .max_message_size = options.max_single_message_size,
                           .max_pending_messages = framing.max_pending_messages,
                           .max_pending_bytes = framing.max_pending_bytes}
           .Validate()));
  return std::make_shared<State>(std::move(channel), std::move(id), role,
                                 std::move(open_operation), options, framing);
}

ChannelWireStream::~ChannelWireStream() {
  (void)state_->channel->ResetCallbacks();
  (void)state_->channel->Close();
}

void ChannelWireStream::Notify(const std::shared_ptr<State>& state) {
  std::shared_ptr<thread::PermanentEvent> event;
  {
    thread::MutexLock lock(&state->mu);
    event = std::exchange(state->changed,
                          std::make_shared<thread::PermanentEvent>());
  }
  event->Notify();
}

absl::Status ChannelWireStream::Send(data::WireMessage message) {
  ABSL_RETURN_IF_ERROR(message.Validate());
  End end = End::kNone;
  if (data::IsHalfCloseMessage(message)) {
    ABSL_ASSIGN_OR_RETURN(data::ByteMap headers,
                          NormalizeWireHeaders(std::move(message.headers)));
    message.headers = std::move(headers);
    end = message.headers.find(kAbortStatusHeader) != message.headers.end()
              ? End::kAbort
              : End::kHalfClose;
  }
  ABSL_ASSIGN_OR_RETURN(std::string bytes, message.ToMsgpack());
  bool queued = false;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->remote_aborted) {
      return absl::FailedPreconditionError("The peer aborted the stream");
    }
    if (state_->local_end != End::kNone || state_->finished) {
      return absl::FailedPreconditionError(
          "This endpoint has already terminated");
    }
    if (state_->deadline <= absl::Now()) {
      // Abort after releasing the mutex.
    } else {
      state_->local_end = end;
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
             {"a11.wire.bytes", absl::StrCat(bytes.size())}});
      }
      const std::uint64_t message_id = state_->next_outgoing_message_id++;
      state_->outgoing.push_back(State::Outbound{
          .bytes = std::move(bytes), .end = end, .message_id = message_id});
      queued = true;
    }
  }
  if (queued) {
    Notify(state_);
    return absl::OkStatus();
  }
  ForceAbort(state_,
             absl::DeadlineExceededError("WireStream deadline exceeded"),
             false);
  return absl::FailedPreconditionError("WireStream deadline exceeded");
}

a11::Task ChannelWireStream::Start(OnMessage on_message, OnDone on_done) {
  return StartEndpoint(false, std::move(on_message), std::move(on_done));
}

a11::Task ChannelWireStream::Accept(OnMessage on_message, OnDone on_done) {
  return StartEndpoint(true, std::move(on_message), std::move(on_done));
}

a11::Task ChannelWireStream::StartEndpoint(bool accept, OnMessage on_message,
                                           OnDone on_done) {
  if (!on_message || !on_done) {
    return a11::FailedTask(
        absl::InvalidArgumentError("on_message and on_done must be callable"));
  }
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->started) {
      return a11::FailedTask(
          absl::FailedPreconditionError("WireStream is already started"));
    }
    if ((accept && state_->role == ChannelEndpointRole::kClient) ||
        (!accept && state_->role == ChannelEndpointRole::kServer)) {
      return a11::FailedTask(absl::UnimplementedError(
          accept ? "This WireStream cannot accept"
                 : "This WireStream cannot start as a client"));
    }
    state_->started = true;
    state_->on_message = std::move(on_message);
    state_->on_done = std::move(on_done);
    state_->last_activity = absl::Now();
    state_->span = obs::Tracer::StartRootSpan(
        "a11.wire_stream", obs::SpanKind::kInternal, state_->id);
    if (state_->span.IsRecording()) {
      state_->span.SetAttribute("a11.stream.id", state_->id);
      state_->span.SetAttribute(
          "a11.stream.role",
          state_->role == ChannelEndpointRole::kClient ? "client" : "server");
    }
  }
  std::weak_ptr<State> weak = state_;
  internal::BinaryChannelCallbacks callbacks{
      .on_open =
          [weak]() {
            if (std::shared_ptr<State> state = weak.lock(); state != nullptr) {
              {
                thread::MutexLock lock(&state->mu);
                state->open = true;
                state->last_activity = absl::Now();
              }
              (void)state->startup_promise->SetValue(a11::Unit{});
              ChannelWireStream::Notify(state);
            }
          },
      .on_message =
          [weak](std::string packet) {
            std::shared_ptr<State> state = weak.lock();
            if (state == nullptr) {
              return;
            }
            absl::Status error;
            bool enqueued = false;
            try {
              thread::MutexLock lock(&state->mu);
              if (state->finished) {
                return;
              }
              absl::StatusOr<std::optional<std::string>> reassembled =
                  state->reassembler.Feed(std::move(packet));
              std::optional<std::string> complete;
              if (!reassembled.ok()) {
                error = reassembled.status();
              } else {
                complete = std::move(*reassembled);
              }
              if (error.ok() && complete.has_value()) {
                if (complete->size() > state->options.max_single_message_size) {
                  error = absl::OutOfRangeError(
                      "Incoming WireMessage exceeds max_single_message_size");
                } else if (state->incoming.size() >=
                               state->options.max_buffered_incoming_messages ||
                           (!state->incoming.empty() &&
                            state->incoming_bytes + complete->size() >
                                state->options.max_buffered_incoming_bytes)) {
                  error = absl::ResourceExhaustedError(
                      "Incoming WireMessage buffer capacity was exceeded");
                } else {
                  state->incoming_bytes += complete->size();
                  state->incoming.push_back(std::move(*complete));
                  state->last_activity = absl::Now();
                  enqueued = true;
                }
              }
            } catch (const std::exception& exception) {
              error = ExternalError(exception);
            } catch (...) {
              error = absl::UnknownError(
                  "Receiving channel data raised a non-standard exception");
            }
            if (!error.ok()) {
              ChannelWireStream::ForceAbort(state, std::move(error));
            } else if (enqueued) {
              ChannelWireStream::Notify(state);
            }
          },
      .on_error =
          [weak](absl::Status error) {
            if (std::shared_ptr<State> state = weak.lock(); state != nullptr) {
              ChannelWireStream::Finish(state, std::move(error));
            }
          },
      .on_closed =
          [weak]() {
            std::shared_ptr<State> state = weak.lock();
            if (state == nullptr) {
              return;
            }
            bool expected = false;
            {
              thread::MutexLock lock(&state->mu);
              state->channel_closed = true;
              expected = state->finished || state->remote_aborted ||
                         state->local_end_sent == End::kAbort ||
                         (state->local_end_sent == End::kHalfClose &&
                          state->remote_half_closed);
            }
            if (!expected) {
              ChannelWireStream::Finish(
                  state, absl::UnavailableError(
                             "Channel closed before A11 termination"));
            } else {
              ChannelWireStream::MaybeFinish(state);
            }
            ChannelWireStream::Notify(state);
          },
      .on_buffered_amount_low =
          [weak]() {
            if (std::shared_ptr<State> state = weak.lock(); state != nullptr) {
              ChannelWireStream::Notify(state);
            }
          }};
  absl::Status configured = state_->channel->SetCallbacks(std::move(callbacks));
  if (!configured.ok()) {
    Finish(state_, configured);
    return a11::FailedTask(configured);
  }

  a11::Schedule([state = state_]() { Sender(std::move(state)); });
  a11::Schedule([state = state_]() { Receiver(std::move(state)); });
  a11::Schedule([state = state_]() { WatchTiming(std::move(state)); });

  absl::StatusOr<bool> already_open = state_->channel->IsOpen();
  if (!already_open.ok()) {
    Finish(state_, already_open.status());
    return a11::FailedTask(already_open.status());
  }
  bool expired = false;
  {
    thread::MutexLock lock(&state_->mu);
    expired =
        state_->deadline <= absl::Now() || state_->local_end == End::kAbort;
    if (*already_open) {
      state_->open = true;
    }
  }
  if (*already_open) {
    (void)state_->startup_promise->SetValue(a11::Unit{});
    Notify(state_);
  }
  if (expired) {
    ForceAbort(state_,
               absl::DeadlineExceededError("WireStream deadline exceeded"),
               false);
    Finish(state_);
    (void)state_->startup_promise->SetValue(a11::Unit{});
    return state_->startup_future;
  }
  if (state_->open_operation) {
    absl::Status opened;
    try {
      opened = state_->open_operation();
    } catch (const std::exception& error) {
      opened = ExternalError(error);
    } catch (...) {
      opened = absl::UnknownError("Channel open operation raised an exception");
    }
    if (!opened.ok()) {
      Finish(state_, opened);
      (void)state_->startup_promise->SetStatus(opened);
      return state_->startup_future;
    }
  }
  absl::Status opened = state_->channel->Open();
  if (!opened.ok()) {
    Finish(state_, opened);
    (void)state_->startup_promise->SetStatus(opened);
  }
  return state_->startup_future;
}

absl::Status ChannelWireStream::HalfClose(data::ByteMap trailers) {
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->local_end != End::kNone || state_->finished) {
      return absl::OkStatus();
    }
  }
  ABSL_ASSIGN_OR_RETURN(data::ByteMap normalized,
                        NormalizeWireHeaders(std::move(trailers)));
  return Send(data::MakeHalfCloseMessage(std::move(normalized)));
}

a11::Task ChannelWireStream::DrainOutgoingMessages() {
  thread::MutexLock lock(&state_->mu);
  if (state_->local_end != End::kHalfClose) {
    return a11::FailedTask(absl::FailedPreconditionError(
        "DrainOutgoingMessages requires HalfClose first"));
  }
  if (state_->local_end_sent == End::kHalfClose) {
    return a11::ReadyTask();
  }
  return state_->drain_future;
}

absl::Status ChannelWireStream::Abort(absl::Status status) {
  if (status.ok()) {
    return absl::InvalidArgumentError("Abort status must be non-OK");
  }
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->local_end != End::kNone || state_->finished) {
      return absl::OkStatus();
    }
  }
  ABSL_ASSIGN_OR_RETURN(std::string packed, data::PackStatus(status));
  data::WireMessage message;
  message.headers.emplace(std::string(kAbortStatusHeader), std::move(packed));
  return Send(std::move(message));
}

absl::Status ChannelWireStream::SetDeadline(absl::Time deadline) {
  bool expired = false;
  {
    thread::MutexLock lock(&state_->mu);
    state_->deadline = deadline;
    expired = deadline <= absl::Now() && !state_->finished;
  }
  Notify(state_);
  if (expired) {
    ForceAbort(state_,
               absl::DeadlineExceededError("WireStream deadline exceeded"),
               true);
  }
  return absl::OkStatus();
}

absl::Time ChannelWireStream::deadline() const {
  thread::MutexLock lock(&state_->mu);
  return state_->deadline;
}

absl::Status ChannelWireStream::GetStatus() const {
  bool expired = false;
  {
    thread::MutexLock lock(&state_->mu);
    expired = state_->deadline <= absl::Now() && !state_->finished;
  }
  if (expired) {
    ForceAbort(state_,
               absl::DeadlineExceededError("WireStream deadline exceeded"),
               true);
  }
  thread::MutexLock lock(&state_->mu);
  return state_->status;
}

std::optional<data::ByteMap> ChannelWireStream::GetTrailers() const {
  thread::MutexLock lock(&state_->mu);
  return state_->trailers;
}

std::string ChannelWireStream::GetId() const {
  thread::MutexLock lock(&state_->mu);
  return state_->id;
}

void* absl_nullable ChannelWireStream::GetImpl() const {
  return state_->channel->GetImpl();
}

void ChannelWireStream::Sender(std::shared_ptr<State> state) {
  while (true) {
    std::shared_ptr<thread::PermanentEvent> changed;
    State::Outbound outbound;
    bool has_outbound = false;
    {
      thread::MutexLock lock(&state->mu);
      if (state->finished) {
        return;
      }
      if (state->started && state->open && !state->outgoing.empty()) {
        outbound = std::move(state->outgoing.front());
        state->outgoing.pop_front();
        has_outbound = true;
      } else {
        changed = state->changed;
      }
    }
    if (!has_outbound) {
      if (thread::Select({thread::OnCancel(), changed->OnEvent()}) == 0) {
        return;
      }
      continue;
    }
    try {
      absl::StatusOr<std::vector<std::string>> packets = SplitBytesIntoPackets(
          outbound.bytes, outbound.message_id, state->framing.split_size);
      if (!packets.ok()) {
        Finish(state, packets.status());
        return;
      }
      for (std::string& packet : *packets) {
        absl::Status sent = state->channel->Send(std::move(packet));
        if (!sent.ok()) {
          Finish(state, std::move(sent));
          return;
        }
      }
    } catch (const std::exception& error) {
      Finish(state, ExternalError(error));
      return;
    } catch (...) {
      Finish(state, absl::UnknownError("Channel send raised an exception"));
      return;
    }
    MarkActivity(state);
    if (outbound.end != End::kNone) {
      // Send only accepts packets into a transport queue. Wait for that queue
      // to drain before publishing the terminal message as sent.
      while (true) {
        std::shared_ptr<thread::PermanentEvent> drain_changed;
        {
          thread::MutexLock lock(&state->mu);
          if (state->finished) {
            return;
          }
          drain_changed = state->changed;
        }
        absl::StatusOr<size_t> buffered_amount =
            state->channel->BufferedAmount();
        if (!buffered_amount.ok()) {
          Finish(state, buffered_amount.status());
          return;
        }
        if (*buffered_amount == 0) {
          break;
        }
        if (thread::Select({thread::OnCancel(), drain_changed->OnEvent()}) ==
            0) {
          return;
        }
      }
      {
        thread::MutexLock lock(&state->mu);
        state->local_end_sent = outbound.end;
      }
      if (outbound.end == End::kHalfClose) {
        (void)state->drain_promise->SetValue(a11::Unit{});
      }
      MaybeFinish(state);
      return;
    }
  }
}

void ChannelWireStream::Receiver(std::shared_ptr<State> state) {
  while (true) {
    std::shared_ptr<thread::PermanentEvent> changed;
    std::string bytes;
    bool has_message = false;
    {
      thread::MutexLock lock(&state->mu);
      if (state->finished) {
        return;
      }
      if (!state->incoming.empty()) {
        bytes = std::move(state->incoming.front());
        state->incoming.pop_front();
        state->incoming_bytes -= bytes.size();
        has_message = true;
      } else {
        changed = state->changed;
      }
    }
    if (!has_message) {
      if (thread::Select({thread::OnCancel(), changed->OnEvent()}) == 0) {
        return;
      }
      continue;
    }
    absl::StatusOr<data::WireMessage> message =
        data::WireMessage::FromMsgpack(bytes);
    if (!message.ok()) {
      ForceAbort(state, message.status());
      continue;
    }
    MarkActivity(state);
    if (!data::IsHalfCloseMessage(*message)) {
      OnMessage callback;
      bool invalid = false;
      {
        thread::MutexLock lock(&state->mu);
        invalid = state->remote_half_closed || state->remote_aborted;
        callback = state->on_message;
      }
      if (invalid) {
        ForceAbort(state, absl::FailedPreconditionError(
                              "Peer sent data after a terminal message"));
        continue;
      }
      absl::Status callback_status;
      try {
        callback_status = callback(std::move(*message)).Await().status();
      } catch (const std::exception& error) {
        callback_status = ExternalError(error);
      } catch (...) {
        callback_status = absl::UnknownError("on_message raised an exception");
      }
      if (!callback_status.ok()) {
        ForceAbort(state, callback_status);
      }
      continue;
    }

    absl::StatusOr<data::ByteMap> headers =
        NormalizeWireHeaders(std::move(message->headers));
    if (!headers.ok()) {
      ForceAbort(state, headers.status());
      continue;
    }
    const auto abort = headers->find(kAbortStatusHeader);
    if (abort != headers->end()) {
      absl::StatusOr<absl::Status> decoded = data::UnpackStatus(abort->second);
      absl::Status status = decoded.ok() ? *decoded : decoded.status();
      if (status.ok()) {
        status = absl::AbortedError(std::string(status.message()));
      }
      {
        thread::MutexLock lock(&state->mu);
        state->remote_aborted = true;
        state->trailers.reset();
        if (state->status.ok()) {
          state->status = status;
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
    absl::Status callback_status;
    try {
      callback_status = callback(std::nullopt).Await().status();
    } catch (const std::exception& error) {
      callback_status = ExternalError(error);
    } catch (...) {
      callback_status = absl::UnknownError("on_message raised an exception");
    }
    if (!callback_status.ok()) {
      ForceAbort(state, callback_status);
    } else {
      MaybeFinish(state);
    }
    return;
  }
}

void ChannelWireStream::WatchTiming(std::shared_ptr<State> state) {
  while (true) {
    absl::Time wake;
    bool deadline_first = false;
    std::shared_ptr<thread::PermanentEvent> changed;
    {
      thread::MutexLock lock(&state->mu);
      if (state->finished) {
        return;
      }
      const absl::Time inactivity =
          state->options.message_timeout == absl::InfiniteDuration()
              ? absl::InfiniteFuture()
              : state->last_activity + state->options.message_timeout;
      wake = std::min(state->deadline, inactivity);
      deadline_first = state->deadline <= inactivity;
      changed = state->changed;
    }
    const int selected =
        thread::SelectUntil(wake, {thread::OnCancel(), changed->OnEvent()});
    if (selected == 0) {
      return;
    }
    if (selected > 0) {
      continue;
    }
    ForceAbort(state,
               deadline_first
                   ? absl::DeadlineExceededError("WireStream deadline exceeded")
                   : absl::DeadlineExceededError(
                         "Timed out waiting for WireStream activity"),
               true);
    return;
  }
}

void ChannelWireStream::MarkActivity(const std::shared_ptr<State>& state) {
  {
    thread::MutexLock lock(&state->mu);
    state->last_activity = absl::Now();
  }
  Notify(state);
}

void ChannelWireStream::ForceAbort(const std::shared_ptr<State>& state,
                                   absl::Status status, bool can_communicate) {
  if (status.ok()) {
    status = absl::InternalError("Invalid OK stream abort");
  }
  absl::StatusOr<std::string> packed = data::PackStatus(status);
  bool finish_now = false;
  {
    thread::MutexLock lock(&state->mu);
    if (state->finished || state->remote_aborted ||
        state->local_end == End::kAbort) {
      return;
    }
    state->status = status;
    state->trailers.reset();
    state->local_end = End::kAbort;
    state->outgoing.clear();
    if (can_communicate && state->open && packed.ok()) {
      data::WireMessage message;
      message.headers.emplace(std::string(kAbortStatusHeader),
                              std::move(*packed));
      absl::StatusOr<std::string> bytes = message.ToMsgpack();
      if (bytes.ok()) {
        state->outgoing.push_back(
            State::Outbound{.bytes = std::move(*bytes),
                            .end = End::kAbort,
                            .message_id = state->next_outgoing_message_id++});
      } else {
        finish_now = true;
      }
    } else {
      finish_now = true;
    }
  }
  Notify(state);
  if (finish_now) {
    Finish(state);
  }
}

void ChannelWireStream::MaybeFinish(const std::shared_ptr<State>& state) {
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

void ChannelWireStream::Finish(const std::shared_ptr<State>& state,
                               std::optional<absl::Status> terminal_error) {
  OnDone callback;
  std::shared_ptr<a11::Promise<a11::Unit>> startup;
  bool close_channel = terminal_error.has_value();
  obs::Span span;
  absl::Status span_status;
  {
    thread::MutexLock lock(&state->mu);
    if (state->finished) {
      return;
    }
    state->finished = true;
    if (terminal_error.has_value() && state->status.ok()) {
      state->status = *terminal_error;
    }
    close_channel = close_channel || !state->status.ok();
    state->incoming.clear();
    state->incoming_bytes = 0;
    startup = state->startup_promise;
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
  if (terminal_error.has_value()) {
    (void)startup->SetStatus(*terminal_error);
  } else {
    (void)startup->SetValue(a11::Unit{});
  }
  Notify(state);
  // A clean bidirectional A11 half-close is not itself an acknowledgement
  // that the peer has received our final transport packets. In particular,
  // SCTP stream reset can overtake callback delivery for a large message on
  // the peer. Keep a clean channel alive until its WireStream owner releases
  // it; error paths still close immediately. The destructor provides the
  // deterministic RAII close.
  if (close_channel) {
    (void)state->channel->Close();
  }
  if (callback) {
    absl::Status callback_status;
    try {
      callback_status = callback().Await().status();
    } catch (const std::exception& error) {
      callback_status = ExternalError(error);
    } catch (...) {
      callback_status = absl::UnknownError("on_done raised an exception");
    }
    if (!callback_status.ok()) {
      thread::MutexLock lock(&state->mu);
      if (state->status.ok()) {
        state->status = callback_status;
      }
    }
  }
}

}  // namespace a11::net

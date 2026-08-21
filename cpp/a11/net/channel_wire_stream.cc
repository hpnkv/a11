// Copyright 2026 The A11 Authors.

#include "a11/net/channel_wire_stream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
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
#include "a11/net/internal/exception_guarded_callbacks.h"
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

}  // namespace

struct ChannelWireStream::State {
  struct Outbound {
    // The message, not its bytes. Encoding here rather than in Send() is what
    // lets the sender fold messages already queued behind one another into a
    // single frame -- see the merge in Sender() -- and it moves the encode off
    // the caller's thread, which for a Python caller is the event loop.
    data::WireMessage message;
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
  // Set while somebody is writing a message that is no longer in `outgoing`:
  // either Sender, or the thread that called Send and took the fast path. It is
  // what keeps those two from writing at once, and so what keeps them in order.
  // See Send and DeliverClaimed.
  bool sending ABSL_GUARDED_BY(mu) = false;
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
  // Callbacks first: nothing inbound may run against a half-destroyed stream.
  (void)state_->channel->ResetCallbacks();
  // Then *abort*, not Close(). This is the last moment anything will speak for
  // this stream, and `BinaryChannel::Close()` is a graceful close -- over HTTP it
  // ends the request, so a peer still holding its half keeps the socket alive on
  // both ends and the descriptor is never returned. Measured before this changed:
  // dropping the last reference to a stream grew the descriptor count by
  // +1.000/connection on each side and stranded a Session, on a probe that ran a
  // 30s reap and a 60s settle window (`bench/fdprobe.py --teardown drop`).
  //
  // Being graceful here would also have nobody to be graceful *for*: Finish()
  // deliberately leaves a cleanly half-closed channel open so that a peer's
  // in-flight final packets are not cut off by an SCTP stream reset, and it names
  // this destructor as the deterministic release that ends that grace period. So
  // the grace period ends here, by construction.
  //
  // Nothing else may happen in this frame. `Finish()` is not called and `on_done`
  // is not run: a done callback invoked from a destructor would run on whichever
  // thread dropped the last reference, and a Python caller dropping a stream holds
  // the GIL, which is the deadlock item 0a in FINDINGS.md already paid for once.
  (void)state_->channel->Abort(
      absl::CancelledError("WireStream released without an explicit close"));
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
  bool claimed = false;
  std::uint64_t claimed_id = 0;
  End end = End::kNone;
  if (data::IsHalfCloseMessage(message)) {
    ABSL_ASSIGN_OR_RETURN(data::ByteMap headers,
                          NormalizeWireHeaders(std::move(message.headers)));
    message.headers = std::move(headers);
    end = message.headers.find(kAbortStatusHeader) != message.headers.end()
              ? End::kAbort
              : End::kHalfClose;
  }
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
             {"a11.wire.bytes", absl::StrCat(message.ApproxBytes())}});
      }
      const std::uint64_t message_id = state_->next_outgoing_message_id++;
      // Write it here rather than waking the Sender fibre to do it.
      //
      // The fibre is needed for the things a caller of Send must not be made to
      // do: wait for the channel to open, and carry the lifecycle of a
      // half-close or abort through to the transport's drain. An ordinary
      // message on an open channel with an empty queue needs none of that, and
      // handing it over costs a scheduling round trip -- plus an OS thread wake
      // when the fibre's worker is parked -- to move bytes this thread already
      // holds.
      //
      // The claim is what keeps order: while it is held, Sender will not pop
      // and another Send will not take this path, so nothing can overtake the
      // message being written. The message id is still allocated here, under
      // the lock, so ids stay in send order however the message travels.
      if (end == End::kNone && state_->outgoing.empty() && !state_->sending &&
          state_->started && state_->open) {
        state_->sending = true;
        claimed = true;
        claimed_id = message_id;
      } else {
        state_->outgoing.push_back(
            State::Outbound{.message = std::move(message),
                            .end = end,
                            .message_id = message_id});
        queued = true;
      }
    }
  }
  if (claimed) {
    // `claimed` and `queued` are the two arms of one if/else above, and only
    // the queueing arm moves the message: whichever of them ran, the message is
    // moved exactly once. NOLINTNEXTLINE(bugprone-use-after-move)
    DeliverClaimed(state_, message, claimed_id);
    return absl::OkStatus();
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
    // Guarded on the way in, so the Receiver and Finish paths below can invoke
    // them from A11's own fibres without a try of their own. See
    // net/internal/exception_guarded_callbacks.h.
    state_->on_message = internal::GuardOnMessage(std::move(on_message));
    state_->on_done = internal::GuardOnDone(std::move(on_done));
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
            {
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
                  if (state->options.message_timeout !=
                      absl::InfiniteDuration()) {
                    state->last_activity = absl::Now();
                  }
                  enqueued = true;
                }
              }
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

  a11::Schedule([state = state_]() { Sender(state); });
  a11::Schedule([state = state_]() { Receiver(state); });
  a11::Schedule([state = state_]() { WatchTiming(state); });

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
    // The open operation belongs to the transport that built this stream --
    // websocket_wire_stream.cc, webrtc_wire_stream.cc -- and reports through
    // its Status like the rest of A11.
    const absl::Status opened = state_->open_operation();
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
  bool upgrade = false;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->finished || state_->local_end == End::kAbort) {
      return absl::OkStatus();
    }
    // Aborting an already half-closed stream used to return OK and do nothing,
    // which cost one file descriptor per connection and could not be observed
    // from the outside.
    //
    // A half-closed connection is still a connection: the write half is shut,
    // the peer's is open, and the socket is legitimately held until the peer
    // closes too. So the only way for a client that is going away to release
    // the socket is to abort -- and this method silently refused to, because
    // `local_end` was no longer kNone. Measured on Linux, 120 connect/dispatch/
    // disconnect cycles against a real Service, sampling the server's
    // /proc/<pid>/fd by kind: abort() alone held flat at 16 descriptors,
    // half_close() plus drain retained exactly +1.000 per cycle, and
    // half_close() then abort() retained the same +1.000 -- all of them
    // ESTABLISHED, none released after twelve seconds of settling. See
    // `bench/fdprobe.py`, and `FINDINGS.md` item 0, whose "server-side teardown
    // lags under load" reading this replaces: nothing lagged, the abort was a
    // no-op, and the growth was 1:1 with half-closed connections rather than
    // time-dependent.
    //
    // Upgrading locally rather than sending, and `can_communicate=false` is the
    // load-bearing part. The Sender fibre `return`s after it publishes *any*
    // terminal message (see the `outbound.end != End::kNone` branch), so once a
    // half-close has gone out there is nobody left to carry an abort frame:
    // queueing one would leave `local_end_sent` at kHalfClose, MaybeFinish
    // would never fire, and this fix would silently do nothing -- the same
    // failure it is fixing, one layer down.
    //
    // So the abort is local. ForceAbort promotes `local_end` to kAbort, drops
    // whatever is still queued (which is what abort has always meant --
    // "discarding buffered work" -- and a caller that wanted its queue
    // delivered has DrainOutgoingMessages), and finishes; Finish closes the
    // channel because the status is non-OK, which is what returns the
    // descriptor. The peer learns by its connection ending, which is what a
    // client going away looks like on any transport. It therefore sees a
    // transport-level end rather than this status, and that is honest: the
    // status describes why *this* side left, and nothing on the wire could
    // carry it after the half-close.
    upgrade = state_->local_end == End::kHalfClose;
  }
  if (upgrade) {
    // On a fibre, not here. ForceAbort with nothing to send finishes the stream
    // in the same frame, and finishing runs `on_done` -- the session's stream
    // teardown, and through a binding a callback that needs the interpreter.
    // The ordinary abort path never did that on the caller's thread: it queues a
    // message and the *sender* fibre finishes. Running it inline instead
    // deadlocked a Python caller against its own loop, because the binding
    // releases the GIL to call this and the done callback then waits to
    // re-acquire it. Abort has always been "accepted", not "completed", so
    // handing it to a fibre costs the caller nothing it was promised.
    std::shared_ptr<State> state = state_;
    a11::Schedule([state = std::move(state), status = std::move(status)]() {
      ForceAbort(state, status, /*can_communicate=*/false);
    });
    return absl::OkStatus();
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

// Merged frames stay small on purpose.
//
// The whole gain is in folding together the three-byte status markers an
// action trails -- a dispatch status, a completion status, a close marker per
// port. Large data messages get nothing from it: they are already one frame
// per payload, merging them only builds a bigger frame to split again, and it
// would erase message boundaries a peer can reasonably expect to see
// preserved. So accumulate only up to this much and leave anything bigger
// alone.
constexpr size_t kMergeCeilingBytes = 64 * 1024;

void ChannelWireStream::Sender(const std::shared_ptr<State>& state) {
  // Holds `State::sending` while this Sender writes one message, and wakes it
  // again on the way out if more arrived meanwhile.
  class Claim {
   public:
    explicit Claim(const std::shared_ptr<State>& state) : state_(state) {}

    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;

    ~Claim() {
      bool more = false;
      {
        thread::MutexLock lock(&state_->mu);
        state_->sending = false;
        more = !state_->outgoing.empty();
      }
      if (more) {
        Notify(state_);
      }
    }

   private:
    const std::shared_ptr<State>& state_;
  };

  while (true) {
    std::shared_ptr<thread::PermanentEvent> changed;
    State::Outbound outbound;
    bool has_outbound = false;
    {
      thread::MutexLock lock(&state->mu);
      if (state->finished) {
        return;
      }
      if (state->started && state->open && !state->outgoing.empty() &&
          !state->sending) {
        outbound = std::move(state->outgoing.front());
        state->outgoing.pop_front();
        state->sending = true;
        has_outbound = true;
        // Fold in whatever is already waiting behind it. Nothing is ever held
        // back to build a bigger frame: only messages already queued merge, so
        // a lone message still goes out immediately. Worth ~15% on concurrent
        // action throughput, where messages belonging to different actions
        // arrive together. Only compatible ones merge -- no lifecycle marker,
        // identical headers, and within the peer's per-message ceiling.
        // A message already at the ceiling has nothing to gain and is left
        // exactly as it is.
        if (outbound.end == End::kNone &&
            outbound.message.ApproxBytes() < kMergeCeilingBytes) {
          size_t approximate = outbound.message.ApproxBytes();
          while (!state->outgoing.empty()) {
            const State::Outbound& next = state->outgoing.front();
            if (next.end != End::kNone ||
                next.message.headers != outbound.message.headers) {
              break;
            }
            const size_t addition = next.message.ApproxBytes();
            if (approximate + addition > kMergeCeilingBytes) {
              break;
            }
            approximate += addition;
            State::Outbound merged = std::move(state->outgoing.front());
            state->outgoing.pop_front();
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
    // Releases the claim however this iteration ends -- written, abandoned on
    // an encode failure, or returning for good.
    const Claim claim(state);
    {
      // An encode failure here aborts the stream, the same way a framing
      // failure below does. Structural problems were already rejected
      // synchronously by Validate() in Send(); anything left is internal.
      absl::StatusOr<std::string> encoded = outbound.message.ToMsgpack();
      if (!encoded.ok()) {
        Finish(state, encoded.status());
        return;
      }
      absl::StatusOr<std::vector<std::string>> packets =
          SplitOwnedBytesIntoPackets(std::move(*encoded), outbound.message_id,
                                     state->framing.split_size);
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

void ChannelWireStream::Receiver(const std::shared_ptr<State>& state) {
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
      const absl::Status callback_status =
          callback(std::move(*message)).Await().status();
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
    const absl::Status callback_status =
        callback(std::nullopt).Await().status();
    if (!callback_status.ok()) {
      ForceAbort(state, callback_status);
    } else {
      MaybeFinish(state);
    }
    return;
  }
}

void ChannelWireStream::WatchTiming(const std::shared_ptr<State>& state) {
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

void ChannelWireStream::DeliverClaimed(const std::shared_ptr<State>& state,
                                       const data::WireMessage& message,
                                       std::uint64_t message_id) {
  // The same encode, packetise and write the Sender fibre would have done, and
  // the same treatment of a failure: abort the stream. Send still reports OK
  // either way, because that is what it reported when this work happened on the
  // fibre -- a transport failure reaches the caller through the stream's
  // lifecycle, not through the return of the send that happened to trigger it.
  absl::Status failure;
  {
    absl::StatusOr<std::string> encoded = message.ToMsgpack();
    if (!encoded.ok()) {
      failure = encoded.status();
    } else {
      absl::StatusOr<std::vector<std::string>> packets =
          SplitOwnedBytesIntoPackets(std::move(*encoded), message_id,
                                     state->framing.split_size);
      if (!packets.ok()) {
        failure = packets.status();
      } else {
        for (std::string& packet : *packets) {
          absl::Status sent = state->channel->Send(std::move(packet));
          if (!sent.ok()) {
            failure = std::move(sent);
            break;
          }
        }
      }
    }
  }

  bool more = false;
  {
    thread::MutexLock lock(&state->mu);
    state->sending = false;
    more = !state->outgoing.empty();
  }
  if (more) {
    Notify(state);
  }
  if (!failure.ok()) {
    // Off this thread, deliberately. Finish runs the stream's on_done callback,
    // which is user code, and on the fibre path it never ran on a caller of
    // Send. Keeping it there means the fast path cannot hand a caller somebody
    // else's callback -- re-entering the binding, say, from inside a send.
    a11::Schedule([state, failure = std::move(failure)]() mutable {
      Finish(state, std::move(failure));
    });
    return;
  }
  MarkActivity(state);
}

void ChannelWireStream::MarkActivity(const std::shared_ptr<State>& state) {
  // Only a stream with a message timeout has anybody to tell. Without one,
  // `last_activity` is never read, and this runs on every message sent and
  // every message received: a clock read, the endpoint's lock, and a Notify
  // that wakes all three of its fibres, to keep a number nothing would look at.
  if (state->options.message_timeout == absl::InfiniteDuration()) {
    return;
  }
  {
    thread::MutexLock lock(&state->mu);
    state->last_activity = absl::Now();
  }
  // No Notify even then. WatchTiming recomputes its deadline from
  // `last_activity` every time it wakes, so activity needs no prompt -- it will
  // find the newer timestamp and re-arm. Sender and Receiver are woken by the
  // things they actually wait for, in Send and in the inbound enqueue.
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
      // Encoded here only to find out whether it can be, since failing to
      // build the abort frame means there is nothing to send and the stream
      // should just finish. The sender encodes it again for real.
      if (message.ToMsgpack().ok()) {
        state->outgoing.push_back(
            State::Outbound{.message = std::move(message),
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
    // `close_channel` is only ever set because the status is non-OK, so this is
    // the abort path and a graceful close is the wrong operation: over HTTP it
    // half-closes a connection whose other half the peer still holds, and the
    // socket survives on both ends (see BinaryChannel::Abort). Measured: the
    // half-close-then-abort sequence went from +1.000 descriptors per
    // connection on each side to flat.
    (void)state->channel->Abort(span_status.ok()
                                    ? absl::CancelledError("WireStream aborted")
                                    : span_status);
  }
  if (callback) {
    const absl::Status callback_status = callback().Await().status();
    if (!callback_status.ok()) {
      thread::MutexLock lock(&state->mu);
      if (state->status.ok()) {
        state->status = callback_status;
      }
    }
  }
}

}  // namespace a11::net

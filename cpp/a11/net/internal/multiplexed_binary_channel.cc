// Copyright 2026 The A11 Authors.

#include "a11/net/internal/multiplexed_binary_channel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/net/internal/binary_channel.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"
#include "thread/select.h"
#include "thread/selectables.h"

namespace a11::net::internal {
namespace {

// Fixed 8-byte little-endian frame prefix carrying the aggregate send order.
constexpr size_t kSequencePrefix = sizeof(std::uint64_t);

std::string EncodeSequence(std::uint64_t sequence, std::string_view payload) {
  std::string framed;
  framed.reserve(kSequencePrefix + payload.size());
  for (size_t index = 0; index < kSequencePrefix; ++index) {
    framed.push_back(static_cast<char>(sequence & 0xffU));
    sequence >>= 8U;
  }
  framed.append(payload);
  return framed;
}

std::uint64_t DecodeSequence(std::string_view framed) {
  std::uint64_t sequence = 0;
  for (size_t index = 0; index < kSequencePrefix; ++index) {
    sequence |= static_cast<std::uint64_t>(
                    static_cast<unsigned char>(framed[index]))
                << (index * 8U);
  }
  return sequence;
}

}  // namespace

MultiplexedBinaryChannel::MultiplexedBinaryChannel(
    std::vector<std::shared_ptr<BinaryChannel>> initial_members,
    MemberChannelFactory factory, MultiplexedChannelOptions options)
    : factory_(std::move(factory)),
      options_(options),
      changed_(std::make_shared<thread::PermanentEvent>()) {
  for (std::shared_ptr<BinaryChannel>& channel : initial_members) {
    if (channel != nullptr) {
      members_.push_back(std::make_shared<Member>(
          Member{.id = next_member_id_++, .channel = std::move(channel)}));
    }
  }
}

std::shared_ptr<MultiplexedBinaryChannel> MultiplexedBinaryChannel::Create(
    std::vector<std::shared_ptr<BinaryChannel>> initial_members,
    MemberChannelFactory factory, MultiplexedChannelOptions options) {
  if (options.target_channels == 0)
    options.target_channels = 1;
  struct Enabler final : MultiplexedBinaryChannel {
    Enabler(std::vector<std::shared_ptr<BinaryChannel>> members,
            MemberChannelFactory factory, MultiplexedChannelOptions options)
        : MultiplexedBinaryChannel(std::move(members), std::move(factory),
                                   options) {}
  };
  return std::make_shared<Enabler>(std::move(initial_members),
                                   std::move(factory), options);
}

MultiplexedBinaryChannel::~MultiplexedBinaryChannel() {
  (void)Close();
}

void MultiplexedBinaryChannel::NotifyLocked() {
  std::shared_ptr<thread::PermanentEvent> event = std::exchange(
      changed_, std::make_shared<thread::PermanentEvent>());
  event->Notify();
}

size_t MultiplexedBinaryChannel::LiveCountLocked() const {
  size_t live = 0;
  for (const std::shared_ptr<Member>& member : members_) {
    if (member->open)
      ++live;
  }
  return live;
}

size_t MultiplexedBinaryChannel::LiveCount() const {
  thread::MutexLock lock(&mu_);
  return LiveCountLocked();
}

std::shared_ptr<MultiplexedBinaryChannel::Member>
MultiplexedBinaryChannel::AppendMemberLocked(
    std::shared_ptr<BinaryChannel> channel) {
  auto member = std::make_shared<Member>(
      Member{.id = next_member_id_++, .channel = std::move(channel)});
  members_.push_back(member);
  return member;
}

void MultiplexedBinaryChannel::WireMember(
    const std::shared_ptr<Member>& member) {
  const std::uint64_t id = member->id;
  std::weak_ptr<MultiplexedBinaryChannel> weak = weak_from_this();
  BinaryChannelCallbacks callbacks{
      .on_open =
          [weak, id]() {
            if (auto self = weak.lock(); self != nullptr)
              self->OnMemberOpen(id);
          },
      .on_message =
          [weak](std::string framed) {
            if (auto self = weak.lock(); self != nullptr)
              self->OnMemberMessage(std::move(framed));
          },
      .on_error =
          [weak, id](absl::Status status) {
            if (auto self = weak.lock(); self != nullptr)
              self->DropMember(id, status.message());
          },
      .on_closed =
          [weak, id]() {
            if (auto self = weak.lock(); self != nullptr)
              self->DropMember(id, "channel closed");
          },
      // Forward drain notifications to the aggregate so the sender's drain
      // barrier wakes, and try to push buffered packets onto the freed channel.
      .on_buffered_amount_low =
          [weak]() {
            std::shared_ptr<MultiplexedBinaryChannel> self = weak.lock();
            if (self == nullptr)
              return;
            std::function<void()> low;
            {
              thread::MutexLock lock(&self->mu_);
              low = self->callbacks_.on_buffered_amount_low;
            }
            self->FlushPending();
            if (low)
              low();
          }};
  absl::Status configured = member->channel->SetCallbacks(std::move(callbacks));
  if (!configured.ok()) {
    DropMember(id, configured.message());
    return;
  }
  // The transport may already be open before its callbacks were installed.
  absl::StatusOr<bool> already_open = member->channel->IsOpen();
  if (already_open.ok() && *already_open)
    OnMemberOpen(id);
}

absl::Status MultiplexedBinaryChannel::SetCallbacks(
    BinaryChannelCallbacks callbacks) {
  std::vector<std::shared_ptr<Member>> members;
  {
    thread::MutexLock lock(&mu_);
    callbacks_ = std::move(callbacks);
    callbacks_set_ = true;
    members = members_;
  }
  for (const std::shared_ptr<Member>& member : members)
    WireMember(member);
  return absl::OkStatus();
}

absl::Status MultiplexedBinaryChannel::ResetCallbacks() {
  std::vector<std::shared_ptr<Member>> members;
  {
    thread::MutexLock lock(&mu_);
    callbacks_ = BinaryChannelCallbacks{};
    callbacks_set_ = false;
    members = members_;
  }
  for (const std::shared_ptr<Member>& member : members)
    (void)member->channel->ResetCallbacks();
  return absl::OkStatus();
}

absl::Status MultiplexedBinaryChannel::Open() {
  {
    thread::MutexLock lock(&mu_);
    if (closed_)
      return absl::CancelledError("Multiplexed channel is closed");
  }
  // Only replenishes if the initial batch came up short; in steady state this
  // schedules nothing, so no fiber lingers to be torn down with the stream.
  MaybeReplenish();
  return absl::OkStatus();
}

void MultiplexedBinaryChannel::OnMemberOpen(std::uint64_t id) {
  bool announce = false;
  std::function<void()> on_open;
  {
    thread::MutexLock lock(&mu_);
    if (closed_)
      return;
    bool found = false;
    for (const std::shared_ptr<Member>& member : members_) {
      if (member->id == id) {
        found = member->open == false;
        member->open = true;
        break;
      }
    }
    if (!found)
      return;
    if (!any_open_) {
      any_open_ = true;
      announce = true;
      on_open = callbacks_.on_open;
    }
    NotifyLocked();
  }
  FlushPending();
  if (announce && on_open)
    on_open();
}

void MultiplexedBinaryChannel::OnMemberMessage(std::string framed) {
  if (framed.size() < kSequencePrefix) {
    // A frame without its sequence prefix breaks aggregate ordering; there is
    // no safe way to place it, so fail the whole stream.
    std::function<void(absl::Status)> on_error;
    {
      thread::MutexLock lock(&mu_);
      on_error = callbacks_.on_error;
    }
    if (on_error)
      on_error(absl::InvalidArgumentError(
          "Multiplexed member packet was malformed"));
    return;
  }
  const std::uint64_t sequence = DecodeSequence(framed);
  std::function<void(std::string)> on_message;
  std::function<void(absl::Status)> on_error;
  std::vector<std::string> ready;
  {
    thread::MutexLock lock(&mu_);
    if (closed_)
      return;
    if (sequence >= next_deliver_seq_ &&
        reorder_.find(sequence) == reorder_.end()) {
      if (reorder_.size() >= options_.max_reorder_packets) {
        on_error = callbacks_.on_error;
      } else {
        reorder_.emplace(sequence, framed.substr(kSequencePrefix));
      }
    }
    if (on_error == nullptr && !delivering_) {
      delivering_ = true;
      on_message = callbacks_.on_message;
      auto it = reorder_.begin();
      while (it != reorder_.end() && it->first == next_deliver_seq_) {
        ready.push_back(std::move(it->second));
        it = reorder_.erase(it);
        ++next_deliver_seq_;
      }
    }
  }
  if (on_error != nullptr) {
    on_error(absl::ResourceExhaustedError(
        "Multiplexed channel reorder buffer overflowed"));
    return;
  }
  if (on_message == nullptr)
    return;
  // Drain contiguous packets in order; keep draining anything that arrived
  // while we were delivering without the lock.
  while (true) {
    for (std::string& packet : ready)
      on_message(std::move(packet));
    ready.clear();
    thread::MutexLock lock(&mu_);
    auto it = reorder_.begin();
    while (it != reorder_.end() && it->first == next_deliver_seq_) {
      ready.push_back(std::move(it->second));
      it = reorder_.erase(it);
      ++next_deliver_seq_;
    }
    if (ready.empty()) {
      delivering_ = false;
      return;
    }
    on_message = callbacks_.on_message;
    if (on_message == nullptr) {
      delivering_ = false;
      return;
    }
  }
}

absl::Status MultiplexedBinaryChannel::Send(std::string bytes) {
  {
    thread::MutexLock lock(&mu_);
    if (closed_)
      return absl::CancelledError("Multiplexed channel is closed");
    std::string framed = EncodeSequence(next_send_seq_++, bytes);
    pending_out_bytes_ += framed.size();
    pending_out_.push_back(std::move(framed));
  }
  FlushPending();
  return absl::OkStatus();
}

void MultiplexedBinaryChannel::FlushPending() {
  {
    thread::MutexLock lock(&mu_);
    if (closed_)
      return;
    if (flushing_) {
      // Another flusher owns the queue; make sure it revisits it before exit.
      flush_again_ = true;
      return;
    }
    flushing_ = true;
  }
  // Single flusher from here: pending_out_.front() is stable across the
  // lock-free member send below, so a copy-then-pop-on-success reroutes a
  // failed packet to another live member with its sequence number intact.
  while (true) {
    std::string packet;
    std::shared_ptr<Member> chosen;
    {
      thread::MutexLock lock(&mu_);
      if (closed_) {
        flushing_ = false;
        return;
      }
      if (pending_out_.empty()) {
        if (flush_again_) {
          flush_again_ = false;
          continue;
        }
        flushing_ = false;
        return;
      }
      const size_t count = members_.size();
      for (size_t attempt = 0; attempt < count; ++attempt) {
        std::shared_ptr<Member>& candidate =
            members_[(round_robin_ + attempt) % count];
        if (candidate->open) {
          chosen = candidate;
          round_robin_ = (round_robin_ + attempt + 1) % count;
          break;
        }
      }
      if (chosen == nullptr) {
        // No live channel yet; leave packets buffered for a later flush.
        flushing_ = false;
        return;
      }
      packet = pending_out_.front();
    }
    absl::Status sent = chosen->channel->Send(packet);
    if (sent.ok()) {
      thread::MutexLock lock(&mu_);
      if (!pending_out_.empty()) {
        pending_out_bytes_ -= pending_out_.front().size();
        pending_out_.pop_front();
      }
      continue;
    }
    {
      thread::MutexLock lock(&mu_);
      chosen->open = false;
    }
    DropMember(chosen->id, "send failed");
  }
}

absl::StatusOr<size_t> MultiplexedBinaryChannel::BufferedAmount() const {
  std::vector<std::shared_ptr<Member>> members;
  size_t total = 0;
  {
    thread::MutexLock lock(&mu_);
    total = pending_out_bytes_;
    members = members_;
  }
  for (const std::shared_ptr<Member>& member : members) {
    absl::StatusOr<size_t> amount = member->channel->BufferedAmount();
    if (amount.ok())
      total += *amount;
  }
  return total;
}

absl::StatusOr<bool> MultiplexedBinaryChannel::IsOpen() const {
  thread::MutexLock lock(&mu_);
  return !closed_ && any_open_;
}

absl::Status MultiplexedBinaryChannel::Close() {
  std::vector<std::shared_ptr<Member>> members;
  {
    thread::MutexLock lock(&mu_);
    if (closed_)
      return absl::OkStatus();
    closed_ = true;
    members = members_;
    members_.clear();
    pending_out_.clear();
    pending_out_bytes_ = 0;
    reorder_.clear();
    NotifyLocked();
  }
  for (const std::shared_ptr<Member>& member : members) {
    (void)member->channel->ResetCallbacks();
    (void)member->channel->Close();
  }
  return absl::OkStatus();
}

void* absl_nullable MultiplexedBinaryChannel::GetImpl() const {
  thread::MutexLock lock(&mu_);
  for (const std::shared_ptr<Member>& member : members_) {
    if (member->open)
      return member->channel->GetImpl();
  }
  return members_.empty() ? nullptr : members_.front()->channel->GetImpl();
}

bool MultiplexedBinaryChannel::AddMember(
    std::shared_ptr<BinaryChannel> channel) {
  if (channel == nullptr)
    return false;
  std::shared_ptr<Member> member;
  bool wire = false;
  {
    thread::MutexLock lock(&mu_);
    if (closed_ || members_.size() >= options_.target_channels)
      return false;
    member = AppendMemberLocked(std::move(channel));
    wire = callbacks_set_;
    NotifyLocked();
  }
  if (wire)
    WireMember(member);
  return true;
}

void MultiplexedBinaryChannel::DropMember(std::uint64_t id,
                                          std::string_view reason) {
  std::shared_ptr<Member> dropped;
  bool fatal = false;
  std::function<void()> on_closed;
  {
    thread::MutexLock lock(&mu_);
    for (auto it = members_.begin(); it != members_.end(); ++it) {
      if ((*it)->id == id) {
        dropped = *it;
        members_.erase(it);
        break;
      }
    }
    if (dropped == nullptr)
      return;
    VLOG(1) << "a11 webrtc: lost data channel (" << reason << "); "
            << LiveCountLocked() << " of " << options_.target_channels
            << " remain";
    // The aggregate only fails once no live members remain and replenishment
    // can no longer recover one (server side has no factory, or a client whose
    // replenishment has given up).
    if (!closed_ && LiveCountLocked() == 0 &&
        (!factory_ || replenish_gave_up_)) {
      fatal = true;
      on_closed = callbacks_.on_closed;
    }
    NotifyLocked();
  }
  if (dropped != nullptr) {
    (void)dropped->channel->ResetCallbacks();
    (void)dropped->channel->Close();
  }
  if (fatal && on_closed)
    on_closed();
  else if (!fatal)
    MaybeReplenish();
}

bool MultiplexedBinaryChannel::WaitMemberOpen(std::uint64_t id) {
  const absl::Time deadline = absl::Now() + options_.open_timeout;
  while (true) {
    std::shared_ptr<thread::PermanentEvent> changed;
    {
      thread::MutexLock lock(&mu_);
      if (closed_)
        return false;
      bool present = false;
      for (const std::shared_ptr<Member>& member : members_) {
        if (member->id == id) {
          present = true;
          if (member->open)
            return true;
          break;
        }
      }
      if (!present)
        return false;  // Dropped before opening.
      changed = changed_;
    }
    const int selected =
        thread::SelectUntil(deadline, {thread::OnCancel(), changed->OnEvent()});
    if (selected == 0)
      return false;
    if (selected < 0)
      return false;  // Timed out.
  }
}

void MultiplexedBinaryChannel::MaybeReplenish() {
  std::shared_ptr<MultiplexedBinaryChannel> self;
  {
    thread::MutexLock lock(&mu_);
    if (closed_ || !factory_ || !callbacks_set_ || replenishing_ ||
        replenish_gave_up_ || members_.size() >= options_.target_channels) {
      return;
    }
    replenishing_ = true;
    self = shared_from_this();
  }
  // The fiber lives only while it has channels to replace; it exits once the
  // target is met or it gives up, so nothing waits around for teardown.
  a11::Schedule([self = std::move(self)]() mutable { self->Replenish(); });
}

void MultiplexedBinaryChannel::Replenish() {
  while (true) {
    std::function<void()> give_up_on_closed;
    bool give_up = false;
    {
      thread::MutexLock lock(&mu_);
      if (closed_ || members_.size() >= options_.target_channels) {
        replenishing_ = false;
        return;
      }
      if (replenish_failures_ >= options_.max_replenish_failures) {
        replenish_gave_up_ = true;
        replenishing_ = false;
        give_up = true;
        VLOG(1) << "a11 webrtc: giving up channel replenishment after "
                << replenish_failures_ << " consecutive failures";
        // If nothing is live and we have given up, fail the stream rather than
        // buffer outgoing packets forever. Fire outside the lock.
        if (!closed_ && LiveCountLocked() == 0)
          give_up_on_closed = callbacks_.on_closed;
      }
    }
    if (give_up) {
      if (give_up_on_closed)
        give_up_on_closed();
      return;
    }
    absl::StatusOr<std::shared_ptr<BinaryChannel>> created = factory_();
    if (!created.ok() || *created == nullptr) {
      thread::MutexLock lock(&mu_);
      ++replenish_failures_;
      VLOG(1) << "a11 webrtc: channel replenishment attempt failed: "
              << (created.ok() ? std::string("null channel")
                               : std::string(created.status().message()));
      continue;
    }
    std::shared_ptr<Member> member;
    {
      thread::MutexLock lock(&mu_);
      if (closed_) {
        (void)(*created)->Close();
        replenishing_ = false;
        return;
      }
      if (members_.size() >= options_.target_channels) {
        (void)(*created)->Close();
        replenishing_ = false;
        return;
      }
      member = AppendMemberLocked(*created);
      NotifyLocked();
    }
    WireMember(member);
    if (WaitMemberOpen(member->id)) {
      thread::MutexLock lock(&mu_);
      replenish_failures_ = 0;
      VLOG(1) << "a11 webrtc: replenished data channel; " << LiveCountLocked()
              << " of " << options_.target_channels << " live";
    } else {
      {
        thread::MutexLock lock(&mu_);
        ++replenish_failures_;
      }
      VLOG(1) << "a11 webrtc: replenishment channel failed to open";
      DropMember(member->id, "replenishment channel failed to open");
    }
  }
}

}  // namespace a11::net::internal

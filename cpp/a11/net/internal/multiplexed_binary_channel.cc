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

// Fixed 8-byte little-endian frame suffix carrying the aggregate send order.
//
// A suffix rather than a prefix, for the reason A11's byte-chunking metadata is
// one too: a prefix cannot be added to a buffer the caller already owns without
// moving every byte of it, and a suffix can. On send that is `append` into the
// spare capacity the packet already has; on receive it is a truncation, where the
// prefix form had to copy the payload out from behind its header. At A11's 48 KiB
// WebRTC packet size that was a full copy of every packet in each direction.
//
// Both ends must agree, so this is a wire change: `js/src/webrtc_wire_stream.ts`
// carries the same framing and moved with it. A peer on the older prefix format
// reads the sequence out of the first eight payload bytes and gets nonsense.
constexpr size_t kSequenceSuffix = sizeof(std::uint64_t);

// Takes the packet by value and returns it with the sequence appended, so a
// caller that owns its bytes -- Send does -- pays no copy at all.
std::string EncodeSequence(std::uint64_t sequence, std::string packet) {
  for (size_t index = 0; index < kSequenceSuffix; ++index) {
    packet.push_back(static_cast<char>(sequence & 0xffU));
    sequence >>= 8U;
  }
  return packet;
}

// Reads the sequence from the tail. The caller has already checked the size.
std::uint64_t DecodeSequence(std::string_view framed) {
  std::uint64_t sequence = 0;
  const size_t start = framed.size() - kSequenceSuffix;
  for (size_t index = 0; index < kSequenceSuffix; ++index) {
    sequence |= static_cast<std::uint64_t>(
                    static_cast<unsigned char>(framed[start + index]))
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
  if (options.target_channels == 0) {
    options.target_channels = 1;
  }

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
  std::shared_ptr<thread::PermanentEvent> event =
      std::exchange(changed_, std::make_shared<thread::PermanentEvent>());
  event->Notify();
}

size_t MultiplexedBinaryChannel::LiveCountLocked() const {
  size_t live = 0;
  for (const std::shared_ptr<Member>& member : members_) {
    if (member->open) {
      ++live;
    }
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
            if (auto self = weak.lock(); self != nullptr) {
              self->OnMemberOpen(id);
            }
          },
      .on_message =
          [weak](std::string framed) {
            if (auto self = weak.lock(); self != nullptr) {
              self->OnMemberMessage(std::move(framed));
            }
          },
      .on_error =
          [weak, id](absl::Status status) {
            if (auto self = weak.lock(); self != nullptr) {
              self->DropMember(id, status.message());
            }
          },
      .on_closed =
          [weak, id]() {
            if (auto self = weak.lock(); self != nullptr) {
              self->DropMember(id, "channel closed");
            }
          },
      // Forward drain notifications to the aggregate so the sender's drain
      // barrier wakes, and try to push buffered packets onto the freed channel.
      .on_buffered_amount_low =
          [weak]() {
            std::shared_ptr<MultiplexedBinaryChannel> self = weak.lock();
            if (self == nullptr) {
              return;
            }
            std::function<void()> low;
            {
              thread::MutexLock lock(&self->mu_);
              low = self->callbacks_.on_buffered_amount_low;
            }
            self->FlushPending();
            if (low) {
              low();
            }
          }};
  absl::Status configured = member->channel->SetCallbacks(std::move(callbacks));
  if (!configured.ok()) {
    DropMember(id, configured.message());
    return;
  }
  // The transport may already be open before its callbacks were installed.
  absl::StatusOr<bool> already_open = member->channel->IsOpen();
  if (already_open.ok() && *already_open) {
    OnMemberOpen(id);
  }
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
  for (const std::shared_ptr<Member>& member : members) {
    WireMember(member);
  }
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
  for (const std::shared_ptr<Member>& member : members) {
    (void)member->channel->ResetCallbacks();
  }
  return absl::OkStatus();
}

absl::Status MultiplexedBinaryChannel::Open() {
  {
    thread::MutexLock lock(&mu_);
    if (closed_) {
      return absl::CancelledError("Multiplexed channel is closed");
    }
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
    if (closed_) {
      return;
    }
    bool found = false;
    for (const std::shared_ptr<Member>& member : members_) {
      if (member->id == id) {
        found = member->open == false;
        member->open = true;
        break;
      }
    }
    if (!found) {
      return;
    }
    if (!any_open_) {
      any_open_ = true;
      announce = true;
      on_open = callbacks_.on_open;
    }
    NotifyLocked();
  }
  FlushPending();
  if (announce && on_open) {
    on_open();
  }
}

void MultiplexedBinaryChannel::OnMemberMessage(std::string framed) {
  if (framed.size() < kSequenceSuffix) {
    // A frame without its sequence suffix breaks aggregate ordering; there is
    // no safe way to place it, so fail the whole stream.
    std::function<void(absl::Status)> on_error;
    {
      thread::MutexLock lock(&mu_);
      on_error = callbacks_.on_error;
    }
    if (on_error) {
      on_error(absl::InvalidArgumentError(
          "Multiplexed member packet was malformed"));
    }
    return;
  }
  // Read before the truncation below, which is what removes those bytes.
  const std::uint64_t sequence = DecodeSequence(framed);
  framed.resize(framed.size() - kSequenceSuffix);
  std::function<void(std::string)> on_message;
  std::function<void(absl::Status)> on_error;
  std::vector<std::string> ready;
  {
    thread::MutexLock lock(&mu_);
    if (closed_) {
      return;
    }
    if (sequence >= next_deliver_seq_ &&
        reorder_.find(sequence) == reorder_.end()) {
      if (reorder_.size() >= options_.max_reorder_packets) {
        on_error = callbacks_.on_error;
      } else {
        // The payload is the frame with its tail cut off, so this moves no bytes
        // at all where the prefix form copied the whole payload out.
        reorder_.emplace(sequence, std::move(framed));
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
  if (on_message == nullptr) {
    return;
  }
  // Drain contiguous packets in order; keep draining anything that arrived
  // while we were delivering without the lock.
  while (true) {
    for (std::string& packet : ready) {
      on_message(std::move(packet));
    }
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
    if (closed_) {
      return absl::CancelledError("Multiplexed channel is closed");
    }
    // Moved in, so the sequence is appended to the caller's buffer rather than
    // copied into a new one. Send owns `bytes`; nothing reads it afterwards.
    std::string framed = EncodeSequence(next_send_seq_++, std::move(bytes));
    pending_out_bytes_ += framed.size();
    pending_out_.push_back(std::move(framed));
  }
  FlushPending();
  return absl::OkStatus();
}

void MultiplexedBinaryChannel::AssignPendingLocked() {
  if (members_.empty()) {
    return;
  }
  while (!pending_out_.empty()) {
    std::shared_ptr<Member> chosen;
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
      return;  // No live channel yet; leave packets queued for a later flush.
    }
    const size_t size = pending_out_.front().size();
    chosen->pending.push_back(std::move(pending_out_.front()));
    chosen->pending_bytes += size;
    pending_out_.pop_front();
    pending_out_bytes_ -= size;
  }
}

void MultiplexedBinaryChannel::FlushPending() {
  std::vector<std::shared_ptr<Member>> claimed;
  {
    thread::MutexLock lock(&mu_);
    if (closed_) {
      return;
    }
    AssignPendingLocked();
    for (const std::shared_ptr<Member>& member : members_) {
      if (member->open && !member->flushing && !member->pending.empty()) {
        member->flushing = true;
        claimed.push_back(member);
      }
    }
  }
  if (claimed.empty()) {
    // Either nothing to send or every member already has an owner handing it
    // bytes; that owner rechecks its queue before releasing the claim.
    return;
  }
  // Each member is handed its bytes independently, so one channel's send does not
  // wait behind another's. The caller carries the first -- which is the whole job
  // when only one channel has work, the common case at small message sizes -- and
  // the rest get a fiber so the handoffs overlap.
  for (size_t index = 1; index < claimed.size(); ++index) {
    std::shared_ptr<MultiplexedBinaryChannel> self = shared_from_this();
    a11::Schedule([self = std::move(self), member = claimed[index]]() mutable {
      self->FlushMember(member);
    });
  }
  FlushMember(claimed.front());
}

void MultiplexedBinaryChannel::FlushMember(
    const std::shared_ptr<Member>& member) {
  while (true) {
    std::string packet;
    {
      thread::MutexLock lock(&mu_);
      if (closed_ || !member->open || member->pending.empty()) {
        member->flushing = false;
        return;
      }
      packet = std::move(member->pending.front());
      member->pending.pop_front();
      member->pending_bytes -= packet.size();
    }
    // Sent outside the lock, and by copy: a member that fails hands its packet
    // back for another to carry, and a lost packet is not recoverable -- the
    // peer's reorder buffer would wait on that sequence number for good. Send
    // takes ownership, so keeping a retryable copy is the price of that.
    absl::Status sent = member->channel->Send(packet);
    if (sent.ok()) {
      continue;
    }
    {
      thread::MutexLock lock(&mu_);
      member->open = false;
      member->flushing = false;
      pending_out_bytes_ += packet.size();
      pending_out_.push_front(std::move(packet));
    }
    // Requeues this member's remaining packets too, and re-flushes.
    DropMember(member->id, "send failed");
    return;
  }
}

absl::StatusOr<size_t> MultiplexedBinaryChannel::BufferedAmount() const {
  std::vector<std::shared_ptr<Member>> members;
  size_t total = 0;
  {
    thread::MutexLock lock(&mu_);
    total = pending_out_bytes_;
    members = members_;
    for (const std::shared_ptr<Member>& member : members_) {
      total += member->pending_bytes;
    }
  }
  for (const std::shared_ptr<Member>& member : members) {
    absl::StatusOr<size_t> amount = member->channel->BufferedAmount();
    if (amount.ok()) {
      total += *amount;
    }
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
    if (closed_) {
      return absl::OkStatus();
    }
    closed_ = true;
    members = members_;
    members_.clear();
    for (const std::shared_ptr<Member>& member : members) {
      member->pending.clear();
      member->pending_bytes = 0;
    }
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
    if (member->open) {
      return member->channel->GetImpl();
    }
  }
  return members_.empty() ? nullptr : members_.front()->channel->GetImpl();
}

bool MultiplexedBinaryChannel::AddMember(
    std::shared_ptr<BinaryChannel> channel) {
  if (channel == nullptr) {
    return false;
  }
  std::shared_ptr<Member> member;
  bool wire = false;
  {
    thread::MutexLock lock(&mu_);
    if (closed_ || members_.size() >= options_.target_channels) {
      return false;
    }
    member = AppendMemberLocked(std::move(channel));
    wire = callbacks_set_;
    NotifyLocked();
  }
  if (wire) {
    WireMember(member);
  }
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
    if (dropped == nullptr) {
      return;
    }
    // Whatever was assigned to it and not yet handed over goes back to the front
    // of the unassigned queue, so the next live member carries it. Dropping it
    // would leave a hole the peer's reorder buffer never fills.
    while (!dropped->pending.empty()) {
      std::string packet = std::move(dropped->pending.back());
      dropped->pending.pop_back();
      pending_out_bytes_ += packet.size();
      pending_out_.push_front(std::move(packet));
    }
    dropped->pending_bytes = 0;
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
  if (fatal && on_closed) {
    on_closed();
  } else if (!fatal) {
    MaybeReplenish();
  }
}

bool MultiplexedBinaryChannel::WaitMemberOpen(std::uint64_t id) {
  const absl::Time deadline = absl::Now() + options_.open_timeout;
  while (true) {
    std::shared_ptr<thread::PermanentEvent> changed;
    {
      thread::MutexLock lock(&mu_);
      if (closed_) {
        return false;
      }
      bool present = false;
      for (const std::shared_ptr<Member>& member : members_) {
        if (member->id == id) {
          present = true;
          if (member->open) {
            return true;
          }
          break;
        }
      }
      if (!present) {
        return false;  // Dropped before opening.
      }
      changed = changed_;
    }
    const int selected =
        thread::SelectUntil(deadline, {thread::OnCancel(), changed->OnEvent()});
    if (selected == 0) {
      return false;
    }
    if (selected < 0) {
      return false;  // Timed out.
    }
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
        if (!closed_ && LiveCountLocked() == 0) {
          give_up_on_closed = callbacks_.on_closed;
        }
      }
    }
    if (give_up) {
      if (give_up_on_closed) {
        give_up_on_closed();
      }
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

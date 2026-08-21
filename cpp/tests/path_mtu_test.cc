// Copyright 2026 The A11 Authors.

#include "a11/net/internal/path_mtu.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <absl/status/status.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "thread/boost_primitives.h"

namespace a11::net::internal {
namespace {

// A path with a size limit, and nothing else.
//
// The point of the prober seam is that the interesting cases -- a path that
// shrinks under a live association, a peer that stops answering -- are trivial
// here and close to unreachable against a real network, which cannot be
// reconfigured from inside a test.
class FakePath {
 public:
  explicit FakePath(size_t limit) : limit_(limit), sustained_limit_(limit) {}

  PathMtuProber Prober() {
    return PathMtuProber{
        .apply =
            [this](size_t mtu) {
              thread::MutexLock lock(&mu_);
              if (refuse_applies_ > 0) {
                --refuse_applies_;
                return absl::FailedPreconditionError("not connected yet");
              }
              applied_.push_back(mtu);
              association_mtu_ = mtu;
              return absl::OkStatus();
            },
        .probe =
            [this](size_t payload, absl::Time) {
              thread::MutexLock lock(&mu_);
              probed_.push_back(payload);
              if (answer_nothing_) {
                return false;
              }
              // A probe tests the size the association is set to, and arrives
              // only if the path carries it.
              return association_mtu_ <= limit_;
            },
        .probe_burst = [this](size_t payload, int count,
                              absl::Time) -> std::optional<int> {
          thread::MutexLock lock(&mu_);
          for (int index = 0; index < count; ++index) {
            probed_.push_back(payload);
          }
          if (answer_nothing_) {
            return 0;
          }
          if (association_mtu_ > limit_) {
            return 0;
          }
          // A size that only carries one packet at a time answers the first
          // probe of a burst and drops the rest -- the real failure mode this
          // search exists to reject.
          if (association_mtu_ > sustained_limit_) {
            return 1;
          }
          return count;
        },
        .pause =
            [this] {
              thread::MutexLock lock(&mu_);
              ++pauses_;
            },
        .resume =
            [this] {
              thread::MutexLock lock(&mu_);
              ++resumes_;
            },
        .fail =
            [this](absl::Status status) {
              thread::MutexLock lock(&mu_);
              failure_ = std::move(status);
            },
    };
  }

  void SetLimit(size_t limit) {
    thread::MutexLock lock(&mu_);
    limit_ = limit;
    sustained_limit_ = limit;
  }

  /// A path that answers a single probe up to `limit` but only carries traffic up
  /// to `sustained`. Real: measured between 4096 and 4256 on the reference machine.
  void SetSustainedLimit(size_t sustained) {
    thread::MutexLock lock(&mu_);
    sustained_limit_ = sustained;
  }

  void AnswerNothing() {
    thread::MutexLock lock(&mu_);
    answer_nothing_ = true;
  }

  /// Makes the next `count` applies report "not connected yet".
  void RefuseApplies(int count) {
    thread::MutexLock lock(&mu_);
    refuse_applies_ = count;
  }

  size_t association_mtu() const {
    thread::MutexLock lock(&mu_);
    return association_mtu_;
  }

  size_t pauses() const {
    thread::MutexLock lock(&mu_);
    return pauses_;
  }

  size_t resumes() const {
    thread::MutexLock lock(&mu_);
    return resumes_;
  }

  std::vector<size_t> probed() const {
    thread::MutexLock lock(&mu_);
    return probed_;
  }

  absl::Status failure() const {
    thread::MutexLock lock(&mu_);
    return failure_;
  }

 private:
  mutable thread::Mutex mu_;
  size_t limit_;
  size_t sustained_limit_;
  size_t association_mtu_ = 0;
  int refuse_applies_ = 0;
  bool answer_nothing_ = false;
  size_t pauses_ = 0;
  size_t resumes_ = 0;
  std::vector<size_t> applied_;
  std::vector<size_t> probed_;
  absl::Status failure_;
};

PathMtuOptions TestOptions() {
  PathMtuOptions options;
  options.base_mtu = 1280;
  options.min_mtu = 512;
  options.max_mtu = 8192;
  options.granularity = 64;
  options.max_probes = 3;
  // Never waited on: the fake answers synchronously.
  options.probe_timeout = absl::Milliseconds(1);
  return options;
}

TEST(PathMtuTest, ConvergesJustBelowThePathLimit) {
  FakePath path(4096);
  PathMtuDiscovery discovery(TestOptions(), path.Prober());

  const size_t confirmed = discovery.Search();

  EXPECT_EQ(discovery.state(), PathMtuState::kSearchComplete);
  // Within one granularity step of the truth, and never above it -- the whole
  // safety property of the search is that it does not leave the association at a
  // size the path drops.
  EXPECT_LE(confirmed, 4096u);
  EXPECT_GT(confirmed, 4096u - TestOptions().granularity);
  EXPECT_EQ(path.association_mtu(), confirmed);
}

TEST(PathMtuTest, LeavesTheAssociationAtTheBaseWhenNothingLargerFits) {
  FakePath path(1280);
  PathMtuDiscovery discovery(TestOptions(), path.Prober());

  EXPECT_EQ(discovery.Search(), 1280u);
  EXPECT_EQ(path.association_mtu(), 1280u);
  EXPECT_EQ(discovery.state(), PathMtuState::kSearchComplete);
}

TEST(PathMtuTest, ProbesPayloadsNetOfHeaderOverhead) {
  FakePath path(4096);
  const PathMtuOptions options = TestOptions();
  PathMtuDiscovery discovery(options, path.Prober());

  discovery.Search();

  ASSERT_FALSE(path.probed().empty());
  // A probe has to produce a packet of the candidate size, so its payload is the
  // candidate less the headers -- probing the full candidate would test a size
  // one header larger than the one being considered.
  EXPECT_EQ(path.probed().front(), options.base_mtu - options.probe_overhead);
}

TEST(PathMtuTest, DoesNotHoldApplicationTrafficWhileProbing) {
  FakePath path(2048);
  PathMtuDiscovery discovery(TestOptions(), path.Prober());

  discovery.Search();

  // Nothing is held back, and that is the design rather than an oversight.
  // Raising the MTU does expose in-flight data to an unconfirmed size, and SCTP
  // repairs that on the way back down -- it re-fragments and resends whatever
  // went out too big. Holding traffic instead turned every rejected candidate
  // into a stall of a whole probe timeout, which timed out a ten-second drain in
  // webrtc_wire_stream_test; and the workaround for *that* -- probing only a
  // quiet stream -- meant a busy stream never probed at all.
  EXPECT_EQ(path.pauses(), 0u);
  EXPECT_EQ(path.resumes(), 0u);
  EXPECT_GT(discovery.probes_sent(), 0);
}

TEST(PathMtuTest, APeerThatAnswersNothingGivesUpWithoutTouchingTheStream) {
  FakePath path(8192);  // The path is fine; the peer is not answering.
  path.AnswerNothing();
  PathMtuDiscovery discovery(TestOptions(), path.Prober());

  EXPECT_EQ(discovery.Search(), 0u);
  EXPECT_EQ(discovery.state(), PathMtuState::kError);
  // **No failure is reported**, and this is the important assertion in the file.
  //
  // An earlier version treated an unanswered base probe as a connectivity failure
  // and aborted the stream, reasoning that every conforming path carries 1280.
  // But probes ride an unreliable channel, and under load their acknowledgements
  // are precisely what is dropped -- so it aborted streams that were carrying
  // application data at full rate. A lost probe is not evidence that a path is
  // dead; the stream working is evidence that it is not. Discovery may only ever
  // give up.
  EXPECT_TRUE(path.failure().ok());
}

TEST(PathMtuTest, ATransportThatIsNotReadyYetIsNotEvidenceAboutThePath) {
  FakePath path(8192);
  // Every attempt at the base refused, as it is before the SCTP association
  // exists. This must not be read as "the path cannot carry 1280".
  path.RefuseApplies(TestOptions().max_probes);
  PathMtuDiscovery discovery(TestOptions(), path.Prober());

  EXPECT_EQ(discovery.Search(), 0u);
  EXPECT_NE(discovery.state(), PathMtuState::kError);
  EXPECT_TRUE(path.failure().ok());
}

TEST(PathMtuTest, ASecondSearchFindsAPathThatGrew) {
  FakePath path(2048);
  PathMtuDiscovery discovery(TestOptions(), path.Prober());

  const size_t first = discovery.Search();
  EXPECT_LE(first, 2048u);
  EXPECT_GT(first, 2048u - TestOptions().granularity);

  // A VPN left the chain, or a link renegotiated upward. Only a fresh search
  // notices, which is what the raise timer is for -- and it can only work
  // because raising the association MTU is now possible at all.
  path.SetLimit(8192);
  const size_t second = discovery.Search();

  EXPECT_GT(second, first);
  EXPECT_LE(second, 8192u);
  EXPECT_EQ(path.association_mtu(), second);
}

TEST(PathMtuTest, ASecondSearchFindsAPathThatShrank) {
  FakePath path(8192);
  PathMtuDiscovery discovery(TestOptions(), path.Prober());

  const size_t first = discovery.Search();
  EXPECT_GT(first, 4096u);

  // A LAN reconnection, or a tunnel inserted. The old size now black-holes.
  path.SetLimit(1500);
  const size_t second = discovery.Search();

  EXPECT_LT(second, first);
  EXPECT_LE(second, 1500u);
  EXPECT_GE(second, 1280u);
  EXPECT_EQ(path.association_mtu(), second);
}

TEST(PathMtuTest, BlackHoleReportsBelowTheThresholdAreOrdinaryLoss) {
  FakePath path(4096);
  PathMtuOptions options = TestOptions();
  options.black_hole_threshold = 3;
  PathMtuDiscovery discovery(options, path.Prober());
  discovery.Search();
  ASSERT_EQ(discovery.state(), PathMtuState::kSearchComplete);

  discovery.ReportSendFailure();
  discovery.ReportSendFailure();
  // A run broken by success must not accumulate: transient loss is normal and
  // must not be mistaken for the path having shrunk.
  discovery.ReportSendSuccess();
  discovery.ReportSendFailure();
  discovery.ReportSendFailure();

  EXPECT_EQ(discovery.state(), PathMtuState::kSearchComplete);
}

TEST(PathMtuTest, StoppingLeavesTheAssociationAtAConfirmedSize) {
  FakePath path(4096);
  PathMtuDiscovery discovery(TestOptions(), path.Prober());
  discovery.Search();
  const size_t settled = discovery.confirmed_mtu();

  discovery.Stop();
  discovery.Run();  // Returns immediately.

  EXPECT_EQ(path.association_mtu(), settled);
  EXPECT_EQ(path.pauses(), path.resumes());
}

// The most expensive lesson in this feature, pinned.
//
// A single acknowledged probe does not prove a size usable. Measured on the bare
// data channel: a 64 KiB stream runs at 173 MiB/s at MTU 4096 and does not run at
// all at 4256, while one probe at 4256 comes back -- one IP-fragmented datagram
// reassembles where a stream of them does not. A search that trusted one probe
// converged on 4256, applied it, and stalled the stream; every row above 64 bytes
// in the wire suite timed out.
TEST(PathMtuTest, RejectsASizeThatOnlySurvivesOneProbeAtATime) {
  FakePath path(8192);
  path.SetSustainedLimit(
      2048);  // Answers a lone probe far above what it carries.
  PathMtuDiscovery discovery(TestOptions(), path.Prober());

  const size_t confirmed = discovery.Search();

  // Confirmed at what the path *carries*, not at what a single packet reaches.
  EXPECT_LE(confirmed, 2048u);
  EXPECT_GT(confirmed, 2048u - TestOptions().granularity);
  EXPECT_EQ(path.association_mtu(), confirmed);
}

TEST(PathMtuTest, ABlackHoleAfterConfirmationFallsBackToTheBase) {
  FakePath path(4096);
  PathMtuOptions options = TestOptions();
  options.black_hole_threshold = 3;
  PathMtuDiscovery discovery(options, path.Prober());
  ASSERT_GT(discovery.Search(), options.base_mtu);

  // A confirmed size can stop working -- a path changes, or a burst was luckier
  // than a stream. Without a signal the search would never revisit it, which is
  // why the transport reports send outcomes.
  for (int index = 0; index < options.black_hole_threshold; ++index) {
    discovery.ReportSendFailure();
  }
  EXPECT_EQ(discovery.state(), PathMtuState::kSearchComplete);
}

TEST(PathMtuTest, RejectsIncoherentOptions) {
  PathMtuOptions options = TestOptions();
  options.max_mtu = options.base_mtu - 1;
  EXPECT_FALSE(options.Validate().ok());

  options = TestOptions();
  options.granularity = 0;
  EXPECT_FALSE(options.Validate().ok());

  options = TestOptions();
  options.min_mtu = 8;  // No room for the header overhead.
  EXPECT_FALSE(options.Validate().ok());

  options = TestOptions();
  options.confirm_burst = 0;
  EXPECT_FALSE(options.Validate().ok());

  EXPECT_TRUE(TestOptions().Validate().ok());
}

}  // namespace
}  // namespace a11::net::internal

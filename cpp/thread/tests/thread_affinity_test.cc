// Copyright 2026 The A11 Authors.

// The worker pool's CPU affinity (A11_POOL_PIN).
//
// Its own executable, and not a file in thread_test, for one reason: the pool is
// a process-global singleton that reads its environment when it starts, and it
// starts on first use. A test that needs A11_POOL_PIN set *before* that first use
// cannot share a binary with tests that reach the pool for their own reasons, so
// this binary sets it in main() and does nothing else first.

#include <atomic>
#include <cstdlib>
#include <set>
#include <vector>

#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "thread/concurrency.h"
#include "thread/thread_pool.h"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace thread {
namespace {

// A stand-in process mask, so the parsing cases say the same thing on every
// platform. Deliberately not contiguous and not zero-based: a spec is written
// against the host's numbering and has to survive being handed a mask that is a
// subset of it, which is what a cpuset does.
const std::vector<int>& Allowed() {
  static const std::vector<int> allowed = {0, 1, 2, 3, 6, 7};
  return allowed;
}

std::vector<int> Parse(const char* spec) {
  return internal::ParsePoolAffinitySpec(spec, Allowed());
}

TEST(PoolAffinitySpecTest, OffSpellingsDisablePinning) {
  EXPECT_TRUE(Parse(nullptr).empty());
  EXPECT_TRUE(Parse("").empty());
  EXPECT_TRUE(Parse("0").empty());
  EXPECT_TRUE(Parse("off").empty());
  EXPECT_TRUE(Parse("OFF").empty());
  EXPECT_TRUE(Parse("no").empty());
  EXPECT_TRUE(Parse("false").empty());
  EXPECT_TRUE(Parse(" off ").empty());
}

// A bare 1 is "on", because that is what 1 means to every other dial in the
// pool (A11_POOL_STATS, A11_POOL_ALWAYS_WAKE). "1-1" is how a caller asks for
// CPU 1 alone, and the pair below is the whole point of pinning that
// distinction down in a test.
TEST(PoolAffinitySpecTest, OnSpellingsTakeTheWholeAllowedMask) {
  EXPECT_EQ(Parse("1"), Allowed());
  EXPECT_EQ(Parse("on"), Allowed());
  EXPECT_EQ(Parse("ALL"), Allowed());
  EXPECT_EQ(Parse("true"), Allowed());
  EXPECT_EQ(Parse("1-1"), std::vector<int>({1}));
}

TEST(PoolAffinitySpecTest, ListsRangesAndMixturesKeepTheirGivenOrder) {
  EXPECT_EQ(Parse("2,0"), std::vector<int>({2, 0}));
  EXPECT_EQ(Parse("1-3"), std::vector<int>({1, 2, 3}));
  EXPECT_EQ(Parse("6-7,0-1"), std::vector<int>({6, 7, 0, 1}));
  EXPECT_EQ(Parse(" 3 , 1 "), std::vector<int>({3, 1}));
  EXPECT_EQ(Parse("1,,2"), std::vector<int>({1, 2}));
  // Worker index modulo the list is what maps workers to CPUs, so the order is
  // the placement and not a detail.
  EXPECT_EQ(Parse("7,6"), std::vector<int>({7, 6}));
}

TEST(PoolAffinitySpecTest, DropsDuplicatesAndCpusOutsideTheProcessMask) {
  EXPECT_EQ(Parse("1,1,2,1"), std::vector<int>({1, 2}));
  // 4 and 5 are not in Allowed(): a spec written for the host still does
  // something sensible inside a narrower cpuset rather than failing outright.
  EXPECT_EQ(Parse("0-7"), Allowed());
  EXPECT_EQ(Parse("4,5,3"), std::vector<int>({3}));
}

// Every one of these means "the pool runs unpinned", and the caller warns. The
// alternative -- a best guess at what was meant -- would place the pool
// somewhere nobody asked for and report success.
TEST(PoolAffinitySpecTest, RefusesSpecsItCannotHonour) {
  EXPECT_TRUE(Parse("4,5").empty());  // allowed by nothing in the mask
  EXPECT_TRUE(Parse("two").empty());
  EXPECT_TRUE(Parse("1-").empty());
  EXPECT_TRUE(Parse("3-1").empty());  // descending
  EXPECT_TRUE(Parse("-2").empty());   // negative
  EXPECT_TRUE(Parse("1-2-3").empty());
}

TEST(PoolAffinityTest, ProcessMaskIsReadableExactlyWherePinningExists) {
#if defined(__linux__)
  EXPECT_FALSE(internal::ProcessAllowedCpus().empty());
#else
  // Not a limitation of this code. macOS has no per-core affinity API at all --
  // thread_policy_set(THREAD_AFFINITY_POLICY) is a cache-sharing hint where it
  // is implemented and returns KERN_NOT_SUPPORTED on Apple arm64 -- so the pool
  // reports no allowed set and declines to pin rather than calling an API that
  // would report success and place nothing.
  EXPECT_TRUE(internal::ProcessAllowedCpus().empty());
#endif
}

TEST(PoolAffinityTest, ThreadsOutsideThePoolAreNeverReportedAsPinned) {
  EXPECT_EQ(internal::ThisWorkerAffinityCpu(), -1);
}

// The contract, end to end: with A11_POOL_PIN=1 (set in main below), work that
// runs on a pool worker runs on a thread whose affinity mask holds exactly the
// one CPU the pool says it placed it on.
//
// It does not assert that every worker was reached. Reaching all of them means
// holding each one busy until the last starts, and the routing policy exists
// precisely to keep small items off most of the pool -- a test that forced the
// pool wide would be testing the forcing. Each observation is the whole claim.
TEST(PoolAffinityTest, PoolWorkersRunOnTheOneCpuTheyWerePinnedTo) {
#if !defined(__linux__)
  GTEST_SKIP()
      << "No per-core CPU affinity on this platform; see "
         "PoolAffinityTest.ProcessMaskIsReadableExactlyWherePinningExists.";
#else
  thread::Mutex mu;
  thread::CondVar cv;
  int outstanding = 0;
  std::set<int> cpus_seen;
  int unpinned = 0;
  int mask_disagreed = 0;
  int mask_not_singleton = 0;

  // Enough items, submitted from outside the pool, that some land on a worker
  // that has to be woken and some on one already awake -- the two paths through
  // RunWorker's prologue.
  constexpr int kItems = 64;
  {
    thread::MutexLock lock(&mu);
    outstanding = kItems;
  }
  for (int item = 0; item < kItems; ++item) {
    Post([&] {
      const int reported = internal::ThisWorkerAffinityCpu();
      cpu_set_t set;
      CPU_ZERO(&set);
      const bool readable =
          pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0;
      int width = 0;
      bool holds_reported = false;
      if (readable) {
        width = CPU_COUNT(&set);
        holds_reported = reported >= 0 && CPU_ISSET(reported, &set);
      }

      thread::MutexLock lock(&mu);
      if (reported < 0) {
        ++unpinned;
      } else {
        cpus_seen.insert(reported);
        if (width != 1) {
          ++mask_not_singleton;
        }
        if (!holds_reported) {
          ++mask_disagreed;
        }
      }
      --outstanding;
      cv.SignalAll();
    });
  }

  thread::MutexLock lock(&mu);
  const absl::Time deadline = absl::Now() + absl::Seconds(10);
  while (outstanding > 0) {
    ASSERT_FALSE(cv.WaitWithDeadline(&mu, deadline)) << outstanding << " left";
  }

  EXPECT_EQ(unpinned, 0);
  EXPECT_EQ(mask_not_singleton, 0);
  EXPECT_EQ(mask_disagreed, 0);
  EXPECT_FALSE(cpus_seen.empty());
#endif
}

}  // namespace
}  // namespace thread

int main(int argc, char** argv) {
  // Before anything at all: the pool reads this when it starts, and it starts on
  // first use, which gtest's own setup can reach.
  ::setenv("A11_POOL_PIN", "1", /*overwrite=*/0);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

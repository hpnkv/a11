// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   The measuring apparatus for A11's native benchmarks, deliberately
 *   equivalent to `bench/harness.py`.
 *
 * The point of this binary is comparison, so the method has to match the
 * Python suite's or the comparison is meaningless. Same three conventions:
 * latency and throughput are measured separately (a `steady_clock` pair around
 * a 40ns operation measures the clock), memory is a slope over a growing
 * population rather than one delta, and a result is a record that goes to JSON
 * rather than a line of text.
 *
 * The records emitted here use the same field names as the Python and
 * TypeScript runners, so `python -m bench --baseline` diffs a native run
 * against a Python one directly. Where a benchmark measures the same operation
 * as its Python counterpart, it carries the same suite and name on purpose.
 */

#ifndef A11_BENCH_HARNESS_H_
#define A11_BENCH_HARNESS_H_

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace a11::bench {

/** @brief One measured thing, in units a person can act on. */
struct Result {
  std::string suite;
  std::string name;
  /** Unit lives in the key: `_us`, `_per_s`, `_bytes`, `bytes_each`. */
  std::map<std::string, double> metrics;
  /** What distinguishes two runs of the same benchmark. */
  std::map<std::string, std::string> params = {};
  std::string note = {};
};

/** @brief Collects results and writes them out. */
class Recorder {
 public:
  void Add(Result result);

  /**
   * @brief
   *   Print the fixed-column table the Python runner also prints.
   *
   * @param suite Print only this suite's rows, or every suite when empty.
   */
  void PrintTable(const std::string& suite = "") const;

  /** @brief Write the shared JSON record shape to `path`. */
  [[nodiscard]] bool WriteJson(const std::string& path) const;

  [[nodiscard]] const std::vector<Result>& results() const { return results_; }

 private:
  std::vector<Result> results_;
};

/** @brief p50/p90/p99/max/mean in microseconds, from nanosecond samples. */
std::map<std::string, double> Percentiles(std::vector<double> samples_ns);

/**
 * @brief
 *   Time `iterations` calls as one batch and report a rate, not a latency.
 *
 * For anything fast enough that the clock is part of what you would be
 * measuring. `per_op_items` is for an operation that moves more than one thing
 * (a PutMany of 64 is one call and 64 items); `per_op_bytes` turns the same run
 * into a byte rate.
 */
std::map<std::string, double> Throughput(
    const std::function<void(std::int64_t)>& operation, std::int64_t iterations,
    std::int64_t warmup = 0, std::int64_t per_op_items = 1,
    std::int64_t per_op_bytes = 0);

/**
 * @brief
 *   Time each call separately and report the distribution.
 *
 * For anything above roughly a microsecond: a round trip, a dispatch, a store
 * write.
 */
std::map<std::string, double> Latency(
    const std::function<void(std::int64_t)>& operation, std::int64_t iterations,
    std::int64_t warmup = 0);

/** @brief The process's resident set right now, in bytes. */
std::uint64_t CurrentRssBytes();

/**
 * @brief
 *   Marginal resident bytes per object, fitted rather than differenced.
 *
 * `make(n)` must produce `n` more objects and keep them alive; the caller owns
 * whatever container it appends to. A single before/after reading is dominated
 * by pages the allocator had already reserved and by fixed costs charged to the
 * first object, so this builds the population in stages and fits a line through
 * (count, RSS). `trail` receives the raw points, for the note.
 */
double MemorySlope(const std::function<void(std::int64_t)>& make,
                   const std::vector<std::int64_t>& counts, std::string* trail);

/** @brief Scale an iteration count by the run's `--scale`. */
std::int64_t Scaled(std::int64_t count, double scale, std::int64_t floor = 8);

}  // namespace a11::bench

#endif  // A11_BENCH_HARNESS_H_

/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 * @brief Native benchmark utilities with Python and TypeScript row names.
 *
 * Latency and throughput are measured separately. Memory uses a slope over
 * growing populations. JSON rows share suite and name fields with the other
 * runners for baseline comparisons.
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

/** Times `iterations` calls as one batch and reports a rate.
 * `per_op_items` and `per_op_bytes` scale the rate for batched operations. */
std::map<std::string, double> Throughput(
    const std::function<void(std::int64_t)>& operation, std::int64_t iterations,
    std::int64_t warmup = 0, std::int64_t per_op_items = 1,
    std::int64_t per_op_bytes = 0);

/** Times each call separately and reports its latency distribution. */
std::map<std::string, double> Latency(
    const std::function<void(std::int64_t)>& operation, std::int64_t iterations,
    std::int64_t warmup = 0);

/** @brief The process's resident set right now, in bytes. */
std::uint64_t CurrentRssBytes();

/** Fits marginal resident bytes per object across growing populations.
 * `make(n)` adds and retains `n` objects. `trail` receives the measured
 * (count, RSS) points.
 */
double MemorySlope(const std::function<void(std::int64_t)>& make,
                   const std::vector<std::int64_t>& counts, std::string* trail);

/** @brief Scale an iteration count by the run's `--scale`. */
std::int64_t Scaled(std::int64_t count, double scale, std::int64_t floor = 8);

}  // namespace a11::bench

#endif  // A11_BENCH_HARNESS_H_

// Copyright 2026 The A11 Authors.

#include "bench/harness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_join.h>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace a11::bench {
namespace {

using Clock = std::chrono::steady_clock;

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

std::map<std::string, double> RateMetrics(std::int64_t elapsed_ns,
                                          std::int64_t iterations,
                                          std::int64_t per_op_items,
                                          std::int64_t per_op_bytes) {
  const double seconds =
      std::max(static_cast<double>(elapsed_ns) / 1e9, 1e-9);
  std::map<std::string, double> metrics{
      {"ops_per_s", static_cast<double>(iterations) / seconds},
      {"ns_per_op",
       static_cast<double>(elapsed_ns) / static_cast<double>(iterations)},
      {"elapsed_s", seconds},
  };
  if (per_op_items != 1) {
    metrics["items_per_s"] =
        static_cast<double>(iterations * per_op_items) / seconds;
  }
  if (per_op_bytes != 0) {
    metrics["mib_per_s"] =
        static_cast<double>(iterations * per_op_bytes) / seconds / 1048576.0;
  }
  return metrics;
}

/** The columns the Python runner prints, in the same order. */
constexpr struct {
  const char* label;
  const char* metric;
} kColumns[] = {
    {"ops/s", "ops_per_s"},     {"items/s", "items_per_s"},
    {"p50 us", "p50_us"},       {"p99 us", "p99_us"},
    {"MiB/s", "mib_per_s"},     {"bytes ea", "bytes_each"},
};

std::string RenderCell(double value, const std::string& metric) {
  if (metric == "ops_per_s" || metric == "items_per_s") {
    if (value >= 1e6) return absl::StrFormat("%.2fM", value / 1e6);
    if (value >= 1e3) return absl::StrFormat("%.1fk", value / 1e3);
    return absl::StrFormat("%.0f", value);
  }
  if (metric == "bytes_each") {
    if (value >= 1048576.0) return absl::StrFormat("%.2fM", value / 1048576.0);
    if (value >= 1024.0) return absl::StrFormat("%.1fK", value / 1024.0);
    return absl::StrFormat("%.0f", value);
  }
  if (value >= 1000.0) return absl::StrFormat("%.0f", value);
  if (value >= 10.0) return absl::StrFormat("%.1f", value);
  return absl::StrFormat("%.3f", value);
}

std::string Label(const Result& result) {
  std::string label = result.name;
  for (const auto& [key, value] : result.params) {
    absl::StrAppend(&label, " ", key, "=", value);
  }
  return label;
}

}  // namespace

void Recorder::Add(Result result) { results_.push_back(std::move(result)); }

void Recorder::PrintTable(const std::string& only_suite) const {
  if (results_.empty()) {
    std::printf("(no results)\n");
    return;
  }
  std::vector<std::pair<std::string, std::string>> present;
  for (const auto& column : kColumns) {
    const bool any = std::any_of(
        results_.begin(), results_.end(), [&](const Result& result) {
          return result.metrics.find(column.metric) != result.metrics.end();
        });
    if (any) present.emplace_back(column.label, column.metric);
  }

  size_t width = 30;
  for (const Result& result : results_) {
    width = std::max(width, Label(result).size() + 2);
  }

  std::vector<std::string> suites;
  for (const Result& result : results_) {
    if (!only_suite.empty() && result.suite != only_suite) {
      continue;
    }
    if (std::find(suites.begin(), suites.end(), result.suite) ==
        suites.end()) {
      suites.push_back(result.suite);
    }
  }

  for (const std::string& suite : suites) {
    std::string header = absl::StrFormat("%-*s", width, "benchmark");
    for (const auto& [label, metric] : present) {
      absl::StrAppend(&header, absl::StrFormat("%11s", label));
    }
    std::printf("\n[%s]\n%s\n%s\n", suite.c_str(), header.c_str(),
                std::string(header.size(), '-').c_str());
    for (const Result& result : results_) {
      if (result.suite != suite) continue;
      std::string row = absl::StrFormat("%-*s", width, Label(result));
      for (const auto& [label, metric] : present) {
        const auto found = result.metrics.find(metric);
        absl::StrAppend(
            &row, absl::StrFormat(
                      "%11s", found == result.metrics.end()
                                  ? "-"
                                  : RenderCell(found->second, metric)));
      }
      std::printf("%s\n", row.c_str());
      if (!result.note.empty()) {
        std::printf("%*s  -- %s\n", static_cast<int>(width), "",
                    result.note.c_str());
      }
    }
  }
}

bool Recorder::WriteJson(const std::string& path) const {
  nlohmann::json payload;
  payload["environment"] = {
      {"runtime", "native c++"},
#if defined(NDEBUG)
      {"build", "release"},
#else
      {"build", "debug"},
#endif
  };
  payload["recorded_at"] =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  payload["results"] = nlohmann::json::array();
  for (const Result& result : results_) {
    payload["results"].push_back({
        {"suite", result.suite},
        {"name", result.name},
        {"metrics", result.metrics},
        {"params", result.params},
        {"note", result.note},
    });
  }
  std::ofstream out(path);
  if (!out) return false;
  out << payload.dump(2) << "\n";
  return out.good();
}

std::map<std::string, double> Percentiles(std::vector<double> samples_ns) {
  if (samples_ns.empty()) return {};
  std::sort(samples_ns.begin(), samples_ns.end());
  const auto at = [&](double fraction) {
    const auto index = static_cast<size_t>(std::max(
        0.0, std::ceil(fraction * static_cast<double>(samples_ns.size())) - 1));
    return samples_ns[std::min(index, samples_ns.size() - 1)] / 1000.0;
  };
  const double total =
      std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0);
  std::map<std::string, double> stats{
      {"p50_us", at(0.50)},
      {"p90_us", at(0.90)},
      {"p99_us", at(0.99)},
      {"max_us", samples_ns.back() / 1000.0},
      {"mean_us", total / static_cast<double>(samples_ns.size()) / 1000.0},
      {"ops_per_s",
       total > 0 ? static_cast<double>(samples_ns.size()) / (total / 1e9) : 0.0},
  };
  return stats;
}

std::map<std::string, double> Throughput(
    const std::function<void(std::int64_t)>& operation, std::int64_t iterations,
    std::int64_t warmup, std::int64_t per_op_items, std::int64_t per_op_bytes) {
  for (std::int64_t index = 0; index < warmup; ++index) operation(index);
  const std::int64_t started = NowNs();
  for (std::int64_t index = 0; index < iterations; ++index) {
    operation(warmup + index);
  }
  return RateMetrics(NowNs() - started, iterations, per_op_items, per_op_bytes);
}

std::map<std::string, double> Latency(
    const std::function<void(std::int64_t)>& operation, std::int64_t iterations,
    std::int64_t warmup) {
  for (std::int64_t index = 0; index < warmup; ++index) operation(index);
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(iterations));
  for (std::int64_t index = 0; index < iterations; ++index) {
    const std::int64_t started = NowNs();
    operation(warmup + index);
    samples.push_back(static_cast<double>(NowNs() - started));
  }
  return Percentiles(std::move(samples));
}

std::uint64_t CurrentRssBytes() {
#if defined(__APPLE__)
  // Same source Activity Monitor reads. getrusage only offers the peak, and a
  // peak never comes back down, which is useless for "what does one more cost".
  proc_taskinfo info{};
  const int written =
      proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &info, sizeof(info));
  if (written != sizeof(info)) return 0;
  return info.pti_resident_size;
#elif defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  long pages_total = 0;
  long pages_resident = 0;
  if (!(statm >> pages_total >> pages_resident)) return 0;
  return static_cast<std::uint64_t>(pages_resident) *
         static_cast<std::uint64_t>(sysconf(_SC_PAGE_SIZE));
#else
  return 0;
#endif
}

double MemorySlope(const std::function<void(std::int64_t)>& make,
                   const std::vector<std::int64_t>& counts,
                   std::string* trail) {
  std::vector<std::pair<double, double>> points;
  std::int64_t total = 0;
  for (const std::int64_t count : counts) {
    make(count);
    total += count;
    points.emplace_back(static_cast<double>(total),
                        static_cast<double>(CurrentRssBytes()));
  }
  // Least squares over the later points only: the first stage absorbs whatever
  // the allocator had already reserved.
  const std::vector<std::pair<double, double>> usable =
      points.size() > 2
          ? std::vector<std::pair<double, double>>(points.begin() + 1,
                                                   points.end())
          : points;
  double mean_x = 0;
  double mean_y = 0;
  for (const auto& [x, y] : usable) {
    mean_x += x;
    mean_y += y;
  }
  mean_x /= static_cast<double>(usable.size());
  mean_y /= static_cast<double>(usable.size());
  double covariance = 0;
  double variance = 0;
  for (const auto& [x, y] : usable) {
    covariance += (x - mean_x) * (y - mean_y);
    variance += (x - mean_x) * (x - mean_x);
  }
  if (trail != nullptr) {
    std::vector<std::string> rendered;
    rendered.reserve(points.size());
    for (const auto& [x, y] : points) {
      rendered.push_back(
          absl::StrFormat("%.0f:%.0fK", x, y / 1024.0));
    }
    *trail = absl::StrCat(usable.size(), "-point fit over ",
                          absl::StrJoin(rendered, " "));
  }
  if (variance == 0) return 0.0;
  return std::max(covariance / variance, 0.0);
}

std::int64_t Scaled(std::int64_t count, double scale, std::int64_t floor) {
  return std::max(static_cast<std::int64_t>(
                      static_cast<double>(count) * scale),
                  floor);
}

}  // namespace a11::bench

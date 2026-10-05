#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <numeric>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace lab::perf {

using Clock = std::chrono::steady_clock;

inline double Milliseconds(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

struct Distribution {
  uint64_t samples = 0;
  double mean = 0.0;
  double stddev = 0.0;
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
  double maximum = 0.0;
  double jitterP99P50 = 0.0;
};

inline double NearestRank(const std::vector<double>& sorted, double percentile) {
  if (sorted.empty()) return 0.0;
  const size_t rank = static_cast<size_t>(
      std::ceil(percentile * static_cast<double>(sorted.size())));
  return sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
}

inline Distribution Summarize(std::vector<double> samples) {
  Distribution result{};
  result.samples = samples.size();
  if (samples.empty()) return result;

  const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
  result.mean = sum / static_cast<double>(samples.size());
  double squared = 0.0;
  for (double sample : samples) {
    const double delta = sample - result.mean;
    squared += delta * delta;
  }
  result.stddev = std::sqrt(squared / static_cast<double>(samples.size()));
  std::sort(samples.begin(), samples.end());
  result.p50 = NearestRank(samples, 0.50);
  result.p95 = NearestRank(samples, 0.95);
  result.p99 = NearestRank(samples, 0.99);
  result.p999 = NearestRank(samples, 0.999);
  result.maximum = samples.back();
  result.jitterP99P50 = result.p99 - result.p50;
  return result;
}

struct ResourceUsage {
  double userSec = 0.0;
  double systemSec = 0.0;
  uint64_t currentRssBytes = 0;
  uint64_t peakRssBytes = 0;
};

inline double TimevalSeconds(const timeval& value) {
  return static_cast<double>(value.tv_sec) +
         static_cast<double>(value.tv_usec) / 1'000'000.0;
}

inline uint64_t CurrentRssBytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    return static_cast<uint64_t>(info.resident_size);
  }
  return 0;
#elif defined(__linux__)
  std::ifstream input("/proc/self/statm");
  uint64_t totalPages = 0;
  uint64_t residentPages = 0;
  if (input >> totalPages >> residentPages) {
    (void)totalPages;
    return residentPages * static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
  }
  return 0;
#else
  return 0;
#endif
}

inline ResourceUsage ReadResourceUsage() {
  rusage usage{};
  ResourceUsage result{};
  if (::getrusage(RUSAGE_SELF, &usage) == 0) {
    result.userSec = TimevalSeconds(usage.ru_utime);
    result.systemSec = TimevalSeconds(usage.ru_stime);
#if defined(__APPLE__)
    result.peakRssBytes = static_cast<uint64_t>(usage.ru_maxrss);
#else
    result.peakRssBytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
  }
  result.currentRssBytes = CurrentRssBytes();
  return result;
}

} // namespace lab::perf

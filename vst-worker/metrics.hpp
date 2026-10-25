#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace cableloom {

struct MetricSnapshot {
  uint64_t calls{};
  uint64_t total_ns{};
  uint64_t max_ns{};
  uint64_t deadline_misses{};
  uint64_t p50_ns{};
  uint64_t p95_ns{};
  uint64_t p99_ns{};
};

class RealtimeMetric {
public:
  void record(uint64_t elapsed_ns, uint64_t deadline_ns) noexcept {
    calls_.fetch_add(1, std::memory_order_relaxed);
    total_ns_.fetch_add(elapsed_ns, std::memory_order_relaxed);
    auto observed = max_ns_.load(std::memory_order_relaxed);
    while (observed < elapsed_ns &&
           !max_ns_.compare_exchange_weak(observed, elapsed_ns,
                                          std::memory_order_relaxed)) {
    }
    if (deadline_ns && elapsed_ns > deadline_ns) {
      deadline_misses_.fetch_add(1, std::memory_order_relaxed);
    }
    histogram_[bucket(elapsed_ns)].fetch_add(1, std::memory_order_relaxed);
  }

  MetricSnapshot snapshot() const noexcept {
    MetricSnapshot value;
    value.calls = calls_.load(std::memory_order_relaxed);
    value.total_ns = total_ns_.load(std::memory_order_relaxed);
    value.max_ns = max_ns_.load(std::memory_order_relaxed);
    value.deadline_misses = deadline_misses_.load(std::memory_order_relaxed);
    std::array<uint64_t, 64> bins{};
    for (size_t i = 0; i < bins.size(); ++i) {
      bins[i] = histogram_[i].load(std::memory_order_relaxed);
    }
    value.p50_ns = percentile(bins, value.calls, 50);
    value.p95_ns = percentile(bins, value.calls, 95);
    value.p99_ns = percentile(bins, value.calls, 99);
    return value;
  }

  void reset() noexcept {
    calls_.store(0, std::memory_order_relaxed);
    total_ns_.store(0, std::memory_order_relaxed);
    max_ns_.store(0, std::memory_order_relaxed);
    deadline_misses_.store(0, std::memory_order_relaxed);
    for (auto &value : histogram_)
      value.store(0, std::memory_order_relaxed);
  }

private:
  static size_t bucket(uint64_t value) noexcept {
    if (!value)
      return 0;
    size_t result = 0;
    while (value >>= 1)
      ++result;
    return result < 64 ? result : 63;
  }

  static uint64_t percentile(const std::array<uint64_t, 64> &bins,
                             uint64_t count, uint64_t percent) noexcept {
    if (!count)
      return 0;
    const uint64_t wanted = (count * percent + 99) / 100;
    uint64_t seen = 0;
    for (size_t i = 0; i < bins.size(); ++i) {
      seen += bins[i];
      if (seen >= wanted) {
        return i == 63 ? UINT64_MAX : (uint64_t{1} << (i + 1)) - 1;
      }
    }
    return 0;
  }

  std::atomic<uint64_t> calls_{};
  std::atomic<uint64_t> total_ns_{};
  std::atomic<uint64_t> max_ns_{};
  std::atomic<uint64_t> deadline_misses_{};
  std::array<std::atomic<uint64_t>, 64> histogram_{};
};

} // namespace cableloom

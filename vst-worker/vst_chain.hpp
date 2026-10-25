#pragma once

#include "metrics.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cableloom {

struct PluginSnapshot {
  std::string path;
  std::string class_id;
  std::string name;
  std::string vendor;
  std::string version;
  uint32_t declared_latency_samples{};
  uint32_t restart_flags{};
  bool reconfigure_requested{};
  MetricSnapshot process_time;
};

struct ChainSnapshot {
  uint32_t sample_rate{};
  uint32_t max_block_frames{};
  uint64_t processed_frames{};
  uint64_t failed_blocks{};
  uint64_t declared_latency_samples{};
  MetricSnapshot process_time;
  std::vector<PluginSnapshot> plugins;
};

class VstChain {
public:
  VstChain(std::vector<std::string> plugin_specs, uint32_t sample_rate,
           uint32_t max_block_frames);
  ~VstChain();
  VstChain(VstChain &&) noexcept;
  VstChain &operator=(VstChain &&) noexcept;
  VstChain(const VstChain &) = delete;
  VstChain &operator=(const VstChain &) = delete;

  bool process(const float *input_left, const float *input_right,
               float *output_left, float *output_right,
               uint32_t frames) noexcept;
  ChainSnapshot snapshot() const;
  bool empty() const noexcept;
  void reset_metrics() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

std::string scan_module_json(const std::string &path);
std::string chain_snapshot_json(const ChainSnapshot &snapshot,
                                const char *mode);

} // namespace cableloom

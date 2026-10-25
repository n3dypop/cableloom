#pragma once

#include <cstdint>
#include <string>

namespace cableloom {
class VstChain;

struct LiveOptions {
  std::wstring capture_id;
  std::wstring render_id;
  uint32_t period_frames{96};
  uint32_t ring_blocks{4};
  uint32_t seconds{};
};

std::string list_endpoints_json();
int run_live_route(const LiveOptions &options, VstChain &chain);

} // namespace cableloom

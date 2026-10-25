#include "vst_chain.hpp"
#include "wasapi_route.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string utf8(const std::wstring &value) {
  if (value.empty())
    return {};
  const int size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (!size)
    throw std::runtime_error("Cannot convert command line to UTF-8.");
  std::string result(static_cast<size_t>(size), '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                           static_cast<int>(value.size()), result.data(), size,
                           nullptr, nullptr))
    throw std::runtime_error("Cannot convert command line to UTF-8.");
  return result;
}

uint32_t number(const std::wstring &value, uint32_t minimum, uint32_t maximum,
                const char *name) {
  size_t parsed{};
  unsigned long result{};
  try {
    result = std::stoul(value, &parsed, 10);
  } catch (...) {
    throw std::runtime_error(std::string("Invalid ") + name + '.');
  }
  if (parsed != value.size() || result < minimum || result > maximum)
    throw std::runtime_error(std::string("Invalid ") + name + '.');
  return static_cast<uint32_t>(result);
}

struct CommonOptions {
  uint32_t sample_rate{48000};
  uint32_t block_frames{96};
  std::vector<std::string> plugins;
};

void parse_common(int argc, wchar_t **argv, int begin, CommonOptions &options,
                  cableloom::LiveOptions *live, uint32_t *blocks) {
  for (int i = begin; i < argc; ++i) {
    const std::wstring arg = argv[i];
    auto require_value = [&]() -> std::wstring {
      if (++i >= argc)
        throw std::runtime_error("Missing value for " + utf8(arg) + '.');
      return argv[i];
    };
    if (arg == L"--plugin")
      options.plugins.push_back(utf8(require_value()));
    else if (arg == L"--block")
      options.block_frames = number(require_value(), 16, 4096, "block size");
    else if (arg == L"--sample-rate")
      options.sample_rate =
          number(require_value(), 8000, 384000, "sample rate");
    else if (blocks && arg == L"--blocks")
      *blocks = number(require_value(), 1, 10000000, "block count");
    else if (live && arg == L"--capture")
      live->capture_id = require_value();
    else if (live && arg == L"--render")
      live->render_id = require_value();
    else if (live && arg == L"--period")
      live->period_frames = number(require_value(), 16, 4096, "period");
    else if (live && arg == L"--ring-blocks")
      live->ring_blocks = number(require_value(), 2, 64, "ring block count");
    else if (live && arg == L"--seconds")
      live->seconds = number(require_value(), 1, 86400, "duration");
    else
      throw std::runtime_error("Unknown option: " + utf8(arg));
  }
}

const char *help = R"(Cableloom VST3 worker

Usage:
  cableloom-vst-worker scan <plugin.vst3>
  cableloom-vst-worker list-endpoints
  cableloom-vst-worker benchmark [--plugin <path[#class-id]>]... [--block <frames>] [--blocks <count>]
  cableloom-vst-worker live --capture <endpoint-id> --render <endpoint-id>
      --plugin <path[#class-id]> [--plugin ...] [--period <frames>]
      [--ring-blocks <count>] [--seconds <duration>]

The live route uses exclusive 48 kHz stereo PCM24 between two different cable
devices. Each JSON snapshot reports every plug-in's declared sample latency,
processing-time histogram and deadline misses. The low-latency non-VST route is
the driver's direct Send-to-Receive path and does not launch this worker.
)";

int benchmark(const CommonOptions &options, uint32_t blocks) {
  cableloom::VstChain chain(options.plugins, options.sample_rate,
                            options.block_frames);
  std::vector<float> input_left(options.block_frames),
      input_right(options.block_frames);
  std::vector<float> output_left(options.block_frames),
      output_right(options.block_frames);
  double phase{};
  const double increment = 6.2831853071795864769 * 997.0 / options.sample_rate;
  const uint32_t warmup = std::min<uint32_t>(blocks, 32);
  auto process_block = [&] {
    for (uint32_t i = 0; i < options.block_frames; ++i) {
      input_left[i] = static_cast<float>(std::sin(phase) * 0.25);
      input_right[i] = static_cast<float>(std::cos(phase) * 0.25);
      phase += increment;
      if (phase > 6.2831853071795864769)
        phase -= 6.2831853071795864769;
    }
    if (!chain.process(input_left.data(), input_right.data(),
                       output_left.data(), output_right.data(),
                       options.block_frames))
      throw std::runtime_error("VST benchmark processing failed.");
  };
  for (uint32_t block = 0; block < warmup; ++block)
    process_block();
  chain.reset_metrics();
  for (uint32_t block = 0; block < blocks; ++block)
    process_block();
  std::cout << cableloom::chain_snapshot_json(chain.snapshot(),
                                              chain.empty()
                                                  ? "benchmark_user_mode_bypass"
                                                  : "benchmark_vst_chain")
            << '\n';
  return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    if (argc < 2 || std::wstring(argv[1]) == L"--help" ||
        std::wstring(argv[1]) == L"-h") {
      std::cout << help;
      return argc < 2 ? 1 : 0;
    }
    const std::wstring command = argv[1];
    if (command == L"scan") {
      if (argc != 3)
        throw std::runtime_error("scan requires exactly one VST3 module path.");
      std::cout << cableloom::scan_module_json(utf8(argv[2])) << '\n';
      return 0;
    }
    if (command == L"list-endpoints") {
      if (argc != 2)
        throw std::runtime_error("list-endpoints takes no options.");
      std::cout << cableloom::list_endpoints_json() << '\n';
      return 0;
    }
    if (command == L"benchmark") {
      CommonOptions options;
      uint32_t blocks = 1000;
      parse_common(argc, argv, 2, options, nullptr, &blocks);
      return benchmark(options, blocks);
    }
    if (command == L"live") {
      CommonOptions options;
      cableloom::LiveOptions live;
      parse_common(argc, argv, 2, options, &live, nullptr);
      if (live.capture_id.empty() || live.render_id.empty())
        throw std::runtime_error(
            "live requires --capture and --render endpoint IDs.");
      if (options.sample_rate != 48000)
        throw std::runtime_error("The live route currently requires 48000 Hz.");
      options.block_frames = std::max(options.block_frames, live.period_frames);
      cableloom::VstChain chain(options.plugins, options.sample_rate,
                                options.block_frames);
      return cableloom::run_live_route(live, chain);
    }
    throw std::runtime_error("Unknown command. Use --help.");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

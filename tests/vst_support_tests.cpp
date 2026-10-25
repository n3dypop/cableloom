#include "audio_buffer.hpp"
#include "metrics.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

static void require(bool condition) {
  if (!condition)
    std::abort();
}

int main() {
  cableloom::RealtimeMetric metric;
  for (uint64_t value : {100u, 200u, 300u, 400u, 500u})
    metric.record(value, 350);
  const auto snapshot = metric.snapshot();
  require(snapshot.calls == 5);
  require(snapshot.total_ns == 1500);
  require(snapshot.max_ns == 500);
  require(snapshot.deadline_misses == 2);
  require(snapshot.p50_ns >= 300 && snapshot.p50_ns <= 511);
  require(snapshot.p99_ns >= 500 && snapshot.p99_ns <= 511);

  for (int32_t value : {-8388608, -1, 0, 1, 8388607}) {
    std::array<uint8_t, 3> packed{static_cast<uint8_t>(value),
                                  static_cast<uint8_t>(value >> 8),
                                  static_cast<uint8_t>(value >> 16)};
    std::array<uint8_t, 3> restored{};
    const float sample = cableloom::pcm24_to_float(packed.data());
    require(!cableloom::float_to_pcm24(sample, restored.data()));
    require(packed == restored);
  }
  std::array<uint8_t, 3> exceptional{};
  require(cableloom::float_to_pcm24(std::numeric_limits<float>::quiet_NaN(),
                                    exceptional.data()));
  require(exceptional == std::array<uint8_t, 3>{0, 0, 0});
  require(cableloom::float_to_pcm24(std::numeric_limits<float>::infinity(),
                                    exceptional.data()));
  require(exceptional == std::array<uint8_t, 3>{0xff, 0xff, 0x7f});
  require(cableloom::float_to_pcm24(-std::numeric_limits<float>::infinity(),
                                    exceptional.data()));
  require(exceptional == std::array<uint8_t, 3>{0, 0, 0x80});

  cableloom::StereoRing ring(2);
  uint64_t dropped = 0;
  ring.push(1.0f, 2.0f, dropped);
  ring.push(3.0f, 4.0f, dropped);
  ring.push(5.0f, 6.0f, dropped);
  require(dropped == 1 && ring.size() == 2);
  float left = 0, right = 0;
  require(ring.pop(left, right) && left == 3.0f && right == 4.0f);
  require(ring.pop(left, right) && left == 5.0f && right == 6.0f);
  require(!ring.pop(left, right));
}

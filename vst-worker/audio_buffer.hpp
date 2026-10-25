#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cableloom {

inline float pcm24_to_float(const uint8_t *bytes) noexcept {
  int32_t value = static_cast<int32_t>(bytes[0]) |
                  (static_cast<int32_t>(bytes[1]) << 8) |
                  (static_cast<int32_t>(bytes[2]) << 16);
  if (value & 0x00800000)
    value |= static_cast<int32_t>(0xff000000);
  return static_cast<float>(value) / 8388608.0f;
}

inline bool float_to_pcm24(float sample, uint8_t *bytes) noexcept {
  const bool clipped =
      !std::isfinite(sample) || sample < -1.0f || sample >= 1.0f;
  if (std::isnan(sample))
    sample = 0.0f;
  constexpr float maximum = 8388607.0f / 8388608.0f;
  sample = std::clamp(sample, -1.0f, maximum);
  const auto value = static_cast<int32_t>(std::lrint(sample * 8388608.0f));
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8);
  bytes[2] = static_cast<uint8_t>(value >> 16);
  return clipped;
}

class StereoRing {
public:
  explicit StereoRing(size_t frames) : data_(std::max<size_t>(frames, 2) * 2) {}

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return data_.size() / 2; }

  void push(float left, float right, uint64_t &dropped) noexcept {
    if (size_ == capacity()) {
      read_ = (read_ + 1) % capacity();
      --size_;
      ++dropped;
    }
    data_[write_ * 2] = left;
    data_[write_ * 2 + 1] = right;
    write_ = (write_ + 1) % capacity();
    ++size_;
  }

  bool pop(float &left, float &right) noexcept {
    if (!size_)
      return false;
    left = data_[read_ * 2];
    right = data_[read_ * 2 + 1];
    read_ = (read_ + 1) % capacity();
    --size_;
    return true;
  }

private:
  std::vector<float> data_;
  size_t read_{};
  size_t write_{};
  size_t size_{};
};

} // namespace cableloom

#pragma once
#include <stdint.h>
namespace cableloom {
struct Frame { int32_t left, right; };
static_assert(sizeof(Frame) == 8, "stereo internal samples");
inline Frame decode(const uint8_t* bytes, uint32_t bits) {
    Frame frame{}; int32_t* samples[] = {&frame.left, &frame.right};
    uint32_t width = bits / 8;
    for (uint32_t channel = 0; channel < 2; ++channel) {
        uint32_t raw = 0;
        for (uint32_t b = 0; b < width; ++b) raw |= static_cast<uint32_t>(*bytes++) << (b * 8);
        if (bits < 32 && (raw & (1u << (bits - 1)))) raw |= ~((1u << bits) - 1);
        *samples[channel] = static_cast<int32_t>(raw);
    }
    return frame;
}
inline void encode(uint8_t* bytes, uint32_t bits, Frame frame) {
    uint32_t samples[] = {static_cast<uint32_t>(frame.left), static_cast<uint32_t>(frame.right)};
    for (auto sample : samples) for (uint32_t b = 0; b < bits / 8; ++b) *bytes++ = static_cast<uint8_t>(sample >> (b * 8));
}
class DelayLine {
public:
    static constexpr uint32_t capacity = 8192;
    void reset(uint32_t delay) {
        read_ = write_ = count_ = 0;
        for (uint32_t i = 0; i < delay && i < capacity; ++i) push(Frame{});
    }
    bool push(Frame value) {
        bool dropped = count_ == capacity;
        if (dropped) { read_ = (read_ + 1) % capacity; --count_; }
        samples_[write_] = value;
        write_ = (write_ + 1) % capacity;
        ++count_;
        return dropped;
    }
    bool pull(Frame& value) {
        if (!count_) { value = {}; return false; }
        value = samples_[read_];
        read_ = (read_ + 1) % capacity;
        --count_;
        return true;
    }
    uint32_t size() const { return count_; }
private:
    Frame samples_[capacity]{};
    uint32_t read_ = 0, write_ = 0, count_ = 0;
};
// Called under the adapter spin lock. Counts QPC time, not timer callbacks.
class FrameClock {
public:
    void reset(uint64_t now, uint64_t frequency) { last_ = now; frequency_ = frequency; carry_ = 0; }
    uint64_t advance(uint64_t now) {
        if (!frequency_ || now < last_) { last_ = now; carry_ = 0; return 0; }
        uint64_t delta = now - last_;
        last_ = now;
        uint64_t partial = (delta % frequency_) * 48000 + carry_;
        uint64_t frames = (delta / frequency_) * 48000 + partial / frequency_;
        carry_ = partial % frequency_;
        return frames;
    }
private:
    uint64_t last_ = 0, frequency_ = 0, carry_ = 0;
};
}

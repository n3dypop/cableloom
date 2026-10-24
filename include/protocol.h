#pragma once
#include <stdint.h>
#define CL_PROTOCOL_VERSION 1u
#define CL_SAMPLE_RATE 48000u
#define CL_CHANNELS 2u
#define CL_FRAME_BYTES 8u
#define CL_MAX_CABLES 8u
#define CL_MIN_DELAY 48u
#define CL_MAX_DELAY 480u
// GUID_DEVINTERFACE_CABLELOOM = {1678cb4e-9a71-4c0f-bae5-12ef00cb7128}
#define CL_IOCTL_STATUS 0x00226000u // FILE_DEVICE_UNKNOWN, 0x800, READ, BUFFERED
#define CL_IOCTL_DELAY  0x0022a004u // FILE_DEVICE_UNKNOWN, 0x801, WRITE, BUFFERED
struct ClRequest { uint32_t version; uint32_t size; };
struct ClDelay { uint32_t version; uint32_t size; uint32_t frames; uint32_t reserved; };
struct ClStatus {
    uint32_t version, size, cable_id, delay_frames;
    uint32_t sample_rate, channels, bits, running; // bit 0 render, bit 1 capture
    uint64_t render_frames, capture_frames, underrun_frames, overrun_frames;
    uint64_t discontinuities, timer_ticks;
};
static_assert(sizeof(ClRequest) == 8, "wire ABI");
static_assert(sizeof(ClDelay) == 16, "wire ABI");
static_assert(sizeof(ClStatus) == 80, "wire ABI");

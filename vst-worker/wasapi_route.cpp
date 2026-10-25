#include "wasapi_route.hpp"

#include "audio_buffer.hpp"
#include "vst_chain.hpp"

// clang-format off: Windows property-key headers have ordering dependencies.
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <cfgmgr32.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cableloom {
namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT value, const char *operation) {
  if (FAILED(value)) {
    char message[192]{};
    sprintf_s(message, "%s failed with HRESULT 0x%08lx", operation,
              static_cast<unsigned long>(value));
    throw std::runtime_error(message);
  }
}

std::string utf8(const wchar_t *value) {
  if (!value)
    return {};
  const int characters = static_cast<int>(wcslen(value));
  if (!characters)
    return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, characters,
                          nullptr, 0, nullptr, nullptr);
  if (!size)
    throw std::runtime_error("Cannot convert endpoint text to UTF-8.");
  std::string result(static_cast<size_t>(size), '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, characters,
                           result.data(), size, nullptr, nullptr))
    throw std::runtime_error("Cannot convert endpoint text to UTF-8.");
  return result;
}

std::string json(const std::string &text) {
  std::string result = "\"";
  for (const unsigned char c : text) {
    if (c == '\\')
      result += "\\\\";
    else if (c == '"')
      result += "\\\"";
    else if (c == '\n')
      result += "\\n";
    else if (c == '\r')
      result += "\\r";
    else if (c == '\t')
      result += "\\t";
    else if (c >= 0x20)
      result += static_cast<char>(c);
  }
  result += '"';
  return result;
}

struct Apartment {
  Apartment() {
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
  }
  ~Apartment() { CoUninitialize(); }
};

struct Handle {
  HANDLE value{};
  ~Handle() {
    if (value)
      CloseHandle(value);
  }
  Handle() = default;
  Handle(Handle &&other) noexcept : value(other.value) {
    other.value = nullptr;
  }
  Handle &operator=(Handle &&other) noexcept {
    if (this != &other) {
      if (value)
        CloseHandle(value);
      value = other.value;
      other.value = nullptr;
    }
    return *this;
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};

std::wstring property(IMMDevice *device, const PROPERTYKEY &key) {
  ComPtr<IPropertyStore> store;
  check(device->OpenPropertyStore(STGM_READ, &store), "OpenPropertyStore");
  PROPVARIANT value;
  PropVariantInit(&value);
  const HRESULT result = store->GetValue(key, &value);
  std::wstring text;
  if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal)
    text = value.pwszVal;
  PropVariantClear(&value);
  return text;
}

struct Endpoint {
  ComPtr<IMMDevice> device;
  ComPtr<IAudioClient> audio;
  Handle event;
  UINT32 frames{};
  std::wstring instance_id;
  std::wstring cable_instance_id;
  bool started{};

  ~Endpoint() {
    if (started && audio)
      audio->Stop();
  }
  Endpoint() = default;
  Endpoint(Endpoint &&) noexcept = default;
  Endpoint &operator=(Endpoint &&) noexcept = default;
  Endpoint(const Endpoint &) = delete;
  Endpoint &operator=(const Endpoint &) = delete;
};

std::wstring cable_instance(const std::wstring &endpoint_id) {
  std::wstring instance = L"SWD\\MMDEVAPI\\" + endpoint_id;
  DEVINST node{};
  if (CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_NORMAL) !=
      CR_SUCCESS)
    return {};
  for (unsigned depth = 0; depth < 16; ++depth) {
    ULONG size{};
    if (CM_Get_Device_ID_Size(&size, node, 0) != CR_SUCCESS)
      return {};
    std::vector<wchar_t> id(static_cast<size_t>(size) + 1);
    if (CM_Get_Device_IDW(node, id.data(), static_cast<ULONG>(id.size()), 0) !=
        CR_SUCCESS)
      return {};
    std::wstring current(id.data());
    std::wstring upper = current;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](wchar_t value) {
                     return static_cast<wchar_t>(std::towupper(value));
                   });
    if (upper.rfind(L"ROOT\\CABLELOOM\\", 0) == 0)
      return current;
    DEVINST parent{};
    if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS)
      break;
    node = parent;
  }
  return {};
}

Endpoint open_endpoint(IMMDeviceEnumerator *enumerator, const std::wstring &id,
                       EDataFlow expected, uint32_t period_frames) {
  Endpoint result;
  check(enumerator->GetDevice(id.c_str(), &result.device), "GetDevice");
  ComPtr<IMMEndpoint> endpoint;
  check(result.device.As(&endpoint), "IMMEndpoint");
  EDataFlow flow{};
  check(endpoint->GetDataFlow(&flow), "GetDataFlow");
  if (flow != expected)
    throw std::runtime_error("Endpoint direction does not match the route.");
  result.instance_id = property(result.device.Get(), PKEY_Device_InstanceId);
  result.cable_instance_id = cable_instance(id);
  check(result.device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void **>(result.audio.GetAddressOf())),
        "Activate IAudioClient");

  WAVEFORMATEXTENSIBLE format{};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = 2;
  format.Format.nSamplesPerSec = 48000;
  format.Format.nAvgBytesPerSec = 48000 * 6;
  format.Format.nBlockAlign = 6;
  format.Format.wBitsPerSample = 24;
  format.Format.cbSize = 22;
  format.Samples.wValidBitsPerSample = 24;
  format.dwChannelMask = 3;
  format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  check(result.audio->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        &format.Format, nullptr),
        "PCM24 exclusive format support");
  const REFERENCE_TIME period =
      (static_cast<REFERENCE_TIME>(period_frames) * 10000000 + 47999) / 48000;
  const HRESULT initialized = result.audio->Initialize(
      AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
      period, &format.Format, nullptr);
  if (initialized == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
    UINT32 aligned{};
    result.audio->GetBufferSize(&aligned);
    throw std::runtime_error(
        "Endpoint rejected the requested period; aligned frame count is " +
        std::to_string(aligned) + ".");
  }
  check(initialized, "Initialize exclusive stream");
  check(result.audio->GetBufferSize(&result.frames), "GetBufferSize");
  result.event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!result.event.value)
    throw std::runtime_error("CreateEvent failed.");
  check(result.audio->SetEventHandle(result.event.value), "SetEventHandle");
  return result;
}

struct Counters {
  std::atomic<uint64_t> captured_frames{};
  std::atomic<uint64_t> rendered_frames{};
  std::atomic<uint64_t> ring_dropped_frames{};
  std::atomic<uint64_t> render_underflow_frames{};
  std::atomic<uint64_t> clipped_samples{};
  std::atomic<uint64_t> discontinuity_packets{};
  std::atomic<uint64_t> timestamp_error_packets{};
};

std::string live_json(const LiveOptions &options, const VstChain &chain,
                      const Counters &counters, size_t ring_frames,
                      bool mmcss) {
  std::ostringstream out;
  out << "{\"schema\":1,\"mode\":\""
      << (chain.empty() ? "live_user_mode_bypass" : "live_vst_route")
      << "\",\"period_frames\":" << options.period_frames
      << ",\"ring_capacity_frames\":" << ring_frames
      << ",\"mmcss\":" << (mmcss ? "true" : "false")
      << ",\"captured_frames\":" << counters.captured_frames.load()
      << ",\"rendered_frames\":" << counters.rendered_frames.load()
      << ",\"ring_dropped_frames\":" << counters.ring_dropped_frames.load()
      << ",\"render_underflow_frames\":"
      << counters.render_underflow_frames.load()
      << ",\"clipped_samples\":" << counters.clipped_samples.load()
      << ",\"discontinuity_packets\":" << counters.discontinuity_packets.load()
      << ",\"timestamp_error_packets\":"
      << counters.timestamp_error_packets.load()
      << ",\"vst\":" << chain_snapshot_json(chain.snapshot(), "live_vst_chain")
      << '}';
  return out.str();
}

std::atomic<std::atomic<bool> *> stop_pointer{};
BOOL WINAPI stop_handler(DWORD type) {
  auto *stop = stop_pointer.load(std::memory_order_acquire);
  if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
       type == CTRL_CLOSE_EVENT) &&
      stop) {
    stop->store(true, std::memory_order_relaxed);
    return TRUE;
  }
  return FALSE;
}

class StopRegistration {
public:
  explicit StopRegistration(std::atomic<bool> &stop) {
    stop_pointer.store(&stop, std::memory_order_release);
    if (!SetConsoleCtrlHandler(stop_handler, TRUE)) {
      stop_pointer.store(nullptr, std::memory_order_release);
      throw std::runtime_error("SetConsoleCtrlHandler failed.");
    }
  }
  ~StopRegistration() {
    stop_pointer.store(nullptr, std::memory_order_release);
    SetConsoleCtrlHandler(stop_handler, FALSE);
  }
  StopRegistration(const StopRegistration &) = delete;
  StopRegistration &operator=(const StopRegistration &) = delete;
};

} // namespace

std::string list_endpoints_json() {
  Apartment apartment;
  ComPtr<IMMDeviceEnumerator> enumerator;
  check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&enumerator)),
        "Create MMDeviceEnumerator");
  std::ostringstream out;
  out << "{\"schema\":1,\"endpoints\":[";
  bool first = true;
  for (const EDataFlow flow : {eRender, eCapture}) {
    ComPtr<IMMDeviceCollection> devices;
    check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices),
          "EnumAudioEndpoints");
    UINT count{};
    check(devices->GetCount(&count), "GetCount");
    for (UINT i = 0; i < count; ++i) {
      ComPtr<IMMDevice> device;
      check(devices->Item(i, &device), "Endpoint item");
      LPWSTR raw_id{};
      check(device->GetId(&raw_id), "Endpoint ID");
      const std::wstring wide_id = raw_id;
      const auto id = utf8(wide_id.c_str());
      CoTaskMemFree(raw_id);
      if (!first)
        out << ',';
      first = false;
      out << "{\"flow\":\"" << (flow == eRender ? "render" : "capture")
          << "\",\"name\":"
          << json(
                 utf8(property(device.Get(), PKEY_Device_FriendlyName).c_str()))
          << ",\"id\":" << json(id) << ",\"instance_id\":"
          << json(utf8(property(device.Get(), PKEY_Device_InstanceId).c_str()))
          << ",\"cable_instance_id\":"
          << json(utf8(cable_instance(wide_id).c_str())) << '}';
    }
  }
  out << "]}";
  return out.str();
}

int run_live_route(const LiveOptions &options, VstChain &chain) {
  Apartment apartment;
  ComPtr<IMMDeviceEnumerator> enumerator;
  check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&enumerator)),
        "Create MMDeviceEnumerator");
  auto capture = open_endpoint(enumerator.Get(), options.capture_id, eCapture,
                               options.period_frames);
  auto render = open_endpoint(enumerator.Get(), options.render_id, eRender,
                              options.period_frames);
  if (capture.cable_instance_id.empty() || render.cable_instance_id.empty())
    throw std::runtime_error(
        "Both live-route endpoints must belong to Cableloom devices.");
  if (capture.cable_instance_id == render.cable_instance_id)
    throw std::runtime_error("Capture and render endpoints belong to the same "
                             "cable; a worker route requires two cables.");
  if (capture.frames > chain.snapshot().max_block_frames)
    throw std::runtime_error(
        "Capture buffer exceeds the configured VST maximum block size.");

  ComPtr<IAudioCaptureClient> input;
  ComPtr<IAudioRenderClient> output;
  check(capture.audio->GetService(IID_PPV_ARGS(&input)), "Capture service");
  check(render.audio->GetService(IID_PPV_ARGS(&output)), "Render service");
  const size_t ring_frames =
      static_cast<size_t>(std::max(capture.frames, render.frames)) *
      std::max<uint32_t>(options.ring_blocks, 2);
  StereoRing ring(ring_frames);
  std::vector<float> input_left(chain.snapshot().max_block_frames);
  std::vector<float> input_right(chain.snapshot().max_block_frames);
  std::vector<float> output_left(chain.snapshot().max_block_frames);
  std::vector<float> output_right(chain.snapshot().max_block_frames);
  Counters counters;
  std::atomic<bool> stop{};
  StopRegistration stop_registration(stop);

  DWORD task_index{};
  HANDLE mmcss_handle =
      AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  const bool mmcss = mmcss_handle != nullptr;
  struct Revert {
    HANDLE value;
    ~Revert() {
      if (value)
        AvRevertMmThreadCharacteristics(value);
    }
  } revert{mmcss_handle};

  auto fill_render = [&](bool count_underflow) {
    BYTE *bytes{};
    check(output->GetBuffer(render.frames, &bytes), "Get render buffer");
    uint64_t underflow{};
    uint64_t clipped{};
    for (UINT32 i = 0; i < render.frames; ++i) {
      float left{}, right{};
      if (!ring.pop(left, right))
        ++underflow;
      if (float_to_pcm24(left, bytes + static_cast<size_t>(i) * 6))
        ++clipped;
      if (float_to_pcm24(right, bytes + static_cast<size_t>(i) * 6 + 3))
        ++clipped;
    }
    if (count_underflow)
      counters.render_underflow_frames.fetch_add(underflow);
    counters.clipped_samples.fetch_add(clipped);
    counters.rendered_frames.fetch_add(render.frames);
    check(output->ReleaseBuffer(render.frames, 0), "Release render buffer");
  };

  fill_render(false);
  check(capture.audio->Start(), "Start capture");
  capture.started = true;
  check(render.audio->Start(), "Start render");
  render.started = true;

  std::thread telemetry([&] {
    while (!stop.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!stop.load())
        std::cout << live_json(options, chain, counters, ring_frames, mmcss)
                  << std::endl;
    }
  });

  const auto deadline = options.seconds
                            ? std::chrono::steady_clock::now() +
                                  std::chrono::seconds(options.seconds)
                            : std::chrono::steady_clock::time_point::max();
  try {
    HANDLE events[] = {capture.event.value, render.event.value};
    while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
      const DWORD waited = WaitForMultipleObjects(2, events, FALSE, 2000);
      if (waited == WAIT_FAILED)
        throw std::runtime_error("WaitForMultipleObjects failed.");
      if (waited == WAIT_TIMEOUT)
        throw std::runtime_error("Audio event timeout.");

      UINT32 packet{};
      check(input->GetNextPacketSize(&packet), "GetNextPacketSize");
      while (packet) {
        BYTE *bytes{};
        UINT32 frames{};
        DWORD flags{};
        check(input->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr),
              "Get capture buffer");
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
          counters.discontinuity_packets.fetch_add(1);
        if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
          counters.timestamp_error_packets.fetch_add(1);
        UINT32 offset{};
        while (offset < frames) {
          const UINT32 block = std::min<UINT32>(
              frames - offset, static_cast<UINT32>(input_left.size()));
          for (UINT32 i = 0; i < block; ++i) {
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
              input_left[i] = input_right[i] = 0.0f;
            } else {
              const auto *frame = bytes + static_cast<size_t>(offset + i) * 6;
              input_left[i] = pcm24_to_float(frame);
              input_right[i] = pcm24_to_float(frame + 3);
            }
          }
          if (!chain.process(input_left.data(), input_right.data(),
                             output_left.data(), output_right.data(), block))
            throw std::runtime_error(
                "VST processing failed or requested I/O reconfiguration.");
          uint64_t dropped{};
          for (UINT32 i = 0; i < block; ++i)
            ring.push(output_left[i], output_right[i], dropped);
          counters.ring_dropped_frames.fetch_add(dropped);
          offset += block;
        }
        counters.captured_frames.fetch_add(frames);
        check(input->ReleaseBuffer(frames), "Release capture buffer");
        check(input->GetNextPacketSize(&packet), "GetNextPacketSize");
      }
      if (waited == WAIT_OBJECT_0 + 1)
        fill_render(true);
    }
  } catch (...) {
    stop.store(true);
    if (telemetry.joinable())
      telemetry.join();
    std::cout << live_json(options, chain, counters, ring_frames, mmcss)
              << std::endl;
    throw;
  }
  stop.store(true);
  if (telemetry.joinable())
    telemetry.join();
  std::cout << live_json(options, chain, counters, ring_frames, mmcss)
            << std::endl;
  return counters.discontinuity_packets.load() ||
                 counters.timestamp_error_packets.load()
             ? 2
             : 0;
}

} // namespace cableloom
